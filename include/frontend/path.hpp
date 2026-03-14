#pragma once

#include <memory>
#include <string>
#include <vector>

struct constraint_t {
  std::string name;
};

struct generic_t;
struct type_decl_t;
// struct type_t;

struct specialized_segment_t {
  std::string                               name;
  std::vector<std::shared_ptr<type_decl_t>> types;

  specialized_segment_t() = default;
  specialized_segment_t(const std::string &name, const std::vector<type_decl_t> &types);
  specialized_segment_t(const std::string &name);
};

struct specialized_path_t {
  std::vector<specialized_segment_t> segments;

  specialized_path_t();
  specialized_path_t(const std::string &);
  specialized_path_t(const std::vector<specialized_segment_t> &segments);

  bool
  operator==(const specialized_path_t &) const;

  std::shared_ptr<type_decl_t>
  param(size_t);
  void
  push(const std::string &segment);
  bool
  is_simple() const;
};

template<>
struct std::hash<specialized_path_t> {
  std::size_t
  operator()(const specialized_path_t &) const noexcept;
};

struct template_segment_t {
  std::string            name;
  std::vector<generic_t> bindings;
};

struct template_path_t {
  std::vector<template_segment_t> segments;

  // specialized_path_t bind(const std::vector<type_t> &) const;
  size_t
  params() const;
  generic_t
  param(size_t) const;
};

struct generic_t {
  std::string                     binding;     //< type "name" within the scope (e.g. T, etc.)
  std::vector<specialized_path_t> constraints; //< constraint, contracts., etc.
};

std::string
to_string(const specialized_path_t &);
std::string
to_string(const template_path_t &);
std::string
to_string(const generic_t &);
