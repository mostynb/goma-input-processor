/* Generate known_warnings_options.cc from gcc documents.

How to run:
  $ go run generate_known_warnings_list.go > lib/known_warnings_options.h
*/
package main

import (
	"fmt"
	"io"
	"net/http"
	"os"
	"regexp"
	"sort"
	"strings"
)

const GnuDocumentUrl = "https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html"
const ClangDocumentUrl = "https://clang.llvm.org/docs/DiagnosticsReference.html"
const ClangRepositoryUrl = "https://raw.githubusercontent.com/llvm/llvm-project/main/clang/include/clang/Basic/DiagnosticGroups.td"

// Already known warnings. These warnings will be merged.
var knownWarnings = []string{
	"",
	"address",
	"aggregate-return",
	"aligned-new",
	"all",
	"alloc-size-larger-than=",
	"alloc-zero",
	"alloca",
	"alloca-larger-than=",
	"array-bounds",
	"array-bounds=",
	"attribute-alias",
	"bad-function-cast",
	"bool-compare",
	"bool-operation",
	"c++-compat",
	"c++11-compat",
	"c++11-narrowing",
	"c++14-compat",
	"c++17-compat",
	"c90-c99-compat",
	"c99-c11-compat",
	"cast-align",
	"cast-align=",
	"cast-qual",
	"catch-value",
	"catch-value=",
	"char-subscripts",
	"chkp",
	"clobbered",
	"comment",
	"comments",
	"conditionally-supported",
	"conversion",
	"covered-switch-default",
	"dangling-else",
	"date-time",
	"declaration-after-statement",
	"delete-incomplete",
	"delete-non-virtual-dtor",
	"deprecated",
	"disabled-optimization",
	"double-promotion",
	"duplicate-decl-specifier",
	"duplicated-branches",
	"duplicated-cond",
	"effc++",
	"empty-body",
	"endif-labels",
	"enum-compare",
	"error",
	"error-implicit-function-declaration",
	"error=",
	"everything",
	"exit-time-destructors",
	"expansion-to-defined",
	"extra",
	"extra-semi",
	"fallthrough",
	"fatal-errors",
	"float-conversion",
	"float-equal",
	"format",
	"format-nonliteral",
	"format-overflow",
	"format-overflow=",
	"format-security",
	"format-signedness",
	"format-truncation",
	"format-truncation=",
	"format-y2k",
	"format=",
	"frame-address",
	"frame-larger-than",
	"frame-larger-than=",
	"global-constructors",
	"header-hygiene",
	"hsa",
	"if-not-aligned",
	"ignored-attributes",
	"ignored-qualifiers",
	"implicit",
	"implicit-fallthrough",
	"implicit-fallthrough=",
	"implicit-function-declaration",
	"implicit-int",
	"inconsistent-missing-override",
	"init-self",
	"inline",
	"int-in-bool-context",
	"int-to-void-pointer-cast",
	"invalid-memory-model",
	"invalid-pch",
	"jump-misses-init",
	"larger-than=",
	"logical-not-parentheses",
	"logical-op",
	"long-long",
	"main",
	"maybe-uninitialized",
	"memset-elt-size",
	"memset-transposed-args",
	"misleading-indentation",
	"missing-braces",
	"missing-declarations",
	"missing-field-initializers",
	"missing-format-attribute",
	"missing-include-dirs",
	"missing-noreturn",
	"missing-parameter-type",
	"missing-prototypes",
	"multistatement-macros",
	"nested-externs",
	"no-#pragma-messages",
	"no-#warnings",
	"no-abi",
	"no-absolute-value",
	"no-abstract-vbase-init",
	"no-address-of-packed-member",
	"no-aggressive-loop-optimizations",
	"no-array-bounds",
	"no-attributes",
	"no-backend-plugin",
	"no-bitfield-width",
	"no-bool-conversion",
	"no-builtin-declaration-mismatch",
	"no-builtin-macro-redefined",
	"no-builtin-requires-header",
	"no-c++11-compat",
	"no-c++11-extensions",
	"no-c++11-narrowing",
	"no-c++98-compat",
	"no-c++98-compat-pedantic",
	"no-c99-extensions",
	"no-cast-align",
	"no-cast-qual",
	"no-char-subscripts",
	"no-comment",
	"no-conditional-uninitialized",
	"no-constant-conversion",
	"no-constant-logical-operand",
	"no-conversion",
	"no-conversion-null",
	"no-coverage-mismatch",
	"no-covered-switch-default",
	"no-cpp",
	"no-dangling-else",
	"no-delete-incomplete",
	"no-delete-non-virtual-dtor",
	"no-deprecated",
	"no-deprecated-declarations",
	"no-deprecated-register",
	"no-designated-init",
	"no-disabled-macro-expansion",
	"no-discarded-array-qualifiers",
	"no-discarded-qualifiers",
	"no-div-by-zero",
	"no-documentation",
	"no-documentation-unknown-command",
	"no-double-promotion",
	"no-duplicate-decl-specifier",
	"no-empty-body",
	"no-endif-labels",
	"no-enum-compare",
	"no-enum-compare-switch",
	"no-enum-conversion",
	"no-error",
	"no-error-sometimes-uninitialized",
	"no-error-unused",
	"no-error=",
	"no-exit-time-destructors",
	"no-expansion-to-defined",
	"no-extern-c-compat",
	"no-extern-initializer",
	"no-extra",
	"no-extra-tokens",
	"no-float-conversion",
	"no-float-equal",
	"no-for-loop-analysis",
	"no-format",
	"no-format-contains-nul",
	"no-format-extra-args",
	"no-format-nonliteral",
	"no-format-pedantic",
	"no-format-security",
	"no-format-y2k",
	"no-format-zero-length",
	"no-four-char-constants",
	"no-frame-larger-than",
	"no-frame-larger-than=",
	"no-free-nonheap-object",
	"no-gcc-compat",
	"no-global-constructors",
	"no-gnu-anonymous-struct",
	"no-gnu-designator",
	"no-gnu-variable-sized-type-not-at-end",
	"no-gnu-zero-variadic-macro-arguments",
	"no-header-guard",
	"no-header-hygiene",
	"no-ignored-attributes",
	"no-ignored-qualifiers",
	"no-implicit-exception-spec-mismatch",
	"no-implicit-fallthrough",
	"no-implicit-function-declaration",
	"no-implicit-int",
	"no-implicitly-unsigned-literal",
	"no-import",
	"no-incompatible-library-redeclaration",
	"no-incompatible-pointer-types",
	"no-incompatible-pointer-types-discards-qualifiers",
	"no-inconsistent-dllimport",
	"no-inconsistent-missing-override",
	"no-inherited-variadic-ctor",
	"no-initializer-overrides",
	"no-inline-asm",
	"no-inline-new-delete",
	"no-int-conversion",
	"no-int-to-pointer-cast",
	"no-int-to-void-pointer-cast",
	"no-invalid-noreturn",
	"no-invalid-offsetof",
	"no-knr-promoted-parameter",
	"no-literal-conversion",
	"no-logical-not-parentheses",
	"no-logical-op-parentheses",
	"no-long-long",
	"no-macro-redefined",
	"no-max-unsigned-zero",
	"no-maybe-uninitialized",
	"no-microsoft-cast",
	"no-microsoft-enum-forward-reference",
	"no-microsoft-extra-qualification",
	"no-microsoft-goto",
	"no-microsoft-include",
	"no-mismatched-tags",
	"no-missing-braces",
	"no-missing-field-initializers",
	"no-missing-noescape",
	"no-missing-noreturn",
	"no-missing-prototypes",
	"no-missing-variable-declarations",
	"no-multichar",
	"no-narrowing",
	"no-nested-anon-types",
	"no-newline-eof",
	"no-non-literal-null-conversion",
	"no-non-pod-varargs",
	"no-non-virtual-dtor",
	"no-nonnull",
	"no-nonportable-include-path",
	"no-nonportable-system-include-path",
	"no-null-conversion",
	"no-null-dereference",
	"no-null-pointer-arithmetic",
	"no-nullability-completeness",
	"no-objc-missing-property-synthesis",
	"no-odr",
	"no-old-style-cast",
	"no-overflow",
	"no-overloaded-virtual",
	"no-override-init",
	"no-padded",
	"no-parentheses",
	"no-parentheses-equality",
	"no-pedantic",
	"no-pedantic-ms-format",
	"no-pessimizing-move",
	"no-pointer-arith",
	"no-pointer-bool-conversion",
	"no-pointer-sign",
	"no-pointer-to-int-cast",
	"no-pragmas",
	"no-psabi",
	"no-reorder",
	"no-reserved-id-macro",
	"no-return-local-addr",
	"no-return-type",
	"no-scalar-storage-order",
	"no-self-assign",
	"no-section",
	"no-semicolon-before-method-body",
	"no-sequence-point",
	"no-shadow",
	"no-shadow-ivar",
	"no-shift-count-overflow",
	"no-shift-negative-value",
	"no-shift-op-parentheses",
	"no-shift-overflow",
	"no-shift-sign-overflow",
	"no-shorten-64-to-32",
	"no-sign-compare",
	"no-sign-conversion",
	"no-sign-promo",
	"no-signed-enum-bitfield",
	"no-sizeof-pointer-memaccess",
	"no-sometimes-uninitialized",
	"no-strict-aliasing",
	"no-strict-overflow",
	"no-string-conversion",
	"no-string-plus-int",
	"no-switch",
	"no-switch-enum",
	"no-system-headers",
	"no-tautological-compare",
	"no-tautological-constant-compare",
	"no-tautological-constant-out-of-range-compare",
	"no-tautological-pointer-compare",
	"no-tautological-undefined-compare",
	"no-tautological-unsigned-enum-zero-compare",
	"no-tautological-unsigned-zero-compare",
	"no-thread-safety-analysis",
	"no-thread-safety-negative",
	"no-trigraphs",
	"no-type-limits",
	"no-typedef-redefinition",
	"no-undeclared-selector",
	"no-undef",
	"no-undefined-bool-conversion",
	"no-undefined-func-template",
	"no-undefined-var-template",
	"no-unguarded-availability",
	"no-uninitialized",
	"no-unknown-attributes",
	"no-unknown-pragmas",
	"no-unknown-warning-option",
	"no-unnamed-type-template-args",
	"no-unneeded-internal-declaration",
	"no-unreachable-code",
	"no-unreachable-code-break",
	"no-unreachable-code-return",
	"no-unused",
	"no-unused-but-set-variable",
	"no-unused-command-line-argument",
	"no-unused-const-variable",
	"no-unused-function",
	"no-unused-label",
	"no-unused-lambda-capture",
	"no-unused-local-typedef",
	"no-unused-local-typedefs",
	"no-unused-macros",
	"no-unused-member-function",
	"no-unused-parameter",
	"no-unused-private-field",
	"no-unused-result",
	"no-unused-template",
	"no-unused-value",
	"no-unused-variable",
	"no-used-but-marked-unused",
	"no-user-defined-warnings",
	"no-varargs",
	"no-variadic-macros",
	"no-virtual-move-assign",
	"no-visibility",
	"no-vla",
	"no-weak-vtables",
	"no-writable-strings",
	"no-write-strings",
	"no-zero-as-null-pointer-constant",
	"no-zero-length-array",
	"non-virtual-dtor",
	"nonnull",
	"nonnull-compare",
	"normalized=",
	"null-dereference",
	"objc-missing-property-synthesis",
	"old-style-cast",
	"old-style-declaration",
	"old-style-definition",
	"openmp-simd",
	"overlength-strings",
	"overloaded-virtual",
	"override-init",
	"override-init-side-effects",
	"packed",
	"packed-bitfield-compat",
	"packed-not-aligned",
	"padded",
	"parentheses",
	"partial-availability",
	"pedantic",
	"placement-new",
	"placement-new=",
	"pointer-arith",
	"pointer-compare",
	"pointer-sign",
	"redundant-decls",
	"restrict",
	"return-type",
	"sequence-point",
	"shadow",
	"shadow=",
	"shift-count-negative",
	"shift-count-overflow",
	"shift-negative-value",
	"shift-overflow",
	"shift-overflow=",
	"shorten-64-to-32",
	"sign-compare",
	"sign-conversion",
	"sign-promo",
	"sized-deallocation",
	"sizeof-array-argument",
	"sizeof-pointer-div",
	"sizeof-pointer-memaccess",
	"stack-protector",
	"stack-usage",
	"stack-usage=",
	"strict-aliasing",
	"strict-aliasing=",
	"strict-overflow",
	"strict-overflow=",
	"strict-prototypes",
	"string-conversion",
	"stringop-overflow",
	"stringop-overflow=",
	"stringop-truncation",
	"subobject-linkage",
	"suggest-attribute=",
	"suggest-final-methods",
	"suggest-final-types",
	"suggest-override",
	"switch",
	"switch-bool",
	"switch-default",
	"switch-enum",
	"switch-unreachable",
	"sync-nand",
	"system-headers",
	"tautological-compare",
	"tautological-constant-out-of-range-compare",
	"tautological-overlap-compare",
	"tautological-unsigned-zero-compare",
	"thread-safety",
	"thread-safety-negative",
	"traditional",
	"traditional-conversion",
	"trampolines",
	"trigraphs",
	"type-limits",
	"undeclared-selector",
	"undef",
	"unguarded-availability",
	"uninitialized",
	"unknown-pragmas",
	"unreachable-code",
	"unreachable-code-break",
	"unreachable-code-return",
	"unsafe-loop-optimizations",
	"unsuffixed-float-constants",
	"unused",
	"unused-but-set-parameter",
	"unused-but-set-variable",
	"unused-const-variable",
	"unused-const-variable=",
	"unused-function",
	"unused-label",
	"unused-lambda-capture",
	"unused-local-typedefs",
	"unused-macros",
	"unused-parameter",
	"unused-value",
	"unused-variable",
	"used-but-marked-unused",
	"useless-cast",
	"user-defined-warnings",
	"varargs",
	"variadic-macros",
	"vector-operation-performance",
	"vla",
	"vla-larger-than=",
	"volatile-register-var",
	"write-strings",
	"zero-as-null-pointer-constant",
}

// removeAfterEqual removes string after '='. '=' is preserved.
func removeAfterEqual(s string) string {
	p := strings.Index(s, "=")
	if p >= 0 {
		return s[:p+1]
	}

	return s
}

// loadFromWeb reads body from url.
func loadFromWeb(url string) (string, error) {
	resp, err := http.Get(url)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return "", err
	}

	return string(body), nil
}

// parseGnuDocument loads gnu document and parses it to list all warnings.
// The result might contain "no-" form warnings or duplicate.
func parseGnuDocument() ([]string, error) {
	body, err := loadFromWeb(GnuDocumentUrl)
	if err != nil {
		return nil, err
	}

	var warnings []string

	codeRE := regexp.MustCompile(`(?m)<dt><code>-W(.*)</code></dt>`)
	removeTagRE := regexp.MustCompile(`<.*>`)
	for _, matched := range codeRE.FindAllStringSubmatch(body, -1) {
		s := matched[1]
		s = removeTagRE.ReplaceAllString(s, "")
		s = removeAfterEqual(s)
		warnings = append(warnings, strings.TrimSpace(s))
	}

	return warnings, nil
}

// parseClangDocument loads clang document and parses it to list all warnings.
// The result might contain "no-" form warnings or duplicate.
func parseClangDocument() ([]string, error) {
	body, err := loadFromWeb(ClangDocumentUrl)
	if err != nil {
		return nil, err
	}

	var warnings []string

	// clang document has ToC which consists of lines like the following:
	//   <li><a class="reference internal" href="#wcl4" id="id8">-WCL4</a></li>
	// In this example, take "CL4" from this line. The content of href and id varies.
	codeRE := regexp.MustCompile(`<li><a class="reference internal" href=".*" id=".*">-W(.*)</a></li>`)
	for _, matched := range codeRE.FindAllStringSubmatch(body, -1) {
		s := matched[1]
		s = removeAfterEqual(s)
		warnings = append(warnings, strings.TrimSpace(s))
	}

	return warnings, nil
}

// parseClangRepository loads flag config file from llvm repository.
func parseClangRepository() ([]string, error) {
	body, err := loadFromWeb(ClangRepositoryUrl)
	if err != nil {
		return nil, err
	}
	var warnings []string
	// clang warning flags config has flag config like belows.
	// * def ObjCStringComparison : DiagGroup<"objc-string-compare">;
	// * def : DiagGroup<"switch-default">;
	// * def Shadow : DiagGroup<"shadow", [ShadowFieldInConstructorModified,
	codeRE := regexp.MustCompile(`def.* : DiagGroup<"(.*)"`)
	for _, matched := range codeRE.FindAllStringSubmatch(body, -1) {
		s := matched[1]
		s = removeAfterEqual(s)
		warnings = append(warnings, strings.TrimSpace(s))
	}

	return warnings, nil
}

func main() {
	clangWarnings, err := parseClangDocument()
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to read clang documents: %v\n", err)
		os.Exit(1)
	}

	clangWarningsInRepository, err := parseClangRepository()
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to read clang reposotiry: %v\n", err)
		os.Exit(1)
	}

	clangWarnings = append(clangWarnings, clangWarningsInRepository...)

	gnuWarnings, err := parseGnuDocument()
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to read gnu documents: %v\n", err)
		os.Exit(1)
	}

	warnings := make(map[string]bool)
	// We register warnings without "no-" form.
	for _, w := range knownWarnings {
		warnings[strings.TrimPrefix(w, "no-")] = true
	}
	for _, w := range clangWarnings {
		warnings[strings.TrimPrefix(w, "no-")] = true
	}
	for _, w := range gnuWarnings {
		warnings[strings.TrimPrefix(w, "no-")] = true
	}

	var sortedWarnings []string
	for w, _ := range warnings {
		sortedWarnings = append(sortedWarnings, w)
	}
	sort.Strings(sortedWarnings)

	fmt.Print(`// Copyright 2017 Google Inc. All Rights Reserved.
//
// This is auto generated by build/generate_known_warnings_list.go
// DO NOT EDIT

#ifndef DEVTOOLS_GOMA_LIB_KNOWN_WARNING_OPTIONS_H_
#define DEVTOOLS_GOMA_LIB_KNOWN_WARNING_OPTIONS_H_

namespace devtools_goma {
const char* const kKnownWarningOptions[] {
`)
	for _, w := range sortedWarnings {
		fmt.Printf("  \"%s\",\n", w)
	}
	fmt.Print(`};
}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_LIB_KNOWN_WARNING_OPTIONS_H_
`)
}
