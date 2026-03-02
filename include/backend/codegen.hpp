#pragma once

#include "backend/analyzer.hpp"
#include "backend/type.hpp"
#include "frontend/ast.hpp"
#include "llvm/IR/Instructions.h"
#include <memory>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Target/TargetMachine.h>

#define VISITOR(name) SP<llvm_value_t> \
    visit_##name(SP<ast_node_t> node)

struct llvm_value_t {
  llvm::Value *value;
  llvm::Type *type;
  bool is_rvalue;
};

struct llvm_scope_t {
  std::map<std::string, SP<llvm_value_t>> symbol_map;
  SP<llvm_scope_t> parent;

  llvm_scope_t(SP<llvm_scope_t> parent) : parent(parent) {}
  SP<llvm_value_t> resolve(const std::string &);
  SP<llvm_value_t> set(const std::string &, const SP<llvm_value_t>);
};

struct codegen_t {
  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::Module> module;
  std::unique_ptr<llvm::IRBuilder<>> builder;

  codegen_t(semantic_info_t &&su);

  void generate();
  void compile_to_object(const std::string &filename);
private:
  semantic_info_t info;
  std::map<llvm::Type *, SP<type_t>> llvm_type_cache;
  std::map<SP<type_t>, llvm::Type *> type_llvm_cache;
  std::vector<SP<llvm_scope_t>> scopes;

  SP<llvm_scope_t> scope();

  void init_target();

  llvm::Type *map_type(SP<ast_node_t>);
  llvm::Type *ensure_type(SP<type_t>);

  llvm::Value *get_address_of_node(SP<ast_node_t> node);

  llvm_value_t load(llvm::Type *type, const llvm_value_t &val);
  llvm_value_t load(SP<llvm_value_t> val);
  llvm_value_t load(const llvm_value_t &val);

  llvm_value_t cast(llvm::Type *type, const llvm_value_t &);

  llvm::Instruction::BinaryOps map_binop_type(llvm::Type *, llvm::Type *, binop_type_t);
  bool is_scalar_binop(binop_type_t);

  // ----------
  //   Visitors
  // ----------
  SP<llvm_value_t> address_of(SP<ast_node_t> node);

  uint32_t field_index(llvm::Type *, const std::string &);
  SP<llvm_value_t> resolve_member(const llvm_value_t &, const std::string &);
  void link_external_symbol(const std::string &name, SP<type_t> type);

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
  VISITOR(struct_initializer);
  VISITOR(function_decl);
  VISITOR(function_impl);

  llvm::TargetMachine *target_machine;
  std::optional<specialized_path_t> current_binding;

  std::optional<std::string> external_name; // For FFI
};
