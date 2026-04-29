#pragma once

#include "backend/analyzer.hpp"
#include "backend/type.hpp"
#include "frontend/ast.hpp"
#include "llvm/IR/Instructions.h"
#include "llvm/Passes/OptimizationLevel.h"
#include <memory>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

#define VISITOR(name) SP<llvm_value_t> visit_##name(SP<ast_node_t> node)

struct llvm_value_t {
  llvm::Value *value;
  llvm::Type  *type;
  bool         is_rvalue;
  SP<type_t>   base_type = nullptr;
};

struct llvm_scope_t {
  std::map<std::string, SP<llvm_value_t>> symbol_map;
  SP<llvm_scope_t>                        parent;

  llvm::Value                *return_value = nullptr;
  llvm::BasicBlock           *exit_block = nullptr, *return_block = nullptr;
  std::vector<SP<ast_node_t>> defer_stack;

  llvm_scope_t(SP<llvm_scope_t> parent);
  llvm_scope_t(const llvm_scope_t &) = delete;

  SP<llvm_value_t>
  resolve(const std::string &);
  SP<llvm_value_t>
  set(const std::string &, const SP<llvm_value_t>);
  llvm_scope_t &
  root();
};

struct codegen_t {
  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::Module>      module;
  std::unique_ptr<llvm::IRBuilder<>> builder;

  codegen_t(std::shared_ptr<source_t> source, semantic_info_t &&su);

  void
  generate();

  std::string
  compile_to_object(std::optional<std::string> filename);
  void
  compile_to_llvm_ir(std::optional<std::string> filename);

  void
  set_opt_level(const std::string &level);

  private:
  semantic_info_t                    info;
  std::map<llvm::Type *, SP<type_t>> llvm_type_cache;
  std::map<SP<type_t>, llvm::Type *> type_llvm_cache;
  std::vector<SP<llvm_scope_t>>      scopes;
  std::shared_ptr<source_t>          source;

  SP<llvm_scope_t>
  scope();

  void
  init_target();

  llvm::Type *map_type(SP<ast_node_t>);
  llvm::Type *ensure_type(SP<type_t>);

  llvm::Value *
  get_address_of_node(SP<ast_node_t> node);

  llvm_value_t
  load(llvm::Type *type, const llvm_value_t &val);
  llvm_value_t
  load(SP<llvm_value_t> val);
  llvm_value_t
  load(const llvm_value_t &val);

  llvm_value_t
  cast(SP<type_t> type, const llvm_value_t &);

  llvm_value_t
  slice_create_from_array(const llvm_value_t &stack_array);
  llvm_value_t
  slice_create_from_parts(SP<type_t>, const llvm_value_t &pointer, const llvm_value_t &size);

  llvm::Instruction::BinaryOps
       map_binop_type(llvm::Type *, llvm::Type *, binop_type_t);
  bool is_scalar_binop(binop_type_t);

  // ----------
  //   Visitors
  // ----------
  SP<llvm_value_t>
  address_of(SP<ast_node_t> node);

  uint32_t
  field_index(llvm::Type *, const std::string &);
  SP<type_t>
  field_type(SP<type_t>, const std::string &);
  SP<llvm_value_t>
  resolve_member(const llvm_value_t &, const std::string &);
  void
  link_external_symbol(const std::string &name, SP<type_t> type);

  llvm::Value *
  decay_contract_to_data(llvm::Value *);
  llvm::Value *
  decay_contract_to_vtable(llvm::Value *);
  void
               contract_emit_dynamic_dispatcher(SP<type_t>, const std::string &, SP<type_t>);
  llvm::Value *contract_emit_static_vtable(SP<type_t>, SP<type_t>);

  llvm::Value *
  string_literal_global_create(const std::string &);

  VISITOR(node);
  VISITOR(binding);
  VISITOR(block);
  VISITOR(call);
  VISITOR(attribute);
  VISITOR(literal);
  VISITOR(symbol);
  VISITOR(declaration);
  VISITOR(address_of);
  VISITOR(binop);
  VISITOR(array_access);
  VISITOR(while);
  VISITOR(for);
  VISITOR(struct);
  VISITOR(enum);
  VISITOR(tuple);
  VISITOR(unary);
  VISITOR(contract);
  VISITOR(assignment);
  VISITOR(cast);
  VISITOR(struct_initializer);
  VISITOR(nil);
  VISITOR(if);
  VISITOR(deref);
  VISITOR(member_access);
  VISITOR(slice_expr);
  VISITOR(defer);
  VISITOR(return);
  VISITOR(array_initializer);
  VISITOR(pointer_coerce);
  VISITOR(function_decl);
  VISITOR(function_impl);

  VISITOR(zero);
  VISITOR(uninitialized);

  llvm::TargetMachine                           *target_machine;
  std::optional<specialized_path_t>              current_binding;
  std::optional<llvm::GlobalValue::LinkageTypes> current_linkage;

  llvm::OptimizationLevel    opt_level;
  std::optional<std::string> external_name; // For FFI
};
