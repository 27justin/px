#pragma once

#include <cstdint>

#include "frontend/source.hpp"

enum class token_type_t {
  identifier,

  keywordIf,
  keywordFor,
  keywordWhile,
  keywordReturn,
  keywordFn,
  keywordExtend,
  keywordWith,
  keywordMixin,
  keywordIn,
  keywordAuto,
  keywordStruct,
  keywordStatic,
  keywordType,
  keywordExtern,
  keywordLet,
  keywordVar,
  keywordSelf,
  keywordElse,
  keywordDistinct,
  keywordNil,
  keywordZero,
  keywordDefer,
  keywordMove,
  keywordUninitialized,
  keywordContract,
  keywordAs,
  keywordImport,
  keywordSizeOf,
  keywordSlice,
  keywordEnum,
  keywordUnion,

  literalString,
  literalInt,
  literalFloat,
  literalBool,

  operatorPlus,
  operatorMinus,
  operatorDivide,
  operatorMultiply,
  operatorEqual,
  operatorEquality,
  operatorMod,
  operatorRange, // <operatorDot,operatorDot>
  operatorDot,
  operatorSize,
  operatorComma,
  operatorExclamation,
  operatorNotEqual,
  operatorColon,
  operatorLiteral,
  operatorPipe,
  operatorBooleanOr,
  operatorXor,
  operatorAnd,
  operatorBooleanAnd,
  operatorAt,
  operatorQuestion,
  operatorDollar,
  operatorTilde,
  operatorArrow,
  operatorLTE, // <=
  operatorGTE, // >=
  operatorCaret,
  operatorBind, // :=
  operatorDeref, // <expression>.*

  delimiterLParen,
  delimiterRParen,
  delimiterLBrace,
  delimiterRBrace,
  delimiterLBracket,
  delimiterRBracket,
  delimiterSemicolon,
  delimiterLAngle,
  delimiterRAngle,

  specialEof,
  specialInvalid
};

struct token_t {
  token_type_t type;
  // Relative span (line & column)
  source_location_t location;
};

const char *to_text(token_type_t);

bool is_operator(token_type_t);
bool is_keyword(token_type_t);
bool is_delimiter(token_type_t);
bool is_literal(token_type_t);
bool is_identifier(token_type_t);


