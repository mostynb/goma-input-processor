// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compiler_info_builder.h"

#include "absl/time/clock.h"
#include "compiler_flag_type_specific.h"
#include "compiler_info.h"
#include "counterz.h"
#include "glog/logging.h"
#include "goma_hash.h"
#include "path.h"
#include "path_resolver.h"
#include "sha256_hash_cache.h"

#ifdef _WIN32
#include "posix_helper_win.h"
#endif

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace devtools_goma {

namespace {

bool AddResourceAsExecutableBinaryInternal(
    const std::string& resource_path,
    const std::string& cwd,
    int remaining_symlink_follow_count,
    absl::flat_hash_set<std::string>* visited_paths,
    CompilerInfoData* data) {
  std::string abs_resource_path =
      file::JoinPathRespectAbsolute(cwd, resource_path);
  if (!visited_paths->insert(PathResolver::ResolvePath(abs_resource_path))
           .second) {
    // This path has been visited before. Abort.
    return true;
  }

  CompilerInfoData::ResourceInfo r;
  if (!CompilerInfoBuilder::ResourceInfoFromPath(
          cwd, resource_path, CompilerInfoData::EXECUTABLE_BINARY, &r)) {
    CompilerInfoBuilder::AddErrorMessage(
        "failed to get resource info for " + resource_path, data);
    LOG(ERROR) << "failed to get resource info for " + resource_path;
    return false;
  }

  if (r.symlink_path().empty()) {
    // Not a symlink, add it as a resource directly.
    *data->add_resource() = std::move(r);
    return true;
  }

  // It's a symlink.
  if (remaining_symlink_follow_count <= 0) {
    // Too many nested symlink. Abort and return an error.
    CompilerInfoBuilder::AddErrorMessage(
        "too deep nested symlink: " + resource_path, data);
    return false;
  }
  std::string symlink_path = file::JoinPathRespectAbsolute(
      file::Dirname(resource_path), r.symlink_path());
  // Implementation Note: the original resource must come first. If the resource
  // is a symlink, the actual file must be added after the symlink. The server
  // assumes the first resource is a compiler used in a command line, even
  // if it's a symlink.
  *data->add_resource() = std::move(r);
  return AddResourceAsExecutableBinaryInternal(
      symlink_path, cwd, remaining_symlink_follow_count - 1, visited_paths,
      data);
}

}  // namespace

/* static */
std::unique_ptr<CompilerInfoData> CompilerInfoBuilder::FillFromCompilerOutputs(
    const CompilerFlags& flags,
    const std::string& local_compiler_path,
    const std::vector<std::string>& compiler_info_envs) {
  GOMA_COUNTERZ("");
  std::unique_ptr<CompilerInfoData> data(new CompilerInfoData);
  SetLanguageExtension(data.get());

  data->set_last_used_at(time(nullptr));

  // TODO: minimize the execution of ReadCommandOutput.
  // If we execute gcc/clang with -xc -v for example, we can get not only
  // real compiler path but also target and version.
  // However, I understand we need large refactoring of CompilerInfo
  // for minimizing the execution while keeping readability.
  SetCompilerPath(flags, local_compiler_path, compiler_info_envs, data.get());

  if (!file::IsAbsolutePath(local_compiler_path)) {
    data->set_cwd(flags.cwd());
  }

  const std::string abs_local_compiler_path = PathResolver::ResolvePath(
      file::JoinPathRespectAbsolute(flags.cwd(), data->local_compiler_path()));
  VLOG(2) << "FillFromCompilerOutputs:"
          << " abs_local_compiler_path=" << abs_local_compiler_path
          << " cwd=" << flags.cwd()
          << " local_compiler_path=" << data->local_compiler_path();

  const std::string abs_real_compiler_path = PathResolver::ResolvePath(
      file::JoinPathRespectAbsolute(flags.cwd(), data->real_compiler_path()));
  VLOG(2) << "FillFromCompilerOutputs:"
          << " abs_real_compiler_path=" << abs_real_compiler_path
          << " cwd=" << flags.cwd()
          << " real_compiler_path=" << data->real_compiler_path();

  if (!SHA256HashCache::instance()->GetHashFromCacheOrFile(
          abs_local_compiler_path, data->mutable_local_compiler_hash())) {
    LOG(ERROR) << "Could not open local compiler file "
               << abs_local_compiler_path;
    data->set_found(false);
    return data;
  }

  if (!SHA256HashCache::instance()->GetHashFromCacheOrFile(
          abs_real_compiler_path, data->mutable_hash())) {
    LOG(ERROR) << "Could not open real compiler file "
               << abs_real_compiler_path;
    data->set_found(false);
    return data;
  }

  data->set_name(GetCompilerName(*data));
  if (data->name().empty()) {
    AddErrorMessage("Failed to get compiler name of " + abs_local_compiler_path,
                    data.get());
    LOG(ERROR) << data->error_message();
    return data;
  }
  data->set_lang(flags.lang());

  FileStat local_compiler_stat(abs_local_compiler_path);
  if (!local_compiler_stat.IsValid()) {
    LOG(ERROR) << "Failed to get file id of " << abs_local_compiler_path;
    data->set_found(false);
    return data;
  }
  SetFileStatToData(local_compiler_stat, data->mutable_local_compiler_stat());
  data->mutable_real_compiler_stat()->CopyFrom(data->local_compiler_stat());

  data->set_found(true);

  if (abs_local_compiler_path != abs_real_compiler_path) {
    FileStat real_compiler_stat(abs_real_compiler_path);
    if (!real_compiler_stat.IsValid()) {
      LOG(ERROR) << "Failed to get file stat of " << abs_real_compiler_path;
      data->set_found(false);
      return data;
    }
    SetFileStatToData(real_compiler_stat, data->mutable_real_compiler_stat());
  }

  SetTypeSpecificCompilerInfo(flags, local_compiler_path,
                              abs_local_compiler_path, compiler_info_envs,
                              data.get());
  return data;
}

void CompilerInfoBuilder::SetCompilerPath(
    const CompilerFlags& flags,
    const std::string& local_compiler_path,
    const std::vector<std::string>& compiler_info_envs,
    CompilerInfoData* data) const {
  data->set_local_compiler_path(local_compiler_path);
  data->set_real_compiler_path(local_compiler_path);
}

std::string CompilerInfoBuilder::GetCompilerName(
    const CompilerInfoData& data) const {
  // The default implementation is to return compilername from local compiler
  // path.
  return CompilerFlagTypeSpecific::GetCompilerNameFromArg(
      data.local_compiler_path());
}

/* static */
void CompilerInfoBuilder::AddErrorMessage(const std::string& message,
                                          CompilerInfoData* compiler_info) {
  if (compiler_info->failed_at() == 0)
    compiler_info->set_failed_at(absl::ToTimeT(absl::Now()));

  if (compiler_info->has_error_message()) {
    compiler_info->set_error_message(compiler_info->error_message() + "\n");
  }
  compiler_info->set_error_message(compiler_info->error_message() + message);
}

/* static */
void CompilerInfoBuilder::OverrideError(const std::string& message,
                                        absl::optional<absl::Time> failed_at,
                                        CompilerInfoData* compiler_info) {
  DCHECK((message.empty() && !failed_at.has_value()) ||
         (!message.empty() && failed_at.has_value()));
  compiler_info->set_error_message(message);
  if (failed_at.has_value()) {
    compiler_info->set_failed_at(absl::ToTimeT(*failed_at));
  }
}

/* static */
bool CompilerInfoBuilder::ResourceInfoFromPath(
    const std::string& cwd,
    const std::string& path,
    CompilerInfoData::ResourceType type,
    CompilerInfoData::ResourceInfo* r) {
  const std::string abs_path = file::JoinPathRespectAbsolute(cwd, path);
  FileStat file_stat(abs_path);
  if (!file_stat.IsValid()) {
    return false;
  }
  r->set_name(path);
  r->set_type(type);

#ifndef _WIN32
  // Support symlink in non Windows env.
  struct stat st;
  if (lstat(abs_path.c_str(), &st) < 0) {
    PLOG(WARNING) << "failed to lstat: " << abs_path;
    return false;
  }
  if (S_ISLNK(st.st_mode)) {
    auto symlink_path(absl::make_unique<char[]>(st.st_size + 1));
    ssize_t size =
        readlink(abs_path.c_str(), symlink_path.get(), st.st_size + 1);
    if (size < 0) {
      // failed to read symlink
      PLOG(WARNING) << "failed readlink: " << abs_path;
      return false;
    }
    if (size != st.st_size) {
      PLOG(WARNING) << "unexpected symlink size: path=" << abs_path
                    << " actual=" << size << " expected=" << st.st_size;
      return false;
    }
    symlink_path[size] = '\0';
    r->set_symlink_path(symlink_path.get());
    return true;
  }
#endif

  std::string hash;
  if (!SHA256HashCache::instance()->GetHashFromCacheOrFile(abs_path, &hash)) {
    return false;
  }
  r->set_hash(std::move(hash));
  SetFileStatToData(file_stat, r->mutable_file_stat());
  r->set_is_executable(access(abs_path.c_str(), X_OK) == 0);

  return true;
}

bool CompilerInfoBuilder::AddResourceAsExecutableBinary(
    const std::string& resource_path,
    const std::string& cwd,
    absl::flat_hash_set<std::string>* visited_paths,
    CompilerInfoData* data) {
  // On Linux, MAX_NESTED_LINKS is 8.
  constexpr int kMaxNestedLinks = 8;
  return AddResourceAsExecutableBinaryInternal(
      resource_path, cwd, kMaxNestedLinks, visited_paths, data);
}

}  // namespace devtools_goma
