#include "sbr/doc/Modeler.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <BRepTools.hxx>
#include <TopoDS.hxx>

#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Freeform.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"
#include "sbr/geom/ShapeIO.h"

namespace sbr::doc {
namespace {

std::string opKey(std::size_t index) { return "op" + std::to_string(index) + ".distance"; }

bool faceCentroid(const TopoDS_Shape& face, gp_Pnt& centroid) {
  const auto ref = geom::FaceRef::fromFace(TopoDS::Face(face));
  if (!ref) return false;
  centroid = ref->centroid;
  return true;
}

// Collects object/definition changes and turns them into one undoable
// command with a delta order that restores indices correctly on undo.
class ChangeSet {
 public:
  explicit ChangeSet(const Document& document) : document_(document) {}

  void modify(const Object& object) {
    modified_.push_back(
        ObjectDelta{object.id, *document_.find(object.id), object, document_.indexOf(object.id)});
  }
  void add(const Object& object) {
    added_.push_back(ObjectDelta{object.id, std::nullopt, object, std::nullopt});
  }
  void erase(const ObjectId& id) {
    for (const ObjectDelta& d : removed_) {
      if (d.id == id) return;
    }
    removed_.push_back(ObjectDelta{id, *document_.find(id), std::nullopt, document_.indexOf(id)});
  }
  void putDefinition(const ComponentDefinition& definition) {
    const ComponentDefinition* before = document_.findDefinition(definition.id);
    definitions_.push_back(DefinitionDelta{
        definition.id, before ? std::optional<ComponentDefinition>(*before) : std::nullopt,
        definition});
  }

  Command build(const std::string& name) {
    // Undo applies deltas in reverse: removals must come back in ascending
    // index order, so store them descending.
    std::sort(removed_.begin(), removed_.end(), [](const ObjectDelta& a, const ObjectDelta& b) {
      return a.indexBefore.value_or(0) > b.indexBefore.value_or(0);
    });
    Command command{name, {}, definitions_};
    for (auto* list : {&modified_, &added_, &removed_}) {
      command.deltas.insert(command.deltas.end(), list->begin(), list->end());
    }
    return command;
  }

 private:
  const Document& document_;
  std::vector<ObjectDelta> modified_;
  std::vector<ObjectDelta> added_;
  std::vector<ObjectDelta> removed_;
  std::vector<DefinitionDelta> definitions_;
};

gp_Trsf localDelta(const gp_Trsf& parentWorld, const gp_Trsf& worldDelta) {
  return parentWorld.Inverted() * worldDelta * parentWorld;
}

}  // namespace

Modeler::Modeler() : history_(document_) {}

std::string Modeler::uniqueName(const std::string& base) const {
  for (int n = 1;; ++n) {
    const std::string candidate = base + " " + std::to_string(n);
    bool taken = false;
    for (const Object& o : document_.objects()) {
      if (o.name == candidate) {
        taken = true;
        break;
      }
    }
    for (const ComponentDefinition& d : document_.definitions()) {
      if (d.name == candidate) taken = true;
    }
    if (!taken) return candidate;
  }
}

std::vector<ObjectId> Modeler::topmost(const std::vector<ObjectId>& ids) const {
  std::vector<ObjectId> out;
  for (const ObjectId& id : ids) {
    if (!document_.find(id) || std::find(out.begin(), out.end(), id) != out.end()) continue;
    bool covered = false;
    for (const ObjectId& other : ids) {
      if (other != id && document_.isAncestor(other, id)) covered = true;
    }
    if (!covered) out.push_back(id);
  }
  return out;
}

Status Modeler::checkSameParent(const std::vector<ObjectId>& ids, ObjectId& parent) const {
  if (ids.empty()) return Status::failure("Nothing selected");
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const Object* o = document_.find(ids[i]);
    if (!o) return Status::failure("Object not found");
    if (i == 0) parent = o->parent;
    if (o->parent != parent) return Status::failure("Objects must be in the same group");
  }
  return Status::success();
}

std::vector<Object> Modeler::cloneSubtree(const ObjectId& id, const ObjectId& newParent) const {
  std::vector<Object> out;
  std::map<ObjectId, ObjectId> mapping;
  for (const ObjectId& old : document_.subtree(id)) {
    Object c = *document_.find(old);
    c.id = generateObjectId();
    mapping[old] = c.id;
    c.parent = old == id ? newParent : mapping[c.parent];
    out.push_back(std::move(c));
  }
  return out;
}

Result<ObjectId> Modeler::addObject(Object object, const std::string& commandName) {
  if (object.id.empty()) object.id = generateObjectId();
  const Status built = rebuild(object, document_.regenContext());
  if (!built) return Result<ObjectId>::failure(built.message());
  const ObjectId id = object.id;
  history_.execute(Command{commandName, {ObjectDelta{id, std::nullopt, object, std::nullopt}}, {}});
  return id;
}

Status Modeler::commitModified(Object object, const std::string& commandName) {
  const Object* current = document_.find(object.id);
  if (!current) return Status::failure("Object not found");
  // Regenerate the full recipe; on failure the document is left untouched.
  const Status built = rebuild(object, document_.regenContext());
  if (!built) return built;
  history_.execute(Command{
      commandName, {ObjectDelta{object.id, *current, object, document_.indexOf(object.id)}}, {}});
  return Status::success();
}

// --- creation -----------------------------------------------------------------

Result<ObjectId> Modeler::createRectangle(const geom::SketchPlane& plane, double width,
                                          double height, const std::string& name) {
  Object o;
  o.name = name.empty() ? uniqueName("Rectangle") : name;
  o.recipe.base = RectangleProfile{plane, width, height};
  return addObject(std::move(o), "Rectangle");
}

Result<ObjectId> Modeler::createPolygon(const std::vector<gp_Pnt>& points, const std::string& name) {
  Object o;
  o.name = name.empty() ? uniqueName("Face") : name;
  o.recipe.base = PolygonProfile{points};
  return addObject(std::move(o), "Face");
}

Result<ObjectId> Modeler::createPolyline(const std::vector<gp_Pnt>& points,
                                         const std::string& name) {
  Object o;
  o.name = name.empty() ? uniqueName("Line") : name;
  o.recipe.base = PolylineCurve{points};
  return addObject(std::move(o), "Line");
}

Result<ObjectId> Modeler::importShape(const TopoDS_Shape& shape, const std::string& name,
                                      const std::string& source) {
  Object o;
  o.name = name.empty() ? uniqueName("Imported") : name;
  o.recipe.base = ImportedShape{shape, source};
  return addObject(std::move(o), "Import");
}

// --- editing --------------------------------------------------------------------

Result<PushPullResult> Modeler::pushPull(const ObjectId& id, const TopoDS_Face& worldFace,
                                         double distance) {
  const Object* current = document_.find(id);
  if (!current) return Result<PushPullResult>::failure("Object not found");
  if (current->kind() == ObjectKind::Instance) {
    return Result<PushPullResult>::failure(
        "Component instances cannot be pushed/pulled; edit the definition or explode it");
  }
  if (!std::isfinite(distance) || std::abs(distance) < tolerance::kMinDimension) {
    return Result<PushPullResult>::failure("Distance must be non-zero");
  }
  Object object = *current;
  const gp_Trsf world = document_.worldTransform(object);
  const TopoDS_Face face = TopoDS::Face(worldFace.Moved(TopLoc_Location(world.Inverted())));
  const auto pickedNormal = geom::planarFaceNormal(face);
  if (!pickedNormal) return Result<PushPullResult>::failure("Only planar faces can be pushed/pulled");

  auto& mods = object.recipe.modifiers;
  const RegenContext context = document_.regenContext();
  std::string key;

  if (object.kind() == ObjectKind::Face) {
    // Profile face -> parametric extrusion.
    const auto profileNormal = geom::planarFaceNormal(TopoDS::Face(object.shape));
    if (!profileNormal) return Result<PushPullResult>::failure("Profile is not planar");
    const double sign = profileNormal->Dot(*pickedNormal) >= 0 ? 1.0 : -1.0;
    mods.push_back(ExtrudeOp{distance * sign});
    key = opKey(mods.size() - 1);
  } else if (object.kind() == ObjectKind::Solid) {
    // Pulling the cap of a solid whose last step is an extrusion edits the
    // extrusion distance instead of adding a new operation.
    bool isCap = false;
    if (!mods.empty() && std::holds_alternative<ExtrudeOp>(mods.back())) {
      auto& extrude = std::get<ExtrudeOp>(mods.back());
      Recipe before{object.recipe.base, {mods.begin(), mods.end() - 1}};
      const auto profile = regenerate(before, context);
      gp_Pnt profileCentroid, pickedCentroid;
      std::optional<gp_Dir> profileNormal;
      if (profile && !profile->IsNull() && profile->ShapeType() == TopAbs_FACE) {
        profileNormal = geom::planarFaceNormal(TopoDS::Face(profile.value()));
      }
      if (profileNormal && faceCentroid(profile.value(), profileCentroid) &&
          faceCentroid(face, pickedCentroid)) {
        const double sign = extrude.distance >= 0 ? 1.0 : -1.0;
        const gp_Dir capNormal = sign > 0 ? *profileNormal : profileNormal->Reversed();
        const double offset = gp_Vec(profileCentroid, pickedCentroid).Dot(gp_Vec(*profileNormal));
        isCap = pickedNormal->Angle(capNormal) < 1e-6 &&
                std::abs(offset - extrude.distance) < 1e-3;
        if (isCap) {
          extrude.distance += sign * distance;
          key = opKey(mods.size() - 1);
        }
      }
    }
    if (!isCap) {
      auto ref = geom::FaceRef::fromFace(face);
      if (!ref) return Result<PushPullResult>::failure(ref.error());
      mods.push_back(PushPullOp{ref.value(), distance});
      key = opKey(mods.size() - 1);
    }
  } else {
    return Result<PushPullResult>::failure("Push/pull needs a face or a solid");
  }

  const Status committed = commitModified(std::move(object), "Push/Pull");
  if (!committed) return Result<PushPullResult>::failure(committed.message());
  return PushPullResult{id, key};
}

Result<PushPullResult> Modeler::extrude(const ObjectId& id, double distance) {
  const Object* current = document_.find(id);
  if (!current) return Result<PushPullResult>::failure("Object not found");
  if (current->kind() != ObjectKind::Face) {
    return Result<PushPullResult>::failure("Only profile faces can be extruded");
  }
  Object object = *current;
  object.recipe.modifiers.push_back(ExtrudeOp{distance});
  const std::string key = opKey(object.recipe.modifiers.size() - 1);
  const Status committed = commitModified(std::move(object), "Extrude");
  if (!committed) return Result<PushPullResult>::failure(committed.message());
  return PushPullResult{id, key};
}

Result<PushPullResult> Modeler::offset(const ObjectId& id, double distance) {
  const Object* current = document_.find(id);
  if (!current) return Result<PushPullResult>::failure("Object not found");
  if (current->kind() != ObjectKind::Face) {
    return Result<PushPullResult>::failure("Only planar profile faces can be offset");
  }
  Object object = *current;
  object.id = generateObjectId();
  object.name = uniqueName("Offset");
  object.recipe.modifiers.push_back(OffsetOp{distance});
  const std::string key = opKey(object.recipe.modifiers.size() - 1);
  auto added = addObject(std::move(object), "Offset");
  if (!added) return Result<PushPullResult>::failure(added.error());
  return PushPullResult{added.value(), key};
}

Status Modeler::setParameter(const ObjectId& id, const std::string& key, double value) {
  const Object* current = document_.find(id);
  if (!current) return Status::failure("Object not found");
  Object object = *current;
  std::string label = key;
  for (const Parameter& p : doc::parameters(object.recipe)) {
    if (p.key == key) label = p.label;
  }
  const Status changed = doc::setParameter(object.recipe, key, value);
  if (!changed) return changed;
  return commitModified(std::move(object), "Edit " + label);
}

namespace {
template <class Fn>
Status modifyAttribute(Document& document, CommandStack& history, const ObjectId& id,
                       const std::string& name, Fn&& fn) {
  const Object* current = document.find(id);
  if (!current) return Status::failure("Object not found");
  Object object = *current;
  fn(object);
  history.execute(Command{name, {ObjectDelta{id, *current, object, document.indexOf(id)}}, {}});
  return Status::success();
}
}  // namespace

Status Modeler::rename(const ObjectId& id, const std::string& name) {
  if (name.empty()) return Status::failure("Name must not be empty");
  return modifyAttribute(document_, history_, id, "Rename", [&](Object& o) { o.name = name; });
}

Status Modeler::setVisible(const ObjectId& id, bool visible) {
  return modifyAttribute(document_, history_, id, visible ? "Show" : "Hide",
                         [&](Object& o) { o.visible = visible; });
}

Status Modeler::setLayer(const ObjectId& id, const std::string& layer) {
  if (layer.empty()) return Status::failure("Tag must not be empty");
  return modifyAttribute(document_, history_, id, "Set Tag", [&](Object& o) { o.layer = layer; });
}

Status Modeler::setColor(const ObjectId& id, const Color& color) {
  return modifyAttribute(document_, history_, id, "Set Color", [&](Object& o) { o.color = color; });
}

Status Modeler::remove(const std::vector<ObjectId>& ids) {
  ChangeSet changes(document_);
  bool any = false;
  for (const ObjectId& id : ids) {
    if (!document_.find(id)) return Status::failure("Object not found");
  }
  for (const ObjectId& root : topmost(ids)) {
    for (const ObjectId& id : document_.subtree(root)) {
      changes.erase(id);
      any = true;
    }
  }
  if (!any) return Status::failure("Nothing selected");
  history_.execute(changes.build("Delete"));
  return Status::success();
}

// --- placement ------------------------------------------------------------------

Status Modeler::transform(const std::vector<ObjectId>& ids, const gp_Trsf& worldDelta,
                          const std::string& commandName) {
  const std::vector<ObjectId> roots = topmost(ids);
  if (roots.empty()) return Status::failure("Nothing selected");
  ChangeSet changes(document_);
  for (const ObjectId& id : roots) {
    Object o = *document_.find(id);
    o.transform = localDelta(document_.parentTransform(o), worldDelta) * o.transform;
    changes.modify(o);
  }
  history_.execute(changes.build(commandName));
  return Status::success();
}

Status Modeler::move(const std::vector<ObjectId>& ids, const gp_Vec& offset) {
  gp_Trsf t;
  t.SetTranslation(offset);
  return transform(ids, t, "Move");
}

Status Modeler::rotate(const std::vector<ObjectId>& ids, const gp_Ax1& axis, double angle) {
  if (!std::isfinite(angle)) return Status::failure("Angle must be finite");
  gp_Trsf t;
  t.SetRotation(axis, angle);
  return transform(ids, t, "Rotate");
}

Result<std::vector<ObjectId>> Modeler::copy(const std::vector<ObjectId>& ids,
                                            const gp_Trsf& worldDelta, int count) {
  if (count < 1 || count > 10000) {
    return Result<std::vector<ObjectId>>::failure("Copy count must be between 1 and 10000");
  }
  const std::vector<ObjectId> roots = topmost(ids);
  if (roots.empty()) return Result<std::vector<ObjectId>>::failure("Nothing selected");

  ChangeSet changes(document_);
  std::vector<ObjectId> created;
  gp_Trsf step;  // worldDelta^k
  for (int k = 1; k <= count; ++k) {
    step = worldDelta * step;
    for (const ObjectId& id : roots) {
      std::vector<Object> clones = cloneSubtree(id, document_.find(id)->parent);
      Object& root = clones.front();
      root.transform = localDelta(document_.parentTransform(root), step) * root.transform;
      created.push_back(root.id);
      for (const Object& c : clones) changes.add(c);
    }
  }
  history_.execute(changes.build(count == 1 ? "Copy" : "Array"));
  return created;
}

Status Modeler::setPosition(const ObjectId& id, const gp_Vec& translation) {
  return modifyAttribute(document_, history_, id, "Set Position",
                         [&](Object& o) { o.transform.SetTranslationPart(translation); });
}

// --- structure ------------------------------------------------------------------

Result<ObjectId> Modeler::group(const std::vector<ObjectId>& ids, const std::string& name) {
  const std::vector<ObjectId> roots = topmost(ids);
  ObjectId parent;
  const Status same = checkSameParent(roots, parent);
  if (!same) return Result<ObjectId>::failure(same.message());

  Object groupObject;
  groupObject.id = generateObjectId();
  groupObject.parent = parent;
  groupObject.name = name.empty() ? uniqueName("Group") : name;
  groupObject.recipe.base = GroupNode{};

  ChangeSet changes(document_);
  changes.add(groupObject);
  for (const ObjectId& id : roots) {
    Object child = *document_.find(id);
    child.parent = groupObject.id;
    changes.modify(child);
  }
  history_.execute(changes.build("Group"));
  return groupObject.id;
}

Status Modeler::ungroup(const ObjectId& groupId) {
  const Object* g = document_.find(groupId);
  if (!g || g->kind() != ObjectKind::Group) return Status::failure("Not a group");
  ChangeSet changes(document_);
  for (const ObjectId& id : document_.children(groupId)) {
    Object child = *document_.find(id);
    child.parent = g->parent;
    child.transform = g->transform * child.transform;
    changes.modify(child);
  }
  changes.erase(groupId);
  history_.execute(changes.build("Ungroup"));
  return Status::success();
}

Result<ObjectId> Modeler::makeComponent(const std::vector<ObjectId>& ids, const std::string& name) {
  const std::vector<ObjectId> roots = topmost(ids);
  ObjectId parent;
  const Status same = checkSameParent(roots, parent);
  if (!same) return Result<ObjectId>::failure(same.message());

  Bnd_Box box;
  for (const ObjectId& root : roots) {
    for (const ObjectId& id : document_.subtree(root)) {
      const Object* o = document_.find(id);
      if (o->kind() == ObjectKind::Instance) {
        return Result<ObjectId>::failure("Nested components are not supported yet");
      }
      box.Add(geom::exactBounds(document_.worldShape(*o)));
    }
  }
  if (box.IsVoid()) return Result<ObjectId>::failure("Selection has no geometry");

  // The component origin is the lower corner of the selection, as in SketchUp.
  const gp_Trsf parentWorld = parent.empty() ? gp_Trsf() : document_.worldTransform(*document_.find(parent));
  const gp_Pnt origin = box.CornerMin().Transformed(parentWorld.Inverted());
  gp_Trsf toOrigin;
  toOrigin.SetTranslation(gp_Vec(origin.XYZ()).Reversed());

  ComponentDefinition definition;
  definition.id = generateObjectId();
  definition.name = name.empty() ? uniqueName("Component") : name;
  for (const ObjectId& root : roots) {
    for (const ObjectId& id : document_.subtree(root)) {
      Object o = *document_.find(id);
      if (id == root) {
        o.parent.clear();
        o.transform = toOrigin * o.transform;
      }
      definition.objects.push_back(std::move(o));
    }
  }
  auto shape = buildDefinitionShape(definition);
  if (!shape) return Result<ObjectId>::failure(shape.error());
  definition.shape = shape.value();

  Object instance;
  instance.id = generateObjectId();
  instance.parent = parent;
  instance.name = definition.name;
  instance.transform.SetTranslation(gp_Vec(origin.XYZ()));
  instance.recipe.base = ComponentInstance{definition.id};
  instance.shape = definition.shape;

  ChangeSet changes(document_);
  changes.putDefinition(definition);
  for (const ObjectId& root : roots) {
    for (const ObjectId& id : document_.subtree(root)) changes.erase(id);
  }
  changes.add(instance);
  history_.execute(changes.build("Make Component"));
  return instance.id;
}

Result<std::vector<ObjectId>> Modeler::explode(const ObjectId& instanceId) {
  const Object* instance = document_.find(instanceId);
  if (!instance || instance->kind() != ObjectKind::Instance) {
    return Result<std::vector<ObjectId>>::failure("Not a component instance");
  }
  const auto& ref = std::get<ComponentInstance>(instance->recipe.base);
  const ComponentDefinition* definition = document_.findDefinition(ref.definitionId);
  if (!definition) return Result<std::vector<ObjectId>>::failure("Definition not found");

  ChangeSet changes(document_);
  std::map<ObjectId, ObjectId> mapping;
  std::vector<ObjectId> roots;
  for (const Object& source : definition->objects) {
    Object o = source;
    o.id = generateObjectId();
    mapping[source.id] = o.id;
    if (source.parent.empty()) {
      o.parent = instance->parent;
      o.transform = instance->transform * source.transform;
      roots.push_back(o.id);
    } else {
      o.parent = mapping[source.parent];
    }
    changes.add(o);
  }
  changes.erase(instanceId);
  history_.execute(changes.build("Explode"));
  return roots;
}

std::vector<Parameter> Modeler::definitionParameters(const std::string& definitionId,
                                                     const ObjectId& objectId) const {
  const ComponentDefinition* d = document_.findDefinition(definitionId);
  if (!d) return {};
  for (const Object& o : d->objects) {
    if (o.id == objectId) return doc::parameters(o.recipe);
  }
  return {};
}

Status Modeler::setDefinitionParameter(const std::string& definitionId, const ObjectId& objectId,
                                       const std::string& key, double value) {
  const ComponentDefinition* current = document_.findDefinition(definitionId);
  if (!current) return Status::failure("Definition not found");
  ComponentDefinition definition = *current;
  bool found = false;
  for (Object& o : definition.objects) {
    if (o.id != objectId) continue;
    found = true;
    const Status changed = doc::setParameter(o.recipe, key, value);
    if (!changed) return changed;
    const Status built = rebuild(o, document_.regenContext());
    if (!built) return built;
  }
  if (!found) return Status::failure("Object not found in definition");
  auto shape = buildDefinitionShape(definition);
  if (!shape) return shape.status();
  definition.shape = shape.value();

  ChangeSet changes(document_);
  changes.putDefinition(definition);
  for (const Object& o : document_.objects()) {
    const auto* c = std::get_if<ComponentInstance>(&o.recipe.base);
    if (!c || c->definitionId != definitionId) continue;
    Object updated = o;
    updated.shape = definition.shape;
    changes.modify(updated);
  }
  history_.execute(changes.build("Edit Component"));
  return Status::success();
}

Status Modeler::renameDefinition(const std::string& definitionId, const std::string& name) {
  const ComponentDefinition* current = document_.findDefinition(definitionId);
  if (!current) return Status::failure("Definition not found");
  if (name.empty()) return Status::failure("Name must not be empty");
  ComponentDefinition definition = *current;
  definition.name = name;
  ChangeSet changes(document_);
  changes.putDefinition(definition);
  history_.execute(changes.build("Rename Component"));
  return Status::success();
}

Result<ObjectId> Modeler::boolean(BooleanKind kind, const std::vector<ObjectId>& ids) {
  std::vector<ObjectId> unique;
  for (const ObjectId& id : ids) {
    if (std::find(unique.begin(), unique.end(), id) == unique.end()) unique.push_back(id);
  }
  if (unique.size() < 2) return Result<ObjectId>::failure("Select at least two solids");

  Object result;
  result.id = generateObjectId();
  BooleanFeature feature;
  feature.kind = kind;
  for (const ObjectId& id : unique) {
    const Object* o = document_.find(id);
    if (!o) return Result<ObjectId>::failure("Object not found");
    if (o->kind() != ObjectKind::Solid) {
      return Result<ObjectId>::failure("'" + o->name + "' is not a solid");
    }
    feature.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(o->recipe),
                                              document_.worldTransform(*o), o->name});
  }
  const Object& first = *document_.find(unique.front());
  result.color = first.color;
  result.layer = first.layer;
  result.name = uniqueName(toString(kind));
  result.recipe.base = std::move(feature);
  const Status built = rebuild(result, document_.regenContext());
  if (!built) return Result<ObjectId>::failure(built.message());

  ChangeSet changes(document_);
  for (const ObjectId& id : unique) changes.erase(id);
  changes.add(result);
  history_.execute(changes.build(std::string("Solid ") + toString(kind)));
  return result.id;
}

// --- files ----------------------------------------------------------------------

void Modeler::newDocument() {
  history_.clear();
  document_.clear();
}

Status Modeler::save(const std::string& path) {
  const Status saved = saveDocument(document_, path);
  if (saved) history_.markClean();
  return saved;
}

Status Modeler::load(const std::string& path, std::vector<std::string>* warnings) {
  Document loaded;
  const Status status = loadDocument(loaded, path, warnings);
  if (!status) return status;
  history_.clear();
  document_.clear();
  document_.settings() = loaded.settings();
  for (const ComponentDefinition& d : loaded.definitions()) document_.putDefinition(d);
  for (const Object& o : loaded.objects()) document_.insert(o);
  return Status::success();
}

Status Modeler::exportStep(const std::string& path, const std::vector<ObjectId>& ids) const {
  std::vector<ObjectId> wanted;
  if (ids.empty()) {
    for (const Object& o : document_.objects()) wanted.push_back(o.id);
  } else {
    for (const ObjectId& root : topmost(ids)) {
      for (const ObjectId& id : document_.subtree(root)) wanted.push_back(id);
    }
  }
  std::vector<geom::NamedShape> parts;
  for (const ObjectId& id : wanted) {
    const Object* o = document_.find(id);
    if (!o || o->shape.IsNull() || !document_.isVisible(id)) continue;
    parts.push_back(geom::NamedShape{document_.worldShape(*o), o->name,
                                     std::array<double, 3>{o->color.r, o->color.g, o->color.b}});
  }
  if (parts.empty()) return Status::failure("Nothing to export");
  return geom::writeStepParts(parts, path);
}

Result<std::vector<ObjectId>> Modeler::importStep(const std::string& path) {
  auto parts = geom::readStepParts(path);
  if (!parts) return Result<std::vector<ObjectId>>::failure(parts.error());

  ChangeSet changes(document_);
  std::vector<ObjectId> ids;
  std::set<std::string> usedNames;
  for (const geom::NamedShape& part : parts.value()) {
    Object o;
    o.id = generateObjectId();
    std::string name = part.name.empty() ? uniqueName("Imported") : part.name;
    // Keep names unique within the batch as well.
    for (int n = 2; usedNames.count(name); ++n) name = part.name + " (" + std::to_string(n) + ")";
    usedNames.insert(name);
    o.name = name;
    if (part.color) o.color = Color{(*part.color)[0], (*part.color)[1], (*part.color)[2]};
    o.recipe.base = ImportedShape{part.shape, path};
    const Status built = rebuild(o);
    if (!built) return Result<std::vector<ObjectId>>::failure(built.message());
    ids.push_back(o.id);
    changes.add(o);
  }
  history_.execute(changes.build("Import STEP"));
  return ids;
}

// --- queries ----------------------------------------------------------------------

std::vector<ObjectSummary> Modeler::listObjects() const {
  std::vector<ObjectSummary> out;
  for (const Object& o : document_.objects()) {
    out.push_back({o.id, o.parent, o.name, o.kind(), describe(o.recipe),
                   o.kind() == ObjectKind::Solid ? geom::volume(o.shape) : 0.0});
  }
  return out;
}

std::vector<Parameter> Modeler::parameters(const ObjectId& id) const {
  const Object* o = document_.find(id);
  return o ? doc::parameters(o->recipe) : std::vector<Parameter>{};
}

// --- freeform ------------------------------------------------------------------

namespace {
// Outer wire of a face object or the wire of a curve object, in world space.
std::optional<TopoDS_Wire> sectionWire(const Document& document, const ObjectId& id) {
  const TopoDS_Shape shape = document.worldShape(id);
  if (shape.IsNull()) return std::nullopt;
  if (shape.ShapeType() == TopAbs_FACE) return BRepTools::OuterWire(TopoDS::Face(shape));
  if (shape.ShapeType() == TopAbs_WIRE) return TopoDS::Wire(shape);
  TopExp_Explorer it(shape, TopAbs_WIRE);
  if (it.More()) return TopoDS::Wire(it.Current());
  return std::nullopt;
}
}  // namespace

Result<ObjectId> Modeler::createCurve(const std::vector<gp_Pnt>& points, bool closed,
                                      const std::string& name) {
  auto wire = geom::makeInterpolatedCurve(points, closed);
  if (!wire) return Result<ObjectId>::failure(wire.error());
  return importShape(wire.value(), name.empty() ? uniqueName("Curve") : name, "NURBS curve");
}

Result<ObjectId> Modeler::loftObjects(const std::vector<ObjectId>& sections, bool solid) {
  if (sections.size() < 2) return Result<ObjectId>::failure("Loft needs at least 2 sections");
  // Sections stay in the document; the loft keeps editable snapshots.
  LoftFeature loft;
  loft.solid = solid;
  for (const ObjectId& id : sections) {
    const Object* o = document_.find(id);
    if (!o || (o->kind() != ObjectKind::Face && o->kind() != ObjectKind::Curve)) {
      return Result<ObjectId>::failure("Loft sections must be curves or faces");
    }
    loft.sections.push_back(BooleanOperand{std::make_shared<const Recipe>(o->recipe),
                                           document_.worldTransform(*o), o->name});
  }
  Object object;
  object.name = uniqueName("Loft");
  object.recipe.base = std::move(loft);
  return addObject(std::move(object), "Loft");
}

Result<ObjectId> Modeler::revolveObject(const ObjectId& profile, const gp_Ax1& axis,
                                        double angle) {
  const Object* source = document_.find(profile);
  if (!source) return Result<ObjectId>::failure("Profile not found");
  if (source->kind() == ObjectKind::Face) {
    // Parametric: the revolve becomes a step of the profile's recipe, so the
    // angle stays editable ("op<N>.angle", degrees).
    Object object = *source;
    const gp_Trsf toLocal = document_.worldTransform(object).Inverted();
    object.recipe.modifiers.push_back(RevolveOp{axis.Transformed(toLocal), angle});
    const Status committed = commitModified(object, "Revolve");
    if (!committed) return Result<ObjectId>::failure(committed.message());
    return profile;
  }
  const TopoDS_Shape shape = document_.worldShape(profile);
  if (shape.IsNull()) return Result<ObjectId>::failure("Profile not found");
  auto result = geom::revolve(shape, axis, angle);
  if (!result) return Result<ObjectId>::failure(result.error());
  return importShape(result.value(), uniqueName("Revolve"), "Revolve");
}

Result<ObjectId> Modeler::sweepObject(const ObjectId& profile, const ObjectId& spine) {
  const TopoDS_Shape shape = document_.worldShape(profile);
  const auto path = sectionWire(document_, spine);
  if (shape.IsNull() || !path) return Result<ObjectId>::failure("Profile or path not found");
  auto result = geom::sweep(shape, *path);
  if (!result) return Result<ObjectId>::failure(result.error());
  return importShape(result.value(), uniqueName("Sweep"), "Sweep");
}

}  // namespace sbr::doc
