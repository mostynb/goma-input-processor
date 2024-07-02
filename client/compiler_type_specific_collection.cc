// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compiler_type_specific_collection.h"

namespace devtools_goma {

// static
CompilerTypeSpecific* CompilerTypeSpecificCollection::Get(
    CompilerFlagType type) {
  switch (type) {
    case CompilerFlagType::Rustc:
      return &rustc_;
    case CompilerFlagType::Gcc:
      return &gcc_;
    case CompilerFlagType::Clexe:
      return &vc_;
    case CompilerFlagType::ClangTidy:
      return &clang_tidy_;
    case CompilerFlagType::Javac:
      return &javac_;
    case CompilerFlagType::Java:
      return &java_;
    case CompilerFlagType::DartAnalyzer:
      return &dart_analyzer_;
    case CompilerFlagType::Fake:
      return &fake_;
    case CompilerFlagType::Unknown:
    default:
      return nullptr;
  }
}

}  // namespace devtools_goma
