// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/gcc_flags.h"

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "base/filesystem.h"
#include "base/options.h"
#include "base/path.h"
#include "glog/logging.h"
#include "glog/stl_logging.h"
#include "lib/clang_flags_helper.h"
#include "lib/cmdline_parser.h"
#include "lib/compiler_flags.h"
#include "lib/file_helper.h"
#include "lib/flag_parser.h"
#include "lib/known_warning_options.h"
#include "lib/path_resolver.h"
#include "lib/path_util.h"

namespace devtools_goma {

/* static */
std::string GCCFlags::GetCompilerName(absl::string_view arg) {
  absl::string_view name = GetBasename(arg);
  if (name.find("clang++") != std::string::npos) {
    return "clang++";
  }
  if (name.find("clang") != std::string::npos) {
    return "clang";
  }
  if (name.find("g++") != std::string::npos || name == "c++") {
    return "g++";
  }
  return "gcc";
}

// Return the key 'gcc' or 'g++' with architecture and version
// stripped from compiler_name.
std::string GCCFlags::compiler_name() const {
  return GetCompilerName(compiler_name_);
}

GCCFlags::GCCFlags(const std::vector<std::string>& args, const std::string& cwd)
    : CxxFlags(args, cwd),
      is_cplusplus_(false),
      has_nostdinc_(false),
      has_no_integrated_as_(false),
      has_pipe_(false),
      has_ffreestanding_(false),
      has_fno_hosted_(false),
      has_fsyntax_only_(false),
      has_ftime_trace_(false),
      has_wrapper_(false),
      has_fplugin_(false),
      is_precompiling_header_(false),
      is_stdin_input_(false),
      has_fmodules_(false),
      has_fimplicit_module_maps_(false),
      has_emit_module_(false) {
  if (!CompilerFlags::ExpandPosixArgs(cwd, args, &expanded_args_,
                                      &optional_input_filenames_)) {
    Fail("Unable to expand args");
    return;
  }
  bool has_at_file = !optional_input_filenames_.empty();
  bool no_integrated_as = false;
  bool fno_integrated_as = false;
  bool ffreestanding = false;
  bool fno_hosted = false;
  bool fsyntax_only = false;
  bool print_file_name = false;

  bool fmodules = false;
  bool fno_implicit_module_maps = false;
  std::vector<std::string> xclang_flags;

  FlagParser parser;
  DefineFlags(&parser);

  FlagParser::Flag* flag_c = parser.AddBoolFlag("c");
  FlagParser::Flag* flag_S = parser.AddBoolFlag("S");
  FlagParser::Flag* flag_E = parser.AddBoolFlag("E");
  FlagParser::Flag* flag_M = parser.AddBoolFlag("M");
  FlagParser::Flag* flag_MM = parser.AddBoolFlag("MM");
  FlagParser::Flag* flag_MD = parser.AddBoolFlag("MD");
  FlagParser::Flag* flag_MMD = parser.AddBoolFlag("MMD");
  FlagParser::Flag* flag_g = parser.AddPrefixFlag("g");
  parser.AddBoolFlag("nostdinc")->SetSeenOutput(&has_nostdinc_);
  parser.AddBoolFlag("nostdinc++")->SetOutput(&compiler_info_flags_);
  parser.AddBoolFlag("nostdlibinc")->SetOutput(&compiler_info_flags_);
  parser.AddBoolFlag("integrated-as")->SetOutput(&compiler_info_flags_);
  parser.AddBoolFlag("no-integrated-as")->SetSeenOutput(&no_integrated_as);
  parser.AddBoolFlag("fno-integrated-as")->SetSeenOutput(&fno_integrated_as);
  parser.AddBoolFlag("pipe")->SetSeenOutput(&has_pipe_);
  parser.AddBoolFlag("-pipe")->SetSeenOutput(&has_pipe_);
  parser.AddBoolFlag("fcoverage-mapping")
      ->SetSeenOutput(&has_fcoverage_mapping_);
  parser.AddBoolFlag("ffreestanding")->SetSeenOutput(&ffreestanding);
  parser.AddBoolFlag("fno-hosted")->SetSeenOutput(&fno_hosted);
  parser.AddBoolFlag("fsyntax-only")->SetSeenOutput(&fsyntax_only);
  parser.AddBoolFlag("ftime-trace")->SetSeenOutput(&has_ftime_trace_);
  parser.AddBoolFlag("print-file-name")->SetSeenOutput(&print_file_name);
  parser.AddBoolFlag("-print-file-name")->SetSeenOutput(&print_file_name);
  FlagParser::Flag* flag_x = parser.AddFlag("x");
  FlagParser::Flag* flag_o = parser.AddFlag("o");
  FlagParser::Flag* flag_MF = parser.AddFlag("MF");
  FlagParser::Flag* flag_isysroot = parser.AddFlag("isysroot");
  // TODO: Consider split -fprofile-* flags? Some options take an extra
  // arguement, other do not. Merging such kind of flags do not look good.
  FlagParser::Flag* flag_fprofile = parser.AddPrefixFlag("fprofile-");
  FlagParser::Flag* flag_fprofile_sample_use =
      parser.AddFlag("fprofile-sample-use");
  FlagParser::Flag* flag_fthinlto_index =
      parser.AddPrefixFlag("fthinlto-index=");
  FlagParser::Flag* flag_fdebug_compilation_dir =
      parser.AddFlag("fdebug-compilation-dir");
  FlagParser::Flag* flag_fcoverage_compilation_dir =
      parser.AddPrefixFlag("fcoverage-compilation-dir=");
  FlagParser::Flag* flag_ffile_compilation_dir =
      parser.AddPrefixFlag("ffile-compilation-dir=");
  FlagParser::Flag* flag_fprofile_list = parser.AddPrefixFlag("fprofile-list=");

  parser.AddFlag("wrapper")->SetSeenOutput(&has_wrapper_);
  parser.AddPrefixFlag("fplugin=")->SetSeenOutput(&has_fplugin_);

  // -mllvm takes extra arg.
  // ASAN uses -mllvm -asan-blacklist=$FILE
  // TSAN uses -mllvm -tsan-blacklist=$FILE
  // ASAN uses -mllvm -asan-ignorelist=$FILE
  // TSAN uses -mllvm -tsan-ignorelist=$FILE
  std::vector<std::string> llvm_options;
  parser.AddFlag("mllvm")->SetOutput(&llvm_options);
  FlagParser::Flag* flag_fsanitize_blacklist =
      parser.AddFlag("fsanitize-blacklist");
  FlagParser::Flag* flag_fsanitize_ignorelist =
      parser.AddFlag("fsanitize-ignorelist");
  FlagParser::Flag* flag_fsanitize = parser.AddFlag("fsanitize");
  flag_fsanitize->SetOutput(&compiler_info_flags_);

  FlagParser::Flag* flag_resource_dir = parser.AddFlag("resource-dir");
  flag_resource_dir->SetOutput(&compiler_info_flags_);

  FlagParser::Flag* flag_fdebug_prefix_map =
      parser.AddFlag("fdebug-prefix-map");
  FlagParser::Flag* flag_gsplit_dwarf = parser.AddBoolFlag("gsplit-dwarf");
  flag_gsplit_dwarf->SetOutput(&compiler_info_flags_);

  parser.AddFlag("m")->SetOutput(&compiler_info_flags_);
  FlagParser::Flag* flag_arch = parser.AddFlag("arch");
  flag_arch->SetOutput(&compiler_info_flags_);
  parser.AddFlag("target")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("-target")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("gcc-toolchain")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("-gcc-toolchain")->SetOutput(&compiler_info_flags_);
  // TODO: Uncomment this and remove isysroot_ once we stop
  //               supporting API version 0.
  // parser.AddFlag("isysroot")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("imultilib")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("isystem")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("stdlib++-isystem")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("iquote")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("idirafter")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("-sysroot")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("B")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("iframework")->SetOutput(&compiler_info_flags_);
  parser.AddPrefixFlag("O")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("b")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("V")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("specs")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("-specs")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("std")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("-std")->SetOutput(&compiler_info_flags_);
  parser.AddPrefixFlag("f")->SetOutput(&compiler_info_flags_);
  // Avoid to add -ftrivial-auto-var-init=.. to compiler_info_flags_.
  // See b/138157691.
  parser.AddPrefixFlag("ftrivial-auto-var-init=");
  parser.AddBoolFlag("pthread")->SetOutput(&compiler_info_flags_);
  parser.AddBoolFlag("undef")->SetOutput(&compiler_info_flags_);
  // If pnacl-clang, it need to support --pnacl-bias and --pnacl-*-bias.
  // See: b/17982273
  if (IsPNaClClangCommand(compiler_base_name())) {
    parser.AddPrefixFlag("-pnacl-bias=")->SetOutput(&compiler_info_flags_);
    parser.AddBoolFlag("-pnacl-arm-bias")->SetOutput(&compiler_info_flags_);
    parser.AddBoolFlag("-pnacl-mips-bias")->SetOutput(&compiler_info_flags_);
    parser.AddBoolFlag("-pnacl-i686-bias")->SetOutput(&compiler_info_flags_);
    parser.AddBoolFlag("-pnacl-x86_64-bias")->SetOutput(&compiler_info_flags_);
    parser.AddBoolFlag("-pnacl-allow-translate")
        ->SetOutput(&compiler_info_flags_);
  }
  parser.AddBoolFlag("no-canonical-prefixes")->SetOutput(&compiler_info_flags_);
  parser.AddFlag("Xclang")->SetOutput(&xclang_flags);
  parser.AddFlag("I")->SetValueOutputWithCallback(nullptr,
                                                  &non_system_include_dirs_);

  // TODO: -fmodules is clang only.
  // See https://clang.llvm.org/docs/Modules.html
  // TODO: Support -fbuiltin-module-maps?
  parser.AddBoolFlag("fmodules")->SetSeenOutput(&fmodules);
  parser.AddBoolFlag("fno-implicit-module-maps")
      ->SetSeenOutput(&fno_implicit_module_maps);
  FlagParser::Flag* flag_fmodule_file = parser.AddPrefixFlag("fmodule-file=");
  FlagParser::Flag* flag_fmodule_map_file =
      parser.AddPrefixFlag("fmodule-map-file=");
  // -fmodule-name should not be added to compiler_info_flags_, since
  // -fmodule-name is considered as an input for clang modules, especially for
  // `-Xclang -emit-module`.
  parser.AddPrefixFlag("-fmodule-name=");

  // We should allow both -imacro and --imacro, -include and --include.
  // See: b/10020850.
  std::vector<std::string> includes, imacros;
  parser.AddFlag("imacros")->SetValueOutputWithCallback(nullptr, &imacros);
  parser.AddFlag("-imacros")->SetValueOutputWithCallback(nullptr, &imacros);
  parser.AddFlag("include")->SetValueOutputWithCallback(nullptr, &includes);
  parser.AddFlag("-include")->SetValueOutputWithCallback(nullptr, &includes);
  // TODO: We need to consider the order of -I and -F.
  parser.AddFlag("F")->SetValueOutputWithCallback(nullptr, &framework_dirs_);
  // TODO: Support -iprefix, -I-, and etc.
  MacroStore<true> defined_macro_store(&commandline_macros_);
  MacroStore<false> undefined_macro_store(&commandline_macros_);
  parser.AddFlag("D")->SetCallbackForParsedArgs(&defined_macro_store);
  parser.AddFlag("U")->SetCallbackForParsedArgs(&undefined_macro_store);

  // Special handle for "-W", "-Wa,", "-Wl,", "-Wp,".
  // We want to parse "-Wa,", "-Wp,"
  // We want to mark "-Wl," unknown.
  // However, we want to parse -Wsomething.
  FlagParser::Flag* flag_W = parser.AddPrefixFlag("W");
  FlagParser::Flag* flag_Wa = parser.AddPrefixFlag("Wa,");
  FlagParser::Flag* flag_Wl = parser.AddPrefixFlag("Wl,");
  FlagParser::Flag* flag_Wp = parser.AddPrefixFlag("Wp,");
  std::vector<std::string> assembler_flags;
  std::vector<std::string> preprocessor_flags;
  flag_Wa->SetValueOutputWithCallback(nullptr, &assembler_flags);
  flag_Wp->SetValueOutputWithCallback(nullptr, &preprocessor_flags);

  parser.AddNonFlag()->SetOutput(&input_filenames_);

  parser.Parse(expanded_args_);
  unknown_flags_ = parser.unknown_flag_args();

  arch_ = flag_arch->values();

  ClangFlagsHelper clang_flags_helper(expanded_args_);

  if (flag_fdebug_compilation_dir->seen()) {
    fdebug_compilation_dir_ = flag_fdebug_compilation_dir->GetLastValue();
  } else if (clang_flags_helper.fdebug_compilation_dir()) {
    fdebug_compilation_dir_ = *clang_flags_helper.fdebug_compilation_dir();
  }
  if (flag_fcoverage_compilation_dir->seen()) {
    fcoverage_compilation_dir_ = flag_fcoverage_compilation_dir->GetLastValue();
  } else if (clang_flags_helper.fcoverage_compilation_dir()) {
    fcoverage_compilation_dir_ =
        *clang_flags_helper.fcoverage_compilation_dir();
  }
  if (flag_ffile_compilation_dir->seen()) {
    ffile_compilation_dir_ = flag_ffile_compilation_dir->GetLastValue();
  }
  if (flag_fprofile_list->seen()) {
    const std::vector<std::string>& values = flag_fprofile_list->values();
    for (const auto& value : values) {
      // -fprofile-list doesn't affect system include dirs or
      // predefined macros, so don't include it in compiler_info_flags_.
      optional_input_filenames_.push_back(value);
    }
  }

  // We need to handle -Xclang specially.
  ProcessXclangFlags(xclang_flags);

  // -Wa, is a flag for assembler.
  // -Wa,--noexecstack is often used.
  if (!assembler_flags.empty()) {
    std::vector<std::string> subflags;
    for (const auto& fs : assembler_flags) {
      for (auto&& f : absl::StrSplit(fs, ',')) {
        subflags.emplace_back(f);
      }
    }

    FlagParser pp;
    FlagParser::Options* opts = pp.mutable_options();
    opts->flag_prefix = '-';
    opts->allows_equal_arg = true;
    opts->allows_nonspace_arg = true;
    opts->has_command_name = false;

    pp.AddBoolFlag("-noexecstack");  // --noexecstack to make stack unexecutable
    pp.AddFlag("-defsym");           // --defsym,SYM=VALUE to defin symbol SYM.
    pp.AddPrefixFlag("I");           // -Iout/somewhere; add include path
    pp.AddBoolFlag("gdwarf-2");      // -gdwarf-2; debug info
    pp.AddFlag("march");             // -march=foo; set architecture
    pp.AddFlag("mfpu");              // -mfpu=foo; set cpu

    pp.Parse(subflags);
    for (const auto& unknown : pp.unknown_flag_args()) {
      unknown_flags_.push_back("-Wa," + unknown);
    }
  }

  if (flag_Wl->seen()) {
    // For "-Wl,", Mark the whole flag as unknown.
    // We won't support linker flags.
    for (const auto& v : flag_Wl->values()) {
      unknown_flags_.push_back("-Wl," + v);
    }
  }

  // Note: -Wp,-D -Wp,FOOBAR can be considered as -Wp,-D,FOOBAR
  if (!preprocessor_flags.empty()) {
    std::vector<std::string> subflags;
    for (const auto& fs : preprocessor_flags) {
      for (auto&& f : absl::StrSplit(fs, ',')) {
        subflags.emplace_back(f);
      }
    }

    FlagParser pp;
    FlagParser::Options* opts = pp.mutable_options();
    opts->flag_prefix = '-';
    opts->allows_equal_arg = true;
    opts->allows_nonspace_arg = true;
    opts->has_command_name = false;

    pp.AddFlag("D")->SetCallbackForParsedArgs(&defined_macro_store);
    pp.AddFlag("U")->SetCallbackForParsedArgs(&undefined_macro_store);
    FlagParser::Flag* flag_MD_pp = pp.AddFlag("MD");

    pp.Parse(subflags);

    if (flag_MD_pp->seen()) {
      output_files_.push_back(flag_MD_pp->GetLastValue());
    }
    for (const auto& unknown : pp.unknown_flag_args()) {
      unknown_flags_.push_back("-Wp," + unknown);
    }
  }

  // Check -W flag.
  for (const auto& value : flag_W->values()) {
    if (!IsKnownWarningOption(value)) {
      unknown_flags_.push_back("-W" + value);
    }
  }

  // Check debug flags. We match -g with prefix flag. It covers too much.
  // If the value is not known, we'd like to mark it as unknown option.
  for (const auto& value : flag_g->values()) {
    if (!IsKnownDebugOption(value)) {
      unknown_flags_.push_back("-g" + value);
    }
  }

  if (!has_at_file) {
    // no @file in args.
    CHECK_EQ(args_, expanded_args_);
    expanded_args_.clear();
  }

  if (flag_isysroot->seen())
    isysroot_ = flag_isysroot->GetLastValue();
  if (flag_resource_dir->seen())
    resource_dir_ = flag_resource_dir->GetLastValue();
  if (flag_fsanitize->seen()) {
    for (const auto& value : flag_fsanitize->values()) {
      for (auto&& v : absl::StrSplit(value, ',')) {
        fsanitize_.insert(std::string(v));
      }
    }
  }
  if (flag_fdebug_prefix_map->seen()) {
    for (const auto& value : flag_fdebug_prefix_map->values()) {
      size_t pos = value.find('=');
      if (pos == std::string::npos) {
        LOG(ERROR) << "invalid argument is given to -fdebug-prefix-map:"
                   << value;
        return;
      }
      bool inserted = fdebug_prefix_map_
                          .insert(std::make_pair(value.substr(0, pos),
                                                 value.substr(pos + 1)))
                          .second;
      LOG_IF(INFO, !inserted) << "-fdebug-prefix-map has duplicated entry."
                              << " ignored: " << value;
    }
    // -fdebug-prefix-map does not affect system include dirs or
    // predefined macros.  We do not include it in compiler_info_flags_.
    // Especially for clang, it is only used in lib/CodeGen/CGDebugInfo.cpp,
    // which is code to generate debug info.
  }

  is_successful_ = true;

  mode_ = COMPILE;
  if (flag_E->seen() || flag_M->seen() || flag_MM->seen()) {
    mode_ = PREPROCESS;
  } else if (!flag_c->seen() && !flag_S->seen()) {
    mode_ = LINK;
  }

  if (input_filenames_.size() == 1) {
    if (input_filenames_[0] == "-" || input_filenames_[0] == "/dev/stdin") {
      is_stdin_input_ = true;
    }
  } else if (mode_ != LINK && input_filenames_.size() > 1) {
    std::string buf = absl::StrJoin(input_filenames_, ", ");
    Fail("multiple input file names: " + buf);
  }

  if (!llvm_options.empty()) {
    // TODO: no need to set -*-blacklist options in compiler_info_flags_?
    std::copy(llvm_options.begin(), llvm_options.end(),
              back_inserter(compiler_info_flags_));

    FlagParser llvm_parser;
    FlagParser::Options* opts = llvm_parser.mutable_options();
    opts->flag_prefix = '-';
    opts->allows_equal_arg = true;
    opts->has_command_name = false;

    llvm_parser.AddFlag("asan-blacklist")
        ->SetValueOutputWithCallback(nullptr, &optional_input_filenames_);
    llvm_parser.AddFlag("asan-ignorelist")
        ->SetValueOutputWithCallback(nullptr, &optional_input_filenames_);
    llvm_parser.AddFlag("tsan-blacklist")
        ->SetValueOutputWithCallback(nullptr, &optional_input_filenames_);
    llvm_parser.AddFlag("tsan-ignorelist")
        ->SetValueOutputWithCallback(nullptr, &optional_input_filenames_);
    llvm_parser.AddFlag("enable-ml-inliner");
    llvm_parser.AddFlag("training-log")
        ->SetValueOutputWithCallback(nullptr, &output_files_);
    llvm_parser.AddFlag("ml-inliner-model-under-training")
        ->SetValueOutputWithCallback(nullptr, &optional_input_filenames_);
    llvm_parser.Parse(llvm_options);
  }
  // Any files specified by -fsanitize-blacklist must exist in goma server
  // even if -fno-sanitize-blacklist is set, or clang dies.
  // Please see also:
  // https://github.com/llvm-mirror/clang/blob/5b04748157cbb00ccb3e91f6633a1561b3250e25/lib/Driver/SanitizerArgs.cpp#L485
  if (flag_fsanitize_blacklist->seen()) {
    const std::vector<std::string>& values = flag_fsanitize_blacklist->values();
    for (const auto& value : values) {
      // -fsanitize-blacklist doesn't affect system include dirs or
      // predefined macros, so don't include it in compiler_info_flags_.
      optional_input_filenames_.push_back(value);
    }
  }
  if (flag_fsanitize_ignorelist->seen()) {
    const std::vector<std::string>& values =
        flag_fsanitize_ignorelist->values();
    for (const auto& value : values) {
      // -fsanitize-ignorelist doesn't affect system include dirs or
      // predefined macros, so don't include it in compiler_info_flags_.
      optional_input_filenames_.push_back(value);
    }
  }

  if (flag_x->seen()) {
    compiler_info_flags_.push_back("-x");
    compiler_info_flags_.push_back(flag_x->GetLastValue());
  }
  if (has_nostdinc_) {
    compiler_info_flags_.push_back("-nostdinc");
  }
  if (no_integrated_as) {
    compiler_info_flags_.push_back("-no-integrated-as");
    has_no_integrated_as_ = true;
  }
  if (fno_integrated_as) {
    compiler_info_flags_.push_back("-fno-integrated-as");
    has_no_integrated_as_ = true;
  }
  if (ffreestanding) {
    compiler_info_flags_.push_back("-ffreestanding");
    has_ffreestanding_ = true;
  }
  if (fno_hosted) {
    compiler_info_flags_.push_back("-fno-hosted");
    has_fno_hosted_ = true;
  }
  if (fsyntax_only) {
    compiler_info_flags_.push_back("-fsyntax-only");
    has_fsyntax_only_ = true;
  }
  if (has_ftime_trace_) {
    compiler_info_flags_.push_back("-ftime-trace");
  }
  if (fmodules) {
    compiler_info_flags_.push_back("-fmodules");
    has_fmodules_ = true;
    if (!fno_implicit_module_maps) {
      // -fmodules implies -fimplicit-module-maps
      // unless -fno-implicit-module-maps.
      has_fimplicit_module_maps_ = true;
    }
  }
  // Note: since -fcoverage-mapping does not change predefined macro or
  // system include paths, we do not add it to compiler_info_flags_.

  if (!isysroot_.empty()) {
    compiler_info_flags_.push_back("-isysroot");
    compiler_info_flags_.push_back(isysroot_);
  }

  // Workaround for ChromeOS.
  // https://code.google.com/p/chromium/issues/detail?id=338646
  //
  // TODO: remove this when we drop chromeos wrapper support.
  // In https://code.google.com/p/chromium/issues/detail?id=316963,
  // we are discussing about the drop of chromeos wrapper support.
  // In other words, goma is called by the wrapper, and we do not have
  // the wrapper installed in the goma server.
  for (const auto& it : commandline_macros_) {
    if (it.first == "__KERNEL__" && it.second) {
      compiler_info_flags_.push_back("-D__KERNEL__");
      break;
    }
  }

  // All files specified by -imacros are processed before all files
  // specified by -include.
  std::copy(imacros.begin(), imacros.end(), back_inserter(root_includes_));
  std::copy(includes.begin(), includes.end(), back_inserter(root_includes_));

  if (print_file_name) {
    Fail("not supported on remote");
  }

  if (flag_x->seen()) {
    lang_ = flag_x->GetLastValue();
  } else {
    lang_ = GetLanguage(compiler_name_,
                        (!input_filenames_.empty() ? input_filenames_[0] : ""));
  }
  is_cplusplus_ = (lang_.find("c++") != std::string::npos);
  if (mode_ == COMPILE)
    is_precompiling_header_ = absl::EndsWith(lang_, "-header");

  if (mode_ == LINK && has_ftime_trace_) {
    LOG(WARNING) << "clang uses unpredictable file names for -ftime-trace json "
                    "output when linking";
  }

  {  // outout
    std::string output;
    if (flag_o->seen()) {
      output = flag_o->GetLastValue();
    }
    // Create a default output flag.
    if (output.empty() && !input_filenames_.empty()) {
      absl::string_view stem = GetStem(input_filenames_[0]);
      if (mode_ == LINK) {
        output = "a.out";
      } else if (flag_M->seen() || flag_MM->seen()) {
        // If -M or -MM is specified, output will be done to stdout.
        // However, if -MF is specified, output must be to -MF value.
        if (flag_MF->seen()) {
          output_files_.push_back(flag_MF->GetLastValue());
        }
        if (has_ftime_trace_) {
          output_files_.push_back(".json");
        }
        return;
      } else if (flag_E->seen()) {
        // preprocess. No output.
        // If -E, -M and -MF are all specified in the command line,
        // gcc outputs deps to -MF value.
        if (has_ftime_trace_) {
          output_files_.push_back("-.json");
        }
        return;
      } else if (flag_S->seen()) {
        output = absl::StrCat(stem, ".s");
      } else if (is_precompiling_header_) {
        output = input_filenames_[0] + ".gch";
      } else if (flag_c->seen()) {
        output = absl::StrCat(stem, ".o");
      }
    }

    if (output.empty()) {
      if (has_ftime_trace_) {
        if (flag_M->seen() || flag_MM->seen()) {
          output_files_.push_back(".json");
        } else {
          output_files_.push_back("-.json");
        }
      }
    } else {
      // make output as output_files_[0].
      // Since we logs output_files_[0], it is usually preferred to be so.
      output_files_.insert(output_files_.begin(), output);

      // if -MD or -MMD flag was specified, and -MF flag was not specified,
      // assume .d file output.
      if ((flag_MD->seen() || flag_MMD->seen()) && !flag_MF->seen()) {
        size_t ext_start = output.rfind('.');
        if (ext_start != std::string::npos) {
          output_files_.push_back(output.substr(0, ext_start) + ".d");
        }
      }

      if (flag_gsplit_dwarf->seen()) {
        if (mode_ == COMPILE) {
          output_files_.push_back(
              file::JoinPath(GetDirname(output), GetStem(output)) + ".dwo");
        }

        const std::string& input0 = input_filenames_[0];
        if (mode_ == LINK && GetExtension(input0) != "o") {
          output_files_.push_back(
              file::JoinPath(GetDirname(input0), GetStem(input0)) + ".dwo");
        }
      }

      if (has_ftime_trace_) {
        if (flag_M->seen() || flag_MM->seen()) {
          output_files_.push_back(".json");
        } else {
          size_t ext_start = output.rfind('.');
          if (ext_start != std::string::npos) {
            output_files_.push_back(output.substr(0, ext_start) + ".json");
          } else {
            output_files_.push_back(output + ".json");
          }
        }
      }
    }
  }

  if (flag_MF->seen()) {
    output_files_.push_back(flag_MF->GetLastValue());
  }

  bool use_profile_input = false;
  absl::string_view profile_input_dir = ".";

  for (absl::string_view value : flag_fprofile->values()) {
    if (absl::StartsWith(value, "instr-use=")) {
      continue;
    }
    compiler_info_flags_.emplace_back(absl::StrCat("-fprofile-", value));

    // Pick the last profile dir, this is how GCC works.
    if (absl::ConsumePrefix(&value, "dir=") ||
        absl::ConsumePrefix(&value, "generate=")) {
      profile_input_dir = value;
    }
  }

  for (absl::string_view value : flag_fprofile->values()) {
    // -fprofile-use is alias of -fprofile-instr-use
    // https://github.com/llvm/llvm-project/blob/90c78073f73eac58f4f8b4772a896dc8aac023bc/clang/include/clang/Driver/Options.td#L783
    use_profile_input |= absl::StartsWith(value, "use");
    use_profile_input |= absl::StartsWith(value, "instr-use");

    if (absl::ConsumePrefix(&value, "use=")) {
      // https://clang.llvm.org/docs/ClangCommandLineReference.html#cmdoption-clang1-fprofile-use
      if (IsClangCommand(compiler_name_) &&
          file::IsDirectory(
              file::JoinPathRespectAbsolute(cwd, profile_input_dir, value),
              file::Defaults())
              .ok()) {
        optional_input_filenames_.push_back(file::JoinPathRespectAbsolute(
            profile_input_dir, value, "default.profdata"));
      } else {
        optional_input_filenames_.push_back(
            file::JoinPathRespectAbsolute(profile_input_dir, value));
      }
    } else if (absl::ConsumePrefix(&value, "instr-use=")) {
      optional_input_filenames_.push_back(
          file::JoinPathRespectAbsolute(profile_input_dir, value));
    }
  }

  if (fmodules) {
    // -fmodule-file=[<name>=]<file>
    // According to my experiment, if several -fmodule-file are specified,
    // only the first one is valid.
    // -fmodule-file works only if -fmodules is specified.
    //
    // See https://clang.llvm.org/docs/Modules.html
    if (flag_fmodule_file->seen() && !flag_fmodule_file->values().empty()) {
      absl::string_view path = flag_fmodule_file->value(0);
      absl::string_view::size_type pos = path.find('=');
      if (pos != absl::string_view::npos) {
        clang_module_file_.first = std::string(path.substr(0, pos));
        clang_module_file_.second = std::string(path.substr(pos + 1));
      } else {
        clang_module_file_.first = "";
        clang_module_file_.second = std::string(path);
      }
    }

    // -fmodule-map-file=<file>
    if (flag_fmodule_map_file->seen() &&
        !flag_fmodule_map_file->values().empty()) {
      clang_module_map_file_ = flag_fmodule_map_file->values().front();
    }
  }

  if (!IsClangCommand(compiler_name_) && use_profile_input &&
      !is_precompiling_header_) {
    for (const auto& filename : input_filenames_) {
      size_t ext_start = filename.rfind('.');
      if (ext_start == std::string::npos)
        continue;
      size_t last_dir = filename.rfind('/');
      if (last_dir == std::string::npos)
        last_dir = 0;
      else
        last_dir++;
      optional_input_filenames_.push_back(file::JoinPath(
          profile_input_dir,
          filename.substr(last_dir, ext_start - last_dir) + ".gcda"));
    }
  }
  if (flag_fprofile_sample_use->seen()) {
    optional_input_filenames_.push_back(
        flag_fprofile_sample_use->GetLastValue());
  }
  if (flag_fthinlto_index->seen()) {
    optional_input_filenames_.push_back(flag_fthinlto_index->GetLastValue());
    thinlto_index_ = flag_fthinlto_index->GetLastValue();
  }
}

const std::vector<std::string> GCCFlags::include_dirs() const {
  std::vector<std::string> dirs(non_system_include_dirs_);
  std::copy(framework_dirs_.begin(), framework_dirs_.end(),
            back_inserter(dirs));
  return dirs;
}

bool GCCFlags::IsClientImportantEnv(const char* env) const {
  if (IsServerImportantEnv(env)) {
    return true;
  }

  static const char* kClientImportantEnvs[] = {
      // Allow WINEDEBUG= only in client.
      "WINDEBUG=",

      // Allow DEVELOPER_DIR only in client.
      "DEVELOPER_DIR=",

      // environments used by vpython.
      "VPYTHON_VIRTUALENV_ROOT=",
      "LUCI_CONTEXT=",
      "CIPD_CACHE_DIR=",
  };
  for (const char* e : kClientImportantEnvs) {
    if (absl::StartsWith(env, e)) {
      return true;
    }
  }

  static const char* kClientImportantEnvsIgnoreCase[] = {
      // These are used for nacl on Win.
      // Don't send this to server.
      "PATHEXT=",
      "SystemRoot=",

      // environments used by vpython.
      "HOMEDRIVE=",
      "HOMEPATH=",
      "USERPROFILE=",
  };
  for (const char* e : kClientImportantEnvsIgnoreCase) {
    if (absl::StartsWithIgnoreCase(env, e)) {
      return true;
    }
  }

  return false;
}

bool GCCFlags::IsServerImportantEnv(const char* env) const {
  // http://gcc.gnu.org/onlinedocs/gcc/Environment-Variables.html
  //
  // Although ld(1) manual mentions following variables, they are not added
  // without actual needs. That is because it may lead security risks and
  // gold (linker used by chromium) seems not use them.
  // - LD_RUN_PATH
  // - LD_LIBRARY_PATH
  //
  // - MACOSX_DEPLOYMENT_TARGET and SDKROOT are used for Darwin targets.
  // https://clang.llvm.org/docs/CommandGuide/clang.html#envvar-MACOSX_DEPLOYMENT_TARGET
  // https://llvm.org/viewvc/llvm-project/cfe/trunk/lib/Driver/ToolChains/Darwin.cpp?revision=346276&view=markup
  //
  // PWD is used for current working directory. b/27487704

  static const char* kCheckEnvs[] = {
      "LIBRARY_PATH=",
      "CPATH=",
      "C_INCLUDE_PATH=",
      "CPLUS_INCLUDE_PATH=",
      "OBJC_INCLUDE_PATH=",
      // TODO: remove this when python2 compatibility is removed
      //                    from chromeos-chrome package build, and we do not
      //                    need to set PNACLPYTHON as a workaround.
      "PNACLPYTHON=",
      "DEPENDENCIES_OUTPUT=",
      "SUNPRO_DEPENDENCIES=",
      "MACOSX_DEPLOYMENT_TARGET=",
      "SDKROOT=",
      "PWD=",

      // ChromeOS toolchain wrapper.
      // http://b/197903824
      "FORCE_DISABLE_WERROR",
  };

  for (const char* check_env : kCheckEnvs) {
    if (absl::StartsWith(env, check_env)) {
      return true;
    }
  }

  return false;
}

/* static */
void GCCFlags::DefineFlags(FlagParser* parser) {
  FlagParser::Options* opts = parser->mutable_options();
  opts->flag_prefix = '-';
  opts->allows_equal_arg = true;
  opts->allows_nonspace_arg = true;

  // clang options can be taken from:
  // https://github.com/llvm/llvm-project/blob/main/clang/include/clang/Driver/Options.td
  // gcc options
  // https://gcc.gnu.org/onlinedocs/gcc-6.4.0/gcc/Option-Summary.html#Option-Summary

  static const struct {
    const char* name;
    FlagType flag_type;
  } kFlags[] = {
      // gcc/clang flags
      {"-C", kBool},  // preprocessor option; don't remove comment
      {"-P",
       kBool},  // preprocessor option; Disable linemarker output in -E mode
      {"-include", kNormal},  // preprocess <file> first
      {"-macros", kNormal},   // preprocess <file> first
      {"-param", kNormal},
      {"-sysroot", kNormal},
      {"-version", kBool},  // --version
      {"B", kNormal},       // add dir to compiler's search paths
      {"D", kNormal},       // preprocessor defines
      {"F", kNormal},
      {"I", kNormal},   // add dir to header search paths
      {"L", kNormal},   // add dir to linker search paths
      {"MF", kNormal},  // specify dependency output
      {"MP", kBool},    // Create phony target for each dependency (other than
                        // main file)
      {"MQ", kBool},    // Specify name of main file output to quote in depfile
      {"MT", kNormal},
      {"Qunused-arguments",
       kBool},           // Don't emit warning for unused driver arguments
      {"V", kNormal},    // specify target version
      {"W", kPrefix},    // -Wsomething; disable/disable warnings
      {"Wa,", kPrefix},  // Options to assembly
      {"Wl,", kPrefix},  // Options to linker
      {"Wp,", kPrefix},  // Options to proprocessor
      {"Xassembler", kNormal},
      {"Xlinker", kNormal},
      {"Xpreprocessor", kNormal},
      {"ansi", kBool},        // -ansi. choose c dialect
      {"arch", kNormal},      // processor type
      {"b", kNormal},         // specify target machine
      {"dA", kBool},          // Annotate the assembler output with
                              // miscellaneous debugging information.
      {"dD", kBool},          // Like '-dM', without predefined macros etc.
      {"dM", kBool},          // Generate a list of ‘#define’ directiv.
      {"fplugin=", kPrefix},  // -fplugin=<dsopath>; gcc plugin
      {"g", kPrefix},  // debug information. NOTE: Needs special treatment.
      {"gsplit-dwarf", kBool},  // to enable the generation of split DWARF.
      {"idirafter", kNormal},
      {"iframework", kNormal},
      {"imacros", kNormal},  // preprocess <file> first
      {"imultilib", kNormal},
      {"include", kNormal},  // preprocess <file> first
      {"iquote", kNormal},
      {"isysroot", kNormal},
      {"isystem", kNormal},
      {"stdlib++-isystem", kNormal},
      {"m", kNormal},       // machine dependent options
      {"o", kNormal},       // specify output
      {"pedantic", kBool},  // old form of -Wpedantic (older gcc has this)
      {"pg", kBool},        // Generate extra code for gprof
      {"specs", kNormal},
      {"std", kNormal},
      {"target", kNormal},
      {"v", kBool},    // Show commands to run and use verbose output
      {"w", kBool},    // Inhibit all warning messages.
      {"x", kNormal},  // specify language
      {"fdebug-compilation-dir", kNormal},

      // darwin options
      {"-serialize-diagnostics", kNormal},
      {"allowable_client", kNormal},
      {"client_name", kNormal},
      {"compatibility_version", kNormal},
      {"current_version", kNormal},
      {"dylib_file", kNormal},
      {"dylinker_install_name", kNormal},
      {"exported_symbols_list", kNormal},
      {"filelist", kNormal},
      {"framework", kNormal},
      {"image_base", kNormal},
      {"init", kNormal},
      {"install_name", kNormal},
      {"multiply_defined", kNormal},
      {"multiply_defined_unused", kNormal},
      {"no-canonical-prefixes", kBool},
      {"pagezero_size", kNormal},
      {"read_only_relocs", kNormal},
      {"seg_addr_table", kNormal},
      {"seg_addr_table_filename", kNormal},
      {"segs_read_only_addr", kNormal},
      {"segs_read_write_addr", kNormal},
      {"sub_library", kNormal},
      {"sub_umbrella", kNormal},
      {"umbrella", kNormal},
      {"undefined", kNormal},
      {"unexported_symbols_list", kNormal},
      {"weak_reference_mismatches", kNormal},
      // TODO: -segproto takes 3 arguments (segname, max_prot and
      // init_prot)
      // TODO: -segaddr takes 2 arguments (name and address)
      // TODO: -sectobjectsymbols takes 2 arguments (segname and sectname)
      // TODO: -sectorder takes 3 arguments (segname, sectname and
      // orderfile)

      // for clang
      {"-coverage", kBool},  // take code coverage
      {"-no-system-header-prefix=",
       kPrefix},                           // Specify header is not a system
                                           // header
                                           // --no-system-header-prefix=<prefix>
      {"-system-header-prefix", kNormal},  // Specify header is a system header
                                           // (for diagnosis)
                                           // --system-header-prefix=<prefix> or
                                           // --sytem-header-prefix=<arg>
      {"Xanalyzer", kNormal},
      {"Xclang", kNormal},
      {"gcc-toolchain", kNormal},
      {"nostdlibinc", kBool},  // Do not search the standard system directories
                               // for include files, but do search compiler
                               // builtin include directories.
      {"print-libgcc-file-name", kBool},  // Print the library path for the
                                          // currently used compiler runtime
                                          // library
      {"print-prog-name=", kPrefix},      // Print the full program path of
                                          //  <name> -print-prog-name=<name>

      // linker flags
      // https://gcc.gnu.org/onlinedocs/gcc/Link-Options.html
      {"nodefaultlibs", kBool},  // Do not use the standard system libraries
      {"nostdlib",
       kBool},  // Do not use the standard system startup files or libraries
      {"nostdlib++", kBool},  // Don't use the ld_stdlib++ section
      {"pie",
       kBool},  // Produce a dynamically linked position independent executable
      {"rdynamic", kBool},  // Pass the flag -export-dynamic to the ELF linker
      {"static", kBool},    // this overrides -pie and prevents linking with the
                            // shared libraries.
  };

  for (const auto& f : kFlags) {
    switch (f.flag_type) {
      case kNormal:
        parser->AddFlag(f.name);
        break;
      case kPrefix:
        parser->AddPrefixFlag(f.name);
        break;
      case kBool:
        parser->AddBoolFlag(f.name);
        break;
    }
  }
}

// static
bool GCCFlags::IsKnownWarningOption(absl::string_view option) {
  // TODO: If we can have constexpr version of is_sorted,
  // we can check this in compile time.
  DCHECK(std::is_sorted(
      std::begin(kKnownWarningOptions), std::end(kKnownWarningOptions),
      [](absl::string_view lhs, absl::string_view rhs) { return lhs < rhs; }))
      << "kKnownWarningOptions must be sorted";

  // for "foo=x", take "foo=" only.
  std::string::size_type p = option.find('=');
  if (p != std::string::npos) {
    option = option.substr(0, p + 1);  // Keep '='.
  }

  // Remove "no-"
  if (absl::StartsWith(option, "no-")) {
    option = option.substr(strlen("no-"));
  }

  return std::binary_search(std::begin(kKnownWarningOptions),
                            std::end(kKnownWarningOptions), option);
}

// static
bool GCCFlags::IsKnownDebugOption(absl::string_view v) {
  // See https://gcc.gnu.org/onlinedocs/gcc/Debugging-Options.html
  // -gz is not handled here, since it's used like -gz=<type>.
  // It's not suitable to handle it here.
  static const char* const kKnownDebugOptions[]{
      "",
      "0",
      "1",
      "2",
      "3",
      "column-info",
      "dw",
      "dwarf",
      "dwarf-2",
      "dwarf-3",
      "dwarf-4",
      "dwarf-5",
      "gdb",
      "gdb1",
      "gdb2",
      "gdb3",
      "gnu-pubnames",
      "line-tables-only",
      "no-column-info",
      "no-record-gcc-switches",
      "no-strict-dwarf",
      "pubnames",
      "record-gcc-switches",
      "split-dwarf",
      "stabs",
      "stabs+",
      "stabs0",
      "stabs1",
      "stabs2",
      "stabs3",
      "strict-dwarf",
      "vms",
      "vms0",
      "vms1",
      "vms2",
      "vms3",
      "xcoff",
      "xcoff+",
      "xcoff0",
      "xcoff1",
      "xcoff2",
      "xcoff3",
  };

  DCHECK(std::is_sorted(
      std::begin(kKnownDebugOptions), std::end(kKnownDebugOptions),
      [](absl::string_view lhs, absl::string_view rhs) { return lhs < rhs; }))
      << "kKnownDebugOptions must be sorted";

  return std::binary_search(std::begin(kKnownDebugOptions),
                            std::end(kKnownDebugOptions), v);
}

/* static */
std::string GCCFlags::GetLanguage(const std::string& compiler_name,
                                  const std::string& input_filename) {
  // Decision based on a compiler name.
  bool is_cplusplus = false;
  if (compiler_name.find("g++") != std::string::npos) {
    is_cplusplus = true;
  }
  if (input_filename.empty())
    return is_cplusplus ? "c++" : "c";

  // Decision based on a file extension.
  absl::string_view suffix = GetExtension(input_filename);
  if (!is_cplusplus && suffix != "c") {
    // GCC may change the language by suffix of input files.
    // See gcc/gcc.c and gcc/cp/lang-specs.h .
    // Note that slow operation is OK because we've checked .c first
    // so we come here rarely.
    if (suffix == "cc" || suffix == "cxx" || suffix == "cpp" ||
        suffix == "cp" || suffix == "c++" || suffix == "C" || suffix == "CPP" ||
        suffix == "ii" || suffix == "H" || suffix == "hpp" || suffix == "hp" ||
        suffix == "hxx" || suffix == "h++" || suffix == "HPP" ||
        suffix == "tcc" || suffix == "hh" || suffix == "mm" || suffix == "M" ||
        suffix == "mii") {
      is_cplusplus = true;
    }
  }
  if (is_cplusplus && suffix == "m") {
    // g++ and clang++ think .m as objc not objc++. (b/11521718)
    is_cplusplus = false;
  }

  const std::string lang = is_cplusplus ? "c++" : "c";
  if (!suffix.empty()) {
    if (suffix[0] == 'm' || suffix[0] == 'M')
      return std::string("objective-") + lang;

    if (suffix[0] == 'h' || suffix[0] == 'H' || suffix == "tcc")
      return lang + "-header";
  }
  return lang;
}

void GCCFlags::ProcessXclangFlags(
    const std::vector<std::string>& xclang_flags) {
  // xclang_flags should be like:
  // ["-Xclang", "something", "-Xclang", "foo"]
  // The odd element must be always "-Xclang".
  // However, the current flag parser might accept -Xclang=foo.
  // So, evil input like
  // ["-Xclang=foo", "-Xclang", "bar"]
  // might come.

  for (size_t i = 0; i < xclang_flags.size();) {
    // Not just "-Xclang". Keep as-is.
    if (xclang_flags[i] != "-Xclang") {
      compiler_info_flags_.push_back(xclang_flags[i]);
      ++i;
      continue;
    }

    DCHECK_EQ("-Xclang", xclang_flags[i]) << xclang_flags;

    // Last orphan -Xclang? Keep as-is anyway.
    if (i + 1 >= xclang_flags.size()) {
      compiler_info_flags_.push_back(xclang_flags[i]);
      ++i;
      continue;
    }

    // There are several -Xclang flags to cause CompilerInfoBuilder to fail.
    // These flags usually won't change CompilerInfo, so it is OK if it's not
    // included in compiler_info_flags_.

    // -Xclang -emit-module needs special handle for include processor.
    if (xclang_flags[i + 1] == "-emit-module") {
      has_emit_module_ = true;
      i += 2;
      continue;
    }

    // -Xclang -emit-ast prints AST to stdout, so it makes CompilerInfoBuilder
    // be confused. This flag doesn't change CompilerInfo.
    // -Xclang -emit-pch prints pch object to stdout, so it will fail to take
    // macro values.
    if (xclang_flags[i + 1] == "-emit-ast" ||
        xclang_flags[i + 1] == "-emit-pch") {
      i += 2;
      continue;
    }

    // TODO: There might be more flags to cause CompilerInfoBuilder
    // to fail.

    // everything else
    compiler_info_flags_.push_back(xclang_flags[i]);
    compiler_info_flags_.push_back(xclang_flags[i + 1]);
    i += 2;
  }
}

std::string GetCxxCompilerVersionFromCommandOutputs(
    const std::string& /* command */,
    const std::string& dumpversion,
    const std::string& version) {
  std::string result(GetFirstLine(dumpversion));
  // Both GCC and clang contain their full version info in the first
  // line of their --version output.
  // E.g., clang version 2.9 (trunk 127176), gcc (Ubuntu 4.4.3-4ubuntu5) 4.4.3
  result += "[" + NormalizeGccVersion(GetFirstLine(version)) + "]";
  return result;
}

std::string GetFirstLine(const std::string& buf) {
  size_t pos = buf.find_first_of("\r\n");
  if (pos == std::string::npos) {
    return buf;
  }
  return buf.substr(0, pos);
}

std::string NormalizeGccVersion(const std::string& version) {
  // gcc version string format:
  // <program name> <package version string> <version string>
  // Note: <package version string> is "(<something>)" by default.
  // Then, we can expect the string until '(' is <program name>.
  size_t pos = version.find('(');
  if (pos == std::string::npos)
    return version;

  const std::string program_name = version.substr(0, pos);
  // No need to normalize clang.
  if (program_name.find("clang") != std::string::npos)
    return version;
  // Only need to normalize cc/c++/gcc/g++/<arch>-<os>-gcc/<arch>-<os>-g++.
  // TODO: should we handle <arch>-<os>-cc or so?
  if (program_name.find("g++") == std::string::npos &&
      program_name.find("gcc") == std::string::npos && program_name != "c++ " &&
      program_name != "cc ") {
    return version;
  }

  return version.substr(pos);
}

/* static */
bool GCCFlags::IsGCCCommand(absl::string_view arg) {
  const absl::string_view stem = GetStem(arg);
  if (stem.find("gcc") != absl::string_view::npos ||
      stem.find("g++") != absl::string_view::npos)
    return true;
  // As a substring "cc" would be found even in other commands such
  // as "distcc", we check if the name is "cc" or "*-cc"
  // (e.g., "i586-mingw32msvc-cc").
  if (stem == "c++" || stem == "cc" || absl::EndsWith(arg, "-cc"))
    return true;
  if (IsClangCommand(arg))
    return true;
  return false;
}

/* static */
bool GCCFlags::IsClangCommand(absl::string_view arg) {
  const absl::string_view stem = GetStem(arg);
  // allow pnacl-clang etc.
  // However, don't allow clang-tidy.
  if (stem == "clang" || stem == "clang++" || absl::EndsWith(stem, "-clang") ||
      absl::EndsWith(stem, "-clang++") || stem == "llvm")
    return true;

  // For b/25937763 but we should not consider the followings as clang:
  // clang-cl, clang-check, clang-tblgen, clang-format, clang-tidy-diff, etc.
  constexpr absl::string_view kClang = "clang-";
  constexpr absl::string_view kClangxx = "clang++-";
  absl::string_view version = stem;
  if (absl::StartsWith(stem, kClang))
    version.remove_prefix(kClang.size());
  else if (absl::StartsWith(stem, kClangxx))
    version.remove_prefix(kClangxx.size());
  if (stem == version)
    return false;
  // version should only have digits and '.'.
  return version.find_first_not_of("0123456789.") == absl::string_view::npos;
}

/* static */
bool GCCFlags::IsNaClGCCCommand(absl::string_view arg) {
  const absl::string_view basename = GetBasename(arg);
  return basename.find("nacl-gcc") != absl::string_view::npos ||
         basename.find("nacl-g++") != absl::string_view::npos;
}

/* static */
bool GCCFlags::IsNaClClangCommand(absl::string_view arg) {
  // pnacl-clang and pnalc-clang++ are not nacl-clang.
  if (IsPNaClClangCommand(arg)) {
    return false;
  }
  const absl::string_view basename = GetBasename(arg);
  return basename.find("nacl-clang") != absl::string_view::npos ||
         basename.find("nacl-clang++") != absl::string_view::npos;
}

/* static */
bool GCCFlags::IsPNaClClangCommand(absl::string_view arg) {
  const absl::string_view stem = GetStem(arg);
  return stem == "pnacl-clang" || stem == "pnacl-clang++";
}

}  // namespace devtools_goma
