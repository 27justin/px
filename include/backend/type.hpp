#pragma once

#include "frontend/path.hpp"
#include <memory>
#include <unordered_map>
#include <vector>
#include <map>

template<typename T>
using SP = std::shared_ptr<T>;

struct symbol_t;
struct type_t;
enum class literal_type_t;

struct struct_layout_t {
  struct field_t {
    std::string name;
    SP<type_t> type;
    size_t offset; //< Offset from struct start
  };

  uint64_t alignment = 0, size = 0; //< Alignment in bytes, size in bits
  std::vector<field_t> members;

  field_t *member(const std::string &name);
  void compute_memory_layout();
};

struct type_alias_t {
  SP<type_t> alias;
  bool is_transparent = false; ///< Is the type alias is transparent,
                               ///resolvers should immediately return
                               ///the `alias` of this, instead of the
                               ///alias type itself
};

enum class pointer_kind_t {
  eNullable,
  eNonNullable
};

struct pointer_t {
  // ?!u8    -> nullable pointer to non-nullable
  //            pointer_t { mutable = false, indirections = (eNullable, eNonNullable) }
  // var !u8 -> non-nullable pointer to mutable data
  bool is_mutable = false;
  std::vector<pointer_kind_t> indirections;
  SP<type_t> base;

  SP<type_t> deref() const;
  static SP<type_t> pointer_to(pointer_kind_t, SP<type_t>, bool = false);
};

struct array_t {
  SP<type_t> element_type;
  size_t size;
};

struct slice_t {
  SP<type_t> element_type;
  bool is_mutable;
};

struct tuple_t {
  std::vector<std::pair<std::string, SP<type_t>>> elements;

  std::vector<std::pair<std::string, SP<type_t>>>::const_iterator
  element(const std::string &name) const;
};

struct function_signature_t {
  std::shared_ptr<type_t> return_type;
  std::vector<std::shared_ptr<type_t>> arg_types;
  SP<type_t> receiver;
  bool is_var_args;
};

struct contract_t {
  std::map<std::string, SP<type_t>> requirements;
};

struct rvalue_reference_t {
  SP<type_t> base;
};

struct untyped_literal_t {
  std::string value;
  literal_type_t type;
};

struct enum_t {
  std::map<std::string, int64_t> values;
};

struct union_t {
  std::map<std::string, SP<type_t>> composite;
};

enum type_kind_t { eStruct, eFunction, eInt, eUint, eFloat, ePointer, eOpaque, eAlias, eVoid, eBool, eArray, eSlice, eContract, eSelf, eRValueReference, eUntypedLiteral, eTuple, eEnum, eUnion };
struct type_t {
  type_kind_t kind;
  specialized_path_t name;

  size_t size, alignment; //< Alignment in bytes, size in bits.

  struct {
    union {
      function_signature_t *function;
      struct_layout_t *struct_layout;
      type_alias_t *alias;
      pointer_t *pointer;
      array_t *array;
      slice_t *slice;
      contract_t *contract;
      rvalue_reference_t *rvalue;
      untyped_literal_t *literal;
      tuple_t *tuple;
      enum_t *enum_;
      union_t *union_;
      void *any;
    };
  } as;

  ~type_t() = default;

  bool is_numeric() const;
  bool is_signed() const;

  bool operator ==(const type_t &other) const;

  // Return the "base type".
  // This returns nullptr in every case, except in these:
  //  1. Arrays -> returns the element type
  //  2. Slices -> returns the element type
  //  3. Pointers -> returns next indirection (or concrete)
  SP<type_t> base_type();

  static SP<type_t> make_slice(SP<type_t>, bool is_mutable);
};

/// @brief Return bitsize of type
size_t size_of(SP<type_t>);
size_t alignment_of(SP<type_t>);

size_t size_of(const type_t &);
size_t alignment_of(const type_t &);

std::string to_string(const type_t &);
std::string to_string(SP<type_t>);
