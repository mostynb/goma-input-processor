// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vc_compiler_info_builder.h"

#include "clang_compiler_info_builder_helper.h"
#include "client/env_flags.h"
#include "cmdline_parser.h"
#include "counterz.h"
#include "glog/logging.h"
#include "glog/stl_logging.h"
#include "mypath.h"
#include "path.h"
#include "util.h"
#include "vc_flags.h"

#ifdef __linux__
#include "binutils/elf_dep_parser.h"
#include "binutils/elf_parser.h"
#include "binutils/elf_util.h"
#endif

// TODO: remove this when SEND_COMPILER_BINARY_AS_INPUT become
//                    default behavior.
GOMA_DECLARE_bool(SEND_COMPILER_BINARY_AS_INPUT);

namespace devtools_goma {

namespace {

std::string GetVCOutputString(
    const std::string& cl_exe_path,
    const std::string& vcflags,
    const std::string& dumb_file,
    const std::vector<std::string>& compiler_info_flags,
    const std::vector<std::string>& compiler_info_envs,
    const std::string& cwd) {
  // The trick we do here gives both include path and predefined macros.
  std::vector<std::string> argv;
  argv.push_back(cl_exe_path);
  argv.push_back("/nologo");
  argv.push_back(vcflags);
  copy(compiler_info_flags.begin(), compiler_info_flags.end(),
       back_inserter(argv));
  argv.push_back(dumb_file);
  int32_t dummy;  // It is fine to return non zero status code.

  {
    GOMA_COUNTERZ("ReadCommandOutput(/nologo)");
    return ReadCommandOutput(cl_exe_path, argv, compiler_info_envs, cwd,
                             MERGE_STDOUT_STDERR, &dummy);
  }
}

// Since clang-cl is emulation of cl.exe, it might not have meaningful
// clang-cl -dumpversion.  It leads inconsistency of goma's compiler version
// format between clang and clang-cl.  Former expect <dumpversion>[<version>]
// latter cannot have <dumpversion>.
// As a result, let me use different way of getting version string.
// TODO: make this support gcc and use this instead of
//                    GetGccTarget.
std::string GetClangClSharpOutput(
    const std::string& clang_path,
    const std::vector<std::string>& compiler_info_flags,
    const std::vector<std::string>& compiler_info_envs,
    const std::string& cwd) {
  std::vector<std::string> argv;
  argv.push_back(clang_path);
  argv.push_back("-Qunused-arguments");
  argv.push_back("-Wno-unknown-argument");
  copy(compiler_info_flags.begin(), compiler_info_flags.end(),
       back_inserter(argv));
  argv.push_back("-###");
  int32_t status = 0;
  std::string output;
  {
    GOMA_COUNTERZ("ReadCommandOutput(###)");
    output = ReadCommandOutput(clang_path, argv, compiler_info_envs, cwd,
                               MERGE_STDOUT_STDERR, &status);
  }
  if (status != 0) {
    LOG(ERROR) << "ReadCommandOutput exited with non zero status code."
               << " clang_path=" << clang_path << " status=" << status
               << " argv=" << argv
               << " compiler_info_envs=" << compiler_info_envs << " cwd=" << cwd
               << " output=" << output;
    return "";
  }
  return output;
}

}  // anonymous namespace

void VCCompilerInfoBuilder::SetTypeSpecificCompilerInfo(
    const CompilerFlags& flags,
    const std::string& local_compiler_path,
    const std::string& abs_local_compiler_path,
    const std::vector<std::string>& compiler_info_envs,
    CompilerInfoData* data) const {
  const VCFlags& vc_flags = static_cast<const VCFlags&>(flags);
  if (VCFlags::IsClangClCommand(local_compiler_path)) {
    SetClangClSpecificCompilerInfo(vc_flags, local_compiler_path,
                                   abs_local_compiler_path, compiler_info_envs,
                                   data);
  } else {
    SetClexeSpecificCompilerInfo(vc_flags, local_compiler_path,
                                 abs_local_compiler_path, compiler_info_envs,
                                 data);
  }
}

void VCCompilerInfoBuilder::SetClexeSpecificCompilerInfo(
    const VCFlags& vc_flags,
    const std::string& local_compiler_path,
    const std::string& abs_local_compiler_path,
    const std::vector<std::string>& compiler_info_envs,
    CompilerInfoData* data) const {
  std::string vcflags_path = GetMyDirectory();
  vcflags_path += "\\vcflags.exe";
  data->mutable_cxx()->set_predefined_macros(data->cxx().predefined_macros() +
                                             vc_flags.implicit_macros());
  if (!VCCompilerInfoBuilder::GetVCVersion(
          abs_local_compiler_path, compiler_info_envs, vc_flags.cwd(),
          data->mutable_version(), data->mutable_target())) {
    AddErrorMessage(
        "Failed to get cl.exe version for " + abs_local_compiler_path, data);
    LOG(ERROR) << data->error_message();
    return;
  }
  if (!GetVCDefaultValues(abs_local_compiler_path, vcflags_path,
                          vc_flags.compiler_info_flags(), compiler_info_envs,
                          vc_flags.cwd(), data->lang(), data)) {
    AddErrorMessage(
        "Failed to get cl.exe system include path "
        " or predifined macros for " +
            abs_local_compiler_path,
        data);
    LOG(ERROR) << data->error_message();
    return;
  }

  // TODO: collect executable resources?
}

void VCCompilerInfoBuilder::SetClangClSpecificCompilerInfo(
    const VCFlags& vc_flags,
    const std::string& local_compiler_path,
    const std::string& abs_local_compiler_path,
    const std::vector<std::string>& compiler_info_envs,
    CompilerInfoData* data) const {
  const std::string& lang_flag = vc_flags.is_cplusplus() ? "/TP" : "/TC";
  if (!ClangCompilerInfoBuilderHelper::SetBasicCompilerInfo(
          local_compiler_path, vc_flags.compiler_info_flags(),
          compiler_info_envs, vc_flags.cwd(), lang_flag,
          vc_flags.resource_dir(), vc_flags.is_cplusplus(), false, data)) {
    DCHECK(data->has_error_message());
    // If error occurred in SetBasicCompilerInfo, we do not need to
    // continue.
    return;
  }

  const std::string& sharp_output =
      GetClangClSharpOutput(local_compiler_path, vc_flags.compiler_info_flags(),
                            compiler_info_envs, vc_flags.cwd());
  if (sharp_output.empty() ||
      !ClangCompilerInfoBuilderHelper::ParseClangVersionTarget(
          sharp_output, data->mutable_version(), data->mutable_target())) {
    AddErrorMessage(
        "Failed to get version string for " + abs_local_compiler_path, data);
    LOG(ERROR) << data->error_message();
    return;
  }

  // --- Experimental. Add compiler resource.
  {
    std::vector<std::string> resource_paths_to_collect;

    // local compiler.
    resource_paths_to_collect.push_back(local_compiler_path);

    // TODO: Not sure the whole list of dlls to run clang-cl.exe
    // correctly. However, `dumpbin /DEPENDENTS clang-cl.exe` prints nothing
    // special, so currently I don't collect dlls. Some dlls might be necessary
    // to use some feature.

#ifdef __linux__
    // for clang-cl on linux

    if (ElfParser::IsElf(abs_local_compiler_path)) {
      constexpr absl::string_view kLdSoConfPath = "/etc/ld.so.conf";
      std::vector<std::string> searchpath = LoadLdSoConf(kLdSoConfPath);
      ElfDepParser edp(vc_flags.cwd(), searchpath, false);
      absl::flat_hash_set<std::string> exec_deps;
      if (!edp.GetDeps(data->local_compiler_path(), &exec_deps)) {
        LOG(ERROR) << "failed to get library dependencies for executable."
                   << " cwd=" << vc_flags.cwd()
                   << " local_compiler_path=" << data->local_compiler_path();
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
    }
#endif

    absl::flat_hash_set<std::string> visited_paths;
    for (const auto& resource_path : resource_paths_to_collect) {
      if (!AddResourceAsExecutableBinary(resource_path, vc_flags.cwd(),
                                         &visited_paths, data)) {
        AddErrorMessage("failed to get VC resource info for " + resource_path,
                        data);
        return;
      }
    }
  }
}

/* static */
bool VCCompilerInfoBuilder::ParseVCVersion(const std::string& vc_logo,
                                           std::string* version,
                                           std::string* target) {
  // VC's logo format:
  // ... Version 16.00.40219.01 for 80x86
  // so we return cl 16.00.40219.01
  std::string::size_type pos = vc_logo.find("Version ");
  std::string::size_type pos2 = vc_logo.find(" for");
  std::string::size_type pos3 = vc_logo.find("\r");
  if (pos == std::string::npos || pos2 == std::string::npos ||
      pos3 == std::string::npos || pos2 < pos || pos3 < pos2) {
    LOG(INFO) << "Unable to parse cl.exe output."
              << " vc_logo=" << vc_logo;
    return false;
  }
  pos += 8;  // 8: length of "Version "
  *version = vc_logo.substr(pos, pos2 - pos);
  *target = vc_logo.substr(pos2 + 5, pos3 - pos2 - 5);
  return true;
}

/* static */
bool VCCompilerInfoBuilder::GetVCVersion(const std::string& cl_exe_path,
                                         const std::vector<std::string>& env,
                                         const std::string& cwd,
                                         std::string* version,
                                         std::string* target) {
  std::vector<std::string> argv;
  argv.push_back(cl_exe_path);
  int32_t status = 0;
  std::string vc_logo;
  {
    GOMA_COUNTERZ("ReadCommandOutput(vc version)");
    vc_logo = ReadCommandOutput(cl_exe_path, argv, env, cwd,
                                MERGE_STDOUT_STDERR, &status);
  }
  if (status != 0) {
    LOG(ERROR) << "ReadCommandOutput exited with non zero status code."
               << " cl_exe_path=" << cl_exe_path
               << " status=" << status
               << " argv=" << argv
               << " env=" << env
               << " cwd=" << cwd
               << " vc_logo=" << vc_logo;
    return false;
  }
  if (!ParseVCVersion(vc_logo, version, target)) {
    LOG(ERROR) << "Failed to parse VCVersion."
               << " cl_exe_path=" << cl_exe_path
               << " status=" << status
               << " argv=" << argv
               << " env=" << env
               << " cwd=" << cwd
               << " vc_logo=" << vc_logo;
    return false;
  }
  return true;
}

/* static */
bool VCCompilerInfoBuilder::ParseVCOutputString(
    const std::string& output,
    std::vector<std::string>* include_paths,
    std::string* predefined_macros) {
  std::vector<std::string> args;
  // |output| doesn't contains command name, so adds "cl.exe" here.
  args.push_back("cl.exe");
  if (!ParseWinCommandLineToArgv(output, &args)) {
    LOG(ERROR) << "Fail parse cmdline:" << output;
    return false;
  }

  VCFlags flags(args, ".");
  if (!flags.is_successful()) {
    LOG(ERROR) << "ParseVCOutput error:" << flags.fail_message();
    return false;
  }

  copy(flags.include_dirs().begin(), flags.include_dirs().end(),
       back_inserter(*include_paths));

  if (predefined_macros == nullptr)
    return true;
  std::ostringstream ss;
  for (const auto& elm : flags.commandline_macros()) {
    const std::string& macro = elm.first;
    DCHECK(elm.second) << macro;
    size_t found = macro.find('=');
    if (found == std::string::npos) {
      ss << "#define " << macro << "\n";
    } else {
      ss << "#define " << macro.substr(0, found) << " "
         << macro.substr(found + 1) << "\n";
    }
  }
  *predefined_macros += ss.str();
  return true;
}

// static
bool VCCompilerInfoBuilder::GetVCDefaultValues(
    const std::string& cl_exe_path,
    const std::string& vcflags_path,
    const std::vector<std::string>& compiler_info_flags,
    const std::vector<std::string>& compiler_info_envs,
    const std::string& cwd,
    const std::string& lang,
    CompilerInfoData* compiler_info) {
  // VC++ accepts two different undocumented flags to dump all predefined values
  // in preprocessor.  /B1 is for C and /Bx is for C++.
  std::string vc_cpp_flags = "/Bx";
  std::string vc_c_flags = "/B1";
  vc_cpp_flags += vcflags_path;
  vc_c_flags += vcflags_path;

  // It does not matter that non-exist-file.cpp/.c is on disk or not.  VCFlags
  // will error out cl.exe and display the information we want before actually
  // opening that file.
  std::string output_cpp =
      GetVCOutputString(cl_exe_path, vc_cpp_flags, "non-exist-file.cpp",
                        compiler_info_flags, compiler_info_envs, cwd);
  std::string output_c =
      GetVCOutputString(cl_exe_path, vc_c_flags, "non-exist-file.c",
                        compiler_info_flags, compiler_info_envs, cwd);

  std::vector<std::string> cxx_system_include_paths;
  if (!VCCompilerInfoBuilder::ParseVCOutputString(
          output_cpp, &cxx_system_include_paths,
          lang == "c++"
              ? compiler_info->mutable_cxx()->mutable_predefined_macros()
              : nullptr)) {
    return false;
  }
  for (const auto& p : cxx_system_include_paths) {
    compiler_info->mutable_cxx()->add_cxx_system_include_paths(p);
  }
  std::vector<std::string> system_include_paths;
  if (!VCCompilerInfoBuilder::ParseVCOutputString(
          output_c, &system_include_paths,
          lang == "c"
              ? compiler_info->mutable_cxx()->mutable_predefined_macros()
              : nullptr)) {
    return false;
  }
  for (const auto& p : system_include_paths) {
    compiler_info->mutable_cxx()->add_system_include_paths(p);
  }
  return true;
}

}  // namespace devtools_goma
