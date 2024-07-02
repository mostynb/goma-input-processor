// Copyright 2010 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#include "compiler_info.h"

#include "absl/strings/str_cat.h"
#include "autolock_timer.h"
#include "compiler_info_builder.h"
#include "gcc_flags.h"
#include "glog/logging.h"
#include "path.h"
#include "path_util.h"
#include "sha256_hash_cache.h"

#ifndef _WIN32
#include <errno.h>
#include <string.h>
#endif

namespace devtools_goma {

/* static */
void CompilerInfo::SubprogramInfo::FromData(
    const CompilerInfoData::SubprogramInfo& info_data,
    SubprogramInfo* info) {
  info->abs_path = info_data.abs_path();
  info->user_specified_path = info_data.user_specified_path();
  info->hash = info_data.hash();
  if (info_data.has_file_stat()) {
    GetFileStatFromData(info_data.file_stat(), &info->file_stat);
  }
}

std::string CompilerInfo::SubprogramInfo::DebugString() const {
  std::stringstream ss;
  ss << "abs_path: " << abs_path;
  ss << ", user_specified_path: " << user_specified_path;
  ss << ", valid: " << file_stat.IsValid();
  ss << ", hash: " << hash;
  return ss.str();
}

/* static */
CompilerInfo::ResourceInfo CompilerInfo::ResourceInfo::FromData(
    const CompilerInfoData::ResourceInfo& info_data) {
  ResourceInfo info;
  info.name = info_data.name();
  info.type = info_data.type();
  info.hash = info_data.hash();
  if (info_data.has_file_stat()) {
    GetFileStatFromData(info_data.file_stat(), &info.file_stat);
  }
  info.is_executable = info_data.is_executable();
  info.symlink_path = info_data.symlink_path();
  return info;
}

std::string CompilerInfo::ResourceInfo::DebugString() const {
  return absl::StrCat("name: ", name, ", type: ", type,
                      ", valid:", file_stat.IsValid(), ", hash: ", hash,
                      ", is_executable: ", is_executable,
                      ", symlink_path: ", symlink_path);
}

bool CompilerInfo::ResourceInfo::IsUpToDate(const std::string& cwd,
                                            std::string* reason) const {
  if (!symlink_path.empty()) {
    // symlink case.
#ifdef _WIN32
    // symlink is not supported on Windows.
    return false;
#else
    std::string abs_path = file::JoinPathRespectAbsolute(cwd, name);
    struct stat st;
    if (lstat(abs_path.c_str(), &st) < 0) {
      *reason = absl::StrCat("lstat failed: path=", abs_path,
                             " error=", strerror(errno));
      return false;
    }
    if (!S_ISLNK(st.st_mode)) {
      *reason = absl::StrCat("file is not symlink: path=", abs_path);
      return false;
    }

    auto new_symlink_path(absl::make_unique<char[]>(st.st_size + 1));
    ssize_t size =
        readlink(abs_path.c_str(), new_symlink_path.get(), st.st_size + 1);
    if (size < 0) {
      *reason = absl::StrCat("readlink failed: path=", abs_path,
                             " error=", strerror(errno));
      return false;
    }
    if (size != st.st_size) {
      *reason = absl::StrCat("unexpected symlink size: path=", abs_path,
                             " actual=", size, " expected=", st.st_size);
      return false;
    }
    new_symlink_path[size] = '\0';
    if (symlink_path != absl::string_view(new_symlink_path.get(), size)) {
      *reason =
          absl::StrCat("symlink outdate: path=", abs_path,
                       " old=", symlink_path, " new=", new_symlink_path.get());
      return false;
    }

    return true;
#endif
  }

  // Non symlink case. Check FileStat is not changed.
  FileStat new_file_stat(file::JoinPathRespectAbsolute(cwd, name));
  if (new_file_stat != this->file_stat) {
    *reason = absl::StrCat("file stat does not match",
                           " old=", file_stat.DebugString(),
                           " new=", new_file_stat.DebugString());
    return false;
  }

  return true;
}

std::string CompilerInfo::DebugString() const {
  return data_->DebugString();
}

CompilerInfo::CompilerInfo(std::unique_ptr<CompilerInfoData> data)
    : data_(std::move(data)) {
  CHECK(data_.get());
  if (data_->has_local_compiler_stat()) {
    GetFileStatFromData(data_->local_compiler_stat(), &local_compiler_stat_);
  }
  if (data_->has_real_compiler_stat()) {
    GetFileStatFromData(data_->real_compiler_stat(), &real_compiler_stat_);
  }

  for (const auto& f : data_->additional_flags()) {
    additional_flags_.push_back(f);
  }

  for (const auto& data : data_->subprograms()) {
    SubprogramInfo s;
    SubprogramInfo::FromData(data, &s);
    subprograms_.push_back(s);
  }

  for (const auto& data : data_->resource()) {
    resource_.emplace_back(ResourceInfo::FromData(data));
  }

  for (const auto& d : data_->dimensions()) {
    dimensions_.push_back(d);
  }
}

bool CompilerInfo::IsUpToDate(
    const std::string& abs_local_compiler_path) const {
  FileStat cur_local(abs_local_compiler_path);
  if (cur_local != local_compiler_stat_) {
    LOG(INFO) << "compiler id is not matched:"
              << " path=" << abs_local_compiler_path
              << " local_compiler_stat=" << local_compiler_stat_.DebugString()
              << " cur_local=" << cur_local.DebugString();
    return false;
  }
  if (abs_local_compiler_path != abs_real_compiler_path()) {
    // Since |abs_local_compiler_path| != |abs_real_compiler_path|,
    // We need to check that the real compiler is also the same.
    FileStat cur_real(abs_real_compiler_path());
    if (cur_real != real_compiler_stat_) {
      LOG(INFO) << "real compiler id is not matched:"
                << " abs_local_compiler_path=" << abs_local_compiler_path
                << " abs_real_compiler_path=" << abs_real_compiler_path()
                << " local_compiler_stat=" << local_compiler_stat_.DebugString()
                << " real_compiler_stat=" << real_compiler_stat_.DebugString()
                << " cur_real=" << cur_real.DebugString();
      return false;
    }
  }

  for (const auto& subprog : subprograms_) {
    FileStat file_stat(subprog.abs_path);
    if (file_stat != subprog.file_stat) {
      LOG(INFO) << "subprogram is not matched:"
                << " abs_local_compiler_path=" << abs_local_compiler_path
                << " subprogram=" << subprog.abs_path
                << " subprogram_file_stat=" << subprog.file_stat.DebugString()
                << " file_stat=" << file_stat.DebugString();
      return false;
    }
  }

  for (const auto& r : resource_) {
    std::string error_reason;
    if (!r.IsUpToDate(data_->cwd(), &error_reason)) {
      LOG(INFO) << "resource file is not matched:"
                << " abs_local_compiler_path=" << abs_local_compiler_path << " "
                << error_reason;
      return false;
    }
  }

  return true;
}

bool CompilerInfo::UpdateFileStatIfHashMatch() {
  // Checks real compiler hash and subprogram hash.
  // If they are all matched, we update FileStat.

  SHA256HashCache* sha256_cache = SHA256HashCache::instance();

  std::string local_hash;
  if (!sha256_cache->GetHashFromCacheOrFile(abs_local_compiler_path(),
                                            &local_hash)) {
    LOG(WARNING) << "calculating local compiler hash failed: "
                 << "path=" << abs_local_compiler_path();
    return false;
  }
  if (local_hash != local_compiler_hash()) {
    LOG(INFO) << "local compiler hash didn't match:"
              << " path=" << abs_local_compiler_path()
              << " prev=" << local_compiler_hash() << " current=" << local_hash;
    return false;
  }

  std::string real_hash;
  if (!sha256_cache->GetHashFromCacheOrFile(abs_real_compiler_path(),
                                            &real_hash)) {
    LOG(WARNING) << "calculating real compiler hash failed: "
                 << "path=" << abs_real_compiler_path();
    return false;
  }
  if (real_hash != real_compiler_hash()) {
    LOG(INFO) << "real compiler hash didn't match:"
              << " path=" << abs_real_compiler_path()
              << " prev=" << real_compiler_hash() << " current=" << real_hash;
    return false;
  }

  for (const auto& subprog : subprograms_) {
    std::string subprogram_hash;
    if (!sha256_cache->GetHashFromCacheOrFile(subprog.abs_path,
                                              &subprogram_hash)) {
      LOG(WARNING) << "calculating subprogram hash failed: "
                   << "abs_path=" << subprog.abs_path;
      return false;
    }
    if (subprogram_hash != subprog.hash) {
      LOG(INFO) << "subprogram hash didn't match:"
                << " path=" << abs_real_compiler_path()
                << " subprogram=" << subprog.abs_path
                << " prev=" << subprog.hash << " current=" << subprogram_hash;
      return false;
    }
  }

  if (subprograms().size() !=
      static_cast<size_t>(data_->subprograms().size())) {
    LOG(ERROR) << "CompilerInfo subprograms and data subprograms size differs: "
               << " Inconsistent state: " << abs_real_compiler_path();
    return false;
  }

  for (size_t i = 0; i < subprograms_.size(); ++i) {
    const auto& subprog = subprograms_[i];
    const auto& data_subprog = data_->subprograms(i);
    if (subprog.user_specified_path != data_subprog.user_specified_path() ||
        subprog.abs_path != data_subprog.abs_path()) {
      LOG(ERROR) << "CompilerInfo subprogram and its data subprograms"
                 << " is inconsistent: compiler=" << abs_real_compiler_path()
                 << " inconsistent subprogram: "
                 << " user_specified_path: " << subprog.user_specified_path
                 << " vs " << data_subprog.user_specified_path()
                 << " abs_path: " << subprog.abs_path << " vs "
                 << data_subprog.abs_path();
      return false;
    }
  }

  for (const auto& r : resource_) {
    // Resource can be a symlink. In that case, we compare a symlink instead of
    // hash.
    if (!r.symlink_path.empty()) {
#ifdef _WIN32
      LOG(ERROR) << "CompilerInfo resource contains a symlink, however it is "
                    "not supported on Windows: "
                 << r.name;
      return false;
#else
      std::string abs_path =
          file::JoinPathRespectAbsolute(data_->cwd(), r.name);
      struct stat st;
      if (lstat(abs_path.c_str(), &st) < 0) {
        LOG(ERROR) << "CompilerInfo resource: not found: " << abs_path;
        return false;
      }
      if (!S_ISLNK(st.st_mode)) {
        LOG(ERROR) << "CompilerInfo resource: expected a symlink, but not: "
                   << abs_path;
        return false;
      }

      auto symlink_path(absl::make_unique<char[]>(st.st_size + 1));
      ssize_t size =
          readlink(abs_path.c_str(), symlink_path.get(), st.st_size + 1);
      if (size < 0) {
        PLOG(ERROR) << "CompilerInfo resource: readlink failed: path="
                    << abs_path;
        return false;
      }
      if (size != st.st_size) {
        LOG(ERROR) << "CompilerInfo resource: unexpected symlink size"
                   << " path=" << abs_path << " actual=" << size
                   << " expected=" << st.st_size;
        return false;
      }
      symlink_path[size] = '\0';
      const auto symlink_path_view =
          absl::string_view(symlink_path.get(), size);
      if (r.symlink_path != symlink_path_view) {
        LOG(ERROR) << "CompilerInfo symlink resource doesn't match:"
                   << " name=" << r.name << " symlink_path: " << r.symlink_path
                   << " vs " << symlink_path_view;
        return false;
      }

      continue;
#endif
    }

    std::string r_hash;
    if (!sha256_cache->GetHashFromCacheOrFile(
        file::JoinPathRespectAbsolute(data_->cwd(), r.name), &r_hash)) {
      LOG(WARNING) << "calculating file hash failed: "
                   << "name=" << r.name;
      return false;
    }
    if (r_hash != r.hash) {
      LOG(INFO) << "file hash didn't match:"
                << " path=" << abs_real_compiler_path() << " name=" << r.name
                << " prev=" << r.hash << " current=" << r_hash;
      return false;
    }
  }

  if (resource_.size() !=
      static_cast<size_t>(data_->resource().size())) {
    LOG(ERROR) << "CompilerInfo resource and data resource size differs: "
               << " Inconsistent state: " << abs_real_compiler_path();
    return false;
  }

  for (size_t i = 0; i < resource_.size(); ++i) {
    const auto& r = resource_[i];
    const auto& data_r = data_->resource(i);
    if (r.name != data_r.name()) {
      LOG(ERROR) << "CompilerInfo resource and its data resource"
                 << " is inconsistent: compiler=" << abs_real_compiler_path()
                 << " inconsistent resource: " << r.name
                 << " != " << data_r.name();
      return false;
    }
  }

  // OK. all hash matched. Let's update FileStat.

  FileStat cur_local(local_compiler_path());
  if (cur_local != local_compiler_stat_) {
    LOG(INFO) << "local_compiler_stat_ is updated:"
              << " old=" << local_compiler_stat_.DebugString()
              << " new=" << cur_local.DebugString();
    local_compiler_stat_ = cur_local;
    SetFileStatToData(cur_local, data_->mutable_local_compiler_stat());
  }

  // When |abs_local_compiler_path| == |abs_real_compiler_path|,
  // local_compiler_stat and real_compiler_stat should be the same.
  // Otherwise, we take FileStat for abs_real_compiler_path().
  FileStat cur_real(cur_local);
  if (abs_local_compiler_path() != abs_real_compiler_path()) {
    cur_real = FileStat(abs_real_compiler_path());
  }
  if (cur_real != real_compiler_stat_) {
    LOG(INFO) << "real_compiler_stat_ is updated:"
              << " old=" << real_compiler_stat_.DebugString()
              << " new=" << cur_real.DebugString();
    real_compiler_stat_ = cur_real;
    SetFileStatToData(cur_real, data_->mutable_real_compiler_stat());
  }

  for (size_t i = 0; i < subprograms_.size(); ++i) {
    auto& subprog = subprograms_[i];
    auto* data_subprog = data_->mutable_subprograms(i);

    FileStat file_stat(subprog.abs_path);
    if (file_stat != subprog.file_stat) {
      LOG(INFO) << "subprogram id is updated:"
                << " abs_path=" << subprog.abs_path
                << " old=" << subprog.file_stat.DebugString()
                << " new=" << file_stat.DebugString();
      subprog.file_stat = file_stat;
      SetFileStatToData(file_stat, data_subprog->mutable_file_stat());
    }
  }

  for (size_t i = 0; i < resource_.size(); ++i) {
    auto& resource = resource_[i];
    auto* data_resource = data_->mutable_resource(i);

    if (!resource.symlink_path.empty()) {
      // symlink resource won't have valid file_stat.
      // Since we have checked the content of sysmlink is not changed before,
      // we don't need to update FileStat.
      continue;
    }

    std::string abs_path =
        file::JoinPathRespectAbsolute(data_->cwd(), resource.name);
    FileStat file_stat(abs_path);
    if (file_stat != resource.file_stat) {
      LOG(INFO) << "resource is updated:"
                << " abs_path=" << abs_path
                << " old=" << resource.file_stat.DebugString()
                << " new=" << file_stat.DebugString();
      resource.file_stat = file_stat;
      SetFileStatToData(file_stat, data_resource->mutable_file_stat());
    }
  }

  return true;
}

bool CompilerInfo::DependsOnCwd(const std::string& cwd) const {
  if (!data_->real_compiler_path().empty()) {
    if (!file::IsAbsolutePath(data_->real_compiler_path()) ||
        HasPrefixDir(data_->real_compiler_path(), cwd)) {
      VLOG(1) << "real_compiler_path is cwd relative:"
              << data_->real_compiler_path() << " @" << cwd;
      return true;
    }
  }
  for (size_t i = 0; i < subprograms_.size(); ++i) {
    // user_specified_path can be absolute (if specified so).
    const std::string& user_specified_path =
        subprograms_[i].user_specified_path;
    if (!file::IsAbsolutePath(user_specified_path) ||
        HasPrefixDir(user_specified_path, cwd)) {
      VLOG(1) << "subprograms[" << i
              << "] is cwd relative: " << user_specified_path << " @" << cwd;
      return true;
    }
  }
  for (size_t i = 0; i < resource_.size(); ++i) {
    if (!file::IsAbsolutePath(resource_[i].name) ||
        HasPrefixDir(resource_[i].name, cwd)) {
      VLOG(1) << "resource[" << i << "].name is cwd relative:"
              << resource_[i].name << " @" << cwd;
      return true;
    }
  }

  return false;
}

std::string CompilerInfo::abs_local_compiler_path() const {
  return file::JoinPathRespectAbsolute(
      data_->cwd(), data_->local_compiler_path());
}

std::string CompilerInfo::abs_real_compiler_path() const {
  return file::JoinPathRespectAbsolute(data_->cwd(),
                                       data_->real_compiler_path());
}

const std::string& CompilerInfo::request_compiler_hash() const {
  if (GCCFlags::IsPNaClClangCommand(data_->local_compiler_path())) {
    return data_->local_compiler_hash();
  }
  return data_->hash();
}

absl::Time CompilerInfo::last_used_at() const {
  AUTO_SHARED_LOCK(lock, &last_used_at_mu_);
  return absl::FromTimeT(data_->last_used_at());
}

void CompilerInfo::set_last_used_at(absl::Time time) {
  AUTO_EXCLUSIVE_LOCK(lock, &last_used_at_mu_);
  data_->set_last_used_at(absl::ToTimeT(time));
}

}  // namespace devtools_goma
