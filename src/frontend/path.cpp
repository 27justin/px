#include "frontend/path.hpp"
#include "frontend/ast.hpp"
#include "frontend/lexer.hpp"

#include <sstream>
#include <stdexcept>

bool
specialized_path_t::is_simple() const {
  for (auto &segment : segments) {
    if (segment.types.size() > 0)
      return false;
  }

  return true;
}

void
specialized_path_t::push(const std::string &name) {
  segments.emplace_back(name);
}

std::size_t
std::hash<specialized_path_t>::operator()(const specialized_path_t &path) const noexcept {
  return std::hash<std::string>{}(to_string(path));
}

bool
specialized_path_t::operator==(const specialized_path_t &other) const {
  return to_string(*this) == to_string(other);
}

std::string
to_string(const generic_t &generic) {
  std::stringstream ss;
  ss << generic.binding;

  if (generic.constraints.size() > 0) {
    ss << ": ";
    for (const specialized_path_t &elem : generic.constraints) {
      ss << to_string(elem);
    }
  }

  return ss.str();
}

std::string
to_string(const template_path_t &path) {
  std::stringstream ss;
  for (auto i = 0; i < path.segments.size(); ++i) {
    auto &segment = path.segments[i];
    ss << segment.name;
    if (segment.bindings.size() > 0) {
      ss << '<';
      for (auto j = 0; j < segment.bindings.size(); ++j) {
        ss << '@';
        ss << to_string(segment.bindings[j]);
        if (j < segment.bindings.size() - 1)
          ss << ", ";
      }
      ss << ">";
    }
    if (i < path.segments.size() - 1)
      ss << '.';
  }
  return ss.str();
}

std::string
to_string(const specialized_path_t &path) {
  std::stringstream ss;
  for (auto i = 0; i < path.segments.size(); ++i) {
    auto &segment = path.segments[i];
    ss << segment.name;
    if (segment.types.size() > 0) {
      ss << '<';
      for (auto j = 0; j < segment.types.size(); ++j) {
        ss << to_string(*segment.types[j]);
        if (j < segment.types.size() - 1)
          ss << ", ";
      }
      ss << ">";
    }
    if (i < path.segments.size() - 1)
      ss << '.';
  }
  return ss.str();
}

specialized_segment_t::specialized_segment_t(const std::string              &name,
                                             const std::vector<type_decl_t> &types)
  : name(name) {
  for (auto &ty : types) {
    this->types.push_back(std::make_shared<type_decl_t>(ty));
  }
}

specialized_segment_t::specialized_segment_t(const std::string &name)
  : name(name)
  , types({}) {}

specialized_path_t::specialized_path_t() {}

specialized_path_t::specialized_path_t(const std::string &name) {
  segments.push_back(name);
}

specialized_path_t::specialized_path_t(const std::vector<specialized_segment_t> &segments)
  : segments(segments) {}

size_t
template_path_t::params() const {
  size_t bindings = 0;

  for (auto &segment : segments) {
    bindings += segment.bindings.size();
  }

  return bindings;
}

generic_t
template_path_t::param(size_t n) const {
  size_t bindings = 0;

  for (auto &segment : segments) {
    if (n >= bindings && n < bindings + segment.bindings.size()) {
      return segment.bindings[n - bindings];
    }
    bindings += segment.bindings.size();
  }

  throw std::runtime_error{ "Out of range" };
}

std::shared_ptr<type_decl_t>
specialized_path_t::param(size_t n) {
  size_t bindings = 0;

  for (auto &segment : segments) {
    if (n >= bindings && n < bindings + segment.types.size()) {
      return segment.types[n - bindings];
    }
    bindings += segment.types.size();
  }

  throw std::runtime_error{ "Out of range" };
}
