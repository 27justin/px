#pragma once

#include "frontend/ast.hpp"
#include "type.hpp"
#include "frontend/path.hpp"

#include <map>
#include <unordered_map>

struct type_registry_t {
  type_registry_t();
  type_registry_t(type_registry_t *parent);

  SP<type_t> resolve(const std::string &name);
  SP<type_t> resolve(const specialized_path_t &name);

  SP<type_t> add_builtin(const specialized_path_t &name, size_t size, size_t alignment, type_kind_t kind);
  SP<type_t> add_function(SP<type_t> return_type, const std::vector<SP<type_t>> &arguments, SP<type_t> receiver, bool is_var_args);
  SP<type_t> add_struct(const specialized_path_t &name, struct_layout_t layout);
  SP<type_t> add_alias(const specialized_path_t &name, SP<type_t>, bool is_distinct);
  SP<type_t> add_contract(const specialized_path_t &name, const std::map<std::string, SP<type_t>> &requirements);
  SP<type_t> add_enum(const specialized_path_t &name, const enum_decl_t &);

  SP<type_t> pointer_to(SP<type_t> base, std::vector<pointer_kind_t> indirections, bool is_mutable);
  SP<type_t> array_of(SP<type_t> base, size_t len);
  SP<type_t> slice_of(SP<type_t> base, bool is_mutable);
  SP<type_t> self_placeholder(const specialized_path_t &name);
  SP<type_t> tuple_of(const std::unordered_map<std::string, SP<type_t>> &elements);
  SP<type_t> union_of(const std::map<std::string, SP<type_t>> &union_types);
  SP<type_t> add_template_alias(const specialized_path_t &name, SP<type_t>);

  SP<type_t> rvalue_of(SP<type_t> base);

  SP<type_t> untyped_literal(const std::string &, literal_type_t);

  void merge(const type_registry_t &other);
private:
  type_registry_t *parent = nullptr;
  std::map<std::string, SP<type_t>> registry;
};
