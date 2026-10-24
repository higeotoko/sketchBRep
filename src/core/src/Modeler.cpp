#include "sbr/doc/Modeler.h"

#include <algorithm>
#include <deque>
#include <cmath>
#include <limits>
#include <map>
#include <set>

#include <BRepBndLib.hxx>
#include <memory>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Compound.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <TopExp.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt2d.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <functional>
#include <sstream>
#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <gp_Ax2.hxx>
#include <BRepTools.hxx>
#include <TopoDS.hxx>

#include "sbr/doc/NativeFormat.h"
#include "sbr/doc/ChangeSummary.h"
#include "sbr/geom/Freeform.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Arrangement.h"
#include "sbr/geom/Operations.h"
#include "sbr/geom/Rhino3dm.h"
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

  void putMaterial(const Material& material) {
    const Material* before = document_.findMaterial(material.id);
    materials_.push_back(MaterialDelta{material.id, before ? std::optional<Material>(*before) : std::nullopt, material});
  }
  void removeMaterial(const std::string& id) {
    const Material* before = document_.findMaterial(id);
    if (before) materials_.push_back(MaterialDelta{id, *before, std::nullopt});
  }
  // The material made from a plain colour, added to the library if missing.
  std::string colorMaterialFor(const Color& color) {
    const Material m = colorMaterial(color);
    bool known = document_.findMaterial(m.id) != nullptr;
    for (const MaterialDelta& d : materials_) known |= d.id == m.id && d.after.has_value();
    if (!known) putMaterial(m);
    return m.id;
  }

  Command build(const std::string& name) {
    // Undo applies deltas in reverse: removals must come back in ascending
    // index order, so store them descending.
    std::sort(removed_.begin(), removed_.end(), [](const ObjectDelta& a, const ObjectDelta& b) {
      return a.indexBefore.value_or(0) > b.indexBefore.value_or(0);
    });
    Command command{name, {}, definitions_, materials_};
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
  std::vector<MaterialDelta> materials_;
};

// Materials through Booleans (docs/11_materials.md): the result takes the
// first input's material; a face of the result lying on a face of an input
// (same plane, same facing) keeps what that face showed when it differs
// from the result's material. Faces made by cutting face the other way and
// inherit the result's. `eitherSide`: faces drawn one by one face either
// way; what they showed goes to the solid's face on them whichever way.
void carryMaterials(Object& result, const std::vector<std::pair<const Object*, gp_Trsf>>& inputs,
                    bool eitherSide = false) {
  if (inputs.empty() || result.shape.IsNull()) return;
  result.material = inputs.front().first->material;
  if (eitherSide) {
    // Faces drawn one by one: the whole takes a material only when they all
    // had it; otherwise each face keeps its own.
    for (const auto& [o, placement] : inputs) {
      if (o->material != result.material) result.material.clear();
    }
  }
  struct Source {
    TopoDS_Face face;
    gp_Dir normal;
    gp_Pnt point;  // on its plane
    std::string material;
  };
  std::vector<Source> sources;
  for (const auto& [o, placement] : inputs) {
    if (o->shape.IsNull()) continue;
    std::map<int, std::string> own;
    for (const auto& [i, m] : resolveFaceMaterials(*o).faces) own[i] = m;
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(o->shape, TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i) {
      const auto it = own.find(i);
      const std::string material = it != own.end() ? it->second : o->material;
      if (material.empty() || material == result.material) continue;
      const TopoDS_Face placed = TopoDS::Face(faces(i).Moved(TopLoc_Location(placement)));
      const auto ref = geom::FaceRef::fromFace(placed);
      if (!ref) continue;
      sources.push_back({placed, ref->normal, ref->centroid, material});
    }
  }
  if (sources.empty()) return;
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(result.shape, TopAbs_FACE, faces);
  for (int i = 1; i <= faces.Extent(); ++i) {
    const TopoDS_Face face = TopoDS::Face(faces(i));
    auto mine = FaceMaterial::of(face, std::string());
    if (!mine || !mine->plane || !mine->plane->inside) continue;
    const gp_Pnt p = *mine->plane->inside;
    for (const Source& src : sources) {
      if (eitherSide ? !src.normal.IsParallel(mine->plane->normal, 1e-6) : src.normal.Angle(mine->plane->normal) > 1e-6) {
        continue;
      }
      if (std::abs(gp_Vec(src.point, p).Dot(gp_Vec(src.normal))) > 1e-3) continue;
      BRepClass_FaceClassifier classifier(src.face, p, 1e-6);
      if (classifier.State() != TopAbs_IN) continue;
      mine->material = src.material;
      result.faceMaterials.push_back(std::move(*mine));
      break;
    }
  }
}

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
  return addProfile(std::move(o), "Rectangle");
}

Result<ObjectId> Modeler::createCircle(const geom::SketchPlane& plane, double radius,
                                       const std::string& name) {
  Object o;
  o.name = name.empty() ? uniqueName("Circle") : name;
  o.recipe.base = CircleProfile{plane, radius};
  return addProfile(std::move(o), "Circle");
}

Result<ObjectId> Modeler::createRegularPolygon(const geom::SketchPlane& plane, double radius, int sides,
                                               const std::string& name) {
  Object o;
  o.name = name.empty() ? uniqueName("Polygon") : name;
  o.recipe.base = RegularPolygonProfile{plane, radius, sides};
  return addProfile(std::move(o), "Polygon");
}

Result<ObjectId> Modeler::createArc(const gp_Pnt& start, const gp_Pnt& end, const gp_Dir& normal,
                                    double bulge, bool closed, const std::string& name) {
  Object o;
  o.name = name.empty() ? uniqueName("Arc") : name;
  o.recipe.base = ArcCurve{start, end, normal, bulge, closed};
  return closed ? addProfile(std::move(o), "Arc") : addObject(std::move(o), "Arc");
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

Result<ObjectId> Modeler::createGuide(const gp_Pnt& point, const gp_Dir& direction, const std::string& name,
                                      const std::string& layer) {
  Object o;
  o.name = name.empty() ? uniqueName("Guide") : name;
  o.layer = layer;
  o.recipe.base = GuideLine{point, direction};
  return addObject(std::move(o), "Guide");
}

Result<ObjectId> Modeler::addImageUnderlay(const std::string& bytes, const std::string& name, int pixelWidth,
                                          int pixelHeight, const geom::SketchPlane& plane, double width) {
  if (bytes.empty() || pixelWidth <= 0 || pixelHeight <= 0) return Result<ObjectId>::failure("Not an image");
  if (!(width > tolerance::kMinDimension)) return Result<ObjectId>::failure("Width must be positive");
  ImageUnderlay u;
  u.image = document_.addImage(bytes, name);
  u.plane = plane;
  u.width = width;
  u.height = width * pixelHeight / pixelWidth;
  Object o;
  o.name = uniqueName(name.empty() ? "Image" : name);
  o.layer = "Images";
  o.recipe.base = u;
  return addObject(std::move(o), "Import Image");
}

Status Modeler::setLocked(const std::vector<ObjectId>& ids, bool locked) {
  ChangeSet changes(document_);
  bool any = false;
  for (const ObjectId& id : ids) {
    const Object* current = document_.find(id);
    if (!current) return Status::failure("Object not found");
    if (current->locked == locked) continue;
    Object o = *current;
    o.locked = locked;
    changes.modify(o);
    any = true;
  }
  if (any) history_.execute(changes.build(locked ? "Lock" : "Unlock"));
  return Status::success();
}

Status Modeler::clearGuides() {
  std::vector<ObjectId> guides;
  for (const Object& o : document_.objects()) {
    if (o.kind() == ObjectKind::Guide && o.layer == "Guides") guides.push_back(o.id);
  }
  if (guides.empty()) return Status::failure("There are no guides");
  return remove(guides);
}

Result<Modeler::GridLines> Modeler::createGridLines(const gp_Pnt& origin, const std::vector<double>& xSpacings,
                                                    const std::vector<double>& ySpacings, double labelOffset) {
  for (double s : xSpacings) {
    if (!(s > tolerance::kMinDimension)) return Result<GridLines>::failure("Grid spacings must be positive");
  }
  for (double s : ySpacings) {
    if (!(s > tolerance::kMinDimension)) return Result<GridLines>::failure("Grid spacings must be positive");
  }
  if (!(labelOffset > 0)) return Result<GridLines>::failure("Label offset must be positive");
  // Positions of the lines along each axis (a single line when no spacing).
  auto positions = [](double start, const std::vector<double>& spacings) {
    std::vector<double> p{start};
    for (double s : spacings) p.push_back(p.back() + s);
    return p;
  };
  const std::vector<double> xs = positions(origin.X(), xSpacings);
  const std::vector<double> ys = positions(origin.Y(), ySpacings);
  const double z = origin.Z();
  const double dimOffset = labelOffset / 2;

  GridLines grid;
  history_.beginGroup("Grid Lines");
  auto fail = [&](const std::string& message) {
    history_.abortGroup();
    return Result<GridLines>::failure(message);
  };
  auto addText = [&](const gp_Pnt& at, const std::string& text) -> Status {
    Object o;
    o.name = "Grid " + text;
    o.layer = "Grid";
    o.recipe.base = TextNote{at, std::nullopt, text};
    auto id = addObject(std::move(o), "Grid Lines");
    if (!id) return Status::failure(id.error());
    grid.labels.push_back(*id);
    return Status::success();
  };
  for (std::size_t i = 0; i < xs.size(); ++i) {
    const std::string name = "X" + std::to_string(i + 1);
    auto id = createGuide(gp_Pnt(xs[i], ys.front(), z), gp::DY(), name, "Grid");
    if (!id) return fail(id.error());
    grid.lines.push_back(*id);
  }
  for (std::size_t i = 0; i < ys.size(); ++i) {
    const std::string name = "Y" + std::to_string(i + 1);
    auto id = createGuide(gp_Pnt(xs.front(), ys[i], z), gp::DX(), name, "Grid");
    if (!id) return fail(id.error());
    grid.lines.push_back(*id);
  }
  for (std::size_t i = 0; i < xs.size(); ++i) {
    const Status s = addText(gp_Pnt(xs[i], ys.front() - labelOffset, z), "X" + std::to_string(i + 1));
    if (!s) return fail(s.message());
  }
  for (std::size_t i = 0; i < ys.size(); ++i) {
    const Status s = addText(gp_Pnt(xs.front() - labelOffset, ys[i], z), "Y" + std::to_string(i + 1));
    if (!s) return fail(s.message());
  }
  auto addDimension = [&](const gp_Pnt& from, const gp_Pnt& to, const gp_Vec& offset) -> Status {
    Object o;
    o.name = uniqueName("Grid Dimension");
    o.layer = "Grid";
    o.recipe.base = LinearDimension{from, to, offset, {}};
    auto id = addObject(std::move(o), "Grid Lines");
    if (!id) return Status::failure(id.error());
    grid.dimensions.push_back(*id);
    return Status::success();
  };
  for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
    const Status s = addDimension(gp_Pnt(xs[i], ys.front(), z), gp_Pnt(xs[i + 1], ys.front(), z),
                                  gp_Vec(0, -dimOffset, 0));
    if (!s) return fail(s.message());
  }
  for (std::size_t i = 0; i + 1 < ys.size(); ++i) {
    const Status s = addDimension(gp_Pnt(xs.front(), ys[i], z), gp_Pnt(xs.front(), ys[i + 1], z),
                                  gp_Vec(-dimOffset, 0, 0));
    if (!s) return fail(s.message());
  }
  history_.endGroup();
  return grid;
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

Result<ObjectId> Modeler::addProfile(Object object, const std::string& commandName) {
  history_.beginGroup(commandName);
  auto id = addObject(std::move(object), commandName);
  if (!id) {
    history_.abortGroup();
    return id;
  }
  punchHole(*id);
  // Drawn where a face already is: not stacked twice.
  auto settled = settleOverlaps({*id});
  if (!settled) {
    history_.abortGroup();
    return Result<ObjectId>::failure(settled.error());
  }
  history_.endGroup();
  const std::vector<ObjectId> now = settledIds({*id}, *settled);
  return now.empty() ? *id : now.front();
}

void Modeler::punchHole(const ObjectId& profileId) {
  const Object* inner = document_.find(profileId);
  if (!inner || inner->kind() != ObjectKind::Face) return;
  const TopoDS_Shape innerShape = document_.worldShape(*inner);
  if (innerShape.ShapeType() != TopAbs_FACE) return;
  // The smallest coplanar profile around it is the one to cut.
  const Object* host = nullptr;
  double hostArea = 0.0;
  for (const Object& o : document_.objects()) {
    if (o.id == profileId || o.parent != inner->parent || o.kind() != ObjectKind::Face ||
        !document_.isVisible(o.id)) {
      continue;
    }
    const TopoDS_Shape outer = document_.worldShape(o);
    if (outer.ShapeType() != TopAbs_FACE) continue;
    if (!geom::faceInsideFace(TopoDS::Face(innerShape), TopoDS::Face(outer))) continue;
    const double a = geom::area(outer);
    if (!host || a < hostArea) {
      host = &o;
      hostArea = a;
    }
  }
  if (!host) return;
  Object cut = *host;
  // The outline's placement in the host's own coordinates.
  const gp_Trsf placement = host->transform.Inverted() * inner->transform;
  cut.recipe.modifiers.push_back(
      HoleOp{BooleanOperand{std::make_shared<const Recipe>(inner->recipe), placement, inner->name}});
  commitModified(std::move(cut), "Hole");  // on failure the host stays as it was
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
      // As the part regenerates (FacePart): e.g. a line across a hole.
      if (!parts) parts = geom::splitPlanarFaceExtended(placed, line);
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
        o.material = candidate.material;
        o.faceMaterials = candidate.faceMaterials;  // found again on each part
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
    const auto ref = geom::FaceRef::identify(localFace);
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

double distanceToPolyline(const gp_Pnt& p, const std::vector<gp_Pnt>& line) {
  double best = std::numeric_limits<double>::max();
  for (std::size_t k = 1; k < line.size(); ++k) {
    const gp_Vec ab(line[k - 1], line[k]);
    const double len2 = ab.SquareMagnitude();
    double t = len2 > 0 ? gp_Vec(line[k - 1], p).Dot(ab) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    best = std::min(best, p.Distance(line[k - 1].Translated(ab * t)));
  }
  return best;
}

// The edge lies along the polyline (both ends and the middle).
bool edgeOnLine(const TopoDS_Edge& edge, const std::vector<gp_Pnt>& line) {
  BRepAdaptor_Curve c(edge);
  for (double f : {0.0, 0.5, 1.0}) {
    const gp_Pnt p = c.Value(c.FirstParameter() + f * (c.LastParameter() - c.FirstParameter()));
    if (distanceToPolyline(p, line) > 1e-3) return false;
  }
  return true;
}

int faceCount(const TopoDS_Shape& shape) {
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  return faces.Extent();
}

}  // namespace

std::vector<TopoDS_Face> Modeler::facesAtEdge(const ObjectId& id, const TopoDS_Edge& worldEdge) const {
  std::vector<TopoDS_Face> out;
  const Object* o = document_.find(id);
  if (!o || o->shape.IsNull()) return out;
  const TopoDS_Shape world = document_.worldShape(*o);
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  TopExp::MapShapesAndAncestors(world, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
  int index = edgeFaces.FindIndex(worldEdge);
  for (int i = 1; i <= edgeFaces.Extent() && index == 0; ++i) {
    if (edgeFaces.FindKey(i).TShape() == worldEdge.TShape()) index = i;
  }
  if (index == 0) return out;
  for (const TopoDS_Shape& f : edgeFaces(index)) {
    const TopoDS_Face face = TopoDS::Face(f);
    if (std::none_of(out.begin(), out.end(), [&](const TopoDS_Face& g) { return g.IsSame(face); })) out.push_back(face);
  }
  return out;
}

Status Modeler::eraseCornerEdge(const ObjectId& id, const TopoDS_Edge& worldEdge) {
  const Object* o = document_.find(id);
  if (!o || o->shape.IsNull()) return Status::failure("Object not found");
  if (o->locked) return Status::failure("The object is locked");
  if (o->kind() != ObjectKind::Solid && o->kind() != ObjectKind::Shell) {
    return Status::failure("Only edges of solids and open shapes take their faces with them");
  }
  const std::vector<TopoDS_Face> faces = facesAtEdge(id, worldEdge);
  if (faces.empty()) return Status::failure("The edge is not part of this object");
  return removeFaces(id, faces);
}

Status Modeler::removeFaces(const ObjectId& id, const std::vector<TopoDS_Face>& worldFaces) {
  const Object* current = document_.find(id);
  if (!current || current->shape.IsNull()) return Status::failure("Object not found");
  if (current->kind() != ObjectKind::Solid && current->kind() != ObjectKind::Shell) {
    return Status::failure("Only faces of solids and open shells are deleted on their own");
  }
  Object object = *current;
  const gp_Trsf toLocal = document_.worldTransform(object).Inverted();
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(object.shape, TopAbs_FACE, faces);
  for (const TopoDS_Face& world : worldFaces) {
    int index = faces.FindIndex(world.Moved(TopLoc_Location(toLocal)));
    for (int i = 1; i <= faces.Extent() && index == 0; ++i) {
      if (faces(i).TShape() == world.TShape()) index = i;
    }
    if (index == 0) return Status::failure("The face is not part of this object");
    const TopoDS_Face local = TopoDS::Face(faces(index));
    RemoveFaceOp op;
    if (auto plane = geom::FaceRef::identify(local)) {
      op.plane = plane.value();
    } else if (auto surface = geom::SurfaceRef::fromFace(local)) {
      op.surface = surface.value();
    } else {
      return Status::failure("This face cannot be deleted on its own");
    }
    object.recipe.modifiers.push_back(std::move(op));
  }
  return commitModified(std::move(object), "Delete Face");
}

Status Modeler::fillHoles(const std::vector<ObjectId>& ids) {
  ChangeSet changes(document_);
  bool any = false;
  for (const ObjectId& id : ids) {
    const Object* current = document_.find(id);
    if (!current) return Status::failure("Object not found");
    if (current->kind() != ObjectKind::Shell) continue;
    Object object = *current;
    object.recipe.modifiers.push_back(CapHolesOp{});
    const Status built = rebuild(object, document_.regenContext());
    if (!built) return built;
    changes.modify(object);
    any = true;
  }
  if (!any) return Status::failure("Select an open shape (with deleted faces) to fill");
  history_.execute(changes.build("Fill Holes"));
  return Status::success();
}

Status Modeler::reverseFaces(const std::vector<ObjectId>& ids) {
  ChangeSet changes(document_);
  bool any = false;
  for (const ObjectId& id : ids) {
    const Object* current = document_.find(id);
    if (!current) return Status::failure("Object not found");
    if (current->kind() != ObjectKind::Shell && current->kind() != ObjectKind::Face) continue;
    Object object = *current;
    // Reversing twice is no reversal: drop the earlier one instead.
    if (!object.recipe.modifiers.empty() && std::holds_alternative<ReverseOp>(object.recipe.modifiers.back())) {
      object.recipe.modifiers.pop_back();
    } else {
      object.recipe.modifiers.push_back(ReverseOp{});
    }
    const Status built = rebuild(object, document_.regenContext());
    if (!built) return built;
    changes.modify(object);
    any = true;
  }
  if (!any) return Status::failure("Solids always face outwards; reverse open shapes or single faces");
  history_.execute(changes.build("Reverse Faces"));
  return Status::success();
}

namespace {

// Corners of a face, in no particular order.
std::vector<gp_Pnt> cornersOf(const TopoDS_Face& face) {
  std::vector<gp_Pnt> out;
  TopTools_IndexedMapOfShape vertices;
  TopExp::MapShapes(face, TopAbs_VERTEX, vertices);
  for (int i = 1; i <= vertices.Extent(); ++i) out.push_back(BRep_Tool::Pnt(TopoDS::Vertex(vertices(i))));
  return out;
}

bool sameCorners(std::vector<gp_Pnt> a, const std::vector<gp_Pnt>& b) {
  if (a.size() != b.size()) return false;
  for (const gp_Pnt& p : b) {
    auto it = std::find_if(a.begin(), a.end(), [&](const gp_Pnt& q) { return q.Distance(p) < 1e-3; });
    if (it == a.end()) return false;
    a.erase(it);
  }
  return true;
}

}  // namespace

Result<Object> Modeler::movedVertices(const Object& current, const std::vector<gp_Pnt>& worldPoints,
                                      const gp_Vec& worldDelta, bool autofold) const {
  using R = Result<Object>;
  if (current.shape.IsNull()) return R::failure("Object not found");
  if (current.locked) return R::failure("The object is locked");
  const ObjectKind kind = current.kind();
  if (kind == ObjectKind::Instance) {
    return R::failure("Component instances cannot be reshaped directly; edit the component (double-click)");
  }
  if (kind != ObjectKind::Solid && kind != ObjectKind::Face && kind != ObjectKind::Shell) {
    return R::failure("Only solids, faces and open shapes can be reshaped by moving corners");
  }
  const gp_Trsf toLocal = document_.worldTransform(current).Inverted();
  MoveVerticesOp op;
  for (gp_Pnt p : worldPoints) {
    p.Transform(toLocal);
    if (std::none_of(op.points.begin(), op.points.end(), [&](const gp_Pnt& q) { return q.Distance(p) < 1e-6; })) {
      op.points.push_back(p);
    }
  }
  const gp_Vec delta = worldDelta.Transformed(toLocal);
  op.direction = gp_Dir(delta);
  op.distance = delta.Magnitude();
  op.autofold = autofold;

  Object object = current;
  object.recipe.modifiers.push_back(op);
  const Status built = rebuild(object, document_.regenContext());
  if (!built) return R::failure(built.message());

  // Painted faces: the face rebuilt from the same corners (moved ones moved).
  // A face folded in two keeps its material on both pieces.
  if (!current.faceMaterials.empty()) {
    TopTools_IndexedMapOfShape before;
    TopExp::MapShapes(current.shape, TopAbs_FACE, before);
    std::vector<TopoDS_Face> after;
    for (TopExp_Explorer e(object.shape, TopAbs_FACE); e.More(); e.Next()) after.push_back(TopoDS::Face(e.Current()));
    std::vector<FaceMaterial> carried;
    for (const auto& [index, material] : resolveFaceMaterials(current).faces) {
      std::vector<gp_Pnt> corners = cornersOf(TopoDS::Face(before(index)));
      for (gp_Pnt& c : corners) {
        if (std::any_of(op.points.begin(), op.points.end(), [&](const gp_Pnt& q) { return q.Distance(c) < 1e-3; })) {
          c.Translate(delta);
        }
      }
      for (const TopoDS_Face& f : after) {
        const std::vector<gp_Pnt> mine = cornersOf(f);
        const bool same = sameCorners(mine, corners);
        // A piece of a folded face: all its corners are corners of the face.
        const bool piece = !same && std::all_of(mine.begin(), mine.end(), [&](const gp_Pnt& c) {
          return std::any_of(corners.begin(), corners.end(), [&](const gp_Pnt& d) { return c.Distance(d) < 1e-3; });
        });
        if (!same && !piece) continue;
        if (auto fm = FaceMaterial::of(f, material)) carried.push_back(*fm);
        if (same) break;
      }
    }
    object.faceMaterials = std::move(carried);
  }
  return object;
}

Status Modeler::moveVertices(const ObjectId& id, const std::vector<gp_Pnt>& worldPoints, const gp_Vec& worldDelta,
                             bool autofold) {
  const Object* current = document_.find(id);
  if (!current || current->shape.IsNull()) return Status::failure("Object not found");
  if (worldDelta.Magnitude() < tolerance::kMinDimension) return Status::failure("Distance must be non-zero");
  auto moved = movedVertices(*current, worldPoints, worldDelta, autofold);
  if (!moved) return moved.status();
  ChangeSet changes(document_);
  changes.modify(moved.value());

  // Shared boundaries follow (SketchUp): a face, shell or solid beside it in
  // the same group that shares an edge with it (the other half of a split
  // face, a neighbouring profile) moves its corners at those points too.
  // Coordinates alone are not enough: without a shared edge nothing else
  // moves.
  const TopoDS_Shape source = document_.worldShape(*current);
  TopTools_IndexedMapOfShape sourceEdges;
  TopExp::MapShapes(source, TopAbs_EDGE, sourceEdges);
  auto endsOf = [](const TopoDS_Shape& e) {
    return std::make_pair(BRep_Tool::Pnt(TopExp::FirstVertex(TopoDS::Edge(e))),
                          BRep_Tool::Pnt(TopExp::LastVertex(TopoDS::Edge(e))));
  };
  auto sameEdge = [](const std::pair<gp_Pnt, gp_Pnt>& a, const std::pair<gp_Pnt, gp_Pnt>& b) {
    return (a.first.Distance(b.first) < 1e-3 && a.second.Distance(b.second) < 1e-3) ||
           (a.first.Distance(b.second) < 1e-3 && a.second.Distance(b.first) < 1e-3);
  };
  std::vector<std::pair<gp_Pnt, gp_Pnt>> sourceEnds;
  for (int i = 1; i <= sourceEdges.Extent(); ++i) sourceEnds.push_back(endsOf(sourceEdges(i)));
  int followers = 0;
  for (const Object& o : document_.objects()) {
    if (o.id == id || o.parent != current->parent || o.locked || !document_.isVisible(o.id)) continue;
    const ObjectKind k = o.kind();
    if (k != ObjectKind::Face && k != ObjectKind::Shell && k != ObjectKind::Solid) continue;
    const TopoDS_Shape world = document_.worldShape(o);
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(world, TopAbs_EDGE, edges);
    bool connected = false;
    for (int i = 1; i <= edges.Extent() && !connected; ++i) {
      const auto ends = endsOf(edges(i));
      connected = std::any_of(sourceEnds.begin(), sourceEnds.end(), [&](const auto& s) { return sameEdge(s, ends); });
    }
    if (!connected) continue;
    TopTools_IndexedMapOfShape vertices;
    TopExp::MapShapes(world, TopAbs_VERTEX, vertices);
    std::vector<gp_Pnt> mine;
    for (const gp_Pnt& p : worldPoints) {
      for (int i = 1; i <= vertices.Extent(); ++i) {
        if (BRep_Tool::Pnt(TopoDS::Vertex(vertices(i))).Distance(p) < 1e-3) {
          mine.push_back(p);
          break;
        }
      }
    }
    if (mine.empty()) continue;
    auto follow = movedVertices(o, mine, worldDelta, autofold);
    if (!follow) {
      return Status::failure("'" + o.name + "' shares this edge and cannot follow: " + follow.error());
    }
    changes.modify(follow.value());
    ++followers;
  }
  history_.execute(changes.build(worldPoints.size() == 1 ? "Move Corner" : "Move Edge"));
  return Status::success();
}

Status Modeler::setEdgesHidden(const ObjectId& id, const std::vector<TopoDS_Edge>& worldEdges, bool hidden) {
  const Object* current = document_.find(id);
  if (!current || current->shape.IsNull()) return Status::failure("Object not found");
  const ObjectKind kind = current->kind();
  if (kind != ObjectKind::Solid && kind != ObjectKind::Face && kind != ObjectKind::Shell) {
    return Status::failure("Edges are hidden on solids, faces and open shapes (inside a group or component, edit it)");
  }
  Object o = *current;
  const gp_Trsf toLocal = document_.worldTransform(o).Inverted();
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(o.shape, TopAbs_EDGE, edges);
  bool changed = false;
  for (const TopoDS_Edge& world : worldEdges) {
    const TopoDS_Shape moved = world.Moved(TopLoc_Location(toLocal));
    int index = edges.FindIndex(moved);
    for (int i = 1; i <= edges.Extent() && index == 0; ++i) {
      if (edges(i).TShape() == world.TShape()) index = i;
    }
    if (index == 0) return Status::failure("The edge is not part of this object");
    const std::vector<int> before = hiddenEdgeIndices(o);
    const bool isHidden = std::find(before.begin(), before.end(), index) != before.end();
    if (hidden == isHidden) continue;
    if (hidden) {
      const auto mark = edgeMark(TopoDS::Edge(edges(index)));
      if (!mark) return Status::failure("This edge cannot be hidden");
      o.hiddenEdges.push_back(*mark);
    } else {
      // Every mark that finds this edge goes.
      std::vector<EdgeMark> kept;
      for (const EdgeMark& mark : o.hiddenEdges) {
        Object probe = o;
        probe.hiddenEdges = {mark};
        const std::vector<int> on = hiddenEdgeIndices(probe);
        if (std::find(on.begin(), on.end(), index) == on.end()) kept.push_back(mark);
      }
      o.hiddenEdges = std::move(kept);
    }
    changed = true;
  }
  if (!changed) return Status::success();
  history_.execute(Command{hidden ? "Hide Edge" : "Show Edge",
                           {ObjectDelta{o.id, *current, o, document_.indexOf(o.id)}}, {}});
  return Status::success();
}

Status Modeler::showHiddenEdges(const std::vector<ObjectId>& ids) {
  ChangeSet changes(document_);
  bool any = false;
  for (const ObjectId& root : ids) {
    for (const ObjectId& id : document_.subtree(root)) {
      const Object* current = document_.find(id);
      if (!current || current->hiddenEdges.empty()) continue;
      Object o = *current;
      o.hiddenEdges.clear();
      changes.modify(o);
      any = true;
    }
  }
  if (any) history_.execute(changes.build("Show Hidden Edges"));
  return Status::success();
}

Result<ObjectId> Modeler::eraseEdge(const ObjectId& id, const TopoDS_Edge& worldEdge) {
  using R = Result<ObjectId>;
  const Object* current = document_.find(id);
  if (!current || current->shape.IsNull()) return R::failure("Object not found");
  const R corner = R::failure("This edge is a corner of the shape, not a line dividing a face; "
                              "erase the whole object by clicking a face");
  // Loose profile split in parts: put them back together.
  if (const auto* part = std::get_if<FacePart>(&current->recipe.base); part && current->recipe.modifiers.empty()) {
    const TopoDS_Edge inParent = TopoDS::Edge(worldEdge.Moved(TopLoc_Location(document_.parentTransform(*current).Inverted())));
    if (part->source.empty() || !edgeOnLine(inParent, part->line)) return corner;
    std::vector<ObjectId> parts;
    for (const Object& o : document_.objects()) {
      const auto* other = std::get_if<FacePart>(&o.recipe.base);
      if (!other || o.parent != current->parent || !o.recipe.modifiers.empty() || other->source.empty()) continue;
      if (other->line.size() != part->line.size()) continue;
      bool same = other->source.front().name == part->source.front().name;
      for (std::size_t k = 0; same && k < part->line.size(); ++k) same = other->line[k].Distance(part->line[k]) < 1e-9;
      if (same) parts.push_back(o.id);
    }
    if (parts.size() < 2) return R::failure("The other part of this face is gone (extruded or deleted); cannot merge");
    const BooleanOperand& source = part->source.front();
    Object merged;
    merged.id = generateObjectId();
    merged.name = source.name.empty() ? current->name : source.name;
    merged.parent = current->parent;
    merged.layer = current->layer;
    merged.color = current->color;
    merged.material = current->material;
    merged.transform = source.placement;
    merged.recipe = *source.recipe;
    if (!rebuild(merged, document_.regenContext())) return R::failure("The original face could not be rebuilt");
    ChangeSet changes(document_);
    for (const ObjectId& p : parts) changes.erase(p);
    changes.add(merged);
    history_.execute(changes.build("Erase Line"));
    return merged.id;
  }
  if (current->kind() != ObjectKind::Solid && current->kind() != ObjectKind::Face) return corner;
  // Solids: the two faces beside the edge must lie in one plane.
  const gp_Trsf toLocal = document_.worldTransform(*current).Inverted();
  const TopoDS_Edge local = TopoDS::Edge(worldEdge.Moved(TopLoc_Location(toLocal)));
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  TopExp::MapShapesAndAncestors(current->shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
  int index = edgeFaces.FindIndex(local);
  for (int i = 1; i <= edgeFaces.Extent() && index == 0; ++i) {
    if (edgeFaces.FindKey(i).TShape() == worldEdge.TShape()) index = i;
  }
  if (index == 0) return R::failure("The edge is not part of this object");
  const TopTools_ListOfShape& beside = edgeFaces(index);
  if (beside.Extent() != 2) return corner;
  const auto a = geom::FaceRef::fromFace(TopoDS::Face(beside.First()));
  const auto b = geom::FaceRef::fromFace(TopoDS::Face(beside.Last()));
  if (!a || !b || a->normal.Angle(b->normal) > 1e-6 ||
      std::abs(gp_Vec(a->centroid, b->centroid).Dot(gp_Vec(a->normal))) > 1e-3) {
    return corner;
  }
  // The split that drew this line: dropping it must merge faces and change
  // nothing else (same volume, fewer faces).
  const double volume = geom::volume(current->shape);
  const int faces = faceCount(current->shape);
  const TopoDS_Edge edge = TopoDS::Edge(edgeFaces.FindKey(index));
  for (std::size_t k = current->recipe.modifiers.size(); k-- > 0;) {
    const auto* split = std::get_if<SplitOp>(&current->recipe.modifiers[k]);
    if (!split || !edgeOnLine(edge, split->points)) continue;
    Object object = *current;
    object.recipe.modifiers.erase(object.recipe.modifiers.begin() + static_cast<std::ptrdiff_t>(k));
    if (!rebuild(object, document_.regenContext())) continue;
    const double after = geom::volume(object.shape);
    if (std::abs(after - volume) > 1e-6 * std::max(1.0, std::abs(volume)) || faceCount(object.shape) >= faces) continue;
    const Status s = commitModified(std::move(object), "Erase Line");
    if (!s) return R::failure(s.message());
    return id;
  }
  return R::failure("This line cannot be erased on its own (it does not come from a drawn split, or later edits "
                    "depend on it)");
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

bool Modeler::facesIntoSolid(const TopoDS_Face& worldFace, const ObjectId& parent) const {
  const auto n = geom::planarFaceNormal(worldFace);
  const auto inside = geom::SurfaceRef::fromFace(worldFace);
  if (!n || !inside) return false;
  for (const Object& o : document_.objects()) {
    if (o.kind() != ObjectKind::Solid || o.parent != parent || !document_.isVisible(o.id)) continue;
    for (TopExp_Explorer e(document_.worldShape(o), TopAbs_FACE); e.More(); e.Next()) {
      const TopoDS_Face host = TopoDS::Face(e.Current());
      const auto hn = geom::planarFaceNormal(host);
      if (hn && hn->IsParallel(*n, 1e-6) && geom::pointOnFace(host, inside->point)) return hn->Dot(*n) < 0;
    }
  }
  return false;
}

bool Modeler::isLooseCurveEnd(const gp_Pnt& p) const {
  for (const Object& o : document_.objects()) {
    if (const auto ends = curveEnds(document_, o)) {
      if (ends->first.Distance(p) < 1e-3 || ends->second.Distance(p) < 1e-3) return true;
    }
  }
  return false;
}

std::vector<gp_Pnt> Modeler::openCurveEnds() const {
  std::vector<gp_Pnt> ends;
  for (const Object& o : document_.objects()) {
    if (o.parent != activeGroup_) continue;
    if (const auto e = curveEnds(document_, o)) {
      ends.push_back(e->first);
      ends.push_back(e->second);
    }
  }
  std::vector<gp_Pnt> open;
  for (std::size_t i = 0; i < ends.size(); ++i) {
    int shared = 0;
    for (std::size_t j = 0; j < ends.size(); ++j) shared += ends[i].Distance(ends[j]) < 1e-3;
    if (shared == 1) open.push_back(ends[i]);
  }
  return open;
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
  // Drawn on a face of a solid: face outwards like it (as profiles drawn
  // with the rectangle tool do), so pushing in cuts and pulling out adds.
  {
    std::vector<TopoDS_Shape> placed;
    for (const BooleanOperand& c : loop.curves) {
      auto shape = regenerate(*c.recipe, document_.regenContext());
      if (!shape) break;
      placed.push_back(shape->Moved(TopLoc_Location(document_.parentTransform(*start) * c.placement)));
    }
    auto candidate = placed.size() == loop.curves.size() ? geom::faceFromCurves(placed)
                                                          : Result<TopoDS_Face>::failure("");
    if (candidate) loop.reversed = facesIntoSolid(candidate.value(), start->parent);
    // There and back along the same line (e.g. a line drawn again over one
    // already there): no area, not a loop.
    if (candidate && geom::area(candidate.value()) < tolerance::kMinDimension * tolerance::kMinDimension) {
      if (loopFound) *loopFound = false;
      return Result<ObjectId>::failure("The curve does not close a loop");
    }
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
  punchHole(*id);
  if (const Status removed = remove(ids); !removed) {
    history_.abortGroup();
    return Result<ObjectId>::failure(removed.message());
  }
  history_.endGroup();
  return id;
}

namespace {

// The loose straight lines a chain can close areas with: same group, same
// plane, visible. Segments of `chain` come first (fresh).
struct CrossingSetup {
  gp_Pln plane;
  std::vector<std::pair<gp_Pnt, gp_Pnt>> segments;
  std::vector<bool> fresh;
  std::vector<std::pair<const Object*, std::size_t>> owners;  // (object, segment index in it), null = chain
  std::vector<bool> fixed;               // an edge of a solid: bounds faces, never replaced
  std::vector<TopoDS_Face> existing;     // faces of those solids on the plane (already faces)
};

std::vector<gp_Pnt> worldPolyline(const Document& document, const Object& o) {
  std::vector<gp_Pnt> out;
  const auto* line = std::get_if<PolylineCurve>(&o.recipe.base);
  if (!line || !o.recipe.modifiers.empty()) return out;
  const gp_Trsf world = document.worldTransform(o);
  for (const gp_Pnt& p : line->points) out.push_back(p.Transformed(world));
  return out;
}

std::optional<CrossingSetup> crossingSetup(const Document& document, const std::vector<gp_Pnt>& chain,
                                           const ObjectId& parent, const ObjectId& skip,
                                           std::optional<gp_Pln> forced = std::nullopt) {
  constexpr double kTol = 1e-4;
  if (chain.size() < 2) return std::nullopt;
  std::vector<const Object*> others;
  for (const Object& o : document.objects()) {
    if (o.id == skip || o.parent != parent || o.kind() != ObjectKind::Curve || !document.isVisible(o.id)) continue;
    if (worldPolyline(document, o).size() >= 2) others.push_back(&o);
  }
  // The plane: the chain's own, else the chain's line with a line it crosses.
  std::optional<gp_Pln> plane = forced;
  const gp_Vec d(chain.front(), chain.back());
  for (std::size_t i = 1; i + 1 < chain.size() && !plane; ++i) {
    const gp_Vec n = gp_Vec(chain.front(), chain[i]).Crossed(gp_Vec(chain.front(), chain.back()));
    if (n.Magnitude() > 1e-6 * d.SquareMagnitude() + 1e-9) plane = gp_Pln(chain.front(), gp_Dir(n));
  }
  for (std::size_t k = 1; k < chain.size() && !plane; ++k) {
    const gp_Vec dk(chain[k - 1], chain[k]);
    if (dk.Magnitude() < kTol) continue;
    for (const Object* o : others) {
      const std::vector<gp_Pnt> pts = worldPolyline(document, *o);
      for (std::size_t j = 1; j < pts.size() && !plane; ++j) {
        const gp_Vec e(pts[j - 1], pts[j]);
        const gp_Vec n = dk.Crossed(e);
        if (n.Magnitude() < 1e-9 * dk.Magnitude() * e.Magnitude()) continue;
        const gp_Pln candidate(chain[k - 1], gp_Dir(n));
        if (candidate.Distance(pts[j - 1]) < kTol) plane = candidate;  // they meet (same plane)
      }
    }
  }
  if (!plane) return std::nullopt;
  // Faces drawn on the ground look up; vertical ones keep the plane's side.
  if (plane->Axis().Direction().Z() < -1e-9) plane = gp_Pln(plane->Location(), plane->Axis().Direction().Reversed());
  for (const gp_Pnt& p : chain) {
    if (plane->Distance(p) > kTol) return std::nullopt;  // not flat: no faces
  }
  CrossingSetup setup;
  setup.plane = *plane;
  for (std::size_t k = 1; k < chain.size(); ++k) {
    setup.segments.emplace_back(chain[k - 1], chain[k]);
    setup.fresh.push_back(true);
    setup.owners.emplace_back(nullptr, k - 1);
  }
  for (const Object* o : others) {
    const std::vector<gp_Pnt> pts = worldPolyline(document, *o);
    if (std::any_of(pts.begin(), pts.end(), [&](const gp_Pnt& p) { return plane->Distance(p) > kTol; })) continue;
    for (std::size_t k = 1; k < pts.size(); ++k) {
      setup.segments.emplace_back(pts[k - 1], pts[k]);
      setup.fresh.push_back(false);
      setup.owners.emplace_back(o, k - 1);
    }
  }
  setup.fixed.assign(setup.segments.size(), false);
  return setup;
}

// Ungrouped solids and open shells beside the chain (same group, visible)
// whose bounding box reaches it: their faces can close areas with it.
std::vector<const Object*> solidsNear(const Document& document, const std::vector<gp_Pnt>& chain,
                                      const ObjectId& parent) {
  Bnd_Box reach;
  for (const gp_Pnt& p : chain) reach.Add(p);
  reach.Enlarge(1e-3);
  std::vector<const Object*> out;
  for (const Object& o : document.objects()) {
    if (o.parent != parent || !document.isVisible(o.id)) continue;
    if (o.kind() != ObjectKind::Solid && o.kind() != ObjectKind::Shell) continue;
    Bnd_Box box;
    BRepBndLib::Add(document.worldShape(o), box);
    if (!box.IsOut(reach)) out.push_back(&o);
  }
  return out;
}

// The setups on planes of solid faces that hold the whole chain: the chain,
// loose lines on that plane, and the straight edges of those solids lying on
// it (fixed: they bound new faces but stay as they are).
std::vector<CrossingSetup> solidSetups(const Document& document, const std::vector<gp_Pnt>& chain,
                                       const ObjectId& parent, const ObjectId& skip) {
  constexpr double kTol = 1e-4;
  std::vector<CrossingSetup> out;
  const std::vector<const Object*> solids = solidsNear(document, chain, parent);
  if (solids.empty()) return out;
  std::vector<gp_Pln> planes;
  for (const Object* o : solids) {
    for (TopExp_Explorer f(document.worldShape(*o), TopAbs_FACE); f.More(); f.Next()) {
      gp_Pln pln;
      if (!geom::isPlanarFace(TopoDS::Face(f.Current()), &pln)) continue;
      if (std::any_of(chain.begin(), chain.end(), [&](const gp_Pnt& p) { return pln.Distance(p) > kTol; })) continue;
      const bool known = std::any_of(planes.begin(), planes.end(), [&](const gp_Pln& q) {
        return q.Axis().Direction().IsParallel(pln.Axis().Direction(), 1e-9) && q.Distance(pln.Location()) < kTol;
      });
      if (!known) planes.push_back(pln);
    }
  }
  for (const gp_Pln& pln : planes) {
    auto setup = crossingSetup(document, chain, parent, skip, pln);
    if (!setup) continue;
    for (const Object* o : solids) {
      const TopoDS_Shape world = document.worldShape(*o);
      for (TopExp_Explorer f(world, TopAbs_FACE); f.More(); f.Next()) {
        gp_Pln on;
        const TopoDS_Face face = TopoDS::Face(f.Current());
        if (geom::isPlanarFace(face, &on) && on.Axis().Direction().IsParallel(pln.Axis().Direction(), 1e-9) &&
            on.Distance(pln.Location()) < kTol) {
          setup->existing.push_back(face);
        }
      }
      TopTools_IndexedMapOfShape edges;
      TopExp::MapShapes(world, TopAbs_EDGE, edges);
      for (int i = 1; i <= edges.Extent(); ++i) {
        const TopoDS_Edge edge = TopoDS::Edge(edges(i));
        if (BRep_Tool::Degenerated(edge) || BRepAdaptor_Curve(edge).GetType() != GeomAbs_Line) continue;
        const gp_Pnt a = BRep_Tool::Pnt(TopExp::FirstVertex(edge));
        const gp_Pnt b = BRep_Tool::Pnt(TopExp::LastVertex(edge));
        if (pln.Distance(a) > kTol || pln.Distance(b) > kTol) continue;
        setup->segments.emplace_back(a, b);
        setup->fresh.push_back(false);
        setup->owners.emplace_back(nullptr, 0);
        setup->fixed.push_back(true);
      }
    }
    out.push_back(std::move(*setup));
  }
  return out;
}

// A point strictly inside a simple polygon lying in `plane`.
std::optional<gp_Pnt> insidePolygon(const std::vector<gp_Pnt>& polygon, const gp_Pln& plane) {
  const gp_Ax3 frame = plane.Position();
  auto uv = [&](const gp_Pnt& p) {
    const gp_Vec d(frame.Location(), p);
    return gp_Pnt2d(d.Dot(gp_Vec(frame.XDirection())), d.Dot(gp_Vec(frame.YDirection())));
  };
  std::vector<gp_Pnt2d> pts;
  for (const gp_Pnt& p : polygon) pts.push_back(uv(p));
  auto inside = [&](const gp_Pnt2d& q) {
    bool in = false;
    for (std::size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
      if ((pts[i].Y() > q.Y()) != (pts[j].Y() > q.Y()) &&
          q.X() < (pts[j].X() - pts[i].X()) * (q.Y() - pts[i].Y()) / (pts[j].Y() - pts[i].Y()) + pts[i].X()) {
        in = !in;
      }
    }
    return in;
  };
  const std::size_t n = pts.size();
  if (n < 3) return std::nullopt;
  std::vector<gp_Pnt2d> tries;
  gp_XY c(0, 0);
  for (const gp_Pnt2d& p : pts) c += p.XY();
  tries.emplace_back(c / static_cast<double>(n));
  for (std::size_t i = 0; i < n; ++i) {  // centres of corner triangles
    tries.emplace_back((pts[(i + n - 1) % n].XY() + pts[i].XY() + pts[(i + 1) % n].XY()) / 3.0);
  }
  for (const gp_Pnt2d& q : tries) {
    if (!inside(q)) continue;
    return gp_Pnt(frame.Location().XYZ() + q.X() * frame.XDirection().XYZ() + q.Y() * frame.YDirection().XYZ());
  }
  return std::nullopt;
}

// Regions of an arrangement that are not already faces of the solids, and
// which pieces bound them.
void keepNewRegions(const CrossingSetup& setup, geom::LineArrangement& arrangement) {
  if (setup.existing.empty()) return;
  std::vector<std::vector<gp_Pnt>> kept;
  for (const auto& region : arrangement.regions) {
    const auto at = insidePolygon(region, setup.plane);
    if (!at) continue;
    const bool covered = std::any_of(setup.existing.begin(), setup.existing.end(), [&](const TopoDS_Face& f) {
      return BRepClass_FaceClassifier(f, *at, 1e-6).State() == TopAbs_IN;
    });
    if (!covered) kept.push_back(region);
  }
  arrangement.regions = kept;
  for (auto& piece : arrangement.pieces) {
    piece.bounding = false;
    for (const auto& region : kept) {
      for (std::size_t i = 0; i < region.size() && !piece.bounding; ++i) {
        const gp_Pnt& a = region[i];
        const gp_Pnt& b = region[(i + 1) % region.size()];
        piece.bounding = (a.Distance(piece.a) < 1e-6 && b.Distance(piece.b) < 1e-6) ||
                         (a.Distance(piece.b) < 1e-6 && b.Distance(piece.a) < 1e-6);
      }
    }
  }
}

// The first setup (the chain's own plane, else a solid face's plane through
// it) whose arrangement closes new areas.
std::optional<std::pair<CrossingSetup, geom::LineArrangement>> closingSetup(const Document& document,
                                                                            const std::vector<gp_Pnt>& chain,
                                                                            const ObjectId& parent,
                                                                            const ObjectId& skip) {
  std::vector<CrossingSetup> setups;
  if (auto own = crossingSetup(document, chain, parent, skip)) setups.push_back(std::move(*own));
  for (CrossingSetup& s : solidSetups(document, chain, parent, skip)) setups.push_back(std::move(s));
  for (CrossingSetup& setup : setups) {
    if (setup.segments.size() < 3) continue;
    geom::LineArrangement arrangement = geom::arrangeLines(setup.segments, setup.fresh, setup.plane);
    keepNewRegions(setup, arrangement);
    if (!arrangement.regions.empty()) return std::make_pair(std::move(setup), std::move(arrangement));
  }
  return std::nullopt;
}

}  // namespace

bool Modeler::closesByCrossing(const std::vector<gp_Pnt>& chain) const {
  return closingSetup(document_, chain, activeGroup_, {}).has_value();
}

Result<std::vector<ObjectId>> Modeler::facesFromCrossings(const ObjectId& curveId) {
  using R = Result<std::vector<ObjectId>>;
  const Object* start = document_.find(curveId);
  if (!start) return R::failure("Object not found");
  const std::vector<gp_Pnt> chain = worldPolyline(document_, *start);
  if (chain.size() < 2) return std::vector<ObjectId>{};
  auto closing = closingSetup(document_, chain, start->parent, curveId);
  if (!closing) return std::vector<ObjectId>{};
  const CrossingSetup* setup = &closing->first;
  const geom::LineArrangement& arrangement = closing->second;

  // Lines with a piece bounding a new face are replaced: their other pieces
  // (tails) come back as lines, in order.
  std::vector<const Object*> touched{start};
  for (const geom::LineArrangement::Piece& piece : arrangement.pieces) {
    const Object* owner = setup->owners[static_cast<std::size_t>(piece.segment)].first;
    if (piece.bounding && owner && std::find(touched.begin(), touched.end(), owner) == touched.end()) {
      touched.push_back(owner);
    }
  }
  std::vector<std::vector<gp_Pnt>> leftovers;
  for (const Object* o : touched) {
    std::vector<gp_Pnt> run;
    for (const geom::LineArrangement::Piece& piece : arrangement.pieces) {
      if (setup->fixed[static_cast<std::size_t>(piece.segment)]) continue;  // a solid's edge
      const Object* owner = setup->owners[static_cast<std::size_t>(piece.segment)].first;
      if ((owner ? owner : start) != o) continue;
      if (piece.bounding || (!run.empty() && run.back().Distance(piece.a) > 1e-6)) {
        if (run.size() >= 2) leftovers.push_back(run);
        run.clear();
      }
      if (piece.bounding) continue;
      if (run.empty()) run.push_back(piece.a);
      run.push_back(piece.b);
    }
    if (run.size() >= 2) leftovers.push_back(run);
  }

  history_.beginGroup("Faces");
  std::vector<ObjectId> faces;
  std::vector<ObjectId> removed;
  for (const Object* o : touched) removed.push_back(o->id);
  if (const Status s = remove(removed); !s) {
    history_.abortGroup();
    return R::failure(s.message());
  }
  for (const std::vector<gp_Pnt>& region : arrangement.regions) {
    auto face = createPolygon(region);
    if (!face) {
      history_.abortGroup();
      return R::failure(face.error());
    }
    faces.push_back(*face);
  }
  for (const std::vector<gp_Pnt>& run : leftovers) {
    if (auto line = createPolyline(run); !line) {
      history_.abortGroup();
      return R::failure(line.error());
    }
  }
  history_.endGroup();
  return faces;
}

namespace {

// A sub-edge of `edge` between curve parameters (null when too short).
TopoDS_Edge subEdge(const TopoDS_Edge& edge, double t0, double t1) {
  if (t1 - t0 < 1e-9) return {};
  double f, l;
  const Handle(Geom_Curve) curve = BRep_Tool::Curve(edge, f, l);
  if (curve.IsNull()) return {};
  BRepBuilderAPI_MakeEdge make(curve, t0, t1);
  if (!make.IsDone()) return {};
  const TopoDS_Edge piece = make.Edge();
  if (GCPnts_AbscissaPoint::Length(BRepAdaptor_Curve(piece)) < tolerance::kMinDimension) return {};
  return piece;
}

// Edges of a wire / curve shape in drawing order.
std::vector<TopoDS_Edge> chainOf(const TopoDS_Shape& shape) {
  std::vector<TopoDS_Edge> out;
  for (TopExp_Explorer w(shape, TopAbs_WIRE); w.More(); w.Next()) {
    for (BRepTools_WireExplorer e(TopoDS::Wire(w.Current())); e.More(); e.Next()) out.push_back(e.Current());
    return out;
  }
  for (TopExp_Explorer e(shape, TopAbs_EDGE); e.More(); e.Next()) out.push_back(TopoDS::Edge(e.Current()));
  return out;
}

// Loose-line recipes (world coordinates) for edges in order: runs of
// straight edges -> one polyline, circular -> arc, others -> the curve as it
// is (exact, never a polyline through points on it).
std::vector<std::pair<Recipe, std::string>> curveRecipes(const std::vector<TopoDS_Edge>& run) {
  std::vector<std::pair<Recipe, std::string>> out;
  std::vector<gp_Pnt> polyline;
  auto flush = [&] {
    if (polyline.size() >= 2) {
      Recipe r;
      r.base = PolylineCurve{polyline};
      out.emplace_back(std::move(r), "Line");
    }
    polyline.clear();
  };
  for (const TopoDS_Edge& edge : run) {
    const gp_Pnt a = BRep_Tool::Pnt(TopExp::FirstVertex(edge, true));
    const gp_Pnt b = BRep_Tool::Pnt(TopExp::LastVertex(edge, true));
    BRepAdaptor_Curve c(edge);
    if (c.GetType() == GeomAbs_Line) {
      if (polyline.empty() || polyline.back().Distance(a) > 1e-6) {
        flush();
        polyline.push_back(a);
      }
      polyline.push_back(b);
      continue;
    }
    flush();
    const gp_Pnt mid = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
    Recipe r;
    if (c.GetType() == GeomAbs_Circle && a.Distance(b) > tolerance::kMinDimension) {
      const gp_Dir normal = c.Circle().Axis().Direction();
      const gp_Vec chord(a, b);
      const gp_Vec side = gp_Vec(normal).Crossed(chord);
      const gp_Pnt chordMid((a.XYZ() + b.XYZ()) / 2.0);
      r.base = ArcCurve{a, b, normal, gp_Vec(chordMid, mid).Dot(side.Normalized()), false};
      out.emplace_back(std::move(r), "Arc");
      continue;
    }
    // A whole circle, an ellipse, a spline: kept exactly as it is.
    r.base = ImportedShape{BRepBuilderAPI_Copy(edge).Shape(), "line"};
    out.emplace_back(std::move(r), c.GetType() == GeomAbs_Circle ? "Circle" : "Curve");
  }
  flush();
  return out;
}

// Turns edges (in order, world coordinates) back into loose lines (see
// curveRecipes).
Status recreateCurves(Modeler& m, const std::vector<TopoDS_Edge>& run, std::vector<ObjectId>& made) {
  for (auto& [recipe, kind] : curveRecipes(run)) {
    Result<ObjectId> id = Result<ObjectId>::failure("");
    if (const auto* line = std::get_if<PolylineCurve>(&recipe.base)) {
      id = m.createPolyline(line->points);
    } else if (const auto* arc = std::get_if<ArcCurve>(&recipe.base)) {
      id = m.createArc(arc->start, arc->end, arc->normal, arc->bulge, false);
    } else {
      id = m.importShape(std::get<ImportedShape>(recipe.base).shape, kind, "line");
    }
    if (!id) return id.status();
    made.push_back(*id);
  }
  return Status::success();
}

}  // namespace

Result<std::vector<ObjectId>> Modeler::eraseLinePiece(const ObjectId& id, const TopoDS_Edge& worldEdge,
                                                      const gp_Pnt& worldPoint) {
  using R = Result<std::vector<ObjectId>>;
  const Object* o = document_.find(id);
  if (!o || o->shape.IsNull()) return R::failure("Object not found");
  if (o->locked) return R::failure("The object is locked");
  const ObjectKind kind = o->kind();
  if (kind != ObjectKind::Curve && kind != ObjectKind::Face) {
    return R::failure("Lines are erased piece by piece on loose lines and flat faces");
  }
  const TopoDS_Shape world = document_.worldShape(*o);
  // The chains of edges: one for a line; each loop of a face (outer first).
  std::vector<std::vector<TopoDS_Edge>> chains;
  std::vector<bool> closed;
  if (kind == ObjectKind::Curve) {
    chains.push_back(chainOf(world));
    closed.push_back(false);
  } else {
    if (world.ShapeType() != TopAbs_FACE) return R::failure("Lines are erased piece by piece on flat faces only");
    const TopoDS_Face face = TopoDS::Face(world);
    const TopoDS_Wire outer = BRepTools::OuterWire(face);
    std::vector<TopoDS_Wire> wires{outer};
    for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
      if (!w.Current().IsSame(outer)) wires.push_back(TopoDS::Wire(w.Current()));
    }
    for (const TopoDS_Wire& w : wires) {
      std::vector<TopoDS_Edge> chain;
      for (BRepTools_WireExplorer e(w, face); e.More(); e.Next()) chain.push_back(e.Current());
      chains.push_back(chain);
      closed.push_back(true);
    }
  }
  std::size_t ci = 0, ei = 0;
  bool found = false;
  for (std::size_t c = 0; c < chains.size() && !found; ++c) {
    for (std::size_t e = 0; e < chains[c].size() && !found; ++e) {
      if (chains[c][e].IsSame(worldEdge) || chains[c][e].TShape() == worldEdge.TShape()) {
        ci = c;
        ei = e;
        found = true;
      }
    }
  }
  if (!found) return R::failure("The edge is not part of this object");
  const TopoDS_Edge edge = chains[ci][ei];

  // Break points on it: where lines or face outlines of the same group (and
  // the object's other edges) cross or end on it.
  double f, l;
  const Handle(Geom_Curve) curve = BRep_Tool::Curve(edge, f, l);
  if (curve.IsNull()) return R::failure("This line cannot be erased piece by piece");
  std::vector<double> cuts{f, l};
  std::vector<TopoDS_Shape> others;
  for (const Object& other : document_.objects()) {
    if (other.id == id || other.parent != o->parent || !document_.isVisible(other.id)) continue;
    if (other.kind() == ObjectKind::Curve || other.kind() == ObjectKind::Face) others.push_back(document_.worldShape(other));
  }
  for (const auto& chain : chains) {
    for (const TopoDS_Edge& e : chain) {
      if (!e.IsSame(edge)) others.push_back(e);
    }
  }
  for (const TopoDS_Shape& other : others) {
    for (TopExp_Explorer oe(other, TopAbs_EDGE); oe.More(); oe.Next()) {
      BRepExtrema_DistShapeShape d(edge, oe.Current());
      if (!d.IsDone() || d.Value() > 1e-3) continue;
      for (int k = 1; k <= d.NbSolution(); ++k) {
        GeomAPI_ProjectPointOnCurve on(d.PointOnShape1(k), curve, f, l);
        if (on.NbPoints() > 0 && on.LowerDistance() < 1e-3) cuts.push_back(on.LowerDistanceParameter());
      }
    }
  }
  std::sort(cuts.begin(), cuts.end());
  GeomAPI_ProjectPointOnCurve click(worldPoint, curve, f, l);
  const double t = click.NbPoints() > 0 ? click.LowerDistanceParameter() : 0.5 * (f + l);
  double ta = f, tb = l;
  for (double c : cuts) {
    if (c <= t) ta = std::max(ta, c);
    if (c >= t) tb = std::min(tb, c);
  }
  if (tb - ta < 1e-12) return R::failure("Click on the line piece to erase");
  // The pieces of the edge before and after the erased one, in chain order.
  const bool reversed = edge.Orientation() == TopAbs_REVERSED;
  TopoDS_Edge low = subEdge(edge, f, ta), high = subEdge(edge, tb, l);
  if (reversed) {
    if (!low.IsNull()) low.Reverse();
    if (!high.IsNull()) high.Reverse();
    std::swap(low, high);
  }
  std::vector<std::vector<TopoDS_Edge>> runs;
  const std::vector<TopoDS_Edge>& chain = chains[ci];
  if (closed[ci]) {
    std::vector<TopoDS_Edge> run;
    if (!high.IsNull()) run.push_back(high);
    for (std::size_t k = 1; k < chain.size(); ++k) run.push_back(chain[(ei + k) % chain.size()]);
    if (!low.IsNull()) run.push_back(low);
    runs.push_back(run);
  } else {
    std::vector<TopoDS_Edge> before(chain.begin(), chain.begin() + static_cast<std::ptrdiff_t>(ei));
    if (!low.IsNull()) before.push_back(low);
    std::vector<TopoDS_Edge> after;
    if (!high.IsNull()) after.push_back(high);
    after.insert(after.end(), chain.begin() + static_cast<std::ptrdiff_t>(ei) + 1, chain.end());
    runs.push_back(before);
    runs.push_back(after);
  }
  for (std::size_t c = 0; c < chains.size(); ++c) {
    if (c != ci) runs.push_back(chains[c]);  // a face's holes stay as lines
  }

  history_.beginGroup("Erase Line");
  if (const Status removed = remove({id}); !removed) {
    history_.abortGroup();
    return R::failure(removed.message());
  }
  std::vector<ObjectId> made;
  for (const auto& run : runs) {
    if (run.empty()) continue;
    if (const Status s = recreateCurves(*this, run, made); !s) {
      history_.abortGroup();
      return R::failure(s.message());
    }
  }
  history_.endGroup();
  return made;
}

namespace {

// Edges joined end to end into runs (each oriented along its run), so that
// lines come back as polylines rather than one object per edge.
std::vector<std::vector<TopoDS_Edge>> chainRuns(std::vector<TopoDS_Edge> edges) {
  auto first = [](const TopoDS_Edge& e) { return BRep_Tool::Pnt(TopExp::FirstVertex(e, true)); };
  auto last = [](const TopoDS_Edge& e) { return BRep_Tool::Pnt(TopExp::LastVertex(e, true)); };
  std::vector<std::vector<TopoDS_Edge>> runs;
  std::vector<bool> used(edges.size(), false);
  for (std::size_t s = 0; s < edges.size(); ++s) {
    if (used[s]) continue;
    used[s] = true;
    std::deque<TopoDS_Edge> run{edges[s]};
    for (bool grown = true; grown;) {
      grown = false;
      for (std::size_t k = 0; k < edges.size() && !grown; ++k) {
        if (used[k]) continue;
        const TopoDS_Edge e = edges[k];
        if (first(e).Distance(last(run.back())) < 1e-6) {
          run.push_back(e);
        } else if (last(e).Distance(last(run.back())) < 1e-6) {
          run.push_back(TopoDS::Edge(e.Reversed()));
        } else if (last(e).Distance(first(run.front())) < 1e-6) {
          run.push_front(e);
        } else if (first(e).Distance(first(run.front())) < 1e-6) {
          run.push_front(TopoDS::Edge(e.Reversed()));
        } else {
          continue;
        }
        used[k] = true;
        grown = true;
      }
    }
    runs.emplace_back(run.begin(), run.end());
  }
  return runs;
}

bool containsEdge(const std::vector<TopoDS_Edge>& list, const TopoDS_Edge& e) {
  return std::any_of(list.begin(), list.end(),
                     [&](const TopoDS_Edge& x) { return x.IsSame(e) || x.TShape() == e.TShape(); });
}

}  // namespace

Result<std::vector<ObjectId>> Modeler::deleteFaces(const ObjectId& id, const std::vector<TopoDS_Face>& worldFaces,
                                                   const std::vector<TopoDS_Edge>& droppedEdges) {
  using R = Result<std::vector<ObjectId>>;
  const Object* o = document_.find(id);
  if (!o || o->shape.IsNull()) return R::failure("Object not found");
  if (o->locked) return R::failure("The object is locked");
  const ObjectKind kind = o->kind();
  if (kind != ObjectKind::Face && kind != ObjectKind::Solid && kind != ObjectKind::Shell) {
    return R::failure("Only faces of profiles, solids and open shapes are deleted on their own");
  }
  if (worldFaces.empty() && droppedEdges.empty()) return R::failure("No face to delete");
  if (o->parent != activeGroup_) return R::failure("Open the group to delete faces inside it");
  const TopoDS_Shape world = document_.worldShape(*o);
  TopTools_IndexedMapOfShape allFaces;
  TopExp::MapShapes(world, TopAbs_FACE, allFaces);
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  TopExp::MapShapesAndAncestors(world, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
  // The faces that go: those asked for, and those an edge taken away bounds.
  auto indexOf = [&](const TopoDS_Shape& f) {
    int index = allFaces.FindIndex(f);
    for (int i = 1; i <= allFaces.Extent() && index == 0; ++i) {
      if (allFaces(i).TShape() == f.TShape()) index = i;
    }
    return index;
  };
  std::set<int> going;
  for (const TopoDS_Face& f : worldFaces) {
    const int index = indexOf(f);
    if (index == 0) return R::failure("The face is not part of this object");
    going.insert(index);
  }
  std::vector<TopoDS_Edge> dropped;
  for (const TopoDS_Edge& e : droppedEdges) {
    int index = edgeFaces.FindIndex(e);
    for (int i = 1; i <= edgeFaces.Extent() && index == 0; ++i) {
      if (edgeFaces.FindKey(i).TShape() == e.TShape()) index = i;
    }
    if (index == 0) return R::failure("The edge is not part of this object");
    dropped.push_back(TopoDS::Edge(edgeFaces.FindKey(index)));
    for (const TopoDS_Shape& f : edgeFaces(index)) going.insert(indexOf(f));
  }
  // Lines left: edges of the faces that go which no remaining face uses.
  std::vector<TopoDS_Edge> lines;
  for (int i = 1; i <= edgeFaces.Extent(); ++i) {
    const TopoDS_Edge edge = TopoDS::Edge(edgeFaces.FindKey(i));
    if (BRep_Tool::Degenerated(edge) || containsEdge(dropped, edge)) continue;
    bool byGoing = false, byStaying = false;
    for (const TopoDS_Shape& f : edgeFaces(i)) (going.count(indexOf(f)) ? byGoing : byStaying) = true;
    if (byGoing && !byStaying) lines.push_back(edge);
  }
  const bool everything = static_cast<int>(going.size()) == allFaces.Extent();
  const std::string layer = o->layer;
  history_.beginGroup("Delete");
  if (everything) {
    if (const Status s = remove({id}); !s) {
      history_.abortGroup();
      return R::failure(s.message());
    }
  } else {
    std::vector<TopoDS_Face> faces;
    for (int index : going) faces.push_back(TopoDS::Face(allFaces(index)));
    if (const Status s = removeFaces(id, faces); !s) {
      history_.abortGroup();
      return R::failure(s.message());
    }
  }
  std::vector<ObjectId> made;
  for (const auto& run : chainRuns(lines)) {
    if (const Status s = recreateCurves(*this, run, made); !s) {
      history_.abortGroup();
      return R::failure(s.message());
    }
  }
  for (const ObjectId& line : made) {
    if (layer.empty()) continue;
    if (const Status s = setLayer(line, layer); !s) {
      history_.abortGroup();
      return R::failure(s.message());
    }
  }
  history_.endGroup();
  return made;
}

namespace {

// An edge recognised by its geometry: both ends and its middle.
struct EdgeKey {
  gp_Pnt a, b, mid;
  static EdgeKey of(const TopoDS_Edge& e) {
    BRepAdaptor_Curve c(e);
    return {BRep_Tool::Pnt(TopExp::FirstVertex(e)), BRep_Tool::Pnt(TopExp::LastVertex(e)),
            c.Value(0.5 * (c.FirstParameter() + c.LastParameter()))};
  }
  bool matches(const TopoDS_Edge& e) const {
    const EdgeKey k = of(e);
    const bool ends = (a.Distance(k.a) < 1e-6 && b.Distance(k.b) < 1e-6) || (a.Distance(k.b) < 1e-6 && b.Distance(k.a) < 1e-6);
    return ends && mid.Distance(k.mid) < 1e-6;
  }
};

// A face recognised by a point strictly inside it and its plane's normal.
struct FaceKey {
  gp_Pnt inside;
  gp_Dir normal;
  bool matches(const TopoDS_Face& f) const {
    const auto n = geom::planarFaceNormal(f);
    if (n && n->Angle(normal) > 1e-6) return false;
    return BRepClass_FaceClassifier(f, inside, 1e-6).State() == TopAbs_IN;
  }
};

}  // namespace

Result<DeleteOutcome> Modeler::deleteSelection(const std::vector<DeleteTarget>& targets) {
  using R = Result<DeleteOutcome>;
  if (targets.empty()) return R::failure("Nothing selected to delete");
  // 1. Everything located and described before anything changes.
  struct Plan {
    ObjectId id;
    ObjectId parent;
    std::string name;
    std::vector<FaceKey> faces;
    std::vector<EdgeKey> edges;
  };
  std::vector<ObjectId> whole;
  std::vector<Plan> plans;
  for (const DeleteTarget& t : targets) {
    const Object* o = document_.find(t.id);
    if (!o) return R::failure("Something selected no longer exists; select again");
    if (o->locked) return R::failure("'" + o->name + "' is locked");
    if (t.whole || (t.faces.empty() && t.edges.empty())) {
      whole.push_back(t.id);
      continue;
    }
    const ObjectKind kind = o->kind();
    if (kind != ObjectKind::Curve && kind != ObjectKind::Face && kind != ObjectKind::Shell && kind != ObjectKind::Solid) {
      return R::failure("Faces and edges of '" + o->name + "' cannot be deleted on their own; delete the whole object");
    }
    Plan plan{t.id, o->parent, o->name, {}, {}};
    for (const TopoDS_Face& f : t.faces) {
      auto ref = geom::FaceRef::identify(f);
      if (!ref || !ref->inside) return R::failure("A selected face of '" + o->name + "' is not flat; delete the whole object");
      plan.faces.push_back({*ref->inside, ref->normal});
    }
    for (const TopoDS_Edge& e : t.edges) plan.edges.push_back(EdgeKey::of(e));
    plans.push_back(std::move(plan));
  }
  // Parts of objects that go as a whole anyway.
  plans.erase(std::remove_if(plans.begin(), plans.end(), [&](const Plan& p) {
                return std::any_of(whole.begin(), whole.end(), [&](const ObjectId& w) {
                  return w == p.id || document_.isAncestor(w, p.id);
                });
              }),
              plans.end());

  // Finds an element again, in its object or (after a merge made a new
  // object) among the objects beside it.
  auto candidates = [&](const Plan& p) {
    std::vector<const Object*> out;
    if (const Object* o = document_.find(p.id)) out.push_back(o);
    for (const Object& o : document_.objects()) {
      if (o.id != p.id && o.parent == p.parent && !o.shape.IsNull() && document_.isVisible(o.id)) out.push_back(&o);
    }
    return out;
  };
  auto findEdge = [&](const Plan& p, const EdgeKey& key) -> std::optional<std::pair<ObjectId, TopoDS_Edge>> {
    for (const Object* o : candidates(p)) {
      TopTools_IndexedMapOfShape edges;
      TopExp::MapShapes(document_.worldShape(*o), TopAbs_EDGE, edges);
      for (int i = 1; i <= edges.Extent(); ++i) {
        if (key.matches(TopoDS::Edge(edges(i)))) return std::make_pair(o->id, TopoDS::Edge(edges(i)));
      }
    }
    return std::nullopt;
  };
  auto findFace = [&](const Plan& p, const FaceKey& key) -> std::optional<std::pair<ObjectId, TopoDS_Face>> {
    for (const Object* o : candidates(p)) {
      for (TopExp_Explorer f(document_.worldShape(*o), TopAbs_FACE); f.More(); f.Next()) {
        if (key.matches(TopoDS::Face(f.Current()))) return std::make_pair(o->id, TopoDS::Face(f.Current()));
      }
    }
    return std::nullopt;
  };

  DeleteOutcome outcome;
  history_.beginGroup("Delete");
  auto fail = [&](const std::string& why) {
    history_.abortGroup();
    return R::failure(why);
  };
  if (!whole.empty()) {
    if (const Status s = remove(whole); !s) return fail(s.message());
    outcome.removed = whole;
  }
  for (const Plan& plan : plans) {
    const Object* o = document_.find(plan.id);
    // 2. Lines: just the selected edges go; the rest stays as lines.
    if (o && o->kind() == ObjectKind::Curve) {
      TopTools_IndexedMapOfShape edges;
      TopExp::MapShapes(document_.worldShape(*o), TopAbs_EDGE, edges);
      std::vector<TopoDS_Edge> rest;
      int hit = 0;
      for (int i = 1; i <= edges.Extent(); ++i) {
        const TopoDS_Edge e = TopoDS::Edge(edges(i));
        const bool chosen =
            std::any_of(plan.edges.begin(), plan.edges.end(), [&](const EdgeKey& k) { return k.matches(e); });
        hit += chosen;
        if (!chosen) rest.push_back(e);
      }
      if (hit == 0) return fail("The selected line of '" + plan.name + "' was not found any more; select again");
      const std::string layer = o->layer;
      if (const Status s = remove({plan.id}); !s) return fail(s.message());
      outcome.removed.push_back(plan.id);
      std::vector<ObjectId> made;
      for (const auto& run : chainRuns(rest)) {
        if (const Status s = recreateCurves(*this, run, made); !s) return fail(s.message());
      }
      for (const ObjectId& line : made) {
        if (!layer.empty()) {
          if (const Status s = setLayer(line, layer); !s) return fail(s.message());
        }
      }
      outcome.lines.insert(outcome.lines.end(), made.begin(), made.end());
      continue;
    }
    // 3. Lines dividing a flat face: the faces join (if that is all the
    //    edge does; a selected face beside it goes below instead).
    std::vector<EdgeKey> bounding;
    for (const EdgeKey& key : plan.edges) {
      auto found = findEdge(plan, key);
      if (!found) continue;  // went with an earlier one
      const auto beside = facesAtEdge(found->first, found->second);
      const bool besideSelected = std::any_of(beside.begin(), beside.end(), [&](const TopoDS_Face& f) {
        return std::any_of(plan.faces.begin(), plan.faces.end(), [&](const FaceKey& k) { return k.matches(f); });
      });
      bool dividing = false;
      if (beside.size() == 2 && !besideSelected) {
        const auto n0 = geom::planarFaceNormal(beside[0]);
        const auto n1 = geom::planarFaceNormal(beside[1]);
        dividing = n0 && n1 && n0->Angle(*n1) < 1e-6;
      }
      const Object* owner = document_.find(found->first);
      const bool facePart = owner && std::holds_alternative<FacePart>(owner->recipe.base);
      if (dividing || (facePart && !besideSelected)) {
        auto merged = eraseEdge(found->first, found->second);
        if (merged) {
          ++outcome.merged;
          continue;
        }
        // Taking it away as a boundary would delete both faces: refused.
        if (dividing) return fail("'" + plan.name + "': " + merged.error());
      }
      bounding.push_back(key);
    }
    // 4. Faces, and the faces that selected edges bound.
    if (plan.faces.empty() && bounding.empty()) continue;
    std::map<ObjectId, std::pair<std::vector<TopoDS_Face>, std::vector<TopoDS_Edge>>> work;
    for (const FaceKey& key : plan.faces) {
      auto found = findFace(plan, key);
      if (!found) continue;  // went with an earlier one
      work[found->first].first.push_back(found->second);
    }
    for (const EdgeKey& key : bounding) {
      auto found = findEdge(plan, key);
      if (!found) continue;
      work[found->first].second.push_back(found->second);
    }
    for (const auto& [id, elements] : work) {
      const bool existed = document_.find(id) != nullptr;
      auto lines = deleteFaces(id, elements.first, elements.second);
      if (!lines) return fail("'" + plan.name + "': " + lines.error());
      outcome.lines.insert(outcome.lines.end(), lines->begin(), lines->end());
      if (existed && !document_.find(id)) outcome.removed.push_back(id);
    }
  }
  history_.endGroup();
  return outcome;
}

namespace {

// The transform maps everything onto itself (a whole number of turns,
// no shift), within the model tolerance at 100 m from the axis.
bool landsOnItself(const gp_Trsf& t) {
  for (const gp_Pnt& p : {gp_Pnt(0, 0, 0), gp_Pnt(1e5, 0, 0), gp_Pnt(0, 1e5, 0), gp_Pnt(0, 0, 1e5)}) {
    if (p.Transformed(t).Distance(p) > tolerance::kMinDimension) return false;
  }
  return true;
}

}  // namespace

Result<std::vector<ObjectId>> Modeler::copyEdges(const std::vector<TopoDS_Edge>& worldEdges,
                                                 const gp_Trsf& worldDelta, int count) {
  using R = Result<std::vector<ObjectId>>;
  if (worldEdges.empty()) return R::failure("No edges to copy");
  if (count < 1 || count > 10000) return R::failure("Count must be between 1 and 10000");
  // Straight edges already there (this group, shown): a copy landing on one
  // is not made again.
  auto onExisting = [&](const TopoDS_Edge& e) {
    if (BRepAdaptor_Curve(e).GetType() != GeomAbs_Line) return false;
    const gp_Pnt a = BRep_Tool::Pnt(TopExp::FirstVertex(e)), b = BRep_Tool::Pnt(TopExp::LastVertex(e));
    for (const Object& o : document_.objects()) {
      if (o.parent != activeGroup_ || !document_.isVisible(o.id) || o.shape.IsNull()) continue;
      const ObjectKind k = o.kind();
      if (k != ObjectKind::Curve && k != ObjectKind::Face && k != ObjectKind::Shell && k != ObjectKind::Solid) continue;
      TopTools_IndexedMapOfShape edges;
      TopExp::MapShapes(document_.worldShape(o), TopAbs_EDGE, edges);
      for (int i = 1; i <= edges.Extent(); ++i) {
        const TopoDS_Edge x = TopoDS::Edge(edges(i));
        const gp_Pnt c = BRep_Tool::Pnt(TopExp::FirstVertex(x)), d = BRep_Tool::Pnt(TopExp::LastVertex(x));
        if ((a.Distance(c) < 1e-6 && b.Distance(d) < 1e-6) || (a.Distance(d) < 1e-6 && b.Distance(c) < 1e-6)) {
          return true;
        }
      }
    }
    return false;
  };
  history_.beginGroup("Copy");
  std::vector<ObjectId> made;
  gp_Trsf at;
  for (int k = 1; k <= count; ++k) {
    at = worldDelta * at;
    if (landsOnItself(at)) continue;
    std::vector<TopoDS_Edge> moved;
    for (const TopoDS_Edge& e : worldEdges) {
      const TopoDS_Edge copy = TopoDS::Edge(BRepBuilderAPI_Transform(e, at, Standard_True).Shape());
      if (!onExisting(copy)) moved.push_back(copy);
    }
    for (const auto& run : chainRuns(moved)) {
      // A straight run across a face divides it (no line of its own then).
      const bool straight = std::all_of(run.begin(), run.end(), [](const TopoDS_Edge& e) {
        return BRepAdaptor_Curve(e).GetType() == GeomAbs_Line;
      });
      if (straight) {
        std::vector<gp_Pnt> points{BRep_Tool::Pnt(TopExp::FirstVertex(run.front(), true))};
        for (const TopoDS_Edge& e : run) points.push_back(BRep_Tool::Pnt(TopExp::LastVertex(e, true)));
        // A closed flat outline (e.g. a face's edges copied): a face, as
        // drawing it would make (SketchUp) - unless its sides run across
        // faces there (a top face's edges copied down a box): then they
        // divide those faces, side by side, and no face fills the outline.
        if (points.size() >= 4 && points.front().Distance(points.back()) < 1e-6) {
          int divided = 0;
          std::vector<std::vector<gp_Pnt>> rest;
          for (std::size_t i = 1; i < points.size(); ++i) {
            auto d = splitAcross({points[i - 1], points[i]});
            if (!d) {
              history_.abortGroup();
              return R::failure(d.error());
            }
            divided += d->faces;
            rest.insert(rest.end(), d->rest.begin(), d->rest.end());
          }
          if (divided > 0) {
            for (const std::vector<gp_Pnt>& part : rest) {
              auto id = createPolyline(part);
              if (!id) {
                history_.abortGroup();
                return R::failure(id.error());
              }
              made.push_back(*id);
            }
            continue;
          }
          points.pop_back();
          if (auto face = createPolygon(points)) {
            made.push_back(*face);
            continue;
          }
          points.push_back(points.front());
        }
        if (splitWithLine(points)) continue;
      }
      if (const Status s = recreateCurves(*this, run, made); !s) {
        history_.abortGroup();
        return R::failure(s.message());
      }
    }
  }
  // Partly on lines already there: only the new pieces stay.
  auto settled = settleOverlaps(made);
  if (!settled) {
    history_.abortGroup();
    return R::failure(settled.error());
  }
  history_.endGroup();
  return settledIds(made, *settled);
}

// --- overlaps -----------------------------------------------------------------------

namespace {

struct Segment {
  gp_Pnt a, b;
};

void collectEdges(const TopoDS_Shape& world, std::vector<Segment>& straight, std::vector<TopoDS_Edge>& curved) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(world, TopAbs_EDGE, edges);
  for (int i = 1; i <= edges.Extent(); ++i) {
    const TopoDS_Edge e = TopoDS::Edge(edges(i));
    if (BRep_Tool::Degenerated(e)) continue;
    if (BRepAdaptor_Curve(e).GetType() == GeomAbs_Line) {
      straight.push_back({BRep_Tool::Pnt(TopExp::FirstVertex(e)), BRep_Tool::Pnt(TopExp::LastVertex(e))});
    } else {
      curved.push_back(e);
    }
  }
}

gp_Pnt midOf(const TopoDS_Edge& e) {
  BRepAdaptor_Curve c(e);
  return c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
}

// The same curved edge (ends either way round and the middle).
bool sameCurvedEdge(const TopoDS_Edge& x, const TopoDS_Edge& y, double tol) {
  const gp_Pnt a = BRep_Tool::Pnt(TopExp::FirstVertex(x)), b = BRep_Tool::Pnt(TopExp::LastVertex(x));
  const gp_Pnt c = BRep_Tool::Pnt(TopExp::FirstVertex(y)), d = BRep_Tool::Pnt(TopExp::LastVertex(y));
  const bool ends = (a.Distance(c) < tol && b.Distance(d) < tol) || (a.Distance(d) < tol && b.Distance(c) < tol);
  return ends && midOf(x).Distance(midOf(y)) < tol;
}

// The parts of [0, length] no interval in `covers` takes (each > tol).
std::vector<std::pair<double, double>> uncovered(double length, std::vector<std::pair<double, double>> covers,
                                                 double tol) {
  std::sort(covers.begin(), covers.end());
  std::vector<std::pair<double, double>> out;
  double at = 0.0;
  for (const auto& [lo, hi] : covers) {
    if (lo - at > tol) out.emplace_back(at, lo);
    at = std::max(at, hi);
  }
  if (length - at > tol) out.emplace_back(at, length);
  return out;
}

// The one flat face of a shape (a face object), else null.
TopoDS_Face lonePlanarFace(const TopoDS_Shape& shape, gp_Pln* plane) {
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  if (faces.Extent() != 1) return {};
  const TopoDS_Face face = TopoDS::Face(faces(1));
  if (!geom::isPlanarFace(face, plane)) return {};
  return face;
}

// Faces of a boolean result worth keeping, turned to face along `normal`.
std::vector<TopoDS_Face> regionsOf(const TopoDS_Shape& shape, const gp_Dir& normal, double minArea) {
  std::vector<TopoDS_Face> out;
  if (shape.IsNull()) return out;
  for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
    TopoDS_Face face = TopoDS::Face(f.Current());
    if (geom::area(face) < minArea) continue;
    const auto n = geom::planarFaceNormal(face);
    if (n && n->Dot(normal) < 0) face = TopoDS::Face(face.Reversed());
    out.push_back(TopoDS::Face(BRepBuilderAPI_Copy(face).Shape()));
  }
  return out;
}

TopoDS_Shape faceBoolean(bool common, const TopoDS_Shape& a, const TopoDS_Shape& b) {
  TopTools_ListOfShape args, tools;
  args.Append(a);
  tools.Append(b);
  std::unique_ptr<BRepAlgoAPI_BooleanOperation> op;
  if (common) {
    op = std::make_unique<BRepAlgoAPI_Common>();
  } else {
    op = std::make_unique<BRepAlgoAPI_Cut>();
  }
  op->SetArguments(args);
  op->SetTools(tools);
  op->SetFuzzyValue(tolerance::kMinDimension);
  op->Build();
  if (!op->IsDone() || op->HasErrors()) return {};
  return op->Shape();
}

}  // namespace

Result<Modeler::Divided> Modeler::splitAcross(const std::vector<gp_Pnt>& worldPoints) {
  constexpr double tol = tolerance::kMinDimension;
  Divided out;
  if (worldPoints.size() < 2) return out;
  history_.beginGroup("Split Face");
  // The whole line on one face, from edge to edge: as before.
  if (splitWithLine(worldPoints)) {
    out.faces = 1;
    history_.endGroup();
    return out;
  }
  std::vector<gp_Pnt> rest;
  auto flushRest = [&] {
    if (rest.size() >= 2) out.rest.push_back(rest);
    rest.clear();
  };
  auto keep = [&](const gp_Pnt& p, const gp_Pnt& q) {
    if (p.Distance(q) < tol) return;
    if (rest.empty() || rest.back().Distance(p) > 1e-6) {
      flushRest();
      rest.push_back(p);
    }
    rest.push_back(q);
  };
  for (std::size_t k = 1; k < worldPoints.size(); ++k) {
    const gp_Pnt a = worldPoints[k - 1], b = worldPoints[k];
    const double length = a.Distance(b);
    if (length < tol) continue;
    const gp_Dir d(gp_Vec(a, b));
    const TopoDS_Edge segment = BRepBuilderAPI_MakeEdge(a, b).Edge();
    Bnd_Box reach;
    BRepBndLib::Add(segment, reach);
    reach.Enlarge(tol);
    // The parts of the segment running across a face of this group from
    // edge to edge, on the face's own plane (found first, then applied one
    // at a time: dividing changes the shapes).
    std::vector<std::pair<double, double>> portions;
    for (int pass = 0; pass < 2; ++pass) {
      for (const Object& o : document_.objects()) {
        if (o.parent != activeGroup_ || !document_.isVisible(o.id) || o.locked) continue;
        const bool profile = o.kind() == ObjectKind::Face;
        if ((pass == 0) != profile || (!profile && o.kind() != ObjectKind::Solid)) continue;
        const TopoDS_Shape shape = document_.worldShape(o);
        Bnd_Box box;
        BRepBndLib::Add(shape, box);
        if (box.IsOut(reach)) continue;
        for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
          const TopoDS_Face face = TopoDS::Face(e.Current());
          gp_Pln plane;
          if (!geom::isPlanarFace(face, &plane) || plane.Distance(a) > tol || plane.Distance(b) > tol) continue;
          BRepAlgoAPI_Common common(segment, face);
          common.SetFuzzyValue(tol);
          common.Build();
          if (!common.IsDone() || common.HasErrors()) continue;
          for (TopExp_Explorer pe(common.Shape(), TopAbs_EDGE); pe.More(); pe.Next()) {
            const TopoDS_Edge piece = TopoDS::Edge(pe.Current());
            const gp_Pnt p0 = BRep_Tool::Pnt(TopExp::FirstVertex(piece)), p1 = BRep_Tool::Pnt(TopExp::LastVertex(piece));
            double t0 = gp_Vec(a, p0).Dot(gp_Vec(d)), t1 = gp_Vec(a, p1).Dot(gp_Vec(d));
            if (t0 > t1) std::swap(t0, t1);
            if (t1 - t0 < tol) continue;
            const gp_Pnt mid = a.Translated(gp_Vec(d) * (0.5 * (t0 + t1)));
            // Across the face (not along its edge), and from edge to edge
            // (a part stopping inside the face is not extended).
            if (geom::pointOnFaceBoundary(face, mid) || !geom::pointOnFaceBoundary(face, p0) ||
                !geom::pointOnFaceBoundary(face, p1)) {
              continue;
            }
            const bool known = std::any_of(portions.begin(), portions.end(), [&](const auto& q) {
              return std::abs(q.first - t0) < tol && std::abs(q.second - t1) < tol;
            });
            if (!known) portions.emplace_back(t0, t1);
          }
        }
      }
    }
    std::sort(portions.begin(), portions.end());
    std::vector<std::pair<double, double>> used, failed;
    auto at = [&](double t) { return a.Translated(gp_Vec(d) * t); };
    for (const auto& [t0, t1] : portions) {
      if (splitWithLine({at(t0), at(t1)})) {
        used.emplace_back(t0, t1);
        ++out.faces;
      } else {
        failed.emplace_back(t0, t1);
      }
    }
    // Parts that divide nothing alone, of one and the same face (on both
    // sides of a hole): together they do, as one line across the hole.
    auto faceAt = [&](const gp_Pnt& p) -> std::pair<ObjectId, TopoDS_Face> {
      for (const Object& o : document_.objects()) {
        if (o.parent != activeGroup_ || !document_.isVisible(o.id) || o.locked) continue;
        if (o.kind() != ObjectKind::Face && o.kind() != ObjectKind::Solid) continue;
        for (TopExp_Explorer e(document_.worldShape(o), TopAbs_FACE); e.More(); e.Next()) {
          const TopoDS_Face f = TopoDS::Face(e.Current());
          if (geom::pointOnFace(f, p) && !geom::pointOnFaceBoundary(f, p)) return {o.id, f};
        }
      }
      return {};
    };
    for (std::size_t i = 0; i + 1 < failed.size(); ++i) {
      const auto first = faceAt(at(0.5 * (failed[i].first + failed[i].second)));
      const auto second = faceAt(at(0.5 * (failed[i + 1].first + failed[i + 1].second)));
      if (first.first.empty() || first.first != second.first || !first.second.IsSame(second.second)) continue;
      if (splitWithLine({at(failed[i].first), at(failed[i + 1].second)})) {
        used.emplace_back(failed[i].first, failed[i].second);
        used.emplace_back(failed[i + 1].first, failed[i + 1].second);
        ++out.faces;
        ++i;
      }
    }
    if (used.empty()) {
      keep(a, b);
      continue;
    }
    for (const auto& [s0, s1] : uncovered(length, used, tol)) {
      keep(a.Translated(gp_Vec(d) * s0), a.Translated(gp_Vec(d) * s1));
    }
  }
  flushRest();
  // Nothing divided: nothing was done either. (abortGroup would take back
  // the caller's whole step, this group being inside it.)
  history_.endGroup();
  if (out.faces == 0) out.rest = {worldPoints};
  return out;
}

Status Modeler::divideWithLine(const ObjectId& lineId, std::vector<ObjectId>& now) {
  const Object* found = document_.find(lineId);
  if (!found) return Status::success();
  const Object line = *found;
  const std::vector<gp_Pnt> points = worldPolyline(document_, line);
  if (points.size() < 2) return Status::success();
  auto divided = splitAcross(points);
  if (!divided) return divided.status();
  if (divided->faces == 0) return Status::success();
  if (const Status s = remove({lineId}); !s) return s;
  now.erase(std::remove(now.begin(), now.end(), lineId), now.end());
  for (const std::vector<gp_Pnt>& part : divided->rest) {
    auto id = createPolyline(part);
    if (!id) return id.status();
    Object o = *document_.find(*id);
    o.layer = line.layer;
    o.material = line.material;
    o.color = line.color;
    o.attributes = line.attributes;
    if (const Status s = commitModified(std::move(o), "Split Face"); !s) return s;
    now.push_back(*id);
  }
  return Status::success();
}

std::vector<ObjectId> Modeler::settledIds(const std::vector<ObjectId>& ids, const Settled& settled) {
  std::vector<ObjectId> out;
  auto add = [&](const ObjectId& id) {
    if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
  };
  for (const ObjectId& id : ids) {
    const auto it = settled.became.find(id);
    if (it == settled.became.end()) {
      add(id);
    } else {
      for (const ObjectId& now : it->second) add(now);
    }
  }
  return out;
}

Status Modeler::closeIntoSolid(const ObjectId& id, Settled& out) {
  const Object* start = document_.find(id);
  if (!start || start->locked || !document_.isVisible(id)) return Status::success();
  if (start->kind() != ObjectKind::Face && start->kind() != ObjectKind::Shell) return Status::success();
  const ObjectId parent = start->parent;
  // Faces and open shells of the same group. Their edges (ends and middle)
  // are read only for pieces reached so far and the ones whose box touches
  // them, so drawing on a large floor does not read the whole floor.
  struct Piece {
    const Object* object;
    Bnd_Box box;
    bool read = false;
    std::vector<std::array<gp_Pnt, 3>> edges;
    std::vector<std::optional<gp_Pln>> planes;  // of the face along each edge
  };
  constexpr double tol = geom::kSewTolerance;
  std::vector<Piece> pieces;
  for (const Object& o : document_.objects()) {
    if (o.parent != parent || o.locked || !document_.isVisible(o.id) || o.shape.IsNull()) continue;
    if (o.kind() != ObjectKind::Face && o.kind() != ObjectKind::Shell) continue;
    Piece piece{&o, {}, false, {}, {}};
    BRepBndLib::Add(document_.worldShape(o), piece.box);
    piece.box.Enlarge(tol);
    pieces.push_back(std::move(piece));
  }
  // Edges by their middle on a grid.
  struct EdgeAt {
    std::size_t piece;
    std::size_t edge;
  };
  constexpr double cell = 1.0;  // mm
  using Key = std::array<long long, 3>;
  auto keyOf = [&](const gp_Pnt& p) {
    return Key{std::llround(std::floor(p.X() / cell)), std::llround(std::floor(p.Y() / cell)),
               std::llround(std::floor(p.Z() / cell))};
  };
  std::map<Key, std::vector<EdgeAt>> grid;
  auto read = [&](std::size_t i) {
    Piece& piece = pieces[i];
    if (piece.read) return;
    piece.read = true;
    // Its free edges (an open shell's edges between two of its own faces
    // are joined already).
    TopTools_IndexedDataMapOfShapeListOfShape edges;
    TopExp::MapShapesAndAncestors(document_.worldShape(*piece.object), TopAbs_EDGE, TopAbs_FACE, edges);
    for (int k = 1; k <= edges.Extent(); ++k) {
      const TopoDS_Edge e = TopoDS::Edge(edges.FindKey(k));
      if (BRep_Tool::Degenerated(e) || edges(k).Extent() > 1) continue;
      BRepAdaptor_Curve c(e);
      piece.edges.push_back({c.Value(c.FirstParameter()), c.Value(0.5 * (c.FirstParameter() + c.LastParameter())),
                             c.Value(c.LastParameter())});
      gp_Pln plane;
      piece.planes.push_back(geom::isPlanarFace(TopoDS::Face(edges(k).First()), &plane) ? std::optional(plane)
                                                                                          : std::nullopt);
      grid[keyOf(piece.edges.back()[1])].push_back({i, piece.edges.size() - 1});
    }
  };
  // Before looking for a piece's partners: everything that could touch it.
  auto readAround = [&](std::size_t i) {
    read(i);
    for (std::size_t k = 0; k < pieces.size(); ++k) {
      if (!pieces[k].read && !pieces[k].box.IsOut(pieces[i].box)) read(k);
    }
  };
  auto same = [&](const std::array<gp_Pnt, 3>& x, const std::array<gp_Pnt, 3>& y) {
    if (x[1].Distance(y[1]) > tol) return false;
    return (x[0].Distance(y[0]) < tol && x[2].Distance(y[2]) < tol) ||
           (x[0].Distance(y[2]) < tol && x[2].Distance(y[0]) < tol);
  };
  // Two faces meeting at an edge on one plane (side by side, not at an
  // angle): not joined while open (SketchUp joins them too; later stage).
  auto flat = [&](const std::optional<gp_Pln>& a, const std::optional<gp_Pln>& b) {
    return a && b && a->Axis().Direction().IsParallel(b->Axis().Direction(), 1e-9) &&
           a->Distance(b->Location()) < tol;
  };
  // The other pieces sharing this edge (only at an angle: `angled`).
  auto partners = [&](std::size_t piece, std::size_t edge, bool angled) {
    std::vector<std::size_t> out;
    const auto& x = pieces[piece].edges[edge];
    const Key k = keyOf(x[1]);
    for (long long dx = -1; dx <= 1; ++dx) {
      for (long long dy = -1; dy <= 1; ++dy) {
        for (long long dz = -1; dz <= 1; ++dz) {
          const auto it = grid.find(Key{k[0] + dx, k[1] + dy, k[2] + dz});
          if (it == grid.end()) continue;
          for (const EdgeAt& at : it->second) {
            if (at.piece == piece || !same(x, pieces[at.piece].edges[at.edge])) continue;
            if (angled && flat(pieces[piece].planes[edge], pieces[at.piece].planes[at.edge])) continue;
            out.push_back(at.piece);
          }
        }
      }
    }
    return out;
  };
  // Everything joined to the start, edge by edge. With `open`: stops at the
  // first edge nobody shares (then no volume is closed).
  auto reach = [&](bool angled, bool* open) {
    std::vector<std::size_t> joined;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
      if (pieces[i].object->id == id) joined.push_back(i);
    }
    for (std::size_t k = 0; k < joined.size(); ++k) {
      const std::size_t piece = joined[k];
      readAround(piece);
      for (std::size_t e = 0; e < pieces[piece].edges.size(); ++e) {
        const auto others = partners(piece, e, angled);
        if (others.empty() && open) {
          *open = true;
          return joined;
        }
        for (std::size_t other : others) {
          if (std::find(joined.begin(), joined.end(), other) == joined.end()) joined.push_back(other);
        }
      }
    }
    std::sort(joined.begin(), joined.end());  // in the order they were drawn
    return joined;
  };
  const gp_Trsf toParent =
      parent.empty() ? gp_Trsf() : document_.worldTransform(*document_.find(parent)).Inverted();
  auto placedOf = [&](const std::vector<std::size_t>& joined) {
    std::vector<TopoDS_Shape> placed;
    for (std::size_t i : joined) {
      placed.push_back(pieces[i].object->shape.Moved(TopLoc_Location(toParent * document_.worldTransform(*pieces[i].object))));
    }
    return placed;
  };
  // Closing a volume (every edge shared - else no need to try): a solid.
  // Otherwise the faces meeting at an angle share their edges as one open
  // shape (a box being drawn side by side), never a face twice or a fin.
  bool open = false;
  std::vector<std::size_t> joined = reach(false, &open);
  if (open || joined.size() < 2 || !geom::sewIntoSolid(placedOf(joined))) {
    joined = reach(true, nullptr);
    if (joined.size() < 2) return Status::success();
  }
  SewnFeature feature;
  std::vector<std::pair<const Object*, gp_Trsf>> inputs;
  for (std::size_t i : joined) {
    const Object* o = pieces[i].object;
    const gp_Trsf at = toParent * document_.worldTransform(*o);
    inputs.emplace_back(o, at);
    // Faces joined before: their own faces again (one sewing, not nested).
    const auto* sewn = std::get_if<SewnFeature>(&o->recipe.base);
    if (sewn && o->recipe.modifiers.empty()) {
      for (const BooleanOperand& f : sewn->faces) feature.faces.push_back(BooleanOperand{f.recipe, at * f.placement, f.name});
      continue;
    }
    feature.faces.push_back(BooleanOperand{std::make_shared<const Recipe>(o->recipe), at, o->name});
  }
  Object result;
  result.id = generateObjectId();
  result.parent = parent;
  const Object& first = *pieces[joined.front()].object;
  result.layer = first.layer;
  result.color = first.color;
  result.attributes = first.attributes;
  result.recipe.base = std::move(feature);
  if (const Status built = rebuild(result, document_.regenContext()); !built) return Status::success();
  result.name = uniqueName(result.kind() == ObjectKind::Solid ? "Solid" : "Surface");
  carryMaterials(result, inputs, true);
  ChangeSet changes(document_);
  for (std::size_t i : joined) {
    const ObjectId gone = pieces[i].object->id;
    changes.erase(gone);
    // What became this piece earlier in the settling became the solid too.
    for (auto& [was, now] : out.became) {
      for (ObjectId& n : now) {
        if (n == gone) n = result.id;
      }
      now.erase(std::unique(now.begin(), now.end()), now.end());
    }
    out.became[gone] = {result.id};
  }
  changes.add(result);
  history_.execute(changes.build(result.kind() == ObjectKind::Solid ? "Close Into Solid" : "Join Faces"));
  return Status::success();
}

Result<Modeler::Settled> Modeler::settleOverlaps(const std::vector<ObjectId>& ids) {
  using R = Result<Settled>;
  Settled out;
  if (settling_) return out;
  settling_ = true;
  struct Reset {
    bool& flag;
    ~Reset() { flag = false; }
  } reset{settling_};
  history_.beginGroup("Tidy");
  for (const ObjectId& id : ids) {
    const Object* o = document_.find(id);
    if (!o || o->locked || !document_.isVisible(id)) continue;
    const ObjectKind kind = o->kind();
    Status s = Status::success();
    if (kind == ObjectKind::Curve) s = settleLine(id, out);
    if (kind == ObjectKind::Face) s = settleFace(id, out);
    if (!s) {
      history_.abortGroup();
      return R::failure(s.message());
    }
  }
  // Faces closing a volume with the ones already there: one solid.
  for (const ObjectId& id : settledIds(ids, out)) {
    if (const Status s = closeIntoSolid(id, out); !s) {
      history_.abortGroup();
      return R::failure(s.message());
    }
  }
  history_.endGroup();
  return out;
}

Status Modeler::settleLine(const ObjectId& id, Settled& out) {
  constexpr double tol = tolerance::kMinDimension;
  const Object line = *document_.find(id);
  const TopoDS_Shape world = document_.worldShape(line);
  Bnd_Box reach;
  BRepBndLib::Add(world, reach);
  reach.Enlarge(10 * tol);
  // Edges already there nearby: lines, and faces' / solids' edges.
  std::vector<Segment> straight;
  std::vector<TopoDS_Edge> curved;
  std::vector<ObjectId> loose;
  for (const Object& o : document_.objects()) {
    if (o.id == id || o.parent != line.parent || !document_.isVisible(o.id) || o.shape.IsNull()) continue;
    const ObjectKind k = o.kind();
    if (k != ObjectKind::Curve && k != ObjectKind::Face && k != ObjectKind::Shell && k != ObjectKind::Solid) continue;
    const TopoDS_Shape shape = document_.worldShape(o);
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    if (box.IsOut(reach)) continue;
    collectEdges(shape, straight, curved);
    if (k == ObjectKind::Curve && !o.locked) loose.push_back(o.id);
  }
  // The pieces of the line not lying on one of them.
  std::vector<TopoDS_Edge> kept;
  bool trimmed = false;
  for (const TopoDS_Edge& e : chainOf(world)) {
    const gp_Pnt a = BRep_Tool::Pnt(TopExp::FirstVertex(e, true));
    const gp_Pnt b = BRep_Tool::Pnt(TopExp::LastVertex(e, true));
    if (BRepAdaptor_Curve(e).GetType() != GeomAbs_Line) {
      if (std::any_of(curved.begin(), curved.end(), [&](const TopoDS_Edge& c) { return sameCurvedEdge(e, c, tol); })) {
        trimmed = true;
      } else {
        kept.push_back(e);
      }
      continue;
    }
    const double length = a.Distance(b);
    if (length < tol) continue;
    const gp_Dir d(gp_Vec(a, b));
    const gp_Lin axis(a, d);
    std::vector<std::pair<double, double>> covers;
    for (const Segment& s : straight) {
      if (axis.Distance(s.a) > tol || axis.Distance(s.b) > tol) continue;
      const double t0 = gp_Vec(a, s.a).Dot(gp_Vec(d)), t1 = gp_Vec(a, s.b).Dot(gp_Vec(d));
      const double lo = std::max(0.0, std::min(t0, t1)), hi = std::min(length, std::max(t0, t1));
      if (hi - lo > tol) covers.emplace_back(lo, hi);
    }
    if (covers.empty()) {
      kept.push_back(e);
      continue;
    }
    trimmed = true;
    for (const auto& [t0, t1] : uncovered(length, covers, tol)) {
      kept.push_back(BRepBuilderAPI_MakeEdge(a.Translated(gp_Vec(d) * t0), a.Translated(gp_Vec(d) * t1)).Edge());
    }
  }
  std::vector<ObjectId> now{id};
  if (trimmed) {
    if (const Status s = remove({id}); !s) return s;
    now.clear();
    for (const auto& run : chainRuns(kept)) {
      if (const Status s = recreateCurves(*this, run, now); !s) return s;
    }
    for (const ObjectId& n : now) {
      Object o = *document_.find(n);
      o.layer = line.layer;
      o.material = line.material;
      o.color = line.color;
      o.attributes = line.attributes;
      if (const Status s = commitModified(std::move(o), "Tidy"); !s) return s;
    }
    out.became[id] = now;
    ++out.linesTrimmed;
  }
  // Running across faces of this group from edge to edge: they are divided
  // there (as drawing it across them does), whatever made the line (drawn,
  // copied, moved, pasted); only the parts not used stay a line.
  {
    const std::vector<ObjectId> before = now;
    for (const ObjectId& n : before) {
      if (const Status s = divideWithLine(n, now); !s) return s;
    }
    if (now != before) out.became[id] = now;
  }
  // Where the line ends on another loose line, that one gets a vertex.
  std::vector<gp_Pnt> ends;
  for (const ObjectId& n : now) {
    TopTools_IndexedMapOfShape vertices;
    TopExp::MapShapes(document_.worldShape(*document_.find(n)), TopAbs_VERTEX, vertices);
    for (int i = 1; i <= vertices.Extent(); ++i) ends.push_back(BRep_Tool::Pnt(TopoDS::Vertex(vertices(i))));
  }
  for (const ObjectId& other : loose) {
    const Object* o = document_.find(other);
    if (!o) continue;
    const Object before = *o;
    std::vector<TopoDS_Edge> pieces;
    bool split = false;
    for (const TopoDS_Edge& e : chainOf(document_.worldShape(before))) {
      const gp_Pnt a = BRep_Tool::Pnt(TopExp::FirstVertex(e, true));
      const gp_Pnt b = BRep_Tool::Pnt(TopExp::LastVertex(e, true));
      if (BRepAdaptor_Curve(e).GetType() != GeomAbs_Line || a.Distance(b) < tol) {
        pieces.push_back(e);
        continue;
      }
      const double length = a.Distance(b);
      const gp_Dir d(gp_Vec(a, b));
      const gp_Lin axis(a, d);
      std::vector<double> cuts;
      for (const gp_Pnt& p : ends) {
        const double t = gp_Vec(a, p).Dot(gp_Vec(d));
        if (t > tol && t < length - tol && axis.Distance(p) < tol) cuts.push_back(t);
      }
      if (cuts.empty()) {
        pieces.push_back(e);
        continue;
      }
      split = true;
      std::sort(cuts.begin(), cuts.end());
      double from = 0.0;
      cuts.push_back(length);
      for (double t : cuts) {
        if (t - from > tol) {
          pieces.push_back(
              BRepBuilderAPI_MakeEdge(a.Translated(gp_Vec(d) * from), a.Translated(gp_Vec(d) * t)).Edge());
        }
        from = t;
      }
    }
    if (!split) continue;
    if (const Status s = remove({other}); !s) return s;
    std::vector<ObjectId> made;
    for (const auto& run : chainRuns(pieces)) {
      // Each piece a line of its own: the vertex is where they meet.
      for (const TopoDS_Edge& e : run) {
        if (const Status s = recreateCurves(*this, {e}, made); !s) return s;
      }
    }
    for (const ObjectId& n : made) {
      Object m = *document_.find(n);
      m.layer = before.layer;
      m.material = before.material;
      m.color = before.color;
      m.attributes = before.attributes;
      if (const Status s = commitModified(std::move(m), "Tidy"); !s) return s;
    }
    ++out.linesSplit;
  }
  return Status::success();
}

Result<std::vector<ObjectId>> Modeler::replaceByFaces(const Object& base, const std::vector<TopoDS_Face>& worldFaces) {
  using R = Result<std::vector<ObjectId>>;
  std::vector<ObjectId> ids;
  const gp_Trsf toLocal = document_.worldTransform(base).Inverted();
  // A face painted on its own is the whole object here: its material goes
  // onto each region.
  std::string material = base.material;
  for (const auto& [index, id] : resolveFaceMaterials(base).faces) {
    if (index == 1) material = id;
  }
  for (std::size_t i = 0; i < worldFaces.size(); ++i) {
    Object o = base;
    o.recipe = Recipe{};
    o.recipe.base = ImportedShape{worldFaces[i].Moved(TopLoc_Location(toLocal)), "Region of " + base.name};
    o.material = material;
    o.faceMaterials.clear();
    o.hiddenEdges.clear();
    if (i == 0) {
      if (const Status s = commitModified(o, "Tidy"); !s) return R::failure(s.message());
      ids.push_back(o.id);
    } else {
      o.id = generateObjectId();
      o.name = uniqueName(base.name);
      auto added = addObject(std::move(o), "Tidy");
      if (!added) return R::failure(added.error());
      ids.push_back(*added);
    }
  }
  return ids;
}

Status Modeler::settleFace(const ObjectId& id, Settled& out) {
  constexpr double tol = tolerance::kMinDimension;
  const Object fresh = *document_.find(id);
  gp_Pln plane;
  const TopoDS_Face face = lonePlanarFace(document_.worldShape(fresh), &plane);
  if (face.IsNull()) return Status::success();
  const auto normal = geom::planarFaceNormal(face);
  if (!normal) return Status::success();
  const double minArea = tol * tol;
  Bnd_Box reach;
  BRepBndLib::Add(face, reach);
  reach.Enlarge(tol);
  // Faces already there on the same plane (either side), nearby.
  std::vector<ObjectId> others;
  for (const Object& o : document_.objects()) {
    if (o.id == id || o.parent != fresh.parent || !document_.isVisible(o.id) || o.kind() != ObjectKind::Face) continue;
    gp_Pln on;
    const TopoDS_Face f = lonePlanarFace(document_.worldShape(o), &on);
    if (f.IsNull() || !on.Axis().Direction().IsParallel(plane.Axis().Direction(), 1e-9) ||
        plane.Distance(on.Location()) > tol) {
      continue;
    }
    Bnd_Box box;
    BRepBndLib::Add(f, box);
    if (!box.IsOut(reach)) others.push_back(o.id);
  }
  std::vector<TopoDS_Face> parts{face};
  std::vector<ObjectId> takenBy;
  bool overlapped = false;
  for (const ObjectId& otherId : others) {
    const Object* other = document_.find(otherId);
    if (!other) continue;
    const Object existing = *other;
    const TopoDS_Face there = lonePlanarFace(document_.worldShape(existing), nullptr);
    if (there.IsNull()) continue;
    if (geom::area(faceBoolean(true, face, there)) < minArea) continue;  // touching at most
    overlapped = true;
    // The new face loses what is already there.
    std::vector<TopoDS_Face> rest;
    for (const TopoDS_Face& p : parts) {
      if (geom::area(faceBoolean(true, p, there)) < minArea) {
        rest.push_back(p);
        continue;
      }
      for (const TopoDS_Face& r : regionsOf(faceBoolean(false, p, there), *normal, minArea)) rest.push_back(r);
    }
    parts = rest;
    // The face there is divided along the new outline (keeping its side);
    // its regions under the new face are what the new face became.
    const auto thereNormal = geom::planarFaceNormal(there);
    std::vector<TopoDS_Face> regions;
    if (!existing.locked && thereNormal) regions = regionsOf(faceBoolean(false, there, face), *thereNormal, minArea);
    if (regions.empty()) {  // all of it under the new one (or locked): stays whole
      takenBy.push_back(otherId);
      continue;
    }
    const std::size_t outside = regions.size();
    for (const TopoDS_Face& r : regionsOf(faceBoolean(true, there, face), *thereNormal, minArea)) regions.push_back(r);
    auto replaced = replaceByFaces(existing, regions);
    if (!replaced) return replaced.status();
    takenBy.insert(takenBy.end(), replaced->begin() + static_cast<std::ptrdiff_t>(outside), replaced->end());
    ++out.facesDivided;
  }
  if (!overlapped) return divideWithLinesOn(fresh.parent, face, plane);
  if (parts.empty()) {
    if (const Status s = remove({id}); !s) return s;
    out.became[id] = takenBy;  // what was there takes its place
    return Status::success();
  }
  auto replaced = replaceByFaces(fresh, parts);
  if (!replaced) return replaced.status();
  out.became[id] = *replaced;
  ++out.facesDivided;
  return divideWithLinesOn(fresh.parent, face, plane);
}

Status Modeler::divideWithLinesOn(const ObjectId& parent, const TopoDS_Face& face, const gp_Pln& plane) {
  // Lines drawn first, the face after: loose lines lying on it across it
  // divide it the same way.
  constexpr double tol = tolerance::kMinDimension;
  Bnd_Box reach;
  BRepBndLib::Add(face, reach);
  reach.Enlarge(tol);
  std::vector<ObjectId> lines;
  for (const Object& o : document_.objects()) {
    if (o.parent != parent || o.kind() != ObjectKind::Curve || o.locked || !document_.isVisible(o.id)) continue;
    const std::vector<gp_Pnt> points = worldPolyline(document_, o);
    if (points.size() < 2) continue;
    if (std::any_of(points.begin(), points.end(), [&](const gp_Pnt& p) { return plane.Distance(p) > tol; })) continue;
    Bnd_Box box;
    for (const gp_Pnt& p : points) box.Add(p);
    if (!box.IsOut(reach)) lines.push_back(o.id);
  }
  for (const ObjectId& line : lines) {
    std::vector<ObjectId> now{line};
    if (const Status s = divideWithLine(line, now); !s) return s;
  }
  return Status::success();
}

namespace {
// Records a change of the persistent settings as one undo step (review A),
// or nothing if `edit` leaves them as they are.
void editSettings(Document& document, CommandStack& history, const std::string& name,
                  const std::function<void(PersistentSettings&)>& edit) {
  const PersistentSettings before = PersistentSettings::of(document.settings());
  PersistentSettings after = before;
  edit(after);
  if (after == before) return;
  history.execute(Command{name, {}, {}, {}, SettingsDelta{before, after}});
}
}  // namespace

void Modeler::setTagVisible(const std::string& tag, bool visible) {
  editSettings(document_, history_, visible ? "Show Tag" : "Hide Tag", [&](PersistentSettings& s) {
    if (visible) {
      s.hiddenTags.erase(tag);
    } else {
      s.hiddenTags.insert(tag);
    }
  });
}

void Modeler::setSectionVisible(bool visible) {
  if (document_.settings().sections.empty() || visible == sectionVisible()) return;
  editSettings(document_, history_, visible ? "Show Section" : "Hide Section", [&](PersistentSettings& s) {
    for (SectionPlane& p : s.sections) p.active = false;
    if (visible) s.sections.back().active = true;
  });
}

bool Modeler::sectionVisible() const {
  for (const SectionPlane& p : document_.settings().sections) {
    if (p.active) return true;
  }
  return false;
}

void Modeler::clearSections() {
  editSettings(document_, history_, "Clear Sections", [](PersistentSettings& s) { s.sections.clear(); });
}

std::vector<std::string> Modeler::tags() const {
  std::set<std::string> all;
  for (const Object& o : document_.objects()) all.insert(o.layer);
  for (const std::string& t : document_.settings().hiddenTags) all.insert(t);
  return {all.begin(), all.end()};
}

void Modeler::setSection(const std::optional<SectionPlane>& plane) {
  editSettings(document_, history_, plane ? "Section" : "Hide Section", [&](PersistentSettings& s) {
    for (SectionPlane& p : s.sections) p.active = false;
    if (plane) {
      SectionPlane p = *plane;
      if (p.name.empty()) p.name = "Section " + std::to_string(s.sections.size() + 1);
      p.active = true;
      s.sections.push_back(p);
    }
  });
}

Result<ObjectId> Modeler::createPolygon(const std::vector<gp_Pnt>& points, const std::string& name) {
  Object o;
  o.name = name.empty() ? uniqueName("Face") : name;
  // On a solid's face, face outwards like it (see faceFromLoop).
  std::vector<gp_Pnt> pts = points;
  if (auto face = geom::makePolygonFace(pts);
      face && facesIntoSolid(face.value(), activeGroup_)) {
    std::reverse(pts.begin(), pts.end());
  }
  o.recipe.base = PolygonProfile{pts};
  return addProfile(std::move(o), "Face");
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
                                         double distance, bool keepStartFace) {
  const Object* current = document_.find(id);
  if (!current) return Result<PushPullResult>::failure("Object not found");
  if (current->kind() == ObjectKind::Instance) {
    return Result<PushPullResult>::failure(
        "Component instances cannot be pushed/pulled; edit the definition or explode it");
  }
  if (!std::isfinite(distance) || std::abs(distance) < tolerance::kMinDimension) {
    return Result<PushPullResult>::failure("Distance must be non-zero");
  }
  // An open shape (faces drawn edge to edge, or a solid with faces
  // deleted): the face moves out with side faces back to where it was, as
  // on a SketchUp surface; closing it makes a solid.
  if (current->kind() == ObjectKind::Shell ||
      (current->kind() == ObjectKind::Face && current->shape.ShapeType() != TopAbs_FACE)) {
    // Ctrl (SketchUp: keep the starting face): a volume of its own instead.
    if (keepStartFace) return volumeFromOpenFace(id, worldFace, distance);
    Object object = *current;
    const TopoDS_Face face =
        TopoDS::Face(worldFace.Moved(TopLoc_Location(document_.worldTransform(object).Inverted())));
    if (!geom::planarFaceNormal(face)) {
      return Result<PushPullResult>::failure("Curved faces of open shapes cannot be pushed/pulled (only flat ones)");
    }
    auto ref = geom::FaceRef::identify(face);
    if (!ref) return Result<PushPullResult>::failure(ref.error());
    object.recipe.modifiers.push_back(PushPullOp{ref.value(), distance});
    const std::string key = opKey(object.recipe.modifiers.size() - 1);
    const Status committed = commitModified(std::move(object), "Push/Pull");
    if (!committed) return Result<PushPullResult>::failure(committed.message());
    return PushPullResult{id, key};
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

  // A face painted on its own moves with it (docs/11_materials.md).
  {
    gp_Trsf shift;
    shift.SetTranslation(gp_Vec(*pickedNormal) * distance);
    for (FaceMaterial& fm : object.faceMaterials) {
      if (!fm.plane) continue;
      const std::vector<TopoDS_Face> on = fm.facesIn(object.shape);
      if (std::any_of(on.begin(), on.end(), [&](const TopoDS_Face& f) { return f.TShape() == face.TShape(); })) {
        fm.plane = fm.plane->transformed(shift);
      }
    }
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
      auto ref = geom::FaceRef::identify(face);
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

Result<PushPullResult> Modeler::volumeFromOpenFace(const ObjectId& id, const TopoDS_Face& worldFace,
                                                   double distance) {
  using R = Result<PushPullResult>;
  const Object* shell = document_.find(id);
  if (!shell) return R::failure("Object not found");
  if (shell->locked) return R::failure("The object is locked");
  const auto normal = geom::planarFaceNormal(worldFace);
  if (!normal) {
    return R::failure("Only flat faces of an open shape can be pushed/pulled into a volume (this face is curved)");
  }
  // The face as it is in the open shape (its orientation is the shape's
  // outside): the volume grows that way for a positive distance.
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(document_.worldShape(*shell), TopAbs_FACE, faces);
  int index = faces.FindIndex(worldFace);
  for (int i = 1; i <= faces.Extent() && index == 0; ++i) {
    if (faces(i).TShape() == worldFace.TShape()) index = i;
  }
  if (index == 0) return R::failure("The face is not part of this open shape (select it again)");
  const TopoDS_Face face = TopoDS::Face(faces(index));
  const auto outward = geom::planarFaceNormal(face);
  if (!outward) return R::failure("The face is not flat");

  // The new volume: a profile from the face, extruded (the distance stays
  // editable, as for any push/pull). It is an object of its own beside the
  // open shape: a solid, the shape stays open.
  Object volume;
  volume.id = generateObjectId();
  volume.name = uniqueName("Volume");
  volume.parent = shell->parent;
  volume.layer = shell->layer;
  volume.color = shell->color;
  volume.material = shell->material;
  const gp_Trsf toParent = document_.parentTransform(*shell).Inverted();
  TopoDS_Face profile = TopoDS::Face(BRepBuilderAPI_Copy(face).Shape());
  profile = TopoDS::Face(profile.Moved(TopLoc_Location(toParent)));
  volume.recipe.base = ImportedShape{profile, "Face of " + shell->name};
  // The profile's own normal decides the extrusion side: follow the face's
  // outside for a positive distance.
  const auto profileNormal = geom::planarFaceNormal(profile);
  const double sign = profileNormal && profileNormal->Transformed(document_.parentTransform(*shell)).Dot(*outward) < 0
                          ? -1.0
                          : 1.0;
  volume.recipe.modifiers.push_back(ExtrudeOp{distance * sign});
  const std::string key = opKey(volume.recipe.modifiers.size() - 1);
  if (const Status built = rebuild(volume, document_.regenContext()); !built) {
    return R::failure("No volume can be made from this face: " + built.message());
  }
  if (volume.kind() != ObjectKind::Solid) return R::failure("No closed volume can be made from this face");

  // The face itself becomes the volume's face: taken out of the open shape
  // so that no two faces lie on top of each other. The shape keeps all its
  // other faces and stays open (nothing is filled).
  history_.beginGroup("Push/Pull");
  ChangeSet changes(document_);
  changes.add(volume);
  history_.execute(changes.build("Push/Pull"));
  if (faces.Extent() > 1) {
    if (const Status s = removeFaces(id, {worldFace}); !s) {
      history_.abortGroup();
      return R::failure(s.message());
    }
  } else if (const Status s = remove({id}); !s) {  // it was that face alone
    history_.abortGroup();
    return R::failure(s.message());
  }
  history_.endGroup();
  return PushPullResult{volume.id, key};
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
      rememberShape(feature.operands.back().recipe, host.shape);
      feature.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(extruded.recipe),
                                                toParent * document_.worldTransform(profileBefore),
                                                profileBefore.name});
      result.recipe.base = std::move(feature);
      if (!rebuild(result, document_.regenContext())) return std::nullopt;  // plain extrusion
      carryMaterials(result, {{&host, toParent * document_.worldTransform(host)},
                              {&profileBefore, toParent * document_.worldTransform(profileBefore)}});

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
  // The two outlines divide the plane (SketchUp): inwards, the original
  // face becomes the frame around the new one; outwards, the new face is
  // the frame around the original. Either can be pushed / pulled alone.
  history_.beginGroup("Offset");
  auto added = addObject(std::move(object), "Offset");
  if (!added) {
    history_.abortGroup();
    return Result<PushPullResult>::failure(added.error());
  }
  punchHole(distance < 0 ? added.value() : id);
  if (auto settled = settleOverlaps({added.value()}); !settled) {
    history_.abortGroup();
    return Result<PushPullResult>::failure(settled.error());
  }
  history_.endGroup();
  return PushPullResult{added.value(), key};
}

Result<ObjectId> Modeler::offsetSolidFace(const ObjectId& id, const TopoDS_Face& worldFace,
                                          double distance) {
  const Object* owner = document_.find(id);
  if (!owner) return Result<ObjectId>::failure("Object not found");
  if (owner->kind() != ObjectKind::Solid) return Result<ObjectId>::failure("Not a solid");
  if (distance < 0) {
    // Inwards: drawn onto the face, which is divided into frame and inner
    // face (parametric InsetOp on the solid; the distance stays editable).
    const TopoDS_Face local =
        TopoDS::Face(worldFace.Moved(TopLoc_Location(document_.worldTransform(*owner).Inverted())));
    auto ref = geom::FaceRef::identify(local);
    if (!ref) return Result<ObjectId>::failure(ref.error());
    Object object = *owner;
    object.recipe.modifiers.push_back(InsetOp{ref.value(), distance});
    const Status s = commitModified(std::move(object), "Offset");
    if (!s) return Result<ObjectId>::failure(s.message());
    return id;
  }
  auto face = geom::offsetFace(worldFace, distance);
  if (!face) return Result<ObjectId>::failure(face.error());
  // Outwards the outline leaves the face: a new face of its own in the
  // owner's group, expressed in that group's frame (committed directly: it
  // must not be re-homed into another group).
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
  const Status built = rebuild(object, document_.regenContext());
  if (!built) return built;
  // A profile that cuts a hole into a face around it (drawn inside it, or
  // offset inwards): the hole follows its new size, in the same step.
  ChangeSet changes(document_);
  if (current->kind() == ObjectKind::Face) {
    const std::string before = describe(current->recipe);
    for (const Object& o : document_.objects()) {
      if (o.id == id || o.parent != current->parent) continue;
      Object host = o;
      bool follows = false;
      for (Modifier& m : host.recipe.modifiers) {
        auto* hole = std::get_if<HoleOp>(&m);
        if (!hole || hole->outline.name != current->name || !hole->outline.recipe ||
            describe(*hole->outline.recipe) != before) {
          continue;
        }
        hole->outline.recipe = std::make_shared<const Recipe>(object.recipe);
        follows = true;
      }
      if (!follows) continue;
      if (const Status s = rebuild(host, document_.regenContext()); !s) {
        return Status::failure("The face around it cannot take the new size: " + s.message());
      }
      changes.modify(host);
    }
  }
  changes.modify(object);
  history_.execute(changes.build("Edit " + label));
  return Status::success();
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

Status Modeler::hide(const std::vector<ObjectId>& ids) {
  if (ids.empty()) return Status::failure("Nothing selected");
  ChangeSet changes(document_);
  for (const ObjectId& id : topmost(ids)) {
    const Object* o = document_.find(id);
    if (!o) return Status::failure("Object not found");
    if (!o->visible) continue;
    Object changed = *o;
    changed.visible = false;
    changes.modify(changed);
  }
  history_.execute(changes.build("Hide"));
  return Status::success();
}

Status Modeler::unhideAll() {
  ChangeSet changes(document_);
  bool any = false;
  for (const Object& o : document_.objects()) {
    if (o.visible) continue;
    Object changed = o;
    changed.visible = true;
    changes.modify(changed);
    any = true;
  }
  if (!any) return Status::failure("Nothing is hidden");
  history_.execute(changes.build("Unhide All"));
  return Status::success();
}

Status Modeler::setLayer(const ObjectId& id, const std::string& layer) {
  if (layer.empty()) return Status::failure("Tag must not be empty");
  return modifyAttribute(document_, history_, id, "Set Tag", [&](Object& o) { o.layer = layer; });
}

Status Modeler::setAttribute(const std::vector<ObjectId>& ids, const std::string& key, const std::string& value) {
  if (key.empty()) return Status::failure("Attribute name must not be empty");
  if (ids.empty()) return Status::failure("Nothing selected");
  ChangeSet changes(document_);
  bool any = false;
  for (const ObjectId& id : ids) {
    const Object* current = document_.find(id);
    if (!current) return Status::failure("Object not found");
    Object o = *current;
    if (value.empty()) {
      o.attributes.erase(key);
    } else {
      o.attributes[key] = value;
    }
    if (o.attributes == current->attributes) continue;
    changes.modify(o);
    any = true;
  }
  if (any) history_.execute(changes.build("Set Attribute"));
  return Status::success();
}

Status Modeler::setColor(const ObjectId& id, const Color& color) { return paint({id}, color); }

Status Modeler::paint(const std::vector<ObjectId>& ids, const Color& color) {
  for (const ObjectId& id : ids) {
    if (!document_.find(id)) return Status::failure("Object not found");
  }
  ChangeSet changes(document_);
  const std::string material = changes.colorMaterialFor(color);
  bool any = false;
  for (const ObjectId& id : topmost(ids)) {
    const Object* current = document_.find(id);
    if (!current) return Status::failure("Object not found");
    if (current->material == material) continue;
    Object o = *current;
    o.material = material;
    changes.modify(o);
    any = true;
  }
  if (!any) return Status::success();
  history_.execute(changes.build("Paint"));
  return Status::success();
}

// --- materials ---------------------------------------------------------------------

Result<std::string> Modeler::createMaterial(const std::string& name, const Color& color) {
  if (name.empty()) return Result<std::string>::failure("Name must not be empty");
  Material m;
  m.id = "mat-" + generateObjectId();
  m.name = name;
  m.color = color;
  ChangeSet changes(document_);
  changes.putMaterial(m);
  history_.execute(changes.build("New Material"));
  return m.id;
}

namespace {

Status checkMaterial(const Document& document, const Material& m) {
  if (m.name.empty()) return Status::failure("Name must not be empty");
  auto unit = [](double v) { return std::isfinite(v) && v >= 0.0 && v <= 1.0; };
  if (!unit(m.color.r) || !unit(m.color.g) || !unit(m.color.b)) return Status::failure("Colour components go from 0 to 1");
  if (!unit(m.opacity)) return Status::failure("Opacity goes from 0 to 1");
  if (!unit(m.metallic) || !unit(m.roughness) || !unit(m.reflectance)) {
    return Status::failure("Metallic, roughness and reflectance go from 0 to 1");
  }
  if (m.texture) {
    if (!document.findImage(m.texture->image)) return Status::failure("The texture picture is not in the document");
    if (!(m.texture->width > tolerance::kMinDimension) || !(m.texture->height > tolerance::kMinDimension)) {
      return Status::failure("The texture size must be positive");
    }
  }
  return Status::success();
}

}  // namespace

Status Modeler::editMaterial(const Material& changed, bool continuing) {
  const Material* current = document_.findMaterial(changed.id);
  if (!current) return Status::failure("Unknown material " + changed.id);
  if (const Status ok = checkMaterial(document_, changed); !ok) return ok;
  if (changed == *current) return Status::success();
  // Dragging a colour slider is one undo step: further edits of the same
  // material right after merge into it.
  const Command* last = history_.lastCommand();
  const bool amend = continuing && last && last->name == "Edit Material" && last->deltas.empty() &&
                     last->materialDeltas.size() == 1 && last->materialDeltas.front().id == changed.id;
  ChangeSet changes(document_);
  changes.putMaterial(changed);
  if (amend) history_.beginAmend();
  history_.execute(changes.build("Edit Material"));
  if (amend) history_.endAmend();
  return Status::success();
}

Result<std::string> Modeler::duplicateMaterial(const std::string& id, const std::string& name) {
  using R = Result<std::string>;
  const Material* current = document_.findMaterial(id);
  if (!current) return R::failure("Unknown material " + id);
  Material copy = *current;
  copy.id = "mat-" + generateObjectId();
  copy.name = name;
  if (copy.name.empty()) {
    // "Brick" -> "Brick 2", the first free number.
    for (int n = 2;; ++n) {
      const std::string candidate = current->name + " " + std::to_string(n);
      const auto& all = document_.materials();
      if (std::none_of(all.begin(), all.end(), [&](const Material& m) { return m.name == candidate; })) {
        copy.name = candidate;
        break;
      }
    }
  }
  if (const Status ok = checkMaterial(document_, copy); !ok) return R::failure(ok.message());
  ChangeSet changes(document_);
  changes.putMaterial(copy);
  history_.execute(changes.build("Duplicate Material"));
  return copy.id;
}

std::string Modeler::addTextureImage(const std::string& bytes, const std::string& name) {
  return document_.addImage(bytes, name);
}

Status Modeler::updateMaterial(const std::string& id, std::optional<std::string> name, std::optional<Color> color) {
  const Material* current = document_.findMaterial(id);
  if (!current) return Status::failure("Unknown material " + id);
  Material m = *current;
  if (name) {
    if (name->empty()) return Status::failure("Name must not be empty");
    m.name = *name;
  }
  if (color) m.color = *color;
  if (m == *current) return Status::success();
  ChangeSet changes(document_);
  changes.putMaterial(m);
  history_.execute(changes.build("Edit Material"));
  return Status::success();
}

Status Modeler::deleteMaterial(const std::string& id) {
  if (!document_.findMaterial(id)) return Status::failure("Unknown material " + id);
  ChangeSet changes(document_);
  auto strip = [&](Object& o) {
    bool changed = false;
    if (o.material == id) {
      o.material.clear();
      changed = true;
    }
    const auto end = std::remove_if(o.faceMaterials.begin(), o.faceMaterials.end(),
                                    [&](const FaceMaterial& f) { return f.material == id; });
    changed |= end != o.faceMaterials.end();
    o.faceMaterials.erase(end, o.faceMaterials.end());
    return changed;
  };
  for (const Object& current : document_.objects()) {
    Object o = current;
    if (strip(o)) changes.modify(o);
  }
  for (const ComponentDefinition& current : document_.definitions()) {
    ComponentDefinition d = current;
    bool changed = false;
    for (Object& o : d.objects) changed |= strip(o);
    if (changed) changes.putDefinition(d);
  }
  changes.removeMaterial(id);
  history_.execute(changes.build("Delete Material"));
  return Status::success();
}

Status Modeler::assignMaterial(const std::vector<ObjectId>& ids, const std::string& material) {
  if (!material.empty() && !document_.findMaterial(material)) return Status::failure("Unknown material " + material);
  for (const ObjectId& id : ids) {
    if (!document_.find(id)) return Status::failure("Object not found");
  }
  ChangeSet changes(document_);
  bool any = false;
  for (const ObjectId& id : topmost(ids)) {
    const Object* current = document_.find(id);
    if (!current) return Status::failure("Object not found");
    if (current->material == material) continue;
    Object o = *current;
    o.material = material;
    changes.modify(o);
    any = true;
  }
  if (any) history_.execute(changes.build(material.empty() ? "Inherit Material" : "Paint"));
  return Status::success();
}

Status Modeler::paintFaces(const ObjectId& id, const std::vector<TopoDS_Face>& worldFaces,
                           const std::string& material) {
  if (!material.empty() && !document_.findMaterial(material)) return Status::failure("Unknown material " + material);
  const Object* current = document_.find(id);
  if (!current || current->shape.IsNull()) return Status::failure("Object not found");
  if (current->isContainer() || current->kind() == ObjectKind::Instance) {
    return Status::failure("Faces of a group or component are painted inside it (or paint it as a whole)");
  }
  Object o = *current;
  const gp_Trsf toLocal = document_.worldTransform(o).Inverted();
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(o.shape, TopAbs_FACE, faces);
  bool any = false;
  for (const TopoDS_Face& world : worldFaces) {
    // The picked face as a face of the object's own shape.
    const TopoDS_Shape moved = world.Moved(TopLoc_Location(toLocal));
    int index = faces.FindIndex(moved);
    if (index == 0) {
      for (int i = 1; i <= faces.Extent() && index == 0; ++i) {
        if (faces(i).TShape() == world.TShape()) index = i;
      }
    }
    if (index == 0) return Status::failure("The face is not part of this object");
    const TopoDS_Face local = TopoDS::Face(faces(index));
    // Whatever painted this face before gives way.
    std::vector<FaceMaterial> kept;
    for (FaceMaterial& fm : o.faceMaterials) {
      const std::vector<TopoDS_Face> on = fm.facesIn(o.shape);
      const bool here = std::any_of(on.begin(), on.end(), [&](const TopoDS_Face& f) { return f.IsSame(local); });
      if (!here) {
        kept.push_back(std::move(fm));
        continue;
      }
      // Pieces of a split face keep it on their other parts.
      for (const TopoDS_Face& f : on) {
        if (f.IsSame(local)) continue;
        if (auto rest = FaceMaterial::of(f, fm.material)) kept.push_back(std::move(*rest));
      }
    }
    o.faceMaterials = std::move(kept);
    if (!material.empty()) {
      auto assigned = FaceMaterial::of(local, material);
      if (!assigned) return Status::failure("This face cannot be painted on its own");
      o.faceMaterials.push_back(std::move(*assigned));
    }
    any = true;
  }
  if (!any) return Status::success();
  if (compareObjects(*current, o) == 0) return Status::success();
  ChangeSet changes(document_);
  changes.modify(o);
  history_.execute(changes.build(material.empty() ? "Inherit Material" : "Paint Face"));
  return Status::success();
}

Status Modeler::clearFaceMaterials(const std::vector<ObjectId>& ids) {
  ChangeSet changes(document_);
  bool any = false;
  for (const ObjectId& id : ids) {
    const Object* current = document_.find(id);
    if (!current) return Status::failure("Object not found");
    if (current->faceMaterials.empty()) continue;
    Object o = *current;
    o.faceMaterials.clear();
    changes.modify(o);
    any = true;
  }
  if (any) history_.execute(changes.build("Inherit Material"));
  return Status::success();
}

Result<Modeler::MaterialAt> Modeler::materialAt(const ObjectId& id, const TopoDS_Face& worldFace) const {
  const Object* o = document_.find(id);
  if (!o) return Result<MaterialAt>::failure("Object not found");
  MaterialAt out;
  if (!o->faceMaterials.empty() && !o->shape.IsNull()) {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(o->shape, TopAbs_FACE, faces);
    const TopoDS_Shape local = worldFace.Moved(TopLoc_Location(document_.worldTransform(*o).Inverted()));
    int index = faces.FindIndex(local);
    for (int i = 1; i <= faces.Extent() && index == 0; ++i) {
      if (faces(i).TShape() == worldFace.TShape()) index = i;
    }
    for (const auto& [i, material] : resolveFaceMaterials(*o).faces) {
      if (i == index) {
        out.material = material;
        out.source = "face";
      }
    }
  }
  if (out.source.empty()) {
    out.material = document_.inheritedMaterial(*o);
    out.source = out.material.empty() ? "none" : o->material.empty() ? "container" : "object";
  }
  out.color = document_.materialColor(out.material, o->color);
  return out;
}

Status Modeler::remove(const std::vector<ObjectId>& ids) {
  ChangeSet changes(document_);
  bool any = false;
  for (const ObjectId& id : ids) {
    const Object* o = document_.find(id);
    if (!o) return Status::failure("Object not found");
    if (o->locked) return Status::failure("'" + o->name + "' is locked");
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
  for (const ObjectId& id : roots) {
    if (const Object* o = document_.find(id); o && o->locked) return Status::failure("'" + o->name + "' is locked");
  }
  ChangeSet changes(document_);
  for (const ObjectId& id : roots) {
    Object o = *document_.find(id);
    o.transform = localDelta(document_.parentTransform(o), worldDelta) * o.transform;
    changes.modify(o);
  }
  history_.beginGroup(commandName);
  history_.execute(changes.build(commandName));
  // Moved onto lines or faces already there: not stacked twice.
  if (auto settled = settleOverlaps(roots); !settled) {
    history_.abortGroup();
    return Status::failure(settled.error());
  }
  history_.endGroup();
  return Status::success();
}

Status Modeler::scale(const std::vector<ObjectId>& ids, const gp_Pnt& center, double factor) {
  if (!std::isfinite(factor) || factor < 1e-6) return Status::failure("Scale factor must be positive");
  const std::vector<ObjectId> roots = topmost(ids);
  if (roots.empty()) return Status::failure("Nothing selected");
  for (const ObjectId& id : roots) {
    if (const Object* o = document_.find(id); o && o->locked) return Status::failure("'" + o->name + "' is locked");
  }
  ChangeSet changes(document_);
  const RegenContext context = document_.regenContext();
  for (const ObjectId& root : roots) {
    for (const ObjectId& id : document_.subtree(root)) {
      const Object* current = document_.find(id);
      if (!current || std::holds_alternative<GroupNode>(current->recipe.base)) continue;
      if (std::holds_alternative<ComponentInstance>(current->recipe.base)) {
        // This placement alone, like its own stretch (XFORM-05): the
        // definition and the other placements stay as they are.
        Object o = *current;
        const gp_Pnt c = center.Transformed(document_.worldTransform(o).Inverted());
        std::get<ComponentInstance>(o.recipe.base).stretches.push_back(StretchOp{c, gp::DX(), gp::DY(), factor, factor, factor});
        if (const Status built = rebuild(o, context); !built) return built;
        changes.modify(o);
        continue;
      }
      Object o = *current;
      // The fixed point in this object's own coordinates.
      const gp_Pnt c = center.Transformed(document_.worldTransform(o).Inverted());
      auto scalePoint = [&](gp_Pnt& p) { p = c.Translated(gp_Vec(c, p) * factor); };
      if (auto* g = std::get_if<GuideLine>(&o.recipe.base)) {
        scalePoint(g->point);
      } else if (auto* d = std::get_if<LinearDimension>(&o.recipe.base)) {
        scalePoint(d->start);
        scalePoint(d->end);
        d->offset *= factor;
      } else if (auto* t = std::get_if<TextNote>(&o.recipe.base)) {
        scalePoint(t->position);
        if (t->target) scalePoint(*t->target);
      } else if (auto* u = std::get_if<ImageUnderlay>(&o.recipe.base)) {
        scalePoint(u->plane.origin);  // calibrating an image: two points and the real distance
        u->width *= factor;
        u->height *= factor;
      } else if (auto* last = o.recipe.modifiers.empty() ? nullptr : std::get_if<ScaleOp>(&o.recipe.modifiers.back());
                 last && last->center.Distance(c) < tolerance::kLinear) {
        last->factor *= factor;
      } else {
        o.recipe.modifiers.push_back(ScaleOp{c, factor});
      }
      gp_Trsf scaling;
      scaling.SetScale(c, factor);
      for (FaceMaterial& fm : o.faceMaterials) {  // painted faces scale with the object
        if (fm.plane) fm.plane = fm.plane->transformed(scaling);
        if (fm.surface) fm.surface->point.Transform(scaling);
      }
      for (EdgeMark& mark : o.hiddenEdges) mark.point.Transform(scaling);
      const Status built = rebuild(o, context);
      if (!built) return built;
      changes.modify(o);
    }
  }
  history_.execute(changes.build("Scale"));
  return Status::success();
}

Status Modeler::stretch(const std::vector<ObjectId>& ids, const gp_Pnt& worldCenter, const gp_Dir& xDir,
                        const gp_Dir& yDir, double fx, double fy, double fz) {
  for (double f : {fx, fy, fz}) {
    if (!std::isfinite(f) || f < 1e-6) return Status::failure("Scale factors must be positive (mirroring is separate)");
  }
  if (std::abs(xDir.Dot(yDir)) > 1e-9) return Status::failure("The scale directions must be perpendicular");
  if (std::abs(fx - fy) < 1e-12 && std::abs(fy - fz) < 1e-12) return scale(ids, worldCenter, fx);
  const std::vector<ObjectId> roots = topmost(ids);
  if (roots.empty()) return Status::failure("Nothing selected");
  for (const ObjectId& id : roots) {
    if (const Object* o = document_.find(id); o && o->locked) return Status::failure("'" + o->name + "' is locked");
  }
  ChangeSet changes(document_);
  const RegenContext context = document_.regenContext();
  for (const ObjectId& root : roots) {
    for (const ObjectId& id : document_.subtree(root)) {
      const Object* current = document_.find(id);
      if (!current || std::holds_alternative<GroupNode>(current->recipe.base)) continue;
      if (std::holds_alternative<ComponentInstance>(current->recipe.base)) {
        // This placement alone: its own stretch, in its own coordinates
        // (the placement transform stays rigid); the definition and the
        // other placements stay as they are.
        Object o = *current;
        const gp_Trsf toLocal = document_.worldTransform(o).Inverted();
        std::get<ComponentInstance>(o.recipe.base)
            .stretches.push_back(StretchOp{worldCenter.Transformed(toLocal), xDir.Transformed(toLocal),
                                           yDir.Transformed(toLocal), fx, fy, fz});
        if (const Status built = rebuild(o, context); !built) return built;
        changes.modify(o);
        continue;
      }
      if (std::holds_alternative<ImageUnderlay>(current->recipe.base)) {
        return Status::failure("Images are scaled uniformly only");
      }
      Object o = *current;
      // The frame in this object's own coordinates.
      const gp_Trsf toLocal = document_.worldTransform(o).Inverted();
      StretchOp op{worldCenter.Transformed(toLocal), xDir.Transformed(toLocal), yDir.Transformed(toLocal), fx, fy, fz};
      const gp_GTrsf g = geom::stretchTransform(op.center, op.xDir, op.yDir, fx, fy, fz);
      auto apply = [&](gp_Pnt& p) {
        gp_XYZ xyz = p.XYZ();
        g.Transforms(xyz);
        p = gp_Pnt(xyz);
      };
      if (auto* guide = std::get_if<GuideLine>(&o.recipe.base)) {
        gp_Pnt a = guide->point, b = guide->point.Translated(gp_Vec(guide->direction) * 1000.0);
        apply(a);
        apply(b);
        guide->point = a;
        if (a.Distance(b) > 1e-9) guide->direction = gp_Dir(gp_Vec(a, b));
      } else if (auto* d = std::get_if<LinearDimension>(&o.recipe.base)) {
        gp_Pnt tip = d->start.Translated(d->offset);
        apply(d->start);
        apply(d->end);
        apply(tip);
        d->offset = gp_Vec(d->start, tip);
      } else if (auto* t = std::get_if<TextNote>(&o.recipe.base)) {
        apply(t->position);
        if (t->target) apply(*t->target);
      } else {
        o.recipe.modifiers.push_back(op);
      }
      const Status built = rebuild(o, context);
      if (!built) return built;
      // Painted faces and hidden lines: the transform keeps the topology,
      // so face / edge k of the old shape is face / edge k of the new one.
      if (!current->faceMaterials.empty() && !current->shape.IsNull()) {
        TopTools_IndexedMapOfShape after;
        TopExp::MapShapes(o.shape, TopAbs_FACE, after);
        std::vector<FaceMaterial> carried;
        for (const auto& [index, material] : resolveFaceMaterials(*current).faces) {
          if (index < 1 || index > after.Extent()) continue;
          if (auto fm = FaceMaterial::of(TopoDS::Face(after(index)), material)) carried.push_back(*fm);
        }
        o.faceMaterials = std::move(carried);
      }
      if (!current->hiddenEdges.empty()) {
        TopTools_IndexedMapOfShape after;
        TopExp::MapShapes(o.shape, TopAbs_EDGE, after);
        std::vector<EdgeMark> marks;
        for (int index : hiddenEdgeIndices(*current)) {
          if (index < 1 || index > after.Extent()) continue;
          if (auto mark = edgeMark(TopoDS::Edge(after(index)))) marks.push_back(*mark);
        }
        o.hiddenEdges = std::move(marks);
      }
      changes.modify(o);
    }
  }
  history_.execute(changes.build("Scale"));
  return Status::success();
}

namespace {

struct OpeningRef {
  std::size_t index;                // 1, 2, ... in the order they were cut
  std::vector<std::size_t> path;    // operand indices from the host's recipe down
  gp_Trsf placement;                // profile frame -> host local
  const BooleanOperand* operand;
  const RectangleProfile* rect;
};

// Walls with several openings nest Subtracts (each cut wraps the previous
// result as operand 0): collect the rectangles at every level, oldest first.
void collectOpenings(const Recipe& recipe, std::vector<std::size_t> path, const gp_Trsf& placement,
                     std::vector<OpeningRef>& out) {
  const auto* boolean = std::get_if<BooleanFeature>(&recipe.base);
  if (!boolean || boolean->kind != BooleanKind::Subtract || boolean->operands.empty()) return;
  const BooleanOperand& base = boolean->operands.front();
  if (base.recipe && base.recipe->modifiers.empty()) {
    std::vector<std::size_t> inner = path;
    inner.push_back(0);
    collectOpenings(*base.recipe, inner, placement * base.placement, out);
  }
  for (std::size_t i = 1; i < boolean->operands.size(); ++i) {
    const BooleanOperand& op = boolean->operands[i];
    if (!op.recipe) continue;
    if (const auto* rect = std::get_if<RectangleProfile>(&op.recipe->base)) {
      std::vector<std::size_t> at = path;
      at.push_back(i);
      out.push_back({out.size() + 1, at, placement * op.placement, &op, rect});
    }
  }
}

std::vector<OpeningRef> rectangularOpenings(const Object& host) {
  std::vector<OpeningRef> out;
  if (host.recipe.modifiers.empty()) collectOpenings(host.recipe, {}, gp_Trsf(), out);
  return out;
}

// World position of a point given in an opening's profile plane.
gp_Pnt openingPoint(const gp_Trsf& hostWorld, const OpeningRef& o, double u, double v) {
  return o.rect->plane.toWorld(u, v).Transformed(hostWorld * o.placement);
}

// Where an opening sits on its wall: along u = Z x normal and in Z.
struct OpeningExtent {
  std::size_t index;
  gp_Dir normal;
  double u0, u1, z0, z1;
};

std::vector<OpeningExtent> openingExtents(const Object& host, const gp_Trsf& hostWorld) {
  std::vector<OpeningExtent> out;
  for (const OpeningRef& o : rectangularOpenings(host)) {
    const gp_Dir n = gp_Dir(o.rect->plane.normal).Transformed(hostWorld * o.placement);
    if (std::abs(n.Z()) > 1e-6) continue;  // only openings in vertical faces
    const gp_Vec u(gp::DZ().Crossed(n));
    OpeningExtent e{o.index, n, 1e300, -1e300, 1e300, -1e300};
    for (double a : {0.0, o.rect->width}) {
      for (double b : {0.0, o.rect->height}) {
        const gp_Pnt p = openingPoint(hostWorld, o, a, b);
        const double d = gp_Vec(p.XYZ()).Dot(u);
        e.u0 = std::min(e.u0, d);
        e.u1 = std::max(e.u1, d);
        e.z0 = std::min(e.z0, p.Z());
        e.z1 = std::max(e.z1, p.Z());
      }
    }
    out.push_back(e);
  }
  return out;
}

// Positive-area overlap of two rectangles on the same (or the opposite)
// side of a wall; touching edges are fine. u is measured along Z x n, so
// for an opening seen from the other side it runs the other way.
bool openingsOverlap(const OpeningExtent& a, const gp_Dir& n, double u0, double u1, double z0, double z1) {
  const double along = a.normal.Dot(n);
  if (std::abs(along) < 1.0 - 1e-6) return false;
  const double a0 = along > 0 ? a.u0 : -a.u1, a1 = along > 0 ? a.u1 : -a.u0;
  return std::min(a1, u1) - std::max(a0, u0) > tolerance::kLinear &&
         std::min(a.z1, z1) - std::max(a.z0, z0) > tolerance::kLinear;
}

// Replaces the operand recipe at `path` (copy on write along the way).
void replaceOperand(Recipe& recipe, const std::vector<std::size_t>& path, std::size_t depth, Recipe replacement) {
  auto& boolean = std::get<BooleanFeature>(recipe.base);
  BooleanOperand& op = boolean.operands[path[depth]];
  if (depth + 1 == path.size()) {
    op.recipe = std::make_shared<const Recipe>(std::move(replacement));
    return;
  }
  Recipe inner = *op.recipe;
  replaceOperand(inner, path, depth + 1, std::move(replacement));
  op.recipe = std::make_shared<const Recipe>(std::move(inner));
}

}  // namespace

Result<std::vector<Modeler::OpeningInfo>> Modeler::openings(const ObjectId& hostId) const {
  const Object* host = document_.find(hostId);
  if (!host) return Result<std::vector<OpeningInfo>>::failure("Object not found");
  const gp_Trsf world = document_.worldTransform(*host);
  std::vector<OpeningInfo> out;
  for (const OpeningRef& o : rectangularOpenings(*host)) {
    OpeningInfo info;
    info.index = o.index;
    info.name = o.operand->name;
    info.width = std::abs(o.rect->width);
    info.height = std::abs(o.rect->height);
    info.center = openingPoint(world, o, o.rect->width / 2, o.rect->height / 2);
    info.bottom = std::numeric_limits<double>::max();
    info.normal = gp_Dir(o.rect->plane.normal).Transformed(world * o.placement);
    const gp_Vec along = std::abs(info.normal.Z()) < 1.0 - 1e-9 ? gp_Vec(gp::DZ().Crossed(info.normal)) : gp_Vec();
    double a0 = 1e300, a1 = -1e300;
    for (double u : {0.0, o.rect->width}) {
      for (double v : {0.0, o.rect->height}) {
        const gp_Pnt p = openingPoint(world, o, u, v);
        info.bottom = std::min(info.bottom, p.Z());
        a0 = std::min(a0, gp_Vec(p.XYZ()).Dot(along));
        a1 = std::max(a1, gp_Vec(p.XYZ()).Dot(along));
      }
    }
    info.along = a1 - a0;
    out.push_back(info);
  }
  return out;
}

Status Modeler::setOpeningSize(const ObjectId& hostId, const std::vector<std::size_t>& indices,
                               std::optional<double> width, std::optional<double> height, bool keepCenter) {
  for (const std::optional<double>& d : {width, height}) {
    if (d && (!std::isfinite(*d) || *d < tolerance::kMinDimension)) {
      return Status::failure("Opening sizes must be positive");
    }
  }
  if (!width && !height) return Status::failure("Give a width or a height");
  const Object* current = document_.find(hostId);
  if (!current) return Status::failure("Object not found");
  const std::vector<OpeningRef> found = rectangularOpenings(*current);
  if (found.empty()) return Status::failure("This object has no rectangular openings");
  std::set<std::size_t> wanted(indices.begin(), indices.end());
  for (std::size_t i : wanted) {
    bool known = false;
    for (const OpeningRef& o : found) known |= o.index == i;
    if (!known) return Status::failure("No opening " + std::to_string(i) + " in this object");
  }
  const gp_Trsf world = document_.worldTransform(*current);
  Object object = *current;
  for (const OpeningRef& o : found) {
    if (!wanted.empty() && !wanted.count(o.index)) continue;
    Recipe recipe = *o.operand->recipe;  // operands are shared snapshots: copy on write
    auto& rect = std::get<RectangleProfile>(recipe.base);
    // Keep the sign (direction) of each side, change its length.
    const double w = width ? std::copysign(*width, rect.width) : rect.width;
    const double h = height ? std::copysign(*height, rect.height) : rect.height;
    // Anchor in the plane: horizontal centre, and the lower edge (sill) or
    // the centre vertically. "Lower" is decided in world coordinates.
    const gp_Vec up = gp_Vec(rect.plane.yDir()).Transformed(world * o.placement);
    double fv = 0.5;
    if (!keepCenter && std::abs(up.Z()) > 1e-6) fv = (up.Z() * rect.height > 0) ? 0.0 : 1.0;
    const double fu = 0.5;
    rect.plane.origin.Translate(gp_Vec(rect.plane.xDir) * (fu * (rect.width - w)) +
                                gp_Vec(rect.plane.yDir()) * (fv * (rect.height - h)));
    rect.width = w;
    rect.height = h;
    replaceOperand(object.recipe, o.path, 0, std::move(recipe));
  }
  // Grown openings must not run into each other (holes would merge).
  const std::vector<OpeningExtent> extents = openingExtents(object, world);
  for (std::size_t i = 0; i < extents.size(); ++i) {
    for (std::size_t j = i + 1; j < extents.size(); ++j) {
      const OpeningExtent& b = extents[j];
      if ((wanted.empty() || wanted.count(extents[i].index) || wanted.count(b.index)) &&
          openingsOverlap(extents[i], b.normal, b.u0, b.u1, b.z0, b.z1)) {
        return Status::failure("Openings " + std::to_string(extents[i].index) + " and " +
                               std::to_string(b.index) + " would overlap");
      }
    }
  }
  return commitModified(std::move(object), "Opening Size");
}

Result<std::vector<ObjectId>> Modeler::assignLevels(const std::vector<ObjectId>& ids, std::vector<Level> levels,
                                                    const std::string& key) {
  if (levels.empty()) return Result<std::vector<ObjectId>>::failure("Give at least one level");
  if (key.empty()) return Result<std::vector<ObjectId>>::failure("Attribute name must not be empty");
  std::sort(levels.begin(), levels.end(), [](const Level& a, const Level& b) { return a.elevation < b.elevation; });
  std::vector<ObjectId> targets;
  if (ids.empty()) {
    for (const Object& o : document_.objects()) {
      if (o.parent.empty()) targets.push_back(o.id);
    }
  } else {
    targets = topmost(ids);
  }
  ChangeSet changes(document_);
  std::vector<ObjectId> below;
  for (const ObjectId& id : targets) {
    const Object* o = document_.find(id);
    if (!o) return Result<std::vector<ObjectId>>::failure("Object not found");
    TopoDS_Shape world = document_.worldShape(*o);
    if (o->isContainer()) {
      // A group's extent is that of its contents.
      BRep_Builder builder;
      TopoDS_Compound all;
      builder.MakeCompound(all);
      for (const ObjectId& c : document_.subtree(id)) {
        const Object* child = document_.find(c);
        if (child && !child->shape.IsNull()) builder.Add(all, document_.worldShape(*child));
      }
      world = all;
    }
    if (world.IsNull()) continue;
    const Bnd_Box box = geom::exactBounds(world);
    if (box.IsVoid()) continue;
    const double bottom = box.CornerMin().Z() + tolerance::kLinear;  // a slab top at the level counts
    const Level* level = nullptr;
    for (const Level& l : levels) {
      if (l.elevation <= bottom + 1.0) level = &l;  // 1 mm: drawn "on" the level
    }
    if (!level) {
      below.push_back(id);
      continue;
    }
    if (const auto it = o->attributes.find(key); it != o->attributes.end() && it->second == level->name) continue;
    Object changed = *o;
    changed.attributes[key] = level->name;
    changes.modify(changed);
  }
  history_.execute(changes.build("Assign Levels"));
  return below;
}

Status Modeler::align(const std::vector<ObjectId>& ids, int axis, int edge, std::optional<double> value) {
  if (axis < 0 || axis > 2) return Status::failure("Axis must be x, y or z");
  const std::vector<ObjectId> roots = topmost(ids);
  if (roots.empty()) return Status::failure("Nothing selected");
  auto coordinate = [&](const ObjectId& id) -> std::optional<double> {
    BRep_Builder builder;
    TopoDS_Compound all;
    builder.MakeCompound(all);
    bool any = false;
    for (const ObjectId& c : document_.subtree(id)) {
      const Object* o = document_.find(c);
      if (o && !o->shape.IsNull()) {
        builder.Add(all, document_.worldShape(*o));
        any = true;
      }
    }
    if (!any) return std::nullopt;
    const Bnd_Box box = geom::exactBounds(all);
    if (box.IsVoid()) return std::nullopt;
    const double lo = box.CornerMin().Coord(axis + 1), hi = box.CornerMax().Coord(axis + 1);
    return edge < 0 ? lo : edge > 0 ? hi : 0.5 * (lo + hi);
  };
  std::optional<double> target = value;
  if (!target) target = coordinate(roots.front());
  if (!target) return Status::failure("The first object has no geometry");
  ChangeSet changes(document_);
  for (const ObjectId& id : roots) {
    const auto at = coordinate(id);
    if (!at || std::abs(*at - *target) < 1e-9) continue;
    gp_Vec v(0, 0, 0);
    v.SetCoord(axis + 1, *target - *at);
    gp_Trsf delta;
    delta.SetTranslation(v);
    Object o = *document_.find(id);
    o.transform = localDelta(document_.parentTransform(o), delta) * o.transform;
    changes.modify(o);
  }
  history_.execute(changes.build("Align"));
  return Status::success();
}

Result<std::vector<ObjectId>> Modeler::createWalls(std::vector<gp_Pnt> points, double thickness, double height,
                                                  int justification) {
  using R = Result<std::vector<ObjectId>>;
  if (points.size() < 2) return R::failure("A wall needs at least two points");
  if (!(thickness > tolerance::kMinDimension) || !(height > tolerance::kMinDimension)) {
    return R::failure("Wall thickness and height must be positive");
  }
  const double base = points.front().Z();
  for (gp_Pnt& p : points) {
    if (std::abs(p.Z() - base) > tolerance::kLinear) return R::failure("Wall points must be at one height");
    p.SetZ(base);
  }
  const bool closed = points.size() > 3 && points.front().Distance(points.back()) < tolerance::kLinear;
  if (closed) points.pop_back();
  const std::size_t segments = closed ? points.size() : points.size() - 1;
  std::vector<gp_Dir> dirs;
  for (std::size_t i = 0; i < segments; ++i) {
    const gp_Vec v(points[i], points[(i + 1) % points.size()]);
    if (v.Magnitude() < tolerance::kMinDimension) return R::failure("Two wall points coincide");
    dirs.emplace_back(v);
  }
  // The wall's band, measured to the left of its line: [o, o + thickness].
  const double o = justification < 0 ? 0.0 : justification > 0 ? -thickness : -thickness / 2;
  // Butt joints (exact for right angles): at a corner the incoming wall
  // runs on to the far side of the corner square, the outgoing one starts
  // after it. turn = +1 left, -1 right, 0 straight on.
  auto turn = [&](std::size_t in, std::size_t out) {
    const double z = gp_Vec(dirs[in]).Crossed(gp_Vec(dirs[out])).Z();
    return std::abs(z) < 1e-9 ? 0 : (z > 0 ? 1 : -1);
  };
  history_.beginGroup("Walls");
  std::vector<ObjectId> walls;
  for (std::size_t i = 0; i < segments; ++i) {
    const gp_Pnt a = points[i], b = points[(i + 1) % points.size()];
    const double length = a.Distance(b);
    double startCut = 0.0, endRun = 0.0;
    if (closed || i > 0) {
      const int t = turn((i + segments - 1) % segments, i);
      startCut = t > 0 ? o + thickness : t < 0 ? -o : 0.0;
    }
    if (closed || i + 1 < segments) {
      const int t = turn(i, (i + 1) % segments);
      endRun = t > 0 ? -o : t < 0 ? o + thickness : 0.0;
    }
    const gp_Dir& dir = dirs[i];
    const gp_Dir left = gp::DZ().Crossed(dir);
    geom::SketchPlane plane;
    plane.normal = gp::DZ();
    plane.xDir = dir;  // the rectangle spans +thickness to the left of its origin line
    plane.origin = a.Translated(gp_Vec(dir) * startCut + gp_Vec(left) * o);
    const double wallLength = length - startCut + endRun;
    if (wallLength < tolerance::kMinDimension) {
      history_.abortGroup();
      return R::failure("A wall segment is shorter than the wall is thick");
    }
    auto id = createRectangle(plane, wallLength, thickness, uniqueName("Wall"));
    if (!id || !extrude(*id, height)) {
      history_.abortGroup();
      return R::failure(id ? "Wall could not be extruded" : id.error());
    }
    walls.push_back(*id);
  }
  const Status tagged = setAttribute(walls, "kind", "wall");
  if (!tagged) {
    history_.abortGroup();
    return R::failure(tagged.message());
  }
  history_.endGroup();
  return walls;
}

Result<ObjectId> Modeler::createSlab(const std::vector<gp_Pnt>& outline, double top, double thickness) {
  if (outline.size() < 3) return Result<ObjectId>::failure("A slab needs at least three corners");
  if (!(thickness > tolerance::kMinDimension)) return Result<ObjectId>::failure("Slab thickness must be positive");
  std::vector<gp_Pnt> pts;
  for (const gp_Pnt& p : outline) pts.emplace_back(p.X(), p.Y(), top - thickness);
  if (pts.size() > 3 && pts.front().Distance(pts.back()) < tolerance::kLinear) pts.pop_back();
  // Counter-clockwise in plan so that extruding goes up.
  double twiceArea = 0.0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const gp_Pnt& p = pts[i];
    const gp_Pnt& q = pts[(i + 1) % pts.size()];
    twiceArea += p.X() * q.Y() - q.X() * p.Y();
  }
  if (std::abs(twiceArea) < tolerance::kMinDimension) return Result<ObjectId>::failure("The slab outline has no area");
  if (twiceArea < 0) std::reverse(pts.begin(), pts.end());
  history_.beginGroup("Slab");
  auto id = createPolygon(pts, uniqueName("Slab"));
  if (!id || !extrude(*id, thickness) || !setAttribute({*id}, "kind", "slab")) {
    history_.abortGroup();
    return Result<ObjectId>::failure(id ? "Slab could not be extruded" : id.error());
  }
  history_.endGroup();
  return id;
}

Result<ObjectId> Modeler::placeOpenings(const ObjectId& hostId, const gp_Dir& faceNormal, int count, double width,
                                        double height, double sill, std::optional<double> spacing) {
  return cutOpenings(hostId, faceNormal, count, width, height, sill, spacing, std::nullopt);
}

Result<ObjectId> Modeler::placeOpeningAt(const ObjectId& hostId, const gp_Dir& faceNormal, const gp_Pnt& at,
                                         double width, double height, double sill) {
  return cutOpenings(hostId, faceNormal, 1, width, height, sill, std::nullopt, at);
}

Result<ObjectId> Modeler::cutOpenings(const ObjectId& hostId, const gp_Dir& faceNormal, int count, double width,
                                      double height, double sill, std::optional<double> spacing,
                                      std::optional<gp_Pnt> at) {
  using R = Result<ObjectId>;
  if (count < 1 || count > 200) return R::failure("Give 1 to 200 openings");
  if (!(width > tolerance::kMinDimension) || !(height > tolerance::kMinDimension) || sill < 0) {
    return R::failure("Opening sizes must be positive");
  }
  if (std::abs(faceNormal.Z()) > 1e-6) return R::failure("Openings go into a vertical face");
  const Object* host = document_.find(hostId);
  if (!host || host->shape.IsNull()) return R::failure("Object not found");
  const TopoDS_Shape world = document_.worldShape(*host);
  // The outermost plane facing that way: all its faces together (a wall
  // face split by a drawn line is still one wall side).
  std::vector<std::pair<double, TopoDS_Face>> facing;
  for (TopExp_Explorer e(world, TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face f = TopoDS::Face(e.Current());
    const auto ref = geom::FaceRef::fromFace(f);
    if (!ref || !ref->normal.IsEqual(faceNormal, 1e-6)) continue;
    facing.emplace_back(gp_Vec(ref->centroid.XYZ()).Dot(gp_Vec(faceNormal)), f);
  }
  if (facing.empty()) return R::failure("No face of this object faces that way");
  const double best =
      std::max_element(facing.begin(), facing.end(), [](const auto& a, const auto& b) { return a.first < b.first; })
          ->first;
  BRep_Builder sideBuilder;
  TopoDS_Compound side;
  sideBuilder.MakeCompound(side);
  std::optional<TopoDS_Face> face;
  for (const auto& [out, f] : facing) {
    if (out < best - tolerance::kLinear) continue;
    sideBuilder.Add(side, f);
    if (!face) face = f;
  }
  // Face extent: along the wall (u, horizontal) and up; thickness along
  // the normal from the whole shape.
  const gp_Dir u = gp::DZ().Crossed(faceNormal);  // with the normal, gives an upward plane
  double u0 = 1e300, u1 = -1e300, z0 = 1e300, z1 = -1e300, n0 = 1e300, n1 = -1e300;
  for (TopExp_Explorer v(side, TopAbs_VERTEX); v.More(); v.Next()) {
    const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
    u0 = std::min(u0, gp_Vec(p.XYZ()).Dot(gp_Vec(u)));
    u1 = std::max(u1, gp_Vec(p.XYZ()).Dot(gp_Vec(u)));
    z0 = std::min(z0, p.Z());
    z1 = std::max(z1, p.Z());
  }
  for (TopExp_Explorer v(world, TopAbs_VERTEX); v.More(); v.Next()) {
    const double d = gp_Vec(BRep_Tool::Pnt(TopoDS::Vertex(v.Current())).XYZ()).Dot(gp_Vec(faceNormal));
    n0 = std::min(n0, d);
    n1 = std::max(n1, d);
  }
  const double length = u1 - u0, depth = n1 - n0;
  // Openings go through a wall's thickness. From the edge of a slab (or a
  // block deeper than it is tall) the cut would tunnel through the whole
  // object instead.
  const auto kindAttribute = host->attributes.find("kind");
  if ((kindAttribute != host->attributes.end() && kindAttribute->second == "slab") ||
      depth > (z1 - z0) + tolerance::kLinear) {
    return R::failure("Openings go through the side of a wall; this face is the edge of a slab or a block, and the "
                      "cut would run through all of it. For a floor opening, draw a rectangle on the top and "
                      "push it through");
  }
  if (sill + height > z1 - z0 + tolerance::kLinear) return R::failure("The openings are taller than the wall");
  std::vector<double> centres;
  if (at) {
    const double c = gp_Vec(at->XYZ()).Dot(gp_Vec(u));
    if (c - width / 2 < u0 - tolerance::kLinear || c + width / 2 > u1 + tolerance::kLinear) {
      return R::failure("The opening does not fit on the wall there");
    }
    centres.push_back(c);
  } else if (spacing) {
    if (!(*spacing >= width)) return R::failure("Spacing must be at least the opening width");
    const double mid = 0.5 * (u0 + u1), start = mid - 0.5 * (count - 1) * *spacing;
    for (int k = 0; k < count; ++k) centres.push_back(start + k * *spacing);
    if (centres.front() - width / 2 < u0 - tolerance::kLinear || centres.back() + width / 2 > u1 + tolerance::kLinear) {
      return R::failure("The openings do not fit along the wall");
    }
  } else {
    const double gap = (length - count * width) / (count + 1);
    if (gap < tolerance::kMinDimension) return R::failure("The openings do not fit along the wall");
    for (int k = 0; k < count; ++k) centres.push_back(u0 + gap * (k + 1) + width * (k + 0.5));
  }
  // Openings already in the wall must not be cut into again.
  const std::vector<OpeningExtent> existing = openingExtents(*host, document_.worldTransform(*host));
  for (double c : centres) {
    for (const OpeningExtent& e : existing) {
      if (openingsOverlap(e, faceNormal, c - width / 2, c + width / 2, z0 + sill, z0 + sill + height)) {
        return R::failure("The opening would overlap opening " + std::to_string(e.index));
      }
    }
  }
  // A point on the face plane to place the profiles on.
  const auto ref = geom::FaceRef::fromFace(*face);
  const double planeOffset = gp_Vec(ref->centroid.XYZ()).Dot(gp_Vec(faceNormal));
  // Every opening is cut in one Boolean (the wall minus all of them): one
  // push-through per opening recomputed the whole wall again for each, ever
  // slower as it gained holes (40 openings took minutes). The recipe is the
  // same kind as a push-through: rectangles extruded through, operands
  // 1..n of a Subtract, so they are listed and resized the same way.
  const Object hostObject = *host;
  const gp_Trsf toParent = document_.parentTransform(hostObject).Inverted();
  const std::string name = uniqueName("Opening");
  BooleanFeature feature;
  feature.kind = BooleanKind::Subtract;
  feature.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(hostObject.recipe),
                                            toParent * document_.worldTransform(hostObject), hostObject.name});
  rememberShape(feature.operands.back().recipe, hostObject.shape);
  for (double c : centres) {
    geom::SketchPlane plane;
    plane.normal = faceNormal;
    plane.xDir = u;  // u = Z x n keeps yDir (= n x xDir) pointing up
    gp_XYZ origin = gp_Vec(faceNormal).XYZ() * planeOffset + gp_Vec(u).XYZ() * (c - width / 2);
    origin.SetZ(z0 + sill);
    plane.origin = gp_Pnt(origin);
    Recipe tool;
    tool.base = RectangleProfile{plane, width, height};
    tool.modifiers.push_back(ExtrudeOp{-depth});
    feature.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(std::move(tool)), toParent, name});
  }
  Object result = hostObject;
  result.id = generateObjectId();
  result.transform = gp_Trsf();
  result.recipe = Recipe{};
  result.recipe.base = std::move(feature);
  result.faceMaterials.clear();
  result.hiddenEdges.clear();
  if (const Status built = rebuild(result, document_.regenContext()); !built) return R::failure(built.message());
  // An opening must leave the wall in one piece.
  auto solids = [](const TopoDS_Shape& s) {
    int n = 0;
    for (TopExp_Explorer e(s, TopAbs_SOLID); e.More(); e.Next()) ++n;
    return n;
  };
  if (solids(result.shape) > solids(world)) return R::failure("The opening would cut the wall in two");
  if (std::abs(geom::volume(result.shape) - geom::volume(hostObject.shape)) < tolerance::kLinear) {
    return R::failure("The opening did not cut the wall");
  }
  carryMaterials(result, {{&hostObject, toParent * document_.worldTransform(hostObject)}});
  ChangeSet changes(document_);
  changes.erase(hostObject.id);
  changes.add(result);
  history_.execute(changes.build("Openings"));
  return result.id;
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
    // A copy landing back on the original (a polar array going full
    // circle, e.g. 30 degrees x 12) is not made: it would only double it.
    if (landsOnItself(step)) continue;
    for (const ObjectId& id : roots) {
      std::vector<Object> clones = cloneSubtree(id, document_.find(id)->parent);
      Object& root = clones.front();
      root.transform = localDelta(document_.parentTransform(root), step) * root.transform;
      created.push_back(root.id);
      for (const Object& c : clones) changes.add(c);
    }
  }
  history_.beginGroup(count == 1 ? "Copy" : "Array");
  history_.execute(changes.build(count == 1 ? "Copy" : "Array"));
  auto settled = settleOverlaps(created);
  if (!settled) {
    history_.abortGroup();
    return Result<std::vector<ObjectId>>::failure(settled.error());
  }
  history_.endGroup();
  return settledIds(created, *settled);
}

Result<Modeler::Clipboard> Modeler::copyToClipboard(const std::vector<ObjectId>& ids) const {
  const std::vector<ObjectId> roots = topmost(ids);
  if (roots.empty()) return Result<Clipboard>::failure("Nothing selected");
  Clipboard clip;
  BRep_Builder builder;
  TopoDS_Compound preview;
  builder.MakeCompound(preview);
  Bnd_Box box;
  for (const ObjectId& root : roots) {
    const Object* r = document_.find(root);
    if (!r) return Result<Clipboard>::failure("Object not found");
    std::vector<Object> clones = cloneSubtree(root, {});
    clones.front().transform = document_.worldTransform(*r);
    for (const ObjectId& id : document_.subtree(root)) {
      const Object* o = document_.find(id);
      if (!o || o->isContainer() || o->shape.IsNull()) continue;
      const TopoDS_Shape world = document_.worldShape(*o);
      builder.Add(preview, world);
      BRepBndLib::Add(world, box);
    }
    for (Object& c : clones) clip.objects.push_back(std::move(c));
  }
  clip.preview = preview;
  if (!box.IsVoid()) {
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    clip.anchor = gp_Pnt(x0, y0, z0);
  }
  return clip;
}

Result<Modeler::Clipboard> Modeler::copyToClipboard(const std::vector<DeleteTarget>& targets) const {
  using R = Result<Clipboard>;
  std::vector<ObjectId> whole;
  for (const DeleteTarget& t : targets) {
    if (t.whole) whole.push_back(t.id);
  }
  Clipboard clip;
  if (!whole.empty()) {
    auto objects = copyToClipboard(whole);
    if (!objects) return objects;
    clip = std::move(objects.value());
  }
  BRep_Builder builder;
  TopoDS_Compound preview;
  builder.MakeCompound(preview);
  Bnd_Box box;
  if (!clip.preview.IsNull()) {
    builder.Add(preview, clip.preview);
    BRepBndLib::Add(clip.preview, box);
  }
  int counter = 0;
  auto add = [&](Object o, const TopoDS_Shape& world) -> Status {
    o.id = "clip-" + std::to_string(++counter);
    if (const Status built = rebuild(o, document_.regenContext()); !built) return built;
    builder.Add(preview, world);
    BRepBndLib::Add(world, box);
    clip.objects.push_back(std::move(o));
    return Status::success();
  };
  for (const DeleteTarget& t : targets) {
    if (t.whole || (t.faces.empty() && t.edges.empty())) continue;
    const Object* owner = document_.find(t.id);
    if (!owner) return R::failure("Object not found");
    // Faces: each its own face, in world coordinates, with its material.
    TopTools_IndexedMapOfShape faceEdges;
    for (const TopoDS_Face& face : t.faces) {
      TopExp::MapShapes(face, TopAbs_EDGE, faceEdges);
      Object o;
      o.name = owner->name + " (face)";
      o.layer = owner->layer;
      o.color = owner->color;
      if (auto m = materialAt(t.id, face)) o.material = m->material;
      const TopoDS_Shape copy = BRepBuilderAPI_Copy(face).Shape();
      o.recipe.base = ImportedShape{copy, "Face of " + owner->name};
      if (const Status s = add(std::move(o), copy); !s) return R::failure(s.message());
    }
    // Edges on their own (not an edge of a copied face): loose lines.
    std::vector<TopoDS_Edge> loose;
    for (const TopoDS_Edge& e : t.edges) {
      bool bounding = false;
      for (int i = 1; i <= faceEdges.Extent() && !bounding; ++i) bounding = faceEdges(i).IsSame(e);
      if (!bounding) loose.push_back(e);
    }
    for (const auto& run : chainRuns(loose)) {
      for (auto& [recipe, kind] : curveRecipes(run)) {
        Object o;
        o.name = kind;
        o.layer = owner->layer;
        o.color = owner->color;
        o.recipe = std::move(recipe);
        Object probe = o;
        if (const Status built = rebuild(probe, document_.regenContext()); !built) return R::failure(built.message());
        if (const Status s = add(std::move(o), probe.shape); !s) return R::failure(s.message());
      }
    }
  }
  if (clip.objects.empty()) return R::failure("Nothing selected");
  clip.preview = preview;
  if (!box.IsVoid()) {
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    clip.anchor = gp_Pnt(x0, y0, z0);
  }
  return clip;
}

Result<std::vector<ObjectId>> Modeler::copyElements(const std::vector<DeleteTarget>& targets,
                                                    const gp_Trsf& worldDelta, int count) {
  using R = Result<std::vector<ObjectId>>;
  if (count < 1 || count > 10000) return R::failure("Copy count must be between 1 and 10000");
  auto clip = copyToClipboard(targets);
  if (!clip) return R::failure(clip.error());
  history_.beginGroup(count == 1 ? "Copy" : "Array");
  std::vector<ObjectId> made;
  gp_Trsf at;
  for (int k = 1; k <= count; ++k) {
    at = worldDelta * at;
    if (landsOnItself(at)) continue;
    auto ids = paste(*clip, at);
    if (!ids) {
      history_.abortGroup();
      return R::failure(ids.error());
    }
    made.insert(made.end(), ids->begin(), ids->end());
  }
  history_.endGroup();
  return made;
}

Result<std::vector<ObjectId>> Modeler::paste(const Clipboard& clipboard, const gp_Trsf& worldDelta) {
  if (clipboard.empty()) return Result<std::vector<ObjectId>>::failure("The clipboard is empty");
  // Instances need their definition (it may have been purged since).
  for (const Object& o : clipboard.objects) {
    if (const auto* instance = std::get_if<ComponentInstance>(&o.recipe.base)) {
      if (!document_.findDefinition(instance->definitionId)) {
        return Result<std::vector<ObjectId>>::failure("The copied component no longer exists");
      }
    }
  }
  gp_Trsf parentWorld;
  if (!activeGroup_.empty()) {
    if (const Object* group = document_.find(activeGroup_)) parentWorld = document_.worldTransform(*group);
  }
  ChangeSet changes(document_);
  std::map<ObjectId, ObjectId> mapping;
  std::vector<ObjectId> created;
  for (const Object& source : clipboard.objects) {
    Object o = source;
    o.id = generateObjectId();
    mapping[source.id] = o.id;
    if (source.parent.empty()) {
      o.parent = activeGroup_;
      o.transform = parentWorld.Inverted() * worldDelta * source.transform;
      created.push_back(o.id);
    } else {
      o.parent = mapping[source.parent];
    }
    changes.add(o);
  }
  history_.beginGroup("Paste");
  history_.execute(changes.build("Paste"));
  auto settled = settleOverlaps(created);
  if (!settled) {
    history_.abortGroup();
    return Result<std::vector<ObjectId>>::failure(settled.error());
  }
  history_.endGroup();
  return settledIds(created, *settled);
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

namespace {

// Every placement of `definition` (about to replace the stored one) gets its
// geometry again: plain ones share the definition's shape, the others are
// rebuilt with their own parameters / mirror. Overrides of objects or keys
// gone from the definition are dropped.
Status updatePlacements(const Document& document, ChangeSet& changes, const ComponentDefinition& definition,
                        const std::function<void(Object&)>& adjust = {}) {
  std::map<std::string, TopoDS_Shape> shared;
  for (const Object& o : document.objects()) {
    const auto* c = std::get_if<ComponentInstance>(&o.recipe.base);
    if (!c || c->definitionId != definition.id) continue;
    Object updated = o;
    auto& ref = std::get<ComponentInstance>(updated.recipe.base);
    if (!ref.overrides.empty()) {
      std::vector<ParameterOverride> dropped;
      auto variant = applyOverrides(definition, ref.overrides, document.regenContext(), &dropped);
      if (!variant) return Status::failure("'" + o.name + "': " + variant.error());
      ref.overrides.erase(std::remove_if(ref.overrides.begin(), ref.overrides.end(),
        [&](const ParameterOverride& p) {
          return std::find(dropped.begin(), dropped.end(), p) != dropped.end();
        }),
        ref.overrides.end());
    }
    // Placements with the same settings share one shape (and display mesh).
    const std::string key = placementKey(ref);
    auto known = shared.find(key);
    if (known == shared.end()) {
      auto shape = buildInstanceShape(definition, ref, document.regenContext());
      if (!shape) return Status::failure("'" + o.name + "': " + shape.error());
      known = shared.emplace(key, shape.value()).first;
    }
    updated.shape = known->second;
    if (adjust) adjust(updated);
    changes.modify(updated);
  }
  return Status::success();
}

// A rigid transform from its matrix (drops the scale sign two mirrors leave).
gp_Trsf rigid(const gp_Trsf& t) {
  gp_Trsf r;
  r.SetValues(t.Value(1, 1), t.Value(1, 2), t.Value(1, 3), t.Value(1, 4), t.Value(2, 1), t.Value(2, 2),
              t.Value(2, 3), t.Value(2, 4), t.Value(3, 1), t.Value(3, 2), t.Value(3, 3), t.Value(3, 4));
  return r;
}

}  // namespace

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
  instance.recipe.base = ComponentInstance{definition.id, {}, false};
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

namespace {

// The definition as one placement shows it: its own parameters written in
// and its own stretches baked into the recipe of every object (a StretchOp
// in that object's own coordinates, so it stays parametric). A placement
// both mirrored and stretched, or a component inside, is refused.
Result<ComponentDefinition> placementDefinition(const ComponentDefinition& stored, const ComponentInstance& ref,
                                                const RegenContext& context) {
  using R = Result<ComponentDefinition>;
  auto written = applyOverrides(stored, ref.overrides, context);
  if (!written || ref.stretches.empty()) return written;
  if (ref.mirrored) return R::failure("A placement both mirrored and stretched cannot be baked yet; mirror it back first");
  ComponentDefinition definition = written.value();
  std::map<ObjectId, const Object*> byId;
  for (const Object& o : definition.objects) byId[o.id] = &o;
  // Each object's placement in the definition (its parents' and its own).
  auto chain = [&](const Object& o) {
    gp_Trsf t = o.transform;
    ObjectId parent = o.parent;
    int guard = 0;
    while (!parent.empty() && guard++ < 1000) {
      const auto it = byId.find(parent);
      if (it == byId.end()) break;
      t = it->second->transform * t;
      parent = it->second->parent;
    }
    return t;
  };
  std::vector<gp_Trsf> placements;
  for (const Object& o : definition.objects) placements.push_back(chain(o));
  for (std::size_t i = 0; i < definition.objects.size(); ++i) {
    Object& o = definition.objects[i];
    if (o.kind() == ObjectKind::Group) continue;  // what is in it is stretched
    if (o.kind() == ObjectKind::Instance) {
      return R::failure("A stretched placement with a component inside cannot be baked yet");
    }
    const gp_Trsf toLocal = placements[i].Inverted();
    for (const StretchOp& s : ref.stretches) {
      o.recipe.modifiers.push_back(StretchOp{s.center.Transformed(toLocal), s.xDir.Transformed(toLocal),
                                             s.yDir.Transformed(toLocal), s.fx, s.fy, s.fz});
    }
    if (const Status built = rebuild(o, context); !built) {
      return R::failure("'" + o.name + "' cannot be stretched: " + built.message());
    }
  }
  auto shape = buildDefinitionShape(definition, context);
  if (!shape) return R::failure(shape.error());
  definition.shape = shape.value();
  return definition;
}

}  // namespace

Result<std::vector<ObjectId>> Modeler::explode(const ObjectId& instanceId) {
  const Object* instance = document_.find(instanceId);
  if (!instance || instance->kind() != ObjectKind::Instance) {
    return Result<std::vector<ObjectId>>::failure("Not a component instance");
  }
  const auto& ref = std::get<ComponentInstance>(instance->recipe.base);
  const ComponentDefinition* stored = document_.findDefinition(ref.definitionId);
  if (!stored) return Result<std::vector<ObjectId>>::failure("Definition not found");
  if (ref.mirrored) {
    // Its pieces would need mirrored placements, which stay rigid here.
    return Result<std::vector<ObjectId>>::failure("A mirrored component cannot be exploded; mirror it back first");
  }
  // Stretched on its own: its pieces come out stretched (baked, LIM-02).
  auto definition = placementDefinition(*stored, ref, document_.regenContext());
  if (!definition) return Result<std::vector<ObjectId>>::failure(definition.error());

  ChangeSet changes(document_);
  std::map<ObjectId, ObjectId> mapping;
  std::vector<ObjectId> roots;
  for (const Object& source : definition.value().objects) {
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
  const Status placed = updatePlacements(document_, changes, definition);
  if (!placed) return placed;
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
    rememberShape(feature.operands.back().recipe, o->shape);
  }
  const Object& first = *document_.find(unique.front());
  result.color = first.color;
  result.layer = first.layer;
  result.name = uniqueName(toString(kind));
  result.recipe.base = std::move(feature);
  const Status built = rebuild(result, document_.regenContext());
  if (!built) return Result<ObjectId>::failure(built.message());
  std::vector<std::pair<const Object*, gp_Trsf>> inputs;
  for (const ObjectId& id : unique) {
    const Object* o = document_.find(id);
    inputs.emplace_back(o, toParent * document_.worldTransform(*o));
  }
  carryMaterials(result, inputs);

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

Result<ObjectId> Modeler::beginComponentEdit(const ObjectId& instanceId) {
  const Object* instance = document_.find(instanceId);
  const auto* ref = instance ? std::get_if<ComponentInstance>(&instance->recipe.base) : nullptr;
  if (!ref) return Result<ObjectId>::failure("Not a component instance");
  const ComponentDefinition* definition = document_.findDefinition(ref->definitionId);
  if (!definition) return Result<ObjectId>::failure("Definition not found");
  if (!ref->stretches.empty()) {
    // Editing shows the shared definition; this placement differs from it.
    return Result<ObjectId>::failure(
        "This placement is stretched on its own; edit the shared component from another placement (the edit "
        "applies to this one too), or Make Unique to bake the stretch into a definition of its own and edit that");
  }

  Object group;
  group.id = generateObjectId();
  group.name = definition->name;
  group.parent = instance->parent;
  group.transform = instance->transform;
  group.layer = instance->layer;
  group.color = instance->color;
  group.material = instance->material;
  group.recipe.base = GroupNode{definition->id, instance->id, instance->name, ref->overrides, ref->mirrored};

  ChangeSet changes(document_);
  changes.add(group);
  // The copies keep the definition's ids (placements' own parameters name
  // them); only an id already taken in the document gets a fresh one.
  // Parents follow, top-level ones go into the group.
  std::map<ObjectId, ObjectId> ids;
  for (const Object& o : definition->objects) ids[o.id] = document_.find(o.id) ? generateObjectId() : o.id;
  for (const Object& o : definition->objects) {
    Object copy = o;
    copy.id = ids[o.id];
    copy.parent = o.parent.empty() ? group.id : ids[o.parent];
    changes.add(copy);
  }
  changes.erase(instanceId);
  history_.execute(changes.build("Edit Component"));
  activeGroup_ = group.id;
  return group.id;
}

Status Modeler::endComponentEdit(const ObjectId& editGroupId) {
  const Object* group = document_.find(editGroupId);
  const auto* node = group ? std::get_if<GroupNode>(&group->recipe.base) : nullptr;
  if (!node || node->editingDefinition.empty()) return Status::failure("Not a component being edited");
  const ComponentDefinition* current = document_.findDefinition(node->editingDefinition);
  if (!current) return Status::failure("Definition not found");

  ComponentDefinition definition = *current;
  definition.objects.clear();
  for (const ObjectId& id : document_.subtree(editGroupId)) {
    if (id == editGroupId) continue;
    Object o = *document_.find(id);
    if (o.parent == editGroupId) o.parent.clear();
    definition.objects.push_back(o);
  }
  if (definition.objects.empty()) {
    return Status::failure("The component would be empty; delete the instance instead");
  }
  auto shape = buildDefinitionShape(definition, document_.regenContext());
  if (!shape) return shape.status();
  definition.shape = shape.value();

  Object instance;
  instance.id = node->editingInstance.empty() ? generateObjectId() : node->editingInstance;
  instance.name = node->editingInstanceName.empty() ? definition.name : node->editingInstanceName;
  instance.parent = group->parent;
  instance.transform = group->transform;
  instance.layer = group->layer;
  instance.color = group->color;
  instance.material = group->material;
  ComponentInstance ref{definition.id, node->editingOverrides, node->editingMirrored};
  std::vector<ParameterOverride> dropped;
  if (!ref.overrides.empty()) {
    auto variant = applyOverrides(definition, ref.overrides, document_.regenContext(), &dropped);
    if (!variant) return Status::failure("'" + instance.name + "': " + variant.error());
    ref.overrides.erase(std::remove_if(ref.overrides.begin(), ref.overrides.end(),
      [&](const ParameterOverride& p) {
        return std::find(dropped.begin(), dropped.end(), p) != dropped.end();
      }),
      ref.overrides.end());
  }
  auto own = buildInstanceShape(definition, ref, document_.regenContext());
  if (!own) return own.status();
  instance.recipe.base = ref;
  instance.shape = own.value();

  ChangeSet changes(document_);
  changes.putDefinition(definition);
  const Status placed = updatePlacements(document_, changes, definition);
  if (!placed) return placed;
  for (const ObjectId& id : document_.subtree(editGroupId)) changes.erase(id);
  changes.add(instance);
  history_.execute(changes.build("Edit Component"));
  if (activeGroup_ == editGroupId || document_.find(activeGroup_) == nullptr) activeGroup_.clear();
  return Status::success();
}

std::optional<ObjectId> Modeler::componentEditGroup(const ObjectId& id) const {
  for (const Object* o = document_.find(id); o; o = document_.find(o->parent)) {
    const auto* node = std::get_if<GroupNode>(&o->recipe.base);
    if (node && !node->editingDefinition.empty()) return o->id;
    if (o->parent.empty()) break;
  }
  return std::nullopt;
}

int Modeler::instanceCount(const std::string& definitionId) const {
  int n = 0;
  for (const Object& o : document_.objects()) {
    const auto* c = std::get_if<ComponentInstance>(&o.recipe.base);
    n += c && c->definitionId == definitionId;
  }
  return n;
}

std::vector<Modeler::PlacementParameter> Modeler::placementParameters(const ObjectId& instanceId) const {
  std::vector<PlacementParameter> out;
  const Object* instance = document_.find(instanceId);
  const auto* ref = instance ? std::get_if<ComponentInstance>(&instance->recipe.base) : nullptr;
  const ComponentDefinition* d = ref ? document_.findDefinition(ref->definitionId) : nullptr;
  if (!d) return out;
  for (const Object& o : d->objects) {
    for (const Parameter& p : doc::parameters(o.recipe)) {
      PlacementParameter row{o.id, o.name, p, p.value, false};
      for (const ParameterOverride& own : ref->overrides) {
        if (own.object == o.id && own.key == p.key) {
          row.parameter.value = own.value;
          row.own = true;
        }
      }
      out.push_back(row);
    }
  }
  return out;
}

namespace {

// Replaces the placement's settings and rebuilds its geometry.
Status changePlacement(Document& document, CommandStack& history, const Object& instance, ComponentInstance ref,
                       const std::string& name) {
  auto shape = doc::regenerate(Recipe{ref}, document.regenContext());
  if (!shape) return shape.status();
  Object updated = instance;
  updated.recipe.base = std::move(ref);
  updated.shape = shape.value();
  ChangeSet changes(document);
  changes.modify(updated);
  history.execute(changes.build(name));
  return Status::success();
}

}  // namespace

Status Modeler::setPlacementParameter(const ObjectId& instanceId, const ObjectId& objectId, const std::string& key,
                                      double value) {
  const Object* instance = document_.find(instanceId);
  const auto* current = instance ? std::get_if<ComponentInstance>(&instance->recipe.base) : nullptr;
  if (!current) return Status::failure("Not a component instance");
  if (instance->locked) return Status::failure("The object is locked");
  std::optional<double> shared;
  for (const PlacementParameter& p : placementParameters(instanceId)) {
    if (p.object == objectId && p.parameter.key == key) shared = p.shared;
  }
  if (!shared) return Status::failure("The component has no parameter '" + key + "' on that object");
  ComponentInstance ref = *current;
  ref.overrides.erase(std::remove_if(ref.overrides.begin(), ref.overrides.end(),
                                     [&](const ParameterOverride& p) { return p.object == objectId && p.key == key; }),
                      ref.overrides.end());
  if (std::abs(value - *shared) > 1e-9) ref.overrides.push_back({objectId, key, value});
  return changePlacement(document_, history_, *instance, std::move(ref), "Edit Placement");
}

Status Modeler::clearPlacementParameters(const ObjectId& instanceId) {
  const Object* instance = document_.find(instanceId);
  const auto* current = instance ? std::get_if<ComponentInstance>(&instance->recipe.base) : nullptr;
  if (!current) return Status::failure("Not a component instance");
  if (current->overrides.empty()) return Status::success();
  ComponentInstance ref = *current;
  ref.overrides.clear();
  return changePlacement(document_, history_, *instance, std::move(ref), "Reset Placement");
}

Result<std::string> Modeler::makeUnique(const ObjectId& instanceId) {
  using R = Result<std::string>;
  const Object* instance = document_.find(instanceId);
  const auto* ref = instance ? std::get_if<ComponentInstance>(&instance->recipe.base) : nullptr;
  if (!ref) return R::failure("Not a component instance");
  const ComponentDefinition* stored = document_.findDefinition(ref->definitionId);
  if (!stored) return R::failure("Definition not found");
  // The only placement: nothing to separate - unless it is stretched on its
  // own, then the stretch is baked into the definition (LIM-02).
  const bool only = document_.instanceCount(stored->id) < 2;
  if (only && ref->stretches.empty()) return R::failure("This is already the only placement of the component");
  auto written = placementDefinition(*stored, *ref, document_.regenContext());
  if (!written) return R::failure(written.error());

  ComponentDefinition definition = written.value();
  if (only) {
    // Baked into its own definition, the placement plain again.
    Object updated = *instance;
    auto& own = std::get<ComponentInstance>(updated.recipe.base);
    own.overrides.clear();
    own.stretches.clear();
    auto shape = buildInstanceShape(definition, own, document_.regenContext());
    if (!shape) return R::failure(shape.error());
    updated.shape = shape.value();
    ChangeSet changes(document_);
    changes.putDefinition(definition);
    changes.modify(updated);
    history_.execute(changes.build("Make Unique"));
    return definition.id;
  }
  definition.id = generateObjectId();
  // "Door" -> "Door #2", the first free number.
  for (int n = 2;; ++n) {
    const std::string name = stored->name + " #" + std::to_string(n);
    bool taken = false;
    for (const ComponentDefinition& d : document_.definitions()) taken = taken || d.name == name;
    if (!taken) {
      definition.name = name;
      break;
    }
  }
  std::map<ObjectId, ObjectId> ids;
  for (const Object& o : definition.objects) ids[o.id] = generateObjectId();
  for (Object& o : definition.objects) {
    o.id = ids[o.id];
    if (!o.parent.empty()) o.parent = ids[o.parent];
  }

  Object updated = *instance;
  ComponentInstance own{definition.id, {}, ref->mirrored, {}};  // its stretches are in the definition now
  auto shape = buildInstanceShape(definition, own, document_.regenContext());
  if (!shape) return R::failure(shape.error());
  updated.recipe.base = own;
  updated.shape = shape.value();
  if (updated.name == stored->name) updated.name = definition.name;

  ChangeSet changes(document_);
  changes.putDefinition(definition);
  changes.modify(updated);
  history_.execute(changes.build("Make Unique"));
  return definition.id;
}

Status Modeler::mirrorPlacement(const ObjectId& instanceId, int axis) {
  const Object* instance = document_.find(instanceId);
  const auto* current = instance ? std::get_if<ComponentInstance>(&instance->recipe.base) : nullptr;
  if (!current) return Status::failure("Not a component instance");
  if (axis < 0 || axis > 2) return Status::failure("Axis must be 0 (red), 1 (green) or 2 (blue)");
  if (instance->locked) return Status::failure("The object is locked");
  // In the parent's frame: mirror across the plane through the placement's
  // centre, normal to its axis. The new placement is that mirror composed
  // with the old one and the definition mirror, which is rigid again.
  const gp_Trsf& placement = instance->transform;
  const Bnd_Box box = geom::exactBounds(instance->shape.Moved(TopLoc_Location(placement)));
  if (box.IsVoid()) return Status::failure("The component has no geometry");
  const gp_Pnt center((box.CornerMin().XYZ() + box.CornerMax().XYZ()) / 2.0);
  const gp_Dir normal = gp_Dir(axis == 0 ? gp::DX() : axis == 1 ? gp::DY() : gp::DZ()).Transformed(placement);
  gp_Trsf across;
  across.SetMirror(gp_Ax2(center, normal));

  ComponentInstance ref = *current;
  ref.mirrored = !ref.mirrored;
  auto shape = doc::regenerate(Recipe{ref}, document_.regenContext());
  if (!shape) return shape.status();
  Object updated = *instance;
  updated.transform = rigid(across * placement * componentMirror());
  updated.recipe.base = ref;
  updated.shape = shape.value();
  ChangeSet changes(document_);
  changes.modify(updated);
  history_.execute(changes.build("Mirror"));
  return Status::success();
}

Status Modeler::setComponentOrigin(const ObjectId& instanceId, const gp_Pnt& worldOrigin,
                                   const std::optional<gp_Dir>& xDirection) {
  const Object* instance = document_.find(instanceId);
  const auto* ref = instance ? std::get_if<ComponentInstance>(&instance->recipe.base) : nullptr;
  if (!ref) return Status::failure("Not a component instance");
  const ComponentDefinition* stored = document_.findDefinition(ref->definitionId);
  if (!stored) return Status::failure("Definition not found");

  // The new frame in definition coordinates: T maps it to the old one.
  gp_Trsf toDefinition = document_.worldTransform(*instance).Inverted();
  if (ref->mirrored) toDefinition = componentMirror() * toDefinition;
  const gp_Pnt origin = worldOrigin.Transformed(toDefinition);
  gp_Trsf frame;
  if (xDirection) {
    const gp_Vec x = gp_Vec(*xDirection).Transformed(toDefinition);
    if (std::hypot(x.X(), x.Y()) < 1e-9) return Status::failure("The red axis must not point along the blue axis");
    frame.SetRotation(gp_Ax1(gp::Origin(), gp::DZ()), std::atan2(x.Y(), x.X()));
  }
  gp_Trsf shift;
  shift.SetTranslation(gp_Vec(origin.XYZ()));
  frame = rigid(shift * frame);

  ComponentDefinition definition = *stored;
  const gp_Trsf back = frame.Inverted();
  for (Object& o : definition.objects) {
    if (o.parent.empty()) o.transform = back * o.transform;
  }
  auto shape = buildDefinitionShape(definition, document_.regenContext());
  if (!shape) return shape.status();
  definition.shape = shape.value();

  ChangeSet changes(document_);
  changes.putDefinition(definition);
  // Placements move by the frame (seen through their mirror) so nothing moves.
  const Status placed = updatePlacements(document_, changes, definition, [&](Object& o) {
    const bool mirrored = std::get<ComponentInstance>(o.recipe.base).mirrored;
    o.transform = mirrored ? rigid(o.transform * componentMirror() * frame * componentMirror())
                           : rigid(o.transform * frame);
  });
  if (!placed) return placed;
  history_.execute(changes.build("Set Component Axes"));
  return Status::success();
}

void Modeler::copyFrom(const Modeler& other) {
  history_.clear();
  document_.clear();
  document_.settings() = other.document_.settings();
  for (const Material& mat : other.document_.materials()) document_.putMaterial(mat);
  for (const auto& [hash, image] : other.document_.images()) document_.addImage(image.bytes, image.name);
  for (const ComponentDefinition& d : other.document_.definitions()) document_.putDefinition(d);
  for (const Object& o : other.document_.objects()) document_.insert(o);
  activeGroup_ = other.activeGroup_;
}

std::vector<std::string> Modeler::verifyRecipes(std::size_t& cursor, std::size_t budget) const {
  std::vector<std::string> warnings;
  const auto& objects = document_.objects();
  const RegenContext context = document_.regenContext();
  if (cursor == 0) {
    // Component definitions once: placements only reference them.
    for (const ComponentDefinition& d : document_.definitions()) {
      for (const Object& o : d.objects) {
        if (o.shape.IsNull() || o.isContainer()) continue;
        if (auto w = checkRecipe(o, context)) warnings.push_back(d.name + ": " + *w);
      }
    }
  }
  for (std::size_t end = std::min(objects.size(), cursor + budget); cursor < end; ++cursor) {
    const Object& o = objects[cursor];
    if (o.shape.IsNull() || o.isContainer() || std::holds_alternative<ComponentInstance>(o.recipe.base)) continue;
    if (auto w = checkRecipe(o, context)) warnings.push_back(*w);
  }
  return warnings;
}

Status Modeler::load(const std::string& path, std::vector<std::string>* warnings, bool verifyRecipes) {
  Document loaded;
  const Status status = loadDocument(loaded, path, warnings, verifyRecipes);
  if (!status) return status;
  activeGroup_.clear();
  history_.clear();
  document_.clear();
  document_.settings() = loaded.settings();
  for (const Material& mat : loaded.materials()) document_.putMaterial(mat);
  for (const auto& [hash, image] : loaded.images()) document_.addImage(image.bytes, image.name);
  for (const ComponentDefinition& d : loaded.definitions()) document_.putDefinition(d);
  for (const Object& o : loaded.objects()) document_.insert(o);
  return Status::success();
}

std::vector<geom::NamedShape> Modeler::exportParts(const std::vector<ObjectId>& ids) const {
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
    if (!o || o->shape.IsNull() || !document_.isVisible(id) || o->kind() == ObjectKind::Annotation ||
        o->kind() == ObjectKind::Guide) {
      continue;
    }
    parts.push_back(geom::NamedShape{document_.worldShape(*o), o->name,
                                     [&] {
                                       const Color c = document_.displayColor(*o);  // per-face colours: not yet
                                       return std::array<double, 3>{c.r, c.g, c.b};
                                     }(),
                                     o->layer == "Default" ? std::string() : o->layer});
  }
  return parts;
}

Status Modeler::exportStep(const std::string& path, const std::vector<ObjectId>& ids) const {
  const auto parts = exportParts(ids);
  if (parts.empty()) return Status::failure("Nothing to export");
  return geom::writeStepParts(parts, path);
}

Status Modeler::export3dm(const std::string& path, const std::vector<ObjectId>& ids) const {
  const auto parts = exportParts(ids);
  if (parts.empty()) return Status::failure("Nothing to export");
  return geom::write3dm(parts, path);
}

Result<std::vector<ObjectId>> Modeler::import3dm(const std::string& path, bool allowPartial,
                                                 geom::Import3dmReport* report) {
  geom::Import3dmReport local;
  geom::Import3dmReport& found = report ? *report : local;
  auto parts = geom::read3dm(path, &found);
  if (!parts) {
    if (found.objects == 0) return Result<std::vector<ObjectId>>::failure(parts.error());
    return Result<std::vector<ObjectId>>::failure(parts.error() + " (" + found.summary() + ")");
  }
  if (found.incomplete() && !allowPartial) {
    return Result<std::vector<ObjectId>>::failure(
        "The 3DM file did not come through whole (" + found.summary() +
        "); nothing was imported. Import again allowing a partial import to take what could be read");
  }
  return addImported(parts.value(), path, "Import 3DM");
}

Result<std::vector<ObjectId>> Modeler::importStep(const std::string& path) {
  auto parts = geom::readStepParts(path);
  if (!parts) return Result<std::vector<ObjectId>>::failure(parts.error());
  return addImported(parts.value(), path, "Import STEP");
}

Result<std::vector<ObjectId>> Modeler::addImported(const std::vector<geom::NamedShape>& parts,
                                                   const std::string& path, const std::string& commandName) {
  ChangeSet changes(document_);
  std::vector<ObjectId> ids;
  std::set<std::string> usedNames;
  for (const geom::NamedShape& part : parts) {
    Object o;
    o.id = generateObjectId();
    if (!part.layer.empty()) o.layer = part.layer;
    std::string name = part.name.empty() ? uniqueName("Imported") : part.name;
    // Keep names unique within the batch as well.
    for (int n = 2; usedNames.count(name); ++n) name = part.name + " (" + std::to_string(n) + ")";
    usedNames.insert(name);
    o.name = name;
    if (part.color) {
      o.color = Color{(*part.color)[0], (*part.color)[1], (*part.color)[2]};
      o.material = changes.colorMaterialFor(o.color);  // imported colours become materials
    }
    o.recipe.base = ImportedShape{part.shape, path};
    const Status built = rebuild(o);
    if (!built) return Result<std::vector<ObjectId>>::failure(built.message());
    ids.push_back(o.id);
    changes.add(o);
  }
  history_.execute(changes.build(commandName));
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

Result<ObjectId> Modeler::followMeAlong(const ObjectId& profileIdIn, const BooleanOperand& path) {
  // A copy: the caller's id may live in the document, which changes below.
  const ObjectId profileId = profileIdIn;
  const Object* profile = document_.find(profileId);
  if (!profile || profile->kind() != ObjectKind::Face) {
    return Result<ObjectId>::failure("Follow Me needs a face to sweep");
  }
  SweepFeature feature;
  feature.inputs.push_back(BooleanOperand{std::make_shared<const Recipe>(profile->recipe),
                                          document_.worldTransform(*profile), profile->name});
  feature.inputs.push_back(path);
  Object object;
  object.name = uniqueName("Follow Me");
  object.color = profile->color;
  object.material = profile->material;
  object.layer = profile->layer;
  object.recipe.base = std::move(feature);
  // The swept solid replaces its profile, as in SketchUp: one undo step.
  history_.beginGroup("Follow Me");
  auto id = addObject(std::move(object), "Follow Me");
  if (!id) {
    history_.abortGroup();
    return id;
  }
  if (const Status removed = remove({profileId}); !removed) {
    history_.abortGroup();
    return Result<ObjectId>::failure(removed.message());
  }
  history_.endGroup();
  return id;
}

Result<ObjectId> Modeler::followMe(const ObjectId& profileId, const ObjectId& pathId) {
  const Object* path = document_.find(pathId);
  if (!path || path->kind() != ObjectKind::Curve) return Result<ObjectId>::failure("Sweep path must be a curve");
  return followMeAlong(profileId, BooleanOperand{std::make_shared<const Recipe>(path->recipe),
                                                 document_.worldTransform(*path), path->name});
}

Result<ObjectId> Modeler::followMe(const ObjectId& profileId, const TopoDS_Wire& worldPath) {
  if (worldPath.IsNull()) return Result<ObjectId>::failure("Sweep path must be a curve");
  Recipe path;
  path.base = ImportedShape{worldPath, "Follow Me path"};
  return followMeAlong(profileId, BooleanOperand{std::make_shared<const Recipe>(path), gp_Trsf(), "Path"});
}

Result<ObjectId> Modeler::followMe(const ObjectId& profileId, const std::vector<TopoDS_Edge>& worldEdges) {
  const Object* profile = document_.find(profileId);
  if (!profile || profile->kind() != ObjectKind::Face) {
    return Result<ObjectId>::failure("Follow Me needs a face to sweep");
  }
  // The profile's own outline is not part of the path (it may have been
  // swept up with a selection).
  const TopoDS_Shape profileShape = document_.worldShape(*profile);
  TopTools_IndexedMapOfShape own;
  TopExp::MapShapes(profileShape, TopAbs_EDGE, own);
  std::vector<TopoDS_Edge> path;
  for (const TopoDS_Edge& e : worldEdges) {
    bool mine = false;
    for (int i = 1; i <= own.Extent() && !mine; ++i) mine = own(i).IsSame(e) || own(i).TShape() == e.TShape();
    if (!mine) path.push_back(e);
  }
  if (path.empty()) return Result<ObjectId>::failure("Select the edges of the path first (not the profile's own)");
  gp_Pnt centre;
  {
    GProp_GProps props;
    BRepGProp::SurfaceProperties(profileShape, props);
    centre = props.CentreOfMass();
  }
  auto wire = geom::pathFromEdges(path, centre);
  if (!wire) return Result<ObjectId>::failure(wire.error());
  return followMe(profileId, wire.value());
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
