// Copyright 2013 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef DEVTOOLS_GOMA_CLIENT_FILE_STAT_CACHE_H_
#define DEVTOOLS_GOMA_CLIENT_FILE_STAT_CACHE_H_

#include <sstream>
#include <string>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "basictypes.h"
#include "file_stat.h"
#include "lockhelper.h"
#include "platform_thread.h"

namespace devtools_goma {

// GlobalFileStatCache caches FileStats globally.
// This only holds valid and non-directory FileStats.
// The instance of this class is thread-safe.
class GlobalFileStatCache {
 public:
  FileStat Get(const std::string& path);

  static void Init();
  static void Quit();
  static GlobalFileStatCache* Instance();

 private:
  mutable ReadWriteLock mu_;
  absl::flat_hash_map<std::string, FileStat> file_stats_ ABSL_GUARDED_BY(mu_);

  static GlobalFileStatCache* instance_;
};

// FileStatCache caches FileStats.
// Instance of this class is thread-unsafe.
class FileStatCache {
 public:
  FileStatCache();
  ~FileStatCache();

  // Returns FileStat cache if any. If not, we create FileStat for |filename|.
  FileStat Get(const std::string& filename);

  // Clears all caches.
  void Clear();

  // Caller thread takes ownership of the instance of FileStatCache.
  void AcquireOwner();

  // Caller thread releases ownership of the instance of FileStatCache.
  void ReleaseOwner();

  friend class DepsCacheTest;

 private:
  typedef absl::flat_hash_map<std::string, FileStat> FileStatMap;

  bool is_acquired_;
  PlatformThreadId owner_thread_id_;

  FileStatMap file_stats_;

  DISALLOW_COPY_AND_ASSIGN(FileStatCache);
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_FILE_STAT_CACHE_H_
