#include "frontend/diagnostic.hpp"
#include <sstream>

diagnostic_t
diagnostic(diagnostic_level_t        level,
           std::shared_ptr<source_t> source,
           const std::string        &message,
           const std::string        &detail,
           const std::string        &suggestion,
           source_location_t         origin) {

  return diagnostic_t{ .level      = level,
                       .message    = message,
                       .detail     = detail,
                       .suggestion = suggestion,
                       .source     = source,
                       .origin     = origin };
}

diagnostic_t
warn(std::shared_ptr<source_t> source,
     source_location_t         loc,
     const std::string        &message,
     std::string               detail,
     std::string               suggestion) {
  return diagnostic(diagnostic_level_t::eWarn, source, message, detail, suggestion, loc);
}

diagnostic_t
error(std::shared_ptr<source_t> source,
      source_location_t         loc,
      const std::string        &message,
      std::string               detail,
      std::string               suggestion) {
  return diagnostic(diagnostic_level_t::eError, source, message, detail, suggestion, loc);
}

#define ANSI_BOLD      "\u001b[1m"
#define ANSI_ITALIC    "\u001b[3m"
#define ANSI_UNDERLINE "\u001b[4m"
#define ANSI_RESET     "\x1b[0m"

#define ANSI_RED    "\x1b[31m"
#define ANSI_GREEN  "\x1b[32m"
#define ANSI_ORANGE "\x1b[33m"
#define ANSI_BLUE   "\x1b[34m"
std::string
serialize(const diagnostic_t &msg) {
  std::stringstream ss;

  ss << msg.source->name() << ":" << msg.origin.start.line << ":" << msg.origin.start.column << ": "
     << ANSI_BOLD;
  switch (msg.level) {
    case diagnostic_level_t::eError:
      ss << ANSI_RED << "error:";
      break;
    case diagnostic_level_t::eWarn:
      ss << ANSI_ORANGE << "warning:";
      break;
    case diagnostic_level_t::eInfo:
      ss << ANSI_BLUE << "info:";
      break;
  }
  ss << ANSI_RESET << " " << msg.message << "\n\n";

  if (msg.origin.start.line > 0) {
    for (size_t i = msg.origin.start.line; i <= msg.origin.end.line; ++i) {
      std::string line = std::string(msg.source->line(i));

      size_t col_start = (i == msg.origin.start.line) ? msg.origin.start.column : 0;
      size_t col_end   = (i == msg.origin.end.line) ? msg.origin.end.column : line.size();

      ss << line.substr(0, col_start);
      ss << "\e[0;91m" << line.substr(col_start, col_end - col_start);
      ss << "\e[0m" << line.substr(col_end) << "\n";
    }
  }

  // Use the start column for indentation logic
  std::string indent = std::string(msg.origin.start.column, ' ');

  if (!msg.detail.empty()) {
    auto repeat = [](std::string str, size_t n) {
      std::string out;
      for (size_t i = 0; i < n; ++i)
        out += str;
      return out;
    };

    ss << indent << "├" << repeat("─", msg.detail.size() + 2) << "┐\n";
    ss << indent << "│ " << msg.detail << " │\n";
    ss << indent << "└" << repeat("─", msg.detail.size() + 2) << "┘\n";
  }

  if (!msg.suggestion.empty()) {
    ss << indent << "  " << msg.suggestion << "\n";
  }

  return ss.str();
}
