#pragma once

#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <map>
#include <optional>

#include "frontend/path.hpp"
#include "frontend/source.hpp"
#include "frontend/token.hpp"
#include "backend/type.hpp"

template<typename T>
using SP = std::shared_ptr<T>;

struct type_decl_t;
struct declaration_t;
struct binop_expr_t;

enum class binop_type_t {
  eAdd,
  eSubtract,
  eDivide,
  eMultiply,
  eAnd,
  eOr,
  eEqual,
  eNotEqual,
  eGT,
  eLT,
  eLTE,
  eGTE,
  eMod,
  eBitAnd,
  eBitOr,
  eBitShiftLeft,
  eBitShiftRight,
  eXor
};

struct unary_expr_t;
struct symbol_expr_t;
struct struct_decl_t;
struct block_node_t;
struct function_decl_t;
struct function_impl_t;
struct extern_decl_t;
struct return_stmt_t;
struct call_expr_t;
struct literal_expr_t;
struct addr_of_expr_t;
struct self_decl_t;
struct function_parameter_t;
struct if_stmt_t;
struct for_stmt_t;
struct while_stmt_t;
struct type_alias_decl_t;
struct cast_expr_t;
struct deref_expr_t;
struct attribute_decl_t;
struct binding_decl_t;
struct template_decl_t;
struct struct_expr_t; // Struct initialization
struct struct_member_t;
struct range_expr_t;
struct contract_decl_t;
struct defer_expr_t;
struct move_expr_t;
struct assign_expr_t;
struct array_access_expr_t;
struct sizeof_expr_t;
struct slice_expr_t;
struct array_initialize_expr_t;
struct tuple_expr_t;
struct member_access_expr_t;
struct enum_decl_t;
struct pointer_coerce_expr_t;

enum class literal_type_t {eString, eInteger, eFloat, eBool};

#define LIST_OF_AST_TYPES \
  X(eInvalid)                \
  X(eType)                   \
  X(eDeclaration)            \
  X(eBinop)                  \
  X(eUnary)                  \
  X(eSymbol)                 \
  X(eStructDecl)             \
  X(eBlock)                  \
  X(eFunctionDecl)           \
  X(eFunctionImpl)           \
  X(eExtern)                 \
  X(eReturn)                 \
  X(eCall)                   \
  X(eLiteral)                \
  X(eSelf)                   \
  X(eMemberAccess)           \
  X(eAddrOf)                 \
  X(eFunctionParameter)      \
  X(eIf)                     \
  X(eTypeAlias)              \
  X(eCast)                   \
  X(eAssignment)             \
  X(eDeref)                  \
  X(eNil)                    \
  X(eAttribute)              \
  X(eFor)                    \
  X(eWhile)                  \
  X(eBinding)                \
  X(eStructExpr)             \
  X(eRangeExpr)              \
  X(eContract)               \
  X(eDefer)                  \
  X(eMove)                   \
  X(eTemplate)               \
  X(eArrayAccess)            \
  X(eSizeOf)                 \
  X(eSliceExpr)              \
  X(eArrayInitializeExpr)    \
  X(eTupleExpr)              \
  X(eEnumDecl)               \
  X(eZero)                   \
  X(eUninitialized)          \
  X(ePointerCoerce)

struct ast_node_t {
  ~ast_node_t();
  ast_node_t(const ast_node_t &);
  ast_node_t() = default;
  void reset();

  enum kind_t {
#define X(name) name,
    LIST_OF_AST_TYPES
#undef X
  } kind;
  struct {
    union {
      type_decl_t *type;
      declaration_t *declaration;
      binop_expr_t *binop;
      unary_expr_t *unary;
      symbol_expr_t *symbol;
      struct_decl_t *struct_decl;
      block_node_t *block;
      function_decl_t *fn_decl;
      function_impl_t *fn_impl;
      function_parameter_t *fn_param;
      extern_decl_t *extern_decl;
      return_stmt_t *return_stmt;
      call_expr_t *call_expr;
      literal_expr_t *literal_expr;
      member_access_expr_t *member_access;
      addr_of_expr_t *addr_of;
      if_stmt_t *if_stmt;
      type_alias_decl_t *alias_decl;
      cast_expr_t *cast;
      assign_expr_t *assign_expr;
      deref_expr_t *deref_expr;
      attribute_decl_t *attribute_decl;
      for_stmt_t *for_stmt;
      while_stmt_t *while_stmt;
      binding_decl_t *binding_decl;
      template_decl_t *template_decl;
      struct_expr_t *struct_expr;
      range_expr_t *range_expr;
      contract_decl_t *contract_decl;
      defer_expr_t *defer_expr;
      move_expr_t *move_expr;
      array_access_expr_t *array_access_expr;
      sizeof_expr_t *sizeof_expr;
      slice_expr_t *slice_expr;
      array_initialize_expr_t *array_initialize_expr;
      tuple_expr_t *tuple_expr;
      enum_decl_t *enum_decl;
      pointer_coerce_expr_t *pointer_coerce_expr;
      void *raw;
    };
  } as;
  source_location_t location;
  SP<type_t> type; //< Type for code generation
  SP<source_t> source; //< Source file this node is from
};

struct tuple_decl_t;
struct union_decl_t;

struct type_decl_t {
  // !u8
  specialized_path_t name; //< u8
  std::vector<pointer_kind_t> indirections;
  bool is_mutable; //< is_mutable = var !/?, only applicable to pointers & slices

  bool is_slice = false;
  SP<ast_node_t> len; //< Stack array if not nullptr
  SP<tuple_decl_t> tuple;
  SP<union_decl_t> union_;
};

struct tuple_decl_t {
  std::vector<std::pair<std::optional<std::string>, type_decl_t>> elements;
};

struct union_decl_t {
  std::map<std::string, type_decl_t> values;
};

struct contract_decl_t {
  std::vector<SP<ast_node_t>> requirements;
};

struct declaration_t {
  // let x: i32 = 1;
  // x: i32;
  std::string identifier; //< x
  std::optional<type_decl_t> type; //< i32
  SP<ast_node_t> value; //< 1
  bool is_mutable; //< let/var, default = false
};

struct binop_expr_t {
  // 1 + 2
  binop_type_t op; // eAdd
  SP<ast_node_t> left, right; //< 1, 2
};

struct unary_expr_t {
  // !true
  token_type_t op; //< !
  SP<ast_node_t> value; //< true
};

struct symbol_expr_t {
  // printf("...", variable)
  // std.io.print("...")

  //std::string identifier; //< print/variable
  specialized_path_t path;
};

struct struct_decl_t {
  // struct name { name: !u8; age: i32; };
  std::string name;
  std::vector<struct_member_t> members;
};

struct enum_decl_t {
  std::map<std::string, int64_t> values;
};


struct block_node_t {
  std::vector<SP<ast_node_t>> body;
  bool has_implicit_return;
  SP<type_t> resolved_return_type = nullptr;
};

struct function_decl_t {
  type_decl_t return_type; //< i32
  std::vector<function_parameter_t> parameters; //< file: !u8, statbuf: !any
  bool is_var_args = false;
};

struct binding_decl_t {
  specialized_path_t name;
  SP<ast_node_t> value;
  std::optional<type_decl_t> type;
};

struct template_decl_t {
  template_path_t name;
  SP<ast_node_t> value;
  std::optional<type_decl_t> type;
};

struct function_impl_t {
  function_decl_t declaration;
  SP<ast_node_t> block;
};

struct extern_decl_t {
  // extern "C" fn <i32> stat(file: !u8, statbuf: !any)
  std::string convention; //< C
  SP<ast_node_t> import;
};

struct return_stmt_t {
  SP<ast_node_t> value;
};

struct call_expr_t {
  SP<ast_node_t> callee;
  std::vector<SP<ast_node_t>> arguments;
  SP<ast_node_t> implicit_receiver;
};

struct literal_expr_t {
  std::string value;
  literal_type_t type;
};

struct member_access_expr_t {
  SP<ast_node_t> object;
  std::string member;
};

struct addr_of_expr_t {
  SP<ast_node_t> value;
};

struct self_decl_t {
  bool is_pointer;
  bool is_mutable;
  SP<ast_node_t> specifier;
};

struct function_parameter_t {
  std::string name;
  type_decl_t type;
  bool is_mutable = false;
  bool is_self = false;
  bool is_self_ref = false;
  bool is_rvalue = false;

  SP<type_t> resolved_type;
};

struct if_stmt_t {
  SP<ast_node_t> condition,
    pass,
    reject;
};

struct type_alias_decl_t {
  type_decl_t type;
  bool is_distinct;
};

struct cast_expr_t {
  SP<ast_node_t> value;
  type_decl_t type;
};

struct assign_expr_t {
  SP<ast_node_t> where;
  SP<ast_node_t> value;
};

struct deref_expr_t {
  SP<ast_node_t> value;
};

struct attribute_decl_t {
  std::map<std::string, literal_expr_t> attributes;
  SP<ast_node_t> affect;
};

struct for_stmt_t {
  SP<ast_node_t> init,
    condition,
    action; //< What happens after every iteration.
  SP<ast_node_t> body;
};

struct while_stmt_t {
  SP<ast_node_t> condition;
  SP<ast_node_t> body;
};

struct range_expr_t {
  SP<ast_node_t> min, max;
  bool is_inclusive;
};

struct struct_member_t {
  std::string name;
  type_decl_t type;
};

struct struct_expr_t {
  SP<ast_node_t> type;
  std::map<std::string, SP<ast_node_t>> values;
};

struct defer_expr_t {
  SP<ast_node_t> action;
};

struct move_expr_t {
  SP<ast_node_t> symbol;
};

struct array_access_expr_t {
  SP<ast_node_t> value;
  SP<ast_node_t> offset;
};

struct sizeof_expr_t {
  specialized_path_t value;
};

struct slice_expr_t {
  type_decl_t type;
  SP<ast_node_t> pointer;
  SP<ast_node_t> size;
  SP<type_t> resolved_type;
};

struct array_initialize_expr_t {
  std::vector<SP<ast_node_t>> values;
};

struct tuple_expr_t {
  std::vector<std::pair<std::optional<std::string>, SP<ast_node_t>>> elements;
};

struct pointer_coerce_expr_t {
  SP<ast_node_t> value;
};

std::string to_string(const type_decl_t &);

void dump_ast(ast_node_t &, size_t indent = 0);

template <typename Data>
SP<ast_node_t>
make_node(ast_node_t::kind_t kind, Data data, source_location_t loc, SP<source_t> source) {
  auto node = std::make_shared<ast_node_t>();
  node->as.raw = new Data{std::move(data)};
  node->kind = kind;
  node->location = loc;
  node->source = source;
  return node;
}

SP<ast_node_t>
make_node(ast_node_t::kind_t kind, source_location_t loc, SP<source_t>);
