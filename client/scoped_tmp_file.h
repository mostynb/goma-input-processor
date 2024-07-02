// Copyright 2014 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef DEVTOOLS_GOMA_CLIENT_SCOPED_TMP_FILE_H_
#define DEVTOOLS_GOMA_CLIENT_SCOPED_TMP_FILE_H_

#include <string>

#include "basictypes.h"
#include "scoped_fd.h"

namespace devtools_goma {

// A class to provide a temporary file available within the scope.
// The temporary file is created in constructor and deleted in destructor.
// The file descriptor is opened in constructor, and closed when the user call
// Close.  If Close is not called, it is automatically closed in destructor.
class ScopedTmpFile {
 public:
  explicit ScopedTmpFile(const std::string& prefix);
  // |extension| should starts with '.'. e.g. ".cc"
  ScopedTmpFile(const std::string& prefix, const std::string& extension);
  ~ScopedTmpFile();

  const std::string& filename() const { return filename_; }
  bool valid() const;
  ssize_t Write(const void* ptr, size_t len) const;
  bool Close();

 private:
  ScopedFd fd_;
  std::string filename_;

  DISALLOW_COPY_AND_ASSIGN(ScopedTmpFile);
};

// A class to provide a temporary directory available within the scope.
class ScopedTmpDir {
 public:
  explicit ScopedTmpDir(const std::string& prefix);
  ~ScopedTmpDir();

  ScopedTmpDir(ScopedTmpDir&&) = delete;
  ScopedTmpDir(const ScopedTmpDir&) = delete;
  ScopedTmpDir& operator=(const ScopedTmpDir&) = delete;
  ScopedTmpDir& operator=(ScopedTmpDir&&) = delete;

  const std::string& dirname() const { return dirname_; }
  bool valid() const { return !dirname_.empty(); }

 private:
  std::string dirname_;
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_SCOPED_TMP_FILE_H_
