#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct eof_t {};

struct source_location_t {
  struct {
    int64_t line, column;
  } start, end;
};

struct source_t {
  char
  next();
  char
  peek(int64_t offset = 0) const;
  bool
  eof() const;

  std::string_view
  line(int64_t line) const;

  std::string
  string(int64_t start, int64_t end) const;
  std::string
  string(const source_location_t &) const;

  int64_t
  line() const;
  int64_t
  column() const;

  std::string_view
  name() const;

  void
  push();
  void
  pop();
  // throw away last pushed state
  void
  commit();

  source_t(std::string_view, const std::string &);
  source_t(const std::string &, const std::string &);
  source_t(const char *, const std::string &);
  source_t(const source_t &);
  ~source_t();

  static source_t
  from_file(const std::string &path);

  private:
  // Find offset from `start` to the N-th \n character
  int64_t
  find_line(int64_t) const;

  const char *start, *pointer, *end;
  int64_t     column_ = 0, line_ = 1;
  std::string filename;

  struct state_t {
    int64_t column, line;
    int64_t offset;
  };
  std::vector<state_t> states;
};
