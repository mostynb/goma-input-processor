// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVTOOLS_GOMA_CLIENT_CLANG_TIDY_CLANG_TIDY_COMPILER_TYPE_SPECIFIC_H_
#define DEVTOOLS_GOMA_CLIENT_CLANG_TIDY_CLANG_TIDY_COMPILER_TYPE_SPECIFIC_H_

#include "clang_tidy/clang_tidy_compiler_info_builder.h"
#include "cxx/cxx_compiler_type_specific.h"

namespace devtools_goma {

class ClangTidyCompilerTypeSpecific : public CxxCompilerTypeSpecific {
 public:
  ClangTidyCompilerTypeSpecific(const ClangTidyCompilerTypeSpecific&) = delete;
  void operator=(const ClangTidyCompilerTypeSpecific&) = delete;

  bool RemoteCompileSupported(const std::string& trace_id,
                              const CompilerFlags& flags,
                              bool verify_output) const override {
    return true;
  }

  std::unique_ptr<CompilerInfoData> BuildCompilerInfoData(
      const CompilerFlags& flags,
      const std::string& local_compiler_path,
      const std::vector<std::string>& compiler_info_envs) override;

  bool SupportsDepsCache(const CompilerFlags&) const override { return false; }

 private:
  ClangTidyCompilerTypeSpecific() = default;

  ClangTidyCompilerInfoBuilder compiler_info_builder_;

  friend class CompilerTypeSpecificCollection;
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_CLANG_TIDY_CLANG_TIDY_COMPILER_TYPE_SPECIFIC_H_
