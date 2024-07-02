// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVTOOLS_GOMA_CLIENT_CXX_INCLUDE_PROCESSOR_INCLUDE_ITEM_H_
#define DEVTOOLS_GOMA_CLIENT_CXX_INCLUDE_PROCESSOR_INCLUDE_ITEM_H_

#include <string>

#include "cpp_directive.h"

namespace devtools_goma {

class IncludeItem {
 public:
  IncludeItem() = default;
  IncludeItem(SharedCppDirectives directives, std::string include_guard_ident)
      : directives_(std::move(directives)),
        include_guard_ident_(std::move(include_guard_ident)) {}

  bool IsValid() const { return directives_.get() != nullptr; }

  const SharedCppDirectives& directives() const { return directives_; }
  const std::string& include_guard_ident() const {
    return include_guard_ident_;
  }

 private:
  SharedCppDirectives directives_;
  std::string include_guard_ident_;
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_CXX_INCLUDE_PROCESSOR_INCLUDE_ITEM_H_
