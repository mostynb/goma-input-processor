// Copyright 2011 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef DEVTOOLS_GOMA_CLIENT_LINKER_LINKER_INPUT_PROCESSOR_LINKER_INPUT_PROCESSOR_H_
#define DEVTOOLS_GOMA_CLIENT_LINKER_LINKER_INPUT_PROCESSOR_LINKER_INPUT_PROCESSOR_H_

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "basictypes.h"

namespace devtools_goma {

class CommandSpec;
class CompilerFlags;
class LinkerInputProcessorTest;
class CompilerInfo;
class LibraryPathResolver;
class FrameworkPathResolver;

class LinkerInputProcessor {
 public:
  enum FileType {
    ARCHIVE_FILE,
    THIN_ARCHIVE_FILE,
    ELF_BINARY_FILE,
    OTHER_FILE,
    BAD_FILE,
    MACHO_FAT_FILE,
    MACHO_OBJECT_FILE,
  };
  LinkerInputProcessor(const std::vector<std::string>& args,
                       const std::string& current_directory);
  ~LinkerInputProcessor();

  // Gets input files for command specified by args and library paths.
  // It runs command with -### flag, which dumps command line arguments
  // of collect2 or ld, and collects input files and library paths.
  // It also checks libraries specified by -L and -l.
  // If a library is a thin archive, it also includes files listed in the
  // thin archive as input files.
  // It also tries parsing a file as linker script, and gets input files
  // described in the linker script.
  // If Dumped command line arguments contain LIBRARY_PATH=, it set the paths
  // to library_paths. Otherwise, it set paths parsed from -L options in dumped
  // command line to library_paths.
  bool GetInputFilesAndLibraryPath(const CompilerInfo& compiler_info,
                                   const CommandSpec& command_spec,
                                   std::set<std::string>* input_files,
                                   std::vector<std::string>* library_paths);

 private:
  friend class LinkerInputProcessorTest;
  // Provided for test.
  explicit LinkerInputProcessor(const std::string& current_directory);
  bool CaptureDriverCommandLine(const CommandSpec& command_spec,
                                std::vector<std::string>* driver_args,
                                std::vector<std::string>* driver_envs);

  // Parses outputs of "gcc -### ..."
  static bool ParseDumpOutput(const std::string& dump_output,
                              std::vector<std::string>* driver_args,
                              std::vector<std::string>* driver_envs);

  void ParseDriverCommandLine(const std::vector<std::string>& driver_args,
                              std::vector<std::string>* input_paths);
  void GetLibraryPath(const std::vector<std::string>& driver_envs,
                      std::vector<std::string>* library_paths);
  static FileType CheckFileType(const std::string& path);
  static void ParseThinArchive(const std::string& filename,
                               std::set<std::string>* input_files);
  void TryParseLinkerScript(const std::string& filename,
                            std::vector<std::string>* input_paths);
  void TryParseElfNeeded(const std::string& filename,
                         std::vector<std::string>* input_paths);
#ifdef __MACH__
  void TryParseMachONeeded(const std::string& filename,
                           const int max_recursion,
                           std::set<std::string>* input_files);
#endif

  std::unique_ptr<CompilerFlags> flags_;
  std::unique_ptr<LibraryPathResolver> library_path_resolver_;
  std::unique_ptr<FrameworkPathResolver> framework_path_resolver_;
  std::string arch_;

  DISALLOW_COPY_AND_ASSIGN(LinkerInputProcessor);
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_LINKER_LINKER_INPUT_PROCESSOR_LINKER_INPUT_PROCESSOR_H_
