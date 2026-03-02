#include "backend/codegen.hpp"
#include "backend/analyzer.hpp"
#include "backend/type.hpp"
#include "frontend/ast.hpp"
#include "frontend/path.hpp"
#include "frontend/token.hpp"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/CmpPredicate.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"

#include <filesystem>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/MC/TargetRegistry.h>

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>

#include <memory>
#include <optional>
#include <stdexcept>

#ifdef VISITOR
#undef VISITOR
#endif

#define VISITOR(name) SP<llvm_value_t> \
    codegen_t::visit_##name(SP<ast_node_t> node)

template<typename T> using SP = std::shared_ptr<T>;

SP<llvm_value_t>
llvm_scope_t::set(const std::string &name, SP<llvm_value_t> value) {
  symbol_map[name] = value;
  return value;
}

SP<llvm_value_t>
llvm_scope_t::resolve(const std::string &name) {
  if (symbol_map.contains(name))
    return symbol_map[name];

  if (parent)
    return parent->resolve(name);

  return nullptr;
}

SP<llvm_scope_t> codegen_t::scope() {
  return scopes.back();
}

void
codegen_t::init_target() {
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();

  auto target_triple = llvm::Triple(llvm::sys::getDefaultTargetTriple());
  module->setTargetTriple(target_triple);

  std::string error;
  auto target = llvm::TargetRegistry::lookupTarget(target_triple, error);

  llvm::TargetOptions opts;
  auto pic_model = llvm::Reloc::Model::PIC_;
  target_machine = target->createTargetMachine(target_triple, "generic", "", opts, pic_model);

  module->setDataLayout(target_machine->createDataLayout());
}

void codegen_t::compile_to_object(const std::string &filename) {
  std::error_code EC;

  auto ir_file = std::filesystem::path(filename)
                     .filename()
                     .replace_extension(".ll")
                     .string();

  auto obj_file = std::filesystem::path(filename)
                     .filename()
                     .replace_extension(".o")
                     .string();

  if (true) {
    llvm::raw_fd_ostream ir_dest(ir_file, EC, llvm::sys::fs::OF_None);
    module->print(ir_dest, nullptr);
    module->print(llvm::outs(), nullptr);
  }

  llvm::raw_fd_ostream obj_dest(obj_file, EC, llvm::sys::fs::OF_None);
  llvm::legacy::PassManager pass;
  auto file_type = llvm::CodeGenFileType::ObjectFile;

  if (target_machine->addPassesToEmitFile(pass, obj_dest, nullptr, file_type)) {
    throw std::runtime_error("Target Machine can't emit object file.");
  }

  pass.run(*module);
  obj_dest.flush();
}

codegen_t::codegen_t(semantic_info_t &&s) : info(std::move(s)) {
  context = std::make_unique<llvm::LLVMContext>();
  module = std::make_unique<llvm::Module>("jcc_module", *context);
  builder = std::make_unique<llvm::IRBuilder<>>(*context);

  scopes.push_back(std::make_shared<llvm_scope_t>(nullptr));

  init_target();
}

void codegen_t::link_external_symbol(const std::string &name, SP<type_t> type) {
    llvm::Type *llvm_type = ensure_type(type);

    switch (type->kind) {
    case type_kind_t::eFunction: {
        auto *func_type = llvm::cast<llvm::FunctionType>(llvm_type);

        auto func_callee = module->getOrInsertFunction(name, func_type);
        auto *func = llvm::cast<llvm::Function>(func_callee.getCallee());

        func->setLinkage(llvm::GlobalValue::ExternalLinkage);

        auto value = std::make_shared<llvm_value_t>(func, llvm_type, false);
        scope()->set(name, value);
        break;
    }
    default: {
        llvm::GlobalVariable *variable = new llvm::GlobalVariable(
            *module,
            llvm_type,
            false,
            llvm::GlobalValue::ExternalLinkage,
            nullptr,
            name
        );

        auto value = std::make_shared<llvm_value_t>(variable, llvm_type, true);
        scope()->set(name, value);
        break;
    }
    }
}

void
codegen_t::generate() {
  for (auto &[external, type] : info.imported_symbols) {
    link_external_symbol(external, type);
  }

  for (auto &node : info.unit.declarations) {
    visit_node(node);
  }
}

llvm::Type *codegen_t::ensure_type(SP<type_t> type) {
  if (type_llvm_cache.contains(type)) {
    return type_llvm_cache.at(type);
  }

  llvm::Type* result = nullptr;

  switch (type->kind) {
  case type_kind_t::eInt:
  case type_kind_t::eUint:
    result = llvm::Type::getIntNTy(*context, type->size);
    break;
  case type_kind_t::eBool:
    result = llvm::Type::getInt1Ty(*context);
    break;
  case type_kind_t::eStruct: {
    result = llvm::StructType::getTypeByName(*context, to_string(type->name));
    if (!result) {
      auto st = llvm::StructType::create(*context, to_string(type->name));

      std::vector<llvm::Type *> fields;
      auto layout = type->as.struct_layout;
      for (auto &member : layout->members) {
        fields.push_back(ensure_type(member.type));
      }
      st->setBody(fields);
      result = st;
    }
    break;
  }
  case type_kind_t::eEnum: {
    // TODO: Data size should be changeable
    result = llvm::Type::getInt32Ty(*context);
    break;
  }
  case type_kind_t::eTuple: {
    std::vector<llvm::Type *> types;
    for (auto &[_, v] : type->as.tuple->elements) {
      types.push_back(ensure_type(v));
    }
    result = llvm::StructType::create(types, to_string(type->name), true);
    break;
  }
  case type_kind_t::eFunction: {
    auto fn = type->as.function;
    llvm::Type* ret_ty = ensure_type(fn->return_type);
    std::vector<llvm::Type*> params;
    for (auto &p : fn->arg_types) {
      auto param_ty = ensure_type(p);
      params.push_back(param_ty);
    }

    if (fn->receiver) {
      params.insert(params.begin(), ensure_type(fn->receiver));
    }

    result = llvm::FunctionType::get(ret_ty, params, fn->is_var_args);
    break;
  }
  case type_kind_t::eFloat: {
    result = type->size == 32 ? llvm::Type::getFloatTy(*context) : llvm::Type::getDoubleTy(*context);
    break;
  }
  case type_kind_t::eOpaque:
  case type_kind_t::eAlias: {
    result = ensure_type(type->as.alias->alias);
    break;
  }
  case type_kind_t::ePointer: {
    result = llvm::PointerType::get(*context, 0);
    break;
  }
  case type_kind_t::eVoid: {
    result = llvm::Type::getVoidTy(*context);
    break;
  }
  case type_kind_t::eArray: {
    array_t *arr = type->as.array;
    result = llvm::ArrayType::get(ensure_type(arr->element_type), arr->size);
    break;
  }
  case type_kind_t::eSlice: {
    // e.g., "slice.i8" or "slice.i32"
    std::string name = "slice." + to_string(type->as.slice->element_type);

    llvm::StructType *st;
    if (st = llvm::StructType::getTypeByName(*context, name); st) {
      result = st;
    } else {
      st = llvm::StructType::create(*context, name);
      st->setBody({
          builder->getPtrTy(), // The raw pointer
          builder->getInt64Ty() // The length (u64/size_t)
        });
      result = st;
    }
    break;
  }
  default:
    assert(false && "Internal Compiler Error: Unhandled type in LLVM type mapping");
  }

  // Cache the result
  type_llvm_cache[type] = result;
  llvm_type_cache[result] = type;
  return result;
}

llvm_value_t
codegen_t::load(llvm::Type *type, const llvm_value_t &val) {
    // If it's already a constant or a temporary result, don't load
    if (llvm::isa<llvm::Constant>(val.value)) {
        return val;
    }

    // If it's a pointer (like an alloca), we need the value inside
    if (!val.is_rvalue) {
      return llvm_value_t {builder->CreateLoad(type, val.value), type, true};
    }
    return val;
}

llvm_value_t
codegen_t::load(SP<llvm_value_t> value) {
  return load(value->type, *value);
}

llvm_value_t
codegen_t::load(const llvm_value_t &value) {
  return load(value.type, value);
}

llvm_value_t
codegen_t::cast(llvm::Type *type, const llvm_value_t &value) {
  if (type->isIntegerTy() && value.type->isIntegerTy()) {
    return llvm_value_t{builder->CreateZExtOrTrunc(load(value).value, type),
                        type, true};
  }
  throw std::runtime_error ("Unhandled cast");
}

uint32_t
codegen_t::field_index(llvm::Type *ty, const std::string &name) {
  auto type = llvm_type_cache.at(ty);

  switch (type->kind) {
  case type_kind_t::ePointer: {
    // Pointers automatically get dereferenced.
    return field_index(ensure_type(type->as.pointer->deref()), name);
  }
  case type_kind_t::eStruct: {
    auto struct_layout = type->as.struct_layout;
    auto member = struct_layout->member(name);
    return std::distance(struct_layout->members.data(), member);
  }
  case type_kind_t::eTuple: {
    auto tuple_layout = type->as.tuple;
    auto member = tuple_layout->element(name);
    return std::distance(tuple_layout->elements.cbegin(), member);
  }
  default:
    throw std::runtime_error("Unknown type in field_index");
  }
}

SP<llvm_value_t>
codegen_t::address_of(SP<ast_node_t> node) {
  addr_of_expr_t *expr = node->as.addr_of;
  switch (expr->value->kind) {
  case ast_node_t::eSymbol: {
    auto id = to_string(expr->value->as.symbol->path);
    // The result of an address of is an rvalue (don't need to load,
    // that would be a deref.)
    auto val = scopes.back()->resolve(id);
    return std::make_shared<llvm_value_t>(val->value, val->type, true);
  }
  case ast_node_t::eDeref: {
    auto result = visit_node(node->as.deref_expr->value);
    return std::make_shared<llvm_value_t>(load(ensure_type(node->type), *result));
  }
  default:
    throw std::runtime_error("Non lvalue address of doesn't work.");
  }
  return nullptr;
}

SP<llvm_value_t>
codegen_t::visit_binding(SP<ast_node_t> node) {
  binding_decl_t *decl = node->as.binding_decl;
  current_binding = decl->name;
  return scope()->set(to_string(decl->name), visit_node(decl->value));
}

VISITOR(function_decl) {
  function_decl_t *decl = node->as.fn_decl;

  std::string name = external_name ? external_name.value() : to_string(*current_binding);
  bool is_import = external_name.has_value();

  auto llvm_type = ensure_type(node->type);
  llvm::FunctionType *llvm_fn = llvm::dyn_cast<llvm::FunctionType>(llvm_type);

  auto func = llvm::Function::Create(llvm_fn, llvm::GlobalValue::ExternalLinkage, name, *module);

  if (is_import && to_string(*current_binding) != name) {
    // Functions that are imported will receive a wrapper implementation.
    //
    // This is due to GlobalAlias not working on imported symbols.

    auto *wrapper = llvm::Function::Create(llvm_fn, llvm::GlobalValue::ExternalLinkage, to_string(*current_binding), *module);

    auto *bb = llvm::BasicBlock::Create(*context, "entry", wrapper);
    builder->SetInsertPoint(bb);

    std::vector<llvm::Value*> args;
    for (auto &arg : wrapper->args()) args.push_back(&arg);
    auto *call = builder->CreateCall(llvm_fn, func, args);

    if (llvm_fn->getReturnType()->isVoidTy()) {
      builder->CreateRetVoid();
    } else {
      builder->CreateRet(call);
    }

    // Always inline, these functions have to be "invisible" to the
    // machine.
    wrapper->addFnAttr(llvm::Attribute::AlwaysInline);

    return scope()->set(to_string(*current_binding), std::make_shared<llvm_value_t>(
                          wrapper, llvm_type, false));
  }

  return scope()->set(to_string(*current_binding), std::make_shared<llvm_value_t>(func, llvm_fn, false));
}

VISITOR(attribute) {
  attribute_decl_t *attr = node->as.attribute_decl;

  if (attr->attributes.contains("extern")) {
    external_name = attr->attributes.at("import").value;
  }

  auto value = visit_node(attr->affect);

  if (attr->attributes.contains("extern")) {
    external_name = std::nullopt;
  }

  return value;
}

VISITOR(function_impl) {
  function_impl_t *impl = node->as.fn_impl;

  auto llvm_type = ensure_type(node->type);
  llvm::FunctionType *llvm_fn = llvm::dyn_cast<llvm::FunctionType>(llvm_type);

  auto func = llvm::Function::Create(llvm_fn, llvm::GlobalValue::ExternalLinkage, to_string(*current_binding), *module);
  auto ret_val = scope()->set(to_string(*current_binding), std::make_shared<llvm_value_t>(func, llvm_fn, false));

  llvm::BasicBlock *entry = llvm::BasicBlock::Create(*context, "entry", func);

  auto current_block = builder->GetInsertBlock();

  builder->SetInsertPoint(entry);

  scopes.emplace_back(std::make_shared<llvm_scope_t>(scopes.back()));

  for (uint64_t i = 0; i < impl->declaration.parameters.size(); ++i) {
    auto &param = impl->declaration.parameters[i];
    auto ty = ensure_type(param.resolved_type);
    auto llvm_value = func->args().begin() + i;
    scope()->set(param.name, std::make_shared<llvm_value_t>(llvm_value, ty, true));
  }

  auto result = visit_node(impl->block);
  if (result) {
    builder->CreateRet(result->value);
  } else {
    builder->CreateRetVoid();
  }

  builder->SetInsertPoint(current_block);
  scopes.pop_back();
  return ret_val;
}

VISITOR(call) {
  call_expr_t *call = node->as.call_expr;

  llvm::FunctionType *func = llvm::dyn_cast<llvm::FunctionType>(ensure_type(call->callee->type));

  std::vector<llvm::Value *> args;
  for (auto &arg : call->arguments) {
    auto value = visit_node(arg);
    args.push_back(load(ensure_type(arg->type), *value).value);
  }

  if (call->implicit_receiver) {
    auto receiver = visit_node(call->implicit_receiver);
    args.insert(args.begin(), load(ensure_type(call->implicit_receiver->type), *receiver).value);
  }

  auto callee = visit_node(call->callee);
  llvm::Value *value = builder->CreateCall(func, callee->value, args);

  return std::make_shared<llvm_value_t>(value, func->getReturnType(), true);
}

VISITOR(block) {
  block_node_t *block = node->as.block;

  SP<llvm_value_t> value {nullptr};

  for (auto &v : block->body) {
    value = visit_node(v);
  }

  if (block->has_implicit_return)
    return std::make_shared<llvm_value_t>(load(value).value, value->type, true);
  else
    return nullptr;
}

VISITOR(declaration) {
  declaration_t *decl = node->as.declaration;

  llvm::Type *value_type = ensure_type(node->type);
  llvm::Value *storage = builder->CreateAlloca(value_type);

  // Is the declaration an lvalue (i.e. has an address and requires
  // loading)
  bool is_lvalue = true;
  if (decl->value) {
    switch (decl->value->kind) {
    case ast_node_t::eZero:
      // Explicit `zero` keyword.
      builder->CreateStore(llvm::ConstantInt::getNullValue(value_type), storage);
      break;

    case ast_node_t::eUninitialized:
      // Explicit `uninitialized` keyword
      break;

    default: {
      auto init = visit_node(decl->value);
      auto val = load(init);

      if (val.type != value_type) {
        val = cast(value_type, val);
      }

      builder->CreateStore(val.value, storage);
      break;
    }
    }
  } else {
    // Zero initialize by default
    builder->CreateStore(llvm::ConstantInt::getNullValue(value_type), storage);
  }

  if (node->type->kind == type_kind_t::eArray) {
    // Stack arrays don't need loading, they are stack addresses.
    is_lvalue = false;
  }

  return scopes.back()->set(decl->identifier, std::make_shared<llvm_value_t>(storage, value_type, !is_lvalue));
}

VISITOR(literal) {
  literal_expr_t *expr = node->as.literal_expr;

  llvm::Type *type = ensure_type(node->type);
  llvm::Value *value = nullptr;
  switch (expr->type) {
  case literal_type_t::eInteger: {
    // TODO: Can't handle uint64_t
    value = llvm::ConstantInt::get(type, std::stoll(expr->value));
    break;
  }
  case literal_type_t::eBool: {
    value = llvm::ConstantInt::get(type, expr->value == "true");
    break;
  }
  case literal_type_t::eFloat: {
    value = llvm::ConstantFP::get(type, std::stof(expr->value));
    break;
  }
  case literal_type_t::eString: {
    value = builder->CreateGlobalString(expr->value, "", 0, module.get());
    break;
  }
  default:
    assert(false && "Internal Compiler Error: unsupported literal");
    break;
  }
  return std::make_shared<llvm_value_t>(value, type, true);
}

SP<llvm_value_t>
codegen_t::resolve_member(const llvm_value_t &val, const std::string &member) {
  try {
    llvm::Value *base_ptr = val.value;
    auto base_type = llvm_type_cache.at(val.type);

    // Auto deref pointers until we reach the base type.
    while (base_type->kind == type_kind_t::ePointer) {
      base_type = base_type->as.pointer->deref();
      base_ptr = builder->CreateLoad(ensure_type(base_type), base_ptr);
    }

    llvm::Type *llvm_type = ensure_type(base_type);

    int field_idx = field_index(llvm_type, member);
    if (field_idx == -1) {
      throw std::runtime_error("Member " + member + " not found on struct");
    }

    llvm::Value* member_ptr = builder->CreateStructGEP(llvm_type, val.value, field_idx, member);
    llvm::Type* member_type = llvm::cast<llvm::StructType>(llvm_type)->getElementType(field_idx);

    return std::make_shared<llvm_value_t>(member_ptr, member_type, false);
  } catch (...) {
    return nullptr;
  }
}

VISITOR(symbol) {
  symbol_expr_t *symbol = node->as.symbol;

  // Fully specified path
  auto value = scope()->resolve(to_string(symbol->path));
  if (value)
    return value;

  // Template
  if (info.template_instantiations.contains(symbol->path)) {
    auto template_ = info.template_instantiations.at(symbol->path);

    current_binding = symbol->path;
    visit_node(template_);

    return scope()->resolve(to_string(symbol->path));
  }

  // Member lookups
  auto path = symbol->path;
  SP<llvm_value_t> left {nullptr};

  specialized_segment_t segment = path.segments.front();
  left = scope()->resolve(segment.name);
  // Erase first segment
  path.segments.erase(path.segments.begin());

  do {
    // Get next one.
    segment = path.segments.front();

    auto member = resolve_member(*left, segment.name);
    if (member == nullptr) {
      // If we couldn't resolve this segment, it might be a UFCS function.
      break;
    }

    left = member;
    path.segments.erase(path.segments.begin());
  } while (left && path.segments.size() > 0);

  if (left && path.segments.size() == 0) {
    return left;
  }

  if (left) {
    // UFCS
    specialized_path_t ufcs = path;
    ufcs.segments.insert(ufcs.segments.begin(), symbol->path.segments.begin(), symbol->path.segments.end() - ufcs.segments.size());

    left = scope()->resolve(to_string(ufcs));
    return left;
  }

  return left;
}

VISITOR(address_of) {
  return address_of(node);
}

llvm::Instruction::BinaryOps
codegen_t::map_binop_type(llvm::Type *left, llvm::Type *right, binop_type_t ty) {
  using I = llvm::Instruction;

  bool is_float = left->isFloatingPointTy() || right->isFloatingPointTy();

  switch (ty) {
  case binop_type_t::eAdd:
    return is_float ? I::FAdd : I::Add;

  case binop_type_t::eSubtract:
    return is_float ? I::FSub : I::Sub;

  case binop_type_t::eDivide:
    // TODO: SDiv vs UDiv
    return is_float ? I::FDiv : I::SDiv;

  case binop_type_t::eMultiply:
    return is_float ? I::FMul : I::Mul;

  case binop_type_t::eAnd:
    return I::And;

  case binop_type_t::eOr:
    return I::Or;

  case binop_type_t::eMod:
    return is_float ? I::FRem : I::SRem;

  default:
    throw std::runtime_error ("Internal Compiler Error: Unhandled binop type");
  }
}

bool codegen_t::is_scalar_binop(binop_type_t ty) {
  using T = binop_type_t;
  switch (ty) {
  case T::eAdd:
  case T::eSubtract:
  case T::eDivide:
  case T::eMultiply:
  case T::eAnd:
  case T::eOr:
  case T::eMod:
    return true;
  default:
    return false;
  }
}

VISITOR(binop) {
  binop_expr_t *expr = node->as.binop;

  llvm::Type *left_type = ensure_type(expr->left->type);
  llvm::Type *right_type = ensure_type(expr->right->type);
  llvm::Type *result_type = nullptr;

  auto left = visit_node(expr->left);
  auto right = visit_node(expr->right);

  if (left_type->isIntOrPtrTy() && right_type->isIntOrPtrTy()) {
    // Figure out the bitsize of each type, we coalesce to the biggest
    // variant.
    if (left_type->getIntegerBitWidth() > right_type->getIntegerBitWidth())
      result_type = left_type;
    else
      result_type = right_type;

    if (is_scalar_binop(expr->op)) {
      if (left_type != result_type) {
        left = std::make_shared<llvm_value_t>(builder->CreateIntCast(load(left_type, *left).value, result_type, true), result_type, true);
        left_type = result_type;
      }

      if (right_type != result_type) {
        right = std::make_shared<llvm_value_t>(builder->CreateIntCast(load(right_type, *right).value, result_type, true), result_type, true);
        right_type = result_type;
      }

      auto intermediate = builder->CreateBinOp(map_binop_type(left_type, right_type, expr->op), load(left_type, *left).value, load(right_type, *right).value);
      return std::make_shared<llvm_value_t>(builder->CreateZExtOrTrunc(intermediate, result_type), result_type, true);
    } else {
      // Probably comparision
      bool is_float = left_type->isFloatingPointTy() || right_type->isFloatingPointTy();
      llvm::Value *value = nullptr;

      using P = llvm::CmpInst::Predicate;
      P inst;
      switch (expr->op) {
      case binop_type_t::eEqual:
        inst = is_float ? P::FCMP_OEQ : P::ICMP_EQ;
        break;

      case binop_type_t::eNotEqual:
        inst = is_float ? P::FCMP_ONE : P::ICMP_NE;
        break;

      case binop_type_t::eGT:
        inst = is_float ? P::FCMP_OGT : P::ICMP_SGT;
        break;

      case binop_type_t::eLT:
        inst = is_float ? P::FCMP_OLT : P::ICMP_SLT;
        break;

      case binop_type_t::eGTE:
        inst = is_float ? P::FCMP_OGE : P::ICMP_SGE;
        break;

      case binop_type_t::eLTE:
        inst = is_float ? P::FCMP_OLE : P::ICMP_SLE;
        break;

      default:
        throw std::runtime_error ("Internal Compiler Error: Unhandled non-scalar binary operation.");
        break;
      }

      return std::make_shared<llvm_value_t>(builder->CreateICmp(inst, load(left_type, *left).value, load(right_type, *right).value), result_type, true);
    }
  }
  throw std::runtime_error ("Internal Compiler Error: Unhandled types in binary operation.");
  return nullptr;
}

VISITOR(array_access) {
  array_access_expr_t *expr = node->as.array_access_expr;

  auto base_val = visit_node(expr->value);
  auto offset_val = visit_node(expr->offset);

  llvm::Value* base = base_val->value;
  llvm::Value* offset = load(builder->getInt32Ty(), *offset_val).value;

  auto element_type = expr->value->type->base_type();

  llvm::Value* address = builder->CreateGEP(ensure_type(element_type), base, {offset}, "array_idx");

  return std::make_shared<llvm_value_t>(load(ensure_type(element_type), llvm_value_t(address, ensure_type(pointer_t::pointer_to(pointer_kind_t::eNonNullable, element_type, true)), false)));
}

VISITOR(for) {
    auto* stmt = node->as.for_stmt;
    auto* function = builder->GetInsertBlock()->getParent();

    scopes.emplace_back(scopes.back());

    // Standard blocks
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(*context, "for.cond", function);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "for.body", function);
    llvm::BasicBlock* actionBB = llvm::BasicBlock::Create(*context, "for.action", function);
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "for.merge", function);

    auto init = visit_node(stmt->init);
    builder->CreateBr(condBB);

    // 2. Handle Condition (Specialized for Ranges)
    builder->SetInsertPoint(condBB);

    if (stmt->condition && stmt->condition->kind == ast_node_t::eRangeExpr) {
        auto* range = stmt->condition->as.range_expr;

        llvm::Value* max = cast(init->type, load(visit_node(range->max))).value;

        // Condition: iter < max (or <= if inclusive)
        llvm::Value* cmp;
        if (range->is_inclusive)
          cmp = builder->CreateICmpSLE(load(init).value, max, "range_cmp");
        else
          cmp = builder->CreateICmpSLT(load(init).value, max, "range_cmp");

        builder->CreateCondBr(cmp, bodyBB, mergeBB);
    } else if (stmt->condition) {
        // Standard boolean condition (for i := 0; i < 10; ...)
        auto cond_val = visit_node(stmt->condition);
        builder->CreateCondBr(cond_val->value, bodyBB, mergeBB);
    } else {
        builder->CreateBr(bodyBB);
    }

    // 3. Body
    builder->SetInsertPoint(bodyBB);
    visit_node(stmt->body);
    builder->CreateBr(actionBB);

    // 4. Action (Specialized for Ranges)
    builder->SetInsertPoint(actionBB);

    if (stmt->condition && stmt->condition->kind == ast_node_t::eRangeExpr) {
      builder->CreateStore(
          builder->CreateBinOp(
              llvm::Instruction::Add, load(init->type, *init).value,
              builder->getIntN(init->type->getIntegerBitWidth(), 1)),
          init->value);
    } else if (stmt->action) {
        // Case: for i := 0; i < 10; i = i + 1 -> Manual action
        visit_node(stmt->action);
    }

    builder->CreateBr(condBB);
    builder->SetInsertPoint(mergeBB);

    scopes.pop_back();
    return nullptr;
}

VISITOR(while) {
    auto* stmt = node->as.while_stmt;
    auto* function = builder->GetInsertBlock()->getParent();

    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(*context, "while.cond", function);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "while.body", function);
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "while.merge", function);

    builder->CreateBr(condBB);

    builder->SetInsertPoint(condBB);
    auto cond_val = visit_node(stmt->condition);
    builder->CreateCondBr(cond_val->value, bodyBB, mergeBB);

    builder->SetInsertPoint(bodyBB);
    scopes.emplace_back(scopes.back());
    visit_node(stmt->body);
    scopes.pop_back();

    builder->CreateBr(condBB);

    builder->SetInsertPoint(mergeBB);
    return nullptr;
}

VISITOR(struct) {
  llvm::StructType *type = llvm::dyn_cast<llvm::StructType>(ensure_type(node->type));
  return nullptr;
}

VISITOR(enum) {
  auto enum_type = ensure_type(node->type);

  auto enum_layout = node->type->as.enum_;
  auto storage_type = builder->getInt32Ty();
  for (auto &[name, val] : enum_layout->values) {
    auto binding = *current_binding;
    binding.push(name);

    scope()->set(to_string(binding), std::make_shared<llvm_value_t>(llvm::ConstantInt::get(storage_type, val), storage_type, true));
  }
  return nullptr;
}

VISITOR(struct_initializer) {
  auto* expr = node->as.struct_expr;
  llvm::Type* struct_ty = ensure_type(node->type);

  llvm::Value* struct_ptr = builder->CreateAlloca(struct_ty, nullptr, "struct_tmp");

  for (auto const& [name, value_ast] : expr->values) {
    uint32_t nfield = field_index(struct_ty, name);

    llvm::Value* field_ptr = builder->CreateStructGEP(struct_ty, struct_ptr, nfield);

    auto val = visit_node(value_ast);
    builder->CreateStore(load(val).value, field_ptr);
  }

  return std::make_shared<llvm_value_t>(struct_ptr, struct_ty);
}

VISITOR(tuple) {
  tuple_expr_t *expr = node->as.tuple_expr;

  auto tuple_type = ensure_type(node->type);

  llvm::Value *tuple_ptr = builder->CreateAlloca(tuple_type, nullptr, "tuple_tmp");

  int64_t nmemb = 0;
  for (auto const &[key, value_ast] : expr->elements) {
    // Tuples can have named indices, unnamed ones are sequentially
    // numbered, while ignoring named ones.
    //
    // This just means that (i32, g: i32, b: i32, i32) has indices (0, g, b, 1)
    auto nfield = field_index(tuple_type, key.has_value() ? *key : std::to_string(nmemb++));

    llvm::Value *field_ptr = builder->CreateStructGEP(tuple_type, tuple_ptr, nfield);

    auto value = visit_node(value_ast);
    builder->CreateStore(load(value).value, field_ptr);
  }

  return std::make_shared<llvm_value_t>(tuple_ptr, tuple_type, false);
}

VISITOR(unary) {
  unary_expr_t *expr = node->as.unary;

  auto value = visit_node(expr->value);

  // ! (negate)
  if (expr->op == token_type_t::operatorExclamation) {
    return std::make_shared<llvm_value_t>(builder->CreateNot(load(value).value, "bool_not"), value->type, true);
  }

  return value;
}

SP<llvm_value_t>
codegen_t::visit_node(SP<ast_node_t> node) {
  if (!node->type) {
    assert(false && "Internal Compiler Error: AST node has no type!");
  }

  SP<llvm_value_t> result = nullptr;
  switch (node->kind) {
  case ast_node_t::eTypeAlias:
    break;

  case ast_node_t::eBinding:
    result = visit_binding(node);
    break;

  case ast_node_t::eFunctionImpl:
    result = visit_function_impl(node);
    break;

  case ast_node_t::eFunctionDecl:
    result = visit_function_decl(node);
    break;

  case ast_node_t::eBlock:
    result = visit_block(node);
    break;

  case ast_node_t::eCall:
    result = visit_call(node);
    break;

  case ast_node_t::eAttribute:
    result = visit_attribute(node);
    break;

  case ast_node_t::eLiteral:
    result = visit_literal(node);
    break;

  case ast_node_t::eSymbol:
    result = visit_symbol(node);
    break;

  case ast_node_t::eDeclaration:
    result = visit_declaration(node);
    break;

  case ast_node_t::eAddrOf:
    result = visit_address_of(node);
    break;

  case ast_node_t::eBinop:
    result = visit_binop(node);
    break;

  case ast_node_t::eArrayAccess:
    result = visit_array_access(node);
    break;

  case ast_node_t::eWhile:
    result = visit_while(node);
    break;

  case ast_node_t::eStructDecl:
    result = visit_struct(node);
    break;

  case ast_node_t::eStructExpr:
    result = visit_struct_initializer(node);
    break;

  case ast_node_t::eEnumDecl:
    result = visit_enum(node);
    break;

  case ast_node_t::eFor:
    result = visit_for(node);
    break;

  case ast_node_t::eTupleExpr:
    result = visit_tuple(node);
    break;

  case ast_node_t::eUnary:
    result = visit_unary(node);
    break;

  default:
    assert(false && "Unexpected AST node in codegen");
  }
  return result;
}
