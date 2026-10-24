#pragma once
// The document: object hierarchy, component definitions and settings.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <Bnd_Box.hxx>

#include "sbr/doc/Object.h"

namespace sbr::doc {

struct CameraState {
  std::array<double, 3> eye{6000, -9000, 6000};
  std::array<double, 3> target{0, 0, 0};
  std::array<double, 3> up{0, 0, 1};
  bool perspective = true;
  double scale = 0.0;  // view scale; 0 = fit on load

  bool operator==(const CameraState& o) const {
    return eye == o.eye && target == o.target && up == o.up && perspective == o.perspective && scale == o.scale;
  }
};

// A saved view (SketchUp "scene"): named camera.
struct Scene {
  std::string name;
  CameraState camera;

  bool operator==(const Scene& o) const { return name == o.name && camera == o.camera; }
};

// Section plane: hides the half-space the normal points to, so the model is
// seen cut open (plans and sections).
struct SectionPlane {
  std::string name;
  std::array<double, 3> origin{0, 0, 0};
  std::array<double, 3> normal{0, 0, -1};
  bool active = true;

  bool operator==(const SectionPlane& o) const {
    return name == o.name && origin == o.origin && normal == o.normal && active == o.active;
  }
};

struct DocumentSettings {
  std::string units = "mm";
  double linearTolerance = tolerance::kLinear;
  CameraState camera;
  std::vector<Scene> scenes;
  std::vector<SectionPlane> sections;
  std::set<std::string> hiddenTags;  // objects with these tags are hidden
};

// The part of the settings that belongs to the model's state (saved, undone
// and rolled back with it). The live camera is view state and stays out.
struct PersistentSettings {
  std::vector<Scene> scenes;
  std::vector<SectionPlane> sections;
  std::set<std::string> hiddenTags;

  static PersistentSettings of(const DocumentSettings& s) { return {s.scenes, s.sections, s.hiddenTags}; }
  void applyTo(DocumentSettings& s) const {
    s.scenes = scenes;
    s.sections = sections;
    s.hiddenTags = hiddenTags;
  }
  bool operator==(const PersistentSettings& o) const {
    return scenes == o.scenes && sections == o.sections && hiddenTags == o.hiddenTags;
  }
  bool operator!=(const PersistentSettings& o) const { return !(*this == o); }
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
// The definition with a placement's own parameters written into its objects
// (rebuilt). Overrides naming objects or keys that no longer exist are
// skipped; `dropped` receives them.
Result<ComponentDefinition> applyOverrides(const ComponentDefinition& definition,
                                           const std::vector<ParameterOverride>& overrides,
                                           const RegenContext& context = {},
                                           std::vector<ParameterOverride>* dropped = nullptr);
// The mirror of a placement: across the definition's YZ plane.
gp_Trsf componentMirror();
// Geometry of a placement: the definition's shape, or, for a placement with
// its own parameters or mirror, that variant built from the definition.
Result<TopoDS_Shape> buildInstanceShape(const ComponentDefinition& definition, const ComponentInstance& instance,
                                        const RegenContext& context = {});
// What makes a placement's shape differ from the definition's (mirror,
// overrides, its own stretches): placements with the same key share it.
std::string placementKey(const ComponentInstance& instance);

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
  // Box around the world shape, kept while the shape and placement stay
  // (checks against every object of a large model compare boxes first).
  Bnd_Box worldBounds(const Object& object) const;
  TopoDS_Shape worldShape(const ObjectId& id) const;

  // --- embedded images (by content hash; never edited, only added) ------
  struct Image {
    std::string name;   // original file name, a hint only
    std::string bytes;  // the file as it was (PNG, JPEG, ...)
  };
  // Adds the image if new; returns its sha-256 (the id objects use).
  std::string addImage(const std::string& bytes, const std::string& name);
  const Image* findImage(const std::string& hash) const;
  const std::map<std::string, Image>& images() const { return images_; }

  // --- materials (shared library, stable ids) ------------------------------
  const std::vector<Material>& materials() const { return materials_; }
  const Material* findMaterial(const std::string& id) const;
  // Material a face of `object` without its own material shows: the
  // object's, else the nearest container's; "" when none applies.
  std::string inheritedMaterial(const Object& object) const;
  // Colour of a material id ("" or unknown: `fallback`).
  Color materialColor(const std::string& id, const Color& fallback) const;
  // Colour of the object where no face says otherwise.
  Color displayColor(const Object& object) const;
  // Opacity of a material id (1 for "" or unknown) / of the object's look.
  double materialOpacity(const std::string& id) const;
  double displayOpacity(const Object& object) const;

  // --- component definitions ----------------------------------------------
  const std::vector<ComponentDefinition>& definitions() const { return definitions_; }
  const ComponentDefinition* findDefinition(const std::string& id) const;
  std::size_t instanceCount(const std::string& definitionId) const;
  RegenContext regenContext() const;

  // Low level mutation used by the command stack. Notifies listeners.
  void insert(Object object, std::optional<std::size_t> index = std::nullopt);
  void replace(const Object& object);
  void remove(const ObjectId& id);
  // Many at once (undo / redo of large steps): the same result as one by
  // one in this order, with one index rebuild and one notification.
  void removeMany(const std::vector<ObjectId>& ids);
  void insertMany(std::vector<std::pair<Object, std::optional<std::size_t>>> objects);
  void replaceMany(const std::vector<const Object*>& objects);
  void putDefinition(const ComponentDefinition& definition);
  void removeDefinition(const std::string& id);
  // Material edits change how many objects look: like settings, they ask
  // consumers for a full (appearance) resync.
  void putMaterial(const Material& material);
  void removeMaterial(const std::string& id);
  void clear();

  DocumentSettings& settings() { return settings_; }
  const DocumentSettings& settings() const { return settings_; }

  void addListener(Listener listener) { listeners_.push_back(std::move(listener)); }
  void notify() const;

  // --- change tracking (incremental display / snap updates) ----------------
  // Every insert / replace / remove gets a new sequence number. Consumers
  // remember the sequence they last saw and ask what changed since; nullopt
  // means "everything may have changed" (cleared document, persistent
  // settings such as hidden tags changed, or the log no longer reaches back
  // that far): resynchronise fully.
  std::uint64_t sequence() const { return sequence_; }
  std::optional<std::vector<ObjectId>> changedSince(std::uint64_t sequence) const;
  // Persistent settings (tag visibility, sections, scenes) were changed in
  // place: forces a full resynchronisation and notifies listeners.
  void settingsChanged();

 private:
  void touch(const ObjectId& id);
  void invalidateIndex() const;
  void ensureIndex() const;

  std::vector<Object> objects_;
  std::vector<ComponentDefinition> definitions_;
  std::vector<Material> materials_;
  std::map<std::string, Image> images_;
  DocumentSettings settings_;
  std::vector<Listener> listeners_;

  // Lookup indexes, rebuilt lazily after changes that move objects in the
  // ordered list. Order of objects_ (display / file order) is unchanged.
  mutable std::unordered_map<ObjectId, std::size_t> index_;
  mutable std::unordered_map<ObjectId, std::vector<ObjectId>> children_;
  mutable bool indexValid_ = false;
  mutable bool childrenValid_ = false;  // children_ (positions in index_ may still be valid)
  void ensureChildren() const;
  // Variant shapes of placements with their own parameters / mirror, by
  // definition and settings; an entry is valid while the definition's shape
  // is the one it was built from.
  struct VariantEntry {
    TopoDS_Shape definitionShape;
    TopoDS_Shape shape;
  };
  mutable std::map<std::string, VariantEntry> variants_;

  struct LogEntry {
    std::uint64_t sequence;
    ObjectId id;
  };
  std::vector<LogEntry> log_;
  std::uint64_t sequence_ = 0;
  std::uint64_t fullChangeSequence_ = 0;  // last change that affects everything
};

}  // namespace sbr::doc
