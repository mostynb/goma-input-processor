// Copyright 2012 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#include "lib/goma_hash.h"

#include "gtest/gtest.h"

TEST(GomaHashTest, ComputeDataHashKey) {
  std::string md_str;
  devtools_goma::ComputeDataHashKey("", &md_str);
  EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            md_str);

  md_str.clear();
  devtools_goma::ComputeDataHashKey(
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n",
      &md_str);
  EXPECT_EQ("38acb15d02d5ac0f2a2789602e9df950c380d2799b4bdb59394e4eeabdd3a662",
            md_str);
}

TEST(GomaHashTest, SHA256HashValue) {
  std::string hex_string =
      "38acb15d02d5ac0f2a2789602e9df950c380d2799b4bdb59394e4eeabdd3a662";

  devtools_goma::SHA256HashValue hash_value;
  EXPECT_TRUE(devtools_goma::SHA256HashValue::ConvertFromHexString(
                  hex_string, &hash_value));
  EXPECT_EQ(hex_string, hash_value.ToHexString());
}

TEST(GomaHashTest, SHA256HashValueEmpty) {
  std::string hex_string;

  devtools_goma::SHA256HashValue hash_value;
  EXPECT_FALSE(devtools_goma::SHA256HashValue::ConvertFromHexString(
                   hex_string, &hash_value));
}

TEST(GomaHashTest, SHA256HashValueNonHex) {
  std::string hex_string =
      "XYacb15d02d5ac0f2a2789602e9df950c380d2799b4bdb59394e4eeabdd3a662";

  devtools_goma::SHA256HashValue hash_value;
  EXPECT_FALSE(devtools_goma::SHA256HashValue::ConvertFromHexString(
                   hex_string, &hash_value));
}
