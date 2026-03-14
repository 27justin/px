#include "frontend/token.hpp"
#include <cctype>
#include <frontend/lexer.hpp>
#include <functional>
#include <iostream>
#include <string>

bool
is_identifier(char c) {
  return std::isalpha(c) || c == '_';
}

bool
is_identifier_alnum(char c) {
  return std::isalnum(c) || c == '_';
}

bool
is_whitespace(char c) {
  return std::isspace(c);
}

bool
is_number(char c) {
  return isdigit(c);
}

bool
is_number_literal(char c) {
  return std::isdigit(c) || c == '\'' || c == '.';
}

token_type_t
delimiter(char c) {
  if (c == '(')
    return token_type_t::delimiterLParen;
  if (c == ')')
    return token_type_t::delimiterRParen;
  if (c == '{')
    return token_type_t::delimiterLBrace;
  if (c == '}')
    return token_type_t::delimiterRBrace;
  if (c == '[')
    return token_type_t::delimiterLBracket;
  if (c == ']')
    return token_type_t::delimiterRBracket;
  if (c == ';')
    return token_type_t::delimiterSemicolon;
  if (c == '<')
    return token_type_t::delimiterLAngle;
  if (c == '>')
    return token_type_t::delimiterRAngle;
  return token_type_t::specialInvalid;
}

token_type_t
operator_(char c) {
  using enum token_type_t;
  switch (c) {
    case '+':
      return operatorPlus;
    case '-':
      return operatorMinus;
    case '/':
      return operatorDivide;
    case '*':
      return operatorMultiply;
    case '%':
      return operatorMod;
    case '.':
      return operatorDot;
    case '#':
      return operatorSize;
    case '=':
      return operatorEqual;
    case ',':
      return operatorComma;
    case '!':
      return operatorExclamation;
    case ':':
      return operatorColon;
    case '`':
      return operatorLiteral;
    case '|':
      return operatorPipe;
    case '^':
      return operatorXor;
    case '&':
      return operatorAnd;
    case '@':
      return operatorAt;
    case '?':
      return operatorQuestion;
    case '$':
      return operatorDollar;
    case '~':
      return operatorTilde;
    default:
      return specialInvalid;
  }
}

token_type_t
keyword(std::string_view kw) {
  using tt = token_type_t;

  if (kw == "if")
    return tt::keywordIf;
  if (kw == "for")
    return tt::keywordFor;
  if (kw == "while")
    return tt::keywordWhile;
  if (kw == "return")
    return tt::keywordReturn;
  if (kw == "fn")
    return tt::keywordFn;
  if (kw == "extend")
    return tt::keywordExtend;
  if (kw == "with")
    return tt::keywordWith;
  if (kw == "mixin")
    return tt::keywordMixin;
  if (kw == "in")
    return tt::keywordIn;
  if (kw == "auto")
    return tt::keywordAuto;
  if (kw == "struct")
    return tt::keywordStruct;
  if (kw == "static")
    return tt::keywordStatic;
  if (kw == "type")
    return tt::keywordType;
  if (kw == "let")
    return tt::keywordLet;
  if (kw == "var")
    return tt::keywordVar;
  if (kw == "self")
    return tt::keywordSelf;
  if (kw == "else")
    return tt::keywordElse;
  if (kw == "distinct")
    return tt::keywordDistinct;
  if (kw == "nil")
    return tt::keywordNil;
  if (kw == "contract")
    return tt::keywordContract;
  if (kw == "as")
    return tt::keywordAs;
  if (kw == "defer")
    return tt::keywordDefer;
  if (kw == "move")
    return tt::keywordMove;
  if (kw == "import")
    return tt::keywordImport;
  if (kw == "sizeof")
    return tt::keywordSizeOf;
  if (kw == "slice")
    return tt::keywordSlice;
  if (kw == "enum")
    return tt::keywordEnum;
  if (kw == "union")
    return tt::keywordUnion;
  if (kw == "uninitialized")
    return tt::keywordUninitialized;
  if (kw == "zero")
    return tt::keywordZero;

  return tt::identifier;
}

bool
lexer_t::eof() const {
  return source->eof();
}

token_t
lexer_t::next() {
  using tt = token_type_t;
start:
  if (eof()) {
    return token_t{ .type = tt::specialEof };
  }

  token.location = {
    { source->line(), source->column() }
  };
  token.type = tt::specialInvalid;
  char next  = source->next();
  if (is_whitespace(next)) {
    goto start;
  }

  // Operators
  // ----------------------
  if (auto op = operator_(next); op != tt::specialInvalid) {
    token.type = op;

    // Merge certain operators
    switch (op) {
      case tt::operatorEqual:
        if (source->peek() == '=') {
          token.type = tt::operatorEquality;
          source->next();
          goto next;
        }
      case tt::operatorExclamation:
        if (source->peek() == '=') {
          token.type = tt::operatorNotEqual;
          source->next();
          goto next;
        }
      case tt::operatorDot:
        if (source->peek() == '.') {
          token.type = tt::operatorRange;
          source->next();
          goto next;
        }
        if (source->peek() == '*') {
          token.type = tt::operatorDeref;
          source->next();
          goto next;
        }
      case tt::operatorDivide:
        if (source->peek() == '/') { // Single-line comment
          while (!source->eof() && source->next() != '\n')
            ;
          goto start;
        }
        if (source->peek() == '*') { // Multi-line comment
          source->next();            // Move past '*'

          while (!source->eof()) {
            if (source->next() == '*') {
              if (source->peek() == '/') {
                source->next(); // Consume the '/'
                goto start;
              }
            }
          }

          goto start;
        }
      case tt::operatorAnd:
        if (source->peek() == '&') {
          token.type = tt::operatorBooleanAnd;
          source->next();
          goto next;
        }
      case tt::operatorPipe:
        if (source->peek() == '|') {
          token.type = tt::operatorBooleanOr;
          source->next();
          goto next;
        }
      case tt::operatorMinus:
        if (source->peek() == '>') {
          token.type = tt::operatorArrow;
          source->next();
          goto next;
        }
      case tt::operatorColon:
        if (source->peek() == '=') {
          token.type = tt::operatorBind;
          source->next();
          goto next;
        }
      default:
        break;
    }

    token.location.end = { source->line(), source->column() };
  }

  // Identifiers & Keywords
  // ----------------------
  if (is_identifier(next)) {
    while (!source->eof() && is_identifier_alnum(source->peek()))
      source->next();
    token.location.end = { source->line(), source->column() };

    if (source->string(token.location) == "true" || source->string(token.location) == "false") {
      token.type = tt::literalBool;
      return token;
    }

    token.type = keyword(source->string(token.location));
    return token;
  }

  // Delimiters
  // ----------
  if (auto del = delimiter(next); del != tt::specialInvalid) {
    token.type = del;

    // Special case for < and >
    auto next = source->peek();
    switch (next) {
      case '=': {
        source->next();
        if (del == tt::delimiterLAngle)
          token.type = tt::operatorLTE;
        else if (del == tt::delimiterRAngle)
          token.type = tt::operatorGTE;
        break;
      }
      case '<': {
        if (del == tt::delimiterLAngle) {
          source->next();
          token.type = tt::operatorShiftLeft;
        }
        break;
      }
      case '>': {
        if (del == tt::delimiterRAngle) {
          source->next();
          token.type = tt::operatorShiftRight;
        }
      }
      default:
        break;
    }
  }

  // String literals
  // ---------------
  if (next == '\'' || next == '"') {
    char delimiter       = next;
    token.location.start = { source->line(), source->column() };

    while (!source->eof()) {
      char c = source->next();

      if (c == '\\') {
        if (source->eof())
          break; // Handle trailing backslash error

        char escape = source->next();
        switch (escape) {
          case '\'':
            continue;
          case '\"':
            continue;
          default:
            break;
        }
      } else if (c == delimiter) {
        break; // End of string
      }
    }

    token.type         = tt::literalString;
    token.location.end = { source->line(), source->column() - 1 };
    return token;
  }

  // Integer & Float literals
  // ------------------------
  if (is_number(next)) {
    token.type = tt::literalInt;

    // Check for prefixes (0x, 0b, 0o)
    if (next == '0') {
      char prefix      = source->peek();
      bool is_prefixed = false;

      if (prefix == 'x' || prefix == 'X') { // Hex
        source->next();                     // consume 'x'
        while (true) {
          char p = source->peek();
          if (isxdigit(p)) {
            source->next();
            continue;
          }
          if (p == '\'') {
            source->next();
            continue;
          }
          break;
        }
        is_prefixed = true;
      } else if (prefix == 'b' || prefix == 'B') { // Binary
        source->next();                            // consume 'b'
        while (true) {
          char p = source->peek();
          if (p == '0' || p == '1') {
            source->next();
            continue;
          }
          if (p == '\'') {
            source->next();
            continue;
          }
          break;
        }
        is_prefixed = true;
      } else if (prefix == 'o' || prefix == 'O') { // Octal
        source->next();                            // consume 'o'
        while (true) {
          char p = source->peek();
          if (p >= '0' && p <= '7') {
            source->next();
            continue;
          }
          if (p == '\'') {
            source->next();
            continue;
          }
          break;
        }
        is_prefixed = true;
      }

      if (is_prefixed) {
        // Exit early for prefixed ints
        token.location.end = { source->line(), source->column() };
        return token;
      }
    }

    // Standard Decimal and Float logic
    while (true) {
      char p = source->peek();

      // Allow ' as arbitrary thousands separator
      if (p == '\'') {
        source->next();
        continue;
      }

      // Check for float
      if (p == '.') {
        char p2 = source->peek(1);

        // If we are already a float, or the next char is '.', stop here (Range operator case)
        if (token.type == tt::literalFloat || p2 == '.') {
          break;
        }

        // If it's a dot followed by a digit, it's a float
        if (isdigit(p2)) {
          token.type = tt::literalFloat;
          source->next(); // consume '.'
          continue;
        } else {
          break;
        }
      }

      if (isdigit(p)) {
        source->next();
        continue;
      }

      // Scientific notation (e.g., 1.0e10 or 10e-5)
      if ((p == 'e' || p == 'E') && token.type == tt::literalFloat) {
        // Peek for sign after 'e'
        char e1 = source->peek(1);
        if (isdigit(e1) || ((e1 == '+' || e1 == '-') && isdigit(source->peek(2)))) {
          source->next(); // consume 'e'
          if (source->peek() == '+' || source->peek() == '-')
            source->next();
          continue;
        }
      }

      break;
    }
  }

next:
  token.location.end = { source->line(), source->column() };
  return token;
}

token_t
lexer_t::peek(int delta) {
  source->push();
  token_t tok;
  do {
    tok = next();
    delta--;
  } while (delta >= 0);
  source->pop();
  return tok;
}

void
lexer_t::push() {
  source->push();
}

void
lexer_t::pop() {
  source->pop();
}

void
lexer_t::commit() {
  source->commit();
}
