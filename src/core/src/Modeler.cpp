#include "sbr/doc/Modeler.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include <BRepClass_FaceClassifier.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <functional>
#include <BRepAdaptor_Surface.hxx>
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
  // New geometry is created in world coordinates; while a group is being
  // edited it goes into that group with an equivalent placement.
  if (object.parent.empty() && !activeGroup_.empty()) {
    if (const Object* group = document_.find(activeGroup_)) {
      object.parent = activeGroup_;
      object.transform = document_.worldTransform(*group).Inverted() * object.transform;
    }
  }
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

Result<ObjectId> Modeler::createCircle(const geom::SketchPlane& plane, double radius,
                                       const std::string& name) {
  Object o;
  o.name = name.empty() ? uniqueName("Circle") : name;
  o.recipe.base = CircleProfile{plane, radius};
  return addObject(std::move(o), "Circle");
}

Result<ObjectId> Modeler::createArc(const gp_Pnt& start, const gp_Pnt& end, const gp_Dir& normal,
                                    double bulge, bool closed, const std::string& name) {
  Object o;
  o.name = name.empty() ? uniqueName("Arc") : name;
  o.recipe.base = ArcCurve{start, end, normal, bulge, closed};
  return addObject(std::move(o), "Arc");
}

Result<ObjectId> Modeler::createDimension(const gp_Pnt& start, const gp_Pnt& end, const gp_Vec& offset,
                                          const std::string& text) {
  Object o;
  o.name = uniqueName("Dimension");
  o.layer = "Annotations";
  o.recipe.base = LinearDimension{start, end, offset, text};
  return addObject(std::move(o), "Dimension");
}

Result<ObjectId> Modeler::createText(const gp_Pnt& position, const std::optional<gp_Pnt>& target,
                                     const std::string& text) {
  if (text.empty()) return Result<ObjectId>::failure("Text must not be empty");
  Object o;
  o.name = uniqueName("Text");
  o.layer = "Annotations";
  o.recipe.base = TextNote{position, target, text};
  return addObject(std::move(o), "Text");
}

Status Modeler::setAnnotationText(const ObjectId& id, const std::string& text) {
  const Object* current = document_.find(id);
  if (!current) return Status::failure("Object not found");
  Object object = *current;
  if (auto* d = std::get_if<LinearDimension>(&object.recipe.base)) {
    d->text = text;  // empty = back to the measured length
  } else if (auto* t = std::get_if<TextNote>(&object.recipe.base)) {
    if (text.empty()) return Status::failure("Text must not be empty");
    t->text = text;
  } else {
    return Status::failure("Not an annotation");
  }
  return commitModified(std::move(object), "Edit Text");
}

namespace {

struct SplitCandidate {
  const Object* object = nullptr;
  TopoDS_Face face;  // world coordinates
  bool profile = false;
};

bool lineCrossesFace(const TopoDS_Face& face, const std::vector<gp_Pnt>& points) {
  if (!geom::isPlanarFace(face)) return false;
  for (const gp_Pnt& p : points) {
    if (!geom::pointOnFace(face, p)) return false;
  }
  return geom::pointOnFaceBoundary(face, points.front()) && geom::pointOnFaceBoundary(face, points.back());
}

// Faces a line could split, in the order to try: loose profiles first (they
// lie on top of solid faces), then faces of solids.
std::vector<SplitCandidate> splitCandidates(const Document& document, const ObjectId& context,
                                            const std::vector<gp_Pnt>& points) {
  std::vector<SplitCandidate> out;
  if (points.size() < 2) return out;
  for (int pass = 0; pass < 2; ++pass) {
    for (const Object& o : document.objects()) {
      if (o.parent != context || !document.isVisible(o.id)) continue;
      const bool profile = o.kind() == ObjectKind::Face;
      if ((pass == 0) != profile || (!profile && o.kind() != ObjectKind::Solid)) continue;
      const TopoDS_Shape shape = document.worldShape(o);
      for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
        const TopoDS_Face face = TopoDS::Face(e.Current());
        if (lineCrossesFace(face, points)) out.push_back({&o, face, profile});
      }
    }
  }
  return out;
}

}  // namespace

std::optional<ObjectId> Modeler::splitTarget(const std::vector<gp_Pnt>& worldPoints) const {
  // Cheap test for hover hints: the line runs edge to edge across a face.
  const auto candidates = splitCandidates(document_, activeGroup_, worldPoints);
  if (candidates.empty()) return std::nullopt;
  return candidates.front().object->id;
}

Result<ObjectId> Modeler::splitWithLine(const std::vector<gp_Pnt>& worldPoints) {
  if (worldPoints.size() < 2) return Result<ObjectId>::failure("Split needs a line");
  std::string lastError = "The line does not cross a face from edge to edge";
  for (const SplitCandidate& c : splitCandidates(document_, activeGroup_, worldPoints)) {
    // A copy: adding objects below may move the document's storage.
    const Object candidate = *c.object;
    if (c.profile) {
      // Work in the parent's frame, where the profile's placement lives.
      const gp_Trsf toParent = document_.parentTransform(candidate).Inverted();
      std::vector<gp_Pnt> line;
      for (const gp_Pnt& p : worldPoints) line.push_back(p.Transformed(toParent));
      const TopoDS_Face placed = TopoDS::Face(c.face.Moved(TopLoc_Location(toParent)));
      auto parts = geom::splitPlanarFace(placed, line);
      if (!parts) {
        lastError = parts.error();
        continue;
      }
      history_.beginGroup("Split Face");
      std::vector<ObjectId> created;
      bool ok = true;
      for (const TopoDS_Face& part : parts.value()) {
        const auto inside = geom::SurfaceRef::fromFace(part);
        if (!inside) {
          ok = false;
          break;
        }
        Object o;
        o.name = uniqueName(candidate.name);
        o.parent = candidate.parent;
        o.layer = candidate.layer;
        o.color = candidate.color;
        o.recipe.base = FacePart{{BooleanOperand{std::make_shared<const Recipe>(candidate.recipe),
                                                 candidate.transform, candidate.name}},
                                 line, inside->point};
        auto id = addObject(std::move(o), "Split Face");
        if (!id) {
          lastError = id.error();
          ok = false;
          break;
        }
        created.push_back(*id);
      }
      if (ok && remove({candidate.id})) {
        history_.endGroup();
        return created.front();
      }
      history_.abortGroup();
      continue;
    }
    const gp_Trsf toLocal = document_.worldTransform(candidate).Inverted();
    const TopoDS_Face localFace = TopoDS::Face(c.face.Moved(TopLoc_Location(toLocal)));
    const auto ref = geom::FaceRef::fromFace(localFace);
    if (!ref) continue;
    std::vector<gp_Pnt> local;
    for (const gp_Pnt& p : worldPoints) local.push_back(p.Transformed(toLocal));
    Object object = candidate;
    object.recipe.modifiers.push_back(SplitOp{*ref, local});
    const Status s = commitModified(std::move(object), "Split Face");
    if (s) return candidate.id;
    lastError = s.message();
  }
  return Result<ObjectId>::failure(lastError);
}

namespace {

// End points of an open curve object (world), if it is one.
std::optional<std::pair<gp_Pnt, gp_Pnt>> curveEnds(const Document& document, const Object& o) {
  if (o.kind() != ObjectKind::Curve || !document.isVisible(o.id)) return std::nullopt;
  const TopoDS_Shape shape = document.worldShape(o);
  TopExp_Explorer w(shape, TopAbs_WIRE);
  if (!w.More()) return std::nullopt;
  const TopoDS_Wire wire = TopoDS::Wire(w.Current());
  TopoDS_Vertex a, b;
  TopExp::Vertices(wire, a, b);
  if (a.IsNull() || b.IsNull()) return std::nullopt;
  const gp_Pnt pa = BRep_Tool::Pnt(a), pb = BRep_Tool::Pnt(b);
  if (pa.Distance(pb) < 1e-3) return std::nullopt;  // already closed
  return std::make_pair(pa, pb);
}

}  // namespace

bool Modeler::isLooseCurveEnd(const gp_Pnt& p) const {
  for (const Object& o : document_.objects()) {
    if (const auto ends = curveEnds(document_, o)) {
      if (ends->first.Distance(p) < 1e-3 || ends->second.Distance(p) < 1e-3) return true;
    }
  }
  return false;
}

Result<ObjectId> Modeler::faceFromLoop(const ObjectId& curveId, bool* loopFound) {
  if (loopFound) *loopFound = false;
  const Object* start = document_.find(curveId);
  if (!start) return Result<ObjectId>::failure("Object not found");
  const auto startEnds = curveEnds(document_, *start);
  if (!startEnds) return Result<ObjectId>::failure("Not an open curve");
  struct Candidate {
    const Object* object;
    gp_Pnt a, b;
  };
  std::vector<Candidate> candidates;
  for (const Object& o : document_.objects()) {
    if (o.id == curveId || o.parent != start->parent) continue;
    if (const auto ends = curveEnds(document_, o)) candidates.push_back({&o, ends->first, ends->second});
  }
  // Depth-first search for a chain from the curve's end back to its start.
  std::vector<int> path;
  std::vector<bool> used(candidates.size(), false);
  std::function<bool(const gp_Pnt&)> search = [&](const gp_Pnt& at) -> bool {
    if (!path.empty() && at.Distance(startEnds->first) < 1e-3) return true;
    if (path.size() > 32) return false;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      if (used[i]) continue;
      const bool fromA = candidates[i].a.Distance(at) < 1e-3;
      const bool fromB = candidates[i].b.Distance(at) < 1e-3;
      if (!fromA && !fromB) continue;
      used[i] = true;
      path.push_back(static_cast<int>(i));
      if (search(fromA ? candidates[i].b : candidates[i].a)) return true;
      path.pop_back();
      used[i] = false;
    }
    return false;
  };
  if (!search(startEnds->second)) return Result<ObjectId>::failure("The curve does not close a loop");
  if (loopFound) *loopFound = true;

  std::vector<const Object*> members = {start};
  for (int i : path) members.push_back(candidates[static_cast<std::size_t>(i)].object);
  LoopProfile loop;
  std::vector<ObjectId> ids;
  for (const Object* o : members) {
    loop.curves.push_back(BooleanOperand{std::make_shared<const Recipe>(o->recipe), o->transform, o->name});
    ids.push_back(o->id);
  }
  Object face;
  face.name = uniqueName("Face");
  face.parent = start->parent;
  face.recipe.base = std::move(loop);
  history_.beginGroup("Face");
  auto id = addObject(std::move(face), "Face");
  if (!id) {
    history_.abortGroup();
    return id;
  }
  if (const Status removed = remove(ids); !removed) {
    history_.abortGroup();
    return Result<ObjectId>::failure(removed.message());
  }
  history_.endGroup();
  return id;
}

void Modeler::setTagVisible(const std::string& tag, bool visible) {
  auto& hidden = document_.settings().hiddenTags;
  if (visible == !hidden.count(tag)) return;
  if (visible) {
    hidden.erase(tag);
  } else {
    hidden.insert(tag);
  }
  history_.markDirty();
  document_.notify();
}

void Modeler::setSectionVisible(bool visible) {
  auto& sections = document_.settings().sections;
  if (sections.empty() || visible == sectionVisible()) return;
  for (SectionPlane& p : sections) p.active = false;
  if (visible) sections.back().active = true;
  history_.markDirty();
  document_.notify();
}

bool Modeler::sectionVisible() const {
  for (const SectionPlane& p : document_.settings().sections) {
    if (p.active) return true;
  }
  return false;
}

void Modeler::clearSections() {
  if (document_.settings().sections.empty()) return;
  document_.settings().sections.clear();
  history_.markDirty();
  document_.notify();
}

std::vector<std::string> Modeler::tags() const {
  std::set<std::string> all;
  for (const Object& o : document_.objects()) all.insert(o.layer);
  for (const std::string& t : document_.settings().hiddenTags) all.insert(t);
  return {all.begin(), all.end()};
}

void Modeler::setSection(const std::optional<SectionPlane>& plane) {
  auto& sections = document_.settings().sections;
  for (SectionPlane& p : sections) p.active = false;
  if (plane) {
    SectionPlane p = *plane;
    if (p.name.empty()) p.name = "Section " + std::to_string(sections.size() + 1);
    p.active = true;
    sections.push_back(p);
  }
  history_.markDirty();
  document_.notify();
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
  if (!pickedNormal) {
    if (object.kind() != ObjectKind::Solid) {
      return Result<PushPullResult>::failure("Only planar faces of profiles can be pushed/pulled");
    }
    std::string curvedKey;
    // An extruded circle's side: change the radius (stays a clean parameter).
    if (auto* circle = std::get_if<CircleProfile>(&object.recipe.base);
        circle && object.recipe.modifiers.size() == 1 &&
        std::holds_alternative<ExtrudeOp>(object.recipe.modifiers.front()) &&
        BRepAdaptor_Surface(face).GetType() == GeomAbs_Cylinder) {
      circle->radius += distance;
      curvedKey = "radius";
    } else {
      // Any other curved face: parametric offset of that face.
      const auto ref = geom::SurfaceRef::fromFace(face);
      if (!ref) return Result<PushPullResult>::failure(ref.error());
      object.recipe.modifiers.push_back(OffsetFaceOp{ref.value(), distance});
      curvedKey = opKey(object.recipe.modifiers.size() - 1);
    }
    const Status committed = commitModified(object, "Push/Pull");
    if (!committed) return Result<PushPullResult>::failure(committed.message());
    return PushPullResult{id, curvedKey};
  }

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
    // A profile drawn on a face of another solid cuts into it or grows it,
    // as in SketchUp (parametric Boolean, one undo step).
    if (auto merged = mergeIntoHost(*current, object, key)) return *merged;
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

std::optional<Result<PushPullResult>> Modeler::mergeIntoHost(const Object& profileBefore,
                                                               Object extruded,
                                                               const std::string& key) {
  const TopoDS_Shape profile = document_.worldShape(profileBefore);
  if (profile.IsNull() || profile.ShapeType() != TopAbs_FACE) return std::nullopt;
  const auto profileNormal = geom::planarFaceNormal(TopoDS::Face(profile));
  const auto* extrude = std::get_if<ExtrudeOp>(&extruded.recipe.modifiers.back());
  if (!profileNormal || !extrude) return std::nullopt;
  const gp_Dir direction = extrude->distance >= 0 ? *profileNormal : profileNormal->Reversed();

  std::vector<gp_Pnt> corners;
  for (TopExp_Explorer it(profile, TopAbs_VERTEX); it.More(); it.Next()) {
    corners.push_back(BRep_Tool::Pnt(TopoDS::Vertex(it.Current())));
  }
  gp_Pnt centroid;
  if (!faceCentroid(profile, centroid)) return std::nullopt;
  corners.push_back(centroid);

  for (const Object& host : document_.objects()) {
    if (host.id == profileBefore.id || host.parent != profileBefore.parent ||
        host.kind() != ObjectKind::Solid || !document_.isVisible(host.id)) {
      continue;
    }
    const TopoDS_Shape hostShape = document_.worldShape(host);
    for (TopExp_Explorer it(hostShape, TopAbs_FACE); it.More(); it.Next()) {
      const TopoDS_Face face = TopoDS::Face(it.Current());
      gp_Pln plane;
      const auto n = geom::planarFaceNormal(face);
      if (!n || !geom::isPlanarFace(face, &plane) || !n->IsParallel(*profileNormal, 1e-6)) continue;
      if (plane.Distance(centroid) > 1e-3) continue;
      const bool inside = std::all_of(corners.begin(), corners.end(), [&](const gp_Pnt& p) {
        BRepClass_FaceClassifier classifier(face, p, 1e-3);
        return classifier.State() == TopAbs_IN || classifier.State() == TopAbs_ON;
      });
      if (!inside) continue;

      const bool inward = direction.Dot(*n) < 0;
      Object result;
      result.id = generateObjectId();
      result.parent = host.parent;
      result.name = host.name;
      result.color = host.color;
      result.layer = host.layer;
      BooleanFeature feature;
      feature.kind = inward ? BooleanKind::Subtract : BooleanKind::Union;
      // Operands are placed in the common parent's frame.
      const gp_Trsf toParent = document_.parentTransform(host).Inverted();
      feature.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(host.recipe),
                                                toParent * document_.worldTransform(host),
                                                host.name});
      feature.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(extruded.recipe),
                                                toParent * document_.worldTransform(profileBefore),
                                                profileBefore.name});
      result.recipe.base = std::move(feature);
      if (!rebuild(result, document_.regenContext())) return std::nullopt;  // plain extrusion

      ChangeSet changes(document_);
      changes.erase(host.id);
      changes.erase(profileBefore.id);
      changes.add(result);
      history_.execute(changes.build(inward ? "Push/Pull (cut)" : "Push/Pull (join)"));
      return Result<PushPullResult>(PushPullResult{result.id, "in1." + key});
    }
  }
  return std::nullopt;
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

Result<ObjectId> Modeler::offsetSolidFace(const ObjectId& id, const TopoDS_Face& worldFace,
                                          double distance) {
  const Object* owner = document_.find(id);
  if (!owner) return Result<ObjectId>::failure("Object not found");
  if (owner->kind() != ObjectKind::Solid) return Result<ObjectId>::failure("Not a solid");
  auto face = geom::offsetFace(worldFace, distance);
  if (!face) return Result<ObjectId>::failure(face.error());
  // The new outline lives in the owner's group, expressed in that group's
  // frame (committed directly: it must not be re-homed into another group).
  Object o;
  o.id = generateObjectId();
  o.name = uniqueName("Offset");
  o.parent = owner->parent;
  const gp_Trsf toParent = document_.parentTransform(*owner).Inverted();
  o.recipe.base = ImportedShape{face->Moved(TopLoc_Location(toParent)), "Offset of " + owner->name};
  const Status built = rebuild(o, document_.regenContext());
  if (!built) return Result<ObjectId>::failure(built.message());
  history_.execute(Command{"Offset", {ObjectDelta{o.id, std::nullopt, o, std::nullopt}}, {}});
  return o.id;
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

Status Modeler::paint(const std::vector<ObjectId>& ids, const Color& color) {
  ChangeSet changes(document_);
  bool any = false;
  for (const ObjectId& root : topmost(ids)) {
    for (const ObjectId& id : document_.subtree(root)) {
      Object o = *document_.find(id);
      if (o.kind() == ObjectKind::Group || o.color == color) continue;
      o.color = color;
      changes.modify(o);
      any = true;
    }
  }
  if (!any) return Status::success();
  history_.execute(changes.build("Paint"));
  return Status::success();
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
  // The result stays in the operands' group when they share one.
  ObjectId parent;
  if (!checkSameParent(unique, parent)) parent.clear();
  const gp_Trsf toParent =
      parent.empty() ? gp_Trsf() : document_.worldTransform(*document_.find(parent)).Inverted();
  result.parent = parent;
  for (const ObjectId& id : unique) {
    const Object* o = document_.find(id);
    if (!o) return Result<ObjectId>::failure("Object not found");
    if (o->kind() != ObjectKind::Solid) {
      return Result<ObjectId>::failure("'" + o->name + "' is not a solid");
    }
    feature.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(o->recipe),
                                              toParent * document_.worldTransform(*o), o->name});
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
  activeGroup_.clear();
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
  activeGroup_.clear();
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
    // Annotations are drawing aids, not model geometry.
    if (!o || o->shape.IsNull() || !document_.isVisible(id) || o->kind() == ObjectKind::Annotation) continue;
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


Result<ObjectId> Modeler::createCurve(const std::vector<gp_Pnt>& points, bool closed,
                                      const std::string& name) {
  Object o;
  o.name = name.empty() ? uniqueName("Curve") : name;
  o.recipe.base = InterpolatedCurve{points, closed};
  return addObject(std::move(o), "Curve");
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
  const Object* p = document_.find(profile);
  const Object* path = document_.find(spine);
  if (!p || !path) return Result<ObjectId>::failure("Profile or path not found");
  if (path->kind() != ObjectKind::Curve) return Result<ObjectId>::failure("Sweep path must be a curve");
  SweepFeature feature;
  for (const Object* o : {p, path}) {
    feature.inputs.push_back(BooleanOperand{std::make_shared<const Recipe>(o->recipe),
                                            document_.worldTransform(*o), o->name});
  }
  Object object;
  object.name = uniqueName("Sweep");
  object.recipe.base = std::move(feature);
  return addObject(std::move(object), "Sweep");
}

Status Modeler::setCurvePoints(const ObjectId& id, const std::vector<gp_Pnt>& points) {
  const Object* current = document_.find(id);
  if (!current) return Status::failure("Object not found");
  if (!std::holds_alternative<InterpolatedCurve>(current->recipe.base)) {
    return Status::failure("Not a NURBS curve");
  }
  Object object = *current;
  std::get<InterpolatedCurve>(object.recipe.base).points = points;
  return commitModified(std::move(object), "Edit Curve");
}

Status Modeler::setActiveGroup(const ObjectId& groupId) {
  if (groupId.empty()) {
    activeGroup_.clear();
    return Status::success();
  }
  const Object* group = document_.find(groupId);
  if (!group || group->kind() != ObjectKind::Group) return Status::failure("Not a group");
  activeGroup_ = groupId;
  return Status::success();
}

}  // namespace sbr::doc
