// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVTOOLS_GOMA_CLIENT_CXX_CXX_COMPILER_INFO_H_
#define DEVTOOLS_GOMA_CLIENT_CXX_CXX_COMPILER_INFO_H_

#include <memory>

#include "absl/container/flat_hash_map.h"
#include "compiler_info.h"
#include "cxx/include_processor/cpp_directive.h"

namespace devtools_goma {

class CxxCompilerInfo : public CompilerInfo {
 public:
  explicit CxxCompilerInfo(std::unique_ptr<CompilerInfoData> data);
  CompilerInfoType type() const override { return CompilerInfoType::Cxx; }

  bool IsSystemInclude(const std::string& filepath) const;

  bool DependsOnCwd(const std::string& cwd) const override;

  // include paths could be relative path from cwd.
  // Also, system include paths could be relative path from toolchain root
  // (Windows NaCl toolchain only).
  // You should file::JoinPathRespectAbsolute with cwd before you use it in
  // include processor.

  // quote dir is valid only if it exists. note quote dir may be cwd relative
  // so it depends on cwd if dir is valid or not.
  const std::vector<std::string>& quote_include_paths() const {
    return quote_include_paths_;
  }
  const std::vector<std::string>& cxx_system_include_paths() const {
    return cxx_system_include_paths_;
  }
  const std::vector<std::string>& system_include_paths() const {
    return system_include_paths_;
  }
  const std::vector<std::string>& system_framework_paths() const {
    return system_framework_paths_;
  }

  const std::string& toolchain_root() const {
    return data_->cxx().toolchain_root();
  }
  const std::string& predefined_macros() const {
    return data_->cxx().predefined_macros();
  }
  const SharedCppDirectives& predefined_directives() const {
    return predefined_directives_;
  }

  const absl::flat_hash_map<std::string, bool>& supported_predefined_macros()
      const {
    return supported_predefined_macros_;
  }
  const absl::flat_hash_map<std::string, int>& has_feature() const {
    return has_feature_;
  }
  const absl::flat_hash_map<std::string, int>& has_extension() const {
    return has_extension_;
  }
  const absl::flat_hash_map<std::string, int>& has_attribute() const {
    return has_attribute_;
  }
  const absl::flat_hash_map<std::string, int>& has_cpp_attribute() const {
    return has_cpp_attribute_;
  }
  const absl::flat_hash_map<std::string, int>& has_declspec_attribute() const {
    return has_declspec_attribute_;
  }
  const absl::flat_hash_map<std::string, int>& has_builtin() const {
    return has_builtin_;
  }
  const absl::flat_hash_map<std::string, int>& has_warning() const {
    return has_warning_;
  }

  std::string cxx_target() const { return data_->cxx().cxx_target(); }

 private:
  std::vector<std::string> quote_include_paths_;
  std::vector<std::string> cxx_system_include_paths_;
  std::vector<std::string> system_include_paths_;
  std::vector<std::string> system_framework_paths_;

  // <macro name, hidden>.
  // If it is hidden macro like __has_include__ in GCC 5, hidden is set.
  absl::flat_hash_map<std::string, bool> supported_predefined_macros_;
  absl::flat_hash_map<std::string, int> has_feature_;
  absl::flat_hash_map<std::string, int> has_extension_;
  absl::flat_hash_map<std::string, int> has_attribute_;
  absl::flat_hash_map<std::string, int> has_cpp_attribute_;
  absl::flat_hash_map<std::string, int> has_declspec_attribute_;
  absl::flat_hash_map<std::string, int> has_builtin_;
  absl::flat_hash_map<std::string, int> has_warning_;

  SharedCppDirectives predefined_directives_;
};

inline const CxxCompilerInfo& ToCxxCompilerInfo(
    const CompilerInfo& compiler_info) {
  DCHECK_EQ(CompilerInfoType::Cxx, compiler_info.type());
  return static_cast<const CxxCompilerInfo&>(compiler_info);
}

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_CXX_CXX_COMPILER_INFO_H_
