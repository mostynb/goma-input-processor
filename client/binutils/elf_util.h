// Copyright 2019 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVTOOLS_GOMA_CLIENT_BINUTILS_ELF_UTIL_H_
#define DEVTOOLS_GOMA_CLIENT_BINUTILS_ELF_UTIL_H_

#include <string>
#include <vector>

#include "absl/strings/string_view.h"

namespace devtools_goma {

// Load library search paths from ld.so.conf.
// The returned value would be used by ElfDepParser.
std::vector<std::string> LoadLdSoConf(const absl::string_view filename);

// Returns true if |path| is in given |system_library_paths|.
// If |path| is not absolute path, it always returns false.
bool IsInSystemLibraryPath(
    const absl::string_view path,
    const std::vector<std::string>& system_library_paths);

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_BINUTILS_ELF_UTIL_H_
