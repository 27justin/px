#include <cassert>
#include <iostream>
#include <memory>
#include <sstream>
#include <filesystem>

#include "backend/analyzer.hpp"
#include "backend/type.hpp"
#include "frontend/ast.hpp"
#include "frontend/diagnostic.hpp"
#include "frontend/parser.hpp"
#include "frontend/path.hpp"
#include "frontend/token.hpp"

using QT = SP<type_t>;
using N = SP<ast_node_t>;
using std::make_shared;
using A = analyzer_t;

void A::import_source_file(const std::string &path) {
  auto src = std::make_shared<source_t>(source_t::from_file(path));
  try {
    lexer_t lexer(src);
    parser_t parser(lexer, src);
    analyzer_t analyzer(src);

    analyzer.set_include_directories(include_directories);

    semantic_info_t info = analyzer.analyze(parser.parse());
    scope_stack.front()->merge(*info.scope);

    for (auto &[symbol, ty] : info.scope->symbol_map()) {
      this->info->imported_symbols[symbol] = ty->type;
    }
  } catch (const analyze_error_t &err) {
    for (auto &msg : err.diagnostics.messages) {
      std::cerr << serialize(msg) << "\n";
    }
  } catch (const parse_error_t &err) {
    for (auto &msg : err.diagnostics.messages) {
      std::cerr << serialize(msg) << "\n";
    }
  }
}

void A::set_include_directories(const std::vector<std::string> &dirs) {
  include_directories = dirs;
}

semantic_info_t
A::analyze(translation_unit_t tu) {
  push_scope();
  info = std::make_unique<semantic_info_t>(semantic_info_t {
      .unit = tu,
      .scope = scope_stack.front()
    });

  // First resolve all imports
  for (auto &import_ : info->unit.imports) {
    bool found = false;
    for (auto &search_path : include_directories) {
      auto relative_disk_path = to_string(import_);
      size_t pos = relative_disk_path.find(".");
      while (pos != std::string::npos) {
        relative_disk_path.replace(pos, 1, "/");
        pos = relative_disk_path.find(".");
      }

      auto absolute_disk_path = search_path + '/' + relative_disk_path + ".px";
      if (std::filesystem::exists(absolute_disk_path)) {
        import_source_file(absolute_disk_path);
        found = true;
        break;
      }
    }

    if (!found) {
      diagnostics.messages.push_back(error(source, {{0,0}, {0,0}}, "Unknown import", "Import " + to_string(import_) + " wasn't found"));
      throw analyze_error_t {diagnostics};
    }
  }

  // Then process our source file
  for (auto it = info->unit.declarations.begin(); it != info->unit.declarations.end();) {
    auto &decl = *it;
    try {
      analyze_node(decl);

      if (!decl) {
        it = info->unit.declarations.erase(it);
      } else {
        ++it;
      }
    } catch (const diagnostic_t &msg) {
      diagnostics.messages.push_back(msg);
    }
  }
  pop_scope();

  if (diagnostics.messages.size() > 0) {
    throw analyze_error_t {std::move(diagnostics)};
  }
  return *info;
}

scope_t &
A::scope() {
  return *scope_stack.back();
}

scope_t &
A::push_scope() {
  auto scope = std::make_shared<scope_t>(scope_stack.size() > 0 ? scope_stack.back() : nullptr);
  scope_stack.emplace_back(scope);
  return *scope;
}

void
A::pop_scope() {
  scope_stack.pop_back();
}

SP<type_t>
A::current_function() {
  return function_stack.back();
}

void
A::push_function(SP<type_t> fn) {
  function_stack.push_back(fn);
}

void
A::pop_function() {
  function_stack.pop_back();
}

QT
A::resolve_type(const type_decl_t &ty) {
  QT base = scope().types.resolve(ty.name);
  if (base) {
    if (ty.indirections.size() > 0) {
      return scope().types.pointer_to(base, ty.indirections, ty.is_mutable);
    }

    if (ty.is_slice && ty.len == nullptr) {
      return scope().types.slice_of(base, ty.is_mutable);
    }

    if (ty.len != nullptr) {
      if (ty.len->kind == ast_node_t::eLiteral)
        return scope().types.array_of(base, std::stoll(ty.len->as.literal_expr->value));
      else
        return scope().types.slice_of(base, ty.is_mutable);
    }
    return base;
  } else {
    // Might be a tuple.
    if (ty.tuple) {
      // Tuples are not non-named composite types
      std::vector<std::pair<std::string, QT>> tuple_types;
      int64_t nmemb = 0;
      for (auto &[k, v] : ty.tuple->elements) {
        tuple_types.emplace_back(k.value_or(std::to_string(nmemb)), ensure_concrete(resolve_type(v)));
        nmemb++;
      }
      return scope().types.tuple_of(tuple_types);
    }

    // Might be a union type.
    if (ty.union_) {
      std::map<std::string, QT> composite;
      for (auto &[k, v] : ty.union_->values) {
        auto ty = resolve_type(v);
        composite[k] = ty;
      }

      return scope().types.union_of(composite);
    }

    // Might be a template, which requires monomorphization
    auto template_candidates = scope().candidates(ty.name);
    if (template_candidates.empty()) {
      return nullptr;
    }
    return monomorphize(template_candidates.front(), ty.name);
  }

  return nullptr;
}

specialized_path_t
A::resolve_path(const specialized_path_t &path) {
  specialized_path_t result = path;

  for (auto &segment : result.segments) {
    for (auto &ty : segment.types) {
      auto type = resolve_type(*ty);

      auto new_ty = make_shared<type_decl_t>();
      new_ty->name = {type->name};

      new_ty->is_mutable = ty->is_mutable;

      if (type->kind == type_kind_t::ePointer)
        new_ty->indirections = type->as.pointer->indirections;

      if (type->kind == type_kind_t::eSlice)
        new_ty->is_slice = true;

      if (type->kind == type_kind_t::eArray)
        new_ty->len = ty->len;

      ty = new_ty;
    }
  }

  return result;
}

QT A::resolve_type(const specialized_path_t &path) {
  type_decl_t decl {
    .name = resolve_path(path),
    .indirections = {}, .is_mutable = false, .is_slice = false,
    .len = nullptr,
  };
  return resolve_type(decl);
}

QT A::resolve_type(const std::string &name) {
  type_decl_t decl {
    .name = specialized_path_t ({name}),
    .indirections = {}, .is_mutable = false, .is_slice = false,
    .len = nullptr,
  };
  return resolve_type(decl);
}

QT
A::analyze_function_decl(const function_decl_t &decl, SP<type_t> implicit_receiver) {
  std::vector<SP<type_t>> params;

  SP<type_t> active_receiver {nullptr};

  for (auto &param : decl.parameters) {
    if (param.is_self) {
      active_receiver = implicit_receiver;
      if (param.is_self_ref) {
        active_receiver = scope().types.pointer_to(active_receiver, {pointer_kind_t::eNonNullable}, param.is_mutable);
      }
    } else {
      params.push_back(resolve_type(param.type));
      if (param.is_rvalue) {
        params.back() = scope().types.rvalue_of(params.back());
      }
    }

    if (params.size() > 0 && !params.back()) {
      diagnostics.messages.push_back(error(source, {{0,0}, {0,0}}, "Unknown type", fmt("Unknown type {}", to_string(param.type))));
      throw analyze_error_t{diagnostics};
    }
  }

  return scope().types.add_function(resolve_type(decl.return_type), params, active_receiver, decl.is_var_args);
}

QT
A::analyze_function_decl(N node, SP<type_t> implicit_receiver) {
  function_decl_t *decl = node->as.fn_decl;
  return analyze_function_decl(*decl, implicit_receiver);
}

QT
A::analyze_function_impl(N node, SP<type_t> implicit_receiver) {
  function_impl_t *impl = node->as.fn_impl;

  // Push a new scope
  push_scope();

  // Register parameters
  for (auto &param : impl->declaration.parameters) {
    if (param.is_self == false) {
      auto ty = resolve_type(param.type);
      scope().add(param.name, ty, param.is_mutable);
      param.resolved_type = ty;
    } else {
      auto self = implicit_receiver;
      if (param.is_self_ref) {
        self = scope().types.pointer_to(self, {pointer_kind_t::eNonNullable}, param.is_mutable);
      }
      scope().add("self", implicit_receiver, param.is_mutable);
      param.resolved_type = self;
    }
  }

  auto function = analyze_function_decl(impl->declaration, implicit_receiver);
  push_function(function);
  QT actual_return_type = analyze_node(impl->block);
  pop_function();

  pop_scope();

  QT expected_return_type = resolve_type(impl->declaration.return_type);

  if (*actual_return_type != *expected_return_type
      && !is_implicit_convertible(actual_return_type, expected_return_type)) {
    diagnostics.messages.push_back(error(node->source, node->location, "Type mismatch", fmt("Function is supposed to return `{}`, but block evaluates to `{}`", to_string(expected_return_type), to_string(actual_return_type))));
    throw analyze_error_t{diagnostics};
  }
  return analyze_function_decl(impl->declaration, implicit_receiver);;
}

QT
A::analyze_binding(N node) {
  if (node->kind == ast_node_t::eTemplate) {
    template_decl_t *decl = node->as.template_decl;
    scope().add_template(make_shared<template_decl_t>(*decl));
    return nullptr;
  }

  binding_decl_t *decl = node->as.binding_decl;

  current_binding = decl->name;

  QT type = analyze_node(decl->value);
  if (decl->type) {
    type = resolve_type(*decl->type);
  }

  scope().add(decl->name, type, false);
  current_binding = std::nullopt;
  return type;
}

QT
A::analyze_block(N node) {
  block_node_t *block = node->as.block;

  QT last_type {};
  for (auto &v : block->body) {
    last_type = analyze_node(v);
  }

  block->resolved_return_type = last_type;
  return block->has_implicit_return ? last_type : resolve_type("void");
}

bool
A::is_static_dispatch(N node) {
  call_expr_t *call = node->as.call_expr;
  if (call->callee->kind != ast_node_t::eSymbol)
    return false;

  return scope().resolve(call->callee->as.symbol->path) != nullptr;
}

bool
A::is_dynamic_dispatch(N node) {
  return !is_static_dispatch(node);
}

value_category_t A::resolve_value_category(N node) {
  switch (node->kind) {
  case ast_node_t::eSymbol:       return value_category_t::eLValue;
  case ast_node_t::eMemberAccess: return value_category_t::eLValue; // x.y
  case ast_node_t::eMove:         return value_category_t::eRValue; // move x
  case ast_node_t::eLiteral:      return value_category_t::eRValue; // 5, "hi"
  case ast_node_t::eCall:         return value_category_t::eRValue; // f()
  default:                        return value_category_t::eRValue;
  }
}

QT
A::analyze_call(N node) {
  call_expr_t *call = node->as.call_expr;

  auto fn = analyze_node(call->callee);
  if (fn->kind != type_kind_t::eFunction) {
    diagnostics.messages.push_back(error(call->callee->source, call->callee->location, "Not callable", "This expression does not evaluate to a function, and thus is not callable."));
    throw analyze_error_t{diagnostics};
  }

  // Check the argument types against the parameters
  function_signature_t *signature = fn->as.function;

  bool has_receiver = signature->receiver != nullptr;

  int64_t passed_arguments = call->arguments.size(),
    required_parameters = signature->arg_types.size();

  if (has_receiver) {
    required_parameters += 1;
  }

  // `i32.to_string(..)`, implies that the user passes the `self` himself.
  if (is_static_dispatch(node) && has_receiver) {
    passed_arguments = call->arguments.size();
  } else {
    // Dynamic dispatch (calling from a local symbol), adds a hidden
    // `self` parameter, if it so requires.
    if (has_receiver) {
      passed_arguments += 1;
    }
  }

  if (required_parameters != passed_arguments
      && !(signature->is_var_args && passed_arguments >= required_parameters)) {
    diagnostics.messages.push_back(error(node->source, node->location, "Mismatched argument count", fmt("Function expects {} arguments, got {}", required_parameters, passed_arguments)));
    throw analyze_error_t{diagnostics};
  }

  // Type-match the passed arguments & required parameters
  for (auto i = 0; i < signature->arg_types.size(); ++i) {
    auto param = signature->arg_types[i];
    auto arg_node = call->arguments[i];

    // Hint what we need, for int types this auto-coerces to them.
    push_type_hint(param);

    auto arg_type = analyze_node(arg_node); // Existing call
    pop_type_hint();

    // Check RValue requirement (^T)
    if (param->kind == type_kind_t::eRValueReference) {
      if (resolve_value_category(arg_node) != value_category_t::eRValue) {
        diagnostics.messages.push_back(error(arg_node->source, arg_node->location, 
                                             "Move required", "Cannot pass an lvalue to a move-only parameter (^T). Use 'move'."));
        throw analyze_error_t{diagnostics};
      }
    }

    if (!is_implicit_convertible(arg_type, param)) {
      diagnostics.messages.push_back(error(call->arguments[i]->source, call->arguments[i]->location, "Invalid type", fmt("Expected {}, got {} for argument #{}", to_string(param), to_string(arg_type), i)));
      throw analyze_error_t{diagnostics};
    }
  }

  if (is_dynamic_dispatch(node) && has_receiver) {
    // Rewrite the callee symbol, to point the static symbol.
    auto member_function = call->callee->as.symbol->path.segments.back();

    // e.g. `self.member.func`
    auto qualified_symbol_path = call->callee->as.symbol->path;

    // e.g. `std.contract.func` (type of `self.member` + member function)
    std::vector<specialized_segment_t> full_symbol_path = { member_function };
    full_symbol_path.insert(full_symbol_path.begin(), signature->receiver->name.segments.begin(), signature->receiver->name.segments.end());

    // e.g. `self.member` path to the receiver
    std::vector<specialized_segment_t> receiver_path;
    receiver_path.insert(receiver_path.begin(), qualified_symbol_path.segments.begin(), qualified_symbol_path.segments.end() - 1);

    call->implicit_receiver = make_node<symbol_expr_t>(ast_node_t::eSymbol, {receiver_path}, node->location, node->source);

    // Automatically add a `&` if the receiver is a pointer and NOT a self
    // placeholder.
    //
    // TODO: This is a crutch for an issue that pertains to just the
    // codegen backend.
    //
    // The dynamic dispatchers within the codegen get passed the
    // contract by-value rather than by-ref, therefore adding this
    // `addr_of_expr_t` breaks our ABI, by passing a pointer where a
    // contract value is expected.
    if (signature->receiver->kind == type_kind_t::ePointer &&
        signature->receiver->base_type()->kind != type_kind_t::eSelf) {
      call->implicit_receiver = make_node<addr_of_expr_t>(ast_node_t::eAddrOf, {call->implicit_receiver}, node->location, node->source);
    }

    analyze_node(call->implicit_receiver);

    call->callee = make_node<symbol_expr_t>(ast_node_t::eSymbol, {full_symbol_path}, node->location, node->source);
    analyze_node(call->callee);
  }

  // The remaining arguments (for variadic functions), can't be type
  // checked. But they still have to be analyzed.
  for (auto i = signature->arg_types.size(); i < call->arguments.size(); ++i) {
    analyze_node(call->arguments[i]);
  }

  return fn->as.function->return_type;
}

QT
A::analyze_literal(N node) {
  literal_expr_t *literal = node->as.literal_expr;

  switch (literal->type) {
  case literal_type_t::eString:
    return scope().types.array_of(resolve_type("u8"), literal->value.size());
  case literal_type_t::eInteger:
  case literal_type_t::eFloat:
    // If we have a type hint (from e.g. `let int: i32 = 1`), we know
    // that we are a i32, otherwise we make it untyped to resolve
    // later.
    if (type_hint())
      return type_hint();
    else
      return scope().types.untyped_literal(literal->value, literal->type);
  case literal_type_t::eBool:
    return resolve_type("bool");
  default:
    assert(false && "Unhandled literal case");
  }
}

QT
A::analyze_declaration(N node) {
  declaration_t *decl = node->as.declaration;

  // Resolve annotation once if it exists
  QT annotated_type = decl->type ? resolve_type(*decl->type) : nullptr;
  bool has_type_hint = false;

  if (annotated_type) {
    push_type_hint(annotated_type);
    has_type_hint = true;
  }

  // Determine the type of the value
  QT inferred_type = nullptr;
  if (decl->value) {
    // Resolve the raw type of the expression
    inferred_type = analyze_node(decl->value);

    // If it's an untyped literal and we have an annotation, try to collapse it early
    if (inferred_type->kind == type_kind_t::eUntypedLiteral && annotated_type) {
      if (annotated_type->is_numeric()) {
        if (can_fit_literal(inferred_type->as.literal->value, annotated_type)) {
          inferred_type = annotated_type;
        } else {
          // Error: Literal is too big for the annotated type
          diagnostics.messages.push_back(error(node->source, node->location,
                                               "Literal overflow",
                                               fmt("Value {} does not fit in type {}.", inferred_type->as.literal->value, to_string(annotated_type))));
          throw analyze_error_t{diagnostics};
        }
      }
    }

    // If it's still untyped (no annotation), force it to default (f32, i32, or itself)
    inferred_type = ensure_concrete(inferred_type);
  }

  // Reconcile types
  QT final_type = nullptr;
  if (annotated_type && inferred_type) {
    if (*annotated_type != *inferred_type
      && !is_implicit_convertible(inferred_type, annotated_type)) {
      diagnostics.messages.push_back(error(node->source, node->location,
                                           "Type mismatch",
                                           fmt("Type annotation holds a different type than the expression. ({} vs {})", to_string(annotated_type), to_string(inferred_type))));
      throw analyze_error_t {diagnostics};
    }
    final_type = annotated_type;
  } else {
    // Fallback to whichever is present (or error if neither)
    final_type = annotated_type ? annotated_type : inferred_type;
  }

  if (!final_type) {
    diagnostics.messages.push_back(error(node->source, node->location, "Type error", "Cannot infer type."));
    throw analyze_error_t {diagnostics};
  }

  if (has_type_hint) {
    pop_type_hint();
  }

  scope().add(decl->identifier, final_type, decl->is_mutable);
  return final_type;
}

QT
A::resolve_tuple_element(QT tuple, const std::string &member_name) {
  auto tuple_layout = tuple->as.tuple;
  if (tuple_layout->element(member_name) == tuple_layout->elements.end()) {
    diagnostics.messages.push_back(error(source, {{0,0},{0,0}}, "Invalid tuple element", fmt("Tuple element {} is unknown", member_name)));
    throw analyze_error_t {diagnostics};
  }
  return tuple_layout->element(member_name)->second;
}

QT
A::resolve_enum_element(QT enum_, const std::string &member_name) {
  if (!enum_->as.enum_->values.contains(member_name)) {
    diagnostics.messages.push_back(error(source, {{0,0},{0,0}}, "Invalid enum element", fmt("Enum element {} is unknown", member_name)));
    throw analyze_error_t {diagnostics};
  }
  return enum_;
}

QT
A::resolve_member_access(QT base, const std::string &member_name) {
  switch (base->kind) {
  case type_kind_t::eStruct: {
    struct_layout_t *layout = base->as.struct_layout;
    auto member = layout->member(member_name);
    if (member) return member->type;
    break;
  }
  case type_kind_t::eContract: {
    contract_t *contract = base->as.contract;
    if (contract->requirements.contains(member_name)) {
      return contract->requirements.at(member_name);
    }
    break;
  }
  case type_kind_t::eSlice:
  case type_kind_t::eArray: {
    auto element = base->kind == type_kind_t::eSlice ? base->as.slice->element_type : base->as.array->element_type;
    if (member_name == "size")
      return resolve_type("u64");
    if (member_name == "ptr")
      return scope().types.pointer_to(element, {pointer_kind_t::eNonNullable},
                                      base->kind == type_kind_t::eSlice);
    break;
  }
  case type_kind_t::ePointer: {
    return resolve_member_access(base->as.pointer->deref(), member_name);
  }
  case type_kind_t::eTuple: {
    return resolve_tuple_element(base, member_name);
  }
  case type_kind_t::eEnum: {
    return resolve_enum_element(base, member_name);
  }
  default:
    break;
  }
  return nullptr;
}

QT
A::analyze_symbol(N node) {
  auto path = node->as.symbol->path;

  // Since we don't disambiguate between namespaces, etc., we have to do some special checks on this symbol now.

  // Any of these cases might be true:
  // 1. `a.b` refers to member `b` on variable `a`
  // 2. `a.b` refers to a fully-specified symbol in global space
  // 3. `a.b` refers to a UFCS, where `a` is a local variable, and `b` refers to a function named like `<type of a>.b`
  // 4. `a.b` refers to enum value `b`, of enum `a`
  //
  // Generally, 1 has higher precendence over two.

  specialized_path_t pcopy = path;
  SP<symbol_t> sym = scope().resolve(specialized_path_t {{{pcopy.segments.front()}}});
  SP<type_t> left = sym ? sym->type : nullptr;
  pcopy.segments.erase(pcopy.segments.begin());

  while (pcopy.segments.size() > 0 && left) {
    auto member = resolve_member_access(left, pcopy.segments.front().name);
    if (member == nullptr) {
      break;
    }
    left = member;
    pcopy.segments.erase(pcopy.segments.begin());
  }

  if (left && pcopy.segments.empty()) {
    // Member access
    return left;
  }

  if (left) { // UFCS
    specialized_path_t ufcs = pcopy;
    ufcs.segments.insert(ufcs.segments.begin(), left->name.segments.begin(), left->name.segments.end());

    // Might be a template..
    if (!ufcs.is_simple()) {
      ufcs = resolve_path(ufcs);
      auto candidates = scope().candidates(ufcs);
      if (candidates.size() > 0) {
        return monomorphize(candidates.front(), ufcs);
      }
    }

    sym = scope().resolve(ufcs);
    if (sym) return sym->type;
  }

  sym = scope().resolve(path);
  if (sym) {
    return sym->type;
  }

  if (!path.is_simple()) {
    path = resolve_path(path);
    auto candidates = scope().candidates(path);
    if (!candidates.empty()) {
      return monomorphize(candidates.front(), path);
    }
  }

  // We tried resolving templates, paths, and UFCS. If none match, we try resolving for an enum.
  pcopy = path;
  pcopy.segments.pop_back();

  if (auto sym = scope().resolve(pcopy)) {
    if (sym->type->kind == type_kind_t::eEnum) {
      return resolve_enum_element(sym->type, path.segments.back().name);
    }

    return sym->type;
  }

  diagnostics.messages.emplace_back(error(source, node->location, "Unknown symbol", fmt("Symbol `{}` is not known in the current scope.", to_string(path))));
  throw analyze_error_t {diagnostics};
}

bool
A::is_rvalue(N node) {
  return !is_lvalue(node);
}

bool
A::is_lvalue(N node) {
  return node->kind == ast_node_t::eSymbol ||
         node->kind == ast_node_t::eMemberAccess ||
         node->kind == ast_node_t::eArrayAccess;
}

bool
A::is_mutable(N node) {
  if (!is_lvalue(node)) return false;

  switch (node->kind) {
  case ast_node_t::eSymbol: {
    symbol_expr_t *expr = node->as.symbol;

    auto symbol = scope().resolve(expr->path);
    if (!symbol) {
      // Try to lookup the first segment.
      //
      // TODO: This is a very crude "detection" of mutability for
      // structs, in such that it doesn't work.
      std::vector<specialized_segment_t> path = {expr->path.segments.front()};
      if (symbol = scope().resolve(path); !symbol) {
        return false;
      }
    }
    return symbol->is_mutable;
  }
  case ast_node_t::eArrayAccess: {
    return is_mutable(node->as.array_access_expr->value);
  }
  default:
    assert(false && "is_mutable on invalid node type");
    break;
  }
  return false;
}

QT A::analyze_deref(N node) {
  deref_expr_t *expr = node->as.deref_expr;

  auto value_type = analyze_node(expr->value);

  if (is_rvalue(expr->value) && value_type->kind != type_kind_t::ePointer) {
    diagnostics.messages.push_back(error(node->source, node->location, "Dereferencing R-value", "This expression is an rvalue (temporary), and has no address, taking a reference of this is not allowed."));
    throw analyze_error_t {diagnostics};
  }

  if (value_type->kind != type_kind_t::ePointer) {
    diagnostics.messages.push_back(error(node->source, node->location, "Dereferencing concrete type", "This expression resolves to a concrete type, expected a pointer."));
    throw analyze_error_t {diagnostics};
  }

  pointer_t *ptr = value_type->as.pointer;
  return ptr->deref();
}

QT
A::analyze_type_alias(N node) {
  type_alias_decl_t *decl = node->as.alias_decl;
  return scope().types.add_alias(*current_binding, resolve_type(decl->type), decl->is_distinct);
}

QT
A::analyze_struct_decl(N node) {
  struct_decl_t *decl = node->as.struct_decl;
  struct_layout_t layout {};

  for (auto &memb : decl->members) {
    layout.members.push_back(struct_layout_t::field_t{
        .name = memb.name,
        .type = resolve_type(memb.type),
      });
  }

  layout.compute_memory_layout();
  return scope().types.add_struct(*current_binding, layout);
}

QT
A::analyze_struct_expr(N node) {
  struct_expr_t *expr = node->as.struct_expr;

  QT struct_type {nullptr};
  if (expr->type)
     struct_type = analyze_node(expr->type);
  else
    struct_type = type_hint();

  if (!struct_type) {
    diagnostics.messages.push_back(error(source, node->location, "Unknown struct type", fmt("Type doesn't exist")));
    throw analyze_error_t{diagnostics};
  }

  if (struct_type->kind != type_kind_t::eStruct) {
    diagnostics.messages.push_back(error(source, node->location, "Invalid type", fmt("This type is not a struct")));
    throw analyze_error_t{diagnostics};
  }

  auto layout = struct_type->as.struct_layout;
  for (auto &[member_name, value] : expr->values) {
    if (layout->member(member_name) == nullptr) {
      diagnostics.messages.push_back(error(source, node->location, "Unknown member", fmt("Member {} doesn't exist on this struct.", member_name)));
      throw analyze_error_t{diagnostics};
    }
    analyze_node(value);
  }
  return struct_type;
}

QT
A::analyze_cast(N node) {
  cast_expr_t *decl = node->as.cast;

  QT value_type = analyze_node(decl->value);
  QT cast_type = resolve_type(decl->type);

  if (!is_cast_convertible(value_type, cast_type)) {
    diagnostics.messages.push_back(error(node->source, node->location, "Invalid cast", fmt("Casting from `{}` to `{}` is not possible.", to_string(value_type), to_string(cast_type))));
    throw analyze_error_t{diagnostics};
  }
  return cast_type;
}

bool
A::satisfies_contract(QT type, QT contract_type) {
  assert(contract_type->kind == type_kind_t::eContract);

  contract_t *contract = contract_type->as.contract;
  for (auto &[req_name, req_type] : contract->requirements) {
    if (req_type->kind == type_kind_t::eFunction) {
      auto contract_fn = req_type->as.function;

      auto sym = scope().resolve(specialized_path_t ({{to_string(type->name), {}}, {req_name, {}}}));
      if (!sym) {
        std::cerr << to_string(type) << " does not implement " << req_name << "\n";
        return false;
      }

      // The symbol type has to be the same kind as the contract member type.
      if (sym->type->kind != type_kind_t::eFunction) {
        diagnostics.messages.push_back(error(source, {{0,0}, {0,0}}, "Contract violation", fmt("`{}` does not implement the contract function `{}` (got {}, expected {})", sym->name, req_name, to_string(sym->type), to_string(req_type))));
        return false;
      }

      auto symbol_fn = sym->type->as.function;

      // Parameter count mismatch breaks contracts.
      if (symbol_fn->arg_types.size() != contract_fn->arg_types.size()) {
        diagnostics.messages.push_back(error(source, {{0,0}, {0,0}}, "Contract violation", fmt("Function `{}` does not match the contract function `{}` (got {}, expected {})", sym->name, req_name, to_string(sym->type), to_string(req_type))));
        return false;
      }

      for (auto i = 0; i < contract_fn->arg_types.size(); ++i) {
        auto contract_fn_param = contract_fn->arg_types[i];
        auto symbol_fn_param = symbol_fn->arg_types[i];

        // Type mismatches in the argument list breaks contract.
        if (*contract_fn_param != *symbol_fn_param) {
          diagnostics.messages.push_back(error(source, {{0,0}, {0,0}}, "Contract violation", fmt("Function `{}` does not match the contract function `{}` (got {}, expected {})", sym->name, req_name, to_string(sym->type), to_string(req_type))));
          return false;
        }
      }

      if ((symbol_fn->receiver.get() != nullptr) !=
          (contract_fn->receiver.get() != nullptr)) {
        diagnostics.messages.push_back(error(source, {{0,0}, {0,0}}, "Contract violation", fmt("Function `{}` does not match the contract function `{}`, receiver present on one, but not the other..", sym->name, req_name)));
        return false;
      }
    } else {
      if (type->kind != type_kind_t::eStruct) {
        diagnostics.messages.push_back(error(source, {{0,0},{0,0}}, "Contract violation", fmt("Type `{}` can't satisfy contract `{}` due to member requirement `{}`", to_string(type->name), to_string(contract_type->name), req_name)));
        return false;
      }

      auto member = type->as.struct_layout->member(req_name);
      if (!member) {
        diagnostics.messages.push_back(error(source, {{0,0},{0,0}}, "Contract violation", fmt("Type `{}` can't satisfy contract `{}` due to missing member `{}`", to_string(type->name), to_string(contract_type->name), req_name)));
        return false;
      }

      if (member->type != req_type) {
        diagnostics.messages.push_back(error(source, {{0,0},{0,0}}, "Contract violation", fmt("Type `{}` can't satisfy contract `{}` due to member `{}` type mismatch (got {}, expected {})", to_string(type->name), to_string(contract_type->name), req_name, to_string(member->type), to_string(req_type))));
        return false;
      }
    }
  }

  return true;
}

bool
A::is_implicit_convertible(QT from, QT into) {
  if (*from == *into)
    return true;

  // Compile-time arrays can be converted to slices.
  if (from->kind == type_kind_t::eArray && into->kind == type_kind_t::eSlice
    && from->as.array->element_type == into->as.slice->element_type) {
    return true;
  }

  // Non-distinct aliases can be converted to the base type
  if (from->kind == type_kind_t::eAlias &&
      from->as.alias->alias == into) {
    return true;
  }

  // Any pointer can be cast into any other concrete pointer and vice-versa
  if (from->kind == type_kind_t::ePointer &&
      into->kind == type_kind_t::ePointer) {
    auto f = from->as.pointer;
    auto i = into->as.pointer;
    auto any = resolve_type("any");

    // Mutability Check: Cannot cast non-mutable to mutable.
    // (If 'into' wants to change things, 'from' must have been mutable).
    if (i->is_mutable && !f->is_mutable) {
        return false;
    }

    // Nullability Check: Cannot cast nullable to non-nullable.
    // f_indir == nullable && i_indir == non-nullable is forbidden.
    auto f_indir = f->indirections.front();
    auto i_indir = i->indirections.front();

    if (f_indir == pointer_kind_t::eNullable && i_indir == pointer_kind_t::eNonNullable) {
        return false;
    }

    // Base Type Compatibility
    // Valid if types match, or if either side is 'any' (Type Erasure/Restoration).
    bool types_match = (f->base == i->base);
    bool involves_any = (*f->base == *any || *i->base == *any);

    return types_match || involves_any;
  }

  // Slices & Stack arrays can decay to pointer
  if ((from->kind == type_kind_t::eArray ||
       from->kind == type_kind_t::eSlice) &&
      into->kind == type_kind_t::ePointer) {

    if (from->kind == type_kind_t::eArray &&
        *from->as.array->element_type == *into->as.pointer->base)
      return true;

    if (from->kind == type_kind_t::eArray &&
        *from->as.slice->element_type == *into->as.pointer->base)
      return true;
  }

  // Arrays can decay to slices
  if (from->kind == type_kind_t::eArray && into->kind == type_kind_t::eSlice) {
    if (*from->as.array->element_type == *into->as.slice->element_type)
      return true;
  }

  // Literals may be coerced into the target type
  if (from->kind == type_kind_t::eUntypedLiteral) {
    if (into->is_numeric()) {
      return can_fit_literal(from->as.literal->value, into);
    }
  }

  return false;
}

bool
A::is_cast_convertible(QT from, QT into) {
  // Casting from concrete types into contracts is allowed.

  if (into->kind == type_kind_t::eContract) {
    return satisfies_contract(from, into);
  }

  if (from->kind == type_kind_t::ePointer &&
      into->kind == type_kind_t::ePointer) {
    auto f = from->as.pointer;
    auto i = into->as.pointer;
    auto any = resolve_type("any");

    bool types_match = (f->base == i->base);
    bool involves_any = (*f->base == *any || *i->base == *any);

    return types_match || involves_any;
  }

  if ((into->kind == type_kind_t::eInt || into->kind == type_kind_t::eUint) &&
      (from->kind == type_kind_t::eInt || from->kind == type_kind_t::eUint))
    return true;

  return false;
}

QT
A::analyze_contract(N node) {
  contract_decl_t *decl = node->as.contract_decl;
  auto contract_name = *current_binding;
  auto self_type = scope().types.self_placeholder(contract_name);

  std::map<std::string, QT> requirements;
  for (auto &member : decl->requirements) {

    switch (member->kind) {
    case ast_node_t::eBinding: {
      binding_decl_t *binding = member->as.binding_decl;

      if (binding->value) {
        // <identifier> := fn ()
        assert(binding->value->kind == ast_node_t::eFunctionDecl);
        auto type = analyze_function_decl(binding->value, self_type);
        requirements[to_string(binding->name)] = type;

        // Contract functions have to be added to the scope.
        scope().add(to_string(contract_name) + "." + to_string(binding->name), type);
      } else {
        // <identifier>: <type>
        requirements[to_string(binding->name)] = resolve_type(*binding->type);
      }
      break;
    }
    default:
      assert(false && "Unsupported contract specification");
    }
  }
  return scope().types.add_contract(contract_name, requirements);
}

QT A::resolve_binop_result_type(binop_type_t ty, QT left, QT right) {
  switch (ty) {
  case binop_type_t::eAnd:
  case binop_type_t::eOr:
  case binop_type_t::eEqual:
  case binop_type_t::eNotEqual:
  case binop_type_t::eGT:
  case binop_type_t::eGTE:
  case binop_type_t::eLT:
  case binop_type_t::eLTE:
    return resolve_type("bool");
  default:
    return left;
  }
}

QT
A::analyze_binop(N node) {
  binop_expr_t *expr = node->as.binop;

  QT left = analyze_node(expr->left);
  QT right = analyze_node(expr->right);

  return resolve_binop_result_type(expr->op, left, right);
}

QT A::resolve_receiver(std::optional<specialized_path_t> path_opt) {
  if (!path_opt) return nullptr;

  // Pop last slice of path
  specialized_path_t path = *path_opt;
  if (path.segments.size() <= 1) return nullptr;

  path.segments.pop_back();
  return resolve_type(path);
}

QT
A::analyze_addr_of(N node) {
  addr_of_expr_t *addr_of = node->as.addr_of;

  if (!is_lvalue(addr_of->value)) {
    diagnostics.messages.push_back(error(node->source, addr_of->value->location, "Taking address of temporary", "This expression is temporary, taking the address of this is not allowed."));
    throw analyze_error_t{diagnostics};
  }

  return scope().types.pointer_to(analyze_node(addr_of->value), {pointer_kind_t::eNonNullable}, is_mutable(addr_of->value));
}

QT
A::analyze_defer(N node) {
  analyze_node(node->as.defer_expr->action);
  return resolve_type("void");
}

QT A::analyze_move(N node) {
  move_expr_t *move = node->as.move_expr;
  if (!is_lvalue(move->symbol)) {
    diagnostics.messages.push_back(error(node->source, node->location, "Moving temporary", "`move` can only be performed on lvalues, this expression is temporary."));
    throw analyze_error_t{diagnostics};
  }

  if (move->symbol->kind == ast_node_t::eSymbol) {
    symbol_expr_t *sym = move->symbol->as.symbol;

    auto symbol = scope().resolve(sym->path);
    if (!symbol) {
      diagnostics.messages.emplace_back(error(source, node->location, "Unknown symbol", "Symbol is not known in the current scope."));
      throw analyze_error_t{diagnostics};
    }

    scope().remove(sym->path);
    return scope().types.rvalue_of(symbol->type);
  } else {
    return scope().types.rvalue_of(analyze_node(move->symbol));
  }
}

QT
A::analyze_nil(N node) {
  return scope().types.pointer_to(resolve_type("any"), {pointer_kind_t::eNullable}, true);
}

QT
A::monomorphize(SP<template_decl_t> template_, specialized_path_t instantiation) {
  push_scope();

  auto old_binding = current_binding;
  current_binding = resolve_path(instantiation);

  // Bind the types from `instantiation`
  for (int i = 0; i < template_->name.params(); i++) {
    auto template_binding = template_->name.param(i);
    auto type = instantiation.param(i);

    scope().types.add_template_alias(template_binding.binding, resolve_type(*type));
  }

  N tree = std::make_shared<ast_node_t>(*template_->value);

  // Analyze the body (returns a type involving 'T')
  QT abstract_type = analyze_node(tree);

  pop_scope();
  current_binding = old_binding;

  info->template_instantiations.emplace(instantiation, tree);
  return abstract_type;
}

QT
A::analyze_if(N node) {
  if_stmt_t *stmt = node->as.if_stmt;

  QT condition_type = analyze_node(stmt->condition);
  // TODO: Ensure `condition_type` is bool

  QT pass_type = analyze_node(stmt->pass);
  QT reject_type {nullptr};
  if (stmt->reject)
    reject_type = analyze_node(stmt->reject);

  if (pass_type && reject_type) {
    // If both branches exist, we can treat this as an expression.

    // If the branch types do not match, we resolve to void
    if (*pass_type != *reject_type)
      return resolve_type("void");

    // If they do match, we return that type.
    return pass_type;
  }

  return resolve_type("void");
}

QT
A::analyze_assignment(N node) {
  assign_expr_t *assign = node->as.assign_expr;

  auto symbol_type = analyze_node(assign->where);
  auto value_type = analyze_node(assign->value);

  if (*symbol_type != *value_type
    && !is_implicit_convertible(value_type, symbol_type)) {
    diagnostics.messages.push_back(error(node->source, node->location, "Invalid type", fmt("Assignment to lvalue of type `{}` with value `{}` is invalid.", to_string(symbol_type), to_string(value_type))));
    throw analyze_error_t{diagnostics};
  }

  return symbol_type;
}

QT
A::analyze_while(N node) {
  while_stmt_t *stmt = node->as.while_stmt;

  QT type = analyze_node(stmt->condition);
  QT bool_type = resolve_type("bool");

  if (!is_implicit_convertible(type, bool_type)) {
    diagnostics.messages.push_back(error(node->source, node->location, "Invalid type", fmt("While condition expression has to be convertible to bool, {} is not.", to_string(type))));
    throw analyze_error_t{diagnostics};
  }

  analyze_node(stmt->body);
  return resolve_type("void");
}

QT
A::analyze_for(N node) {
  for_stmt_t *stmt = node->as.for_stmt;
  QT cond = analyze_node(stmt->condition);

  if (stmt->init) {
    push_type_hint(cond);
    QT init = analyze_node(stmt->init);
    pop_type_hint();
  }

  if (stmt->action)
    QT action = analyze_node(stmt->action);

  analyze_node(stmt->body);
  return resolve_type("void");
}

QT
A::analyze_range(N node) {
  range_expr_t *expr = node->as.range_expr;

  QT min = analyze_node(expr->min);
  QT max = analyze_node(expr->max);

  // min, or max might be untyped. If both are untyped, default to i64
  if (min->kind == type_kind_t::eUntypedLiteral &&
      max->kind == type_kind_t::eUntypedLiteral)
    return resolve_type("i64");

  // Otherwise, resolve to whatever is strongly typed.
  if (min->kind != type_kind_t::eUntypedLiteral)
    return min;

  return max;
}

QT
A::analyze_sizeof(N node) {
  auto *expr = node->as.sizeof_expr;
  specialized_path_t path = expr->value;
  node->reset();

  if (auto ty = resolve_type(path); ty) {
    node->kind = ast_node_t::eLiteral;
    node->as.literal_expr = new literal_expr_t {
      .value = std::format("{}", size_of(ty) / 8),
      .type = literal_type_t::eInteger
    };
  }

  if (auto sym = scope().resolve(path); sym) {
    node->kind = ast_node_t::eLiteral;
    node->as.literal_expr = new literal_expr_t {
      .value = std::format("{}", size_of(sym->type) / 8),
      .type = literal_type_t::eInteger
    };
  }

  return resolve_type("u64");
}

QT
A::analyze_array_access(N node) {
  array_access_expr_t *expr = node->as.array_access_expr;

  QT array_type = analyze_node(expr->value);
  QT offset_type = analyze_node(expr->offset);

  if (array_type->kind == type_kind_t::ePointer)
    return array_type->as.pointer->deref();

  if (array_type->kind == type_kind_t::eArray)
    return array_type->as.array->element_type;

  if (array_type->kind == type_kind_t::eSlice)
    return array_type->as.slice->element_type;

  diagnostics.messages.push_back(error(expr->value->source, expr->value->location, "Invalid array access", fmt("This expression is of type {}, which cannot be indexed into.", to_string(array_type))));
  throw analyze_error_t{diagnostics};
}

QT
A::ensure_concrete(QT ty) {
  if (ty->kind == type_kind_t::eUntypedLiteral) {
    untyped_literal_t *literal = ty->as.literal;
    if (literal->type == literal_type_t::eInteger)
      return resolve_type("i32");
    else if (literal->type == literal_type_t::eFloat)
      return resolve_type("f32");
  }
  return ty;
}

bool A::is_within_bounds(__int128_t val, QT target_type) {
  if (target_type->kind == type_kind_t::eInt || target_type->kind == type_kind_t::eUint) {
    __int128_t min {}, max {};

    if (target_type->size == 8 && target_type->kind == type_kind_t::eUint) {
      min = std::numeric_limits<uint8_t>::min();
      max = std::numeric_limits<uint8_t>::max();
    }

    if (target_type->size == 8 && target_type->kind == type_kind_t::eInt) {
      min = std::numeric_limits<int8_t>::min();
      max = std::numeric_limits<int8_t>::max();
    }

    if (target_type->size == 16 && target_type->kind == type_kind_t::eUint) {
      min = std::numeric_limits<uint16_t>::min();
      max = std::numeric_limits<uint16_t>::max();
    }

    if (target_type->size == 16 && target_type->kind == type_kind_t::eInt) {
      min = std::numeric_limits<int16_t>::min();
      max = std::numeric_limits<int16_t>::max();
    }

    if (target_type->size == 32 && target_type->kind == type_kind_t::eUint) {
      min = std::numeric_limits<uint32_t>::min();
      max = std::numeric_limits<uint32_t>::max();
    }

    if (target_type->size == 32 && target_type->kind == type_kind_t::eInt) {
      min = std::numeric_limits<int32_t>::min();
      max = std::numeric_limits<int32_t>::max();
    }

    if (target_type->size == 64 && target_type->kind == type_kind_t::eUint) {
      min = std::numeric_limits<uint64_t>::min();
      max = std::numeric_limits<uint64_t>::max();
    }

    if (target_type->size == 64 && target_type->kind == type_kind_t::eInt) {
      min = std::numeric_limits<int64_t>::min();
      max = std::numeric_limits<int64_t>::max();
    }

    if (val < min || val > max) {
      diagnostics.messages.push_back(error(source, {{0,0},{0,0}}, "Literal overflow", fmt("Value {} does not fit into type `{}`", val, to_string(target_type))));
      throw analyze_error_t{diagnostics};
    }
  }
  return true;
}

bool
A::can_fit_literal(const std::string &str, QT target_type) {
  if (!target_type->is_numeric()) return false;
  __int128_t value = std::stoll(str);
  return is_within_bounds(value, target_type);
}

void A::push_type_hint(QT ty) {
  type_hint_stack.push_back(ty);
}

void A::pop_type_hint() {
  type_hint_stack.pop_back();
}

SP<type_t> A::type_hint() {
  if (type_hint_stack.empty()) return nullptr;
  return type_hint_stack.back();
}

QT
A::analyze_slice(N node) {
  slice_expr_t *slice = node->as.slice_expr;

  QT pointer_type = analyze_node(slice->pointer);
  QT size_type = analyze_node(slice->size);
  QT slice_base_type = resolve_type(slice->type);

  if (pointer_type->kind != type_kind_t::ePointer) {
    diagnostics.messages.push_back(error(node->source, node->location, fmt("Invalid slice operation", "slice(type, pointer, size) takes a `{}` as a second argument, got `{}`", to_string(scope().types.pointer_to(slice_base_type, {pointer_kind_t::eNonNullable}, false)), to_string(pointer_type))));
    throw analyze_error_t{diagnostics};
  }

  if (*pointer_type->as.pointer->deref() != *slice_base_type) {
    diagnostics.messages.push_back(error(node->source, node->location, "Invalid slice operation", fmt("slice(type, pointer, size) takes a `{}` as a second argument, got `{}`", to_string(scope().types.pointer_to(slice_base_type, {pointer_kind_t::eNonNullable}, false)), to_string(pointer_type))));
    throw analyze_error_t{diagnostics};
  }

  return scope().types.slice_of(pointer_type->as.pointer->base, false);
}

QT A::analyze_return(N node) {
  return_stmt_t *stmt = node->as.return_stmt;

  auto fn = current_function();

  QT ty = analyze_node(stmt->value);
  if (!is_implicit_convertible(ty, fn->as.function->return_type)) {
    diagnostics.messages.push_back(error(node->source, node->location, "Unexpected return type", fmt("This function returns `{}`, but type `{}` is not implicitely convertible to it.", to_string(fn->as.function->return_type), to_string(ty))));
    throw analyze_error_t{diagnostics};
  }

  return resolve_type("void");
}

QT
A::analyze_member_access(N node) {
  member_access_expr_t *expr = node->as.member_access;

  auto left = analyze_node(expr->object);
  return resolve_member_access(left, expr->member);
}

QT
A::analyze_array_initialize(N node) {
  array_initialize_expr_t *expr = node->as.array_initialize_expr;

  auto hinted_type = type_hint();
  SP<type_t> base_type = nullptr;

  if (hinted_type) {
    if (hinted_type->kind == type_kind_t::eArray)
      base_type = hinted_type->as.array->element_type;
    else if (hinted_type->kind == type_kind_t::eSlice)
      base_type = hinted_type->as.slice->element_type;
  }

  for (auto &node : expr->values) {
    auto element_type = analyze_node(node);

    if (!base_type) {
      base_type = element_type;
    }

    if (!is_implicit_convertible(element_type, base_type)) {
      diagnostics.messages.push_back(error(node->source, node->location, "Invalid type", fmt("Expected expression to be of type `{}`, but instead is `{}`", to_string(hinted_type), to_string(element_type))));
      throw analyze_error_t {diagnostics};
    }
  }

  return scope().types.array_of(base_type, expr->values.size());
}

QT
A::analyze_attribute(N node) {
  attribute_decl_t *attr = node->as.attribute_decl;
  return analyze_node(attr->affect);
}

QT
A::analyze_tuple(N node) {
  tuple_expr_t *expr = node->as.tuple_expr;
  QT hinted_type = type_hint();

  std::vector<QT> positional_hints;
  std::unordered_map<std::string, QT> named_hints;

  if (hinted_type && hinted_type->kind == type_kind_t::eTuple) {
    auto tuple_hint = hinted_type->as.tuple;
    for (auto const& [name, type] : tuple_hint->elements) {
      if (std::isdigit(name[0])) {
        positional_hints.push_back(type);
      } else {
        named_hints[name] = type;
      }
    }
  }

  std::vector<std::pair<std::string, QT>> resolved_elements;
  uint64_t positional_idx = 0;

  for (auto &[key, value_node] : expr->elements) {
    QT current_hint = nullptr;
    std::string element_key;

    if (key.has_value()) {
      element_key = *key;
      if (named_hints.count(element_key)) {
        current_hint = named_hints[element_key];
      }
    } else {
      element_key = std::to_string(positional_idx);
      if (positional_idx < positional_hints.size()) {
        current_hint = positional_hints[positional_idx];
      }
      positional_idx++;
    }

    if (current_hint) push_type_hint(current_hint);
    QT resolved_type = ensure_concrete(analyze_node(value_node));
    if (current_hint) pop_type_hint();

    resolved_elements.emplace_back(element_key, resolved_type);
  }

  return scope().types.tuple_of(resolved_elements);
}

QT
A::analyze_enum(N node) {
  enum_decl_t *decl = node->as.enum_decl;
  return scope().types.add_enum(*current_binding, *decl);
}

QT
A::analyze_uninitialized(N) {
  // This expression has no real type, we default to the type hint.
  return type_hint();
}

QT
A::analyze_zero(N) {
  // This expression has no real type, we default to the type hint.
  return type_hint();
}

QT
A::analyze_unary(N node) {
  return analyze_node(node->as.unary->value);
}

N A::expand(const std::string &source) {
  auto src = std::make_shared<source_t>("expansion", source);
  lexer_t lexer(src);
  parser_t parser(lexer, src);
  analyzer_t subanalyzer (src);
  auto info = subanalyzer.analyze(parser.parse());
  return info.unit.declarations[0];
}

QT
A::analyze_node(N &node) {
  QT type {};

  switch (node->kind) {
  case ast_node_t::eTemplate:
  case ast_node_t::eBinding:
    type = analyze_binding(node);

    if (node->kind == ast_node_t::eTemplate) {
      // Templates are "reset" here, which removes them from the AST
      // tree, as they are instantiated on the fly.
      node.reset();
    }

    break;

  case ast_node_t::eFunctionDecl:
    type = analyze_function_decl(node, resolve_receiver(current_binding));
    break;

  case ast_node_t::eFunctionImpl:
    type = analyze_function_impl(node, resolve_receiver(current_binding));
    break;

  case ast_node_t::eCall:
    type = analyze_call(node);
    break;

  case ast_node_t::eBlock:
    type = analyze_block(node);
    break;

  case ast_node_t::eLiteral:
    type = analyze_literal(node);
    break;

  case ast_node_t::eSymbol:
    type = analyze_symbol(node);
    break;

  case ast_node_t::eDeref:
    type = analyze_deref(node);
    break;

  case ast_node_t::eTypeAlias:
    type = analyze_type_alias(node);
    break;

  case ast_node_t::eStructDecl:
    type = analyze_struct_decl(node);
    break;

  case ast_node_t::eStructExpr:
    type = analyze_struct_expr(node);
    break;

  case ast_node_t::eContract:
    type = analyze_contract(node);
    break;

  case ast_node_t::eDeclaration:
    type = analyze_declaration(node);
    break;

  case ast_node_t::eCast:
    type = analyze_cast(node);
    break;

  case ast_node_t::eBinop:
    type = analyze_binop(node);
    break;

  case ast_node_t::eAddrOf:
    type = analyze_addr_of(node);
    break;

  case ast_node_t::eDefer:
    type = analyze_defer(node);
    break;

  case ast_node_t::eMove:
    type = analyze_move(node);
    break;

  case ast_node_t::eNil:
    type = analyze_nil(node);
    break;

  case ast_node_t::eIf:
    type = analyze_if(node);
    break;

  case ast_node_t::eAssignment:
    type = analyze_assignment(node);
    break;

  case ast_node_t::eWhile:
    type = analyze_while(node);
    break;

  case ast_node_t::eFor:
    type = analyze_for(node);
    break;

  case ast_node_t::eRangeExpr:
    type = analyze_range(node);
    break;

  case ast_node_t::eSizeOf:
    type = analyze_sizeof(node);
    break;

  case ast_node_t::eArrayAccess:
    type = analyze_array_access(node);
    break;

  case ast_node_t::eSliceExpr:
    type = analyze_slice(node);
    break;

  case ast_node_t::eReturn:
    type = analyze_return(node);
    break;

  case ast_node_t::eMemberAccess:
    type = analyze_member_access(node);
    break;

  case ast_node_t::eArrayInitializeExpr:
    type = analyze_array_initialize(node);
    break;

  case ast_node_t::eAttribute:
    type = analyze_attribute(node);
    break;

  case ast_node_t::eTupleExpr:
    type = analyze_tuple(node);
    break;

  case ast_node_t::eEnumDecl:
    type = analyze_enum(node);
    break;

  case ast_node_t::eUninitialized:
    type = analyze_uninitialized(node);
    break;

  case ast_node_t::eZero:
    type = analyze_zero(node);
    break;

  case ast_node_t::eUnary:
    type = analyze_unary(node);
    break;

  default:
    assert(false && "Unhandled AST node in analyzer");
  }

  if (type)
    node->type = ensure_concrete(type);
  return type;
}

