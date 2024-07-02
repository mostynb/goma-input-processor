// Copyright 2017 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVTOOLS_GOMA_CLIENT_CXX_INCLUDE_PROCESSOR_CPP_TOKENIZER_H_
#define DEVTOOLS_GOMA_CLIENT_CXX_INCLUDE_PROCESSOR_CPP_TOKENIZER_H_

#include <string>

#include "cpp_input_stream.h"
#include "cpp_token.h"
#include "gtest/gtest_prod.h"
#include "space_handling.h"

namespace devtools_goma {

class CppTokenizer {
 public:
  CppTokenizer() = delete;
  CppTokenizer(const CppTokenizer&) = delete;
  void operator=(const CppTokenizer&) = delete;

  static bool NextTokenFrom(CppInputStream* stream,
                            SpaceHandling space_handling,
                            CppToken* token,
                            std::string* error_reason);

  // utility function to get all tokens
  // Returns true if succeeds, false otherwise.
  static bool TokenizeAll(const std::string& str,
                          SpaceHandling space_handling,
                          ArrayTokenList* result);

  // Reads string CppToken.
  static bool ReadString(CppInputStream* stream,
                         CppToken* result_token,
                         std::string* error_reason);
  // Reads character literal CppToken.
  static bool ReadCharLiteral(CppInputStream* stream,
                              CppToken* result_token);

  // Reads string until |delimiter|.
  // When error happened, error reason is set to |error_reason|, and false is
  // returned.
  static bool ReadStringUntilDelimiter(CppInputStream* stream,
                                       std::string* result,
                                       char delimiter,
                                       std::string* error_reason);

  static CppToken ReadIdentifier(CppInputStream* stream, const char* begin);
  static CppToken ReadNumber(CppInputStream* stream, int c0, const char* begin);

  // Handles line-folding with '\\', updates the token's string_value and
  // advances the begin pointer.
  // Returns true if it has consumed a line break.
  static bool HandleLineFoldingWithToken(CppInputStream* stream,
                                         CppToken* token, const char** begin);

  static bool SkipComment(CppInputStream* stream,
                          std::string* error_reason);
  static bool SkipUntilDirective(CppInputStream* stream,
                                 std::string* error_reason);
  static void SkipUntilLineBreakIgnoreComment(CppInputStream* stream);
  static bool IsAfterEndOfLine(const char* cur, const char* begin);

 private:
  static bool IsValidIntegerSuffix(const std::string& s);
  static bool IsUnsignedIntegerSuffix(const std::string& s);
  static CppToken::Type TypeFrom(int c1, int c2);

  FRIEND_TEST(CppTokenizerTest, IntegerSuffixes);
  FRIEND_TEST(CppTokenizerTest, IsAfterEndOfLine);
  FRIEND_TEST(CppTokenizerTest, TypeFrom);
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_CXX_INCLUDE_PROCESSOR_CPP_TOKENIZER_H_
