// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVTOOLS_GOMA_CLIENT_COMPILER_INFO_BUILDER_H_
#define DEVTOOLS_GOMA_CLIENT_COMPILER_INFO_BUILDER_H_

#include <string>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "basictypes.h"
#include "compiler_flags.h"
#include "compiler_specific.h"

MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "client/compiler_info_data.pb.h"
MSVC_POP_WARNING()

namespace devtools_goma {

// CompilerInfoBuilder provides methods to construct CompilerInfoData.
// For each compiler type, derive own CompilerInfoBuilder from this class.
class CompilerInfoBuilder {
 public:
  virtual ~CompilerInfoBuilder() = default;
  CompilerInfoBuilder(const CompilerInfoBuilder&) = delete;
  CompilerInfoBuilder& operator=(const CompilerInfoBuilder&) = delete;

  // Creates new CompilerInfoData* from compiler outputs.
  // if found is true and error_message in it is empty,
  // it successfully gets compiler info.
  // if found is true and error_message in it is not empty,
  // it finds local compiler but failed to get some information, such as
  // system include paths.
  // if found is false if it fails to find local compiler.
  // Caller should take ownership of returned CompilerInfoData.
  std::unique_ptr<CompilerInfoData> FillFromCompilerOutputs(
      const CompilerFlags& flags,
      const std::string& local_compiler_path,
      const std::vector<std::string>& compiler_info_envs);

  virtual void SetCompilerPath(
      const CompilerFlags& flags,
      const std::string& local_compiler_path,
      const std::vector<std::string>& compiler_info_envs,
      CompilerInfoData* data) const;

  virtual void SetLanguageExtension(CompilerInfoData* data) const = 0;

  virtual void SetTypeSpecificCompilerInfo(
      const CompilerFlags& flags,
      const std::string& local_compiler_path,
      const std::string& abs_local_compiler_path,
      const std::vector<std::string>& compiler_info_envs,
      CompilerInfoData* data) const = 0;

  // Returns compiler name to be used in ExecReq's CompilerSpec.
  // If it fails to identify the compiler name, it returns empty string.
  virtual std::string GetCompilerName(const CompilerInfoData& data) const;

  // Adds error message to CompilerInfo. When |failed_at| is not 0,
  // it's also updated.
  static void AddErrorMessage(const std::string& message,
                              CompilerInfoData* compiler_info);
  // Overrides the current error message.
  // if |message| is not empty, |failed_at| must be valid.
  static void OverrideError(const std::string& message,
                            absl::optional<absl::Time> failed_at,
                            CompilerInfoData* compiler_info);

  static bool ResourceInfoFromPath(const std::string& cwd,
                                   const std::string& path,
                                   CompilerInfoData::ResourceType type,
                                   CompilerInfoData::ResourceInfo* r);

  // Add resource as EXECUTABLE_BINARY. If the resource is a symlink,
  // the symlink and the actual files are both added as resource.
  // |visited_paths| is used not to process the same resource twice.
  //
  // Returns true if succeeded (or ignored).
  // Returns false if an error has occurred.
  static bool AddResourceAsExecutableBinary(
      const std::string& resource_path,
      const std::string& cwd,
      absl::flat_hash_set<std::string>* visited_paths,
      CompilerInfoData* data);

 protected:
  CompilerInfoBuilder() = default;
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_COMPILER_INFO_BUILDER_H_
