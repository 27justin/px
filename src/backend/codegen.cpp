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

#include <cassert>
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

#include <llvm/Support/Program.h>
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

std::string codegen_t::compile_to_object(std::optional<std::string> filename) {
  std::error_code EC;

  auto obj_file = std::filesystem::path(filename.value_or(std::string(source->name())))
                     .replace_extension(".o")
                     .string();

  llvm::legacy::PassManager pass;
  auto file_type = llvm::CodeGenFileType::ObjectFile;

  if (filename == "-") {
    target_machine->addPassesToEmitFile(pass, llvm::outs(), nullptr, file_type);
    pass.run(*module);
  } else {
    llvm::raw_fd_ostream obj_dest(obj_file, EC, llvm::sys::fs::OF_None);
    if (target_machine->addPassesToEmitFile(pass, obj_dest, nullptr, file_type)) {
      throw std::runtime_error("Target Machine can't emit object file.");
    }
    pass.run(*module);
    obj_dest.flush();
  }
  return obj_file;
}

void codegen_t::compile_to_llvm_ir(std::optional<std::string> filename) {
  std::error_code EC;

  if (filename == "-") {
    module->print(llvm::outs(), nullptr);
  } else {
    llvm::raw_fd_ostream ir_dest(filename.value_or(std::filesystem::path(source->name()).replace_extension(".ll").filename().string()), EC, llvm::sys::fs::OF_None);
    module->print(ir_dest, nullptr);
  }
}


codegen_t::codegen_t(std::shared_ptr<source_t> src, semantic_info_t &&s) : source(src), info(std::move(s)) {
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

        auto value = std::make_shared<llvm_value_t>(func, llvm_type, false, type);
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

        auto value = std::make_shared<llvm_value_t>(variable, llvm_type, true, type);
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

void codegen_t::contract_emit_dynamic_dispatcher(SP<type_t> contract,
                                                 const std::string &symbol_name,
                                                 SP<type_t> requirement_type) {

  auto llvm_type = ensure_type(requirement_type);
  llvm::FunctionType *receiver_fn = llvm::dyn_cast<llvm::FunctionType>(llvm_type);

  std::vector<llvm::Type *> params;
  params.push_back(llvm::StructType::getTypeByName(*context, "contract"));
  for (auto i = 1; i < receiver_fn->getNumParams(); ++i) {
    params.push_back(receiver_fn->getParamType(i));
  }

  llvm::FunctionType *dispatcher_fn = llvm::FunctionType::get(receiver_fn->getReturnType(), params, receiver_fn->isVarArg());

  function_signature_t *signature = requirement_type->as.function;

  specialized_path_t dispatcher_path {contract->name.segments};
  dispatcher_path.segments.push_back(symbol_name);

  auto func = llvm::Function::Create(dispatcher_fn, llvm::GlobalValue::LinkOnceODRLinkage, to_string(dispatcher_path), *module);
  func->setVisibility(llvm::GlobalValue::HiddenVisibility);

  // TODO: Required? LinkOnceODRLinkage seems to have no effect at
  // times, causing multiple definitions.
  func->setComdat(module->getOrInsertComdat(to_string(dispatcher_path)));

  llvm::BasicBlock *entry = llvm::BasicBlock::Create(*context, "entry", func);

  auto current_block = builder->GetInsertBlock();

  builder->SetInsertPoint(entry);

  // Dispatching a contract is done by:
  //  1. Retrieving the vtable pointer
  //  2. Retrieving the data pointer
  //  3. Retrieve the correct offset for the symbol from the vtable
  //  4. Call the retrieved function pointer with data + arguments
  llvm::Value *contract_object = func->args().begin();

  auto vtable = decay_contract_to_vtable(contract_object);
  auto data = decay_contract_to_data(contract_object);

  auto offset = std::distance(contract->as.contract->requirements.begin(), contract->as.contract->requirements.find(symbol_name));

  // Load the `offset`-th pointer from the vtable
  auto func_ptr = builder->CreateStructGEP(llvm::StructType::getTypeByName(*context, "contract." + to_string(contract->name)), vtable, offset, "method_ptr_addr");
  auto dyn_func = builder->CreateLoad(llvm::PointerType::get(*context, 0), func_ptr, "method_ptr");

  std::vector<llvm::Value*> args;
  args.push_back(data);

  for (auto it = func->args().begin() + 1; it != func->args().end(); ++it) {
    args.push_back(it);
  }

  auto ret_val = builder->CreateCall(dispatcher_fn, dyn_func, args, dispatcher_fn->getReturnType()->isVoidTy() ? "" : "vtable_dispatch");

  if (dispatcher_fn->getReturnType()->isVoidTy() == false)
    builder->CreateRet(ret_val);
  else
    builder->CreateRetVoid();

  scope()->set(to_string(dispatcher_path), std::make_shared<llvm_value_t>(func, dispatcher_fn, true));
  builder->SetInsertPoint(current_block);
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
  case type_kind_t::eContract: {
    // Contracts consist of two different structs.
    //
    // First is the vtable struct, this holds the requirements of the contract.
    //
    // Second is the actual contract object, this is a 2 *
    // pointer-size struct that holds both a pointer to the vtable,
    // and a pointer to whatever implements the contract.

    if (result = llvm::StructType::getTypeByName(*context, "contract"); !result) {
      std::vector<llvm::Type *> types {llvm::PointerType::get(*context, 0), llvm::PointerType::get(*context, 0)};
      result = llvm::StructType::create(types, "contract", true);
    }

    if (!llvm::StructType::getTypeByName(*context, "contract." + to_string(type->name))) {
      std::vector<llvm::Type *> types;
      auto ptr_type = llvm::PointerType::get(*context, 0);

      for (auto &[name, req] : type->as.contract->requirements) {
        types.push_back(ptr_type);
      }

      llvm::StructType::create(types, "contract." + to_string(type->name), true);
    }
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
      return llvm_value_t {builder->CreateLoad(type, val.value), type, true, val.base_type};
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
codegen_t::cast(SP<type_t> type, const llvm_value_t &value) {
  llvm::Type *target_type = ensure_type(type);

  if (value.base_type && *value.base_type == *type)
    return value;

  if (target_type->isIntegerTy() && value.type->isIntegerTy()) {
    // Int cast
    return llvm_value_t(builder->CreateIntCast(load(value).value, target_type, type->is_signed()), target_type, true);
  }

  if (type->kind == type_kind_t::eContract) {
    // Create the contract object
    auto generic_contract_type = llvm::StructType::getTypeByName(*context, "contract");
    auto constrained_contract_type = llvm::StructType::getTypeByName(*context, "contract." + to_string(value.base_type->name));

    auto static_vtable = contract_emit_static_vtable(type, value.base_type);

    auto contract_ptr = builder->CreateAlloca(generic_contract_type, nullptr, "contract");

    auto field_vtable_ptr = builder->CreateStructGEP(
        generic_contract_type, contract_ptr, 0, "vtable_field");
    auto field_data_ptr = builder->CreateStructGEP(generic_contract_type, contract_ptr, 1, "data_field");

    builder->CreateStore(static_vtable, field_vtable_ptr);
    builder->CreateStore(value.value, field_data_ptr);

    // The resulting fat pointer is an lvalue (duh.. you alloca'd), we
    // have to load it, to be able to retrieve the vtable & data.
    return llvm_value_t(contract_ptr, generic_contract_type, false);
  }

  // Casting from known size array into runtime slice.
  if (value.type->isArrayTy() &&
      type->kind == type_kind_t::eSlice) {
    return slice_create_from_array(load(value));
  }

  return value;
}

llvm::Value *
codegen_t::decay_contract_to_data(llvm::Value *value) {
  auto contract_type = llvm::StructType::getTypeByName(*context, "contract");

  auto data_ptr = builder->CreateExtractValue(value, {1}, "data_field");
  return data_ptr;
}

llvm::Value *
codegen_t::decay_contract_to_vtable(llvm::Value *value) {
  auto contract_type = llvm::StructType::getTypeByName(*context, "contract");

  auto vtable_ptr = builder->CreateExtractValue(value, {0}, "vtable_field");
  return vtable_ptr;
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
  case type_kind_t::eSlice: {
    return name == "data" ? 0 : 1;
  }
  default:
    throw std::runtime_error("Unknown type in field_index");
  }
}

SP<type_t>
codegen_t::field_type(SP<type_t> ty, const std::string &name) {
  switch (ty->kind) {
  case type_kind_t::ePointer: {
    // Pointers automatically get dereferenced.
    return field_type(ty->as.pointer->deref(), name);
  }
  case type_kind_t::eStruct: {
    auto struct_layout = ty->as.struct_layout;
    auto member = struct_layout->member(name);
    return member->type;
  }
  case type_kind_t::eTuple: {
    auto tuple_layout = ty->as.tuple;
    auto member = tuple_layout->element(name);
    return member->second;
  }
  case type_kind_t::eSlice: {
    return name == "data" ? llvm_type_cache.at(llvm::PointerType::get(*context, 0)) : llvm_type_cache.at(builder->getIntNTy(64));
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
    auto path = expr->value->as.symbol->path;
    SP<llvm_value_t> left {nullptr};

    specialized_segment_t segment = path.segments.front();
    left = scope()->resolve(segment.name);
    // Erase first segment
    path.segments.erase(path.segments.begin());

    while (left && path.segments.size() > 0) {
      // Get next one.
      segment = path.segments.front();

      auto member = resolve_member(*left, segment.name);
      if (member == nullptr) {
        // If we couldn't resolve this segment, it might be a UFCS function.
        break;
      }

      left = member;
      path.segments.erase(path.segments.begin());
    }

    //assert(left->is_rvalue == false && left->type->isPointerTy() == false);
    // return std::make_shared<llvm_value_t>(
        // left->value, llvm::PointerType::get(*context, 0), true);
    return std::make_shared<llvm_value_t>(left->value,
                                          llvm::PointerType::get(*context, 0),
                                          true, pointer_t::pointer_to(pointer_kind_t::eNonNullable, left->base_type));
  }
  case ast_node_t::eArrayAccess: {
    auto value = visit_node(expr->value);
    // Address of implies that we need return something that shouldnt
    // be able to be `load'ed any further, therefore is_rvalue = true.
    return std::make_shared<llvm_value_t>(value->value, value->type, true);
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

llvm_value_t
codegen_t::slice_create_from_array(const llvm_value_t &stack_array) {
  assert(stack_array.base_type->kind == type_kind_t::eArray);
  auto arr = llvm::dyn_cast<llvm::ArrayType>(stack_array.type);

  // TODO: is_mutable: false?
  auto slice_type = type_t::make_slice(stack_array.base_type->as.array->element_type, true);
  auto llvm_slice_type = ensure_type(slice_type);

  // Slices are basically named tuples (data: ?T, size: u64).
  //
  // Since they are small, pass them around by-value instead of as stack objects.
  llvm::Value *slice = llvm::ConstantAggregateZero::get(llvm_slice_type);

  slice = builder->CreateInsertValue(slice, stack_array.value, {0}, "slice.data");
  slice = builder->CreateInsertValue(slice, llvm::ConstantInt::get(builder->getInt64Ty(), arr->getNumElements()), {1}, "slice.size");

  return llvm_value_t (slice, llvm_slice_type, true, slice_type);
}

llvm_value_t
codegen_t::slice_create_from_parts(SP<type_t> base_type, const llvm_value_t &pointer, const llvm_value_t &size) {
  // TODO: is_mutable: false?
  auto slice_type = type_t::make_slice(base_type, true);
  auto llvm_slice_type = ensure_type(slice_type);

  // Slices are basically named tuples (data: ?T, size: u64).
  //
  // Since they are small, pass them around by-value instead of as stack objects.
  llvm::Value *slice = llvm::ConstantAggregateZero::get(llvm_slice_type);

  slice = builder->CreateInsertValue(slice, load(pointer).value, {0}, "slice.data");
  slice = builder->CreateInsertValue(slice, cast(llvm_type_cache.at(builder->getIntNTy(64)), size).value, {1}, "slice.size");

  return llvm_value_t (slice, llvm_slice_type, true, slice_type);
}

VISITOR(binding) {
  binding_decl_t *decl = node->as.binding_decl;
  current_binding = decl->name;
  auto value = visit_node(decl->value);
  if (value) { // If we got no value, we likely defined a type or
               // something that doesn't actually store something.
    return scope()->set(to_string(decl->name), std::make_shared<llvm_value_t>(cast(node->type, *value)));
  }
  scope()->set(to_string(decl->name), nullptr);
  return nullptr;
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

  auto func = llvm::Function::Create(llvm_fn, current_linkage.value_or(llvm::GlobalValue::ExternalLinkage), to_string(*current_binding), *module);
  auto ret_val = scope()->set(to_string(*current_binding), std::make_shared<llvm_value_t>(func, llvm_fn, false));

  llvm::BasicBlock *entry = llvm::BasicBlock::Create(*context, "entry", func);

  auto current_block = builder->GetInsertBlock();

  builder->SetInsertPoint(entry);

  scopes.emplace_back(std::make_shared<llvm_scope_t>(scopes.back()));

  for (uint64_t i = 0; i < impl->declaration.parameters.size(); ++i) {
    auto &param = impl->declaration.parameters[i];
    auto ty = ensure_type(param.resolved_type);
    llvm::Value *llvm_value = func->args().begin() + i;
    bool is_rvalue = true;

    if (param.is_mutable && (!param.is_self || !param.is_self_ref)) {
      // Mutable parameters have to be alloca'd + stored, LLVM
      // prohibits modifying parameters directly.
      auto value = builder->CreateAlloca(ty, nullptr, param.name);
      builder->CreateStore(llvm_value, value);
      llvm_value = value;
      is_rvalue = false;
    }

    scope()->set(param.name, std::make_shared<llvm_value_t>(llvm_value, ty, is_rvalue, param.resolved_type));
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

VISITOR(slice_expr) {
  slice_expr_t *slice = node->as.slice_expr;

  return std::make_shared<llvm_value_t>(
    slice_create_from_parts(slice->resolved_type, *visit_node(slice->pointer), *visit_node(slice->size))
    );
}

VISITOR(call) {
  call_expr_t *call = node->as.call_expr;

  llvm::FunctionType *func = llvm::dyn_cast<llvm::FunctionType>(ensure_type(call->callee->type));

  auto signature = call->callee->type->as.function;

  std::vector<llvm::Value *> args;
  int64_t nparam = 0;
  for (auto &arg : call->arguments) {
    auto value = visit_node(arg);

    if (nparam < signature->arg_types.size()) {
      auto param_type = signature->arg_types[nparam];
      if (param_type != arg->type) {
        value = std::make_shared<llvm_value_t>(cast(param_type, *value));
      }
    }

    args.push_back(load(value).value);
    nparam++;
  }

  if (call->implicit_receiver) {
    auto receiver = visit_node(call->implicit_receiver);
    args.insert(args.begin(), load(receiver).value);
  }

  auto callee = visit_node(call->callee);
  llvm::Value *value = builder->CreateCall(func, callee->value, args);

  return std::make_shared<llvm_value_t>(value, func->getReturnType(), true, signature->return_type);
}

VISITOR(block) {
  block_node_t *block = node->as.block;

  auto func = builder->GetInsertBlock()->getParent();
  scopes.emplace_back(std::make_shared<llvm_scope_t>(scopes.back()));
  auto scope = this->scope();
  scope->exit_block = llvm::BasicBlock::Create(*context, "exit", func);
  auto *continue_bb = llvm::BasicBlock::Create(*context, "after", func);

  SP<llvm_value_t> return_value {nullptr};

  // We can't return void, skip the implicit handling.
  if (!block->resolved_return_type || (block->resolved_return_type && ensure_type(block->resolved_return_type)->isVoidTy() == true)) {
    block->has_implicit_return = false;
  }

  if (block->has_implicit_return) {
    // If we do have an implicit return, we create an alloca for the type.
    auto return_type = ensure_type(block->resolved_return_type);
    return_value = std::make_shared<llvm_value_t>(builder->CreateAlloca(return_type, 0, nullptr, "retval"), return_type, false, block->resolved_return_type);
  }

  SP<llvm_value_t> last_value {nullptr};
  for (auto &v : block->body) {
    last_value = visit_node(v);

    if (builder->GetInsertBlock()->getTerminator()) break;
  }

  if (block->has_implicit_return) {
    // Set the local alloca
    builder->CreateStore(load(last_value).value, return_value->value);
  }

  if (!builder->GetInsertBlock()->getTerminator()) {
    builder->CreateBr(scope->exit_block);
  }

  builder->SetInsertPoint(scope->exit_block);
  for (auto it = scope->defer_stack.rbegin(); it != scope->defer_stack.rend();
       ++it) {
    visit_node(*it);
  }

  builder->CreateBr(continue_bb);
  scopes.pop_back();
  builder->SetInsertPoint(continue_bb);

  if (block->has_implicit_return) {
    return std::make_shared<llvm_value_t>(load(return_value).value, ensure_type(block->resolved_return_type), false, block->resolved_return_type);
  } else {
    return nullptr;
  }
}

VISITOR(declaration) {
  declaration_t *decl = node->as.declaration;

  llvm::Type *value_type = ensure_type(node->type);
  llvm::Value *storage = builder->CreateAlloca(value_type, 0, nullptr, decl->identifier);

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
        val = cast(node->type, val);
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

  return scopes.back()->set(decl->identifier, std::make_shared<llvm_value_t>(storage, value_type, !is_lvalue, node->type));
}

VISITOR(literal) {
  literal_expr_t *expr = node->as.literal_expr;

  llvm::Type *type = ensure_type(node->type);
  llvm::Value *value = nullptr;
  switch (expr->type) {
  case literal_type_t::eInteger: {
    // TODO: Can't handle uint64_t at max extent (above limits of int64_t)
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
  return std::make_shared<llvm_value_t>(value, type, true, node->type);
}

SP<llvm_value_t>
codegen_t::resolve_member(const llvm_value_t &val, const std::string &member) {
  llvm::Value *current_val = val.value;
  SP<type_t> current_type = val.base_type;

  // Strip away nested pointer levels.
  while (current_type->kind == type_kind_t::ePointer &&
         current_type->as.pointer->deref()->kind == type_kind_t::ePointer) {
    current_type = current_type->base_type();
    current_val = builder->CreateLoad(ensure_type(current_type), current_val);
  }

  // Now we need to determine if the value is an lvalue (stack), or an
  // rvalue (register).
  // If it's a pointer, its always an lvalue, independant of the rvalue flag.
  bool is_lvalue =
      (current_type->kind == type_kind_t::ePointer || val.is_rvalue == false);

  // If we are still one indirection away from the base struct type,
  // deref that indirection.
  SP<type_t> struct_type = current_type->kind == type_kind_t::ePointer ? current_type->as.pointer->deref() : current_type;

  llvm::Type *llvm_struct_type = ensure_type(struct_type);
  int field_idx = field_index(llvm_struct_type, member);

  if (field_idx == -1) return nullptr;

  llvm::Value *member_data;
  bool result_is_rvalue;

  SP<type_t> field_type = this->field_type(struct_type, member);

  if (is_lvalue) {
    // We have a pointer to a something. Use GEP.
    // Result is the ADDRESS of the field (lvalue).
    member_data = builder->CreateStructGEP(llvm_struct_type, current_val, field_idx, member);
    result_is_rvalue = false;
  } else {
    // We have the struct value in a register. Use ExtractValue.
    // Result is the VALUE of the field (rvalue).
    member_data = builder->CreateExtractValue(current_val, {(unsigned int)field_idx}, member);
    result_is_rvalue = true;
  }

  return std::make_shared<llvm_value_t>(member_data, ensure_type(field_type), result_is_rvalue, field_type);
}

VISITOR(symbol) {
  symbol_expr_t *symbol = node->as.symbol;

  // Fully specified path
  auto value = scope()->resolve(to_string(symbol->path));
  if (value)
    return value;

  // Template
  if (info.template_instantiations.contains(symbol->path)) {
    if (auto sym = scope()->resolve(to_string(symbol->path)); sym) {
      return sym;
    }

    auto template_ = info.template_instantiations.at(symbol->path);

    current_binding = symbol->path;
    current_linkage = llvm::GlobalValue::LinkOnceODRLinkage;

    visit_node(template_);

    current_linkage = std::nullopt;

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
    if (!left)  throw std::runtime_error{"Symbol not found in codegen?"};
    return left;
  }

  throw std::runtime_error{"Symbol not found in codegen?"};
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
    SP<type_t> int_result_type;

    if (left_type->isIntegerTy() && right_type->isIntegerTy()) {
      if (left_type->getIntegerBitWidth() > right_type->getIntegerBitWidth()) {
        result_type = left_type;
        int_result_type = expr->left->type;
      } else {
        result_type = right_type;
        int_result_type = expr->right->type;
      }
    } else {
      int_result_type = left_type->isPointerTy() ? expr->left->type : expr->right->type;
      result_type = ensure_type(int_result_type);
    }

    if (left_type != result_type) {
      left = std::make_shared<llvm_value_t>(builder->CreateIntCast(load(left_type, *left).value, result_type, int_result_type->is_signed()), result_type, true, int_result_type);
      left_type = result_type;
    }

    if (right_type != result_type) {
      right = std::make_shared<llvm_value_t>(builder->CreateIntCast(load(right_type, *right).value, result_type, int_result_type->is_signed()), result_type, true, int_result_type);
      right_type = result_type;
    }

    if (is_scalar_binop(expr->op)) {
      auto intermediate = builder->CreateBinOp(map_binop_type(left_type, right_type, expr->op), load(left_type, *left).value, load(right_type, *right).value);
      return std::make_shared<llvm_value_t>(builder->CreateIntCast(intermediate, result_type, int_result_type->is_signed()), result_type, true, int_result_type);
    } else {
      // Comparision
      bool is_float = left_type->isFloatingPointTy() || right_type->isFloatingPointTy();
      bool use_signed_cmp = expr->left->type->is_signed() || expr->right->type->is_signed();
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
        inst = is_float ? P::FCMP_OGT : use_signed_cmp ? P::ICMP_SGT : P::ICMP_UGT;
        break;

      case binop_type_t::eLT:
        inst = is_float ? P::FCMP_OLT : use_signed_cmp ? P::ICMP_SLT : P::ICMP_ULT;
        break;

      case binop_type_t::eGTE:
        inst = is_float ? P::FCMP_OGE : use_signed_cmp ? P::ICMP_SGE : P::ICMP_UGE;
        break;

      case binop_type_t::eLTE:
        inst = is_float ? P::FCMP_OLE : use_signed_cmp ? P::ICMP_SLE : P::ICMP_ULE;
        break;

      default:
        throw std::runtime_error ("Internal Compiler Error: Unhandled non-scalar binary operation.");
        break;
      }

      return std::make_shared<llvm_value_t>(builder->CreateICmp(inst, load(left_type, *left).value, load(right_type, *right).value), builder->getIntNTy(1), true);
    }
  }
  throw std::runtime_error ("Internal Compiler Error: Unhandled types in binary operation.");
  return nullptr;
}

VISITOR(array_access) {
  array_access_expr_t *expr = node->as.array_access_expr;

  auto base_val = visit_node(expr->value);
  auto offset_val = visit_node(expr->offset);

  llvm::Value* base = load(base_val).value;
  llvm::Value* offset = load(*offset_val).value;

  auto element_type = ensure_type(expr->value->type->base_type());

  llvm::Value* address = builder->CreateGEP(element_type, base, {offset}, "array_idx");

  return std::make_shared<llvm_value_t>(address, element_type, false);
}

VISITOR(deref) {
  deref_expr_t *expr = node->as.deref_expr;

  auto value = visit_node(expr->value);
  auto new_type = value->base_type->base_type();

  return std::make_shared<llvm_value_t>(builder->CreateLoad(ensure_type(new_type), value->value), ensure_type(new_type), true, new_type);
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

        llvm::Value* max = cast(init->base_type, load(visit_node(range->max))).value;

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

    llvm::Value* field_ptr = builder->CreateStructGEP(struct_ty, struct_ptr, nfield, name);

    auto val = visit_node(value_ast);
    builder->CreateStore(cast(field_type(node->type, name), load(val)).value, field_ptr);
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

  if (expr->op == token_type_t::operatorMinus) {
    if (value->type->isIntegerTy())
      return std::make_shared<llvm_value_t>(
          builder->CreateNeg(load(value).value, "int_neg"), value->type, true);
    else
      return std::make_shared<llvm_value_t>(builder->CreateFNeg(load(value).value, "float_neg"), value->type, true);
  }

  // ! (negate)
  if (expr->op == token_type_t::operatorExclamation) {
    return std::make_shared<llvm_value_t>(builder->CreateNot(load(value).value, "bool_not"), value->type, true);
  }

  return value;
}

VISITOR(contract) {
  contract_decl_t *decl = node->as.contract_decl;
  // First ensure_type, this emits the definition of the contract.
  ensure_type(node->type);

  // Now emit the dynamic dispatchers.
  for (auto &[name, req] : node->type->as.contract->requirements) {
    // Create dynamic dispatch function
    contract_emit_dynamic_dispatcher(node->type, name, req);
  }
  return nullptr;
}

VISITOR(assignment) {
  assign_expr_t *expr = node->as.assign_expr;

  auto left = visit_node(expr->where);
  auto right = visit_node(expr->value);

  builder->CreateStore(load(right).value, left->value);
  return std::make_shared<llvm_value_t>(load(left).value, left->type, true);
}

llvm::Value *codegen_t::contract_emit_static_vtable(SP<type_t> contract_ty, SP<type_t> implementor) {
  auto static_variable_name = to_string(contract_ty->name) + "." + to_string(implementor->name) + ".vtable";

  if (auto v = module->getGlobalVariable(static_variable_name)) {
    return v;
  }

  auto vtable_ty = llvm::StructType::getTypeByName(*context, "contract." + to_string(contract_ty));
  auto contract = contract_ty->as.contract;

  std::vector<llvm::Constant *> elements;
  for (auto it = contract->requirements.begin();
       it != contract->requirements.end(); ++it) {
    auto offset = std::distance(contract->requirements.begin(), it);
    elements.push_back(llvm::dyn_cast<llvm::Function>(scope()->resolve(to_string(implementor) + "." + it->first)->value));
  }

  auto *vtable_init = llvm::ConstantStruct::getAnon(*context, elements, true);

  // 2. It doesn't exist, so create it
  return new llvm::GlobalVariable(
    *module,
    vtable_ty,
    true,
    llvm::GlobalValue::InternalLinkage,
    vtable_init,
    static_variable_name
    );
}

VISITOR(cast) {
  cast_expr_t *expr = node->as.cast;
  return std::make_shared<llvm_value_t>(cast(node->type, *visit_node(expr->value)));
}

VISITOR(nil) {
  llvm::PointerType* ptr_ty = llvm::PointerType::get(*context, 0);
  llvm::Value* nil_ptr = llvm::ConstantPointerNull::get(ptr_ty);
  return std::make_shared<llvm_value_t>(nil_ptr, ptr_ty, true);
}

VISITOR(if) {
    if_stmt_t *stmt = node->as.if_stmt;
    llvm::Function *func = builder->GetInsertBlock()->getParent();

    // 1. Create blocks and immediately attach them to the function
    llvm::BasicBlock *pass_bb = llvm::BasicBlock::Create(*context, "if.pass", func);
    llvm::BasicBlock *reject_bb = llvm::BasicBlock::Create(*context, "if.reject", func);
    llvm::BasicBlock *merge_bb = llvm::BasicBlock::Create(*context, "if.merge", func);

    auto cond_val = load(visit_node(stmt->condition)).value;
    builder->CreateCondBr(cond_val, pass_bb, reject_bb);

    builder->SetInsertPoint(pass_bb);
    visit_node(stmt->pass);

    // Only jump if the block has no terminator (return, jump, etc.)
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(merge_bb);
    }

    builder->SetInsertPoint(reject_bb);
    if (stmt->reject) {
        visit_node(stmt->reject);
    }
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(merge_bb);
    }

    builder->SetInsertPoint(merge_bb);
    return nullptr;
}

VISITOR(member_access) {
  member_access_expr_t *expr = node->as.member_access;

  // 1. Visit the base object (e.g., `v2.at(0).*`)
  auto base = visit_node(expr->object);

  // 2. Ensure we are dealing with a struct/contract/slice
  auto base_type = base->base_type;
  if (base_type->kind != type_kind_t::eStruct &&
      base_type->kind != type_kind_t::eSlice &&
      base_type->kind != type_kind_t::eContract) {
    throw std::runtime_error("Member access on non-aggregate type");
  }

  // We return the actual value (is_rvalue = true)
  return resolve_member(*base, expr->member);
}

VISITOR(defer) {
  defer_expr_t *expr = node->as.defer_expr;

  auto current_scope = scope();
  current_scope->defer_stack.emplace_back(expr->action);

  return nullptr;
}

VISITOR(return) {
  return_stmt_t *ret = node->as.return_stmt;

  // Since we handle returns a little more specially (block
  // expression), we can't blindly `CreateRet`, that would prevent
  // deferred statements from running.
  //
  // Instead, we walk forwards from the `scopes`, breaking on the
  // first one that has a return value set, once found, we set our
  // value to that `llvm::Value`, and jump to the current scopes exit block.

  std::shared_ptr<llvm_scope_t> function_scope = nullptr;
  std::shared_ptr<llvm_scope_t> current_scope = scope();

  for (auto scope : scopes) {
    if (scope->block_return_value != nullptr) {
      function_scope = scope;
      break;
    }
  }

  // Void returns
  if (ret->value) {
    builder->CreateStore(load(visit_node(ret->value)).value, function_scope->block_return_value);
  }

  builder->CreateBr(current_scope->exit_block);

  return nullptr;
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

  case ast_node_t::eContract:
    result = visit_contract(node);
    break;

  case ast_node_t::eAssignment:
    result = visit_assignment(node);
    break;

  case ast_node_t::eCast:
    result = visit_cast(node);
    break;

  case ast_node_t::eNil:
    result = visit_nil(node);
    break;

  case ast_node_t::eIf:
    result = visit_if(node);
    break;

  case ast_node_t::eDeref:
    result = visit_deref(node);
    break;

  case ast_node_t::eMemberAccess:
    result = visit_member_access(node);
    break;

  case ast_node_t::eSliceExpr:
    result = visit_slice_expr(node);
    break;

  case ast_node_t::eDefer:
    result = visit_defer(node);
    break;

  case ast_node_t::eReturn:
    result = visit_return(node);
    break;

  default:
    assert(false && "Unexpected AST node in codegen");
  }
  return result;
}
