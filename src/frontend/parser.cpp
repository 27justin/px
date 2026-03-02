#include "frontend/parser.hpp"
#include "backend/type.hpp"
#include "frontend/ast.hpp"
#include "frontend/token.hpp"
#include <optional>
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <cassert>
#include <string>

using TT = token_type_t;
using P = parser_t;
using TU = translation_unit_t;

using std::make_unique;
using std::make_shared;

std::string substitute_string_escape_characters(const std::string &input) {
  std::string result;
  result.reserve(input.size());

  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '\\' && i + 1 < input.size()) {
      i++; // Consume the backslash
      switch (input[i]) {
      case 'n': result += '\n'; break;
      case 'r': result += '\r'; break;
      case 't': result += '\t'; break;
      case '\\': result += '\\'; break;
      case '\"': result += '\"'; break;
      case 'x': {
        // Peek ahead to see how many hex digits we have
        std::string hex_str;
        size_t j = i + 1;
        while (j < input.size() && j < i + 5 && isxdigit(input[j])) {
          hex_str += input[j];
          j++;
        }

        if (!hex_str.empty()) {
          // Convert hex string to integer
          unsigned long value = std::stoul(hex_str, nullptr, 16);

          // If it's 4 digits (\x0049), we treat it as two bytes: 0x00 and 0x49
          if (hex_str.size() > 2) {
            result += static_cast<char>((value >> 8) & 0xFF);
            result += static_cast<char>(value & 0xFF);
          } else {
            result += static_cast<char>(value & 0xFF);
          }
          i = j - 1; // Advance the main loop counter
        } else {
          result += 'x'; // Just a literal 'x' if no digits follow
        }
        break;
      }
      default:
        result += input[i]; // Unknown escape, just keep the character
        break;
      }
    } else {
      result += input[i];
    }
  }
  return result;
}

std::pair<int, int> get_binding_power(TT type) {
  switch (type) {
  // Assignment
  case TT::operatorEqual:       return {2, 1};

  // Logical Operators
  case TT::operatorBooleanOr:   return {5, 6};
  case TT::operatorBooleanAnd:  return {7, 8};

  // Comparisons
  case TT::operatorEquality:
  case TT::operatorNotEqual:
  case TT::operatorGTE:
  case TT::operatorLTE:
  case TT::delimiterLAngle: // <
  case TT::delimiterRAngle: // >
    return {10, 11};

  // Range
  case TT::operatorRange:       return {15, 16};

  // Addition / Subtraction
  case TT::operatorPlus:
  case TT::operatorMinus:       return {20, 21};

  // Multiplication / Division / Bitwise operators
  case TT::operatorMultiply:
  case TT::operatorDivide:
  case TT::operatorMod:
  case TT::operatorXor:         return {30, 31};

  // Casting
  case TT::keywordAs:           return {40, 41};

  // Postfix / Primary
  case TT::operatorExclamation: return {60, 61};
  case TT::operatorDot:         return {70, 71};
  case TT::delimiterLBracket:   return {80, 81}; // Array access
  case TT::delimiterLParen:     return {80, 81}; // Function Call
  case TT::operatorDeref:       return {90, 91};
  case TT::delimiterLBrace:     return {95, 0};  // Struct literal

  default:                      return {-1, -1};
  }
}

int get_unary_binding_power(TT type) {
  switch (type) {
    // The `.` and `^` operator are special cases, they are used for
    // certain syntactic sugar operations, and therefore have the
    // highest precedence.
  case TT::operatorDot:
  case TT::operatorXor:
  case TT::operatorExclamation:
    return 75;
  case TT::operatorAnd:
    return 81;
  default:
    return 25;
  }
}

void P::expect(TT ty) {
  auto next = lexer.peek();
  if (next.type == ty) {
    token = lexer.next();
    return;
  } else {
    diagnostics.messages.push_back(error(
        source, next.location, fmt(UNEXPECTED_TOKEN, to_text(token.type)),
        fmt(UNEXPECTED_TOKEN_DETAIL, to_text(next.type), to_text(ty))));
    // TODO: Try to recover to get as much information as possible.
    throw parse_error_t {.diagnostics = diagnostics};
  }
}

void P::expect_any(std::vector<TT> types) {
  token_t current = lexer.peek();
  for (TT ty : types) {
    if (current.type == ty) {
      token = lexer.next();
      return;
    }
  }

  std::stringstream ss;
  for (int64_t i = 0; i < types.size(); ++i) {
    if (i > 0) ss << ", ";
    ss << "`" << to_text(types[i]) << "`";
  }

  auto err = error(source, current.location,
                   fmt(UNEXPECTED_TOKEN, to_text(current.type)),
                   fmt(UNEXPECTED_TOKEN_ANY_DETAIL, to_text(current.type), ss.str()));
  diagnostics.messages.push_back(err);
  throw parse_error_t {.diagnostics = diagnostics};
}

token_type_t P::peek_any(std::vector<TT> types) {
  token_t current = lexer.peek();
  for (TT ty : types) {
    if (current.type == ty) {
      return current.type;
    }
  }

  std::stringstream ss;
  for (int64_t i = 0; i < types.size(); ++i) {
    if (i > 0) ss << ", ";
    ss << "`" << to_text(types[i]) << "`";
  }

  auto err = error(source, current.location,
                   fmt(UNEXPECTED_TOKEN, to_text(current.type)),
                   fmt(UNEXPECTED_TOKEN_ANY_DETAIL, to_text(current.type), ss.str()));
  diagnostics.messages.push_back(err);
  throw parse_error_t {.diagnostics = diagnostics};
}

bool P::maybe(TT ty) {
  if (lexer.peek().type == ty) {
    // Advance if it matches
    token = lexer.next();
    return true;
  }
  return false;
}

bool P::peek(TT ty) {
  if (lexer.peek().type == ty) {
    return true;
  }
  return false;
}

void
P::parse_generic_specifier(template_segment_t &segment) {
  std::vector<generic_t> generics {};

  generic_t generic;
  expect(TT::identifier);
  generic.binding = source->string(token.location);

  if (maybe(TT::operatorColon)) {
    // Specifier
    generic.constraints.push_back(parse_specialized_path());
  }
  generics.push_back(generic);

  segment.bindings.insert(segment.bindings.end(), generics.begin(), generics.end());
}

template_path_t
P::parse_template_path() {
  template_path_t path;
  while (is_keyword(lexer.peek().type) || is_identifier(lexer.peek().type)) {
    token = lexer.next();
    template_segment_t segment;

    segment.name = source->string(token.location);
    try {
      lexer.push();
      if (maybe(TT::delimiterLAngle)) {
        do {
          expect(TT::operatorAt); // Placeholder token
          parse_generic_specifier(segment);
        } while (maybe(TT::operatorComma));
        expect(TT::delimiterRAngle);
      }

      path.segments.push_back(segment);
      lexer.commit();

      if (!maybe(TT::operatorDot)) {
        return path;
      }
    } catch (...) {
      diagnostics.messages.pop_back();
      path.segments.push_back(segment);
      lexer.pop();
      return path;
    }
  }
  return path;
}

specialized_path_t
P::parse_specialized_path() {
  specialized_path_t path;
  while (is_keyword(lexer.peek().type) || is_identifier(lexer.peek().type) || lexer.peek().type == token_type_t::literalInt) {
    token = lexer.next();
    specialized_segment_t segment {};

    segment.name = source->string(token.location);
    auto current_token = token;
    try {
      lexer.push();
      if (maybe(TT::delimiterLAngle)) {
        do {
          auto type = std::make_shared<type_decl_t>(parse_type());
          segment.types.push_back(type);
        } while (maybe(TT::operatorComma));
        expect(TT::delimiterRAngle);
      }

      path.segments.push_back(segment);
      lexer.commit();

      if (!maybe(TT::operatorDot)) {
        return path;
      }
    } catch (...) {
      diagnostics.messages.pop_back();
      // If we errored, this is not a path, but instead something else. (likely a binop)
      segment.types.erase(segment.types.begin(), segment.types.end());
      path.segments.push_back(segment);
      lexer.pop();
      this->token = current_token;
      return path;
    }
  }
  return path;
}

tuple_decl_t
P::parse_tuple_type() {
  tuple_decl_t decl {};
  expect(TT::delimiterLParen);


  // Tuples can have named members.

  for (;;) {
    auto $token = token;
    std::optional<std::string> identifier;
    try {
      lexer.push();
      // Look for a name, this might be omitted.
      expect(TT::identifier);
      identifier = source->string(token.location);
      expect(TT::operatorColon);
      lexer.commit();
    } catch (...) {
      identifier = std::nullopt;
      diagnostics.messages.pop_back();
      lexer.pop();
      token = $token;
    }

    decl.elements.emplace_back(identifier, parse_type());
    if (peek(TT::delimiterRParen))
      break;
    expect(TT::operatorComma);
  }

  expect(TT::delimiterRParen);
  return decl;
}

union_decl_t
P::parse_union_type() {
  expect(TT::keywordUnion);
  expect(TT::delimiterLBrace);

  union_decl_t decl;

  while (!maybe(TT::delimiterRBrace)) {
    expect(TT::identifier);
    std::string identifier = source->string(token.location);
    expect(TT::operatorColon);
    decl.values[identifier] = parse_type();

    maybe(TT::delimiterSemicolon);
  }
  return decl;
}

type_decl_t
P::parse_type() {
  type_decl_t decl {};

  if (peek(TT::keywordUnion)) {
    decl.union_ = std::make_shared<union_decl_t>(parse_union_type());
    return decl;
  }

  if (maybe(TT::keywordVar)) {
    // var type means pointer type
    decl.is_mutable = true;
  }

  if (maybe(TT::delimiterLBracket)) {
    if (maybe(TT::literalInt)) { // Compile-time stack array
      decl.len = make_node<literal_expr_t>(ast_node_t::eLiteral, {.value = source->string(token.location), .type = literal_type_t::eInteger}, token.location, source);
    } else if (peek(TT::identifier)){
      decl.len = make_node<symbol_expr_t>(ast_node_t::eSymbol, {.path = parse_specialized_path()}, token.location, source);
    } else {
      decl.is_slice = true;
    }
    expect(TT::delimiterRBracket);
  }

  if (peek(TT::delimiterLParen)) {// Tuple
    tuple_decl_t tuple = parse_tuple_type();
    decl.tuple = std::make_shared<tuple_decl_t>(tuple);
    return decl;
  }

  while (maybe(TT::operatorExclamation) || maybe(TT::operatorQuestion)) {
    decl.indirections.push_back(token.type == TT::operatorExclamation ? pointer_kind_t::eNonNullable : pointer_kind_t::eNullable);
  }

  decl.name = parse_specialized_path();
  return decl;
}

SP<ast_node_t>
P::parse_return() {
  expect(TT::keywordReturn);

  auto location = token.location;

  return make_node<return_stmt_t>(ast_node_t::eReturn, {
      .value = parse_expression()
    }, location, source);
}

std::vector<SP<ast_node_t>>
P::parse_function_arguments() {
  expect(TT::delimiterLParen);

  std::vector<SP<ast_node_t>> args;
  while (!peek(TT::delimiterRParen)) {
    args.emplace_back(parse_expression());

    if (peek(TT::operatorComma)) {
      // If we have a comma, consume it and carry on.
      expect(TT::operatorComma);
    }
  }
  expect(TT::delimiterRParen);
  return args;
}

SP<ast_node_t>
P::parse_expression(int min_binding_power, bool allow_struct_literal) {
  auto start = token.location.start;
  auto left = parse_primary(allow_struct_literal); // Pass flag to primary

  while (true) {
    auto next = lexer.peek();

    auto [left_binding, right_binding] = get_binding_power(next.type);

    // Stop if it's not an operator/call/brace
    bool is_postfix_or_infix = std::max(left_binding, right_binding) > -1;
    if (!is_postfix_or_infix) break;

    // Check for struct literals
    if (next.type == TT::delimiterLBrace && !allow_struct_literal) {
      break;
    }

    if (left_binding <= min_binding_power)
      break;

    // Consume the operator token (except for cases that handle it internally)
    switch (next.type) {
    case TT::delimiterLParen:
      if (token.location.start.line != next.location.start.line) {
        // Function calls do not work, if the callee is not on the same line.
        return left;
      }

      left = make_node<call_expr_t>(ast_node_t::eCall, {
          .callee = left,
          .arguments = parse_function_arguments()
        }, {start, token.location.end}, source);
      break;
    case TT::operatorDeref: // Postfix '.*'
      expect(TT::operatorDeref);
      left = make_node<deref_expr_t>(ast_node_t::eDeref, {.value = left}, token.location, source);
      break;

    case TT::keywordAs: // Cast 'as'
      expect(TT::keywordAs);
      left = make_node<cast_expr_t>(ast_node_t::eCast, {
          .value = left,
          .type = parse_type()
        }, {start, token.location.end}, source);
      break;

     case TT::operatorEqual:
      token = lexer.next();
      left = make_node<assign_expr_t>(ast_node_t::eAssignment, {.where = left, .value = parse_expression()}, {start, token.location.end}, source);
      continue;

    case TT::operatorRange: {
      token = lexer.next();
      bool is_inclusive = maybe(TT::operatorEqual);
      left = make_node<range_expr_t>(ast_node_t::eRangeExpr, {.min = left, .max = parse_expression(0, allow_struct_literal), .is_inclusive = is_inclusive}, {start, token.location.end}, source);
      continue;
    }

    case TT::delimiterLBrace: {
      expect(TT::delimiterLBrace);
      struct_expr_t init {};
      init.type = left;
      while (!maybe(TT::delimiterRBrace)) {
        expect(TT::identifier);
        auto member = source->string(token.location);
        expect(TT::operatorColon);
        init.values[member] = parse_expression(0, true); // Nested exprs can have structs
        maybe(TT::operatorComma);
      }
      left = make_node<struct_expr_t>(ast_node_t::eStructExpr, init, {start, token.location.end}, source);
      break;
    }

    case TT::delimiterLBracket: {
      // Array Access
      expect(TT::delimiterLBracket);
      left = make_node<array_access_expr_t>(ast_node_t::eArrayAccess, {
          .value = left,
          .offset = parse_expression(0, allow_struct_literal)
        }, {start, token.location.end}, source);
      expect(TT::delimiterRBracket);
      break;
    }

    case TT::operatorDot: {
      expect(TT::operatorDot);
      expect(TT::identifier);
      left = make_node<member_access_expr_t>(ast_node_t::eMemberAccess,
                                             {
                                                 .object = left,
                                                 .member = source->string(token.location)
                                             }, token.location, source);
      break;
    }

    default: {
      token = lexer.next();
      // Standard Binary Operator
      left = make_node<binop_expr_t>(ast_node_t::eBinop, {
          .op = binop_type(next),
          .left = left,
          .right = parse_expression(right_binding, allow_struct_literal)
        }, {start, token.location.end}, source);
      break;
    }
    }
  }
  return left;
}

SP<ast_node_t>
P::parse_slice() {
  // slice(u8, ptr, size)
  expect(TT::keywordSlice);
  auto location = token.location;

  expect(TT::delimiterLParen);

  auto ty = parse_type();
  expect(TT::operatorComma);

  auto ptr = parse_expression();
  expect(TT::operatorComma);

  auto size = parse_expression();
  expect(TT::delimiterRParen);

  location.end = token.location.end;
  return make_node<slice_expr_t>(ast_node_t::eSliceExpr, {
      .type = ty,
      .pointer = ptr,
      .size = size
    }, location, source);
}

SP<ast_node_t>
P::parse_array_initializer() {
  expect(TT::delimiterLBracket);

  auto location = token.location;

  std::vector<SP<ast_node_t>> values;
  while (!maybe(TT::delimiterRBracket)) {
    values.push_back(parse_expression());
    maybe(TT::operatorComma);
  }

  return make_node<array_initialize_expr_t>(ast_node_t::eArrayInitializeExpr, {.values = values}, location, source);
}

SP<ast_node_t>
P::parse_tuple_expression() {
  expect(TT::delimiterLParen);

  auto start = token.location.start;
  if (maybe(TT::delimiterRParen)) {
    return make_node<tuple_expr_t>(ast_node_t::eTupleExpr, {}, {start, token.location.end}, source);
  }

  std::vector<std::pair<std::optional<std::string>, SP<ast_node_t>>> elements;
  uint64_t nmemb = 0;
  auto parse_element = [&]() {
    std::pair<std::optional<std::string>, SP<ast_node_t>> elem {};
    // Check for named element: 'name: expr'
    if (lexer.peek().type == TT::identifier && lexer.peek(1).type == TT::operatorColon) {
      expect(TT::identifier);
      elem.first = source->string(token.location);
      expect(TT::operatorColon);
    }
    elem.second = parse_expression(0, true);
    return elem;
  };

  do {
    elements.emplace_back(parse_element());
    if (!maybe(TT::operatorComma))
      break;
  } while (token.type != TT::delimiterRParen);

  expect(TT::delimiterRParen);
  return make_node<tuple_expr_t>(ast_node_t::eTupleExpr, {.elements = elements}, {start, token.location.end}, source);
}

SP<ast_node_t>
P::parse_primary(bool allow_struct_literal) {
  SP<ast_node_t> primary = nullptr;

  auto next = lexer.peek();
  switch (next.type) {
  case TT::keywordMove:
    expect(TT::keywordMove);
    return make_node<move_expr_t>(ast_node_t::eMove, {parse_primary(allow_struct_literal)}, token.location, source);
  case TT::keywordSelf:
  case TT::identifier:
    primary = make_node<symbol_expr_t>(ast_node_t::eSymbol, {.path = parse_specialized_path()}, token.location, source);
    return primary;
  case TT::keywordSizeOf:
    expect(TT::keywordSizeOf);
    expect(TT::delimiterLParen);
    primary = make_node<sizeof_expr_t>(ast_node_t::eSizeOf, {parse_specialized_path()}, token.location, source);
    expect(TT::delimiterRParen);
    return primary;
  case TT::literalInt:
    expect(TT::literalInt);
    primary = make_node<literal_expr_t>(
        ast_node_t::eLiteral, {
          .value = source->string(token.location),
          .type = literal_type_t::eInteger,
        }, token.location, source);
    return primary;
  case TT::literalString:
    expect(TT::literalString);
    primary = make_node<literal_expr_t>(
      ast_node_t::eLiteral, {
        .value = substitute_string_escape_characters(source->string(token.location)),
          .type = literal_type_t::eString,
        }, token.location, source);
    return primary;
  case TT::literalFloat:
    expect(TT::literalFloat);
    primary = make_node<literal_expr_t>(
        ast_node_t::eLiteral, {
          .value = source->string(token.location),
          .type = literal_type_t::eFloat,
        }, token.location, source);
    return primary;
  case TT::literalBool:
    expect(TT::literalBool);
    primary = make_node<literal_expr_t>(
        ast_node_t::eLiteral, {
          .value = source->string(token.location),
          .type = literal_type_t::eBool,
        }, token.location, source);
    return primary;
  case TT::keywordNil: {
    expect(TT::keywordNil);
    primary = make_shared<ast_node_t>();
    primary->kind = ast_node_t::eNil;
    return primary;
  }
  case TT::keywordSlice: {
    return parse_slice();
  }
  case TT::delimiterLBracket: {
    return parse_array_initializer();
  }
  case TT::delimiterLParen:
    return parse_tuple_expression();
  case TT::keywordZero:
    expect(TT::keywordZero);
    return make_node(ast_node_t::eZero, token.location, source);
  case TT::keywordUninitialized:
    expect(TT::keywordUninitialized);
    return make_node(ast_node_t::eUninitialized, token.location, source);
  default:
    break;
  }

  if (is_operator(next.type)) {
    token = lexer.next();
    auto bp = get_unary_binding_power(next.type);

    auto operand = parse_expression(bp, allow_struct_literal);

    if (next.type == TT::operatorAnd) {
      return make_node<addr_of_expr_t>(ast_node_t::eAddrOf, {.value = operand}, token.location, source);
    }
    return make_node<unary_expr_t>(ast_node_t::eUnary, {.op = next.type, .value = operand}, token.location, source);
  }
  return primary;
}

SP<ast_node_t>
P::parse_if() {
  expect(TT::keywordIf);
  auto location = token.location;

  if_stmt_t stmt{};
  stmt.condition = parse_expression(0, false);
  stmt.pass = parse_block();

  if (maybe(TT::keywordElse)) {
    stmt.reject = parse_block();
  }

  return make_node<if_stmt_t>(ast_node_t::eIf, stmt, location, source);
}

SP<ast_node_t>
P::parse_for() {
  expect(TT::keywordFor);
  auto location = token.location;
  for_stmt_t for_stmt {};

  // 1. Check for "for i in" or "for i :="
  // We look ahead to see if the first identifier is a loop variable
  if (lexer.peek().type == TT::identifier &&
      (lexer.peek(1).type == TT::keywordIn || lexer.peek(1).type == TT::operatorBind || lexer.peek(1).type == TT::operatorColon)) {
    expect(TT::identifier);
    for_stmt.init = make_node<declaration_t>(ast_node_t::eDeclaration, {.identifier = source->string(token.location), .value = make_node<literal_expr_t>(ast_node_t::eLiteral, {.value = "0", .type = literal_type_t::eInteger}, token.location, source), .is_mutable = true}, location, source);

    if (maybe(TT::keywordIn)) {
      // Case: for i in 0..num_files
      for_stmt.condition = parse_expression(0, false);
    } else if (maybe(TT::operatorBind)) {
      // Case: for i := 0; i < n; i += 1
      for_stmt.init = parse_expression(0, false);
      expect(TT::delimiterSemicolon);
      for_stmt.condition = parse_expression(0, false);
      expect(TT::delimiterSemicolon);
      for_stmt.action = parse_expression(0, true); // Post can usually allow structs
    }
  } else {
    // Case: for 0..num_files
    // No iterator name, just a range expression
    for_stmt.init = make_node<declaration_t>(ast_node_t::eDeclaration, {.identifier = "_", .value = make_node<literal_expr_t>(ast_node_t::eLiteral, {.value = "0", .type = literal_type_t::eInteger}, token.location, source)}, token.location, source);
    for_stmt.condition = parse_expression(0, false);
  }

  // Parse the body
  for_stmt.body = parse_block();
  return make_node<for_stmt_t>(ast_node_t::eFor, for_stmt, location, source);
}

SP<ast_node_t>
P::parse_while() {
  expect(TT::keywordWhile);
  while_stmt_t stmt{};
  auto location = token.location;

  stmt.condition = parse_expression(0, false);
  stmt.body = parse_block();
  return make_node<while_stmt_t>(ast_node_t::eWhile, stmt, location, source);
}

SP<ast_node_t>
P::parse_defer() {
  expect(TT::keywordDefer);
  return make_node<defer_expr_t>(ast_node_t::eDefer, {parse_expression()}, token.location, source);
}

SP<ast_node_t>
P::parse_statement() {
  auto type = lexer.peek().type;

  switch (type) {
  case TT::keywordReturn: {
    auto ret = parse_return();
    maybe(TT::delimiterSemicolon);
    return ret;
  }
  case TT::keywordIf: {
    return parse_if();
  }
  case TT::keywordVar: {
    return parse_runtime_binding();
  }
  case TT::keywordLet: {
    return parse_runtime_binding();
  }
  case TT::keywordFor: {
    return parse_for();
  }
  case TT::keywordWhile: {
    return parse_while();
  }
  case TT::keywordDefer: {
    return parse_defer();
  }
  default: {
    // This is either:
    //   A. Implicit return
    //   B. Function call
    //   C. Variable assignment
    //
    // In any case, all these are handled by parse_expression
    auto expr = parse_expression();
    return expr;
  }
  }
  return nullptr;
}

bool
P::is_controlflow(SP<ast_node_t> node) {
  switch (node->kind) {
  case ast_node_t::eFor:
    return true;
  case ast_node_t::eWhile:
    return true;
  case ast_node_t::eIf:
    return true;
  default:
    break;
  }
  return false;
}

SP<ast_node_t>
P::parse_block() {
  expect(TT::delimiterLBrace);

  auto location = token.location;

  block_node_t block;
  block.has_implicit_return = false;
  while (!peek(TT::delimiterRBrace)) {
    auto statement = parse_statement();
    block.body.push_back(statement);

    if (is_controlflow(statement) == false) {
      if (!maybe(TT::delimiterSemicolon)) {
        if (!block.has_implicit_return)
          block.has_implicit_return = true;
        else {
          diagnostics.messages.push_back(error(source, token.location, "Multiple return", "Only one return expression is allowed, this block has multiple."));
          throw parse_error_t{diagnostics};
        }
      }
    }
  }

  if (block.body.size() > 0 && is_controlflow(block.body.back()) && !block.has_implicit_return) {
    block.has_implicit_return = true;
  }

  expect(TT::delimiterRBrace);
  return make_node<block_node_t>(ast_node_t::eBlock, block, location, source);
}

// Helper to parse a single parameter
function_parameter_t P::parse_function_parameter() {
    bool is_mutable = maybe(TT::keywordVar);
    bool is_self_ref = maybe(TT::operatorExclamation);

    expect_any({TT::identifier, TT::keywordSelf});
    std::string name = source->string(token.location);
    bool is_self = (token.type == TT::keywordSelf);

    type_decl_t type {};
    bool is_rvalue = false;

    // 'self' can skip the colon and type
    if (is_self && !peek(TT::operatorColon)) {
        return {name, type, is_mutable, is_self, is_self_ref, is_rvalue};
    }

    expect(TT::operatorColon);
    is_rvalue = maybe(TT::operatorXor);
    type = parse_type();

    return {name, type, is_mutable, is_self, is_self_ref, is_rvalue};
}

SP<ast_node_t>
P::parse_function() {
  expect(TT::keywordFn);
  auto location = token.location;
  function_decl_t decl {};

  expect(TT::delimiterLParen);

  // Parse parameter list
  while (!peek(TT::delimiterRParen)) {
    if (maybe(TT::operatorRange)) {
      decl.is_var_args = true;
      break; // Var-args is always the last parameter
    }

    decl.parameters.push_back(parse_function_parameter());

    if (!maybe(TT::operatorComma)) break;
  }

  expect(TT::delimiterRParen);

  // Return type
  if (maybe(TT::operatorArrow)) {
    decl.return_type = parse_type();
  } else {
    decl.return_type.name = {"void"};
  }

  // Body vs Declaration
  if (peek(TT::delimiterLBrace)) {
    return make_node<function_impl_t>(
      ast_node_t::eFunctionImpl, 
      { .declaration = decl, .block = parse_block() }, 
      location, source
      );
  }

  return make_node<function_decl_t>(ast_node_t::eFunctionDecl, decl, location, source);
}

SP<ast_node_t>
P::parse_type_alias() {
  type_alias_decl_t alias {};

  auto location = token.location;
  expect(TT::keywordType);

  if (maybe(TT::keywordDistinct))
    alias.is_distinct = true;
  else
    alias.is_distinct = false;

  alias.type = parse_type();

  location.end = token.location.end;
  return make_node<type_alias_decl_t>(ast_node_t::eTypeAlias, alias, location, source);
}

SP<ast_node_t>
P::parse_struct() {
  expect(TT::keywordStruct);
  auto location = token.location;

  expect(TT::delimiterLBrace);
  struct_decl_t decl {};

  while (!maybe(TT::delimiterRBrace)) {
    expect(TT::identifier);
    std::string member_name = source->string(token.location);

    expect(TT::operatorColon);

    type_decl_t type = parse_type();
    maybe(TT::operatorComma);
    decl.members.push_back({
        .name = member_name,
        .type = type
      });

  }
  return make_node<struct_decl_t>(ast_node_t::eStructDecl, decl, location, source);
}

SP<ast_node_t>
P::parse_contract() {
  expect(TT::keywordContract);
  contract_decl_t decl {};
  auto location = token.location;

  expect(TT::delimiterLBrace);

  while (!maybe(TT::delimiterRBrace)) {
    decl.requirements.push_back(parse_identifier());
  }

  return make_node<contract_decl_t>(ast_node_t::eContract, decl, location, source);
}

attribute_decl_t P::parse_attributes() {
  attribute_decl_t decl{};

  expect(TT::operatorAt);
  expect(TT::delimiterLParen);

  while (!maybe(TT::delimiterRParen)) {
    auto next = lexer.peek();
    if (!is_keyword(next.type) && !is_identifier(next.type)) {
      expect(TT::identifier);
    }
    token = lexer.next();
    std::string name = source->string(token.location);

    expect(TT::operatorColon);

    expect_any({TT::literalBool, TT::literalInt, TT::literalFloat, TT::literalString});
    switch (token.type) {
    case TT::literalBool:
      decl.attributes[name] = literal_expr_t{.value = source->string(token.location), .type = literal_type_t::eBool};
      break;
    case TT::literalInt:
      decl.attributes[name] = literal_expr_t{.value = source->string(token.location), .type = literal_type_t::eInteger};
      break;
    case TT::literalFloat:
      decl.attributes[name] = literal_expr_t{.value = source->string(token.location), .type = literal_type_t::eFloat};
      break;
    case TT::literalString:
      decl.attributes[name] = literal_expr_t{.value = source->string(token.location), .type = literal_type_t::eString};
      break;
    default:
      break;
    }

    maybe(TT::operatorComma);
  }
  return decl;
}

SP<ast_node_t>
P::parse_enum() {
  expect(TT::keywordEnum);

  auto location = token.location;

  expect(TT::delimiterLBrace);

  enum_decl_t decl {};
  int64_t value = 0;
  while (!maybe(TT::delimiterRBrace)) {
    expect(TT::identifier);
    std::string name = source->string(token.location);

    // We might hardcode the enum value
    if (maybe(TT::operatorEqual)) {
      expect(TT::literalInt);
      value = std::stoll(source->string(token.location));
    }

    decl.values[name] = value++;

    maybe(TT::operatorComma);
  }

  return make_node<enum_decl_t>(ast_node_t::eEnumDecl, decl, location, source);
}

SP<ast_node_t>
P::parse_binding() {
  SP<ast_node_t> binding {nullptr};

  std::optional<attribute_decl_t> attributes;
  if (peek(TT::operatorAt)) {
    attributes = parse_attributes();
  }

  if(peek(TT::keywordFn)) {
    binding = parse_function();
    goto done;
  }

  if (peek(TT::keywordType)) {
    binding = parse_type_alias();
    goto done;
  }

  if (peek(TT::keywordStruct)) {
    binding = parse_struct();
    goto done;
  }

  if (peek(TT::keywordEnum)) {
    binding = parse_enum();
    goto done;
  }

  if (peek(TT::keywordContract)) {
    binding = parse_contract();
    goto done;
  }

  // Compile time expression
  binding = parse_expression();
  done:
  if (attributes) {
    attributes->affect = binding;
    return make_node<attribute_decl_t>(ast_node_t::eAttribute, *attributes, token.location, source);
  }
  return binding;
}

SP<ast_node_t>
P::parse_runtime_binding() {
  declaration_t decl;

  if (maybe(TT::keywordVar))
    decl.is_mutable = true;
  else if (maybe(TT::keywordLet))
    decl.is_mutable = false;

  auto location = token.location;

  expect(TT::identifier);
  location.end = token.location.end;
  decl.identifier = source->string(token.location);

  if (maybe(TT::operatorBind)) {
    // Infer the type
    decl.type = std::nullopt;
    decl.value = parse_expression();
  } else if (maybe(TT::operatorColon)) {
    decl.type = parse_type();
    if (maybe(TT::operatorEqual)) {
      // Initialization
      decl.value = parse_expression();
    }
  }
  return make_node<declaration_t>(ast_node_t::eDeclaration, decl, location, source);
}

bool
P::is_simple_path() {
  if (is_templated_path()) return false;
  if (is_specialized_path()) return false;

  auto token = this->token;
  lexer.push();

  bool match = true;
  while(maybe(TT::identifier)) {
    if (maybe(TT::operatorDot))
      continue;
    else
      break;
  }

  lexer.pop();
  this->token = token;
  return true;
}

bool
P::is_specialized_path() {
  auto token = this->token;
  lexer.push();

  // A specialized path is a path which is templated, but has no generics.
  // E.g. `std.vector<@T>` is not a specialized path
  //      `std.vector<bool>`, is.

  bool match = false;
  while(maybe(TT::identifier)) {
    if (maybe(TT::delimiterLAngle)) {
      match = true;

      do {
        if (maybe(TT::operatorAt)) {
          // Placeholders directly violate our specialization
          // requirement
          match = false;
          goto exit;
        }

        parse_type();
      } while (maybe(TT::operatorComma));

      expect(TT::delimiterRAngle);
    }

    if (maybe(TT::operatorDot))
      continue;
    else
      break;
  }
  exit:

  lexer.pop();
  this->token = token;
  return match;
}

bool
P::is_templated_path() {
  auto token = this->token;
  lexer.push();

  // A specialized path is a path which is templated, but has no generics.
  // E.g. `std.vector<@T>` is not a specialized path
  //      `std.vector<bool>`, however is.

  bool match = false;
  while(maybe(TT::identifier)) {
    if (maybe(TT::delimiterLAngle)) {
      match = true;

      do {
        if (!maybe(TT::operatorAt)) {
          // Placeholders directly violate our specialization
          // requirement
          match = false;
          goto exit;
        }

        template_segment_t segment {};
        parse_generic_specifier(segment);
      } while (maybe(TT::operatorComma));

      expect(TT::delimiterRAngle);
    }

    if (maybe(TT::operatorDot))
      continue;
    else
      break;
  }
  exit:

  lexer.pop();
  this->token = token;
  return match;
}

SP<ast_node_t>
P::parse_identifier() {
  peek_any({TT::identifier});

  // <path> : <type>? = ...
  // Path here might either be:
  //  A. Specialized (e.g. std.vector<bool>)
  //  B. Simple (e.g. i32.ok)
  //  C. Templated (e.g. std.vector<@T>)

  bool is_template = is_templated_path();
  bool is_simple = is_simple_path();
  bool is_specialized = is_specialized_path();

  if (is_simple || is_specialized) {
    auto path = parse_specialized_path();
    if (maybe(TT::operatorBind)) {
      return make_node<binding_decl_t>(ast_node_t::eBinding, {path, parse_binding()}, token.location, source);
    }

    if (maybe(TT::operatorColon)) {
      auto type = parse_type();
      if (maybe(TT::operatorEqual))
        return make_node<binding_decl_t>(ast_node_t::eBinding, {path, parse_binding(), type}, token.location, source);

      return make_node<binding_decl_t>(ast_node_t::eBinding, {path, nullptr, type}, token.location, source);
    }
  } else {
    auto path = parse_template_path();

    if (maybe(TT::operatorBind)) {
      return make_node<template_decl_t>(ast_node_t::eTemplate, {path, parse_binding()}, token.location, source);
    }

    if (maybe(TT::operatorColon)) {
      auto type = parse_type();
      if (maybe(TT::operatorEqual))
        return make_node<template_decl_t>(ast_node_t::eTemplate, {path, parse_binding(), type}, token.location, source);

      return make_node<template_decl_t>(ast_node_t::eTemplate, {path, nullptr, type}, token.location, source);
    }
  }
  return nullptr;
}

specialized_path_t
P::parse_import() {
  expect(TT::keywordImport);
  return parse_specialized_path();
}

translation_unit_t
P::parse() {
  translation_unit_t tu {.source = source};

  // Parse until EOF reached
  while (!lexer.eof()) {
    if (lexer.peek().type == TT::specialEof) break;

    switch (lexer.peek().type) {
    case TT::identifier:
      tu.declarations.push_back(parse_identifier());
      break;
    case TT::keywordImport:
      tu.imports.push_back(parse_import());
      break;
    default:
      assert(false && "Unhandled parse identifier");
      break;
    }
  }

  if (diagnostics.messages.size() > 0) {
    // Pass our diagnostics to the caller
    throw parse_error_t {.diagnostics = std::move(diagnostics)};
  }
  return tu;
}

binop_type_t P::binop_type(const token_t &tok) {
  using BT = binop_type_t;
  switch (tok.type) {
  case TT::operatorPlus:
    return BT::eAdd;
  case TT::operatorMinus:
    return BT::eSubtract;
  case TT::operatorDivide:
    return BT::eDivide;
  case TT::operatorMultiply:
    return BT::eMultiply;
  case TT::operatorBooleanAnd:
    return BT::eAnd;
  case TT::operatorBooleanOr:
    return BT::eOr;
  case TT::operatorEquality:
    return BT::eEqual;
  case TT::operatorNotEqual:
    return BT::eNotEqual;
  case TT::delimiterLAngle:
    return BT::eLT;
  case TT::delimiterRAngle:
    return BT::eGT;
  case TT::operatorGTE:
    return BT::eGTE;
  case TT::operatorLTE:
    return BT::eLTE;
  case TT::operatorMod:
    return BT::eMod;
  default:
    assert(false && "Invalid binop token");
  }
}

