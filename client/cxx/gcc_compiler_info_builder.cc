// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gcc_compiler_info_builder.h"

#include "absl/strings/match.h"
#include "base/path.h"
#include "client/autolock_timer.h"
#include "client/counterz.h"
#include "client/cxx/clang_compiler_info_builder_helper.h"
#include "client/cxx/nacl_compiler_info_builder_helper.h"
#include "client/env_flags.h"
#include "client/util.h"
#include "glog/logging.h"
#include "glog/stl_logging.h"
#include "lib/gcc_flags.h"
#include "lib/path_resolver.h"

#ifdef __linux__
#include "binutils/elf_dep_parser.h"
#include "binutils/elf_parser.h"
#include "binutils/elf_util.h"
#include "client/cxx/chromeos_compiler_info_builder_helper.h"
#endif

#ifdef _WIN32
#include "posix_helper_win.h"
#endif  // _WIN32

// TODO: remove this when SEND_COMPILER_BINARY_AS_INPUT become
//                    default behavior.
GOMA_DECLARE_bool(SEND_COMPILER_BINARY_AS_INPUT);

namespace devtools_goma {

namespace {

class GetClangPluginPath : public FlagParser::Callback {
 public:
  explicit GetClangPluginPath(std::vector<std::string>* subprograms)
      : load_seen_(false), subprograms_(subprograms) {}
  ~GetClangPluginPath() override {}

  std::string ParseFlagValue(const FlagParser::Flag& flag ALLOW_UNUSED,
                             const std::string& value) override {
    if (load_seen_) {
      load_seen_ = false;
      if (!used_plugin_.insert(value).second) {
        LOG(INFO) << "The same plugin is trying to be added more than twice."
                  << " Let us ignore it to reduce subprogram spec size."
                  << " path=" << value;
      }
      subprograms_->push_back(value);
    }
    if (value == "-load") {
      load_seen_ = true;
    }
    return value;
  }

 private:
  bool load_seen_;
  std::vector<std::string>* subprograms_;
  std::set<std::string> used_plugin_;
};

bool AddSubprogramInfo(
    const std::string& user_specified_path,
    const std::string& abs_path,
    google::protobuf::RepeatedPtrField<CompilerInfoData::SubprogramInfo>* ss) {
  CompilerInfoData::SubprogramInfo* s = ss->Add();
  if (!CxxCompilerInfoBuilder::SubprogramInfoFromPath(user_specified_path,
                                                      abs_path, s)) {
    ss->RemoveLast();
    return false;
  }
  return true;
}

// Execute GCC and get the string output for GCC version
bool GetGccVersion(const std::string& bare_gcc,
                   const std::vector<std::string>& compiler_info_envs,
                   const std::string& cwd,
                   std::string* version) {
  std::vector<std::string> argv;
  argv.push_back(bare_gcc);
  argv.push_back("-dumpversion");
  std::vector<std::string> env(compiler_info_envs);
  env.push_back("LC_ALL=C");
  int32_t status = 0;
  std::string dumpversion_output;
  {
    GOMA_COUNTERZ("ReadCommandOutput(dumpversion)");
    dumpversion_output = ReadCommandOutput(bare_gcc, argv, env, cwd,
                                           MERGE_STDOUT_STDERR, &status);
  }

  if (status != 0) {
    LOG(ERROR) << "ReadCommandOutput exited with non zero status code."
               << " bare_gcc=" << bare_gcc << " status=" << status
               << " argv=" << argv << " env=" << env << " cwd=" << cwd
               << " dumpversion_output=" << dumpversion_output;
    return false;
  }

  argv[1] = "--version";
  std::string version_output;
  {
    GOMA_COUNTERZ("ReadCommandOutput(version)");
    version_output = ReadCommandOutput(bare_gcc, argv, env, cwd,
                                       MERGE_STDOUT_STDERR, &status);
  }
  if (status != 0) {
    LOG(ERROR) << "ReadCommandOutput exited with non zero status code."
               << " bare_gcc=" << bare_gcc << " status=" << status
               << " argv=" << argv << " env=" << env << " cwd=" << cwd
               << " version_output=" << version_output;
    return false;
  }

  if (dumpversion_output.empty() || version_output.empty()) {
    LOG(ERROR) << "dumpversion_output or version_output is empty."
               << " bare_gcc=" << bare_gcc << " status=" << status
               << " argv=" << argv << " env=" << env << " cwd=" << cwd
               << " dumpversion_output=" << dumpversion_output
               << " version_output=" << version_output;
    return false;
  }
  *version = GetCxxCompilerVersionFromCommandOutputs(
      bare_gcc, dumpversion_output, version_output);
  return true;
}

// Execute GCC and get the string output for GCC target architecture
// This target is used to pick the same compiler in the backends, so
// we don't need to use compiler_info_flags here.
bool GetGccTarget(const std::string& bare_gcc,
                  const std::vector<std::string>& compiler_info_envs,
                  const std::string& cwd,
                  std::string* target) {
  std::vector<std::string> argv;
  argv.push_back(bare_gcc);
  argv.push_back("-dumpmachine");
  std::vector<std::string> env(compiler_info_envs);
  env.push_back("LC_ALL=C");
  int32_t status = 0;
  std::string gcc_output;
  {
    GOMA_COUNTERZ("ReadCommandOutput(dumpmachine)");
    gcc_output = ReadCommandOutput(bare_gcc, argv, env, cwd,
                                   MERGE_STDOUT_STDERR, &status);
  }
  if (status != 0) {
    LOG(ERROR) << "ReadCommandOutput exited with non zero status code."
               << " bare_gcc=" << bare_gcc << " status=" << status
               << " argv=" << argv << " env=" << env << " cwd=" << cwd
               << " gcc_output=" << gcc_output;
    return false;
  }
  *target = GetFirstLine(gcc_output);
  return !target->empty();
}

bool IsExecutable(const std::string& cwd, const std::string& path) {
  const std::string abs_path = file::JoinPathRespectAbsolute(cwd, path);
  return access(abs_path.c_str(), X_OK) == 0;
}

#if defined(__linux__) || defined(__MACH__)
std::string GetRealClangPath(const std::string& normal_gcc_path,
                             const std::string& cwd,
                             const std::vector<std::string>& envs) {
  std::vector<std::string> argv;
  argv.push_back(normal_gcc_path);
  argv.push_back("-xc");
  argv.push_back("-v");
  argv.push_back("-E");
  argv.push_back("/dev/null");
  if (GCCFlags::IsClangCommand(normal_gcc_path) &&
      // pnacl-clang returns error for -no-canonical-prefixes.
      !GCCFlags::IsPNaClClangCommand(normal_gcc_path)) {
    // Expect clang to print a relative path if possible.
    argv.push_back("-no-canonical-prefixes");
  }
  int32_t status = 0;
  std::string v_output;
  {
    GOMA_COUNTERZ("ReadCommandOutput(-xc -v)");
    v_output = ReadCommandOutput(normal_gcc_path, argv, envs, cwd,
                                 MERGE_STDOUT_STDERR, &status);
  }
  LOG_IF(ERROR, status != 0)
      << "ReadCommandOutput exited with non zero status code."
      << " normal_gcc_path=" << normal_gcc_path << " status=" << status
      << " argv=" << argv << " envs=" << envs << " cwd=" << cwd
      << " v_output=" << v_output;
  const std::string clang_path =
      ClangCompilerInfoBuilderHelper::ParseRealClangPath(v_output);
  if (!clang_path.empty() && IsExecutable(cwd, clang_path)) {
    return clang_path;
  }
  return std::string();
}
#endif

}  // anonymous namespace

void GCCCompilerInfoBuilder::SetTypeSpecificCompilerInfo(
    const CompilerFlags& flags,
    const std::string& local_compiler_path,
    const std::string& abs_local_compiler_path,
    const std::vector<std::string>& compiler_info_envs,
    CompilerInfoData* data) const {
  // Some compilers uses wrapper script to set build target, and in such a
  // situation, build target could be different.
  // To make goma backend use proper wrapper script, or set proper -target,
  // we should need to use local_compiler_path instead of real path.
  bool has_version = GetGccVersion(abs_local_compiler_path, compiler_info_envs,
                                   flags.cwd(), data->mutable_version());
  bool has_target = GetGccTarget(abs_local_compiler_path, compiler_info_envs,
                                 flags.cwd(), data->mutable_target());


  const GCCFlags& gcc_flags = static_cast<const GCCFlags&>(flags);

  // If input is LLVM IR, we assume it ThinLTO backend phase.
  // The phase should not use system include paths, predefined macro and
  // features.
  //
  // See also:
  // http://blog.llvm.org/2016/06/thinlto-scalable-and-incremental-lto.html
  const bool is_input_ir = gcc_flags.lang() == "ir";

  // TODO: As we have -x flags in compiler_info,
  //               include_processor don't need to have 2 kinds of
  //               system include paths (C and C++).
  //               However, we still need them because backend
  //               should set them using different ways
  //               (-isystem and CPLUS_INCLUDE_PATH).
  //               Once b/5218687 is fixed, we should
  //               be able to eliminate cxx_system_include_paths.
  if (!is_input_ir &&
      !ClangCompilerInfoBuilderHelper::SetBasicCompilerInfo(
          local_compiler_path, gcc_flags.compiler_info_flags(),
          compiler_info_envs, gcc_flags.cwd(), "-x" + flags.lang(),
          gcc_flags.resource_dir(), gcc_flags.is_cplusplus(),
          gcc_flags.has_nostdinc(), data)) {
    DCHECK(data->has_error_message());
    // If error occurred in SetBasicCompilerInfo, we do not need to
    // continue.
    return;
  }

#ifdef _WIN32
  // In the (build: Windows, target: NaCl (not PNaCl)) compile,
  // include paths under toolchain root are shown as relative path from it.
  if (GCCFlags::IsNaClGCCCommand(local_compiler_path)) {
    data->mutable_cxx()->set_toolchain_root(
        NaClCompilerInfoBuilderHelper::GetNaClToolchainRoot(
            local_compiler_path));
  }
#endif

  if (!has_version) {
    AddErrorMessage("Failed to get version for " + data->real_compiler_path(),
                    data);
    LOG(ERROR) << data->error_message();
    return;
  }
  if (!has_target) {
    AddErrorMessage("Failed to get target for " + data->real_compiler_path(),
                    data);
    LOG(ERROR) << data->error_message();
    return;
  }

  if (!GetExtraSubprograms(local_compiler_path, gcc_flags, compiler_info_envs,
                           data)) {
    std::ostringstream ss;
    ss << "Failed to get subprograms for " << data->real_compiler_path();
    AddErrorMessage(ss.str(), data);
    LOG(ERROR) << data->error_message();
    return;
  }

  // Hack for GCC 5's has_include and has_include_next support.
  // GCC has built-in macro that defines __has_include to __has_include__
  // and __has_include_next to __has_include_next__.
  // https://gcc.gnu.org/viewcvs/gcc/trunk/gcc/c-family/c-cppbuiltin.c?revision=229533&view=markup#l794
  // However, __has_include__ and __has_include_next__ are usable but not
  // defined.
  // https://gcc.gnu.org/viewcvs/gcc/trunk/libcpp/init.c?revision=229154&view=markup#l376
  // i.e.
  // if we execute gcc -E to followings, we only get
  // "__has_include__(<stddef.h>)"
  //   #ifdef __has_include__
  //   "__has_include__"
  //   #endif
  //   #ifdef __has_include__(<stddef.h>)
  //   "__has_include__(<stddef.h>)"
  //   #endif
  // See also: b/25581637
  //
  // Note that I do not think we need version check because:
  // 1. __has_include is the new feature and old version does not have it.
  // 2. I can hardly think they change their implementation as far as
  //    I guessed from the code
  if (data->name() == "gcc" || data->name() == "g++") {
    bool has_include = false;
    bool has_include__ = false;
    bool has_include_next = false;
    bool has_include_next__ = false;
    for (const auto& m : data->cxx().supported_predefined_macros()) {
      if (m == "__has_include")
        has_include = true;
      if (m == "__has_include__")
        has_include__ = true;
      if (m == "__has_include_next")
        has_include_next = true;
      if (m == "__has_include_next__")
        has_include_next__ = true;
    }

    if (has_include && !has_include__ &&
        (data->cxx().predefined_macros().find("__has_include__") !=
         std::string::npos)) {
      data->mutable_cxx()->add_hidden_predefined_macros("__has_include__");
    }
    if (has_include_next && !has_include_next__ &&
        (data->cxx().predefined_macros().find("__has_include_next__") !=
         std::string::npos)) {
      data->mutable_cxx()->add_hidden_predefined_macros("__has_include_next__");
    }
  }

  // --- Experimental. Add compiler resource.
  std::vector<std::string> resource_paths_to_collect;
  const std::string abs_real_compiler_path =
      file::JoinPathRespectAbsolute(flags.cwd(), data->real_compiler_path());

  // local compiler.
  // The server assumes the first resource path is always the local compiler.
  // So we have to start from a local compiler.
  resource_paths_to_collect.push_back(local_compiler_path);

  // real compiler if it differs from local compiler.
  // When clang++ is local compiler, the real compiler is usually clang, and
  // clang++ is just a symlink to clang. In that case, we don't need to collect
  // real compiler. We think the files are the same if hash is the same.
  if (local_compiler_path != data->real_compiler_path() &&
      data->local_compiler_hash() != data->hash()) {
    resource_paths_to_collect.push_back(data->real_compiler_path());
  }
  // subprograms.
  for (const auto& subprogram : data->subprograms()) {
    resource_paths_to_collect.push_back(subprogram.user_specified_path());
  }
  // TODO: Currently GCCCompilerInfoBuilder covers all
  // gcc/clang/nacl-clang/pnacl-clang compilers. However, it's better to have
  // a subclass for each type of compiler to support type specific procedures.
  if (GCCFlags::IsPNaClClangCommand(local_compiler_path)) {
    NaClCompilerInfoBuilderHelper::CollectPNaClClangResources(
        local_compiler_path, flags.cwd(), &resource_paths_to_collect);
  }
  if (GCCFlags::IsNaClGCCCommand(local_compiler_path)) {
    NaClCompilerInfoBuilderHelper::CollectNaClGccResources(
        local_compiler_path, flags.cwd(), &resource_paths_to_collect);
  }
  if (GCCFlags::IsNaClClangCommand(local_compiler_path)) {
    NaClCompilerInfoBuilderHelper::CollectNaClClangResources(
        local_compiler_path, flags.cwd(), &resource_paths_to_collect);
  }

#ifdef __linux__
  if (ChromeOSCompilerInfoBuilderHelper::IsSimpleChromeClangCommand(
          local_compiler_path, data->real_compiler_path())) {
    if (!ChromeOSCompilerInfoBuilderHelper::CollectSimpleChromeClangResources(
            flags.cwd(), data->real_compiler_path(),
            &resource_paths_to_collect)) {
      // HACK: we should not affect people not using ATS.
      if (FLAGS_SEND_COMPILER_BINARY_AS_INPUT) {
        AddErrorMessage("failed to add simple chrome resources", data);
      }
      LOG(ERROR)
          << "failed to add simple chrome resources: local_compiler_path="
          << local_compiler_path
          << " real_compiler_path=" << data->real_compiler_path();
      return;
    }
  } else if (ChromeOSCompilerInfoBuilderHelper::IsClangInChrootEnv(
                 abs_local_compiler_path)) {
    if (!ChromeOSCompilerInfoBuilderHelper::CollectChrootClangResources(
            flags.cwd(), compiler_info_envs, local_compiler_path,
            data->real_compiler_path(), &resource_paths_to_collect)) {
      // HACK: we should not affect people not using ATS.
      if (FLAGS_SEND_COMPILER_BINARY_AS_INPUT) {
        AddErrorMessage("failed to add chromeos chroot env clang resources",
                        data);
      }
      LOG(ERROR) << "failed to add chromeos chroot env clang resources: "
                    "local_compiler_path="
                 << local_compiler_path
                 << " real_compiler_path=" << data->real_compiler_path();
      return;
    }

    ChromeOSCompilerInfoBuilderHelper::SetAdditionalFlags(
        abs_local_compiler_path, data->mutable_additional_flags());
    data->add_dimensions("os:linux-hermetic");
  } else if (ElfParser::IsElf(abs_real_compiler_path)) {
    constexpr absl::string_view kLdSoConfPath = "/etc/ld.so.conf";
    std::vector<std::string> searchpath = LoadLdSoConf(kLdSoConfPath);
    ElfDepParser edp(flags.cwd(), searchpath, false);
    absl::flat_hash_set<std::string> exec_deps;
    if (!edp.GetDeps(data->real_compiler_path(), &exec_deps)) {
      LOG(ERROR) << "failed to get library dependencies for executable."
                 << " cwd=" << flags.cwd()
                 << " real_compiler_path=" << data->real_compiler_path();
      // HACK: we should not affect people not using ATS.
      if (FLAGS_SEND_COMPILER_BINARY_AS_INPUT) {
        AddErrorMessage("failed to add compiler resources", data);
      }
      return;
    }
    for (const auto& path : exec_deps) {
      if (IsInSystemLibraryPath(path, searchpath)) {
        continue;
      }
      resource_paths_to_collect.push_back(path);
    }
  } else {
    LOG(INFO) << "The compiler is neither ChromeOS clang nor an ELF binary:"
              << " local_compiler_path=" << local_compiler_path
              << " abs_real_compiler_path=" << abs_real_compiler_path;
  }
#endif  // __linux__

  absl::flat_hash_set<std::string> visited_paths;
  for (const auto& resource_path : resource_paths_to_collect) {
    if (!AddResourceAsExecutableBinary(resource_path, gcc_flags.cwd(),
                                       &visited_paths, data)) {
      return;
    }
  }
}

void GCCCompilerInfoBuilder::SetCompilerPath(
    const CompilerFlags& flags,
    const std::string& local_compiler_path,
    const std::vector<std::string>& compiler_info_envs,
    CompilerInfoData* data) const {
  data->set_local_compiler_path(local_compiler_path);
  data->set_real_compiler_path(GetRealCompilerPath(
      local_compiler_path, flags.cwd(), compiler_info_envs));
}

std::string GCCCompilerInfoBuilder::GetCompilerName(
    const CompilerInfoData& data) const {
  absl::string_view base = file::Basename(data.local_compiler_path());
  if (base != "cc" && base != "c++") {
    // We can simply use local_compiler_path for judging compiler name
    // if basename is not "cc" or "c++".
    // See also b/13107706
    return GCCFlags::GetCompilerName(data.local_compiler_path());
  }

  if (!GCCFlags::IsClangCommand(data.real_compiler_path())) {
    return GCCFlags::GetCompilerName(data.real_compiler_path());
  }

  // clang++ is usually symlink to clang, and real compiler path is
  // usually be clang.  It does not usually reflect what we expect as a
  // compiler name.
  std::string real_name = GCCFlags::GetCompilerName(data.real_compiler_path());
  if (base == "cc") {
    return real_name;
  }
  if (real_name == "clang") {
    return std::string("clang++");
  }
  LOG(WARNING) << "Cannot detect compiler name:"
               << " local=" << data.local_compiler_path()
               << " real=" << data.real_compiler_path();
  return std::string();
}

/* static */
bool GCCCompilerInfoBuilder::GetExtraSubprograms(
    const std::string& normal_gcc_path,
    const GCCFlags& gcc_flags,
    const std::vector<std::string>& compiler_info_envs,
    CompilerInfoData* compiler_info) {
  // TODO: support linker subprograms on linking.
  std::vector<std::string> clang_plugins;
  std::vector<std::string> B_options;
  bool no_integrated_as = false;
  std::set<std::string> known_subprograms;
  ParseSubprogramFlags(normal_gcc_path, gcc_flags, &clang_plugins, &B_options,
                       &no_integrated_as);
  for (const auto& path : clang_plugins) {
    std::string absolute_path =
        file::JoinPathRespectAbsolute(gcc_flags.cwd(), path);
    if (!known_subprograms.insert(absolute_path).second) {
      LOG(INFO) << "ignored duplicated subprogram: " << absolute_path;
      continue;
    }
    if (!AddSubprogramInfo(path, absolute_path,
                           compiler_info->mutable_subprograms())) {
      LOG(ERROR) << "invalid plugin:"
                 << " absolute_path=" << absolute_path
                 << " normal_gcc_path=" << normal_gcc_path
                 << " compiler_info_flags=" << gcc_flags.compiler_info_flags();
      return false;
    }
  }

  std::vector<std::string> subprogram_paths;
  if (!CxxCompilerInfoBuilder::GetSubprograms(
          normal_gcc_path, gcc_flags.lang(), gcc_flags.compiler_info_flags(),
          compiler_info_envs, gcc_flags.cwd(), no_integrated_as,
          &subprogram_paths)) {
    LOG(ERROR) << "failed to get subprograms.";
    return false;
  }
  if (no_integrated_as && !HasAsPath(subprogram_paths)) {
    LOG(ERROR) << "no_integrated_as is set but we cannot find as.";
    return false;
  }
  for (const auto& path : subprogram_paths) {
    bool may_register = false;
    if (no_integrated_as && absl::EndsWith(path, "as")) {
      may_register = true;
    } else {
      // List only subprograms under -B path for backward compatibility.
      // See b/63082235
      for (const std::string& b : B_options) {
        if (absl::StartsWith(path, b)) {
          may_register = true;
          break;
        }
      }
    }
    if (!may_register) {
      LOG(INFO) << "showed up as subprogram but not sent for"
                << " backword compatibility."
                << " path=" << path << " normal_gcc_path=" << normal_gcc_path
                << " compiler_info_flags=" << gcc_flags.compiler_info_flags();
      continue;
    }

    std::string absolute_path =
        file::JoinPathRespectAbsolute(gcc_flags.cwd(), path);
    if (!known_subprograms.insert(absolute_path).second) {
      LOG(INFO) << "ignored duplicated subprogram: " << absolute_path;
      continue;
    }
    if (!AddSubprogramInfo(path, absolute_path,
                           compiler_info->mutable_subprograms())) {
      LOG(ERROR) << "invalid subprogram:"
                 << " absolute_path=" << absolute_path
                 << " normal_gcc_path=" << normal_gcc_path
                 << " compiler_info_flags=" << gcc_flags.compiler_info_flags();
      return false;
    }
  }
  return true;
}

/* static */
void GCCCompilerInfoBuilder::ParseSubprogramFlags(
    const std::string& normal_gcc_path,
    const GCCFlags& gcc_flags,
    std::vector<std::string>* clang_plugins,
    std::vector<std::string>* B_options,
    bool* no_integrated_as) {
  const std::vector<std::string>& compiler_info_flags =
      gcc_flags.compiler_info_flags();
  FlagParser flag_parser;
  GCCFlags::DefineFlags(&flag_parser);

  // Clang plugin support.
  GetClangPluginPath get_clang_plugin_path(clang_plugins);
  flag_parser.AddFlag("Xclang")->SetCallbackForParsedArgs(
      &get_clang_plugin_path);

  // Support no-integrated-as.
  flag_parser.AddBoolFlag("no-integrated-as")->SetSeenOutput(no_integrated_as);
  flag_parser.AddBoolFlag("fno-integrated-as")->SetSeenOutput(no_integrated_as);

  // Parse -B options.
  FlagParser::Flag* flag_B = flag_parser.AddBoolFlag("B");

  std::vector<std::string> argv;
  argv.push_back(normal_gcc_path);
  copy(compiler_info_flags.begin(), compiler_info_flags.end(),
       back_inserter(argv));
  flag_parser.Parse(argv);

  std::copy(flag_B->values().cbegin(), flag_B->values().cend(),
            std::back_inserter(*B_options));
}

// static
bool GCCCompilerInfoBuilder::HasAsPath(
    const std::vector<std::string>& subprogram_paths) {
  for (const auto& path : subprogram_paths) {
    absl::string_view basename = file::Basename(path);
    if (basename == "as" || absl::EndsWith(basename, "-as")) {
      return true;
    }
  }
  return false;
}

// static
std::string GCCCompilerInfoBuilder::GetRealCompilerPath(
    const std::string& normal_gcc_path,
    const std::string& cwd,
    const std::vector<std::string>& envs) {
#if !defined(__linux__) && !defined(__MACH__) && !defined(_WIN32)
  return normal_gcc_path;
#endif

#if defined(__linux__) || defined(__MACH__)
  // For whom using a wrapper script for clang.
  // E.g. ChromeOS clang and Android.
  //
  // Since clang invokes itself as cc1, we can find its real name by capturing
  // what is cc1.  Exception is that it is invoked via a shell script that
  // invokes loader, which might be only done by ChromeOS clang.
  //
  // For pnacl-clang, although we still use binary_hash of local_compiler for
  // command_spec in request, we also need real compiler to check toolchain
  // update for compiler_info_cache.
  if (GCCFlags::IsClangCommand(normal_gcc_path)) {
    const std::string real_path = GetRealClangPath(normal_gcc_path, cwd, envs);
    if (real_path.empty()) {
      LOG(WARNING) << "seems not be a clang?"
                   << " normal_gcc_path=" << normal_gcc_path;
      return normal_gcc_path;
    }
#ifndef __linux__
    return real_path;
#else
    // Ubuntu Linux is required to build ChromeOS.
    // We do not need to consider ChromeOS clang for Mac.
    // http://www.chromium.org/chromium-os/quick-start-guide
    //
    // Consider the clang is ChromeOS clang, which runs via a wrapper.
    // TODO: handle empty return value as error.
    return ChromeOSCompilerInfoBuilderHelper::GetRealClangPath(cwd, real_path);
#endif
  }
#endif

#ifdef __linux__
  // For ChromeOS compilers.
  // Note: Ubuntu Linux is required to build ChromeOS.
  // http://www.chromium.org/chromium-os/quick-start-guide
  std::vector<std::string> argv;
  argv.push_back(normal_gcc_path);
  argv.push_back("-v");
  int32_t status = 0;
  std::string v_output;
  {
    GOMA_COUNTERZ("ReadCommandOutput(-v)");
    v_output = ReadCommandOutput(normal_gcc_path, argv, envs, cwd,
                                 MERGE_STDOUT_STDERR, &status);
  }
  LOG_IF(ERROR, status != 0)
      << "ReadCommandOutput exited with non zero status code."
      << " normal_gcc_path=" << normal_gcc_path << " status=" << status
      << " argv=" << argv << " envs=" << envs << " cwd=" << cwd
      << " v_output=" << v_output;
  const char* kCollectGcc = "COLLECT_GCC=";
  size_t index = v_output.find(kCollectGcc);
  if (index == std::string::npos)
    return normal_gcc_path;
  index += strlen(kCollectGcc);

  // If COLLECT_GCC is specified and gcc is accompanied by gcc.real,
  // we assume the "real" one is the last binary we will run.
  // TODO: More reliable ways?
  const std::string& gcc_path =
      v_output.substr(index, v_output.find_first_of("\r\n", index) - index);
  const std::string& real_gcc_path = gcc_path + ".real";
  if (IsExecutable(cwd, real_gcc_path)) {
    return real_gcc_path;
  }
  return gcc_path;
#endif

#ifdef __MACH__
  if (file::Dirname(normal_gcc_path) != "/usr/bin") {
    return normal_gcc_path;
  }
  const std::string clang_path = GetRealClangPath(normal_gcc_path, cwd, envs);
  if (!clang_path.empty()) {
    return clang_path;
  }
  LOG(INFO) << "The command seems not clang. Use it as-is: " << normal_gcc_path;
  return normal_gcc_path;
#endif
#ifdef _WIN32
  // For Windows nacl-{gcc,g++}.
  // The real binary is ../libexec/nacl-{gcc,g++}.exe.  Binaries under
  // the bin directory are just wrappers to them.
  if (GCCFlags::IsNaClGCCCommand(normal_gcc_path)) {
    const std::string& candidate_path = file::JoinPath(
        NaClCompilerInfoBuilderHelper::GetNaClToolchainRoot(normal_gcc_path),
        file::JoinPath("libexec", file::Basename(normal_gcc_path)));
    if (IsExecutable(cwd, candidate_path)) {
      return candidate_path;
    }
    LOG(ERROR) << "cannot find nacl-gcc's real compiler path."
               << " normal_gcc_path=" << normal_gcc_path
               << " cwd=" << cwd
               << " candidate_path=" << candidate_path;
  }
  return normal_gcc_path;
#endif
}

}  // namespace devtools_goma
