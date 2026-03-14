#pragma once

#include <cctype>
#include <stdexcept>
#include <string>

// Turn integer literal strings `0xFF1122`, `1234`, `1'000`, `0b11000`, `0o8080`
// into native 128 bit integer.
__int128_t
evaluate_int_literal(const std::string &);
