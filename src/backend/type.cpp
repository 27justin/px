#include "backend/type.hpp"
#include <memory>
#include <sstream>

bool type_t::is_numeric() const {
  return kind == eInt || kind == eUint || kind == eFloat;
}

bool type_t::is_signed() const {
  return kind == eInt;
}

bool type_t::operator==(const type_t &other) const {
  if (other.kind == type_kind_t::eAlias) {
    if (*other.as.alias->alias == *this)
      return true;
  }

  return to_string(other.name) == to_string(name) &&
    other.kind == kind &&
    other.size == size &&
    other.alignment == alignment;
}

void
struct_layout_t::compute_memory_layout() {
  size_t current_offset = 0;
  size_t max_align = 1; // Minimum alignment is typically 1 byte

  for (auto& member : members) {
    size_t member_align = alignment_of(member.type);
    size_t member_size  = size_of(member.type);

    if (current_offset % member_align != 0) {
      current_offset += (member_align - (current_offset % member_align));
    }

    member.offset = current_offset;
    current_offset += member_size;

    if (member_align > max_align) {
      max_align = member_align;
    }
  }

  size_t final_size = current_offset;
  if (final_size % max_align != 0) {
    final_size += (max_align - (final_size % max_align));
  }

  this->size = final_size * 8; // We store size in bits
  this->alignment = max_align;
}

struct_layout_t::field_t *
struct_layout_t::member(const std::string &name) {
  auto it = members.begin();
  for (; it != members.end(); ++it) {
    if (it->name == name)
      return &*it;
  }
  return nullptr;
}

size_t size_of(SP<type_t> type) {
  return size_of(*type);
}

size_t alignment_of(SP<type_t> type) {
  return alignment_of(*type);
}

size_t size_of(const type_t &type) {
  if (type.kind == type_kind_t::ePointer)
    return sizeof(void*);

  if (type.kind == type_kind_t::eArray)
    return type.size * type.as.array->size;

  return type.size;
}

size_t alignment_of(const type_t &type) {
  if (type.kind != type_kind_t::ePointer)
    return type.alignment;
  return alignof(void*);
}

std::string to_string(SP<type_t> type) {
  return to_string(*type);
}

std::string to_string(const type_t &type) {
  std::stringstream ss;

  switch (type.kind) {
  case type_kind_t::ePointer: {
    pointer_t *ptr = type.as.pointer;
    if (ptr->is_mutable) ss << "var ";
    for (auto &indirection : ptr->indirections) {
      ss << (indirection == pointer_kind_t::eNonNullable ? '!' : '?');
    }
    ss << to_string(ptr->base);
    break;
  }
  case type_kind_t::eArray: {
    array_t *arr = type.as.array;
    ss << "[" << arr->size << "]" << to_string(arr->element_type);
    break;
  }
  case type_kind_t::eSlice: {
    slice_t *arr = type.as.slice;
    if (arr->is_mutable) ss << "var ";
    ss << "[]" << to_string(arr->element_type);
    break;
  }
  case type_kind_t::eRValueReference: {
    ss << "^" << to_string(*type.as.rvalue->base);
    break;
  }
  case type_kind_t::eAlias: {
    return to_string(type.as.alias->alias);
  }
  case type_kind_t::eTuple: {
    ss << "(";
    auto tuple = type.as.tuple;
    for (auto it = tuple->elements.begin(); it != tuple->elements.end(); ++it) {
      bool is_positional = !it->first.empty() && std::isdigit(it->first[0]);

      if (!is_positional) {
        ss << it->first << ": ";
      }

      ss << to_string(it->second);

      auto next_it = std::next(it);
      if (next_it != tuple->elements.end()) {
        ss << ", ";
      }
    }
    ss << ")";
    break;
  }
  default:
    ss << to_string(type.name);
    break;
  }
  return ss.str();
}

SP<type_t>
pointer_t::deref() const {
  if (indirections.size() == 1) {
    return base;
  } else {
    // Strip away first indirection and return that as a new type.
    auto indirections = this->indirections;
    indirections.erase(indirections.begin());

    auto type = std::make_shared<type_t>();
    type->name = base->name;
    type->kind = type_kind_t::ePointer;
    type->size = sizeof(void*);
    type->alignment = sizeof(void*);
    type->as.pointer = new pointer_t {
      .is_mutable = this->is_mutable,
      .indirections = indirections,
      .base = this->base
    };
    return type;
  }
}

SP<type_t>
type_t::make_slice(SP<type_t> base, bool is_mutable) {
  auto type = std::make_shared<type_t>();
  type->size = (sizeof(void*) + sizeof(uint64_t)) * sizeof(void*);
  type->alignment = sizeof(void*);

  slice_t *arr = new slice_t {};
  arr->element_type = base;
  arr->is_mutable = is_mutable;

  type->kind = type_kind_t::eSlice;
  type->as.slice = arr;
  type->name = base->name;

  return type;
}

SP<type_t> type_t::base_type() {
  switch (kind) {
  case type_kind_t::eArray:
    return as.array->element_type;
  case type_kind_t::eSlice:
    return as.slice->element_type;
  case type_kind_t::ePointer:
    return as.pointer->deref();
  default:
    return nullptr;
  }
}

SP<type_t>
pointer_t::pointer_to(pointer_kind_t kind, SP<type_t> base, bool is_mutable) {
  auto type = std::make_shared<type_t>();
  type->size = sizeof(void*);
  type->alignment = sizeof(void*);

  pointer_t *ptr = new pointer_t {};
  ptr->indirections = {kind};
  ptr->is_mutable = is_mutable;
  ptr->base = base;

  if (base->kind == type_kind_t::ePointer) {
    auto base_ptr = base->as.pointer;
    ptr->indirections.insert(ptr->indirections.begin(), base_ptr->indirections.begin(), base_ptr->indirections.end());
  }

  type->kind = type_kind_t::ePointer;
  type->as.pointer = ptr;
  type->name = base->name;

  return type;
}

std::vector<std::pair<std::string, SP<type_t>>>::const_iterator
tuple_t::element(const std::string &name) const {
  for (auto it = elements.cbegin(); it != elements.cend(); ++it) {
    if (it->first == name)
      return it;
  }
  return elements.cend();
}
