#include "frontend/ast.hpp"
#include "backend/type.hpp"

#include <iostream>
#include <sstream>
#include <string>

SP<ast_node_t> make_node(ast_node_t::kind_t kind, source_location_t loc, SP<source_t> source) {
  auto node = std::make_shared<ast_node_t>();

  node->kind = kind;
  node->location = loc;
  node->as.raw = nullptr;
  node->source = source;

  return node;
}

void ast_node_t::reset() {
  switch (kind) {
  case eType: delete as.type; break;
  case eDeclaration: delete as.declaration; break;
  case eBinop: delete as.binop; break;
  case eUnary: delete as.unary; break;
  case eSymbol: delete as.symbol; break;
  case eStructDecl: delete as.struct_decl; break;
  case eBlock: delete as.block; break;
  case eFunctionDecl: delete as.fn_decl; break;
  case eFunctionImpl: delete as.fn_impl; break;
  case eFunctionParameter: delete as.fn_param; break;
  case eExtern: delete as.extern_decl; break;
  case eReturn: delete as.return_stmt; break;
  case eCall: delete as.call_expr; break;
  case eLiteral: delete as.literal_expr; break;
  case eMemberAccess: delete as.member_access; break;
  case eAddrOf: delete as.addr_of; break;
  case eIf: delete as.if_stmt; break;
  case eTypeAlias: delete as.alias_decl; break;
  case eCast: delete as.cast; break;
  case eAssignment: delete as.assign_expr; break;
  case eDeref: delete as.deref_expr; break;
  case eAttribute: delete as.attribute_decl; break;
  case eFor: delete as.for_stmt; break;
  case eWhile: delete as.while_stmt; break;
  case eBinding: delete as.binding_decl; break;
  case eStructExpr: delete as.struct_expr; break;
  case eRangeExpr: delete as.range_expr; break;
  case eContract: delete as.contract_decl; break;
  case eSelf:
  case eInvalid:
    break;
  }
  kind = eInvalid;
  as.raw = nullptr;
}

ast_node_t::~ast_node_t() {
  reset();
}

void dump_ast(ast_node_t &node, size_t indent_val) {
  auto indent = [indent_val]() {
    return std::string(indent_val * 2, ' ');
  };

  std::cout << indent();
  switch (node.kind) {
  case ast_node_t::eTupleExpr: {
    tuple_expr_t *tuple = node.as.tuple_expr;
    std::cout << "[Tuple (";

    for (auto &[k, v] : tuple->elements) {
      if (k) std::cout << *k << ": ";
      dump_ast(*v);
      std::cout << ", ";
    }

    std::cout << ")]";
    return;
  }
  case ast_node_t::eRangeExpr: {
    range_expr_t *expr = node.as.range_expr;
    std::cout << "[Range ";
    dump_ast(*expr->min);
    std::cout << "..";
    if (expr->is_inclusive) std::cout << "=";
    dump_ast(*expr->max);
    std::cout << "]";
    return;
  }
  case ast_node_t::eStructExpr: {
    struct_expr_t *expr = node.as.struct_expr;
    std::cout << "[Struct "; if(expr->type) dump_ast(*expr->type, indent_val);
    std::cout << "\n";
    for (auto &[memb, val] : expr->values) {
      std::cout << indent() << "  " << memb << ": ";
      dump_ast(*val, 0);
      std::cout << "\n";
    }
    std::cout << "]";
    return;
  }
  case ast_node_t::eWhile: {
    while_stmt_t *stmt = node.as.while_stmt;
    std::cout << "[While ";
    dump_ast(*stmt->condition, indent_val + 1);std::cout << "\n";
    dump_ast(*stmt->body, indent_val + 1);
    std::cout <<indent()<<"]\n";
    return;
  }
  case ast_node_t::eBinding: {
    binding_decl_t *decl = node.as.binding_decl;

    std::cout << "[Bind " << to_string(decl->name) << "\n";
    dump_ast(*decl->value, indent_val + 1);
    std::cout << "]\n";
    return;
  }
  case ast_node_t::eCast: {
    cast_expr_t *expr = node.as.cast;
    std::cout << "[Cast ";
    dump_ast(*expr->value, 0);
    std::cout << " into ";
    std::cout << to_string(expr->type);
    std::cout << "]";
    return;
  }
  case ast_node_t::eFor: {
    for_stmt_t *stmt = node.as.for_stmt;
    std::cout << "[For\n";
    if (stmt->init) {std::cout << indent(); dump_ast(*stmt->init, indent_val+1); std::cout << "\n";}
    if (stmt->condition) {std::cout << indent(); dump_ast(*stmt->condition, indent_val+1); std::cout << "\n";}
    if (stmt->action) {std::cout << indent(); dump_ast(*stmt->action, indent_val+1); std::cout << "\n";}
    std::cout << indent(); dump_ast(*stmt->body, indent_val+2); std::cout << "\n";
    std::cout << "]\n";
    return;
  }
  case ast_node_t::eAttribute: {
    std::cout << "[Attribute\n";
    for (auto &[n, v] : node.as.attribute_decl->attributes) {
      std::cout << indent() << " " << n << "\n";
    }
    dump_ast(*node.as.attribute_decl->affect, indent_val + 1);
    return;
  }
  case ast_node_t::eNil: {
    std::cout << "[nil]";
    return;
  };
  case ast_node_t::eAssignment: {
    assign_expr_t *expr = node.as.assign_expr;

    std::cout << "[Assign \n";
    dump_ast(*expr->where, indent_val + 1);std::cout << "\n";
    dump_ast(*expr->value, indent_val+1);
    std::cout << "]\n";
    return;
  }
  case ast_node_t::eIf: {
    if_stmt_t *stmt = node.as.if_stmt;
    std::cout << "[If ";
    dump_ast(*stmt->condition, 0);
    std::cout << "\n";
    dump_ast(*stmt->pass, indent_val + 1);
    if (stmt->reject) {
      std::cout << "\n";
      dump_ast(*stmt->reject, indent_val + 1);
    }
    return;
  }
  case ast_node_t::eAddrOf: {
    addr_of_expr_t *addr = node.as.addr_of;
    std::cout << "[Address ";
    dump_ast(*addr->value, 0);
    std::cout << "]";
    return;
  }
  case ast_node_t::eMemberAccess: {
    member_access_expr_t *expr = node.as.member_access;
    std::cout << "[MemberAccess "; dump_ast(*expr->object, 0); std::cout <<"."<< expr->member <<"]";
    return;
  }
  case ast_node_t::eReturn: {
    return_stmt_t &ret = *node.as.return_stmt;
    std::cout << "[Return ";
    if (ret.value) dump_ast(*ret.value);
    std::cout << "]\n";
    return;
  };
  case ast_node_t::eCall: {
    call_expr_t &call = *node.as.call_expr;

    std::cout << "[Call ";
    dump_ast(*call.callee);
    std::cout << " with (";

    for (auto &v : call.arguments) {
      dump_ast(*v);
      std::cout << " ";
    }

    std::cout << ")]";
    return;
  }
  case ast_node_t::eUnary: {
    unary_expr_t &expr = *node.as.unary;
    std::cout << "[Unary " << to_text(expr.op);
    dump_ast(*expr.value);
    std::cout << "]";
    return;
  }
  case ast_node_t::eBinop: {
    binop_expr_t &expr = *node.as.binop;
    std::cout << "[Binary Operation " << (int)(expr.op) << "\n";
    dump_ast(*expr.left, indent_val + 1);
    std::cout << "\n";
    dump_ast(*expr.right, indent_val + 1);
    std::cout << "\n";
    std::cout << indent() << "]\n";
    return;
  }
  case ast_node_t::eBlock: {
    block_node_t &block = *node.as.block;

    std::cout << "[Block\n";
    for (auto &v : block.body) {
      dump_ast(*v, indent_val + 1);std::cout << "\n";
    }
    std::cout << "]";
    return;
  }
  case ast_node_t::eLiteral: {
    literal_expr_t &lit = *node.as.literal_expr;
    std::cout << "[Literal <";
    switch (lit.type) {
    case literal_type_t::eBool:
      std::cout << "bool";
      break;
    case literal_type_t::eInteger:
      std::cout << "int?";
      break;
    case literal_type_t::eFloat:
      std::cout << "f32";
      break;
    case literal_type_t::eString:
      std::cout << "!u8";
      break;
    }
    std::cout << "> \"" << lit.value << "\"]";
    return;
  }
  case ast_node_t::eExtern: {
    auto &decl = *node.as.extern_decl;
    std::cout << "[Extern " << decl.convention << "\n";
    dump_ast(*decl.import, indent_val + 1);
    std::cout << indent() << "]\n";
    return;
  }
  case ast_node_t::eSymbol: {
    symbol_expr_t &lookup = *node.as.symbol;
    // Type or variable
    std::cout << "[Symbol " << to_string(lookup.path) << "]";
    return;
  }
  case ast_node_t::eDeclaration: {
    declaration_t &decl = *node.as.declaration;

    std::cout << "[Declare ";
    std::cout << decl.identifier;
    std::cout << " <";
    if (decl.is_mutable) {
      std::cout << "var ";
    }
    if (decl.type)
      std::cout << to_string(*decl.type);
    else
      std::cout << "infer";
    std::cout << ">";

    if (decl.value) {
      std::cout << " "; dump_ast(*decl.value);
    }
    std::cout << "]\n";
    return;
  }
  case ast_node_t::eType: {
    auto &ty = *node.as.type;

    if (ty.is_mutable) std::cout << "var ";
    for (auto indirection : ty.indirections)
      std::cout << (indirection == pointer_kind_t::eNonNullable ? '!' : '?');

    std::cout << to_string(ty.name);
    return;
  }
  case ast_node_t::eFunctionImpl: {
    auto &impl = *node.as.fn_impl;
    std::cout << "[Function \n";

    dump_ast(*impl.block, indent_val + 1);

    std::cout << "]\n";
    return;
  }
  case ast_node_t::eFunctionDecl: {
    auto &decl = *node.as.fn_decl;
    std::cout << "fn (";

    for (auto i = 0; i < decl.parameters.size(); ++i) {
      std::cout << decl.parameters[i].name << ": " << to_string(decl.parameters[i].type);

      if (i < decl.parameters.size() - 1) std::cout << ", ";
    }
    std::cout << ")";
    std::cout<<" -> "; to_string(decl.return_type);
    return;
  }
  case ast_node_t::eStructDecl: {
    struct_decl_t *decl = node.as.struct_decl;

    std::cout << "[Struct " << decl->name << "\n";
    for (auto &memb : decl->members) {
      std::cout << indent() << "  [" << memb.name << " of " << to_string(memb.type) << "]\n";
    }
    std::cout << "]\n";
    return;
  }
  case ast_node_t::eTypeAlias: {
    type_alias_decl_t *decl = node.as.alias_decl;
    std::cout << "[Alias " << (decl->is_distinct ? " (opaque)" : "") << " -> ";
    std::cout << to_string(decl->type);
    std::cout << "]\n";
    return;
  }
  case ast_node_t::eDeref: {
    deref_expr_t *deref = node.as.deref_expr;
    std::cout << "[Deref ";
    dump_ast(*deref->value, 0);
    std::cout << "]";
    return;
  }
  case ast_node_t::eSelf: {
    std::cout << "[Self]";
    return;
  }
  default:
    std::cerr << "<Unhandled dump_ast node type: " << (int)node.kind << ">\n";
    return;
  }
}

std::string to_string(const type_decl_t &type) {
  std::stringstream ss;
  if (type.is_mutable) ss << "var ";

  if (type.is_slice) {
    ss << "[]";
  }

  if (type.len > 0) {
    ss << "["<<type.len<<"]";
  }

  if (type.indirections.size() > 0) {
    for (auto &dir : type.indirections) {
      ss << (dir == pointer_kind_t::eNonNullable ? '!' : '?');
    }
  }

  ss << to_string(type.name);
  return ss.str();
}
