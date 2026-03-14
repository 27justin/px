#include "frontend/source.hpp"
#include "frontend/diagnostic.hpp"

#include <cassert>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

#define MIN(a, b) ((a < b) ? (a) : (b))

source_t
source_t::from_file(const std::string &path) {
  std::ifstream stream(path);
  if (!stream.good()) {
    throw std::runtime_error{ "Failed to open file " + path };
  }

  std::stringstream ss;
  ss << stream.rdbuf();
  return source_t(ss.str(), path);
}

source_t::source_t(std::string_view view, const std::string &name)
  : filename(name) {
  start = new char[view.size()];
  std::memcpy((void *)start, view.data(), view.size());
  end     = start + view.size();
  pointer = start;
};

source_t::source_t(const std::string &str, const std::string &name)
  : filename(name) {
  start = new char[str.size()];
  std::memcpy((void *)start, str.data(), str.size());
  end     = start + str.size();
  pointer = start;
}

source_t::source_t(const char *str, const std::string &name)
  : filename(name) {
  auto len = std::strlen(str);

  start = new char[len];
  std::memcpy((void *)start, str, len);
  end     = start + len;
  pointer = start;
}

source_t::source_t(const source_t &other) {
  start = new char[other.end - other.start];
  end   = start + (other.end - other.start);
  std::memcpy((void *)start, other.start, other.end - other.start);
  pointer  = start;
  filename = other.filename;
}

source_t::~source_t() {
  delete[] start;
}

bool
source_t::eof() const {
  return pointer == end;
}

char
source_t::next() {
  if (eof())
    return '\0';
  char c = *pointer++;
  if (c == '\n') {
    column_ = 0;
    line_++;
  } else {
    column_++;
  }
  return c;
}

char
source_t::peek(int64_t offset) const {
  const char *pos = pointer + offset;
  assert(pos >= start && pos <= end);

  return *pos;
}

std::string
source_t::string(const source_location_t &location) const {
  int64_t start = find_line(location.start.line);
  int64_t end   = location.end.line == location.start.line ? start : find_line(location.end.line);

  const char *line_start = this->start + start + location.start.column,
             *line_end   = this->start + end + location.end.column;

  assert(line_start >= this->start && line_start <= this->end);
  assert(line_end >= this->start && line_end <= this->end);
  return std::string(line_start, line_end);
}

std::string
source_t::string(int64_t start, int64_t end) const {
  const char *s = this->start + start;
  const char *e = this->start + end;

  assert(s >= this->start && s <= this->end);
  assert(e >= this->start && e <= this->end);

  return std::string(s, e);
}

std::string_view
source_t::line(int64_t line) const {
  auto start = find_line(line);
  auto end   = find_line(line + 1);

  return std::string_view(this->start + start, this->start + end - 1);
}

int64_t
source_t::find_line(int64_t line_no) const {
  const char *p        = start;
  int64_t     cur_line = 1;
  while (p < end && cur_line < line_no) {
    if (*p++ == '\n')
      cur_line++;
  }
  return p - start;
}

int64_t
source_t::line() const {
  return line_;
}

int64_t
source_t::column() const {
  return column_;
}

std::string_view
source_t::name() const {
  return filename;
}

void
source_t::push() {
  states.push_back(state_t{ .column = column_, .line = line_, .offset = (pointer - start) });
}

void
source_t::pop() {
  auto state = states.back();

  line_   = state.line;
  column_ = state.column;
  pointer = start + state.offset;

  states.pop_back();
}

void
source_t::commit() {
  states.pop_back();
}
