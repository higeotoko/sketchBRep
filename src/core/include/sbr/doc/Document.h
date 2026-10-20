#pragma once
// The document: object hierarchy, component definitions and settings.

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

// Shared geometry of a component. Its objects form a small hierarchy of their
// own (parent ids refer to objects inside the definition) expressed in the
// definition's local frame. Instances place the cached compound `shape`.
struct ComponentDefinition {
  std::string id;
  std::string name;
  std::vector<Object> objects;
  TopoDS_Shape shape;
};

// Builds the compound of all leaf geometry of a definition.
Result<TopoDS_Shape> buildDefinitionShape(const ComponentDefinition& definition,
                                          const RegenContext& context = {});

class Document {
 public:
  using Listener = std::function<void()>;

  const std::vector<Object>& objects() const { return objects_; }
  const Object* find(const ObjectId& id) const;
  std::optional<std::size_t> indexOf(const ObjectId& id) const;

  // --- hierarchy ----------------------------------------------------------
  std::vector<ObjectId> children(const ObjectId& id) const;
  // The object and all of its descendants, parents before children.
  std::vector<ObjectId> subtree(const ObjectId& id) const;
  // Top-level ancestor (the object itself when it has no parent).
  ObjectId rootOf(const ObjectId& id) const;
  bool isAncestor(const ObjectId& ancestor, const ObjectId& id) const;
  // Effective visibility (hidden parents hide their children).
  bool isVisible(const ObjectId& id) const;
  gp_Trsf parentTransform(const Object& object) const;  // parent world transform
  gp_Trsf worldTransform(const Object& object) const;
  TopoDS_Shape worldShape(const Object& object) const;
  TopoDS_Shape worldShape(const ObjectId& id) const;

  // --- component definitions ----------------------------------------------
  const std::vector<ComponentDefinition>& definitions() const { return definitions_; }
  const ComponentDefinition* findDefinition(const std::string& id) const;
  std::size_t instanceCount(const std::string& definitionId) const;
  RegenContext regenContext() const;

  // Low level mutation used by the command stack. Notifies listeners.
  void insert(Object object, std::optional<std::size_t> index = std::nullopt);
  void replace(const Object& object);
  void remove(const ObjectId& id);
  void putDefinition(const ComponentDefinition& definition);
  void removeDefinition(const std::string& id);
  void clear();

  DocumentSettings& settings() { return settings_; }
  const DocumentSettings& settings() const { return settings_; }

  void addListener(Listener listener) { listeners_.push_back(std::move(listener)); }
  void notify() const;

 private:
  std::vector<Object> objects_;
  std::vector<ComponentDefinition> definitions_;
  DocumentSettings settings_;
  std::vector<Listener> listeners_;
};

}  // namespace sbr::doc
