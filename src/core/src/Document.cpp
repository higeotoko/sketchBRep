#include "sbr/doc/Document.h"

#include <algorithm>

namespace sbr::doc {

const Object* Document::find(const ObjectId& id) const {
  for (const Object& o : objects_) {
    if (o.id == id) return &o;
  }
  return nullptr;
}

std::optional<std::size_t> Document::indexOf(const ObjectId& id) const {
  for (std::size_t i = 0; i < objects_.size(); ++i) {
    if (objects_[i].id == id) return i;
  }
  return std::nullopt;
}

void Document::insert(Object object, std::optional<std::size_t> index) {
  const std::size_t at = std::min(index.value_or(objects_.size()), objects_.size());
  objects_.insert(objects_.begin() + static_cast<std::ptrdiff_t>(at), std::move(object));
  notify();
}

void Document::replace(const Object& object) {
  for (Object& o : objects_) {
    if (o.id == object.id) {
      o = object;
      notify();
      return;
    }
  }
}

void Document::remove(const ObjectId& id) {
  const auto it = std::remove_if(objects_.begin(), objects_.end(),
                                 [&](const Object& o) { return o.id == id; });
  if (it == objects_.end()) return;
  objects_.erase(it, objects_.end());
  notify();
}

void Document::clear() {
  objects_.clear();
  settings_ = DocumentSettings{};
  notify();
}

void Document::notify() const {
  for (const Listener& listener : listeners_) listener();
}

}  // namespace sbr::doc
