// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVTOOLS_GOMA_LIB_CLANG_TIDY_FLAGS_H_
#define DEVTOOLS_GOMA_LIB_CLANG_TIDY_FLAGS_H_

#include <string>
#include <vector>

#include "lib/cxx_flags.h"
#include "lib/gcc_flags.h"

namespace devtools_goma {

// ClangTidy will be used like this.
// $ clang-tidy -checks='*' foo.cc -- -I. -std=c++11
// This command line contains options for clang-tidy and options for clang.
// clang options are parsed in the internal |gcc_flags_|.
// When '--' is not given in the command line, compilation database
// (compile_commands.json) is read. Otherwise, compilation database won't
// be used.
class ClangTidyFlags : public CxxFlags {
 public:
  ClangTidyFlags(const std::vector<std::string>& args, const std::string& cwd);

  std::string compiler_name() const override;
  CompilerFlagType type() const override { return CompilerFlagType::ClangTidy; }

  const std::string& cwd_for_include_processor() const override;

  // Sets the corresponding clang args for IncludeProcessor.
  // These are set in CompilerTask::InitCompilerFlags.
  void SetClangArgs(const std::vector<std::string>& clang_args,
                    const std::string& dir);
  void SetCompilationDatabasePath(const std::string& compdb_path);

  // NOTE: These methods are valid only after SetClangArgs() is called.
  // Calling these before SetClangArgs() will cause undefined behavior.
  const std::vector<std::string>& non_system_include_dirs() const;
  const std::vector<std::string>& root_includes() const;
  const std::vector<std::string>& framework_dirs() const;
  const std::vector<std::pair<std::string, bool>>& commandline_macros() const;
  bool is_cplusplus() const override;
  bool has_nostdinc() const;

  const std::string& build_path() const { return build_path_; }
  const std::vector<std::string>& extra_arg() const { return extra_arg_; }
  const std::vector<std::string>& extra_arg_before() const {
    return extra_arg_before_;
  }

  bool seen_hyphen_hyphen() const { return seen_hyphen_hyphen_; }
  const std::vector<std::string>& args_after_hyphen_hyphen() const {
    return args_after_hyphen_hyphen_;
  }

  bool IsClientImportantEnv(const char* env) const override { return false; }
  bool IsServerImportantEnv(const char* env) const override { return false; }

  static void DefineFlags(FlagParser* parser);
  static bool IsClangTidyCommand(absl::string_view arg);
  static std::string GetCompilerName(absl::string_view arg);

 private:
  std::string build_path_;  // the value of option "-p".
  std::vector<std::string> extra_arg_;
  std::vector<std::string> extra_arg_before_;

  bool seen_hyphen_hyphen_;
  std::vector<std::string> args_after_hyphen_hyphen_;

  // Converted clang flag. This should be made in the constructor.
  std::unique_ptr<GCCFlags> gcc_flags_;
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_LIB_CLANG_TIDY_FLAGS_H_
