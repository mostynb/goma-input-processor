// Copyright 2011 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef DEVTOOLS_GOMA_CLIENT_JAVA_JAR_PARSER_H_
#define DEVTOOLS_GOMA_CLIENT_JAVA_JAR_PARSER_H_

#include <set>
#include <string>
#include <vector>

namespace devtools_goma {

class JarParser {
 public:
  JarParser();

  // Reads |input_jar_files| and push required jar files into |jar_files|.
  // Note that we will not add non-existing .jar files to |jar_files| even
  // if they are listed in class path of MANIFEST files.
  // TODO: We may want to return additional class pathes as well.
  void GetJarFiles(const std::vector<std::string>& input_jar_files,
                   const std::string& cwd,
                   std::set<std::string>* jar_files);
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_JAVA_JAR_PARSER_H_
