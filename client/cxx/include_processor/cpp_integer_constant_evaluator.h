// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVTOOLS_GOMA_CLIENT_CXX_INCLUDE_PROCESSOR_CPP_INTEGER_CONSTANT_EVALUATOR_H_
#define DEVTOOLS_GOMA_CLIENT_CXX_INCLUDE_PROCESSOR_CPP_INTEGER_CONSTANT_EVALUATOR_H_

#include "cpp_parser.h"
#include "cpp_token.h"

namespace devtools_goma {

class CppIntegerConstantEvaluator {
 public:
  CppIntegerConstantEvaluator(const ArrayTokenList& tokens, CppParser* parser);

  int64_t GetValue() { return Conditional().value; }

 private:
  CppToken::int_value Conditional();
  CppToken::int_value Expression(CppToken::int_value v1, int min_precedence);
  CppToken::int_value Primary();

  const ArrayTokenList& tokens_;
  ArrayTokenList::const_iterator iter_;
  CppParser* parser_;

  DISALLOW_COPY_AND_ASSIGN(CppIntegerConstantEvaluator);
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_CXX_INCLUDE_PROCESSOR_CPP_INTEGER_CONSTANT_EVALUATOR_H_
