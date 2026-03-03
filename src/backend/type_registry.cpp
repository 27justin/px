#include "backend/type_registry.hpp"
#include "backend/type.hpp"

#include <sstream>
#include <format>
#include <memory>

type_registry_t::type_registry_t(type_registry_t *parent_)
  : parent(parent_) {
  if (parent == nullptr) {
    add_builtin({"void"}, 0, 0, type_kind_t::eVoid);

    // Only allowed as pointer
    add_builtin({"any"}, 0, 0, type_kind_t::eVoid);

    // Integers
    add_builtin({"i8"}, 1 * 8, 1, type_kind_t::eInt);
    add_builtin({"u8"}, 1 * 8, 1, type_kind_t::eUint);

    add_builtin({"i16"}, 2 * 8, 2, type_kind_t::eInt);
    add_builtin({"u16"}, 2 * 8, 2, type_kind_t::eUint);

    add_builtin({"i32"}, 4 * 8, 4, type_kind_t::eInt);
    add_builtin({"u32"}, 4 * 8, 4, type_kind_t::eUint);
    add_builtin({"i64"}, 8 * 8, 8, type_kind_t::eInt);
    add_builtin({"u64"}, 8 * 8, 8, type_kind_t::eUint);
    add_builtin({"f32"}, 4 * 8, 4, type_kind_t::eFloat);
    add_builtin({"f64"}, 8 * 8, 8, type_kind_t::eFloat);
    add_builtin({"bool"}, 1, 1, type_kind_t::eBool);
  }
}

SP<type_t> type_registry_t::resolve(const std::string &name) {
  if (registry.contains(name)) {
    auto ty = registry.at(name);
    if (ty->kind == type_kind_t::eAlias) {
      if (ty->as.alias->is_transparent) {
        return ty->as.alias->alias;
      }
    }
    return ty;
  }

  else if (parent)
    return parent->resolve(name);
  return nullptr;
}

SP<type_t> type_registry_t::resolve(const specialized_path_t &path) {
  std::string name = to_string(path);
  return resolve(name);
}

SP<type_t> type_registry_t::add_builtin(const specialized_path_t &name,
                                        size_t size,
                                        size_t alignment,
                                        type_kind_t kind) {
  auto t = std::make_shared<type_t>();
  t->name = name;
  t->size = size;
  t->alignment = alignment;
  t->kind = kind;
  registry[to_string(name)] = t;
  return t;
}

SP<type_t> type_registry_t::add_function(
    SP<type_t> return_type,
    const std::vector<SP<type_t>> &arguments,
    SP<type_t> receiver,
    bool is_var_args) {
  auto t = std::make_shared<type_t>();

  // Signature to later validate calls.
  auto signature = new function_signature_t;
  signature->arg_types = arguments;
  signature->return_type = return_type ? return_type : resolve("void");
  signature->receiver = receiver;
  signature->is_var_args = is_var_args;

  t->kind = type_kind_t::eFunction;
  t->as.function = signature;

  // Serialize into a name
  std::stringstream ss;
  ss << "fn (";

  for (auto i = 0; i < arguments.size(); i++) {
    ss << to_string(arguments[i]);
    if (i < arguments.size() - 1) ss << ", ";
  }

  ss << ")";

  if (return_type) {
    ss << " -> " << to_string(return_type);
  }

  // Set the name
  t->name = ss.str();

  // Use zero size for type, this can't be allocated anyhow
  t->size = 0;
  t->alignment = 0;

  registry[to_string(t->name)] = t;
  return t;
}

SP<type_t> type_registry_t::add_struct(const specialized_path_t &name,
                                       struct_layout_t layout) {
  auto type = std::make_shared<type_t>();
  type->size = layout.size;
  type->alignment = layout.alignment;

  type->kind = type_kind_t::eStruct;
  type->as.struct_layout = new struct_layout_t(std::move(layout));
  type->name = name;

  registry[to_string(name)] = type;
  return type;
}

SP<type_t>
type_registry_t::add_contract(const specialized_path_t &name, const std::map<std::string, SP<type_t>> &requirements) {
  auto type = std::make_shared<type_t>();
  type->size = (2 * sizeof(void*)) * 8;
  type->alignment = 8;

  type->kind = type_kind_t::eContract;
  type->as.contract = new contract_t(requirements);
  type->name = name;

  registry[to_string(name)] = type;
  return type;
}

SP<type_t> type_registry_t::add_alias(const specialized_path_t &name,
                                      SP<type_t> alias,
                                      bool is_distinct) {
  auto type = std::make_shared<type_t>();
  type->size = size_of(alias);
  type->alignment = alignment_of(alias);

  type_alias_t *alias_decl = new type_alias_t {};
  alias_decl->alias = alias;

  type->kind = is_distinct ? type_kind_t::eOpaque : type_kind_t::eAlias;
  type->as.alias = alias_decl;
  type->name = name;

  registry[to_string(name)] = type;
  return type;
}

SP<type_t> type_registry_t::add_template_alias(const specialized_path_t &name,
                                      SP<type_t> alias) {
  auto type = std::make_shared<type_t>();
  type->size = size_of(alias);
  type->alignment = alignment_of(alias);

  type_alias_t *alias_decl = new type_alias_t {};
  alias_decl->alias = alias;
  alias_decl->is_transparent = true;

  type->kind = type_kind_t::eAlias;
  type->as.alias = alias_decl;
  type->name = name;

  registry[to_string(name)] = type;
  return type;
}

SP<type_t> type_registry_t::pointer_to(SP<type_t> base,
                                       std::vector<pointer_kind_t> indirections,
                                       bool is_mutable) {
  auto type = std::make_shared<type_t>();
  type->size = sizeof(void*);
  type->alignment = sizeof(void*);

  pointer_t *ptr = new pointer_t {};
  ptr->indirections = indirections;
  ptr->is_mutable = is_mutable;
  ptr->base = base;

  type->kind = type_kind_t::ePointer;
  type->as.pointer = ptr;
  type->name = base->name;

  registry[to_string(type)] = type;
  return type;
}

SP<type_t>
type_registry_t::array_of(SP<type_t> base,
                          size_t len) {
  auto type = std::make_shared<type_t>();
  type->size = sizeof(void*);
  type->alignment = sizeof(void*);

  array_t *arr = new array_t {};
  arr->element_type = base;
  arr->size = len;

  type->kind = type_kind_t::eArray;
  type->as.array = arr;
  type->name = base->name;

  registry[to_string(type)] = type;
  return type;
}

SP<type_t>
type_registry_t::slice_of(SP<type_t> base,
                          bool is_mutable) {
  auto type = type_t::make_slice(base, is_mutable);

  registry[to_string(type)] = type;
  return type;
}

SP<type_t> type_registry_t::self_placeholder(const specialized_path_t& owner_name) {
  auto t = std::make_shared<type_t>();
  t->kind = eSelf;
  t->name = owner_name;
  return t;
}

SP<type_t>
type_registry_t::tuple_of(const std::vector<std::pair<std::string, SP<type_t>>> &elements) {
  auto t = std::make_shared<type_t>();
  t->kind = eTuple;
  t->as.tuple = new tuple_t {elements};

  uint64_t max_alignment = 0;
  uint64_t size = 0;
  for (auto &[_, v] : elements) {
    max_alignment = v->alignment > max_alignment ? v->alignment : max_alignment;
    size += v->size;
  }

  t->alignment = max_alignment;
  t->size = size;
  t->name = to_string(t);

  if (registry.contains(to_string(t)))
    return registry.at(to_string(t));

  registry[to_string(t)] = t;
  return t;
}

SP<type_t>
type_registry_t::union_of(const std::map<std::string, SP<type_t>> &elements) {
  auto t = std::make_shared<type_t>();
  t->kind = eUnion;
  t->as.union_ = new union_t {elements};

  uint64_t max_size = 0, max_alignment = 0;
  for (auto &[_, v] : elements) {
    max_alignment = v->alignment > max_alignment ? v->alignment : max_alignment;
    max_size = v->size > max_size ? v->size : max_size;
  }

  t->alignment = max_alignment;
  t->size = max_size;

  return t;
}

SP<type_t>
type_registry_t::add_enum(const specialized_path_t &name, const enum_decl_t &decl) {
  auto t = std::make_shared<type_t>();
  t->kind = eEnum;
  t->as.enum_ = new enum_t {decl.values};
  t->name = name;
  t->size = sizeof(int32_t) * 8;
  t->alignment = alignof(int32_t);

  registry[to_string(name)] = t;
  return t;
}

SP<type_t>
type_registry_t::rvalue_of(SP<type_t> base) {
  auto type = std::make_shared<type_t>();
  type->size = base->size;
  type->alignment = base->alignment;

  rvalue_reference_t *rvalue = new rvalue_reference_t {};
  rvalue->base = base;

  type->kind = type_kind_t::eRValueReference;
  type->as.rvalue = rvalue;
  type->name = base->name;

  registry[to_string(type)] = type;
  return type;
}

SP<type_t> type_registry_t::untyped_literal(const std::string &value,
                                            literal_type_t ty) {
  auto type = std::make_shared<type_t>();
  type->size = 0;
  type->kind = type_kind_t::eUntypedLiteral;
  type->as.literal = new untyped_literal_t {
    .value = value,
    .type = ty
  };
  return type;
}

void
type_registry_t::merge(const type_registry_t &other) {
  registry.insert(other.registry.begin(), other.registry.end());
}
