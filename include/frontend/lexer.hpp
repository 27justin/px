#pragma once

#include "frontend/source.hpp"
#include <frontend/token.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

struct lexer_t {
  public:
  lexer_t(std::shared_ptr<source_t> source)
    : source(source)
    , token() {}

  token_t
  next();
  token_t
  peek(int delta = 0);
  bool
  eof() const;

  void
  push();
  void
  pop();
  void
  commit();

  private:
  std::shared_ptr<source_t> source;
  token_t                   token;
};
