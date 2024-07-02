// Copyright 2011 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef DEVTOOLS_GOMA_CLIENT_CXX_INCLUDE_PROCESSOR_CPP_PARSER_H_
#define DEVTOOLS_GOMA_CLIENT_CXX_INCLUDE_PROCESSOR_CPP_PARSER_H_

#include <bitset>
#include <list>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "absl/base/call_once.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/string_view.h"
#include "autolock_timer.h"
#include "basictypes.h"
#include "cpp_directive.h"
#include "cpp_input.h"
#include "cpp_macro.h"
#include "cpp_macro_env.h"
#include "cpp_macro_expander.h"
#include "cpp_macro_set.h"
#include "cpp_token.h"
#include "cxx/cxx_compiler_info.h"
#include "glog/logging.h"
#include "gtest/gtest_prod.h"
#include "platform_thread.h"
#include "predefined_macros.h"

#ifdef _WIN32
# include "config_win.h"
#endif

namespace devtools_goma {

class Content;
class CppInputStream;

// CppParser is thread-unsafe.
// TODO: Add unittest for this class.
class CppParser {
 public:
  class IncludeObserver {
   public:
    virtual ~IncludeObserver() {}

    // Handles include directive that CppParser processes.
    // Returns true if the include file is found (or it was already processed).
    // Returns false if the include file was not found and failed to process
    // the include directive.
    virtual bool HandleInclude(const std::string& path,
                               const std::string& current_directory,
                               const std::string& current_filepath,
                               char quote_char,  // '"' or '<'
                               int include_dir_index) = 0;

    // Handles __has_include() macro.
    // Returns value of __has_include().
    virtual bool HasInclude(const std::string& path,
                            const std::string& current_directory,
                            const std::string& current_filepath,
                            char quote_char,  // '"' or '<'
                            int include_dir_index) = 0;
  };
  class ErrorObserver {
   public:
    virtual ~ErrorObserver() {}
    virtual void HandleError(const std::string& error) = 0;
  };

  using Token = CppToken;
  using Input = CppInput;

  CppParser();
  ~CppParser();

  void set_bracket_include_dir_index(int index) {
    bracket_include_dir_index_ = index;
  }
  void set_include_observer(IncludeObserver* obs) { include_observer_ = obs; }
  void set_error_observer(ErrorObserver* obs) { error_observer_ = obs; }
  void SetCompilerInfo(const CxxCompilerInfo* compiler_info);

  void set_is_vc() { is_vc_ = true; }
  bool is_vc() const { return is_vc_; }
  void set_is_cplusplus(bool is_cplusplus) { is_cplusplus_ = is_cplusplus; }
  bool is_cplusplus() const { return is_cplusplus_; }

  // Parses and processes directives only.
  // Returns false if it failed to process and is pretty sure it missed some
  // input files.
  bool ProcessDirectives();

  const CppDirective* NextDirective();

  // Macro dictionary helpers.
  void AddMacroByString(const std::string& name, const std::string& body);
  void AddMacro(const Macro* macro);
  void DeleteMacro(const std::string& name);
  const Macro* GetMacro(const std::string& name);
  bool IsMacroDefined(const std::string& name);
  // For testing purpose
  bool EnablePredefinedMacro(const std::string& name, bool is_hidden);

  void ClearBaseFile() { base_file_.clear(); }

  void AddStringInput(const std::string& content, const std::string& pathname);
  void AddPreparsedDirectivesInput(SharedCppDirectives directives);
  void AddPredefinedMacros(const CxxCompilerInfo& compiler_info);

  // Adds |content| of |path|, which exists in |directory|.
  // |include_dir_index| is an index of a list of include dirs.
  void AddFileInput(IncludeItem include_item,
                    const std::string& path,
                    const std::string& directory,
                    int include_dir_index);

  // Returns true if the parser has already processed the |path|
  // and the set of macros that the file depends on have not changed.
  bool IsProcessedFile(const std::string& filepath, int include_dir_index) {
    ++total_files_;
    if (!IsProcessedFileInternal(filepath, include_dir_index))
      return false;
    ++skipped_files_;
    return true;
  }

  bool disabled() const { return disabled_; }
  void ClearDisabled() { disabled_ = false; }

  int total_files() const { return total_files_; }
  int skipped_files() const { return skipped_files_; }

  // For debug.
  std::string DumpMacros();

  void Error(absl::string_view error);
  void Error(absl::string_view error, absl::string_view arg);
  std::string DebugStringPrefix();

  // include_dir_index for the current directory, which is not specified by -I.
  // This is mainly used for the source file, or header files included by
  // #include "somewhere.h"
  static const int kCurrentDirIncludeDirIndex = 0;
  // include_dir_index will start from this value
  // for include directories specified by -iquote, -I, -isystem etc.
  // -iquote range [kIncludeDirIndexStarting, bracket_include_dir_index_).
  // others [bracket_include_dir_index_, ...).
  // in other words,
  //  #include "..." search starts from kIncludeDirIndexStarting.
  //    kCurrentDirIncludeDirIndex is special for current dir.
  //    directories specified by option are from kIncludeDirIndexStarting.
  //  #include <...> search starts from bracket_include_dir_index_.
  static const int kIncludeDirIndexStarting = 1;
 private:
  FRIEND_TEST(CppParserTest, DateTimeToken);

  // Manage files having #pragma once.
  class PragmaOnceFileSet {
   public:
    void Insert(const std::string& file);
    bool Has(const std::string& file) const;

   private:
    absl::flat_hash_set<std::string> files_;
  };

  struct Condition {
    explicit Condition(bool cond) : cond(cond), taken(cond) {}
    bool cond;
    bool taken;
  };

  void SetTarget(absl::string_view target);

  bool IsProcessedFileInternal(const std::string& filepath,
                               int include_dir_index);

  void ProcessDirective(const CppDirective&);
  void ProcessDirectiveInFalseCondition(const CppDirective&);

  void ProcessInclude(const CppDirectiveInclude&);
  void ProcessImport(const CppDirectiveImport&);
  void ProcessIncludeNext(const CppDirectiveIncludeNext&);
  void ProcessDefine(const CppDirectiveDefine&);
  void ProcessUndef(const CppDirectiveUndef&);
  void ProcessIfdef(const CppDirectiveIfdef&);
  void ProcessIfndef(const CppDirectiveIfndef&);
  void ProcessIf(const CppDirectiveIf&);
  void ProcessElse(const CppDirectiveElse&);
  void ProcessEndif(const CppDirectiveEndif&);
  void ProcessElif(const CppDirectiveElif&);
  void ProcessPragma(const CppDirectivePragma&);
  void ProcessError(const CppDirectiveError&);

  void ProcessIncludeInternal(const CppDirectiveIncludeBase&);
  void ProcessConditionInFalse(const CppDirective&);

  void EvalFunctionMacro(const std::string& name);
  int64_t EvalCondition(const ArrayTokenList& orig_tokens);
  // Detects include guard from #if condition.
  std::string DetectIncludeGuard(const ArrayTokenList& orig_tokens);

  Input* input() const {
    if (HasMoreInput()) {
      return inputs_.back().get();
    }
    return last_input_.get();
  }

  bool HasMoreInput() const {
    return !inputs_.empty();
  }
  void PopInput();

  bool CurrentCondition() const {
    return conditions_.empty() || conditions_.back().cond;
  }

  // Predefined macro callbacks.
  Token GetFileName();
  Token GetLineNumber();
  Token GetDate();
  Token GetTime();
  Token GetCounter();
  Token GetBaseFile();

  Token ProcessHasInclude(const ArrayTokenList& tokens);
  Token ProcessHasIncludeNext(const ArrayTokenList& tokens);
  bool ProcessHasIncludeInternal(const ArrayTokenList& tokens,
                                 bool is_include_next);

  Token ProcessHasFeature(const ArrayTokenList& tokens) {
    if (!compiler_info_) {
      VLOG(1) << DebugStringPrefix() << " CompilerInfo is not set.";
      return Token(0);
    }
    return ProcessHasCheckMacro("__has_feature", tokens,
                                compiler_info_->has_feature());
  }
  Token ProcessHasExtension(const ArrayTokenList& tokens) {
    if (!compiler_info_) {
      VLOG(1) << DebugStringPrefix() << " CompilerInfo is not set.";
      return Token(0);
    }
    Token token = ProcessHasCheckMacro("__has_extension", tokens,
                                       compiler_info_->has_extension());
    if (token == Token(0)) {
      // https://github.com/llvm/llvm-project/blob/5c352e69e76a26e4eda075e20aa6a9bb7686042c/clang/docs/LanguageExtensions.rst#__has_feature-and-__has_extensio
      // // __has_extension is superset of __has_feature.
      // __has_feature is 1 if supported both by Clang and standard.
      // __has_extension is 1 if supported by Clang

      // ignore error here to avoid double reports by
      // has_extension and has_feature.
      // error is already reported when checking with has_extension.
      ErrorObserver* obs = error_observer_;
      error_observer_ = nullptr;
      token = ProcessHasCheckMacro("__has_extension", tokens,
                                   compiler_info_->has_feature());
      error_observer_ = obs;
    }
    return token;
  }
  Token ProcessHasAttribute(const ArrayTokenList& tokens) {
    if (!compiler_info_) {
      VLOG(1) << DebugStringPrefix() << " CompilerInfo is not set.";
      return Token(0);
    }
    return ProcessHasCheckMacro("__has_attribute", tokens,
                                compiler_info_->has_attribute());
  }
  Token ProcessHasCppAttribute(const ArrayTokenList& tokens) {
    if (!compiler_info_) {
      VLOG(1) << DebugStringPrefix() << " CompilerInfo is not set.";
      return Token(0);
    }
    return ProcessHasCheckMacro("__has_cpp_attribute", tokens,
                                compiler_info_->has_cpp_attribute());
  }
  Token ProcessHasDeclspecAttribute(const ArrayTokenList& tokens) {
    if (!compiler_info_) {
      VLOG(1) << DebugStringPrefix() << " CompilerInfo is not set.";
      return Token(0);
    }
    return ProcessHasCheckMacro("__has_declspec_attribute", tokens,
                                compiler_info_->has_declspec_attribute());
  }
  Token ProcessHasBuiltin(const ArrayTokenList& tokens) {
    if (!compiler_info_) {
      VLOG(1) << DebugStringPrefix() << " CompilerInfo is not set.";
      return Token(0);
    }
    return ProcessHasCheckMacro("__has_builtin", tokens,
                                compiler_info_->has_builtin());
  }
  Token ProcessHasWarning(const ArrayTokenList& tokens) {
    if (!compiler_info_) {
      VLOG(1) << DebugStringPrefix() << " CompilerInfo is not set.";
      return Token(0);
    }
    return ProcessHasCheckMacro("__has_warning", tokens,
                                compiler_info_->has_warning());
  }
  Token ProcessHasCheckMacro(
      const std::string& name,
      const ArrayTokenList& tokens,
      const absl::flat_hash_map<std::string, int>& has_check_macro);

  Token ProcessIsTargetArch(const ArrayTokenList& tokens);
  Token ProcessIsTargetVendor(const ArrayTokenList& tokens);
  Token ProcessIsTargetOS(const ArrayTokenList& tokens);
  Token ProcessIsTargetEnvironment(const ArrayTokenList& tokens);
  Token ProcessIsTarget(const std::string& name,
                        const ArrayTokenList& tokens,
                        const std::string& target_part);
  Token MacroParamToken(const ArrayTokenList& tokens);

  // Initializes tables etc.
  static void EnsureInitialize();
  static void InitializeStaticOnce();

  std::vector<std::unique_ptr<Input>> inputs_;
  std::unique_ptr<Input> last_input_;

  // All used CppDirectiveList is preserved here to ensure Macro is alive.
  // All macro implementation should be alive in |input_protects_.|
  std::vector<SharedCppDirectives> input_protects_;
  CppMacroEnv macro_env_;

  std::vector<Condition> conditions_;
  int condition_in_false_depth_;

  PragmaOnceFileSet pragma_once_fileset_;

  std::string current_date_;
  std::string current_time_;
  std::string base_file_;
  int counter_;

  bool is_cplusplus_;

  int bracket_include_dir_index_;
  IncludeObserver* include_observer_;
  ErrorObserver* error_observer_;

  // When include guard macro is detected, the token is preserved here.
  absl::flat_hash_map<std::string, std::string> include_guard_ident_;

  struct TargetTriples {
    std::string arch;
    std::string vendor;
    std::string os;
    std::string environment;
  };
  TargetTriples target_;
  const CxxCompilerInfo* compiler_info_;
  bool is_vc_;

  // disabled_ becomes true if it detects unsupported features and is
  // pretty sure it couldn't pass necessary files to IncludeObserver.
  // b/9286087
  bool disabled_;

  // For statistics.
  int skipped_files_;
  int total_files_;

  PlatformThreadId owner_thread_id_;

  // Holds (name, Macro*).
  // The same name macro might be registered twice.
  using PredefinedMacros =
      std::vector<std::pair<std::string, std::unique_ptr<Macro>>>;
  static PredefinedMacros* predefined_macros_;

  static bool global_initialized_;

  friend class CppMacroExpanderFast;
  friend class CppMacroExpanderPrecise;
  friend class CppParserTest;
  DISALLOW_COPY_AND_ASSIGN(CppParser);
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_CXX_INCLUDE_PROCESSOR_CPP_PARSER_H_
