// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVTOOLS_GOMA_CLIENT_CLANG_TIDY_CLANG_TIDY_COMPILER_INFO_BUILDER_H_
#define DEVTOOLS_GOMA_CLIENT_CLANG_TIDY_CLANG_TIDY_COMPILER_INFO_BUILDER_H_

#include <string>
#include <vector>

#include "cxx/cxx_compiler_info_builder.h"

namespace devtools_goma {

class ClangTidyCompilerInfoBuilder : public CxxCompilerInfoBuilder {
 public:
  ~ClangTidyCompilerInfoBuilder() override = default;

  void SetTypeSpecificCompilerInfo(
      const CompilerFlags& flags,
      const std::string& local_compiler_path,
      const std::string& abs_local_compiler_path,
      const std::vector<std::string>& compiler_info_envs,
      CompilerInfoData* data) const override;

  // Executes clang-tidy and gets the string output for clang-tidy version.
  static bool GetClangTidyVersionTarget(
      const std::string& clang_tidy_path,
      const std::vector<std::string>& compiler_info_envs,
      const std::string& cwd,
      std::string* version,
      std::string* target);
  static bool ParseClangTidyVersionTarget(const std::string& output,
                                          std::string* version,
                                          std::string* target);
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_CLANG_TIDY_CLANG_TIDY_COMPILER_INFO_BUILDER_H_
