// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVTOOLS_GOMA_CLIENT_CLANG_MODULES_MODULEMAP_TYPE_H_
#define DEVTOOLS_GOMA_CLIENT_CLANG_MODULES_MODULEMAP_TYPE_H_

#include <ostream>
#include <string>
#include <vector>

namespace devtools_goma {
namespace modulemap {

// Feature represents a feature.
// If '!' is prefixed, is_positive_ is false.
class Feature {
 public:
  Feature() = default;
  Feature(std::string name, bool is_positive)
      : name_(std::move(name)), is_positive_(is_positive) {}

  bool is_positive() const { return is_positive_; }
  const std::string& name() const { return name_; }

  void set_is_positive(bool b) { is_positive_ = b; }
  std::string* mutable_name() { return &name_; }

  friend bool operator==(const Feature& lhs, const Feature& rhs) {
    return lhs.name_ == rhs.name_ && lhs.is_positive_ == rhs.is_positive_;
  }

 private:
  std::string name_;
  bool is_positive_ = true;
};

class Header {
 public:
  bool is_umbrella() const { return is_umbrella_; }
  void set_is_umbrella(bool b) { is_umbrella_ = b; }

  bool is_exclude() const { return is_exclude_; }
  void set_is_exclude(bool b) { is_exclude_ = b; }

  bool is_private() const { return is_private_; }
  void set_is_private(bool b) { is_private_ = b; }

  bool is_textual() const { return is_textual_; }
  void set_is_textual(bool b) { is_textual_ = b; }

  const std::string& name() const { return name_; }
  std::string* mutable_name() { return &name_; }

  const std::string& size() const { return size_; }
  std::string* mutable_size() { return &size_; }

  const std::string& mtime() const { return mtime_; }
  std::string* mutable_mtime() { return &mtime_; }

 private:
  bool is_umbrella_ = false;
  bool is_exclude_ = false;
  bool is_private_ = false;
  bool is_textual_ = false;

  std::string name_;
  std::string size_;   // TODO: should be int64_t?
  std::string mtime_;  // TODO: should be int64_t?
};

struct ConfigMacro {
  const std::vector<std::string>& attributes() const { return attributes_; }
  std::vector<std::string>* mutable_attributes() { return &attributes_; }

  const std::vector<std::string>& macros() const { return macros_; }
  std::vector<std::string>* mutable_macros() { return &macros_; }

  std::vector<std::string> attributes_;
  std::vector<std::string> macros_;
};

struct Link {
  const std::string& name() const { return name_; }
  bool is_framework() const { return is_framework_; }

  std::string name_;
  bool is_framework_;
};

struct Conflict {
  const std::string& module_id() const { return module_id_; }
  const std::string& reason() const { return reason_; }

  std::string module_id_;
  std::string reason_;
};

class Module {
 public:
  void set_is_explicit(bool b) { is_explicit_ = b; }
  void set_is_framework(bool b) { is_framework_ = b; }
  void set_is_extern(bool b) { is_extern_ = b; }
  void set_is_inferred_submodule(bool b) { is_inferred_submodule_ = b; }
  void set_has_inferfered_submodule_member(bool b) {
    has_inferred_submodule_member_ = b;
  }

  const std::string& module_id() const { return module_id_; }
  void set_module_id(std::string module_id) {
    module_id_ = std::move(module_id);
  }
  std::string* mutable_module_id() { return &module_id_; }

  // For extern modules
  // extern module <module-id> <string-literal>
  const std::string& extern_filename() const { return extern_filename_; }
  std::string* mutable_extern_filename() { return &extern_filename_; }

  const std::vector<std::string>& attributes() const { return attributes_; }
  std::vector<std::string>* mutable_attributes() { return &attributes_; }
  // Returns true if |attr| exists in attributes.
  bool HasAttribute(const std::string& attr) const {
    for (const auto& x : attributes()) {
      if (attr == x) {
        return true;
      }
    }
    return false;
  }

  const std::vector<Module>& submodules() const { return submodules_; }
  void add_submodule(Module module) {
    submodules_.push_back(std::move(module));
  }

  const std::vector<Feature>& requires() const { return requires_; }
  std::vector<Feature>* mutable_requires() { return &requires_; }

  const std::vector<Header>& headers() const { return headers_; }
  void add_header(Header header) { headers_.push_back(std::move(header)); }

  const std::vector<std::string>& umbrella_dirs() const {
    return umbrella_dirs_;
  }
  void add_umbrella_dir(std::string name) {
    umbrella_dirs_.push_back(std::move(name));
  }

  const std::vector<std::string>& exports() const { return exports_; }
  void add_export(std::string name) { exports_.push_back(std::move(name)); }

  const std::vector<std::string>& export_as() const { return export_as_; }
  void add_export_as(std::string name) {
    export_as_.push_back(std::move(name));
  }

  const std::vector<std::string>& uses() const { return uses_; }
  void add_use(std::string name) { uses_.push_back(std::move(name)); }

  const std::vector<Link>& links() const { return links_; }
  void add_link(Link link) { links_.push_back(std::move(link)); }

  const std::vector<ConfigMacro>& config_macros() const {
    return config_macros_;
  }
  void add_config_macros(ConfigMacro config_macro) {
    config_macros_.push_back(std::move(config_macro));
  }

  const std::vector<Conflict>& conflicts() const { return conflicts_; }
  void add_conflict(Conflict conflict) {
    conflicts_.push_back(std::move(conflict));
  }

  // pretty printing the module.
  void PrettyPrint(std::ostream* os, int indent_level = 0) const;
  friend std::ostream& operator<<(std::ostream& os, const Module& module) {
    module.PrettyPrint(&os);
    return os;
  }

 private:
  // Common attributes.
  std::string module_id_;
  std::vector<std::string> attributes_;

  // For usual module.
  bool is_explicit_ = false;
  bool is_framework_ = false;
  std::vector<Feature> requires_;
  std::vector<Header> headers_;
  std::vector<std::string> umbrella_dirs_;
  std::vector<std::string> exports_;
  std::vector<std::string> export_as_;
  std::vector<std::string> uses_;
  std::vector<Module> submodules_;
  std::vector<Link> links_;
  std::vector<ConfigMacro> config_macros_;
  std::vector<Conflict> conflicts_;

  // For extern module.
  bool is_extern_ = false;
  std::string extern_filename_;

  // For inferred submodule
  bool is_inferred_submodule_ = false;
  bool has_inferred_submodule_member_ = false;
};

class ModuleMap {
 public:
  const std::vector<Module>& modules() const { return modules_; }

  void add_module(Module module) { modules_.push_back(std::move(module)); }

 private:
  std::vector<Module> modules_;
};

}  // namespace modulemap
}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_CLANG_MODULES_MODULEMAP_TYPE_H_
