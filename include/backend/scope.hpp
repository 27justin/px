#pragma once

#include "backend/type.hpp"
#include "backend/type_registry.hpp"
#include "frontend/ast.hpp"
#include <map>

struct scope_t {
  SP<symbol_t>
  resolve(const std::string &identifier);
  SP<symbol_t>
  resolve(const specialized_path_t &identifier);

  SP<scope_t>
  get_parent() const;

  /// Return whether the identifier is present in the current scope
  /// (checks only the current symbol table, use `resolve` to
  /// recursively check for a path.)
  bool
  contains(const std::string &identifier);

  SP<symbol_t>
  add(const std::string &identifier, SP<type_t>, bool is_mutable = false);
  SP<symbol_t>
  add(const specialized_path_t &identifier, SP<type_t>, bool is_mutable = false);

  void
  remove(const std::string &identifier);
  void
  remove(const specialized_path_t &identifier);

  void add_template(SP<template_decl_t>);

  std::vector<SP<template_decl_t>>
  candidates(const specialized_path_t &path);

  scope_t(SP<scope_t>);

  void
  merge(const scope_t &);

  const std::map<std::string, SP<symbol_t>> &
  symbol_map() const;

  type_registry_t types;

  private:
  std::map<std::string, SP<symbol_t>> symbols;
  std::vector<SP<template_decl_t>>    templates;
  SP<scope_t>                         parent;
};
