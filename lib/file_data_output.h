// Copyright 2019 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVTOOLS_GOMA_LIB_FILE_DATA_OUTPUT_H_
#define DEVTOOLS_GOMA_LIB_FILE_DATA_OUTPUT_H_

#include <sys/types.h>

#include <memory>
#include <string>

namespace devtools_goma {

// TODO: provide Input too.
// An abstract interface of output destination for receiving output file data.
class FileDataOutput {
 public:
  FileDataOutput() {}
  virtual ~FileDataOutput() {}

  FileDataOutput(const FileDataOutput&) = delete;
  FileDataOutput& operator=(const FileDataOutput&) = delete;

  // IsValid returns true if this output is valid to use.
  virtual bool IsValid() const = 0;
  // WriteAt writes content at offset in output.
  virtual bool WriteAt(off_t offset, const std::string& content) = 0;
  // Close closes the output.
  virtual bool Close() = 0;
  // ToString returns string representation of this output. e.g. filename.
  virtual std::string ToString() const = 0;

  // NewFileOutput returns Output for filename.
  static std::unique_ptr<FileDataOutput> NewFileOutput(
      const std::string& filename,
      int mode);

  // NewStringOutput returns Output into buf.
  // It doesn't take ownership of buf.
  // *buf will have output size when Close().
  // Note that, unlike sparse file in unix, it will not modify data in a hole,
  // if the hole exists. This class won't create any sparse file, so may not
  // need to worry about this.
  // If you care, pass empty buf (StringOutput will
  // allocate enough space), or zero-cleared preallocated buf.
  static std::unique_ptr<FileDataOutput> NewStringOutput(
      const std::string& name,
      std::string* buf);
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_LIB_FILE_DATA_OUTPUT_H_
