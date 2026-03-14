#include "backend/scope.hpp"
#include "backend/symbol.hpp"
#include "backend/type.hpp"

#include <memory>

scope_t::scope_t(SP<scope_t> parent)
  : parent(parent)
  , types(parent ? &parent->types : nullptr) {}

SP<scope_t>
scope_t::get_parent() const {
  return parent;
}

const std::map<std::string, SP<symbol_t>> &
scope_t::symbol_map() const {
  return symbols;
}

void
scope_t::merge(const scope_t &other) {
  symbols.insert(other.symbols.begin(), other.symbols.end());
  templates.insert(templates.end(), other.templates.begin(), other.templates.end());
  types.merge(other.types);
}

SP<symbol_t>
scope_t::resolve(const std::string &identifier) {
  if (symbols.contains(identifier))
    return symbols.at(identifier);

  if (parent)
    return parent->resolve(identifier);

  return nullptr;
}

SP<symbol_t>
scope_t::resolve(const specialized_path_t &identifier) {
  return resolve(to_string(identifier));
}

bool
scope_t::contains(const std::string &identifier) {
  return symbols.contains(identifier);
}

void
scope_t::remove(const specialized_path_t &identifier) {
  remove(to_string(identifier));
}

void
scope_t::remove(const std::string &identifier) {
  if (symbols.contains(identifier)) {
    symbols.erase(identifier);
    return;
  }

  if (parent) {
    parent->remove(identifier);
  }
}

SP<symbol_t>
scope_t::add(const std::string &identifier, SP<type_t> type, bool is_mutable) {
  symbols[identifier] = std::make_shared<symbol_t>(
    symbol_t{ .name = identifier, .type = type, .is_mutable = is_mutable });
  return symbols.at(identifier);
}

SP<symbol_t>
scope_t::add(const specialized_path_t &identifier, SP<type_t> type, bool is_mutable) {
  return add(to_string(identifier), type, is_mutable);
}

void
scope_t::add_template(SP<template_decl_t> node) {
  templates.push_back(node);
}

std::vector<SP<template_decl_t>>
scope_t::candidates(const specialized_path_t &path) {
  std::vector<SP<template_decl_t>> result;
  for (auto &template_ : templates) {
    if (path.segments.size() != template_->name.segments.size())
      continue;

    bool match = true;

    for (auto i = 0; i < path.segments.size(); i++) {
      match = match && path.segments[i].name == template_->name.segments[i].name;
      if (path.segments[i].types.size() != template_->name.segments[i].bindings.size())
        match = false;
      if (!match)
        break;
    }

    if (match)
      result.push_back(template_);
  }

  if (parent) {
    result.insert_range(result.begin(), parent->candidates(path));
  }
  return result;
}
