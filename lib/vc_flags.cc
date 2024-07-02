// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/vc_flags.h"

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

namespace {

// Normalize paths surrounded by '"' to paths without it.
// e.g. "c:\Windows\Program Files" -> c:\Windows\Program Files.
std::string NormalizeWin32Path(absl::string_view path) {
  // TODO: omit orphan '"' at the end of path?
  if (absl::StartsWith(path, "\"")) {
    if (absl::EndsWith(path, "\"")) {
      path = path.substr(1, path.length() - 2);
    } else {
      path = path.substr(1);
    }
  }
  return std::string(path);
}

std::string ToNormalizedBasename(absl::string_view in) {
  // Note file::Basename does not understand "\\" as a path delimiter
  // on non-Windows.
  return absl::AsciiStrToLower(GetBasename(in));
}

}  // namespace

class Win32PathNormalizer : public FlagParser::Callback {
 public:
  // Returns parsed flag value of value for flag.
  std::string ParseFlagValue(const FlagParser::Flag& flag,
                             const std::string& value) override;
};

std::string Win32PathNormalizer::ParseFlagValue(
    const FlagParser::Flag& /* flag */,
    const std::string& value) {
  return NormalizeWin32Path(value);
}

/* static */
bool VCFlags::IsVCCommand(absl::string_view arg) {
  // As a substring "cl" would be found in other commands like "clang" or
  // "nacl-gcc".  Also, "cl" is case-insensitive on Windows and can be postfixed
  // with ".exe".
  const std::string& s = ToNormalizedBasename(arg);
  return s == "cl.exe" || s == "cl";
}

/* static */
bool VCFlags::IsClangClCommand(absl::string_view arg) {
  const std::string& s = ToNormalizedBasename(arg);
  return s == "clang-cl.exe" || s == "clang-cl";
}

/* static */
std::string VCFlags::GetCompilerName(absl::string_view arg) {
  if (IsClangClCommand(arg)) {
    return "clang-cl";
  }
  return "cl.exe";
}

std::string VCFlags::compiler_name() const {
  return GetCompilerName(compiler_name_);
}

VCFlags::VCFlags(const std::vector<std::string>& args, const std::string& cwd)
    : CxxFlags(args, cwd),
      is_cplusplus_(true),
      ignore_stdinc_(false),
      has_Brepro_(false),
      require_mspdbserv_(false),
      has_ftime_trace_(false) {
  bool result =
      ExpandArgs(cwd, args, &expanded_args_, &optional_input_filenames_);
  if (!result) {
    Fail("Unable to expand args");
    return;
  }

  FlagParser parser;
  DefineFlags(&parser);
  Win32PathNormalizer normalizer;

  // Compile only, no link
  FlagParser::Flag* flag_c = parser.AddBoolFlag("c");

  // Preprocess only, do not compile
  FlagParser::Flag* flag_E = parser.AddBoolFlag("E");
  FlagParser::Flag* flag_EP = parser.AddBoolFlag("EP");
  FlagParser::Flag* flag_P = parser.AddBoolFlag("P");

  // Ignore "standard places".
  FlagParser::Flag* flag_X = parser.AddBoolFlag("X");

  // Compile file as .c
  FlagParser::Flag* flag_Tc = parser.AddFlag("Tc");

  // Compile all files as .c
  FlagParser::Flag* flag_TC = parser.AddBoolFlag("TC");

  // Compile file as .cpp
  FlagParser::Flag* flag_Tp = parser.AddFlag("Tp");

  // Compile all files as .cpp
  FlagParser::Flag* flag_TP = parser.AddBoolFlag("TP");

  // Specify output.
  FlagParser::Flag* flag_o = parser.AddFlag("o");  // obsoleted but always there
  FlagParser::Flag* flag_Fo = parser.AddPrefixFlag("Fo");  // obj file path
  FlagParser::Flag* flag_Fe = parser.AddPrefixFlag("Fe");  // exe file path

  // Optimization prefix
  parser.AddPrefixFlag("O")->SetOutput(&compiler_info_flags_);

  // M[DT]d? define _DEBUG, _MT, and _DLL.
  parser.AddPrefixFlag("MD")->SetOutput(&compiler_info_flags_);
  parser.AddPrefixFlag("MT")->SetOutput(&compiler_info_flags_);

  // standard
  parser.AddBoolFlag("permissive-")->SetOutput(&compiler_info_flags_);
  FlagParser::Flag* flag_std = parser.AddPrefixFlag("std:");  // e.g./std:c++17
  flag_std->SetOutput(&compiler_info_flags_);

  // Additional include path.
  parser.AddFlag("I")->SetValueOutputWithCallback(&normalizer, &include_dirs_);

  MacroStore<true> defined_macro_store(&commandline_macros_);
  MacroStore<false> undefined_macro_store(&commandline_macros_);
  parser.AddFlag("D")->SetCallbackForParsedArgs(&defined_macro_store);
  parser.AddFlag("U")->SetCallbackForParsedArgs(&undefined_macro_store);

  // specifies the architecture for code generation.
  // It is passed to compiler_info_flags_ to get macros.
  parser.AddFlag("arch")->SetOutput(&compiler_info_flags_);

  // Flags that affects predefined macros
  FlagParser::Flag* flag_ZI = parser.AddBoolFlag("ZI");
  FlagParser::Flag* flag_RTC = parser.AddPrefixFlag("RTC");
  FlagParser::Flag* flag_Zc_wchar_t = parser.AddBoolFlag("Zc:wchar_t");

  FlagParser::Flag* flag_Zi = parser.AddBoolFlag("Zi");

  parser.AddFlag("FI")->SetValueOutputWithCallback(nullptr, &root_includes_);

  FlagParser::Flag* flag_Yc = parser.AddPrefixFlag("Yc");
  FlagParser::Flag* flag_Yu = parser.AddPrefixFlag("Yu");
  FlagParser::Flag* flag_Fp = parser.AddPrefixFlag("Fp");

  // Machine options used by clang-cl.
  FlagParser::Flag* flag_m = parser.AddFlag("m");
  FlagParser::Flag* flag_fmsc_version = parser.AddPrefixFlag("fmsc-version=");
  FlagParser::Flag* flag_fms_compatibility_version =
      parser.AddPrefixFlag("fms-compatibility-version=");
  FlagParser::Flag* flag_resource_dir = nullptr;
  FlagParser::Flag* flag_fdebug_compilation_dir = nullptr;
  FlagParser::Flag* flag_fcoverage_compilation_dir = nullptr;
  FlagParser::Flag* flag_ffile_compilation_dir = nullptr;
  FlagParser::Flag* flag_fsanitize = parser.AddFlag("fsanitize");
  FlagParser::Flag* flag_fthinlto_index =
      parser.AddPrefixFlag("fthinlto-index=");
  FlagParser::Flag* flag_fsanitize_blacklist = nullptr;
  FlagParser::Flag* flag_fsanitize_ignorelist = nullptr;
  FlagParser::Flag* flag_fprofile_list = nullptr;
  FlagParser::Flag* flag_mllvm = parser.AddFlag("mllvm");
  FlagParser::Flag* flag_isystem = parser.AddFlag("isystem");
  // TODO: check -iquote?
  // http://clang.llvm.org/docs/UsersManual.html#id8
  FlagParser::Flag* flag_imsvc = parser.AddFlag("imsvc");
  FlagParser::Flag* flag_vctoolsdir = parser.AddFlag("vctoolsdir");
  FlagParser::Flag* flag_vctoolsversion = parser.AddFlag("vctoolsversion");
  FlagParser::Flag* flag_winsdkdir = parser.AddFlag("winsdkdir");
  FlagParser::Flag* flag_winsdkversion = parser.AddFlag("winsdkversion");
  FlagParser::Flag* flag_winsysroot = parser.AddFlag("winsysroot");
  FlagParser::Flag* flag_clang_std = parser.AddFlag("std");  // e.g. -std=c11
  FlagParser::Flag* flag_no_canonical_prefixes =
      parser.AddBoolFlag("no-canonical-prefixes");
  FlagParser::Flag* flag_target = parser.AddFlag("target");
  FlagParser::Flag* flag_hyphen_target = parser.AddFlag("-target");
  std::vector<std::string> incremental_linker_flags;
  parser.AddBoolFlag("Brepro")->SetOutput(&incremental_linker_flags);
  parser.AddBoolFlag("Brepro-")->SetOutput(&incremental_linker_flags);
  if (compiler_name() == "clang-cl") {
    flag_m->SetOutput(&compiler_info_flags_);
    flag_fmsc_version->SetOutput(&compiler_info_flags_);
    flag_fms_compatibility_version->SetOutput(&compiler_info_flags_);
    flag_resource_dir = parser.AddFlag("resource-dir");
    flag_resource_dir->SetOutput(&compiler_info_flags_);

    flag_fdebug_compilation_dir = parser.AddFlag("fdebug-compilation-dir");
    flag_fcoverage_compilation_dir =
        parser.AddPrefixFlag("fcoverage-compilation-dir=");
    flag_ffile_compilation_dir = parser.AddPrefixFlag("ffile-compilation-dir=");
    flag_fprofile_list = parser.AddPrefixFlag("fprofile-list=");
    flag_fsanitize->SetOutput(&compiler_info_flags_);
    // TODO: do we need to support more sanitize options?
    flag_fsanitize_blacklist = parser.AddFlag("fsanitize-blacklist=");
    flag_fsanitize_ignorelist = parser.AddFlag("fsanitize-ignorelist=");
    flag_mllvm->SetOutput(&compiler_info_flags_);
    flag_isystem->SetOutput(&compiler_info_flags_);
    flag_imsvc->SetOutput(&compiler_info_flags_);
    flag_vctoolsdir->SetOutput(&compiler_info_flags_);
    flag_vctoolsversion->SetOutput(&compiler_info_flags_);
    flag_winsdkdir->SetOutput(&compiler_info_flags_);
    flag_winsdkversion->SetOutput(&compiler_info_flags_);
    flag_winsysroot->SetOutput(&compiler_info_flags_);
    flag_clang_std->SetOutput(&compiler_info_flags_);
    flag_no_canonical_prefixes->SetOutput(&compiler_info_flags_);
    flag_target->SetOutput(&compiler_info_flags_);
    flag_hyphen_target->SetOutput(&compiler_info_flags_);

    parser.AddBoolFlag("w")->SetOutput(&compiler_info_flags_);

    parser.AddBoolFlag("fcoverage-mapping")
        ->SetSeenOutput(&has_fcoverage_mapping_);
    parser.AddBoolFlag("ftime-trace")->SetSeenOutput(&has_ftime_trace_);

    // Make these understood.
    parser.AddBoolFlag(
        "fansi-escape-codes");  // Use ANSI escape codes for diagnostics
    parser.AddBoolFlag(
        "fdiagnostics-absolute-paths");  // Print absolute paths in diagnostics

    parser.AddBoolFlag("fno-integrated-cc1")->SetOutput(&compiler_info_flags_);

    // Make it understand /clang: and Xclang.
    parser.AddPrefixFlag("clang:")->SetOutput(&compiler_info_flags_);
    parser.AddFlag("Xclang")->SetOutput(&compiler_info_flags_);

    parser.AddBoolFlag("mincremental-linker-compatible")
        ->SetOutput(&incremental_linker_flags);
    parser.AddBoolFlag("mno-incremental-linker-compatible")
        ->SetOutput(&incremental_linker_flags);
  }
  // TODO: Consider split -fprofile-* flags? Some options take
  // an extra arguement, other do not. Merging such kind of flags do not
  // look good.
  FlagParser::Flag* flag_fprofile = parser.AddPrefixFlag("fprofile-");

  parser.AddNonFlag()->SetOutput(&input_filenames_);

  parser.Parse(expanded_args_);
  unknown_flags_ = parser.unknown_flag_args();

  ClangFlagsHelper clang_flags_helper(expanded_args_);

  if (flag_fdebug_compilation_dir && flag_fdebug_compilation_dir->seen()) {
    // -fdebug-compilation-dir accepts both joined and separate form,
    // we need to omit "=" if the flag is given with joined form.
    fdebug_compilation_dir_ = std::string(
        absl::StripPrefix(flag_fdebug_compilation_dir->GetLastValue(), "="));
  } else if (clang_flags_helper.fdebug_compilation_dir()) {
    fdebug_compilation_dir_ = *clang_flags_helper.fdebug_compilation_dir();
  }
  if (flag_fcoverage_compilation_dir &&
      flag_fcoverage_compilation_dir->seen()) {
    fcoverage_compilation_dir_ = flag_fcoverage_compilation_dir->GetLastValue();
  } else if (clang_flags_helper.fcoverage_compilation_dir()) {
    fcoverage_compilation_dir_ =
        *clang_flags_helper.fcoverage_compilation_dir();
  }
  if (flag_ffile_compilation_dir && flag_ffile_compilation_dir->seen()) {
    ffile_compilation_dir_ = flag_ffile_compilation_dir->GetLastValue();
  }

  is_successful_ = true;

  lang_ = "c++";
  // CL.exe default to C++ unless /Tc /TC specified,
  // or the file is named .c and /Tp /TP are not specified.
  if (flag_Tc->seen() || flag_TC->seen() ||
      ((!input_filenames_.empty() &&
        GetExtension(input_filenames_[0]) == "c") &&
       !flag_TP->seen() && !flag_Tp->seen())) {
    is_cplusplus_ = false;
    lang_ = "c";
  }

  // Handle implicit macros, lang_ must not change after this.
  // See http://msdn.microsoft.com/en-us/library/b0084kay(v=vs.90).aspx
  if (lang_ == "c++") {
    implicit_macros_.append("#define __cplusplus\n");
  }
  if (flag_ZI->seen()) {
    implicit_macros_.append("#define _VC_NODEFAULTLIB\n");
  }
  if (flag_RTC->seen()) {
    implicit_macros_.append("#define __MSVC_RUNTIME_CHECKS\n");
  }
  if (flag_Zc_wchar_t->seen()) {
    implicit_macros_.append("#define _NATIVE_WCHAR_T_DEFINED\n");
    implicit_macros_.append("#define _WCHAR_T_DEFINED\n");
  }

  if (flag_std->seen()) {
    // https://docs.microsoft.com/en-us/cpp/preprocessor/predefined-macros?view=vs-2019
    // as of Aug 2019, wintoolchain checks _MSVC_LANG > 201402L only.
    std::string stdvalue = flag_std->GetLastValue();
    if (stdvalue == "c++14") {
      implicit_macros_.append("#define _MSVC_LANG 201402L\n");
    } else if (stdvalue == "c++17") {
      implicit_macros_.append("#define _MSVC_LANG 201703L\n");
    }
    // https://docs.microsoft.com/en-us/cpp/build/reference/std-specify-language-standard-version?view=vs-2019
    // TODO: stdvalue == "c++latest", "c++20" ?
  } else if (lang_ == "c++") {
    // default /std:c++14 is specified.
    implicit_macros_.append("#define _MSVC_LANG 201402L\n");
  }

  // Debug information format.
  // http://msdn.microsoft.com/en-us/library/958x11bc.aspx
  // For VC, /Zi and /ZI generated PDB.
  // For clang-cl, /Zi is alias to /Z7. /ZI is not supported.
  // Probably OK to deal them as the same?
  // See https://msdn.microsoft.com/en-us/library/958x11bc.aspx,
  // and http://clang.llvm.org/docs/UsersManual.html
  if (compiler_name() != "clang-cl" && (flag_Zi->seen() || flag_ZI->seen())) {
    require_mspdbserv_ = true;
  }

  if (flag_resource_dir && flag_resource_dir->seen()) {
    resource_dir_ = flag_resource_dir->GetLastValue();
  }

  // clang always checks existence of -fsanitize-ignorelist.
  // We should always upload files or compile fails.
  // https://github.com/llvm/llvm-project/blob/d7ec48d71bd67118e7996c45e9c7fb1b09d4f59a/clang/lib/Driver/SanitizerArgs.cpp#L178
  if (flag_fsanitize_blacklist && flag_fsanitize_blacklist->seen()) {
    const std::vector<std::string>& values = flag_fsanitize_blacklist->values();
    std::copy(values.begin(), values.end(),
              back_inserter(optional_input_filenames_));
  }

  // clang always checks existence of -fsanitize-ignorelist.
  // We should always upload files or compile fails.
  // https://github.com/llvm/llvm-project/blob/d7ec48d71bd67118e7996c45e9c7fb1b09d4f59a/clang/lib/Driver/SanitizerArgs.cpp#L178
  if (flag_fsanitize_ignorelist && flag_fsanitize_ignorelist->seen()) {
    const std::vector<std::string>& values =
        flag_fsanitize_ignorelist->values();
    std::copy(values.begin(), values.end(),
              back_inserter(optional_input_filenames_));
  }

  if (flag_fprofile_list && flag_fprofile_list->seen()) {
    const std::vector<std::string>& values = flag_fprofile_list->values();
    std::copy(values.begin(), values.end(),
              back_inserter(optional_input_filenames_));
  }

  if (flag_fthinlto_index->seen()) {
    optional_input_filenames_.push_back(flag_fthinlto_index->GetLastValue());
    thinlto_index_ = flag_fthinlto_index->GetLastValue();
  }

  if (flag_X->seen()) {
    ignore_stdinc_ = true;
    compiler_info_flags_.push_back("/X");
  }

  if (flag_EP->seen() || flag_E->seen()) {
    return;  // output to stdout
  }

  if (flag_Yc->seen()) {
    creating_pch_ = flag_Yc->GetLastValue();
  }
  if (flag_Yu->seen()) {
    using_pch_ = flag_Yu->GetLastValue();
  }
  if (flag_Fp->seen()) {
    using_pch_filename_ = flag_Fp->GetLastValue();
  }

  if (!incremental_linker_flags.empty()) {
    const std::string& last = incremental_linker_flags.back();
    if (last == "-mno-incremental-linker-compatible" || last == "/Brepro" ||
        last == "-Brepro") {
      has_Brepro_ = true;
    }
  }

  if (has_ftime_trace_) {
    compiler_info_flags_.push_back("-ftime-trace");
  }
  // Note: since -fcoverage-mapping does not change predefined macro or
  // system include paths, we do not add it to compiler_info_flags_.

  std::string new_extension = ".obj";
  std::string force_output;
  if (flag_Fo->seen())
    force_output = flag_Fo->GetLastValue();

  if (flag_P->seen()) {
    new_extension = ".i";
    // any option to control output filename?
    force_output = "";
  } else if (!flag_c->seen()) {
    new_extension = ".exe";
    if (flag_Fe->seen()) {
      force_output = flag_Fe->GetLastValue();
    } else {
      force_output = "";
    }
  }

  // copy from gcc_flags.cc
  // TODO: share clang and clang-cl flag parsing?
  absl::string_view profile_input_dir = ".";
  for (absl::string_view value : flag_fprofile->values()) {
    if (absl::StartsWith(value, "instr-use=")) {
      continue;
    }
    if (absl::StartsWith(value, "sample-use=")) {
      continue;
    }

    compiler_info_flags_.emplace_back(absl::StrCat("-fprofile-", value));
    // Pick the last profile dir.
    if (absl::ConsumePrefix(&value, "dir=") ||
        absl::ConsumePrefix(&value, "generate=")) {
      profile_input_dir = value;
    }
  }
  for (absl::string_view value : flag_fprofile->values()) {
    if (absl::ConsumePrefix(&value, "use=")) {
      // https://clang.llvm.org/docs/ClangCommandLineReference.html#cmdoption-clang1-fprofile-use
      if (IsClangClCommand(compiler_name_) &&
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
    } else if (absl::ConsumePrefix(&value, "instr-use=") ||
               absl::ConsumePrefix(&value, "sample-use=")) {
      optional_input_filenames_.push_back(
          file::JoinPathRespectAbsolute(profile_input_dir, value));
    }
  }

  // Single file with designated destination
  if (input_filenames_.size() == 1) {
    if (force_output.empty() && flag_o->seen()) {
      force_output = flag_o->GetLastValue();
    }

    if (!force_output.empty()) {
      std::string of = ComposeOutputFilePath(input_filenames_[0], force_output,
                                             new_extension);
      output_files_.push_back(of);

      if (has_ftime_trace_) {
        size_t ext_start = of.rfind('.');
        if (ext_start != std::string::npos) {
          output_files_.push_back(of.substr(0, ext_start) + ".json");
        } else {
          output_files_.push_back(of + ".json");
        }
      }
    }
    if (!output_files_.empty()) {
      return;
    }
  }

  for (const auto& input_filename : input_filenames_) {
    std::string of =
        ComposeOutputFilePath(input_filename, force_output, new_extension);
    output_files_.push_back(of);

    if (has_ftime_trace_) {
      size_t ext_start = of.rfind('.');
      if (ext_start != std::string::npos) {
        output_files_.push_back(of.substr(0, ext_start) + ".json");
      } else {
        output_files_.push_back(of + ".json");
      }
    }
  }
}

bool VCFlags::IsClientImportantEnv(const char* env) const {
  if (IsServerImportantEnv(env)) {
    return true;
  }

  // We don't override these variables in goma server.
  // So, these are client important, but don't send to server.
  static const char* kCheckEnvs[] = {
      "PATHEXT=", "SystemDrive=", "SystemRoot=",
  };

  for (const char* check_env : kCheckEnvs) {
    if (absl::StartsWithIgnoreCase(env, check_env)) {
      return true;
    }
  }

  return false;
}

bool VCFlags::IsServerImportantEnv(const char* env) const {
  static const char* kCheckEnvs[] = {
      "INCLUDE=",      "LIB=",          "MSC_CMD_FLAGS=",
      "VCINSTALLDIR=", "VSINSTALLDIR=", "WindowsSdkDir=",
  };

  for (const char* check_env : kCheckEnvs) {
    if (absl::StartsWithIgnoreCase(env, check_env)) {
      return true;
    }
  }

  return false;
}

// static
void VCFlags::DefineFlags(FlagParser* parser) {
  FlagParser::Options* opts = parser->mutable_options();
  // define all known flags of cl.exe here.
  // undefined flag here would be treated as non flag arg
  // if the arg begins with alt_flag_prefix.
  // b/18063824
  // https://code.google.com/p/chromium/issues/detail?id=427942
  opts->flag_prefix = '-';
  opts->alt_flag_prefix = '/';
  opts->allows_nonspace_arg = true;

  // http://msdn.microsoft.com//library/fwkeyyhe.aspx
  // note: some bool flag may take - as suffix even if it is documented
  // on the above URL? clang-cl defines such flag.
  parser->AddBoolFlag("?");     // alias of help
  parser->AddPrefixFlag("AI");  // specifies a directory to search for #using
  parser->AddPrefixFlag("analyze");  // enable code analysis
  parser->AddPrefixFlag("arch");     // specifies the architecture for code gen
  parser->AddBoolFlag("await");      // enable resumable functions extension

  parser->AddBoolFlag("bigobj");  // increases the num of addressable sections

  parser->AddBoolFlag("C");  // preserves comments during preprocessing
  parser->AddBoolFlag("c");  // compile only
  parser->AddPrefixFlag("cgthreads");  // specify num of cl.exe threads
  parser->AddPrefixFlag("clr");
  parser->AddPrefixFlag("constexpr");  // constexpr options

  parser->AddFlag("D");          // define macro
  parser->AddPrefixFlag("doc");  // process documentation comments
  // /diagnostics:<args,...> controls the format of diagnostic messages
  parser->AddPrefixFlag("diagnostics:");

  parser->AddBoolFlag("E");     // preprocess to stdout
  parser->AddPrefixFlag("EH");  // exception ahdling model
  parser->AddBoolFlag("EP");    // disable linemarker output and preprocess
  parser->AddPrefixFlag("errorReport");

  parser->AddFlag("F");            // set stack size
  parser->AddPrefixFlag("favor");  // optimize for architecture specifics
  parser->AddPrefixFlag("FA");     // output assembly code file
  parser->AddPrefixFlag("Fa");     // output assembly code to this file
  parser->AddBoolFlag("FC");    // full path of source code in diagnostic text
  parser->AddPrefixFlag("Fd");  // set pdb file name
  parser->AddPrefixFlag("Fe");  // set output executable file or directory
  parser->AddFlag("FI");        // include file before parsing
  parser->AddPrefixFlag("Fi");  // set preprocess output file name
  parser->AddPrefixFlag("Fm");  // set map file name
  parser->AddPrefixFlag("Fo");  // set output object file or directory
  parser->AddPrefixFlag("fp");  // specify floating proint behavior
  parser->AddPrefixFlag("Fp");  // set pch file name
  parser->AddPrefixFlag("FR");  // .sbr file
  parser->AddPrefixFlag("Fr");  // .sbr file without info on local var
  parser->AddBoolFlag("FS");    // force synchronous PDB writes
  parser->AddFlag("FU");        // #using
  parser->AddBoolFlag("Fx");    // merges injected code

  parser->AddBoolFlag("GA");   // optimize for win app
  parser->AddBoolFlag("Gd");   // calling convention
  parser->AddBoolFlag("Ge");   // enable stack probes
  parser->AddBoolFlag("GF");   // enable string pool
  parser->AddBoolFlag("GF-");  // disable string pooling
  parser->AddBoolFlag("GH");   // call hook function _pexit
  parser->AddBoolFlag("Gh");   // call hook function _penter
  parser->AddBoolFlag("GL");   // enables whole program optimization
  parser->AddBoolFlag("GL-");
  parser->AddBoolFlag("Gm");  // enables minimal rebuild
  parser->AddBoolFlag("Gm-");
  parser->AddBoolFlag("GR");   // enable emission of RTTI data
  parser->AddBoolFlag("GR-");  // disable emission of RTTI data
  parser->AddBoolFlag("Gr");   // calling convention
  parser->AddBoolFlag("GS");   // buffer security check
  parser->AddBoolFlag("GS-");
  parser->AddPrefixFlag("Gs");       // controls stack probes
  parser->AddBoolFlag("GT");         // fibre safety thread-local storage
  parser->AddPrefixFlag("guard:");   // control flow guard
  parser->AddBoolFlag("Gv");         // calling convention
  parser->AddBoolFlag("Gw");         // put each data item in its own section
  parser->AddBoolFlag("Gw-");  // don't put each data item in its own section
  parser->AddBoolFlag("GX");   // enable exception handling
  parser->AddBoolFlag("Gy");   // put each function in its own section
  parser->AddBoolFlag("Gy-");  // don't put each function in its own section
  parser->AddBoolFlag("GZ");   // same as /RTC
  parser->AddBoolFlag("Gz");   // calling convention

  parser->AddPrefixFlag("H");         // restricts the length of external names
  parser->AddBoolFlag("HELP");        // alias of help
  parser->AddBoolFlag("help");        // display available options
  parser->AddBoolFlag("homeparams");  // copy register parameters to stack
  parser->AddBoolFlag("hotpatch");    // create hotpatchable image

  parser->AddFlag("I");  // add directory to include search path

  parser->AddBoolFlag("J");  // make char type unsinged

  parser->AddBoolFlag("kernel");  // create kernel mode binary
  parser->AddBoolFlag("kernel-");

  parser->AddBoolFlag("LD");   // create DLL
  parser->AddBoolFlag("LDd");  // create debug DLL
  parser->AddFlag("link");     // forward options to the linker
  parser->AddBoolFlag("LN");

  parser->AddPrefixFlag("MD");  // use DLL run time
  // MD, MDd
  parser->AddPrefixFlag("MP");  // build with multiple process
  parser->AddPrefixFlag("MT");  // use static run time
  // MT, MTd

  parser->AddBoolFlag("nologo");

  parser->AddPrefixFlag("O");  // optimization level
  // O1, O2
  // Ob[012], Od, Oi, Oi-, Os, Ot, Ox, Oy, Oy-
  parser->AddBoolFlag("openmp");

  parser->AddBoolFlag("P");  // preprocess to file
  // set standard-conformance mode (feature set subject to change)
  parser->AddBoolFlag("permissive-");

  parser->AddPrefixFlag("Q");
  // Qfast_transcendentals, QIfirst, Qimprecise_fwaits, Qpar
  // Qsafe_fp_loads, Qrev-report:n

  parser->AddPrefixFlag("RTC");  // run time error check

  parser->AddBoolFlag("sdl");  // additional security check
  parser->AddBoolFlag("sdl-");
  parser->AddBoolFlag("showIncludes");  // print info about included files
  parser->AddPrefixFlag("std:");        // C++ standard version

  parser->AddFlag("Tc");      // specify a C source file
  parser->AddBoolFlag("TC");  // treat all source files as C
  parser->AddFlag("Tp");      // specify a C++ source file
  parser->AddBoolFlag("TP");  // treat all source files as C++

  parser->AddFlag("U");      // undefine macro
  parser->AddBoolFlag("u");  // remove all predefined macros

  parser->AddPrefixFlag("V");   // Sets the version string
  parser->AddPrefixFlag("vd");  // control vtordisp placement
  // for member pointers.
  parser->AddBoolFlag("vmb");  // use a best-case representation method
  parser->AddBoolFlag("vmg");  // use a most-general representation
  // set the default most-general representation
  parser->AddBoolFlag("vmm");  // to multiple inheritance
  parser->AddBoolFlag("vms");  // to single inheritance
  parser->AddBoolFlag("vmv");  // to virtual inheritance
  parser->AddBoolFlag("volatile");

  parser->AddPrefixFlag("W");  // warning
  // W0, W1, W2, W3, W4, Wall, WX, WX-, WL, Wp64
  parser->AddPrefixFlag("w");  // disable warning
  // wd4005, ...

  parser->AddBoolFlag("X");  // ignore standard include paths

  parser->AddBoolFlag("Y-");    // ignore precompiled header
  parser->AddPrefixFlag("Yc");  // create precompiled header
  parser->AddBoolFlag("Yd");    // place debug information
  parser->AddPrefixFlag("Yl");  // inject PCH reference for debug library
  parser->AddPrefixFlag("Yu");  // use precompiled header

  parser->AddBoolFlag("Z7");    // debug information format
  parser->AddBoolFlag("Za");    // disable language extensions
  parser->AddPrefixFlag("Zc");  // conformance
  // line number only debug information; b/30077868
  parser->AddBoolFlag("Zd");
  parser->AddBoolFlag("Ze");          // enable microsoft extensions
  parser->AddBoolFlag("ZH:SHA_256");  // use SHA256 for file checksum
  parser->AddBoolFlag("Zg");          // generate function prototype
  parser->AddBoolFlag("ZI");          // produce pdb
  parser->AddBoolFlag("Zi");          // enable debug information
  parser->AddBoolFlag("Zl");          // omit default library name
  parser->AddPrefixFlag("Zm");        // specify precompiled header memory limit
  parser->AddBoolFlag("Zo");          // enhance optimized debugging
  parser->AddBoolFlag("Zo-");
  parser->AddPrefixFlag("Zp");  // default maximum struct packing alignment
  // Zp1, Zp2, Zp4, Zp8, Zp16
  parser->AddFlag("Zs");        // syntax check only
  parser->AddPrefixFlag("ZW");  // windows runtime compilation

  // New flags from VS2015 Update 2
  parser->AddPrefixFlag("source-charset:");     // set source character set.
  parser->AddPrefixFlag("execution-charset:");  // set execution character set.
  parser->AddBoolFlag("utf-8");             // set both character set to utf-8.
  parser->AddBoolFlag("validate-charset");  //  validate utf-8 files.
  parser->AddBoolFlag("validate-charset-");

  // /d2XXX is undocument flag for debugging.
  // See b/27777598, b/68147091
  parser->AddPrefixFlag("d2");

  // Brepro is undocument flag for reproducible build?
  // https://github.com/llvm/llvm-project/blob/90c78073f73eac58f4f8b4772a896dc8aac023bc/clang/include/clang/Driver/CLCompatOptions.td
  parser->AddBoolFlag("Brepro");
  parser->AddBoolFlag("Brepro-");

  // also see clang-cl
  // https://github.com/llvm/llvm-project/blob/90c78073f73eac58f4f8b4772a896dc8aac023bc/clang/include/clang/Driver/CLCompatOptions.td
  parser->AddFlag("o");  // set output file or directory
  parser->AddBoolFlag("fallback");
  parser->AddBoolFlag("G1");
  parser->AddBoolFlag("G2");
  parser->AddFlag("imsvc");  // both -imsvc, /imsvc.

  // http://b/178986079 support clang-cl /winsysroot
  parser->AddFlag("vctoolsdir");
  parser->AddFlag("vctoolsversion");
  parser->AddFlag("winsdkdir");
  parser->AddFlag("winsdkversion");
  parser->AddFlag("winsysroot");

  // http://b/148244706  /clang:<arg> pass <arg> to the clang driver.
  parser->AddPrefixFlag("clang:");
  // http://b/150403114  /showIncludes:user
  // omits system includes from /showIncludes outputs.
  parser->AddBoolFlag("showIncludes:user");  // print info about included files

  // clang-cl flags. only accepts if it starts with '-'.
  opts->flag_prefix = '-';
  opts->alt_flag_prefix = '\0';
  parser->AddFlag("m");
  parser->AddPrefixFlag("fmsc-version=");  // -fmsc-version=<arg>
  parser->AddPrefixFlag(
      "fms-compatibility-version=");  // -fms-compatibility-version=<arg>
  parser->AddFlag("fsanitize");
  parser->AddBoolFlag("fcolor-diagnostics");  // Use color for diagnostics
  parser->AddBoolFlag(
      "fno-standalone-debug");  // turn on the vtable-based optimization
  parser->AddBoolFlag(
      "fstandalone-debug");  // turn off the vtable-based optimization
  parser->AddBoolFlag("gcolumn-info");       // debug information (-g)
  parser->AddBoolFlag("gline-tables-only");  // debug information (-g)
  parser->AddFlag("Xclang");
  parser->AddFlag("isystem");
  parser->AddPrefixFlag("-analyze");  // enable code analysis (--analyze)
  parser->AddFlag("target");
  parser->AddFlag("-target");
  parser->AddFlag("fdebug-compilation-dir");
  parser->AddBoolFlag("fno-integrated-cc1");
  parser->AddPrefixFlag("fprofile-");

  opts->flag_prefix = '-';
  opts->alt_flag_prefix = '/';
}

// static
bool VCFlags::ExpandArgs(const std::string& cwd,
                         const std::vector<std::string>& args,
                         std::vector<std::string>* expanded_args,
                         std::vector<std::string>* optional_input_filenames) {
  // Expand arguments which start with '@'.
  for (const auto& arg : args) {
    if (absl::StartsWith(arg, "@")) {
      const std::string& source_list_filename =
          PathResolver::PlatformConvert(arg.substr(1));
      std::string source_list;
      if (!ReadFileToString(
              file::JoinPathRespectAbsolute(cwd, source_list_filename),
              &source_list)) {
        LOG(ERROR) << "failed to read: " << source_list_filename;
        return false;
      }
      if (optional_input_filenames) {
        optional_input_filenames->push_back(source_list_filename);
      }

      if (source_list[0] == '\xff' && source_list[1] == '\xfe') {
        // UTF-16LE.
        // do we need to handle FEFF(UTF-16BE) case or others?
        // TODO: handle real wide character.
        // use WideCharToMultiByte on Windows, and iconv on posix?
        VLOG(1) << "Convert WC to MB in @" << source_list_filename;
        std::string source_list_mb;
        // We don't need BOM (the first 2 bytes: 0xFF 0xFE)
        source_list_mb.resize(source_list.size() / 2 - 1);
        for (size_t i = 2; i < source_list.size(); i += 2) {
          source_list_mb[i / 2 - 1] = source_list[i];
          if (source_list[i + 1] != 0) {
            LOG(ERROR) << "failed to convert:" << source_list_filename;
            return false;
          }
        }
        source_list.swap(source_list_mb);
        VLOG(1) << "source_list:" << source_list;
      }
      if (!ParseWinCommandLineToArgv(source_list, expanded_args)) {
        LOG(WARNING) << "failed to parse command line: " << source_list;
        return false;
      }
      VLOG(1) << "expanded_args:" << *expanded_args;
    } else {
      expanded_args->push_back(arg);
    }
  }
  return true;
}

// static
std::string VCFlags::ComposeOutputFilePath(
    const std::string& input_filename,
    const std::string& output_file_or_dir,
    const std::string& output_file_ext) {
  std::string input_file = NormalizeWin32Path(input_filename);
  std::string output_target = NormalizeWin32Path(output_file_or_dir);

  bool output_is_dir = false;
  if (output_target.length() &&
      output_target[output_target.length() - 1] == '\\') {
    output_is_dir = true;
  }
  if (output_target.length() && !output_is_dir) {
    return output_target;
  }

  // We need only the filename part of input file
  size_t begin = input_file.find_last_of("/\\");
  size_t end = input_file.rfind('.');
  begin = (begin == std::string::npos) ? 0 : begin + 1;
  end = (end == std::string::npos) ? input_filename.size() : end;
  std::string new_output;
  if (end > begin) {
    new_output = input_file.substr(begin, end - begin);
    new_output.append(output_file_ext);
    if (output_target.length() && output_is_dir) {
      new_output = output_target + new_output;
    }
  } else {
    new_output = output_target;
  }
  return new_output;
}

}  // namespace devtools_goma
