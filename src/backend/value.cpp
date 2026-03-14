#include "backend/value.hpp"

__int128_t
evaluate_int_literal(const std::string &str) {
  if (str.empty())
    return 0;

  size_t     idx    = 0;
  int        base   = 10;
  __int128_t result = 0;

  // Signedness
  bool negative = false;
  if (str[idx] == '-') {
    negative = true;
    idx++;
  } else if (str[idx] == '+') {
    idx++;
  }

  // Base prefix (0x, 0b and 0o)
  if (idx + 1 < str.size() && str[idx] == '0') {
    char prefix = std::tolower(static_cast<unsigned char>(str[idx + 1]));
    if (prefix == 'x') {
      base = 16;
      idx += 2;
    } else if (prefix == 'b') {
      base = 2;
      idx += 2;
    } else if (prefix == 'o') {
      base = 8;
      idx += 2;
    }
  }

  for (; idx < str.size(); ++idx) {
    char c = str[idx];

    // Skip separators
    if (c == '\'')
      continue;

    int digit = -1;
    if (std::isdigit(static_cast<unsigned char>(c))) {
      digit = c - '0';
    } else if (std::isalpha(static_cast<unsigned char>(c))) {
      digit = std::tolower(static_cast<unsigned char>(c)) - 'a' + 10;
    }

    if (digit == -1 || digit >= base) {
      break;
    }

    // Overflow check (optional but recommended for 128-bit)
    // result = result * base + digit;
    result *= base;
    result += digit;
  }

  return negative ? -result : result;
}
