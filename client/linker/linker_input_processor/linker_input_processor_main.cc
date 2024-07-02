// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "linker_input_processor.h"

#include <iostream>

#include "compiler_flags.h"
#include "compiler_flags_parser.h"
#include "cxx/cxx_compiler_info.h"
#include "cxx/gcc_compiler_info_builder.h"
#include "gcc_flags.h"
#include "glog/logging.h"
#include "lib/goma_data.pb.h"
#include "mypath.h"

using devtools_goma::GCCCompilerInfoBuilder;

int main(int argc, char* argv[], const char** envp) {
  google::InitGoogleLogging(argv[0]);

  const std::string cwd = devtools_goma::GetCurrentDirNameOrDie();
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " local_compiler_path gcc ..."
              << std::endl;
    exit(1);
  }
  std::string local_compiler_path = argv[1];
  std::vector<std::string> args;
  for (int i = 2; i < argc; ++i)
    args.push_back(argv[i]);

  std::unique_ptr<devtools_goma::CompilerFlags> flags(
      devtools_goma::CompilerFlagsParser::MustNew(args, cwd));
  if (flags->type() != devtools_goma::CompilerFlagType::Gcc) {
    std::cerr << "only gcc/g++ is supported" << std::endl;
    exit(1);
  }
  const devtools_goma::GCCFlags& gcc_flags =
      static_cast<const devtools_goma::GCCFlags&>(*flags);
  std::vector<std::string> compiler_info_envs;
  flags->GetClientImportantEnvs(envp, &compiler_info_envs);
  std::unique_ptr<devtools_goma::CompilerInfoData> compiler_info_data(
      GCCCompilerInfoBuilder().FillFromCompilerOutputs(
          gcc_flags, local_compiler_path, compiler_info_envs));
  devtools_goma::CxxCompilerInfo compiler_info(std::move(compiler_info_data));
  if (compiler_info.HasError()) {
    std::cerr << compiler_info.error_message() << std::endl;
    exit(1);
  }
  devtools_goma::CommandSpec command_spec;
  command_spec.set_name(flags->compiler_name());
  command_spec.set_local_compiler_path(local_compiler_path);

  devtools_goma::LinkerInputProcessor linker_input_processor(args, cwd);

  std::set<std::string> input_files;
  std::vector<std::string> library_paths;
  if (!linker_input_processor.GetInputFilesAndLibraryPath(
          compiler_info, command_spec, &input_files, &library_paths)) {
    std::cerr << "GetInputFilesAndLibraryPath failed" << std::endl;
    exit(1);
  }
  std::cout << "#Input files" << std::endl;
  for (std::set<std::string>::iterator iter = input_files.begin();
       iter != input_files.end(); ++iter) {
    std::cout << *iter << std::endl;
  }
  std::cout << "#library path" << std::endl;
  for (size_t i = 0; i < library_paths.size(); i++) {
    std::cout << library_paths[i] << std::endl;
  }
  exit(0);
}
