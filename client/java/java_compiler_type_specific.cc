// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "java/java_compiler_type_specific.h"

#include "env_flags.h"
#include "glog/logging.h"
#include "java/jar_parser.h"
#include "java_flags.h"

GOMA_DECLARE_bool(SEND_COMPILER_BINARY_AS_INPUT);

namespace devtools_goma {

bool JavaCompilerTypeSpecific::RemoteCompileSupported(
    const std::string& trace_id,
    const CompilerFlags& flags,
    bool verify_output) const {
  LOG(INFO) << trace_id << " force fallback to avoid running java program in"
            << " goma backend";
  return false;
}

std::unique_ptr<CompilerInfoData>
JavaCompilerTypeSpecific::BuildCompilerInfoData(
    const CompilerFlags& flags,
    const std::string& local_compiler_path,
    const std::vector<std::string>& compiler_info_envs) {
  return compiler_info_builder_.FillFromCompilerOutputs(
      flags, local_compiler_path, compiler_info_envs);
}

CompilerTypeSpecific::IncludeProcessorResult
JavaCompilerTypeSpecific::RunIncludeProcessor(
    const std::string& trace_id,
    const CompilerFlags& compiler_flags,
    const CompilerInfo& compiler_info,
    const CommandSpec& command_spec,
    FileStatCache* file_stat_cache) {
  DCHECK_EQ(CompilerFlagType::Java, compiler_flags.type());

  LOG(ERROR) << "Java type does not have any include processor";
  return IncludeProcessorResult::ErrorToLog(
      "Java type does not have any include processor");
}

// ----------------------------------------------------------------------

bool JavacCompilerTypeSpecific::RemoteCompileSupported(
    const std::string& trace_id,
    const CompilerFlags& flags,
    bool verify_output) const {
  if (FLAGS_SEND_COMPILER_BINARY_AS_INPUT) {
    // TODO: Ensure flags are valid for remote compile.
    return true;
  }

  const JavacFlags& javac_flag = static_cast<const JavacFlags&>(flags);
  // TODO: remove following code when goma backend get ready.
  // Force fallback a compile request with -processor (b/38215808)
  if (!javac_flag.processors().empty()) {
    LOG(INFO) << trace_id
              << " force fallback to avoid running annotation processor in"
              << " goma backend (b/38215808)";
    return false;
  }
  return true;
}

std::unique_ptr<CompilerInfoData>
JavacCompilerTypeSpecific::BuildCompilerInfoData(
    const CompilerFlags& flags,
    const std::string& local_compiler_path,
    const std::vector<std::string>& compiler_info_envs) {
  return compiler_info_builder_.FillFromCompilerOutputs(
      flags, local_compiler_path, compiler_info_envs);
}

CompilerTypeSpecific::IncludeProcessorResult
JavacCompilerTypeSpecific::RunIncludeProcessor(
    const std::string& trace_id,
    const CompilerFlags& compiler_flags,
    const CompilerInfo& compiler_info,
    const CommandSpec& command_spec,
    FileStatCache* file_stat_cache) {
  DCHECK_EQ(CompilerFlagType::Javac, compiler_flags.type());

  std::set<std::string> required_files;
  JarParser jar_parser;
  jar_parser.GetJarFiles(
      static_cast<const JavacFlags&>(compiler_flags).jar_files(),
      compiler_flags.cwd(), &required_files);
  return IncludeProcessorResult::Ok(std::move(required_files));
}

}  // namespace devtools_goma
