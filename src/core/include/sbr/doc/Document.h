#pragma once
// The document: ordered collection of objects plus model settings.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "sbr/doc/Object.h"

namespace sbr::doc {

struct CameraState {
  std::array<double, 3> eye{6000, -9000, 6000};
  std::array<double, 3> target{0, 0, 0};
  std::array<double, 3> up{0, 0, 1};
  bool perspective = true;
  double scale = 0.0;  // view scale; 0 = fit on load
};

struct DocumentSettings {
  std::string units = "mm";
  double linearTolerance = tolerance::kLinear;
  CameraState camera;
};

class Document {
 public:
  using Listener = std::function<void()>;

  const std::vector<Object>& objects() const { return objects_; }
  const Object* find(const ObjectId& id) const;
  std::optional<std::size_t> indexOf(const ObjectId& id) const;

  // Low level mutation used by the command stack. Notifies listeners.
  void insert(Object object, std::optional<std::size_t> index = std::nullopt);
  void replace(const Object& object);
  void remove(const ObjectId& id);
  void clear();

  DocumentSettings& settings() { return settings_; }
  const DocumentSettings& settings() const { return settings_; }

  void addListener(Listener listener) { listeners_.push_back(std::move(listener)); }
  void notify() const;

 private:
  std::vector<Object> objects_;
  DocumentSettings settings_;
  std::vector<Listener> listeners_;
};

}  // namespace sbr::doc
