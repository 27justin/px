#pragma once

#include <string>

#include "backend/type.hpp"
#include "frontend/path.hpp"

//
// Symbols are defined resolvable variables/functions.
//   Every symbol has an associated type.
//

enum class symbol_state_t { eAlive, eMoved };

struct symbol_t {
  std::string name;
  SP<type_t>  type;
  bool        is_mutable;
};
