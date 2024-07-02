// Copyright 2012 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef DEVTOOLS_GOMA_BASE_FILE_DIR_H_
#define DEVTOOLS_GOMA_BASE_FILE_DIR_H_

#include <string>
#include <vector>

namespace devtools_goma {

struct DirEntry {
  std::string name;
  bool is_dir = false;
};

// Gets a entries of directory.
// If dirname does not exist, it returns false.
// If dirname is not a directory, it returns true, but *entries is empty.
// If dirname is a directory, it returns true, and *entries is filled with
// the directory's entries.
bool ListDirectory(const std::string& dirname, std::vector<DirEntry>* entries);

// Returns true if dirname is successfully deleted.
bool DeleteDirectory(const std::string& dirname);

// Ensure directory exists.
bool EnsureDirectory(const std::string& dirname, int mode);

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_BASE_FILE_DIR_H_
