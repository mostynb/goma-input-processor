// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gcc_compiler_type_specific.h"

#include "glog/logging.h"
#include "glog/stl_logging.h"
#include "linker/linker_input_processor/linker_input_processor.h"
#include "linker/linker_input_processor/thinlto_import_processor.h"
#include "path.h"

namespace devtools_goma {

bool GCCCompilerTypeSpecific::RemoteCompileSupported(
    const std::string& trace_id,
    const CompilerFlags& flags,
    bool verify_output) const {
  const GCCFlags& gcc_flag = static_cast<const GCCFlags&>(flags);
  if (gcc_flag.is_stdin_input()) {
    LOG(INFO) << trace_id << " force fallback."
              << " cannot use stdin as input in goma backend.";
    return false;
  }
  if (gcc_flag.has_wrapper()) {
    LOG(INFO) << trace_id << " force fallback. -wrapper is not supported";
    return false;
  }
  if (!verify_output && gcc_flag.mode() == GCCFlags::PREPROCESS) {
    LOG(INFO) << trace_id
              << " force fallback. preprocess is usually light-weight.";
    return false;
  }
  if (!enable_gch_hack_ && gcc_flag.is_precompiling_header()) {
    LOG(INFO) << trace_id
              << " force fallback. gch hack is not enabled and precompiling.";
    return false;
  }
  if (!enable_remote_link_ && gcc_flag.is_linking()) {
    LOG(INFO) << trace_id << " force fallback linking.";
    return false;
  }
  if (gcc_flag.has_fmodules()) {
    if (!enable_remote_clang_modules_) {
      LOG(INFO) << trace_id << " force fallback -fmodules.";
      return false;
    }
    // -Xclang -emit-module is not supported yet. Do fallback.
    // b/24956317
    if (gcc_flag.has_emit_module()) {
      LOG(INFO) << trace_id << " force fallback -emit-module."
                << " -Xclang -emit-module is not supported yet.";
      return false;
    }
    // Without uploading the module cache, performance become worse than
    // local.  However, there might not be easy way to identify that?
    // b/123546938
    if (gcc_flag.has_fimplicit_module_maps()) {
      LOG(INFO) << trace_id << " force fallback implicit clang module maps.";
      return false;
    }
    // Since there is no easy way to identify the cache to upload,
    // we also do not allow -fmodule-map-file used for compile in remote.
    // b/123546938
    if (!gcc_flag.clang_module_map_file().empty()) {
      LOG(INFO) << trace_id << " force fallback -fmodule-map-file.";
      return false;
    }
  }
  if (gcc_flag.arch().size() > 1) {
    LOG(WARNING)
        << trace_id
        << " force fallback. multi arch is not supported. http://b/27730714"
        << " arch=" << gcc_flag.arch();
    return false;
  }
  absl::string_view ext = file::Extension(gcc_flag.input_filenames()[0]);
  if (ext == "s" || ext == "S") {
    LOG(INFO) << trace_id
              << " force fallback. assembler should be light-weight.";
    return false;
  }
  return true;
}

std::unique_ptr<CompilerInfoData>
GCCCompilerTypeSpecific::BuildCompilerInfoData(
    const CompilerFlags& flags,
    const std::string& local_compiler_path,
    const std::vector<std::string>& compiler_info_envs) {
  return compiler_info_builder_.FillFromCompilerOutputs(
      flags, local_compiler_path, compiler_info_envs);
}

CompilerTypeSpecific::IncludeProcessorResult
GCCCompilerTypeSpecific::RunIncludeProcessor(
    const std::string& trace_id,
    const CompilerFlags& compiler_flags,
    const CompilerInfo& compiler_info,
    const CommandSpec& command_spec,
    FileStatCache* file_stat_cache) {
  DCHECK_EQ(CompilerFlagType::Gcc, compiler_flags.type());

  const GCCFlags& flags = static_cast<const GCCFlags&>(compiler_flags);

  if (flags.lang() == "ir") {
    if (flags.thinlto_index().empty()) {
      // No need to to read .imports file. imports is nothing.
      return IncludeProcessorResult::Ok(std::set<std::string>());
    }

    // Otherwise, run ThinLTOImports.
    return RunThinLTOImports(trace_id, flags);
  }

  if (flags.args().size() == 2 && flags.args()[1] == "--version") {
    // for requester_env_.verify_command()
    VLOG(1) << trace_id << " --version";
    return IncludeProcessorResult::Ok(std::set<std::string>());
  }

  // LINK mode.
  if (flags.mode() == GCCFlags::LINK) {
    return RunLinkIncludeProcessor(trace_id, flags, compiler_info,
                                   command_spec);
  }

  // go into the usual path.
  return CxxCompilerTypeSpecific::RunIncludeProcessor(
      trace_id, compiler_flags, compiler_info, command_spec, file_stat_cache);
}

bool GCCCompilerTypeSpecific::SupportsDepsCache(
    const CompilerFlags& flags) const {
  const GCCFlags& gcc_flags = static_cast<const GCCFlags&>(flags);
  return gcc_flags.mode() == GCCFlags::COMPILE;
}

CompilerTypeSpecific::IncludeProcessorResult
GCCCompilerTypeSpecific::RunThinLTOImports(const std::string& trace_id,
                                           const GCCFlags& flags) {
  ThinLTOImportProcessor processor;
  std::set<std::string> required_files;
  if (!processor.GetIncludeFiles(flags.thinlto_index(), flags.cwd(),
                                 &required_files)) {
    LOG(ERROR) << trace_id << " failed to get ThinLTO imports";
    return IncludeProcessorResult::ErrorToLog("failed to get ThinLTO imports");
  }

  return IncludeProcessorResult::Ok(std::move(required_files));
}

CompilerTypeSpecific::IncludeProcessorResult
GCCCompilerTypeSpecific::RunLinkIncludeProcessor(
    const std::string& trace_id,
    const GCCFlags& flags,
    const CompilerInfo& compiler_info,
    const CommandSpec& command_spec) {
  LinkerInputProcessor linker_input_processor(flags.args(), flags.cwd());

  std::set<std::string> required_files;
  std::vector<std::string> system_library_paths;
  if (!linker_input_processor.GetInputFilesAndLibraryPath(
          compiler_info, command_spec, &required_files,
          &system_library_paths)) {
    return IncludeProcessorResult::ErrorToLog("failed to get input files " +
                                              flags.DebugString());
  }

  IncludeProcessorResult result =
      IncludeProcessorResult::Ok(std::move(required_files));
  result.system_library_paths = std::move(system_library_paths);
  return result;
}

// static
bool GCCCompilerTypeSpecific::enable_gch_hack_;

// static
bool GCCCompilerTypeSpecific::enable_remote_link_;

// static
bool GCCCompilerTypeSpecific::enable_remote_clang_modules_;

}  // namespace devtools_goma
