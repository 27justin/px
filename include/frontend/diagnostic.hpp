#pragma once

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "frontend/source.hpp"

enum class diagnostic_level_t { eError, eWarn, eInfo };

struct diagnostic_t {
  diagnostic_level_t level;
  std::string        message,       //< Syntax error, erc.
    detail,                         // Expected X got Y
    suggestion;                     // Did you mean ...

  std::shared_ptr<source_t> source;
  source_location_t         origin; //< What triggered the diagnostic
};

struct diagnostic_stack_t {
  std::vector<diagnostic_t> messages;
};

#define fmt(...) std::format(__VA_ARGS__)

diagnostic_t
warn(std::shared_ptr<source_t> source,
     source_location_t,
     const std::string &message,
     std::string        detail     = "",
     std::string        suggestion = "");

diagnostic_t
error(std::shared_ptr<source_t> source,
      source_location_t,
      const std::string &message,
      std::string        detail     = "",
      std::string        suggestion = "");

std::string
serialize(const diagnostic_t &);
