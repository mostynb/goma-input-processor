// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVTOOLS_GOMA_LIB_GCC_FLAGS_H_
#define DEVTOOLS_GOMA_LIB_GCC_FLAGS_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "lib/cxx_flags.h"
#include "lib/flag_parser.h"

namespace devtools_goma {

class GCCFlags : public CxxFlags {
 public:
  enum Mode { PREPROCESS, COMPILE, LINK };

  GCCFlags(const std::vector<std::string>& args, const std::string& cwd);

  const std::vector<std::string> include_dirs() const;
  const std::vector<std::string>& non_system_include_dirs() const {
    return non_system_include_dirs_;
  }
  const std::vector<std::string>& root_includes() const {
    return root_includes_;
  }
  const std::vector<std::string>& framework_dirs() const {
    return framework_dirs_;
  }
  const std::vector<std::string>& arch() const { return arch_; }

  const std::vector<std::pair<std::string, bool>>& commandline_macros() const {
    return commandline_macros_;
  }

  std::string compiler_name() const override;

  Mode mode() const { return mode_; }

  std::string isysroot() const { return isysroot_; }
  const std::string& resource_dir() const { return resource_dir_; }
  const std::set<std::string>& fsanitize() const { return fsanitize_; }
  const std::map<std::string, std::string>& fdebug_prefix_map() const {
    return fdebug_prefix_map_;
  }
  const std::string& thinlto_index() const { return thinlto_index_; }
  const std::string& fdebug_compilation_dir() const {
    return fdebug_compilation_dir_;
  }
  const std::string& fcoverage_compilation_dir() const {
    return fcoverage_compilation_dir_;
  }
  const std::string& ffile_compilation_dir() const {
    return ffile_compilation_dir_;
  }

  bool is_cplusplus() const override { return is_cplusplus_; }
  bool has_nostdinc() const { return has_nostdinc_; }
  bool has_no_integrated_as() const { return has_no_integrated_as_; }
  bool has_pipe() const { return has_pipe_; }
  bool has_fcoverage_mapping() const { return has_fcoverage_mapping_; }
  bool has_ffreestanding() const { return has_ffreestanding_; }
  bool has_fno_hosted() const { return has_fno_hosted_; }
  bool has_fsyntax_only() const { return has_fsyntax_only_; }
  bool has_ftime_trace() const { return has_ftime_trace_; }
  bool has_resource_dir() const { return !resource_dir_.empty(); }
  bool has_wrapper() const { return has_wrapper_; }
  bool has_fplugin() const { return has_fplugin_; }
  bool is_linking() const override { return mode_ == GCCFlags::LINK; }
  bool is_precompiling_header() const override {
    return is_precompiling_header_;
  }
  bool is_stdin_input() const { return is_stdin_input_; }

  bool has_fmodules() const { return has_fmodules_; }
  bool has_fimplicit_module_maps() const { return has_fimplicit_module_maps_; }
  const std::string& clang_module_map_file() const {
    return clang_module_map_file_;
  }
  const std::pair<std::string, std::string>& clang_module_file() const {
    return clang_module_file_;
  }
  bool has_emit_module() const { return has_emit_module_; }

  CompilerFlagType type() const override { return CompilerFlagType::Gcc; }

  bool IsClientImportantEnv(const char* env) const override;
  bool IsServerImportantEnv(const char* env) const override;

  static void DefineFlags(FlagParser* parser);

  static std::string GetCompilerName(absl::string_view arg);

  // If we know -Wfoo, returns true for "foo".
  static bool IsKnownWarningOption(absl::string_view option);
  static bool IsKnownDebugOption(absl::string_view v);

  // True if arg is gcc command name. Note that clang is considered as
  // gcc variant, so IsGCCCommand("clang") returns true.  However, since
  // clang-cl is not compatible with gcc, IsGCCCommand("clang-cl") returns
  // false.
  static bool IsGCCCommand(absl::string_view arg);
  static bool IsClangCommand(absl::string_view arg);
  static bool IsNaClGCCCommand(absl::string_view arg);
  static bool IsNaClClangCommand(absl::string_view arg);
  static bool IsPNaClClangCommand(absl::string_view arg);

 private:
  friend class GCCFlagsTest;
  static std::string GetLanguage(const std::string& compiler_name,
                                 const std::string& input_filename);

  // Process -Xclang flags.
  // It's OK just to add most -Xclang flags into compiler_info_flags.
  // However, there is a few exceptions (See implementations).
  void ProcessXclangFlags(const std::vector<std::string>& xclang_flags);

  std::vector<std::string> remote_flags_;
  std::vector<std::string> non_system_include_dirs_;
  std::vector<std::string> root_includes_;
  std::vector<std::string> framework_dirs_;
  std::vector<std::string> arch_;
  // The second value is true if the macro is defined and false if undefined.
  std::vector<std::pair<std::string, bool>> commandline_macros_;
  Mode mode_;
  std::string isysroot_;
  std::string resource_dir_;
  std::string thinlto_index_;
  std::string fdebug_compilation_dir_;
  std::string fcoverage_compilation_dir_;
  std::string ffile_compilation_dir_;
  // -fsanitize can be specified multiple times, and can be comma separated
  // values.
  std::set<std::string> fsanitize_;
  std::map<std::string, std::string> fdebug_prefix_map_;
  bool is_cplusplus_;
  bool has_nostdinc_;
  bool has_no_integrated_as_;
  bool has_pipe_;
  bool has_fcoverage_mapping_ = false;
  bool has_ffreestanding_;
  bool has_fno_hosted_;
  bool has_fsyntax_only_;
  bool has_ftime_trace_;
  bool has_wrapper_;
  bool has_fplugin_;
  bool is_precompiling_header_;
  bool is_stdin_input_;

  // clang-modules related variables
  bool has_fmodules_;
  bool has_fimplicit_module_maps_;
  bool has_emit_module_;
  // explicit module-map-file (specified by -fmodule-map-file)
  std::string clang_module_map_file_;
  // explicit module-file (specified by -fmodule-file=[<name>=]<file>)
  // .first is <name>, .second is <file>
  // If <name> is omitted, .first is empty.
  std::pair<std::string, std::string> clang_module_file_;
};

// Get the version of gcc/clang to fill CommandSpec.
// dumpversion is the result of gcc/clang -dumpversion
// version is the result of gcc/clang --version
std::string GetCxxCompilerVersionFromCommandOutputs(
    const std::string& command,
    const std::string& dumpversion,
    const std::string& version);

// Truncate string at \r\n.
std::string GetFirstLine(const std::string& buf);

// Remove a program name from |version| if it comes from gcc/g++.
std::string NormalizeGccVersion(const std::string& version);

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_LIB_GCC_FLAGS_H_
