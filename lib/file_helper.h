// Copyright 2011 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef DEVTOOLS_GOMA_LIB_FILE_HELPER_H_
#define DEVTOOLS_GOMA_LIB_FILE_HELPER_H_

#include <string>

#include "absl/strings/string_view.h"

namespace devtools_goma {

bool ReadFileToString(absl::string_view filename, std::string* OUTPUT);
bool WriteStringToFile(absl::string_view data, absl::string_view filename);

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_LIB_FILE_HELPER_H_
