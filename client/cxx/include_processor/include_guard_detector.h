// Copyright 2017 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVTOOLS_GOMA_CLIENT_CXX_INCLUDE_PROCESSOR_INCLUDE_GUARD_DETECTOR_H_
#define DEVTOOLS_GOMA_CLIENT_CXX_INCLUDE_PROCESSOR_INCLUDE_GUARD_DETECTOR_H_

#include <string>

#include "cpp_directive.h"

namespace devtools_goma {

class IncludeGuardDetector {
 public:
  IncludeGuardDetector(const IncludeGuardDetector&) = delete;
  void operator=(const IncludeGuardDetector&) = delete;

  static std::string Detect(const CppDirectiveList& directives);
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_CXX_INCLUDE_PROCESSOR_INCLUDE_GUARD_DETECTOR_H_
