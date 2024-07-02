// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cpp_macro_expander_cbv.h"

namespace devtools_goma {

bool CppMacroExpanderCBV::ExpandMacro(const ArrayTokenList& input,
                                      SpaceHandling space_handling,
                                      ArrayTokenList* output) {
  output->reserve(32);
  return Expand(input.begin(), input.end(), space_handling, MacroSet(), Env(),
                output);
}

bool CppMacroExpanderCBV::Expand(ArrayTokenList::const_iterator input_begin,
                                 ArrayTokenList::const_iterator input_end,
                                 SpaceHandling space_handling,
                                 const MacroSet& hideset,
                                 const Env& env,
                                 ArrayTokenList* output) {
  for (auto it = input_begin; it != input_end; ++it) {
    const CppToken& token = *it;

    if (token.type == CppToken::SPACE) {
      if (space_handling == SpaceHandling::kKeep) {
        output->push_back(token);
      }
      continue;
    }

    if (token.type == CppToken::MACRO_PARAM) {
      if (token.v.param_index >= env.size()) {
        LOG(WARNING) << "macro index wrong? param_index=" << token.v.param_index
                     << " env.size=" << env.size() << " token=" << token;
        return false;
      }
      output->insert(output->end(),
                     env[token.v.param_index].begin(),
                     env[token.v.param_index].end());
      continue;
    }

    // We don't support these.
    if (token.type == CppToken::SHARP || token.type == CppToken::DOUBLESHARP ||
        token.type == CppToken::MACRO_PARAM_VA_ARGS ||
        token.type == CppToken::VA_OPT) {
      return false;
    }

    // If comma appears as non function argument separator, it fails.
    if (token.IsPuncChar(',')) {
      return false;
    }

    if (token.type != CppToken::IDENTIFIER) {
      output->push_back(token);
      continue;
    }

    // If we encounter "defined" here, it means "defined" is used in
    // #define e.g. "#define FOO (defined(BAR))".
    // "defined" in #if should be expanded before in CppParser::EvalCondition.
    // We don't support "defined" here. Naive expander will handle it.
    if (token.string_value == "defined") {
      return false;
    }

    const Macro* macro = parser_->GetMacro(token.string_value);
    if (!macro || hideset.Has(macro)) {
      output->push_back(token);
      continue;
    }

    // If parens are unbalanced, unexpected expression can happen. So, fail.
    // e.g. F(X) where X = )(, F()() can be produced.
    // This breaks CBV's assumption.
    if (!macro->is_paren_balanced) {
      return false;
    }

    if (macro->type == Macro::OBJ) {
      MacroSet new_hideset(hideset);
      new_hideset.Set(macro);
      if (!Expand(macro->replacement.begin(), macro->replacement.end(),
                  space_handling, new_hideset, Env(), output)) {
        return false;
      }
      continue;
    }

    if (macro->type == Macro::CBK) {
      // __FILE__, __LINE__, etc. Call callback, then token is returned.
      output->push_back((parser_->*(macro->callback))());
      continue;
    }
    if (macro->type == Macro::CBK_FUNC) {
      // Since CBK_FUNC's num_args is 1, arg's size must also be 1.
      if (macro->num_args != 1) {
        LOG(WARNING) << "macro->num_args: " << macro->num_args;
        return false;
      }
      ArgRangeVector args;
      if (!GetMacroArguments(it, input_end, macro->num_args, &it, &args)) {
        return false;
      }
      if (args.size() != 1) {
        LOG(WARNING) << "args: " << args.size();
        return false;
      }
      // expand only if macro params is used.
      // e.g. consider
      //  #if __has_include_next(<errno.h>)
      // where errno is defined.
      // crbug.com/1386100
      bool need_expand = false;
      ArrayTokenList arg;
      for (auto it = args[0].first; it != args[0].second; ++it) {
        const CppToken& token = *it;
        if (token.type == CppToken::MACRO_PARAM) {
          need_expand = true;
        }
        arg.push_back(token);
      }
      if (need_expand) {
        Env new_env(1);
        if (!Expand(args[0].first, args[0].second, space_handling, hideset, env,
                    &new_env[0])) {
          return false;
        }
        arg = new_env[0];
      }

      // CBK_FUNC should always return no-more expandable token.
      output->push_back((parser_->*(macro->callback_func))(arg));
      continue;
    }

    if (macro->type == Macro::FUNC) {
      // We don't support variadic macro. It might cause unexpected ','.
      // Also, if unbalanced parens, it can cause unexpected expression.
      // Fail for these.
      if (macro->is_vararg) {
        return false;
      }

      ArgRangeVector args;
      if (!GetMacroArguments(it, input_end, macro->num_args, &it, &args)) {
        return false;
      }
      DCHECK_EQ(macro->num_args, args.size());

      Env new_env(args.size());
      for (size_t i = 0; i < args.size(); ++i) {
        if (!Expand(args[i].first, args[i].second, space_handling, hideset, env,
                    &new_env[i])) {
          return false;
        }
      }

      // TODO: Don't make new hideset, but update the current
      // hideset (and remove it after Expand to reduce memory allocation).
      MacroSet new_hideset(hideset);
      new_hideset.Set(macro);

      if (!Expand(macro->replacement.begin(), macro->replacement.end(),
                  space_handling, new_hideset, new_env, output)) {
        return false;
      }

      continue;
    }

    CHECK(false) << "unexpected macro type: " << macro->type;
  }

  return true;
}

// static
bool CppMacroExpanderCBV::GetMacroArguments(
    ArrayTokenList::const_iterator begin,
    ArrayTokenList::const_iterator end,
    int n,
    ArrayTokenList::const_iterator* cur,
    ArgRangeVector* args) {
  // skip macro name
  if (*cur == end) {
    return false;
  }
  ++*cur;

  // skip spaces.
  while (*cur != end && (*cur)->type == CppToken::SPACE) {
    ++*cur;
  }

  // Consumes first '('.
  if (*cur == end || !(*cur)->IsPuncChar('(')) {
    // No '(' is found
    return false;
  }
  ++*cur;

  // skip spaces.
  while (*cur != end && (*cur)->type == CppToken::SPACE) {
    ++*cur;
  }

  int arg_pos = 0;
  args->resize(n);

  if (*cur != end && (*cur)->IsPuncChar(')')) {
    // no arguments care. e.g. A().
    // ok if n == 0;
    return n == 0;
  }

  while (*cur != end) {
    // Here, *it is just after '(' or ','.
    ArrayTokenList::const_iterator arg_begin;
    ArrayTokenList::const_iterator arg_end;
    if (!GetMacroArgument(*cur, end, cur, &arg_begin, &arg_end)) {
      // Failed to get Argument.
      return false;
    }

    if (arg_pos < n) {
      (*args)[arg_pos++] = std::make_pair(arg_begin, arg_end);
    } else {
      // Too many argumets.
      return false;
    }

    // Here, *it must be at ')' or ','.
    if ((*cur)->IsPuncChar(')')) {
      break;
    }

    if ((*cur)->IsPuncChar(',')) {
      ++*cur;
    }
  }

  // Now *cur should be on last ')'.
  // Don't Consume last ')'.
  if (*cur == end || !(*cur)->IsPuncChar(')')) {
    return false;
  }

  // argument is short. return false.
  if (arg_pos != n) {
    return false;
  }

  return true;
}

// static
bool CppMacroExpanderCBV::GetMacroArgument(
    ArrayTokenList::const_iterator begin,
    ArrayTokenList::const_iterator end,
    ArrayTokenList::const_iterator* cur,
    ArrayTokenList::const_iterator* argument_begin,
    ArrayTokenList::const_iterator* argument_end) {
  // skip spaces.
  while (*cur != end && (*cur)->type == CppToken::SPACE) {
    ++*cur;
  }

  *argument_begin = *cur;

  int paren_depth = 0;
  while (*cur != end) {
    if (paren_depth == 0 &&
        ((*cur)->IsPuncChar(',') || (*cur)->IsPuncChar(')'))) {
      break;
    }

    if ((*cur)->IsPuncChar('(')) {
      ++paren_depth;
    } else if ((*cur)->IsPuncChar(')')) {
      --paren_depth;
    }
    ++*cur;
  }

  // |*cur| is just ',' or ')'.
  if (*cur == end) {
    return false;
  }
  *argument_end = *cur;

  return paren_depth == 0;
}

}  // namespace devtools_goma
