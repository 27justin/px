#include "frontend/token.hpp"

const char *to_string(token_type_t ty) {
  using tt = token_type_t;
  switch (ty) {
  case tt::identifier:
    return "<identifier>";
  case tt::keywordIf:
    return "if";
  case tt::keywordFor:
    return "for";
  case tt::keywordWhile:
    return "while";
  case tt::keywordReturn:
    return "return";
  case tt::keywordFn:
    return "fn";
  case tt::keywordExtend:
    return "extend";
  case tt::keywordWith:
    return "with";
  case tt::keywordMixin:
    return "mixin";
  case tt::keywordIn:
    return "in";
  case tt::keywordAuto:
    return "auto";
  case tt::keywordStruct:
    return "struct";
  case tt::keywordStatic:
    return "static";
  case tt::keywordType:
    return "type";
  case tt::keywordLet:
    return "let";
  case tt::keywordVar:
    return "var";
  case tt::keywordSelf:
    return "self";
  case tt::keywordElse:
    return "else";
  case tt::keywordDistinct:
    return "distinct";
  case tt::keywordNil:
    return "nil";
  case tt::keywordZero:
    return "zero";
  case tt::keywordUninitialized:
    return "uninitialized";
  case tt::keywordDefer:
    return "defer";
  case tt::keywordMove:
    return "move";
  case tt::keywordContract:
    return "contract";
  case tt::keywordAs:
    return "as";
  case tt::keywordImport:
    return "import";
  case tt::keywordSizeOf:
    return "sizeof";
  case tt::keywordSlice:
    return "slice";
  case tt::keywordEnum:
    return "enum";

  case tt::literalString:
    return "string literal";
  case tt::literalInt:
    return "int literal";
  case tt::literalFloat:
    return "float literal";
  case tt::literalBool:
    return "bool literal";

  case tt::operatorPlus:
    return "+";
  case tt::operatorMinus:
    return "-";
  case tt::operatorDivide:
    return "/";
  case tt::operatorMultiply:
    return "*";
  case tt::operatorEqual:
    return "=";
  case tt::operatorEquality:
    return "==";
  case tt::operatorMod:
    return "%";
  case tt::operatorRange:
    return "..";
  case tt::operatorSize:
    return "#";
  case tt::operatorComma:
    return ",";
  case tt::operatorExclamation:
    return "!";
  case tt::operatorNotEqual:
    return "!=";
  case tt::operatorColon:
    return ":";
  case tt::operatorLiteral:
    return "`";
  case tt::operatorPipe:
    return "|";
  case tt::operatorBooleanOr:
    return "||";
  case tt::operatorXor:
    return "^";
  case tt::operatorAnd:
    return "&";
  case tt::operatorBooleanAnd:
    return "&&";
  case tt::operatorAt:
    return "@";
  case tt::operatorQuestion:
    return "?";
  case tt::operatorDollar:
    return "$";
  case tt::operatorTilde:
    return "~";
  case tt::operatorDot:
    return ".";
  case tt::operatorArrow:
    return "->";
  case tt::operatorLTE:
    return "<=";
  case tt::operatorGTE:
    return ">=";
  case tt::operatorCaret:
    return "^";
  case tt::operatorBind:
    return ":=";
  case tt::operatorDeref:
    return ".*";

  case tt::delimiterLParen:
    return "(";
  case tt::delimiterRParen:
    return ")";
  case tt::delimiterLBrace:
    return "{";
  case tt::delimiterRBrace:
    return "}";
  case tt::delimiterLBracket:
    return "[";
  case tt::delimiterRBracket:
    return "]";
  case tt::delimiterLAngle:
    return "<";
  case tt::delimiterRAngle:
    return ">";
  case tt::delimiterSemicolon:
    return ";";
  case tt::specialEof:
    return "<eof>";

  default: return "<unknown>";
  }
}

bool is_operator(token_type_t ty) {
  using T = token_type_t;
  switch (ty) {
  case T::operatorPlus:
  case T::operatorMinus:
  case T::operatorDivide:
  case T::operatorMultiply:
  case T::operatorEqual:
  case T::operatorEquality:
  case T::operatorMod:
  case T::operatorRange:
  case T::operatorDot:
  case T::operatorSize:
  case T::operatorComma:
  case T::operatorExclamation:
  case T::operatorNotEqual:
  case T::operatorColon:
  case T::operatorLiteral:
  case T::operatorPipe:
  case T::operatorBooleanOr:
  case T::operatorXor:
  case T::operatorAnd:
  case T::operatorBooleanAnd:
  case T::operatorAt:
  case T::operatorQuestion:
  case T::operatorDollar:
  case T::operatorTilde:
  case T::operatorArrow:
  case T::operatorLTE:
  case T::operatorGTE:
  case T::operatorCaret:
  case T::operatorBind:
  case T::operatorDeref:
    return true;
  default:
    return false;
  }
}

bool is_keyword(token_type_t ty) {
  using T = token_type_t;
  switch (ty) {
  case T::keywordIf:
  case T::keywordFor:
  case T::keywordWhile:
  case T::keywordReturn:
  case T::keywordFn:
  case T::keywordExtend:
  case T::keywordWith:
  case T::keywordMixin:
  case T::keywordIn:
  case T::keywordAuto:
  case T::keywordStruct:
  case T::keywordStatic:
  case T::keywordType:
  case T::keywordLet:
  case T::keywordVar:
  case T::keywordSelf:
  case T::keywordElse:
  case T::keywordDistinct:
  case T::keywordZero:
  case T::keywordDefer:
  case T::keywordMove:
  case T::keywordUninitialized:
  case T::keywordNil:
  case T::keywordContract:
  case T::keywordAs:
  case T::keywordImport:
  case T::keywordSizeOf:
  case T::keywordSlice:
  case T::keywordEnum:
  case T::keywordUnion:
    return true;
  default:
    return false;
  }
}

bool is_delimiter(token_type_t ty) {
  using T = token_type_t;
  switch (ty) {
  case T::delimiterLParen:
  case T::delimiterRParen:
  case T::delimiterLBrace:
  case T::delimiterRBrace:
  case T::delimiterLBracket:
  case T::delimiterRBracket:
  case T::delimiterSemicolon:
  case T::delimiterLAngle:
  case T::delimiterRAngle:
    return true;
  default:
    return false;
  }
}

bool is_literal(token_type_t ty) {
  using T = token_type_t;
  switch (ty) {
  case T::literalString:
  case T::literalInt:
  case T::literalFloat:
  case T::literalBool:
    return true;
  default:
    return false;
  }
}

bool is_identifier(token_type_t ty) {
  using T = token_type_t;
  return ty == T::identifier;
}

