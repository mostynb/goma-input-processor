// Copyright 2019 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVTOOLS_GOMA_CLIENT_CXX_CHROMEOS_COMPILER_INFO_BUILDER_HELPER_H_
#define DEVTOOLS_GOMA_CLIENT_CXX_CHROMEOS_COMPILER_INFO_BUILDER_HELPER_H_

#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "client/compiler_info_data.pb.h"

namespace devtools_goma {

class ChromeOSCompilerInfoBuilderHelper {
 public:
  // Returns true if compiler looks like chromeos simple chrome toolcahin.
  static bool IsSimpleChromeClangCommand(absl::string_view local_compiler_path,
                                         absl::string_view real_compiler_path);

  // Collects simple chrome toolchain resources for Arbitrary Toolchain Support.
  static bool CollectSimpleChromeClangResources(
      const std::string& cwd,
      absl::string_view real_compiler_path,
      std::vector<std::string>* resource_paths);

  // Estimates major version from chromeos simple chrome toolchain.
  // Here, assuming real compiler is like `clang-<VERSION>.elf`.
  // Returns true if succeeded, false otherwise.
  static bool EstimateClangMajorVersion(absl::string_view real_compiler_path,
                                        int* version);

  // Returns true if the current environment is chroot env, and
  // abs_local_compiler_path indicates a system clang in the chroot env.
  static bool IsClangInChrootEnv(const std::string& abs_local_compiler_path);
  // Collects clang resources in chromeos chroot env.
  static bool CollectChrootClangResources(
      const std::string& cwd,
      const std::vector<std::string>& envs,
      absl::string_view local_compiler_path,
      absl::string_view real_compiler_path,
      std::vector<std::string>* resource_paths);

  static void SetAdditionalFlags(
      const std::string& abs_local_compiler_path,
      google::protobuf::RepeatedPtrField<std::string>* additional_flags);

  // Returns an ELF executable of clang in the same directory.
  // Or, returns |wrapper_path| if no such executable is found.
  // An empty string is returned on error.
  static std::string GetRealClangPath(const std::string& cwd,
                                      const std::string& wrapper_path);

  static bool IsValidRealClangName(bool is_cxx, absl::string_view path);
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_CXX_CHROMEOS_COMPILER_INFO_BUILDER_HELPER_H_
