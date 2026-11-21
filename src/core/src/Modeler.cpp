#include "sbr/doc/Modeler.h"
#include "sbr/doc/Schedules.h"
#include "sbr/geom/RoomRegions.h"

#include <BRepBuilderAPI_Transform.hxx>
#include "sbr/WallTypes.h"
#include "sbr/OpeningSymbols.h"
#include "sbr/ConstructionGuide.h"
#include "sbr/ConstructionFrame.h"
#include "sbr/ConstructionWall.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <deque>
#include <cmath>
#include <ctime>
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
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
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
#include <TopoDS_Iterator.hxx>

#include "sbr/doc/NativeFormat.h"
#include "sbr/doc/ChangeSummary.h"
#include "sbr/geom/Freeform.h"
#include "sbr/geom/Surfaces.h"
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
    for (ObjectDelta& d : modified_) {
      if (d.id == object.id) {  // changed again in the same step: one delta
        d.after = object;
        return;
      }
    }
    modified_.push_back(
        ObjectDelta{object.id, *document_.find(object.id), object, document_.indexOf(object.id)});
  }
  // The object as this change set leaves it (null: not changed here).
  const Object* pending(const ObjectId& id) const {
    for (const ObjectDelta& d : modified_) {
      if (d.id == id && d.after) return &*d.after;
    }
    return nullptr;
  }
  bool erased(const ObjectId& id) const {
    for (const ObjectDelta& d : removed_) {
      if (d.id == id) return true;
    }
    return false;
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

bool sameTrsf(const gp_Trsf& a, const gp_Trsf& b) {
  for (int r = 1; r <= 3; ++r) {
    for (int c = 1; c <= 4; ++c) {
      if (std::abs(a.Value(r, c) - b.Value(r, c)) > 1e-9) return false;
    }
  }
  return true;
}

// Objects following something (docs/42) after the moves in `changes`: one
// whose host moved goes with it, once; one moved itself (by hand, with its
// group or together with the host) keeps following from where it is now.
void followUp(const Document& document, ChangeSet& changes) {
  // Only objects someone asked to follow: one field looked at per object.
  std::vector<ObjectId> followers;
  for (const Object& o : document.objects()) {
    if (o.follow) followers.push_back(o.id);
  }
  if (followers.empty()) return;
  auto current = [&](const ObjectId& id) -> const Object* {
    if (const Object* p = changes.pending(id)) return p;
    return document.find(id);
  };
  std::function<gp_Trsf(const ObjectId&)> after = [&](const ObjectId& id) -> gp_Trsf {
    const Object* o = current(id);
    if (!o) return gp_Trsf();
    return o->parent.empty() ? o->transform : after(o->parent) * o->transform;
  };
  auto before = [&](const ObjectId& id) {
    const Object* o = document.find(id);
    return o ? document.worldTransform(*o) : gp_Trsf();
  };
  // Rounds: a follower moved by its host moves its own followers next.
  for (std::size_t round = 0; round <= followers.size(); ++round) {
    bool moved = false;
    for (const ObjectId& id : followers) {
      Object f = *current(id);
      if (!f.follow) continue;
      const ObjectId host = f.follow->host;
      const bool selfMoved = !sameTrsf(after(id), before(id));
      if (!document.find(host)) {
        // Suspended: it stays where it is put; the offset is taken again
        // once the host is back.
        if (selfMoved && !f.follow->rebase) {
          f.follow->rebase = true;
          changes.modify(f);
        }
        continue;
      }
      if (selfMoved) {
        // Put somewhere by hand (or moved with its group, or with the host):
        // it keeps following from there, never let go.
        const gp_Trsf offset = after(host).Inverted() * after(id);
        if (f.follow->rebase || !sameTrsf(offset, f.follow->offset)) {
          f.follow->offset = offset;
          f.follow->rebase = false;
          changes.modify(f);
        }
        continue;
      }
      if (f.locked || sameTrsf(after(host), before(host))) continue;
      // The host moved and it did not: once, to the same place on the host.
      const gp_Trsf offset = f.follow->rebase ? before(host).Inverted() * before(id) : f.follow->offset;
      const gp_Trsf parentWorld = f.parent.empty() ? gp_Trsf() : after(f.parent);
      f.transform = parentWorld.Inverted() * after(host) * offset;
      f.follow->offset = offset;
      f.follow->rebase = false;
      changes.modify(f);
      moved = true;
    }
    if (!moved) break;
  }
}


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

Modeler::Modeler() : history_(document_) {
  history_.setOnRecorded([this](const Command& step) { followCeilings(step); });
}

std::string Modeler::uniqueName(const std::string& base) const {
  // Names taken, gathered once (each candidate checked against every object
  // made drawing in a model of thousands ever slower).
  const std::string prefix = base + " ";
  std::unordered_set<std::string> taken;
  auto note = [&](const std::string& name) {
    if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0) taken.insert(name);
  };
  for (const Object& o : document_.objects()) note(o.name);
  for (const ComponentDefinition& d : document_.definitions()) note(d.name);
  for (int n = 1;; ++n) {
    std::string candidate = prefix + std::to_string(n);
    if (!taken.count(candidate)) return candidate;
  }
}

std::vector<ObjectId> Modeler::topmost(const std::vector<ObjectId>& ids) const {
  // Kept: those not inside another one given (a parent walk per object,
  // not a check against every other one).
  const std::unordered_set<ObjectId> given(ids.begin(), ids.end());
  std::unordered_set<ObjectId> seen;
  std::vector<ObjectId> out;
  for (const ObjectId& id : ids) {
    const Object* o = document_.find(id);
    if (!o || !seen.insert(id).second) continue;
    bool covered = false;
    for (const Object* a = o->parent.empty() ? nullptr : document_.find(o->parent); a && !covered;
         a = a->parent.empty() ? nullptr : document_.find(a->parent)) {
      covered = given.count(a->id) > 0;
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

std::vector<Object> Modeler::cloneSubtree(const ObjectId& id, const ObjectId& newParent,
                                          std::map<ObjectId, ObjectId>* copies) const {
  std::vector<Object> out;
  std::map<ObjectId, ObjectId> mapping;
  for (const ObjectId& old : document_.subtree(id)) {
    Object c = *document_.find(old);
    c.id = generateObjectId();
    mapping[old] = c.id;
    c.parent = old == id ? newParent : mapping[c.parent];
    out.push_back(std::move(c));
  }
  if (copies) copies->insert(mapping.begin(), mapping.end());
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

namespace {

std::string attributeOf(const Object& o, const std::string& key) {
  const auto it = o.attributes.find(key);
  return it == o.attributes.end() ? std::string() : it->second;
}

const GridSpec* gridOf(const Object* o) {
  if (!o) return nullptr;
  const auto* node = std::get_if<GroupNode>(&o->recipe.base);
  return node && node->grid ? &*node->grid : nullptr;
}

// What a grid's group holds, made from its description (group
// coordinates): each part keyed by "grid_part" and "grid_axis".
std::vector<Object> gridParts(const GridSpec& spec) {
  std::vector<Object> parts;
  auto part = [&](const std::string& kind, const std::string& key, const std::string& name, BaseFeature base) {
    Object o;
    o.name = name;
    o.layer = "Grid";
    o.attributes["grid_part"] = kind;
    o.attributes["grid_axis"] = key;
    o.recipe.base = std::move(base);
    parts.push_back(std::move(o));
  };
  auto ordered = [](std::vector<GridAxis> axes) {
    std::sort(axes.begin(), axes.end(), [](const GridAxis& a, const GridAxis& b) { return a.offset < b.offset; });
    return axes;
  };
  const std::vector<GridAxis> xs = ordered(spec.x), ys = ordered(spec.y);
  const double x0 = xs.empty() ? 0.0 : xs.front().offset;
  const double y0 = ys.empty() ? 0.0 : ys.front().offset;
  for (const GridAxis& a : xs) part("line", a.id, a.name, GuideLine{gp_Pnt(a.offset, y0, 0), gp::DY()});
  for (const GridAxis& a : ys) part("line", a.id, a.name, GuideLine{gp_Pnt(x0, a.offset, 0), gp::DX()});
  for (const GridAxis& a : xs) {
    part("label", a.id, "Grid " + a.name, TextNote{gp_Pnt(a.offset, y0 - spec.labelOffset, 0), std::nullopt, a.name});
  }
  for (const GridAxis& a : ys) {
    part("label", a.id, "Grid " + a.name, TextNote{gp_Pnt(x0 - spec.labelOffset, a.offset, 0), std::nullopt, a.name});
  }
  const double dimOffset = spec.labelOffset / 2;
  for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
    part("dimension", xs[i].id + ":" + xs[i + 1].id, "Grid Dimension",
         LinearDimension{gp_Pnt(xs[i].offset, y0, 0), gp_Pnt(xs[i + 1].offset, y0, 0), gp_Vec(0, -dimOffset, 0), {}});
  }
  for (std::size_t i = 0; i + 1 < ys.size(); ++i) {
    part("dimension", ys[i].id + ":" + ys[i + 1].id, "Grid Dimension",
         LinearDimension{gp_Pnt(x0, ys[i].offset, 0), gp_Pnt(x0, ys[i + 1].offset, 0), gp_Vec(-dimOffset, 0, 0), {}});
  }
  return parts;
}

Status checkGrid(const GridSpec& spec) {
  if (spec.x.empty() || spec.y.empty()) return Status::failure("A grid needs at least one line in each direction");
  if (!(spec.labelOffset > 0)) return Status::failure("Label offset must be positive");
  std::set<std::string> ids, names;
  for (const auto* list : {&spec.x, &spec.y}) {
    std::vector<double> at;
    for (const GridAxis& a : *list) {
      if (a.id.empty() || !ids.insert(a.id).second) return Status::failure("Grid line ids must be unique");
      if (a.name.empty()) return Status::failure("Grid lines need names");
      if (!names.insert(a.name).second) return Status::failure("Grid line name '" + a.name + "' is used twice");
      if (!std::isfinite(a.offset)) return Status::failure("Grid line positions must be finite");
      for (double o : at) {
        if (std::abs(o - a.offset) < tolerance::kMinDimension) {
          return Status::failure("Two grid lines at the same place ('" + a.name + "')");
        }
      }
      at.push_back(a.offset);
    }
  }
  return Status::success();
}

// The grid's group with `spec`, its parts made again in `changes`: a part
// still wanted keeps its id (and whether it is shown), others are added or
// erased. Parts erased already in `changes` stay erased.
Status regrid(const Document& document, const Object& group, const GridSpec& spec, ChangeSet& changes,
              const RegenContext& context) {
  Object g = group;
  std::get<GroupNode>(g.recipe.base).grid = spec;
  changes.modify(g);
  std::map<std::string, ObjectId> existing;
  for (const ObjectId& id : document.children(group.id)) {
    const Object* c = document.find(id);
    const std::string kind = attributeOf(*c, "grid_part");
    if (kind.empty()) continue;  // drawn into the group by hand: left alone
    existing[kind + "/" + attributeOf(*c, "grid_axis")] = id;
  }
  for (Object& part : gridParts(spec)) {
    const std::string key = part.attributes["grid_part"] + "/" + part.attributes["grid_axis"];
    const auto it = existing.find(key);
    if (it != existing.end()) {
      const ObjectId id = it->second;
      existing.erase(it);
      if (changes.erased(id)) continue;
      const Object* before = changes.pending(id);
      if (!before) before = document.find(id);
      Object o = *before;
      o.name = part.name;
      o.recipe = part.recipe;
      o.transform = gp_Trsf();
      if (const Status built = rebuild(o, context); !built) return built;
      changes.modify(o);
    } else {
      part.id = generateObjectId();
      part.parent = group.id;
      if (const Status built = rebuild(part, context); !built) return built;
      changes.add(part);
    }
  }
  for (const auto& [key, id] : existing) changes.erase(id);
  return Status::success();
}

// Lines of grids moved or erased in `changes` (inside the group, by hand):
// the grid takes the line's new place, or loses the line (erasing a label
// takes its line too), and is made again.
Status gridUp(const Document& document, ChangeSet& changes, const RegenContext& context) {
  for (const Object& g : document.objects()) {
    const GridSpec* spec = gridOf(&g);
    if (!spec || changes.erased(g.id)) continue;
    const Object* now = changes.pending(g.id);
    GridSpec next = *gridOf(now ? now : &g);
    bool changed = false;
    for (const ObjectId& id : document.children(g.id)) {
      const Object* c = document.find(id);
      const std::string kind = attributeOf(*c, "grid_part");
      if (kind != "line" && kind != "label") continue;
      const std::string axis = attributeOf(*c, "grid_axis");
      const bool alongY = std::any_of(next.x.begin(), next.x.end(), [&](const GridAxis& a) { return a.id == axis; });
      std::vector<GridAxis>& list = alongY ? next.x : next.y;
      const auto found = std::find_if(list.begin(), list.end(), [&](const GridAxis& a) { return a.id == axis; });
      if (found == list.end()) continue;
      if (changes.erased(id)) {
        list.erase(found);
        changed = true;
        continue;
      }
      const Object* moved = changes.pending(id);
      if (!moved || sameTrsf(moved->transform, gp_Trsf())) continue;
      gp_Pnt at;
      if (const auto* line = std::get_if<GuideLine>(&moved->recipe.base)) at = line->point;
      else if (const auto* note = std::get_if<TextNote>(&moved->recipe.base)) at = note->position;
      else continue;
      at.Transform(moved->transform);
      found->offset = alongY ? at.X() : at.Y();
      changed = true;
    }
    if (!changed) continue;
    if (next.x.empty() || next.y.empty()) {
      return Status::failure("A grid needs at least one line in each direction (erase the whole grid instead)");
    }
    if (const Status valid = checkGrid(next); !valid) return valid;
    if (const Status made = regrid(document, now ? *now : g, next, changes, context); !made) return made;
  }
  return Status::success();
}

// Where a crossing of a grid is (world), as `grid` and `spec` say.
std::optional<gp_Pnt> crossingOf(const GridSpec& spec, const gp_Trsf& gridWorld, const std::string& x,
                                 const std::string& y) {
  const GridAxis* a = nullptr;
  const GridAxis* b = nullptr;
  for (const GridAxis& g : spec.x) {
    if (g.id == x) a = &g;
  }
  for (const GridAxis& g : spec.y) {
    if (g.id == y) b = &g;
  }
  if (!a || !b) return std::nullopt;
  return gp_Pnt(a->offset, b->offset, 0).Transformed(gridWorld);
}

double levelHeight(const std::vector<FloorLevel>& levels, const std::string& id, bool& found) {
  for (const FloorLevel& l : levels) {
    if (l.id == id) {
      found = true;
      return l.elevation;
    }
  }
  found = false;
  return 0.0;
}

// Walls joined to walls under `roots` that move by `d` follow (docs/90 G-05); defined with
// the wall helpers below.
void followWallJoins(const Document& document, ChangeSet& changes, const RegenContext& context,
                     const std::vector<ObjectId>& roots, const gp_Vec& d, JoinFollow& report);

// Walls pinned to a grid (docs/129) after a grid change in `changes`: each end goes as far as
// its crossing did; walls joined to a wall that went sideways follow. Defined with the wall
// helpers below.
void wallPinUp(const Document& document, ChangeSet& changes, const RegenContext& context);
constexpr const char* kWallGridPin = "wall_grid_pin";

// Walls fitted to a roof (docs/97 N-05) are fitted again when they or their roof are among
// `changes`; defined with the roof helpers below.
void refitWallsToRoofs(const Document& document, ChangeSet& changes, const RegenContext& context);

// Floors opened for stairs (docs/103) are opened again when they or their stair are among
// `changes` (closed when the stair is erased); defined with the stair helpers below.
void refitStairOpenings(const Document& document, ChangeSet& changes, const RegenContext& context);
// A stair on levels: its foot moved by `dBase`, its top by `dTop` (the risers worked out again).
bool shiftStair(const Document& document, const Object& was, Object& o, double dBase, double dTop,
                const RegenContext& context);

// Walls and slabs on levels (docs/90 G-02) after the levels go from `before` to `after`;
// defined with the wall helpers below.
void levelUp(const Document& document, ChangeSet& changes, const RegenContext& context,
             const std::vector<FloorLevel>& before, const std::vector<FloorLevel>& after);

// Members pinned to grid crossings (docs/43) and levels (docs/45) after
// `changes` (and the levels going from `levelsBefore` to `levelsAfter`):
// each end moves by as much as its crossing and its level did. A member
// moved itself in the same step keeps where it was put.
void pinUp(const Document& document, ChangeSet& changes, const RegenContext& context,
           const std::vector<FloorLevel>& levelsBefore, const std::vector<FloorLevel>& levelsAfter) {
  std::vector<ObjectId> pinned;
  for (const Object& o : document.objects()) {
    if (o.gridPin && !changes.erased(o.id)) pinned.push_back(o.id);
  }
  if (pinned.empty()) return;
  auto current = [&](const ObjectId& id) -> const Object* {
    if (changes.erased(id)) return nullptr;
    if (const Object* p = changes.pending(id)) return p;
    return document.find(id);
  };
  std::function<gp_Trsf(const ObjectId&)> after = [&](const ObjectId& id) -> gp_Trsf {
    const Object* o = current(id);
    if (!o) return gp_Trsf();
    return o->parent.empty() ? o->transform : after(o->parent) * o->transform;
  };
  std::map<ObjectId, Object> frameRoots;
  std::map<ObjectId, nlohmann::json> frameManifests;
  for (const ObjectId& id : pinned) {
    const Object* was = document.find(id);
    const GridPin& pin = *was->gridPin;
    if (was->locked) continue;
    if (!sameTrsf(after(id), document.worldTransform(*was))) continue;  // moved itself
    const Object* gridBefore = document.find(pin.grid);
    const Object* gridAfter = current(pin.grid);
    const GridSpec* specBefore = gridOf(gridBefore);
    const GridSpec* specAfter = gridOf(gridAfter);
    auto delta = [&](const std::string& x, const std::string& y, const std::string& level) {
      gp_Vec d(0, 0, 0);
      if (specBefore && specAfter) {
        const auto from = crossingOf(*specBefore, document.worldTransform(*gridBefore), x, y);
        const auto to = crossingOf(*specAfter, after(pin.grid), x, y);
        if (from && to) d = gp_Vec(*from, *to);
      }
      if (!level.empty()) {
        bool had = false, has = false;
        const double from = levelHeight(levelsBefore, level, had);
        const double to = levelHeight(levelsAfter, level, has);
        if (had && has) d += gp_Vec(0, 0, to - from);
      }
      return d;
    };
    const gp_Vec dStart = delta(pin.startX, pin.startY, pin.startLevel);
    const gp_Vec dEnd = delta(pin.endX, pin.endY, pin.endLevel);
    if (dStart.Magnitude() < 1e-9 && dEnd.Magnitude() < 1e-9) continue;
    Object o = *current(id);
    if (auto* member = std::get_if<LinearMember>(&o.recipe.base)) {
      const gp_Trsf toLocal = after(id).Inverted();
      member->start.Translate(dStart.Transformed(toLocal));
      member->end.Translate(dEnd.Transformed(toLocal));
      if (!rebuild(o, context)) continue;
    } else if (std::holds_alternative<ComponentInstance>(o.recipe.base)) {
      if (!construction::moveFrameInstance(o, document, after(id), dStart, dEnd)) continue;
    } else
      continue;
    changes.modify(o);
    // An automatic level/grid move is not a hand edit. Advance only clean
    // generated stamps, keeping earlier manual edits dirty for regeneration.
    ObjectId parent = was->parent;
    while (!parent.empty()) {
      const Object* container = document.find(parent);
      if (!container) break;
      const auto* group = std::get_if<GroupNode>(&container->recipe.base);
      if (group && !group->constructionFrame.empty()) {
        auto found = frameRoots.find(parent);
        if (found == frameRoots.end()) {
          frameRoots.emplace(parent, *container);
          frameManifests.emplace(parent, nlohmann::json::parse(group->constructionFrame));
        }
        auto& manifest = frameManifests.at(parent);
        if (manifest.at("objects").contains(id) &&
            manifest.at("objects").at(id) == construction::frameFingerprint(*was)) {
          manifest["objects"][id] = construction::frameFingerprint(o);
        }
        break;
      }
      parent = container->parent;
    }
  }
  for (auto& [id, root] : frameRoots) {
    std::get<GroupNode>(root.recipe.base).constructionFrame = frameManifests.at(id).dump();
    changes.modify(root);
  }
}

// Everything that goes with moves and grid changes in `changes`: grids
// take moved lines, pinned members follow their crossings, objects
// follow their hosts (docs/42).
Status settleMoves(const Document& document, ChangeSet& changes, const RegenContext& context) {
  if (const Status grids = gridUp(document, changes, context); !grids) return grids;
  pinUp(document, changes, context, document.settings().levels, document.settings().levels);
  wallPinUp(document, changes, context);
  followUp(document, changes);
  return Status::success();
}

}  // namespace

Result<Modeler::GridLines> Modeler::createGridLines(const gp_Pnt& origin, const std::vector<double>& xSpacings,
                                                    const std::vector<double>& ySpacings, double labelOffset) {
  for (const auto* list : {&xSpacings, &ySpacings}) {
    for (double s : *list) {
      if (!(s > tolerance::kMinDimension)) return Result<GridLines>::failure("Grid spacings must be positive");
    }
  }
  GridSpec spec;
  spec.labelOffset = labelOffset;
  auto axes = [&](const std::string& prefix, const std::vector<double>& spacings) {
    std::vector<GridAxis> out;
    double at = 0.0;
    for (std::size_t i = 0; i <= spacings.size(); ++i) {
      if (i > 0) at += spacings[i - 1];
      out.push_back({"a" + std::to_string(spec.nextId++), prefix + std::to_string(i + 1), at});
    }
    return out;
  };
  spec.x = axes("X", xSpacings);
  spec.y = axes("Y", ySpacings);
  if (const Status valid = checkGrid(spec); !valid) return Result<GridLines>::failure(valid.message());

  Object group;
  group.id = generateObjectId();
  group.name = uniqueName("Grid");
  group.layer = "Grid";
  group.recipe.base = GroupNode{};
  gp_Trsf place;
  place.SetTranslation(gp_Vec(origin.XYZ()));
  // Drawn while a group is open: into that group, as other new geometry.
  if (!activeGroup_.empty()) {
    if (const Object* open = document_.find(activeGroup_)) {
      group.parent = activeGroup_;
      place = document_.worldTransform(*open).Inverted() * place;
    }
  }
  group.transform = place;
  ChangeSet changes(document_);
  changes.add(group);
  // Parts made against a document that does not hold the group yet: put
  // the group in first, then make the parts.
  history_.beginGroup("Grid Lines");
  history_.execute(changes.build("Grid Lines"));
  ChangeSet parts(document_);
  if (const Status made = regrid(document_, *document_.find(group.id), spec, parts, document_.regenContext()); !made) {
    history_.abortGroup();
    return Result<GridLines>::failure(made.message());
  }
  history_.execute(parts.build("Grid Lines"));
  history_.endGroup();

  GridLines out;
  out.grid = group.id;
  std::map<std::string, ObjectId> byKey;
  for (const ObjectId& id : document_.children(group.id)) {
    const Object* c = document_.find(id);
    byKey[attributeOf(*c, "grid_part") + "/" + attributeOf(*c, "grid_axis")] = id;
    if (attributeOf(*c, "grid_part") == "dimension") out.dimensions.push_back(id);
  }
  for (const auto* list : {&spec.x, &spec.y}) {
    for (const GridAxis& a : *list) {
      out.lines.push_back(byKey["line/" + a.id]);
      out.labels.push_back(byKey["label/" + a.id]);
    }
  }
  return out;
}

const GridSpec* Modeler::gridSpec(const ObjectId& grid) const { return gridOf(document_.find(grid)); }

Status Modeler::setLevels(std::vector<FloorLevel> levels) {
  std::set<std::string> ids, names;
  int next = 1;
  for (const FloorLevel& l : document_.settings().levels) {
    if (l.id.size() > 1 && l.id[0] == 'L') {
      try {
        next = std::max(next, std::stoi(l.id.substr(1)) + 1);
      } catch (...) {
      }
    }
  }
  for (FloorLevel& l : levels) {
    if (l.name.empty()) return Status::failure("Levels need names");
    if (!std::isfinite(l.elevation)) return Status::failure("Level heights must be numbers");
    if (!names.insert(l.name).second) return Status::failure("Level name '" + l.name + "' is used twice");
    if (l.id.empty()) l.id = "L" + std::to_string(next++);
    if (!ids.insert(l.id).second) return Status::failure("Level ids must be unique");
  }
  std::stable_sort(levels.begin(), levels.end(),
                   [](const FloorLevel& a, const FloorLevel& b) { return a.elevation < b.elevation; });
  const PersistentSettings before = PersistentSettings::of(document_.settings());
  PersistentSettings after = before;
  after.levels = levels;
  if (after == before) return Status::success();
  ChangeSet changes(document_);
  pinUp(document_, changes, document_.regenContext(), before.levels, after.levels);
  levelUp(document_, changes, document_.regenContext(), before.levels, after.levels);
  refitWallsToRoofs(document_, changes, document_.regenContext());
  refitStairOpenings(document_, changes, document_.regenContext());
  Command command = changes.build("Edit Levels");
  command.settings = SettingsDelta{before, after};
  history_.execute(std::move(command));
  return Status::success();
}

Status Modeler::setGrid(const ObjectId& grid, const GridSpec& spec) {
  const Object* g = document_.find(grid);
  if (!gridOf(g)) return Status::failure("Not a grid");
  if (g->locked) return Status::failure("'" + g->name + "' is locked");
  if (const Status valid = checkGrid(spec); !valid) return valid;
  GridSpec next = spec;
  // New ids never reuse old ones (pins of erased lines stay broken).
  for (const auto* list : {&next.x, &next.y}) {
    for (const GridAxis& a : *list) {
      if (a.id.size() > 1 && a.id[0] == 'a') {
        try {
          next.nextId = std::max(next.nextId, std::stoi(a.id.substr(1)) + 1);
        } catch (...) {
        }
      }
    }
  }
  next.nextId = std::max(next.nextId, gridOf(g)->nextId);
  if (next == *gridOf(g)) return Status::success();
  ChangeSet changes(document_);
  if (const Status made = regrid(document_, *g, next, changes, document_.regenContext()); !made) return made;
  if (const Status settled = settleMoves(document_, changes, document_.regenContext()); !settled) return settled;
  history_.execute(changes.build("Edit Grid"));
  return Status::success();
}

Result<Modeler::GridMembers> Modeler::placeMembersOnGrid(const ObjectId& grid, const GridMemberOptions& options) {
  using R = Result<GridMembers>;
  const Object* g = document_.find(grid);
  const GridSpec* spec = gridOf(g);
  if (!spec) return R::failure("Not a grid");
  if (!options.columns && !options.xBeams && !options.yBeams) return R::failure("Nothing to place");
  GridMemberOptions levelled = options;
  if (!options.baseLevel.empty() || !options.topLevel.empty()) {
    bool hasBase = false, hasTop = false;
    const double base = levelHeight(document_.settings().levels, options.baseLevel, hasBase);
    const double top = levelHeight(document_.settings().levels, options.topLevel, hasTop);
    if (!hasBase || !hasTop) return R::failure("Give both levels (base and top) that exist");
    // Levels are world heights; the grid's own plane may sit higher.
    levelled.base = base - document_.worldTransform(*g).TranslationPart().Z();
    levelled.height = top - base;
  }
  if (!(levelled.height > tolerance::kMinDimension) || !std::isfinite(levelled.base)) {
    return R::failure("The height must be positive");
  }
  auto pick = [](const std::vector<GridAxis>& all, const std::vector<std::string>& wanted) -> Result<std::vector<GridAxis>> {
    std::vector<GridAxis> out;
    for (const GridAxis& a : all) {
      if (wanted.empty() || std::find(wanted.begin(), wanted.end(), a.id) != wanted.end()) out.push_back(a);
    }
    for (const std::string& w : wanted) {
      if (std::none_of(all.begin(), all.end(), [&](const GridAxis& a) { return a.id == w; })) {
        return Result<std::vector<GridAxis>>::failure("No grid line with id '" + w + "'");
      }
    }
    std::sort(out.begin(), out.end(), [](const GridAxis& a, const GridAxis& b) { return a.offset < b.offset; });
    return out;
  };
  auto xs = pick(spec->x, options.x);
  if (!xs) return R::failure(xs.error());
  auto ys = pick(spec->y, options.y);
  if (!ys) return R::failure(ys.error());
  // Members go next to the grid (same parent), in its parent's coordinates.
  const gp_Trsf place = g->transform;
  const gp_Dir up = gp::DZ().Transformed(place);
  auto at = [&](const GridAxis& x, const GridAxis& y, double z) { return gp_Pnt(x.offset, y.offset, z).Transformed(place); };
  // Pins already there: the same thing is not placed twice.
  std::set<std::string> taken;
  // (A column of another storey, between other levels, is another one.)
  auto key = [](const GridPin& p) {
    return p.startX + "/" + p.startY + "@" + p.startLevel + "-" + p.endX + "/" + p.endY + "@" + p.endLevel;
  };
  for (const Object& o : document_.objects()) {
    if (o.gridPin && o.gridPin->grid == grid) taken.insert(key(*o.gridPin));
  }
  ChangeSet changes(document_);
  GridMembers out;
  const RegenContext context = document_.regenContext();
  auto add = [&](const MemberSpec& base, const gp_Pnt& from, const gp_Pnt& to, const GridPin& pin,
                 std::vector<ObjectId>& list) -> Status {
    const std::string k = key(pin);
    if (!taken.insert(k).second) {
      ++out.skipped;
      return Status::success();
    }
    MemberSpec s = base;
    s.start = from;
    s.end = to;
    s.up = up;
    s.name.clear();
    auto made = memberObject(s);
    if (!made) return Status::failure(made.error());
    Object o = *made;
    o.id = generateObjectId();
    o.parent = g->parent;
    o.gridPin = pin;
    // Names unique within this step too.
    o.name += " " + std::to_string(out.columns.size() + out.beams.size() + 1);
    if (const Status built = rebuild(o, context); !built) return built;
    changes.add(o);
    list.push_back(o.id);
    return Status::success();
  };
  const double top = levelled.base + levelled.height;
  const std::string& baseLevel = options.baseLevel;
  const std::string& topLevel = options.topLevel;
  if (options.columns) {
    for (const GridAxis& x : *xs) {
      for (const GridAxis& y : *ys) {
        const Status s = add(*options.columns, at(x, y, levelled.base), at(x, y, top),
                             GridPin{grid, x.id, y.id, x.id, y.id, baseLevel, topLevel}, out.columns);
        if (!s) return R::failure(s.message());
      }
    }
  }
  if (options.xBeams) {
    for (const GridAxis& x : *xs) {
      for (std::size_t i = 0; i + 1 < ys->size(); ++i) {
        const GridAxis& a = (*ys)[i];
        const GridAxis& b = (*ys)[i + 1];
        const Status s = add(*options.xBeams, at(x, a, top), at(x, b, top), GridPin{grid, x.id, a.id, x.id, b.id, topLevel, topLevel}, out.beams);
        if (!s) return R::failure(s.message());
      }
    }
  }
  if (options.yBeams) {
    for (const GridAxis& y : *ys) {
      for (std::size_t i = 0; i + 1 < xs->size(); ++i) {
        const GridAxis& a = (*xs)[i];
        const GridAxis& b = (*xs)[i + 1];
        const Status s = add(*options.yBeams, at(a, y, top), at(b, y, top), GridPin{grid, a.id, y.id, b.id, y.id, topLevel, topLevel}, out.beams);
        if (!s) return R::failure(s.message());
      }
    }
  }
  if (out.columns.empty() && out.beams.empty()) {
    return out.skipped ? R::failure("Every member asked for is there already") : R::failure("Nothing to place");
  }
  history_.execute(changes.build("Members on Grid"));
  return out;
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
  // Only objects whose box the line reaches (this runs as the pointer
  // moves, for the hint, over every object of a large model).
  Bnd_Box reach;
  for (const gp_Pnt& p : points) reach.Add(p);
  reach.Enlarge(1e-3);
  for (int pass = 0; pass < 2; ++pass) {
    for (const Object& o : document.objects()) {
      if (o.parent != context || o.shape.IsNull() || document.worldBounds(o).IsOut(reach)) continue;
      if (!document.isVisible(o.id)) continue;
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
  const Bnd_Box innerBox = document_.worldBounds(*inner);
  // Only a face whose box holds the inner one's (boxes first: a floor of
  // thousands of faces checked each geometrically took ever longer).
  auto holds = [&](const Bnd_Box& outer) {
    if (outer.IsVoid() || innerBox.IsVoid()) return false;
    double a0, b0, c0, a1, b1, c1, x0, y0, z0, x1, y1, z1;
    outer.Get(a0, b0, c0, a1, b1, c1);
    innerBox.Get(x0, y0, z0, x1, y1, z1);
    constexpr double slack = 1e-3;
    return x0 >= a0 - slack && y0 >= b0 - slack && z0 >= c0 - slack && x1 <= a1 + slack && y1 <= b1 + slack &&
           z1 <= c1 + slack;
  };
  for (const Object& o : document_.objects()) {
    if (o.id == profileId || o.parent != inner->parent || o.kind() != ObjectKind::Face ||
        !document_.isVisible(o.id) || !holds(document_.worldBounds(o))) {
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
    object.recipe.modifiers.push_back(SplitOp{*ref, local,std::nullopt});
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
  if (current->kind() != ObjectKind::Solid && current->kind() != ObjectKind::Face &&
      current->kind() != ObjectKind::Shell) return corner;
  // Solids: the two faces beside the edge must lie on one surface (one
  // plane, or one cylinder / cone a line was drawn across).
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
  if (beside.First().IsSame(beside.Last()) ||  // the seam of a round face
      !geom::sameSurface(TopoDS::Face(beside.First()), TopoDS::Face(beside.Last()))) return corner;
  // That segment only, as SketchUp's eraser: a step merging the two faces
  // beside it, added after everything else (lines drawn later across it
  // stay; dropping the split that drew it took the whole line and failed
  // when later lines crossed it). Nothing else may change: the same
  // volume, one face less.
  const double volume = geom::volume(current->shape);
  const int faces = faceCount(current->shape);
  const auto mark = edgeMark(TopoDS::Edge(edgeFaces.FindKey(index)));
  if (!mark) return corner;
  Object object = *current;
  object.recipe.modifiers.push_back(MergeAcrossOp{mark->point, mark->direction});
  if (!rebuild(object, document_.regenContext()) || faceCount(object.shape) != faces - 1 ||
      std::abs(geom::volume(object.shape) - volume) > 1e-6 * std::max(1.0, std::abs(volume))) {
    return R::failure("This line cannot be erased: the faces beside it do not join into one");
  }
  const Status s = commitModified(std::move(object), "Erase Line");
  if (!s) return R::failure(s.message());
  return id;
}

namespace {

// End points of an open curve object (world), if it is one.
std::optional<std::pair<gp_Pnt, gp_Pnt>> curveEnds(const Document& document, const Object& o) {
  if (o.kind() != ObjectKind::Curve || !document.isVisible(o.id)) return std::nullopt;
  const TopoDS_Shape shape = document.worldShape(o);
  TopoDS_Vertex a, b;
  if (shape.ShapeType() == TopAbs_EDGE) TopExp::Vertices(TopoDS::Edge(shape), a, b);
  else if (shape.ShapeType() == TopAbs_WIRE) TopExp::Vertices(TopoDS::Wire(shape), a, b);
  else return std::nullopt;  // A layer compound is not a single chain.
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
  // Ends met by no other end: compared with the ends in the same and the
  // neighbouring cells only (every pair took long with thousands of lines).
  constexpr double tol = 1e-3;
  auto cellOf = [](const gp_Pnt& p) {
    return std::array<long long, 3>{std::llround(std::floor(p.X() / (4 * tol))),
                                    std::llround(std::floor(p.Y() / (4 * tol))),
                                    std::llround(std::floor(p.Z() / (4 * tol)))};
  };
  std::map<std::array<long long, 3>, std::vector<std::size_t>> cells;
  for (std::size_t i = 0; i < ends.size(); ++i) cells[cellOf(ends[i])].push_back(i);
  std::vector<gp_Pnt> open;
  for (std::size_t i = 0; i < ends.size(); ++i) {
    int shared = 0;
    const auto c = cellOf(ends[i]);
    for (long long dx = -1; dx <= 1; ++dx) {
      for (long long dy = -1; dy <= 1; ++dy) {
        for (long long dz = -1; dz <= 1; ++dz) {
          const auto it = cells.find({c[0] + dx, c[1] + dy, c[2] + dz});
          if (it == cells.end()) continue;
          for (std::size_t j : it->second) shared += ends[i].Distance(ends[j]) < tol;
        }
      }
    }
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
  // Both ends must meet another curve for a loop (most lines drawn do not):
  // found by boxes first, without reading every curve's ends.
  auto metAt = [&](const gp_Pnt& p) {
    Bnd_Box at;
    at.Add(p);
    at.Enlarge(1e-3);
    for (const Object& o : document_.objects()) {
      if (o.id == curveId || o.parent != start->parent || o.shape.IsNull() || document_.worldBounds(o).IsOut(at)) {
        continue;
      }
      const auto ends = curveEnds(document_, o);
      if (ends && (ends->first.Distance(p) < 1e-3 || ends->second.Distance(p) < 1e-3)) return true;
    }
    return false;
  };
  if (!metAt(startEnds->first) || !metAt(startEnds->second)) {
    return Result<ObjectId>::failure("The curve does not close a loop");
  }
  std::vector<Candidate> candidates;
  for (const Object& o : document_.objects()) {
    if (o.id == curveId || o.parent != start->parent) continue;
    if (const auto ends = curveEnds(document_, o)) candidates.push_back({&o, ends->first, ends->second});
  }
  // The shortest chain from the curve's end back to its start, breadth
  // first over the meeting points (each point once): a depth-first walk
  // through every route froze for minutes on a grid of lines the curve
  // did not close against.
  constexpr double tol = 1e-3;
  std::vector<gp_Pnt> nodes;
  std::map<std::array<long long, 3>, std::vector<int>> cells;
  auto cellOf = [&](const gp_Pnt& p) {
    return std::array<long long, 3>{std::llround(std::floor(p.X() / (4 * tol))),
                                    std::llround(std::floor(p.Y() / (4 * tol))),
                                    std::llround(std::floor(p.Z() / (4 * tol)))};
  };
  auto nodeOf = [&](const gp_Pnt& p) {
    const auto c = cellOf(p);
    for (long long dx = -1; dx <= 1; ++dx) {
      for (long long dy = -1; dy <= 1; ++dy) {
        for (long long dz = -1; dz <= 1; ++dz) {
          const auto it = cells.find({c[0] + dx, c[1] + dy, c[2] + dz});
          if (it == cells.end()) continue;
          for (int n : it->second) {
            if (nodes[static_cast<std::size_t>(n)].Distance(p) < tol) return n;
          }
        }
      }
    }
    nodes.push_back(p);
    cells[c].push_back(static_cast<int>(nodes.size()) - 1);
    return static_cast<int>(nodes.size()) - 1;
  };
  const int from = nodeOf(startEnds->second), to = nodeOf(startEnds->first);
  std::vector<std::vector<std::pair<int, int>>> links;  // node -> (candidate, other node)
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const int a = nodeOf(candidates[i].a), b = nodeOf(candidates[i].b);
    if (static_cast<int>(links.size()) < static_cast<int>(nodes.size())) links.resize(nodes.size());
    if (a == b) continue;
    links[static_cast<std::size_t>(a)].push_back({static_cast<int>(i), b});
    links[static_cast<std::size_t>(b)].push_back({static_cast<int>(i), a});
  }
  links.resize(nodes.size());
  std::vector<std::pair<int, int>> came(nodes.size(), {-1, -1});  // (candidate, previous node)
  std::vector<int> depth(nodes.size(), -1);
  std::deque<int> queue{from};
  depth[static_cast<std::size_t>(from)] = 0;
  while (!queue.empty() && depth[static_cast<std::size_t>(to)] < 0) {
    const int at = queue.front();
    queue.pop_front();
    if (depth[static_cast<std::size_t>(at)] > 32) break;
    for (const auto& [candidate, next] : links[static_cast<std::size_t>(at)]) {
      if (depth[static_cast<std::size_t>(next)] >= 0) continue;
      depth[static_cast<std::size_t>(next)] = depth[static_cast<std::size_t>(at)] + 1;
      came[static_cast<std::size_t>(next)] = {candidate, at};
      queue.push_back(next);
    }
  }
  if (from == to || depth[static_cast<std::size_t>(to)] < 0) {
    return faceFromCurveJunctions(curveId, loopFound);
  }
  std::vector<int> path;
  for (int at = to; at != from; at = came[static_cast<std::size_t>(at)].second) {
    path.push_back(came[static_cast<std::size_t>(at)].first);
  }
  std::reverse(path.begin(), path.end());
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
  // Lines on the plane, kept only when joined to the chain (touching or
  // crossing it, directly or through other such lines): the rest cannot
  // bound a region with it, and arranging every line of a large drawing
  // took ever longer.
  struct Line {
    const Object* object;
    std::vector<gp_XY> pts;
    std::vector<gp_Pnt> world;
    bool joined = false;
  };
  const gp_Ax3 frame = plane->Position();
  auto to2d = [&](const gp_Pnt& p) {
    const gp_XYZ d = p.XYZ() - frame.Location().XYZ();
    return gp_XY(d.Dot(frame.XDirection().XYZ()), d.Dot(frame.YDirection().XYZ()));
  };
  std::vector<Line> lines;
  for (const Object* o : others) {
    std::vector<gp_Pnt> pts = worldPolyline(document, *o);
    if (std::any_of(pts.begin(), pts.end(), [&](const gp_Pnt& p) { return plane->Distance(p) > kTol; })) continue;
    Line line{o, {}, std::move(pts), false};
    for (const gp_Pnt& p : line.world) line.pts.push_back(to2d(p));
    lines.push_back(std::move(line));
  }
  // Segments by the cells they pass through (within kTol): column by
  // column, the rows the segment spans there. A slanted line covers about
  // as many cells as it is long, not as its bounding box is large (a 1 km
  // diagonal covered a million).
  constexpr double kCell = 1000.0;  // mm
  auto cellRange = [&](const gp_XY& a, const gp_XY& b, auto&& visit) {
    auto cell = [](double v) { return std::llround(std::floor(v / kCell)); };
    const gp_XY& left = a.X() <= b.X() ? a : b;
    const gp_XY& right = a.X() <= b.X() ? b : a;
    const double dx = right.X() - left.X();
    const long long x0 = cell(left.X() - kTol), x1 = cell(right.X() + kTol);
    for (long long x = x0; x <= x1; ++x) {
      // The part of the segment over this column (widened by kTol).
      const double xa = std::max(left.X(), x * kCell - kTol), xb = std::min(right.X(), (x + 1) * kCell + kTol);
      double ya = left.Y(), yb = right.Y();
      if (dx > 1e-12) {
        ya = left.Y() + (right.Y() - left.Y()) * std::clamp((xa - left.X()) / dx, 0.0, 1.0);
        yb = left.Y() + (right.Y() - left.Y()) * std::clamp((xb - left.X()) / dx, 0.0, 1.0);
      }
      const long long y0 = cell(std::min(ya, yb) - kTol), y1 = cell(std::max(ya, yb) + kTol);
      for (long long y = y0; y <= y1; ++y) visit(std::make_pair(x, y));
    }
  };
  std::map<std::pair<long long, long long>, std::vector<std::size_t>> cells;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    for (std::size_t k = 1; k < lines[i].pts.size(); ++k) {
      cellRange(lines[i].pts[k - 1], lines[i].pts[k], [&](const auto& c) { cells[c].push_back(i); });
    }
  }
  auto meet = [&](const gp_XY& a, const gp_XY& b, const gp_XY& c, const gp_XY& d) {
    // Segments ab and cd crossing or touching (within kTol).
    auto distance = [](const gp_XY& p, const gp_XY& u, const gp_XY& v) {
      const gp_XY uv = v - u;
      const double len2 = uv.SquareModulus();
      const double t = len2 < 1e-18 ? 0.0 : std::clamp((p - u).Dot(uv) / len2, 0.0, 1.0);
      return (p - (u + uv * t)).Modulus();
    };
    auto side = [](const gp_XY& u, const gp_XY& v, const gp_XY& p) { return (v - u).Crossed(p - u); };
    const double s1 = side(a, b, c), s2 = side(a, b, d), s3 = side(c, d, a), s4 = side(c, d, b);
    if (((s1 > 0 && s2 < 0) || (s1 < 0 && s2 > 0)) && ((s3 > 0 && s4 < 0) || (s3 < 0 && s4 > 0))) return true;
    return distance(c, a, b) < kTol || distance(d, a, b) < kTol || distance(a, c, d) < kTol || distance(b, c, d) < kTol;
  };
  std::vector<std::pair<gp_XY, gp_XY>> frontier;
  for (std::size_t k = 1; k < chain.size(); ++k) frontier.emplace_back(to2d(chain[k - 1]), to2d(chain[k]));
  while (!frontier.empty()) {
    const auto [a, b] = frontier.back();
    frontier.pop_back();
    cellRange(a, b, [&](const auto& c) {
      const auto it = cells.find(c);
      if (it == cells.end()) return;
      for (std::size_t i : it->second) {
        Line& line = lines[i];
        if (line.joined) continue;
        for (std::size_t k = 1; k < line.pts.size() && !line.joined; ++k) {
          line.joined = meet(a, b, line.pts[k - 1], line.pts[k]);
        }
        if (!line.joined) continue;
        for (std::size_t k = 1; k < line.pts.size(); ++k) frontier.emplace_back(line.pts[k - 1], line.pts[k]);
      }
    });
  }
  for (const Line& line : lines) {
    if (!line.joined) continue;
    for (std::size_t k = 1; k < line.world.size(); ++k) {
      setup.segments.emplace_back(line.world[k - 1], line.world[k]);
      setup.fresh.push_back(false);
      setup.owners.emplace_back(line.object, k - 1);
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
    if (o.parent != parent || o.shape.IsNull() || document.worldBounds(o).IsOut(reach)) continue;
    if (o.kind() != ObjectKind::Solid && o.kind() != ObjectKind::Shell) continue;
    if (document.isVisible(o.id)) out.push_back(&o);
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
  // Faces inside an opening (its jambs, head or sill) stand for the opening: deleting them
  // removes that opening and the wall is whole again there (docs/90 G-01), instead of tearing
  // those faces out of the wall. Only when nothing else is selected with them.
  {
    std::map<ObjectId, std::set<std::size_t>> byHost;
    bool reveals = true;
    for (const DeleteTarget& t : targets) {
      if (t.whole || t.faces.empty() || !t.edges.empty()) reveals = false;
      for (std::size_t i = 0; reveals && i < t.faces.size(); ++i) {
        const auto index = openingOfFace(t.id, t.faces[i]);
        if (index) byHost[t.id].insert(*index);
        else reveals = false;
      }
      if (!reveals) break;
    }
    if (reveals && !byHost.empty()) {
      DeleteOutcome out;
      history_.beginGroup("Remove Opening");
      for (const auto& [host, indices] : byHost) {
        const Status removed = removeOpenings(host, std::vector<std::size_t>(indices.begin(), indices.end()));
        if (!removed) {
          history_.abortGroup();
          return R::failure(removed.message());
        }
        out.openings += static_cast<int>(indices.size());
      }
      history_.endGroup();
      return out;
    }
  }
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
        dividing = (n0 && n1 && n0->Angle(*n1) < 1e-6) ||
                   (!beside[0].IsSame(beside[1]) && geom::sameSurface(beside[0], beside[1]));  // across a cylinder
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
        if (o.parent != activeGroup_ || o.locked || o.shape.IsNull() || document_.worldBounds(o).IsOut(reach)) continue;
        const bool profile = o.kind() == ObjectKind::Face;
        if ((pass == 0) != profile || (!profile && o.kind() != ObjectKind::Solid)) continue;
        if (!document_.isVisible(o.id)) continue;
        const TopoDS_Shape shape = document_.worldShape(o);
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

Result<bool> Modeler::splitWithCurve(const ObjectId& curveId) {
  const auto* object=document_.find(curveId);
  if(!object)return Result<bool>::failure("Object not found");
  if(object->kind()!=ObjectKind::Curve)return false;
  history_.beginGroup("Split Face");
  std::vector<ObjectId> now{curveId};
  const auto status=divideWithCurve(curveId,now);
  if(!status){history_.abortGroup();return Result<bool>::failure(status.message());}
  history_.endGroup();
  return now!=std::vector<ObjectId>{curveId};
}

namespace {

// Curved faces painted on their own and divided from `before` to `after`:
// every part carries the paint on (the mark of a curved face is one point
// on it, which may now be on the dividing line or in one part only).
void paintTheParts(std::vector<FaceMaterial>& faceMaterials, const TopoDS_Shape& before, const TopoDS_Shape& after) {
  std::vector<FaceMaterial> out;
  for (const FaceMaterial& fm : faceMaterials) {
    const auto old = fm.surface && !fm.plane ? fm.surface->resolve(before) : Result<TopoDS_Face>::failure("");
    if (!old) {
      out.push_back(fm);
      continue;
    }
    std::size_t parts = 0;
    for (TopExp_Explorer e(after, TopAbs_FACE); e.More(); e.Next()) {
      const TopoDS_Face part = TopoDS::Face(e.Current());
      const auto inside = geom::SurfaceRef::fromFace(part);
      if (!inside || !geom::sameSurface(part, *old) ||
          BRepClass_FaceClassifier(*old, inside->point, 1e-6).State() != TopAbs_IN) continue;
      if (auto again = FaceMaterial::of(part, fm.material)) {
        out.push_back(std::move(*again));
        ++parts;
      }
    }
    if (parts == 0) out.push_back(fm);
  }
  faceMaterials = std::move(out);
}

}  // namespace

Status Modeler::divideWithCurve(const ObjectId& curveId, std::vector<ObjectId>& now) {
  const auto* found=document_.find(curveId);
  if(!found)return Status::success();
  const Object curveObject=*found;
  if(curveObject.locked || !document_.isVisible(curveId) || curveObject.parent!=activeGroup_)return Status::success();
  const auto world=document_.worldShape(curveObject);
  const auto edges=chainOf(world);
  // Arc and interpolated-curve tools create one exact edge. Drawing layers
  // and arbitrary multi-edge imports are not treated as one sketch stroke.
  // A straight line divides flat faces elsewhere (splitAcross); here only
  // the curved ones it lies on.
  if(edges.size()!=1)return Status::success();
  const bool straight=BRepAdaptor_Curve(edges.front()).GetType()==GeomAbs_Line;
  const auto edge=edges.front();BRepAdaptor_Curve original(edge);
  const double first=original.FirstParameter(),last=original.LastParameter();
  Bnd_Box reach;BRepBndLib::Add(world,reach);reach.Enlarge(tolerance::kMinDimension);
  std::vector<ObjectId> targets;
  for(const auto& o:document_.objects()) {
    if(o.id==curveId || o.parent!=curveObject.parent || o.locked || !document_.isVisible(o.id) ||
       (o.kind()!=ObjectKind::Face && o.kind()!=ObjectKind::Solid && o.kind()!=ObjectKind::Shell) ||
       document_.worldBounds(o).IsOut(reach))continue;
    targets.push_back(o.id);
  }
  std::vector<std::pair<double,double>> used;
  for(const auto& id:targets) {
    // Revisit a solid after each split: the cutter may cross several of its
    // existing coplanar regions. Previously inserted edges divide nothing.
    // (Open shells: only their curved faces, below.)
    const bool flat=!straight && document_.find(id)->kind()!=ObjectKind::Shell;
    for(int pass=0;flat && pass<256;++pass) {
      const auto* current=document_.find(id);if(!current)break;
      const Object candidate=*current;
      const auto toLocal=document_.worldTransform(candidate).Inverted();
      const auto localCurve=world.Moved(TopLoc_Location(toLocal));
      bool divided=false;
      for(TopExp_Explorer e(candidate.shape,TopAbs_FACE);e.More();e.Next()) {
        const auto face=TopoDS::Face(e.Current());
        if(!geom::isPlanarFace(face))continue;
        Bnd_Box bounds;BRepBndLib::Add(face.Moved(TopLoc_Location(document_.worldTransform(candidate))),bounds);
        if(bounds.IsOut(reach))continue;
        auto result=geom::splitFaceWithCurve(candidate.shape,face,localCurve);
        if(!result)continue;
        // Common preserves the source curve's parameterization. Consume only
        // edge-to-edge or closed portions that were offered to the splitter.
        BRepAlgoAPI_Common common;TopTools_ListOfShape arguments,tools;arguments.Append(localCurve);tools.Append(face);
        common.SetArguments(arguments);common.SetTools(tools);common.SetNonDestructive(Standard_True);common.Build();
        std::vector<std::pair<double,double>> portions;
        if(common.IsDone() && !common.HasErrors())for(TopExp_Explorer pe(common.Shape(),TopAbs_EDGE);pe.More();pe.Next()) {
          BRepAdaptor_Curve c(TopoDS::Edge(pe.Current()));
          const auto a=c.Value(c.FirstParameter()),b=c.Value(c.LastParameter());
          if(geom::pointOnFaceBoundary(face,c.Value((c.FirstParameter()+c.LastParameter())/2)))continue;
          if(a.Distance(b)>1e-6 && (!geom::pointOnFaceBoundary(face,a)||!geom::pointOnFaceBoundary(face,b)))continue;
          portions.emplace_back(c.FirstParameter()-first,c.LastParameter()-first);
        }
        if(portions.empty())continue;
        Recipe cutter;cutter.base=ImportedShape{BRepBuilderAPI_Copy(localCurve).Shape(),"Planar split curve"};
        BooleanOperand operand{std::make_shared<const Recipe>(cutter),gp_Trsf(),curveObject.name};
        if(candidate.shape.ShapeType()==TopAbs_FACE) {
          auto parts=geom::splitPlanarFaceWithCurve(face,localCurve);
          if(!parts)return parts.status();
          std::string material=candidate.material;
          for(const auto& entry:resolveFaceMaterials(candidate).faces)if(entry.first==1)material=entry.second;
          std::size_t n=0;
          for(const auto& part:*parts) {
            auto inside=geom::SurfaceRef::fromFace(part);if(!inside)return inside.status();
            Object o=candidate;o.recipe=Recipe{};
            o.recipe.base=FacePart{{{std::make_shared<const Recipe>(candidate.recipe),gp_Trsf(),candidate.name},operand},{},inside->point};
            o.material=material;o.faceMaterials.clear();o.hiddenEdges.clear();
            if(n++==0){if(auto status=commitModified(std::move(o),"Split Face");!status)return status;}
            else{o.id=generateObjectId();o.name=uniqueName(candidate.name);auto added=addObject(std::move(o),"Split Face");if(!added)return added.status();}
          }
        } else {
          auto ref=geom::FaceRef::identify(face);if(!ref)return ref.status();
          Object o=candidate;o.recipe.modifiers.push_back(SplitOp{*ref,{},operand});
          if(auto status=commitModified(std::move(o),"Split Face");!status)return status;
        }
        used.insert(used.end(),portions.begin(),portions.end());
        divided=true;break;
      }
      if(!divided || candidate.shape.ShapeType()==TopAbs_FACE)break;
      if(pass==255)return Status::failure("Too many face regions for one curved stroke");
    }
    // Cylinders and cones: the parts of the stroke lying exactly on them,
    // drawn into all of them in one step (never projected, no chords).
    const auto* current=document_.find(id);
    if(!current || current->shape.ShapeType()==TopAbs_FACE)continue;
    const Object candidate=*current;
    const gp_Trsf toWorld=document_.worldTransform(candidate);
    const auto localCurve=world.Moved(TopLoc_Location(toWorld.Inverted()));
    BRep_Builder builder;TopoDS_Compound pieces;builder.MakeCompound(pieces);
    std::vector<std::pair<double,double>> portions;
    for(TopExp_Explorer e(candidate.shape,TopAbs_FACE);e.More();e.Next()) {
      const auto face=TopoDS::Face(e.Current());
      if(!geom::isCylinderOrCone(face))continue;
      Bnd_Box bounds;BRepBndLib::Add(face.Moved(TopLoc_Location(toWorld)),bounds);
      if(bounds.IsOut(reach))continue;
      for(const auto& piece:geom::curvePortionsAcross(face,localCurve)) {
        BRepAdaptor_Curve c(piece);
        portions.emplace_back(c.FirstParameter()-first,c.LastParameter()-first);
      }
    }
    if(portions.empty())continue;
    // Parts on faces side by side drawn as one edge (one line across a
    // divided cylinder is one tool for the splitter, not one per face).
    std::sort(portions.begin(),portions.end());
    std::vector<std::pair<double,double>> runs;
    for(const auto& p:portions) {
      if(!runs.empty() && p.first<=runs.back().second+1e-9)runs.back().second=std::max(runs.back().second,p.second);
      else runs.push_back(p);
    }
    const auto localEdge=TopoDS::Edge(edge.Moved(TopLoc_Location(toWorld.Inverted())));
    for(const auto& run:runs) {
      const auto piece=subEdge(localEdge,first+run.first,first+run.second);
      if(!piece.IsNull())builder.Add(pieces,piece);
    }
    const TopoDS_Shape copy=BRepBuilderAPI_Copy(pieces).Shape();
    std::vector<TopoDS_Edge> drawnEdges;
    for(TopExp_Explorer e(copy,TopAbs_EDGE);e.More();e.Next())drawnEdges.push_back(TopoDS::Edge(e.Current()));
    // Drawn into the current shape: the same as regenerating the recipe with
    // the step added, without going through every earlier step again.
    auto next=geom::imprintEdges(candidate.shape,drawnEdges);
    if(!next)return next.status();
    if(faceCount(*next)<=faceCount(candidate.shape))continue;  // it divided nothing
    Recipe drawn;drawn.base=ImportedShape{copy,"Curved face division"};
    Object o=candidate;
    o.recipe.modifiers.push_back(ImprintOp{BooleanOperand{std::make_shared<const Recipe>(drawn),gp_Trsf(),curveObject.name}});
    o.shape=*next;
    paintTheParts(o.faceMaterials,candidate.shape,o.shape);
    history_.execute(Command{"Split Face",{ObjectDelta{o.id,candidate,o,document_.indexOf(o.id)}},{}});
    used.insert(used.end(),portions.begin(),portions.end());
  }
  if(used.empty())return Status::success();
  if(auto status=remove({curveId});!status)return status;
  now.erase(std::remove(now.begin(),now.end(),curveId),now.end());
  for(const auto& interval:uncovered(last-first,used,1e-9)) {
    if(straight) {
      // What is left of a straight line stays a line (faces close on it).
      auto id=createPolyline({original.Value(first+interval.first),original.Value(first+interval.second)});
      if(!id)return id.status();
      Object o=*document_.find(*id);o.layer=curveObject.layer;o.material=curveObject.material;o.color=curveObject.color;
      o.attributes=curveObject.attributes;
      if(auto status=commitModified(std::move(o),"Split Face");!status)return status;
      now.push_back(*id);continue;
    }
    auto piece=subEdge(edge,first+interval.first,first+interval.second);if(piece.IsNull())continue;
    Object o=curveObject;o.id=generateObjectId();o.name=uniqueName(curveObject.name);o.recipe=Recipe{};
    o.recipe.base=ImportedShape{piece.Moved(TopLoc_Location(document_.worldTransform(curveObject).Inverted())),"Unused curve portion"};
    auto added=addObject(std::move(o),"Split Face");if(!added)return added.status();now.push_back(*added);
  }
  return Status::success();
}

Status Modeler::divideWithLine(const ObjectId& lineId, std::vector<ObjectId>& now) {
  const Object* found = document_.find(lineId);
  if (!found) return Status::success();
  const Object line = *found;
  const std::vector<gp_Pnt> points = worldPolyline(document_, line);
  if (points.size() < 2) return divideWithCurve(lineId,now);
  auto divided = splitAcross(points);
  if (!divided) return divided.status();
  if (divided->faces == 0) return divideWithCurve(lineId, now);  // perhaps along a cylinder
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
    Piece piece{&o, document_.worldBounds(o), false, {}, {}};
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
  // The joined object as its recipe makes it (the faces sewn once: what
  // closes a volume is told from this result, not from a trial sewing
  // done twice).
  auto build = [&](const std::vector<std::size_t>& joined, Object& result,
                   std::vector<std::pair<const Object*, gp_Trsf>>& inputs) {
    SewnFeature feature;
    inputs.clear();
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
    result = Object();
    result.id = generateObjectId();
    result.parent = parent;
    const Object& first = *pieces[joined.front()].object;
    result.layer = first.layer;
    result.color = first.color;
    result.attributes = first.attributes;
    result.recipe.base = std::move(feature);
    return rebuild(result, document_.regenContext()).ok();
  };
  Object result;
  std::vector<std::pair<const Object*, gp_Trsf>> inputs;
  bool built = false;
  bool open = false;
  std::vector<std::size_t> joined = reach(false, &open);
  if (!open && joined.size() >= 2) {
    built = build(joined, result, inputs);
    if (built && result.kind() != ObjectKind::Solid) {
      // Did not close after all: the faces meeting at an angle instead,
      // the same result when they are the same faces.
      if (reach(true, nullptr) != joined) built = false;
    }
  }
  if (!built) {
    joined = reach(true, nullptr);
    if (joined.size() < 2) return Status::success();
    if (!build(joined, result, inputs)) return Status::success();
  }
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

namespace {
// An imported drawing layer is an assembly of independent curves, not a
// single sketch chain. Moving it must preserve its contents and identity.
bool importedCurveBatch(const Object& object) {
  return std::holds_alternative<ImportedShape>(object.recipe.base) &&
         !object.shape.IsNull() && object.shape.ShapeType() == TopAbs_COMPOUND &&
         object.kind() == ObjectKind::Curve;
}
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
    if (!o || o->locked || !document_.isVisible(id) || importedCurveBatch(*o)) continue;
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
    if (o.id == id || o.parent != line.parent || o.shape.IsNull() || document_.worldBounds(o).IsOut(reach)) continue;
    const ObjectKind k = o.kind();
    if (k != ObjectKind::Curve && k != ObjectKind::Face && k != ObjectKind::Shell && k != ObjectKind::Solid) continue;
    if (!document_.isVisible(o.id)) continue;
    const TopoDS_Shape shape = document_.worldShape(o);
    collectEdges(shape, straight, curved);
    if (k == ObjectKind::Curve && !o.locked && !importedCurveBatch(o)) loose.push_back(o.id);
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
    if (o.id == id || o.parent != fresh.parent || o.kind() != ObjectKind::Face || !document_.isVisible(o.id) ||
        document_.worldBounds(o).IsOut(reach)) {
      continue;
    }
    gp_Pln on;
    const TopoDS_Face f = lonePlanarFace(document_.worldShape(o), &on);
    if (f.IsNull() || !on.Axis().Direction().IsParallel(plane.Axis().Direction(), 1e-9) ||
        plane.Distance(on.Location()) > tol) {
      continue;
    }
    others.push_back(o.id);
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

Status Modeler::setWallCalculation(const std::string& inputs) {
  try {
    const auto normalized = construction::normalizeWallInputs(inputs);
    editSettings(document_, history_, "Wall Quantity Inputs",
                 [&](PersistentSettings& s) { s.wallCalculation = normalized; });
    return Status::success();
  } catch (const std::exception& e) {
    return Status::failure(e.what());
  }
}

Status Modeler::setSun(const geom::SunSettings& sun) {
  if (const char* error = geom::sunSettingsError(sun); *error) return Status::failure(error);
  editSettings(document_, history_, "Sun", [&](PersistentSettings& s) {
    s.sun = sun;
    if (s.siteNorth) s.sun.northAngle = s.siteNorth->angle;
  });
  return Status::success();
}

Status Modeler::setSiteNorth(const SiteNorth& north) {
  if (!std::isfinite(north.angle) || north.source.empty() || north.checkedOn.empty())
    return Status::failure("Site north needs a finite angle, source and confirmation date");
  editSettings(document_, history_, "Site North", [&](PersistentSettings& s) {
    s.siteNorth = north;
    s.siteNorth->angle = std::remainder(north.angle, 360.0);
    s.sun.northAngle = s.siteNorth->angle;
  });
  return Status::success();
}

Status Modeler::setSiteNorthFromPoints(const gp_Pnt& tail, const gp_Pnt& tip, const std::string& source,
                                       const std::string& checkedOn) {
  const double x = tip.X() - tail.X(), y = tip.Y() - tail.Y();
  if (!std::isfinite(x) || !std::isfinite(y) || std::hypot(x, y) <= tolerance::kMinDimension)
    return Status::failure("Choose two distinct points in plan");
  return setSiteNorth({std::atan2(x, y) * 180.0 / std::acos(-1.0), source, checkedOn});
}

namespace {
bool oneLine(const std::string& s, std::size_t max) { return s.size() <= max && s.find_first_of("\r\n") == std::string::npos; }
}  // namespace

Status Modeler::setEnergyRoute(const std::string& route, int region, const std::string& source) {
  const auto& site = document_.settings().siteBoundary;
  if (!site) return Status::failure("敷地境界を先に登録してください（地域の区分は敷地に持たせます）");
  if (!route.empty() && route != "spec" && route != "performance" && route != "other")
    return Status::failure("適用ルートは spec / performance / other のどれかです");
  if (region < 0 || region > 8) return Status::failure("地域の区分は 1〜8 です（0：未入力）");
  if ((!route.empty() || region > 0) && source.empty()) return Status::failure("確認資料を入力してください");
  if (!oneLine(source, 300)) return Status::failure("確認資料は1行、100 字までです");
  const Object* outline = document_.find(site->outline);
  if (!outline) return Status::failure("敷地境界の輪郭が見つかりません");
  if (outline->locked) return Status::failure("The object is locked");
  Object object = *outline;
  for (const char* key : {"energy_route", "energy_region", "energy_source"}) object.attributes.erase(key);
  if (!route.empty()) object.attributes["energy_route"] = route;
  if (region > 0) object.attributes["energy_region"] = std::to_string(region);
  if (!route.empty() || region > 0) object.attributes["energy_source"] = source;
  ChangeSet changes(document_);
  changes.modify(object);
  history_.execute(changes.build("Energy Route"));
  return Status::success();
}

Status Modeler::setInsulation(const std::vector<ObjectId>& ids, const std::string& material, double thicknessMm,
                              double conductivity, double resistance, const std::string& placement, const std::string& source) {
  if (ids.empty()) return Status::failure("Nothing selected");
  if (!oneLine(material, 180) || !oneLine(source, 300)) return Status::failure("断熱材の名前・資料は1行です");
  if (!material.empty()) {
    if (!(thicknessMm > 0 && thicknessMm <= 1000)) return Status::failure("断熱材の厚さは 0〜1000 mm です");
    if (!std::isfinite(conductivity) || conductivity < 0 || conductivity > 10) return Status::failure("熱伝導率は 0〜10 W/(m·K) です（0：未入力）");
    if (!std::isfinite(resistance) || resistance < 0 || resistance > 100) return Status::failure("熱抵抗は 0〜100 m²·K/W です（0：未入力）");
    if (!placement.empty() && placement != "fill" && placement != "outside" && placement != "inside")
      return Status::failure("入れ方は fill / outside / inside のどれかです");
    if (source.empty()) return Status::failure("断熱材の資料（カタログ・仕様書など）を入力してください");
  }
  ChangeSet changes(document_);
  for (const ObjectId& id : ids) {
    const Object* o = document_.find(id);
    if (!o) return Status::failure("Object not found");
    const auto kind = o->attributes.find("kind");
    if (kind == o->attributes.end() || (kind->second != "wall" && kind->second != "slab" && kind->second != "roof"))
      return Status::failure("断熱は壁・床・屋根に入れます（" + o->name + " はどれでもありません）");
    if (o->locked) return Status::failure("The object is locked");
    Object object = *o;
    if (material.empty()) {
      object.attributes.erase("insulation");
    } else {
      nlohmann::json j{{"material", material}, {"thickness_mm", thicknessMm}, {"source", source}};
      if (conductivity > 0) j["conductivity"] = conductivity;
      if (resistance > 0) j["resistance"] = resistance;
      if (!placement.empty()) j["placement"] = placement;
      object.attributes["insulation"] = j.dump();
    }
    changes.modify(object);
  }
  history_.execute(changes.build("Insulation"));
  return Status::success();
}

Status Modeler::setSiteZone(const std::string& zone, const std::string& source) {
  const auto& site = document_.settings().siteBoundary;
  if (!site) return Status::failure("敷地境界を先に登録してください");
  if (!zone.empty() && zone != "residential" && zone != "industrial" && zone != "commercial")
    return Status::failure("用途地域の区分は residential / industrial / commercial のどれかです");
  if (!zone.empty() && source.empty()) return Status::failure("用途地域の確認資料を入力してください");
  if (source.size() > 300 || source.find_first_of("\r\n") != std::string::npos)
    return Status::failure("確認資料は1行、100 字までです");
  const Object* outline = document_.find(site->outline);
  if (!outline) return Status::failure("敷地境界の輪郭が見つかりません");
  if (outline->locked) return Status::failure("The object is locked");
  Object object = *outline;
  if (zone.empty()) {
    object.attributes.erase("use_zone");
    object.attributes.erase("use_zone_source");
  } else {
    object.attributes["use_zone"] = zone;
    object.attributes["use_zone_source"] = source;
  }
  ChangeSet changes(document_);
  changes.modify(object);
  history_.execute(changes.build("Site Zone"));
  return Status::success();
}

Status Modeler::setPermitConfirmation(int stage, bool confirmed, const std::string& by, const std::string& note,
                                      const std::string& on) {
  if (stage < 1 || stage > 12) return Status::failure("工程は 1〜12 です");
  nlohmann::json all = nlohmann::json::parse(document_.settings().permitConfirmations, nullptr, false);
  if (!all.is_object()) all = nlohmann::json::object();
  const std::string key = std::to_string(stage);
  if (!confirmed) {
    if (!all.contains(key)) return Status::success();
    all.erase(key);
  } else {
    if (!oneLine(by, 100) || !oneLine(note, 600)) return Status::failure("確認した人・メモは1行です");
    std::string date = on;
    if (date.empty()) {
      const std::time_t now = std::time(nullptr);
      std::tm local{};
#ifdef _WIN32
      localtime_s(&local, &now);
#else
      localtime_r(&now, &local);
#endif
      char buffer[16];
      std::strftime(buffer, sizeof buffer, "%Y-%m-%d", &local);
      date = buffer;
    }
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') return Status::failure("確認日は YYYY-MM-DD で入れます");
    const auto rows = permitChecks(document_, stage);
    nlohmann::json printed = nlohmann::json::object();
    for (const auto& [item, print] : permitRowFingerprints(rows)) printed[item] = print;
    nlohmann::json record = {{"fingerprint", permitStageFingerprint(rows)}, {"rows", printed}, {"on", date}};
    if (!by.empty()) record["by"] = by;
    if (!note.empty()) record["note"] = note;
    all[key] = record;
  }
  const std::string text = all.dump();
  editSettings(document_, history_, confirmed ? "Confirm Permit Stage" : "Clear Permit Confirmation",
               [&](PersistentSettings& s) { s.permitConfirmations = text; });
  return Status::success();
}

Status Modeler::setSiteBoundary(const std::optional<SiteBoundary>& boundary) {
  if (boundary) {
    if (boundary->source.empty()) return Status::failure("敷地境界の確認資料を入力してください");
    const auto geometry = document_.siteBoundaryGeometry(boundary->outline);
    if (!geometry) return Status::failure(geometry.error());
  }
  editSettings(document_, history_, "Site Boundary", [&](PersistentSettings& s) {
    auto next = boundary;
    if (next) {
      const bool sameOutline = s.siteBoundary && s.siteBoundary->outline == next->outline;
      next->road = sameOutline ? s.siteBoundary->road : std::nullopt;
      next->ground = sameOutline ? s.siteBoundary->ground : std::nullopt;
    }
    s.siteBoundary = std::move(next);
  });
  return Status::success();
}

Status Modeler::setSiteRoad(const std::array<gp_Pnt, 2>& edge, SiteRoad road) {
  if (road.source.empty() || !std::isfinite(road.widthMm) || !std::isfinite(road.frontageMm) ||
      road.widthMm < 0 || road.frontageMm < 0)
    return Status::failure("道路の根拠と、確認できた寸法を入力してください");
  const auto& boundary = document_.settings().siteBoundary;
  if (!boundary) return Status::failure("先に敷地境界を指定してください");
  const auto geometry = document_.siteBoundaryGeometry(boundary->outline);
  if (!geometry) return Status::failure(geometry.error());
  const auto match = geometry->matchEdge(edge[0], edge[1]);
  if (!match) return Status::failure("敷地境界の外周の辺を1本選んでください");
  const gp_Trsf toLocal = document_.worldTransform(*document_.find(boundary->outline)).Inverted();
  for (std::size_t i = 0; i < match->size(); ++i) {
    const gp_Pnt p = (*match)[i].Transformed(toLocal);
    road.localEdge[i] = {p.X(), p.Y(), p.Z()};
  }
  editSettings(document_, history_, "Site Road", [&](PersistentSettings& s) { s.siteBoundary->road = road; });
  return Status::success();
}

void Modeler::clearSiteRoad() {
  if (!document_.settings().siteBoundary || !document_.settings().siteBoundary->road) return;
  editSettings(document_, history_, "Clear Site Road", [](PersistentSettings& s) { s.siteBoundary->road.reset(); });
}

Status Modeler::setSiteGround(SiteGround ground, ObjectId building, bool confirmedPlacement) {
  const auto report = document_.siteGroundReport(ground);
  if (!report) return Status::failure(report.error());
  const auto& previous = document_.settings().siteBoundary->ground;
  if (previous) {
    const ObjectId linked = previous->placement ? previous->placement->building : ObjectId{};
    if (building.empty()) building = linked;
    if ((building != linked || !document_.checkSiteGroundPlacement(*previous)) &&
        !confirmedPlacement) return Status::failure("接地外周・測点と建物の現在位置を照合して確定してください");
  }
  ground.placement.reset();
  if (!building.empty()) {
    const auto current = document_.siteGroundBuildingPlacement(ground.outline, building);
    if (!current) return Status::failure(current.error());
    ground.placement = *current;
  }
  editSettings(document_, history_, "Site Ground", [&](PersistentSettings& s) { s.siteBoundary->ground = std::move(ground); });
  return Status::success();
}

void Modeler::clearSiteGround() {
  if (!document_.settings().siteBoundary || !document_.settings().siteBoundary->ground) return;
  editSettings(document_, history_, "Clear Site Ground", [](PersistentSettings& s) { s.siteBoundary->ground.reset(); });
}

Status Modeler::setConstructionOverrides(const std::string& system, const std::string& patch) {
  try {
    construction::validateOverrides(system, patch);
    editSettings(document_, history_, "Construction Defaults", [&](PersistentSettings& s) {
      s.constructionOverrides[system] = patch;
    });
    return Status::success();
  } catch (const std::exception& e) {
    return Status::failure(e.what());
  }
}

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
  editSettings(document_, history_, visible ? "Show Section Cuts" : "Hide Section Cuts", [&](PersistentSettings& s) {
    s.sectionDisplay.cuts = visible;
    if (visible && !s.sections.empty() && std::none_of(s.sections.begin(), s.sections.end(), [](const auto& p) { return p.active; }))
      s.sections.back().active = true;
  });
}

void Modeler::updateSection(std::size_t index, const SectionPlane& plane) {
  if (index >= document_.settings().sections.size()) return;
  double length = 0;
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(plane.origin[i]) || !std::isfinite(plane.normal[i])) return;
    length += plane.normal[i] * plane.normal[i];
  }
  if (!std::isfinite(length) || length < 1e-20) return;
  editSettings(document_, history_, "Edit Section", [&](PersistentSettings& s) {
    if (plane.active) for (auto& p : s.sections) p.active = false;
    s.sections[index] = plane;
  });
}

void Modeler::activateSection(std::size_t index) {
  if (index >= document_.settings().sections.size()) return;
  editSettings(document_, history_, "Activate Section", [&](PersistentSettings& s) {
    for (std::size_t i = 0; i < s.sections.size(); ++i) s.sections[i].active = i == index;
    s.sectionDisplay.cuts = true;
  });
}

void Modeler::setSectionPlanesVisible(bool visible) {
  editSettings(document_, history_, "Section Plane Visibility", [&](PersistentSettings& s) {
    s.sectionDisplay.planes = visible;
  });
}

void Modeler::setSectionFillVisible(bool visible) {
  editSettings(document_, history_, "Section Fill Visibility", [&](PersistentSettings& s) { s.sectionDisplay.fill = visible; });
}

void Modeler::removeSection(std::size_t index) {
  if (index >= document_.settings().sections.size()) return;
  editSettings(document_, history_, "Delete Section", [&](PersistentSettings& s) {
    s.sections.erase(s.sections.begin() + index);
  });
}

bool Modeler::sectionVisible() const {
  if (!document_.settings().sectionDisplay.cuts) return false;
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
      s.sectionDisplay.planes = true;
      s.sectionDisplay.cuts = true;
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
  // A member's end face pushed / pulled along it: its length, kept as the
  // member's own parameter (docs/40). Only when the member then rebuilds;
  // otherwise as any other face.
  if (const auto* member = std::get_if<LinearMember>(&object.recipe.base); member && pickedNormal && !keepStartFace) {
    const gp_Vec along(member->start, member->end);
    gp_Pln plane;
    if (along.Magnitude() > tolerance::kMinDimension && pickedNormal->IsParallel(gp_Dir(along), 1e-9) &&
        geom::isPlanarFace(face, &plane)) {
      Object longer = object;
      auto& m = std::get<LinearMember>(longer.recipe.base);
      const gp_Vec move = gp_Vec(*pickedNormal) * distance;
      bool atEnd = plane.Distance(member->end) < tolerance::kLinear;
      bool atStart = !atEnd && plane.Distance(member->start) < tolerance::kLinear;
      if (atEnd) m.end.Translate(move);
      if (atStart) m.start.Translate(move);
      if ((atEnd || atStart) && m.start.Distance(m.end) >= tolerance::kMinDimension &&
          gp_Vec(m.start, m.end).Dot(along) > 0 && commitModified(std::move(longer), "Push/Pull")) {
        return PushPullResult{id, "length"};
      }
    }
  }
  if (!pickedNormal) {
    if (object.kind() != ObjectKind::Solid) {
      return Result<PushPullResult>::failure("Only planar faces of profiles can be pushed/pulled");
    }
    std::string curvedKey;
    // One part of a divided cylinder / cone (a line drawn across it): only
    // that part moves, with a step to the rest (SketchUp).
    bool part = false;
    for (TopExp_Explorer e(object.shape, TopAbs_FACE); e.More() && !part; e.Next()) {
      const TopoDS_Face other = TopoDS::Face(e.Current());
      part = other.TShape() != face.TShape() && geom::sameSurface(other, face);  // (face is moved here)
    }
    if (part) {
      const auto ref = geom::SurfaceRef::fromFace(face);
      if (!ref) return Result<PushPullResult>::failure(ref.error());
      object.recipe.modifiers.push_back(OffsetFaceOp{ref.value(), distance, true});
      curvedKey = opKey(object.recipe.modifiers.size() - 1);
    } else if (auto* circle = std::get_if<CircleProfile>(&object.recipe.base);  // a circle's side: its radius
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
    if (auto joined = joinTouching(*current, object, key)) return *joined;
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

std::optional<Result<PushPullResult>> Modeler::joinTouching(const Object& profileBefore, Object extruded,
                                                              const std::string& key) {
  // The new volume where it will be.
  Object built = extruded;
  if (!rebuild(built, document_.regenContext()) || built.kind() != ObjectKind::Solid) return std::nullopt;
  const gp_Trsf profileWorld = document_.worldTransform(profileBefore);
  Bnd_Box reach;
  BRepBndLib::Add(built.shape.Moved(TopLoc_Location(profileWorld)), reach);
  reach.Enlarge(tolerance::kLinear);
  const TopoDS_Shape volume = built.shape.Moved(TopLoc_Location(profileWorld));
  std::vector<const Object*> hosts;
  for (const Object& o : document_.objects()) {
    if (o.id == profileBefore.id || o.parent != profileBefore.parent || o.kind() != ObjectKind::Solid ||
        o.locked || !document_.isVisible(o.id) || document_.worldBounds(o).IsOut(reach)) {
      continue;
    }
    // Joined by a shared face or volume: the two make one solid (along an
    // edge or at a point only, or apart: they stay apart).
    const auto pair = geom::booleanCombine(geom::BooleanOp::Union, {document_.worldShape(o), volume});
    if (pair && pair->ShapeType() == TopAbs_SOLID) hosts.push_back(&o);
  }
  if (hosts.empty()) return std::nullopt;
  // All of them and the volume in one union; only when that is one solid
  // (touching along an edge or at a point only, they stay apart).
  Object result;
  result.id = generateObjectId();
  result.parent = profileBefore.parent;
  result.name = hosts.front()->name;
  result.color = hosts.front()->color;
  result.layer = hosts.front()->layer;
  result.material = hosts.front()->material;
  BooleanFeature feature;
  feature.kind = BooleanKind::Union;
  std::vector<std::pair<const Object*, gp_Trsf>> inputs;
  for (const Object* host : hosts) {
    const gp_Trsf at = document_.parentTransform(*host).Inverted() * document_.worldTransform(*host);
    feature.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(host->recipe), at, host->name});
    rememberShape(feature.operands.back().recipe, host->shape);
    inputs.emplace_back(host, at);
  }
  const gp_Trsf profileAt = document_.parentTransform(profileBefore).Inverted() * profileWorld;
  feature.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(extruded.recipe), profileAt,
                                            profileBefore.name});
  inputs.emplace_back(&profileBefore, profileAt);
  result.recipe.base = std::move(feature);
  if (!rebuild(result, document_.regenContext()) || result.shape.ShapeType() != TopAbs_SOLID) return std::nullopt;
  carryMaterials(result, inputs);
  ChangeSet changes(document_);
  for (const Object* host : hosts) changes.erase(host->id);
  changes.erase(profileBefore.id);
  changes.add(result);
  history_.execute(changes.build("Push/Pull (join)"));
  return Result<PushPullResult>(PushPullResult{result.id, "in" + std::to_string(hosts.size()) + "." + key});
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

namespace {

// A SketchUp placement can express the uniform scales without baking them into the definition.
std::optional<gp_Trsf> componentUniformScale(const ComponentInstance& instance) {
  if (!instance.overrides.empty() || instance.mirrored) return std::nullopt;
  gp_Trsf combined;
  for (const StretchOp& op : instance.stretches) {
    if (!std::isfinite(op.fx) || op.fx < 1e-6 || op.fx != op.fy || op.fx != op.fz) return std::nullopt;
    gp_Trsf scale;
    scale.SetScale(op.center, op.fx);
    combined = scale * combined;
  }
  return combined;
}

// buildDefinitionShape adds the leaves in definition order. Map their material
// assignments to the face indices of the placed compound.
std::map<int, std::string> componentFaceMaterials(const ComponentDefinition& definition, const TopoDS_Shape& shape) {
  std::map<int, std::string> materials;
  TopTools_IndexedMapOfShape allFaces;
  TopExp::MapShapes(shape, TopAbs_FACE, allFaces);
  TopoDS_Iterator placed(shape);
  for (const Object& leaf : definition.objects) {
    if (leaf.shape.IsNull()) continue;
    if (!placed.More()) break;
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(placed.Value(), TopAbs_FACE, faces);
    placed.Next();
    std::string base = leaf.material;
    for (std::string parent = leaf.parent; base.empty() && !parent.empty();) {
      const auto group = std::find_if(definition.objects.begin(), definition.objects.end(),
                                      [&](const Object& child) { return child.id == parent; });
      if (group == definition.objects.end()) break;
      base = group->material;
      parent = group->parent;
    }
    const auto resolved = resolveFaceMaterials(leaf).faces;
    const std::map<int, std::string> painted(resolved.begin(), resolved.end());
    for (int i = 1; i <= faces.Extent(); ++i) {
      const auto own = painted.find(i);
      const std::string& material = own == painted.end() ? base : own->second;
      if (const int index = allFaces.FindIndex(faces(i)); index > 0 && !material.empty())
        materials[index] = material;
    }
  }
  return materials;
}

}  // namespace

Result<Modeler::MaterialAt> Modeler::materialAt(const ObjectId& id, const TopoDS_Face& worldFace) const {
  const Object* o = document_.find(id);
  if (!o) return Result<MaterialAt>::failure("Object not found");
  MaterialAt out;
  const auto* instance = std::get_if<ComponentInstance>(&o->recipe.base);
  const auto* definition = instance && componentUniformScale(*instance) ? document_.findDefinition(instance->definitionId) : nullptr;
  if ((!o->faceMaterials.empty() || definition) && !o->shape.IsNull()) {
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
    if (out.source.empty() && definition && index > 0) {
      const auto materials = componentFaceMaterials(*definition, o->shape);
      const auto material = materials.find(index);
      if (material != materials.end()) {
        out.material = material->second;
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
  // Lines of a grid erased: the grid without them (docs/43).
  if (const Status grids = gridUp(document_, changes, document_.regenContext()); !grids) return grids;
  refitStairOpenings(document_, changes, document_.regenContext());  // a stair erased: its floor closes
  refitWallsToRoofs(document_, changes, document_.regenContext());   // a wall erased: its neighbours' mitres (docs/159)
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
  lastJoins_ = JoinFollow{};
  if (keepWallJoins_ && worldDelta.Form() == gp_Translation)
    followWallJoins(document_, changes, document_.regenContext(), roots, gp_Vec(worldDelta.TranslationPart()),
                    lastJoins_);
  refitWallsToRoofs(document_, changes, document_.regenContext());
  refitStairOpenings(document_, changes, document_.regenContext());
  if (const Status settled = settleMoves(document_, changes, document_.regenContext()); !settled) return settled;
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

// The operand at `path`, made writable (copy on write along the way).
BooleanOperand& writableOperand(Recipe& recipe, const std::vector<std::size_t>& path, std::size_t depth = 0) {
  auto& boolean = std::get<BooleanFeature>(recipe.base);
  BooleanOperand& op = boolean.operands[path[depth]];
  if (depth + 1 == path.size()) return op;
  auto inner = std::make_shared<Recipe>(*op.recipe);
  BooleanOperand& found = writableOperand(*inner, path, depth + 1);
  op.recipe = inner;  // the operand lives in `inner`, which the host now owns
  return found;
}

// A floor opened for a stair (docs/103): the slab's own recipe is operand 0 of a subtraction
// whose other operands are the openings, each with the id "stair:<the stair's id>".
constexpr const char* kStairHole = "stair:";

// A slab's own outline and push: the recipe itself, or what its stair openings are cut from.
// Null when it is cut by anything else.
const Recipe* slabCore(const Recipe& recipe) {
  const auto* cut = std::get_if<BooleanFeature>(&recipe.base);
  if (!cut) return &recipe;
  if (cut->kind != BooleanKind::Subtract || cut->operands.size() < 2 || !recipe.modifiers.empty() || !cut->operands.front().recipe)
    return nullptr;
  for (std::size_t i = 1; i < cut->operands.size(); ++i)
    if (cut->operands[i].id.rfind(kStairHole, 0) != 0) return nullptr;
  return cut->operands.front().recipe.get();
}

void setSlabCore(Recipe& recipe, Recipe core) {
  if (auto* cut = std::get_if<BooleanFeature>(&recipe.base)) cut->operands.front().recipe = std::make_shared<const Recipe>(std::move(core));
  else recipe = std::move(core);
}

// Removes the operand at `path` (copy on write along the way).
void eraseOperand(Recipe& recipe, const std::vector<std::size_t>& path, std::size_t depth = 0) {
  auto& boolean = std::get<BooleanFeature>(recipe.base);
  if (depth + 1 == path.size()) {
    boolean.operands.erase(boolean.operands.begin() + static_cast<std::ptrdiff_t>(path[depth]));
    return;
  }
  BooleanOperand& op = boolean.operands[path[depth]];
  Recipe inner = *op.recipe;
  eraseOperand(inner, path, depth + 1);
  op.recipe = std::make_shared<const Recipe>(std::move(inner));
}

// A Subtract left with nothing to subtract is its first operand: an inner one is replaced by
// that operand in its parent, the outermost by the operand's recipe with `placement` taking
// the operand's placement.
void collapseSubtracts(Recipe& recipe, gp_Trsf& placement) {
  auto* boolean = std::get_if<BooleanFeature>(&recipe.base);
  if (!boolean || boolean->kind != BooleanKind::Subtract || boolean->operands.empty() || !recipe.modifiers.empty())
    return;
  BooleanOperand& base = boolean->operands.front();
  if (!base.recipe) return;
  if (const auto* below = std::get_if<BooleanFeature>(&base.recipe->base);
      below && below->kind == BooleanKind::Subtract && below->operands.size() == 1) {
    Recipe inner = *base.recipe;
    gp_Trsf p = base.placement;
    collapseSubtracts(inner, p);
    base.recipe = std::make_shared<const Recipe>(std::move(inner));
    base.placement = p;
  }
  if (boolean->operands.size() != 1) return;
  placement = placement * base.placement;
  Recipe only = *base.recipe;  // `base` lives in `recipe`: copy before replacing it
  recipe = std::move(only);
}

// What an opening's fitting is stored under: its id, or its number while it has none.
std::string openingKey(const OpeningRef& o) { return o.operand->id.empty() ? std::to_string(o.index) : o.operand->id; }

nlohmann::json storedFittings(const Object& host) {
  if (const auto it = host.attributes.find("opening_fittings"); it != host.attributes.end()) {
    auto all = nlohmann::json::parse(it->second, nullptr, false);
    if (all.is_object()) return all;
  }
  return nlohmann::json::object();
}

// Gives every opening that has none an id of its own, and moves its fitting from its number to
// that id. From then on removing an opening cannot shift another opening's fitting.
void ensureOpeningIds(Object& object) {
  std::vector<std::pair<std::vector<std::size_t>, std::size_t>> missing;
  for (const OpeningRef& o : rectangularOpenings(object))
    if (o.operand->id.empty()) missing.emplace_back(o.path, o.index);
  if (missing.empty()) return;
  nlohmann::json fittings = storedFittings(object);
  for (const auto& [path, index] : missing) {
    const std::string id = "opening-" + generateObjectId();
    writableOperand(object.recipe, path).id = id;
    const std::string old = std::to_string(index);
    if (fittings.contains(old)) {
      fittings[id] = fittings.at(old);
      fittings.erase(old);
    }
  }
  if (object.attributes.count("opening_fittings")) object.attributes["opening_fittings"] = fittings.dump();
}

// The host's extent along u and in Z (world), from its vertices.
std::array<double, 4> hostExtent(const TopoDS_Shape& world, const gp_Vec& u) {
  std::array<double, 4> e{1e300, -1e300, 1e300, -1e300};
  for (TopExp_Explorer v(world, TopAbs_VERTEX); v.More(); v.Next()) {
    const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
    const double d = gp_Vec(p.XYZ()).Dot(u);
    e[0] = std::min(e[0], d);
    e[1] = std::max(e[1], d);
    e[2] = std::min(e[2], p.Z());
    e[3] = std::max(e[3], p.Z());
  }
  return e;
}

}  // namespace

Result<std::vector<Modeler::OpeningInfo>> Modeler::openings(const ObjectId& hostId) const {
  return openingsIn(document_, hostId);
}

std::optional<std::size_t> Modeler::openingOfFace(const ObjectId& hostId, const TopoDS_Face& worldFace) const {
  const Object* host = document_.find(hostId);
  if (!host || worldFace.IsNull()) return std::nullopt;
  const std::vector<OpeningExtent> extents = openingExtents(*host, document_.worldTransform(*host));
  if (extents.empty()) return std::nullopt;
  std::vector<gp_Pnt> corners;
  for (TopExp_Explorer v(worldFace, TopAbs_VERTEX); v.More(); v.Next())
    corners.push_back(BRep_Tool::Pnt(TopoDS::Vertex(v.Current())));
  if (corners.size() < 3) return std::nullopt;
  const double tol = 10 * tolerance::kLinear;
  for (const OpeningExtent& e : extents) {
    // A face of the hole lies within the opening's outline, through the wall. The wall's own
    // faces around the hole reach past it.
    const gp_Vec u(gp::DZ().Crossed(e.normal));
    bool inside = true, through = false;
    double n0 = 1e300, n1 = -1e300;
    for (const gp_Pnt& p : corners) {
      const double d = gp_Vec(p.XYZ()).Dot(u), n = gp_Vec(p.XYZ()).Dot(gp_Vec(e.normal));
      inside &= d >= e.u0 - tol && d <= e.u1 + tol && p.Z() >= e.z0 - tol && p.Z() <= e.z1 + tol;
      n0 = std::min(n0, n);
      n1 = std::max(n1, n);
    }
    through = n1 - n0 > tol;  // it runs through the wall, not along its face
    if (inside && through) return e.index;
  }
  return std::nullopt;
}

Result<std::vector<std::size_t>> Modeler::openingIndices(const ObjectId& hostId,
                                                         const std::vector<std::string>& ids) const {
  using R = Result<std::vector<std::size_t>>;
  const auto list = openings(hostId);
  if (!list) return R::failure(list.status().message());
  std::vector<std::size_t> out;
  for (const std::string& id : ids) {
    const auto it = std::find_if(list->begin(), list->end(), [&](const OpeningInfo& o) { return o.id == id; });
    if (it == list->end()) return R::failure("No opening '" + id + "' in this object");
    out.push_back(it->index);
  }
  return out;
}

namespace {
// The host with one opening moved (not rebuilt yet), or why it cannot go there.
Result<Object> withOpeningMoved(const Document& document_, const ObjectId& hostId, std::size_t index,
                                const gp_Vec& worldDelta) {
  using R = Result<Object>;
  const Object* current = document_.find(hostId);
  if (!current) return R::failure("Object not found");
  if (current->locked) return R::failure("The object is locked");
  Object object = *current;
  ensureOpeningIds(object);
  const gp_Trsf world = document_.worldTransform(*current);
  bool found = false;
  for (const OpeningRef& o : rectangularOpenings(object)) {
    if (o.index != index) continue;
    found = true;
    const gp_Trsf frame = world * o.placement;
    const gp_Dir n = gp_Dir(o.rect->plane.normal).Transformed(frame);
    if (std::abs(n.Z()) > 1e-6) return R::failure("Only openings in a vertical face can be moved this way");
    // Keep the opening in its face: drop the part of the move through the wall.
    const gp_Vec u(gp::DZ().Crossed(n));
    const gp_Vec inFace = u * worldDelta.Dot(u) + gp_Vec(gp::DZ()) * worldDelta.Z();
    if (inFace.Magnitude() < tolerance::kLinear) return R::failure("The opening would not move");
    Recipe recipe = *o.operand->recipe;  // operands are shared snapshots: copy on write
    auto& rect = std::get<RectangleProfile>(recipe.base);
    rect.plane.origin.Translate(inFace.Transformed(frame.Inverted()));
    replaceOperand(object.recipe, o.path, 0, std::move(recipe));
    break;
  }
  if (!found) return R::failure("No opening " + std::to_string(index) + " in this object");
  const std::vector<OpeningExtent> extents = openingExtents(object, world);
  const TopoDS_Shape shape = document_.worldShape(*current);
  for (const OpeningExtent& e : extents) {
    if (e.index != index) continue;
    const auto wall = hostExtent(shape, gp_Vec(gp::DZ().Crossed(e.normal)));
    if (e.u0 < wall[0] - tolerance::kLinear || e.u1 > wall[1] + tolerance::kLinear ||
        e.z0 < wall[2] - tolerance::kLinear || e.z1 > wall[3] + tolerance::kLinear)
      return R::failure("The opening does not fit on the wall there");
    for (const OpeningExtent& other : extents)
      if (other.index != index && openingsOverlap(other, e.normal, e.u0, e.u1, e.z0, e.z1))
        return R::failure("The opening would overlap opening " + std::to_string(other.index));
  }
  return object;
}
}  // namespace

Status Modeler::moveOpening(const ObjectId& hostId, std::size_t index, const gp_Vec& worldDelta) {
  auto moved = withOpeningMoved(document_, hostId, index, worldDelta);
  if (!moved) return moved.status();
  return commitModified(std::move(moved.value()), "Move Opening");
}

Status Modeler::canMoveOpening(const ObjectId& hostId, std::size_t index, const gp_Vec& worldDelta) const {
  return withOpeningMoved(document_, hostId, index, worldDelta).status();
}

Status Modeler::placeOpening(const ObjectId& hostId, std::size_t index, std::optional<double> offset,
                             std::optional<double> sill) {
  for (const std::optional<double>& d : {offset, sill})
    if (d && (!std::isfinite(*d) || *d < -tolerance::kLinear))
      return Status::failure("Opening positions must not be negative");
  if (!offset && !sill) return Status::failure("Give an offset or a sill height");
  const auto list = openings(hostId);
  if (!list) return list.status();
  for (const OpeningInfo& o : *list) {
    if (o.index != index) continue;
    if (std::abs(o.normal.Z()) > 1e-6) return Status::failure("Only openings in a vertical face can be moved this way");
    const gp_Vec u(gp::DZ().Crossed(o.normal));
    return moveOpening(hostId, index,
                       u * (offset ? *offset - o.offset : 0.0) + gp_Vec(gp::DZ()) * (sill ? *sill - o.sill : 0.0));
  }
  return Status::failure("No opening " + std::to_string(index) + " in this object");
}

namespace {
// The left of a wall made by its tool, looking along its line (defined with the wall helpers).
std::optional<gp_Dir> wallLeftSide(const Document& document, const Object& o);
}  // namespace

Result<std::string> Modeler::copyOpening(const ObjectId& hostId, std::size_t index, const gp_Vec& worldDelta,
                                         const std::optional<ObjectId>& target) {
  using R = Result<std::string>;
  const auto list = openings(hostId);
  if (!list) return R::failure(list.status().message());
  const auto original = std::find_if(list->begin(), list->end(), [&](const OpeningInfo& o) { return o.index == index; });
  if (original == list->end()) return R::failure("No opening " + std::to_string(index) + " in this object");
  if (std::abs(original->normal.Z()) > 1e-6) return R::failure("Only openings in a vertical face can be copied");
  const ObjectId into = target.value_or(hostId);
  const Object* destination = document_.find(into);
  if (!destination) return R::failure("Object not found");
  if (destination->locked) return R::failure("'" + destination->name + "' is locked");
  gp_Dir normal = original->normal;
  gp_Pnt at;
  double sill = original->sill;
  if (into == hostId) {
    // In its own face: along the wall and up / down.
    const gp_Vec u(gp::DZ().Crossed(original->normal));
    const gp_Vec inFace = u * worldDelta.Dot(u) + gp_Vec(gp::DZ()) * worldDelta.Z();
    if (inFace.Magnitude() < tolerance::kLinear) return R::failure("The copy would lie on the opening itself");
    at = original->center.Translated(inFace);
    sill += inFace.Z();
    if (sill < -tolerance::kLinear) return R::failure("The opening does not fit on the wall there");
  } else {
    // Another wall: the side facing the original's way (a wall made by its tool), at the point given.
    if (const auto left = wallLeftSide(document_, *destination)) {
      const double facing = gp_Vec(*left).Dot(gp_Vec(original->normal));
      normal = facing >= 0 ? *left : left->Reversed();
    }
    at = gp_Pnt(worldDelta.XYZ());
  }
  // What the openings were before: the new one is the one not among them.
  std::set<std::string> before;
  if (const auto there = openings(into)) {
    for (const OpeningInfo& o : *there) before.insert(o.id);
  }
  std::string fitting;
  if (const auto stored = document_.find(hostId)->attributes.find("opening_fittings");
      stored != document_.find(hostId)->attributes.end()) {
    const nlohmann::json all = nlohmann::json::parse(stored->second, nullptr, false);
    if (all.is_object() && all.contains(original->id)) fitting = all.at(original->id).dump();
  }
  std::string mark, product;
  if (const auto stored = document_.find(hostId)->attributes.find("opening_marks");
      stored != document_.find(hostId)->attributes.end()) {
    const nlohmann::json all = nlohmann::json::parse(stored->second, nullptr, false);
    if (all.is_object() && all.contains(original->id) && all.at(original->id).is_object()) {
      mark = all.at(original->id).value("mark", std::string());
      product = all.at(original->id).value("product", std::string());
    }
  }
  history_.beginGroup("Copy Opening");
  const auto cut = placeOpeningAt(into, normal, at, original->width, original->height, sill);
  if (!cut) {
    history_.abortGroup();
    return R::failure(cut.error());
  }
  const auto after = openings(into);
  std::optional<OpeningInfo> made;
  if (after)
    for (const OpeningInfo& o : *after)
      if (!before.count(o.id)) made = o;
  if (!made) {
    history_.abortGroup();
    return R::failure("The copy could not be found in the wall");
  }
  if (!fitting.empty()) {
    if (const Status s = setOpeningFittings(into, {made->index}, fitting); !s) {
      history_.abortGroup();
      return R::failure(s.message());
    }
  }
  if (!mark.empty() || !product.empty()) {
    if (const Status s = setOpeningMarks(into, {made->index}, mark, product); !s) {
      history_.abortGroup();
      return R::failure(s.message());
    }
  }
  history_.endGroup();
  return made->id;
}

Status Modeler::removeOpenings(const ObjectId& hostId, const std::vector<std::size_t>& indices) {
  const Object* current = document_.find(hostId);
  if (!current) return Status::failure("Object not found");
  if (current->locked) return Status::failure("The object is locked");
  if (indices.empty()) return Status::failure("Give the openings to remove");
  Object object = *current;
  ensureOpeningIds(object);
  std::set<std::string> ids;
  {
    const std::vector<OpeningRef> all = rectangularOpenings(object);
    for (const std::size_t index : indices) {
      const auto it = std::find_if(all.begin(), all.end(), [&](const OpeningRef& o) { return o.index == index; });
      if (it == all.end()) return Status::failure("No opening " + std::to_string(index) + " in this object");
      ids.insert(it->operand->id);
    }
  }
  nlohmann::json fittings = storedFittings(object);
  for (const std::string& id : ids) {
    // One at a time: removing an operand shifts the paths of the others.
    for (const OpeningRef& o : rectangularOpenings(object)) {
      if (o.operand->id != id) continue;
      eraseOperand(object.recipe, o.path);
      break;
    }
    fittings.erase(id);
  }
  if (object.attributes.count("opening_fittings")) object.attributes["opening_fittings"] = fittings.dump();
  gp_Trsf placement = object.transform;
  collapseSubtracts(object.recipe, placement);
  object.transform = placement;
  object.faceMaterials.clear();
  object.hiddenEdges.clear();
  if (const Status built = rebuild(object, document_.regenContext()); !built) return built;
  // Painted faces stay painted: carried from the wall as it was, in the new wall's own frame.
  carryMaterials(object, {{current, object.transform.Inverted() * current->transform}});
  ChangeSet changes(document_);
  changes.modify(object);
  history_.execute(changes.build("Remove Opening"));
  return Status::success();
}

Result<std::vector<Modeler::OpeningInfo>> Modeler::openingsIn(const Document& document_, const ObjectId& hostId) {
  const Object* host = document_.find(hostId);
  if (!host) return Result<std::vector<OpeningInfo>>::failure("Object not found");
  const gp_Trsf world = document_.worldTransform(*host);
  std::vector<OpeningInfo> out;
  for (const OpeningRef& o : rectangularOpenings(*host)) {
    OpeningInfo info;
    info.index = o.index;
    info.id = openingKey(o);
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
    if (along.Magnitude() > 0.5) {
      const auto wall = hostExtent(document_.worldShape(*host), along);
      info.offset = a0 - wall[0];
      info.sill = info.bottom - wall[2];
    }
    // The cutter is extruded through the host from the face: its distance is the thickness.
    for (const auto& m : o.operand->recipe->modifiers)
      if (const auto* e = std::get_if<ExtrudeOp>(&m)) info.depth = std::abs(e->distance);
    out.push_back(info);
  }
  return out;
}

Status Modeler::setOpeningFittings(const ObjectId& hostId, const std::vector<std::size_t>& indices,
                                   const std::string& fitting) {
  const Object* host = document_.find(hostId);
  if (!host) return Status::failure("Object not found");
  const auto list = openings(hostId);
  if (!list) return list.status();
  if (list->empty()) return Status::failure("This object has no openings");
  // docs/171: a fitting type by its name: its kind and sizes, the hands given kept.
  std::string given = fitting;
  {
    const nlohmann::json in = nlohmann::json::parse(fitting, nullptr, false);
    if (in.is_object() && in.contains("type_name") && !in.contains("type")) {
      const auto types = fittingTypes();
      const auto found = in.at("type_name").is_string() ? types.find(in.at("type_name").get<std::string>()) : types.end();
      if (found == types.end()) return Status::failure("No fitting type of that name is registered");
      nlohmann::json merged = nlohmann::json::parse(found->second);
      for (const char* hand : {"hinge", "swing", "slide"})
        if (in.contains(hand)) merged[hand] = in.at(hand);
      merged["type_name"] = found->first;
      given = merged.dump();
    }
  }
  const auto checked = draw::normalizeFitting(given);
  if (!checked) return checked.status();
  // Fittings are kept under the openings' ids, so they stay with their openings when another
  // one is removed. Openings from before ids existed get theirs here.
  Object object = *host;
  ensureOpeningIds(object);
  nlohmann::json all = storedFittings(object);
  const nlohmann::json value = nlohmann::json::parse(*checked);
  std::vector<std::size_t> targets = indices;
  if (targets.empty())
    for (const auto& o : *list) targets.push_back(o.index);
  const std::vector<OpeningRef> refs = rectangularOpenings(object);
  for (const std::size_t index : targets) {
    const auto it = std::find_if(refs.begin(), refs.end(), [&](const OpeningRef& o) { return o.index == index; });
    if (it == refs.end()) return Status::failure("No opening " + std::to_string(index));
    if (value.at("type") == "none") all.erase(openingKey(*it));
    else all[openingKey(*it)] = value;
  }
  object.attributes["opening_fittings"] = all.dump();
  ChangeSet changes(document_);
  changes.modify(object);
  history_.execute(changes.build("Set Attribute"));
  return Status::success();
}

namespace {
constexpr const char* kTypeName = "type_name";

nlohmann::json typeRegister(const Document& d) {
  nlohmann::json j = nlohmann::json::parse(d.settings().elementTypes, nullptr, false);
  return j.is_object() ? j : nlohmann::json::object();
}
const char* partOfKind(const std::string& kind) { return kind == "slab" ? "floor" : kind == "roof" ? "roof" : ""; }
void putValues(nlohmann::json& entry, const Modeler::ElementTypeChange& c) {
  if (c.thickness) entry["thickness"] = *c.thickness;
  if (c.pitch) entry["pitch"] = *c.pitch;
  if (c.eave) entry["eave"] = *c.eave;
  if (c.gableEave) entry["gable_eave"] = *c.gableEave;
}
Modeler::ElementTypeChange valuesOf(const nlohmann::json& entry) {
  Modeler::ElementTypeChange c;
  if (entry.contains("thickness")) c.thickness = entry.at("thickness").get<double>();
  if (entry.contains("pitch")) c.pitch = entry.at("pitch").get<double>();
  if (entry.contains("eave")) c.eave = entry.at("eave").get<double>();
  if (entry.contains("gable_eave")) c.gableEave = entry.at("gable_eave").get<double>();
  return c;
}
std::string typeValuesError(const std::string& part, const Modeler::ElementTypeChange& c) {
  if (part != "floor" && part != "roof") return "A type is for a floor or a roof";
  if (part == "floor" && (c.pitch || c.eave || c.gableEave)) return "A floor type has a thickness only";
  if (!c.thickness && !c.pitch && !c.eave && !c.gableEave) return "Give the type a value (thickness, pitch, eaves)";
  auto in = [](const std::optional<double>& v, double lo, double hi) { return !v || (std::isfinite(*v) && *v >= lo && *v <= hi); };
  if (!in(c.thickness, 10.0, 2000.0)) return "A type's thickness is 10 to 2000";
  if (c.pitch && !(std::isfinite(*c.pitch) && *c.pitch > 0.0 && *c.pitch <= 30.0)) return "A roof's pitch is above 0 and at most 30";
  if (!in(c.eave, 0.0, 3000.0) || !in(c.gableEave, 0.0, 3000.0)) return "A roof's overhangs are 0 to 3000";
  return {};
}
}  // namespace

Status Modeler::setTypeName(const std::vector<ObjectId>& ids, const std::string& name) {
  if (ids.empty()) return Status::failure("Nothing selected");
  if (name.size() > 40 || name.find_first_of("\r\n") != std::string::npos)
    return Status::failure("A type name is one line of at most 40 characters");
  const nlohmann::json reg = typeRegister(document_);
  const nlohmann::json* registered = !name.empty() && reg.contains(name) ? &reg.at(name) : nullptr;
  ChangeSet changes(document_);
  for (const ObjectId& id : ids) {
    const Object* o = document_.find(id);
    if (!o) return Status::failure("Object not found");
    const std::string kind = attributeOf(*o, "kind");
    if (kind != "slab" && kind != "roof") return Status::failure("Type names are for slabs and roofs ('" + o->name + "' is neither)");
    if (o->locked) return Status::failure("'" + o->name + "' is locked");
    if (registered && registered->value("part", "") != partOfKind(kind))
      return Status::failure("'" + name + "' is registered as a " + registered->value("part", "") + " type ('" + o->name + "' is not one)");
    Object named = *o;
    if (name.empty()) named.attributes.erase(kTypeName);
    else named.attributes[kTypeName] = name;
    changes.modify(named);
  }
  if (!registered) {
    history_.execute(changes.build("Set Type Name"));
    return Status::success();
  }
  // A registered type: named and given its values, in one step.
  history_.beginGroup("Set Type Name");
  history_.execute(changes.build("Set Type Name"));
  if (const Status s = applyElementValues(ids, valuesOf(*registered)); !s) {
    history_.abortGroup();
    return s;
  }
  history_.endGroup();
  return Status::success();
}

Status Modeler::applyElementValues(const std::vector<ObjectId>& ids, const ElementTypeChange& change) {
  for (const ObjectId& id : ids) {
    const Object* o = document_.find(id);
    if (!o) return Status::failure("Object not found");
    const std::string kind = attributeOf(*o, "kind");
    if (kind == "slab") {
      if (!change.thickness) continue;
      MemberChange c;
      c.thickness = *change.thickness;
      c.keep = "top";
      if (const auto done = changeMember(id, c); !done) return Status::failure(done.error());
    } else if (kind == "roof") {
      const auto info = roofInfo(id);
      if (!info) return Status::failure(info.error());
      geom::RoofSpec spec = info->spec;
      if (change.thickness) spec.thickness = *change.thickness;
      if (change.pitch) spec.pitch = *change.pitch;
      if (change.eave) spec.eave = *change.eave;
      if (change.gableEave) spec.gableEave = *change.gableEave;
      if (const Status s = changeRoof(id, spec); !s) return s;
    }
  }
  return Status::success();
}

std::vector<Modeler::ElementTypeDefinition> Modeler::elementTypes() const {
  std::vector<ElementTypeDefinition> out;
  const nlohmann::json reg = typeRegister(document_);  // kept: items() refers into it
  for (const auto& [name, entry] : reg.items()) {
    if (!entry.is_object() || entry.value("part", "") == "fitting") continue;  // fitting types: fittingTypes()
    ElementTypeDefinition t{name, entry.value("part", ""), valuesOf(entry), {}};
    if (entry.contains("layers") && entry.at("layers").is_array())
      for (const auto& l : entry.at("layers")) t.layers.push_back({l.value("name", ""), l.value("thickness", 0.0)});
    out.push_back(std::move(t));
  }
  return out;
}

std::vector<double> Modeler::typeLayerDepths(const Document& document, const Object& element) {
  const std::string kind = attributeOf(element, "kind");
  if (kind != "slab" && kind != "roof") return {};
  const std::string name = attributeOf(element, kTypeName);
  if (name.empty()) return {};
  const nlohmann::json reg = typeRegister(document);
  if (!reg.contains(name)) return {};
  const nlohmann::json& entry = reg.at(name);
  if (entry.value("part", "") != partOfKind(kind) || !entry.contains("layers") || !entry.at("layers").is_array()) return {};
  std::vector<double> depths;
  double sum = 0.0;
  for (const auto& l : entry.at("layers")) depths.push_back(sum += l.value("thickness", 0.0));
  if (depths.size() < 2) return {};
  depths.pop_back();
  double thickness = 0.0;
  if (kind == "roof") {
    // Across the slope, as the roof was made.
    const nlohmann::json spec = nlohmann::json::parse(attributeOf(element, "roof_spec"), nullptr, false);
    if (!spec.is_object()) return {};
    thickness = spec.value("thickness", 0.0);
  } else {
    const TopoDS_Shape shape = document.worldShape(element);
    if (shape.IsNull()) return {};
    const Bnd_Box b = geom::exactBounds(shape);
    if (b.IsVoid()) return {};
    thickness = b.CornerMax().Z() - b.CornerMin().Z();
  }
  if (std::abs(thickness - sum) > 0.5) return {};
  return depths;
}

Result<int> Modeler::defineElementType(const std::string& name, const std::string& part, const ElementTypeChange& given,
                                       const std::optional<std::vector<TypeLayer>>& layers) {
  using R = Result<int>;
  if (name.empty() || name.size() > 40 || name.find_first_of("\r\n") != std::string::npos)
    return R::failure("A type name is one line of at most 40 characters");
  ElementTypeChange values = given;
  nlohmann::json layerList = nlohmann::json::array();
  if (layers && !layers->empty()) {
    double sum = 0.0;
    for (const TypeLayer& l : *layers) {
      if (l.name.empty() || l.name.size() > 40 || l.name.find_first_of("\r\n") != std::string::npos)
        return R::failure("A layer's name is one line of at most 40 characters");
      if (!(std::isfinite(l.thickness) && l.thickness > 0.0 && l.thickness <= 1000.0)) return R::failure("A layer is above 0 and at most 1000 thick");
      sum += l.thickness;
      layerList.push_back({{"name", l.name}, {"thickness", l.thickness}});
    }
    if (values.thickness && std::abs(*values.thickness - sum) > 0.5)
      return R::failure("The layers add up to " + std::to_string(static_cast<int>(std::lround(sum))) + ", not the thickness given");
    values.thickness = sum;
  } else if (layers && !values.thickness && !values.pitch && !values.eave && !values.gableEave) {
    // Only taking the layers off: the thickness as registered.
    const nlohmann::json reg = typeRegister(document_);
    if (reg.contains(name) && reg.at(name).contains("thickness")) values.thickness = reg.at(name).at("thickness").get<double>();
  }
  if (const std::string error = typeValuesError(part, values); !error.empty()) return R::failure(error);
  std::vector<ObjectId> named;
  for (const Object& o : document_.objects()) {
    if (attributeOf(o, kTypeName) != name) continue;
    const std::string kind = attributeOf(o, "kind");
    if (kind != "slab" && kind != "roof") continue;
    if (partOfKind(kind) != part) return R::failure("'" + name + "' is used by " + (part == "floor" ? "roofs" : "floors"));
    if (o.locked) return R::failure("'" + o.name + "' is locked");
    named.push_back(o.id);
  }
  nlohmann::json reg = typeRegister(document_);
  nlohmann::json entry = reg.contains(name) && reg.at(name).value("part", "") == part ? reg.at(name) : nlohmann::json::object();
  entry["part"] = part;
  putValues(entry, values);
  if (layers && layers->empty()) entry.erase("layers");
  else if (layers) entry["layers"] = layerList;
  reg[name] = entry;
  history_.beginGroup("Define Type");
  editSettings(document_, history_, "Define Type", [&](PersistentSettings& s) { s.elementTypes = reg.dump(); });
  if (const Status s = applyElementValues(named, values); !s) {
    history_.abortGroup();
    return R::failure(s.message());
  }
  history_.endGroup();
  return static_cast<int>(named.size());
}

Result<int> Modeler::registerElementType(const ObjectId& id) {
  using R = Result<int>;
  const Object* o = document_.find(id);
  if (!o) return R::failure("Object not found");
  const std::string name = attributeOf(*o, kTypeName);
  if (name.empty()) return R::failure("It has no type name: give it one first");
  const std::string kind = attributeOf(*o, "kind");
  ElementTypeChange values;
  if (kind == "roof") {
    const auto info = roofInfo(id);
    if (!info) return R::failure(info.error());
    values.thickness = info->spec.thickness;
    values.pitch = info->spec.pitch;
    values.eave = info->spec.eave;
    values.gableEave = info->spec.gableEave;
  } else if (kind == "slab") {
    const auto info = memberInfo(id);
    if (!info) return R::failure(info.error());
    for (const auto& field : info->fields)
      if (field.key == "thickness") values.thickness = field.value;
    if (!values.thickness) return R::failure("Its thickness cannot be read");
  } else {
    return R::failure("Types are for slabs and roofs");
  }
  const auto changed = defineElementType(name, partOfKind(kind), values);
  if (!changed) return changed;
  return *changed - 1;  // the others
}

std::map<std::string, std::string> Modeler::fittingTypes() const {
  std::map<std::string, std::string> out;
  const nlohmann::json reg = typeRegister(document_);
  for (const auto& [name, entry] : reg.items())
    if (entry.is_object() && entry.value("part", "") == "fitting" && entry.contains("fitting")) out[name] = entry.at("fitting").dump();
  return out;
}

namespace {
// Every wall's fittings of a type's name, rewritten by `rewrite` (nullopt: as they are).
ChangeSet fittingsOfType(const Document& document, const std::string& name,
                         const std::function<std::optional<nlohmann::json>(const nlohmann::json&)>& rewrite, int& count) {
  ChangeSet changes(document);
  for (const Object& o : document.objects()) {
    const auto stored = o.attributes.find("opening_fittings");
    if (stored == o.attributes.end()) continue;
    nlohmann::json all = nlohmann::json::parse(stored->second, nullptr, false);
    if (!all.is_object()) continue;
    bool any = false;
    for (auto& [key, fitting] : all.items()) {
      if (!fitting.is_object() || fitting.value("type_name", std::string()) != name) continue;
      if (const auto next = rewrite(fitting)) {
        fitting = *next;
        any = true;
        ++count;
      }
    }
    if (!any) continue;
    Object object = o;
    object.attributes["opening_fittings"] = all.dump();
    changes.modify(object);
  }
  return changes;
}
}  // namespace

std::optional<Modeler::FittingOpeningSize> Modeler::fittingOpeningSize(const std::string& name) const {
  const auto reg = typeRegister(document_);
  const auto entry = reg.find(name);
  if (entry == reg.end() || !entry->is_object() || entry->value("part", "") != "fitting") return std::nullopt;
  const auto size = entry->find("opening_size");
  if (size == entry->end() || !size->is_object()) return std::nullopt;
  for (const char* key : {"width", "height", "sill"})
    if (!size->contains(key) || !size->at(key).is_number()) return std::nullopt;
  return FittingOpeningSize{size->at("width").get<double>(), size->at("height").get<double>(),
                            size->at("sill").get<double>()};
}

Result<int> Modeler::defineFittingType(const std::string& name, const std::string& fitting, const ObjectId& host,
                                     std::size_t index, std::optional<FittingOpeningSize> openingSize) {
  using R = Result<int>;
  if (name.empty() || name.size() > 120 || name.find_first_of("\r\n") != std::string::npos)
    return R::failure("A type name is one line of at most 40 characters");
  nlohmann::json in = nlohmann::json::parse(fitting, nullptr, false);
  if (!in.is_object()) return R::failure("fitting needs {\"type\": ...}");
  // The type is its kind and sizes; the hands are each opening's own.
  for (const char* key : {"type_name", "hinge", "swing", "slide"}) in.erase(key);
  const auto checked = draw::normalizeFitting(in.dump());
  if (!checked) return R::failure(checked.error());
  nlohmann::json type = nlohmann::json::parse(*checked);
  for (const char* key : {"hinge", "swing", "slide"}) type.erase(key);
  if (type.value("type", std::string()) == "none") return R::failure("A fitting type is a door or a window, not none");
  nlohmann::json reg = typeRegister(document_);
  if (reg.contains(name) && reg.at(name).value("part", "") != "fitting") return R::failure("'" + name + "' is a type of floors or roofs");
  if (!host.empty()) {
    const auto list = openings(host);
    if (!list) return R::failure(list.error());
    const auto source = std::find_if(list->begin(), list->end(), [index](const OpeningInfo& op) { return op.index == index; });
    if (source == list->end()) return R::failure("No opening " + std::to_string(index));
    if (!openingSize) openingSize = FittingOpeningSize{source->width, source->height, source->sill};
  }
  if (!openingSize) openingSize = fittingOpeningSize(name);
  if (openingSize) {
    const auto [w, h, sill] = *openingSize;
    if (!std::isfinite(w) || !std::isfinite(h) || !std::isfinite(sill) ||
        w <= tolerance::kMinDimension || h <= tolerance::kMinDimension || sill < 0)
      return R::failure("Opening width and height must be positive and sill must be nonnegative");
  }
  reg[name] = {{"part", "fitting"}, {"fitting", type}};
  if (openingSize)
    reg[name]["opening_size"] = {{"width", openingSize->width}, {"height", openingSize->height}, {"sill", openingSize->sill}};
  int count = 0;
  ChangeSet changes = fittingsOfType(document_, name, [&](const nlohmann::json& old) -> std::optional<nlohmann::json> {
    nlohmann::json next = type;
    for (const char* hand : {"hinge", "swing", "slide"})
      if (old.contains(hand)) next[hand] = old.at(hand);
    next["type_name"] = name;
    const auto again = draw::normalizeFitting(next.dump());
    if (!again) return std::nullopt;
    return nlohmann::json::parse(*again);
  }, count);
  history_.beginGroup("Define Fitting Type");
  editSettings(document_, history_, "Define Fitting Type", [&](PersistentSettings& s) { s.elementTypes = reg.dump(); });
  if (count > 0) history_.execute(changes.build("Define Fitting Type"));
  if (!host.empty()) {
    // The opening it was registered from takes the name (its hands kept).
    nlohmann::json own = {{"type_name", name}};
    const nlohmann::json given = nlohmann::json::parse(fitting, nullptr, false);
    for (const char* hand : {"hinge", "swing", "slide"})
      if (given.is_object() && given.contains(hand)) own[hand] = given.at(hand);
    if (const Status s = setOpeningFittings(host, {index}, own.dump()); !s) {
      history_.abortGroup();
      return R::failure(s.message());
    }
    ++count;
  }
  history_.endGroup();
  return count;
}

Result<int> Modeler::removeFittingType(const std::string& name) {
  using R = Result<int>;
  nlohmann::json reg = typeRegister(document_);
  if (!reg.contains(name) || reg.at(name).value("part", "") != "fitting") return R::failure("No fitting type '" + name + "' is registered");
  reg.erase(name);
  int count = 0;
  ChangeSet changes = fittingsOfType(document_, name, [](const nlohmann::json& old) -> std::optional<nlohmann::json> {
    nlohmann::json next = old;
    next.erase("type_name");
    return next;
  }, count);
  history_.beginGroup("Remove Fitting Type");
  editSettings(document_, history_, "Remove Fitting Type", [&](PersistentSettings& s) { s.elementTypes = reg.dump(); });
  if (count > 0) history_.execute(changes.build("Remove Fitting Type"));
  history_.endGroup();
  return count;
}

Status Modeler::removeElementType(const std::string& name) {
  nlohmann::json reg = typeRegister(document_);
  if (!reg.contains(name)) return Status::failure("No type '" + name + "' is registered");
  if (reg.at(name).value("part", "") == "fitting") return Status::failure("'" + name + "' is a fitting type: remove it as one");
  reg.erase(name);
  editSettings(document_, history_, "Remove Type", [&](PersistentSettings& s) { s.elementTypes = reg.dump(); });
  return Status::success();
}

Result<int> Modeler::changeElementType(const std::string& name, const ElementTypeChange& change) {
  using R = Result<int>;
  if (name.empty()) return R::failure("Name the type to change");
  std::vector<ObjectId> slabs, roofs;
  for (const Object& o : document_.objects()) {
    if (attributeOf(o, kTypeName) != name) continue;
    const std::string kind = attributeOf(o, "kind");
    if (kind == "slab") slabs.push_back(o.id);
    else if (kind == "roof") roofs.push_back(o.id);
  }
  if (slabs.empty() && roofs.empty()) return R::failure("Nothing has the type '" + name + "'");
  history_.beginGroup("Change Type");
  std::vector<ObjectId> all = slabs;
  all.insert(all.end(), roofs.begin(), roofs.end());
  if (const Status s = applyElementValues(all, change); !s) {
    history_.abortGroup();
    return R::failure(s.message());
  }
  // A registered type (docs/149) changes with them.
  nlohmann::json reg = typeRegister(document_);
  if (reg.contains(name)) {
    ElementTypeChange kept = change;
    if (reg.at(name).value("part", "") == "floor") kept.pitch = kept.eave = kept.gableEave = std::nullopt;
    putValues(reg[name], kept);
    editSettings(document_, history_, "Change Type", [&](PersistentSettings& s) { s.elementTypes = reg.dump(); });
  }
  history_.endGroup();
  return static_cast<int>(all.size());
}

Result<int> Modeler::matchElementType(const ObjectId& id) {
  using R = Result<int>;
  const Object* o = document_.find(id);
  if (!o) return R::failure("Object not found");
  const std::string name = attributeOf(*o, kTypeName);
  if (name.empty()) return R::failure("It has no type name: give it one first");
  int others = 0;
  for (const Object& other : document_.objects()) others += other.id != id && attributeOf(other, kTypeName) == name;
  if (others == 0) return 0;
  ElementTypeChange change;
  if (attributeOf(*o, "kind") == "roof") {
    const auto info = roofInfo(id);
    if (!info) return R::failure(info.error());
    change.thickness = info->spec.thickness;
    change.pitch = info->spec.pitch;
    change.eave = info->spec.eave;
    change.gableEave = info->spec.gableEave;
  } else {
    const auto info = memberInfo(id);
    if (!info) return R::failure(info.error());
    for (const auto& field : info->fields)
      if (field.key == "thickness") change.thickness = field.value;
    if (!change.thickness) return R::failure("Its thickness cannot be read");
  }
  const auto changed = changeElementType(name, change);
  if (!changed) return changed;
  return others;
}

Result<ObjectId> Modeler::makeCanopy(const ObjectId& host, std::size_t index, const CanopySpec& spec) {
  using R = Result<ObjectId>;
  if (!(spec.depth >= 100.0 && spec.depth <= 5000.0)) return R::failure("A canopy stands out 100 to 5000");
  if (!(spec.thickness >= 10.0 && spec.thickness <= 500.0)) return R::failure("A canopy is 10 to 500 thick");
  if (!(spec.extra >= 0.0 && spec.extra <= 2000.0) || !(spec.above >= 0.0 && spec.above <= 2000.0))
    return R::failure("A canopy's overhang at the sides and height over the opening are 0 to 2000");
  const auto list = openings(host);
  if (!list) return R::failure(list.status().message());
  const auto o = std::find_if(list->begin(), list->end(), [&](const OpeningInfo& x) { return x.index == index; });
  if (o == list->end()) return R::failure("No opening " + std::to_string(index) + " in this object");
  if (std::abs(o->normal.Z()) > 1e-6) return R::failure("A canopy goes over an opening in a wall");
  const gp_Vec n(o->normal.X(), o->normal.Y(), 0), u = gp_Vec(0, 0, 1).Crossed(n);
  const double half = o->width / 2 + spec.extra;
  const gp_Pnt face(o->center.X(), o->center.Y(), 0.0);
  const std::vector<gp_Pnt> outline = {face.Translated(u * -half), face.Translated(u * half), face.Translated(u * half + n * spec.depth),
                                       face.Translated(u * -half + n * spec.depth)};
  const double top = o->bottom + o->height + spec.above + spec.thickness;
  history_.beginGroup("Canopy");
  const auto made = createSlab(outline, top, spec.thickness);
  if (!made) {
    history_.abortGroup();
    return R::failure(made.error());
  }
  Object board = *document_.find(*made);
  board.name = uniqueName("Canopy");
  board.attributes["kind"] = "canopy";
  board.attributes["host"] = host;
  board.attributes["opening"] = o->id;
  board.attributes.erase("top_level");
  board.attributes.erase("base_level");
  // Where it stands out from (the face; the wall's line half its depth behind), for the building area.
  board.attributes["canopy"] = nlohmann::json{{"face", {face.X(), face.Y()}}, {"out", {n.X(), n.Y()}}, {"width", 2 * half},
                                              {"depth", spec.depth}, {"behind", o->depth / 2}, {"top", top},
                                              {"thickness", spec.thickness}, {"extra", spec.extra}, {"above", spec.above}}
                                   .dump();
  ChangeSet changes(document_);
  changes.modify(board);
  history_.execute(changes.build("Canopy"));
  history_.endGroup();
  return *made;
}

Result<Modeler::CanopySpec> Modeler::canopySpec(const ObjectId& id) const {
  using R = Result<CanopySpec>;
  const Object* c = document_.find(id);
  if (!c) return R::failure("Object not found");
  if (attributeOf(*c, "kind") != "canopy") return R::failure("It is not a canopy");
  const nlohmann::json j = nlohmann::json::parse(attributeOf(*c, "canopy"), nullptr, false);
  if (!j.is_object()) return R::failure("Its sizes are not kept: make the canopy again");
  CanopySpec spec;
  spec.depth = j.value("depth", spec.depth);
  spec.thickness = j.value("thickness", spec.thickness);
  spec.extra = j.value("extra", spec.extra);
  spec.above = j.value("above", spec.above);
  return spec;
}

Result<ObjectId> Modeler::changeCanopy(const ObjectId& id, const CanopySpec& spec) {
  using R = Result<ObjectId>;
  const Object* c = document_.find(id);
  if (!c) return R::failure("Object not found");
  if (attributeOf(*c, "kind") != "canopy") return R::failure("It is not a canopy");
  if (c->locked) return R::failure("'" + c->name + "' is locked");
  const std::string host = attributeOf(*c, "host"), openingId = attributeOf(*c, "opening"), name = c->name;
  const auto list = openings(host);
  const auto op = !list ? std::vector<OpeningInfo>::const_iterator() :
                          std::find_if(list->begin(), list->end(), [&](const OpeningInfo& x) { return x.id == openingId; });
  if (!list || op == list->end()) return R::failure("Its opening is gone: make the canopy again");
  const std::size_t index = op->index;
  history_.beginGroup("Change Canopy");
  if (const Status s = remove({id}); !s) {
    history_.abortGroup();
    return R::failure(s.message());
  }
  const auto made = makeCanopy(host, index, spec);
  if (!made) {
    history_.abortGroup();  // the old one back
    return R::failure(made.error());
  }
  Object renamed = *document_.find(*made);
  renamed.name = name;
  ChangeSet changes(document_);
  changes.modify(renamed);
  history_.execute(changes.build("Change Canopy"));
  history_.endGroup();
  return *made;
}

Result<int> Modeler::changeOpeningType(const std::string& mark, const OpeningTypeChange& change) {
  using R = Result<int>;
  if (mark.empty()) return R::failure("Name the mark of the openings to change");
  if (change.width && !(*change.width > 0)) return R::failure("An opening's width and height must be positive");
  if (change.height && !(*change.height > 0)) return R::failure("An opening's width and height must be positive");
  if (change.fitting) {
    const nlohmann::json parsed = nlohmann::json::parse(*change.fitting, nullptr, false);
    if (!parsed.is_object()) return R::failure("A fitting is a JSON object, e.g. {\"type\":\"window\"}");
  }
  // The openings of that mark, by wall, as the schedule has them now (their ids stay theirs).
  struct Of {
    std::vector<std::string> ids;
    bool ownMark = false;
    std::string mark;
  };
  std::map<ObjectId, Of> byHost;
  int count = 0;
  for (const OpeningRow& r : openingSchedule(document_).rows) {
    if (r.mark != mark) continue;
    Of& of = byHost[r.host];
    of.ids.push_back(r.openingId);
    of.ownMark |= r.ownMark;
    of.mark = r.mark;
    ++count;
  }
  if (count == 0) return R::failure("No openings have the mark '" + mark + "'");
  for (const auto& [host, of] : byHost)
    if (const Object* o = document_.find(host); o && o->locked) return R::failure("'" + o->name + "' is locked");
  history_.beginGroup("Change Opening Type");
  auto fail = [&](const std::string& why) {
    history_.abortGroup();
    return R::failure(why);
  };
  for (const auto& [host, of] : byHost) {
    auto indices = openingIndices(host, of.ids);
    if (!indices) return fail(indices.error());
    if (change.width || change.height)
      if (const Status s = setOpeningSize(host, *indices, change.width, change.height); !s) return fail(s.message());
    if (change.fitting) {
      indices = openingIndices(host, of.ids);
      if (!indices) return fail(indices.error());
      if (const Status s = setOpeningFittings(host, *indices, *change.fitting); !s) return fail(s.message());
    }
    if (change.product) {
      indices = openingIndices(host, of.ids);
      if (!indices) return fail(indices.error());
      // A mark given by hand stays; a worked-out one stays worked out.
      if (const Status s = setOpeningMarks(host, *indices, of.ownMark ? of.mark : std::string(), *change.product); !s) return fail(s.message());
    }
  }
  history_.endGroup();
  return count;
}

Result<int> Modeler::matchOpeningType(const ObjectId& host, std::size_t index) {
  using R = Result<int>;
  const auto list = openings(host);
  if (!list) return R::failure(list.status().message());
  const auto self = std::find_if(list->begin(), list->end(), [&](const OpeningInfo& o) { return o.index == index; });
  if (self == list->end()) return R::failure("No opening " + std::to_string(index) + " in this object");
  const OpeningRow* row = nullptr;
  const OpeningSchedule schedule = openingSchedule(document_);
  for (const OpeningRow& r : schedule.rows)
    if (r.host == host && r.openingId == self->id) row = &r;
  if (!row) return R::failure("This opening is not in the door and window schedule");
  int others = 0;
  for (const OpeningRow& r : schedule.rows) others += r.mark == row->mark && !(r.host == host && r.openingId == self->id);
  if (others == 0) return 0;
  std::string fitting = R"({"type":"none"})";
  if (const auto stored = document_.find(host)->attributes.find("opening_fittings"); stored != document_.find(host)->attributes.end()) {
    const nlohmann::json all = nlohmann::json::parse(stored->second, nullptr, false);
    if (all.is_object() && all.contains(self->id)) fitting = all.at(self->id).dump();
  }
  OpeningTypeChange change;
  change.width = self->width;
  change.height = self->height;
  change.fitting = fitting;
  change.product = row->product;
  const auto changed = changeOpeningType(row->mark, change);
  if (!changed) return changed;
  return others;
}

namespace {
bool isRoomMarker(const Object& o);  // with the rooms below
}  // namespace

Status Modeler::setOpeningPerformance(const ObjectId& hostId, const std::vector<std::size_t>& indices, double uValue,
                                      const std::string& glass, const std::string& source) {
  const Object* host = document_.find(hostId);
  if (!host) return Status::failure("Object not found");
  if (host->locked) return Status::failure("The object is locked");
  const auto list = openings(hostId);
  if (!list) return list.status();
  if (list->empty()) return Status::failure("This object has no openings");
  if (!std::isfinite(uValue) || uValue > 10) return Status::failure("熱貫流率は 0〜10 W/(m²·K) です");
  if (!oneLine(glass, 180) || !oneLine(source, 300)) return Status::failure("ガラス・資料は1行です");
  if (uValue >= 0 && source.empty()) return Status::failure("建具の性能の資料（カタログなど）を入力してください");
  Object object = *host;
  ensureOpeningIds(object);
  nlohmann::json all = nlohmann::json::object();
  if (const auto stored = object.attributes.find("opening_performance"); stored != object.attributes.end()) {
    const nlohmann::json parsed = nlohmann::json::parse(stored->second, nullptr, false);
    if (parsed.is_object()) all = parsed;
  }
  std::vector<std::size_t> targets = indices;
  if (targets.empty())
    for (const auto& o : *list) targets.push_back(o.index);
  const std::vector<OpeningRef> refs = rectangularOpenings(object);
  for (const std::size_t index : targets) {
    const auto it = std::find_if(refs.begin(), refs.end(), [&](const OpeningRef& o) { return o.index == index; });
    if (it == refs.end()) return Status::failure("No opening " + std::to_string(index));
    if (uValue < 0) all.erase(openingKey(*it));
    else all[openingKey(*it)] = {{"u", uValue}, {"glass", glass}, {"source", source}};
  }
  if (all.empty()) object.attributes.erase("opening_performance");
  else object.attributes["opening_performance"] = all.dump();
  ChangeSet changes(document_);
  changes.modify(object);
  history_.execute(changes.build("Opening Performance"));
  return Status::success();
}

Status Modeler::setOpeningVeranda(const ObjectId& hostId, const std::vector<std::size_t>& indices, bool veranda) {
  const Object* host = document_.find(hostId);
  if (!host) return Status::failure("Object not found");
  if (host->locked) return Status::failure("The object is locked");
  const auto list = openings(hostId);
  if (!list) return list.status();
  if (list->empty()) return Status::failure("This object has no openings");
  Object object = *host;
  ensureOpeningIds(object);
  nlohmann::json all = nlohmann::json::object();
  if (const auto stored = object.attributes.find("opening_daylight"); stored != object.attributes.end()) {
    const nlohmann::json parsed = nlohmann::json::parse(stored->second, nullptr, false);
    if (parsed.is_object()) all = parsed;
  }
  std::vector<std::size_t> targets = indices;
  if (targets.empty())
    for (const auto& o : *list) targets.push_back(o.index);
  const std::vector<OpeningRef> refs = rectangularOpenings(object);
  for (const std::size_t index : targets) {
    const auto it = std::find_if(refs.begin(), refs.end(), [&](const OpeningRef& o) { return o.index == index; });
    if (it == refs.end()) return Status::failure("No opening " + std::to_string(index));
    nlohmann::json one = all.contains(openingKey(*it)) && all.at(openingKey(*it)).is_object() ? all.at(openingKey(*it)) : nlohmann::json::object();
    if (veranda) one["veranda"] = true;
    else one.erase("veranda");
    if (one.empty()) all.erase(openingKey(*it));
    else all[openingKey(*it)] = one;
  }
  if (all.empty()) object.attributes.erase("opening_daylight");
  else object.attributes["opening_daylight"] = all.dump();
  ChangeSet changes(document_);
  changes.modify(object);
  history_.execute(changes.build("Opening Veranda"));
  return Status::success();
}

Status Modeler::setOpeningDaylight(const ObjectId& hostId, const std::vector<std::size_t>& indices, double factor,
                                   const std::string& basis, double ventilationM2) {
  const Object* host = document_.find(hostId);
  if (!host) return Status::failure("Object not found");
  if (host->locked) return Status::failure("The object is locked");
  const auto list = openings(hostId);
  if (!list) return list.status();
  if (list->empty()) return Status::failure("This object has no openings");
  // NaN: not given (what is stored stays); below 0: taken away.
  if (std::isinf(factor) || factor > 3.0) return Status::failure("A daylight factor is 0 to 3 (令 20 条 2 項: at most 3.0)");
  if (std::isinf(ventilationM2) || ventilationM2 > 100.0) return Status::failure("An area open for ventilation is 0 to 100 m2");
  if (basis.size() > 300 || basis.find_first_of("\r\n") != std::string::npos)
    return Status::failure("A basis is one line of at most 100 characters");
  Object object = *host;
  ensureOpeningIds(object);
  nlohmann::json all = nlohmann::json::object();
  if (const auto stored = object.attributes.find("opening_daylight"); stored != object.attributes.end()) {
    const nlohmann::json parsed = nlohmann::json::parse(stored->second, nullptr, false);
    if (parsed.is_object()) all = parsed;
  }
  std::vector<std::size_t> targets = indices;
  if (targets.empty())
    for (const auto& o : *list) targets.push_back(o.index);
  const std::vector<OpeningRef> refs = rectangularOpenings(object);
  for (const std::size_t index : targets) {
    const auto it = std::find_if(refs.begin(), refs.end(), [&](const OpeningRef& o) { return o.index == index; });
    if (it == refs.end()) return Status::failure("No opening " + std::to_string(index));
    nlohmann::json one = all.contains(openingKey(*it)) && all.at(openingKey(*it)).is_object() ? all.at(openingKey(*it))
                                                                                              : nlohmann::json::object();
    if (factor >= 0.0) {
      one["factor"] = factor;
      if (!basis.empty()) one["basis"] = basis;
    } else if (factor < 0.0) {
      one.erase("factor");
      one.erase("basis");
    }
    if (ventilationM2 >= 0.0) one["ventilation_m2"] = ventilationM2;
    else if (ventilationM2 < 0.0) one.erase("ventilation_m2");
    if (one.empty()) all.erase(openingKey(*it));
    else all[openingKey(*it)] = one;
  }
  if (all.empty()) object.attributes.erase("opening_daylight");
  else object.attributes["opening_daylight"] = all.dump();
  ChangeSet changes(document_);
  changes.modify(object);
  history_.execute(changes.build("Set Opening Daylight"));
  return Status::success();
}

Status Modeler::setRoomVentilation(const ObjectId& room, const std::string& joinedWith, double fanM3h,
                                   const std::optional<std::string>& fanUnit) {
  const Object* marker = document_.find(room);
  if (!marker || !isRoomMarker(*marker)) return Status::failure("Not a room made by the room tool");
  if (marker->locked) return Status::failure("The object is locked");
  if (std::isinf(fanM3h) || fanM3h > 100000) return Status::failure("有効換気量は 0〜100000 m³/h です");
  if (fanUnit && (fanUnit->size() > 120 || fanUnit->find_first_of("\r\n") != std::string::npos))
    return Status::failure("換気設備の名前は1行、40 字までです");
  const auto stored = [](const Object& o) {
    const auto it = o.attributes.find("room_daylight");
    const nlohmann::json j = it == o.attributes.end() ? nlohmann::json() : nlohmann::json::parse(it->second, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
  };
  const auto put = [](Object& o, const nlohmann::json& j) {
    if (j.empty()) o.attributes.erase("room_daylight");
    else o.attributes["room_daylight"] = j.dump();
  };
  ChangeSet changes(document_);
  Object object = *marker;
  nlohmann::json mine = stored(object);
  const std::string before = mine.value("joined_with", std::string());
  if (joinedWith != before) {
    // The pair is kept on both rooms; the old partner lets go.
    if (!joinedWith.empty()) {
      const Object* other = document_.find(joinedWith);
      if (!other || !isRoomMarker(*other) || joinedWith == room) return Status::failure("一続きにする相手は、別の室にしてください");
      Object partner = *other;
      nlohmann::json theirs = stored(partner);
      theirs["joined_with"] = room;
      put(partner, theirs);
      changes.modify(partner);
    }
    if (!before.empty())
      if (const Object* old = document_.find(before)) {
        Object partner = *old;
        nlohmann::json theirs = stored(partner);
        if (theirs.value("joined_with", std::string()) == room) theirs.erase("joined_with");
        put(partner, theirs);
        changes.modify(partner);
      }
    if (joinedWith.empty()) mine.erase("joined_with");
    else mine["joined_with"] = joinedWith;
  }
  if (fanUnit) {
    if (fanUnit->empty()) mine.erase("fan_unit");
    else mine["fan_unit"] = *fanUnit;
  }
  if (!std::isnan(fanM3h)) {
    if (fanM3h > 0) mine["fan_m3h"] = fanM3h;
    else mine.erase("fan_m3h");
  }
  put(object, mine);
  changes.modify(object);
  // docs/213: the airflow of a unit goes to every room it serves.
  const std::string unit = mine.value("fan_unit", std::string());
  if (!unit.empty() && !std::isnan(fanM3h)) {
    for (const Object& listed : document_.objects()) {
      if (listed.id == room || !isRoomMarker(listed) || listed.locked) continue;
      const Object* pending = changes.pending(listed.id);  // a partner changed above
      Object copy = pending ? *pending : listed;
      nlohmann::json theirs = stored(copy);
      if (theirs.value("fan_unit", std::string()) != unit) continue;
      if (fanM3h > 0) theirs["fan_m3h"] = fanM3h;
      else theirs.erase("fan_m3h");
      put(copy, theirs);
      changes.modify(copy);
    }
  }
  history_.execute(changes.build("Room Ventilation"));
  return Status::success();
}

Status Modeler::setRoomDaylight(const ObjectId& room, const std::string& kind, const std::string& ventilation) {
  const Object* marker = document_.find(room);
  if (!marker || !isRoomMarker(*marker)) return Status::failure("Not a room made by the room tool");
  if (marker->locked) return Status::failure("The object is locked");
  if (!kind.empty() && kind != "dwelling" && kind != "other" && kind != "none")
    return Status::failure("A room's kind for daylight is dwelling, other or none");
  if (!ventilation.empty() && ventilation != "window" && ventilation != "equipment")
    return Status::failure("A room is ventilated by window or equipment");
  Object object = *marker;
  // docs/211: what else is kept there (the room joined to it, the airflow) stays.
  nlohmann::json j = nlohmann::json::object();
  if (const auto it = object.attributes.find("room_daylight"); it != object.attributes.end()) {
    const nlohmann::json parsed = nlohmann::json::parse(it->second, nullptr, false);
    if (parsed.is_object()) j = parsed;
  }
  j.erase("kind");
  j.erase("ventilation");
  if (!kind.empty()) j["kind"] = kind;
  if (!ventilation.empty()) j["ventilation"] = ventilation;
  if (j.empty()) object.attributes.erase("room_daylight");
  else object.attributes["room_daylight"] = j.dump();
  ChangeSet changes(document_);
  changes.modify(object);
  history_.execute(changes.build("Set Room Daylight"));
  return Status::success();
}

Status Modeler::setOpeningMarks(const ObjectId& hostId, const std::vector<std::size_t>& indices, const std::string& mark,
                                const std::string& product) {
  const Object* host = document_.find(hostId);
  if (!host) return Status::failure("Object not found");
  const auto list = openings(hostId);
  if (!list) return list.status();
  if (list->empty()) return Status::failure("This object has no openings");
  if (mark.size() > 40 || product.size() > 120) return Status::failure("A mark is at most 40 and a product number 120 characters");
  if (mark.find_first_of("\r\n") != std::string::npos || product.find_first_of("\r\n") != std::string::npos)
    return Status::failure("A mark or a product number is one line");
  Object object = *host;
  ensureOpeningIds(object);
  nlohmann::json all = nlohmann::json::object();
  if (const auto stored = object.attributes.find("opening_marks"); stored != object.attributes.end()) {
    const nlohmann::json parsed = nlohmann::json::parse(stored->second, nullptr, false);
    if (parsed.is_object()) all = parsed;
  }
  std::vector<std::size_t> targets = indices;
  if (targets.empty())
    for (const auto& o : *list) targets.push_back(o.index);
  const std::vector<OpeningRef> refs = rectangularOpenings(object);
  for (const std::size_t index : targets) {
    const auto it = std::find_if(refs.begin(), refs.end(), [&](const OpeningRef& o) { return o.index == index; });
    if (it == refs.end()) return Status::failure("No opening " + std::to_string(index));
    if (mark.empty() && product.empty()) all.erase(openingKey(*it));
    else all[openingKey(*it)] = {{"mark", mark}, {"product", product}};
  }
  if (all.empty()) object.attributes.erase("opening_marks");
  else object.attributes["opening_marks"] = all.dump();
  ChangeSet changes(document_);
  changes.modify(object);
  history_.execute(changes.build("Set Opening Mark"));
  return Status::success();
}

namespace {
// Mitred corners (docs/159): the wall's own rectangle is united with "corner-fill" pieces and
// "corner-trim" pieces are taken from it, innermost of all its cuts. The rectangle stays the
// wall as drawn: joins, rooms, lengths and openings read it as before.
constexpr const char* kCornerFill = "corner-fill";
constexpr const char* kCornerTrim = "corner-trim";

// A Boolean of `kind` whose operands after the first all are corner pieces of that prefix.
bool cornerNode(const Recipe& r, BooleanKind kind, const char* prefix) {
  const auto* b = std::get_if<BooleanFeature>(&r.base);
  if (!b || b->kind != kind || b->operands.size() < 2 || !r.modifiers.empty()) return false;
  for (std::size_t i = 1; i < b->operands.size(); ++i)
    if (b->operands[i].id.rfind(prefix, 0) != 0) return false;
  return true;
}

// A wall as drawn: its pushed-up rectangle (inside any opening cuts) and the line it was drawn on.
struct WallBand {
  const Object* object = nullptr;
  std::vector<std::size_t> path;  // operand indices down to the wall's own recipe
  gp_Trsf toWorld;                // the rectangle's frame -> world
  Recipe recipe;                  // the rectangle + push (copy)
  double lineOffset = 0.0;        // the drawn line, from the right face (rectangle origin side)
  gp_Pnt start;                   // world: the line where the rectangle starts
  gp_Dir dir, left;               // world: along the line, and to its left
  double length = 0.0;            // the rectangle's length along the line
  double right = 0.0, leftExt = 0.0;  // band: from -right to +leftExt across the line
  double z0 = 0.0, z1 = 0.0;
};

std::optional<WallBand> wallBand(const Document& document, const Object& o) {
  const auto kind = o.attributes.find("kind");
  if (kind == o.attributes.end() || kind->second != "wall" || o.shape.IsNull()) return std::nullopt;
  WallBand w;
  w.object = &o;
  const Recipe* recipe = &o.recipe;
  gp_Trsf placement;
  while (const auto* cut = std::get_if<BooleanFeature>(&recipe->base)) {
    const bool cuts = cut->kind == BooleanKind::Subtract, fills = cornerNode(*recipe, BooleanKind::Union, kCornerFill);
    if (!recipe->modifiers.empty() || (!cuts && !fills) || cut->operands.empty() || !cut->operands.front().recipe)
      return std::nullopt;
    placement = placement * cut->operands.front().placement;
    w.path.push_back(0);
    recipe = cut->operands.front().recipe.get();
  }
  const auto* rect = std::get_if<RectangleProfile>(&recipe->base);
  if (!rect || recipe->modifiers.size() != 1 || !std::holds_alternative<ExtrudeOp>(recipe->modifiers.front()) ||
      rect->width <= 0 || rect->height <= 0)
    return std::nullopt;
  w.recipe = *recipe;
  w.toWorld = document.worldTransform(o) * placement;
  if (std::abs(w.toWorld.ScaleFactor() - 1.0) > 1e-9) return std::nullopt;
  const gp_Dir normal = rect->plane.normal.Transformed(w.toWorld);
  if (normal.Z() < 1 - 1e-9) return std::nullopt;  // drawn on the ground plane, pushed up
  const auto line = o.attributes.find("wall_line");
  const std::string ref = line == o.attributes.end() ? "center" : line->second;
  w.lineOffset = ref == "right" ? 0.0 : ref == "left" ? rect->height : rect->height / 2;
  if (ref == "offset") {
    const auto at = o.attributes.find("wall_line_offset");
    try {
      w.lineOffset = at == o.attributes.end() ? rect->height / 2 : std::stod(at->second);
    } catch (...) {
      return std::nullopt;
    }
  }
  w.dir = rect->plane.xDir.Transformed(w.toWorld);
  w.left = rect->plane.yDir().Transformed(w.toWorld);
  w.start = rect->plane.toWorld(0, w.lineOffset).Transformed(w.toWorld);
  w.length = rect->width;
  w.right = w.lineOffset;
  w.leftExt = rect->height - w.lineOffset;
  const Bnd_Box box = document.worldBounds(o);
  if (box.IsVoid()) return std::nullopt;
  w.z0 = box.CornerMin().Z();
  w.z1 = box.CornerMax().Z();
  return w;
}

// Where the line through `a` along `da` meets the line through `b` along `db` (plan):
// parameters along each; nullopt when parallel.
std::optional<std::pair<double, double>> meet(const gp_Pnt& a, const gp_Dir& da, const gp_Pnt& b, const gp_Dir& db) {
  const double den = da.X() * db.Y() - da.Y() * db.X();
  if (std::abs(den) < 1e-6) return std::nullopt;
  const double ex = b.X() - a.X(), ey = b.Y() - a.Y();
  return std::make_pair((ex * db.Y() - ey * db.X()) / den, (ex * da.Y() - ey * da.X()) / den);
}

// --- wall joints follow moved walls (docs/90 G-05, docs/102) -------------------------------
// A wall butts against another where its end lies on one of the other's faces (how the wall
// tool makes corners, and how a partition meets a wall).
//  - A wall moved sideways: the ends on its faces go to the same faces again (those walls get
//    longer or shorter).
//  - A wall moved along its own line, or made longer / shorter: the walls at its corners are
//    carried along (they move as they are), and the walls joined to *them* get longer or shorter.
//  - Slabs: a corner of a slab's outline standing in a wall goes with that wall's sideways move.
// Everything is read from the walls as they are before the move.
struct WallEnd {
  std::optional<std::size_t> partner;  // the wall this end reaches
  bool joined = false;                 // ...and lies on one of its faces
  bool far = false;                    // on the face farther along this wall's line
  double onPartner = 0.0;              // where along the partner's line
  double reach = 0.0;
};

std::vector<std::array<WallEnd, 2>> wallEnds(const std::vector<WallBand>& walls) {
  std::vector<std::array<WallEnd, 2>> ends(walls.size());
  for (std::size_t k = 0; k < walls.size(); ++k) {
    const WallBand& w = walls[k];
    for (int end = 0; end < 2; ++end) {
      const double s = end == 0 ? 0.0 : w.length;
      WallEnd& e = ends[k][static_cast<std::size_t>(end)];
      double bestGap = 1e300;
      for (std::size_t i = 0; i < walls.size(); ++i) {
        if (i == k) continue;
        const WallBand& o = walls[i];
        if (std::min(w.z1, o.z1) - std::max(w.z0, o.z0) < 1.0) continue;  // another storey
        const auto m = meet(w.start, w.dir, o.start, o.dir);
        if (!m) continue;
        const double reach = o.right + o.leftExt + std::max(w.right + w.leftExt, 1.0) + 1.0;
        const double gap = std::abs(m->first - s);
        if (gap > reach || m->second < -reach || m->second > o.length + reach || gap >= bestGap) continue;
        bestGap = gap;
        e.partner = i;
        e.onPartner = m->second;
        e.reach = reach;
      }
      if (!e.partner) continue;
      const WallBand& o = walls[*e.partner];
      const auto a = meet(w.start, w.dir, o.start.Translated(gp_Vec(o.left) * o.leftExt), o.dir);
      const auto b = meet(w.start, w.dir, o.start.Translated(gp_Vec(o.left) * -o.right), o.dir);
      if (!a || !b) continue;
      const double nearFace = std::min(a->first, b->first), farFace = std::max(a->first, b->first);
      e.far = std::abs(s - farFace) < std::abs(s - nearFace);
      e.joined = std::min(std::abs(s - farFace), std::abs(s - nearFace)) <= 1.0;  // an end this near a face is on it
    }
  }
  return ends;
}

// The walls at the corner of wall `i`'s end `end` (0 start, 1 far end).
std::vector<std::size_t> cornerPartners(const std::vector<WallBand>& walls, const std::vector<std::array<WallEnd, 2>>& ends,
                                        std::size_t i, int end) {
  std::vector<std::size_t> out;
  const WallBand& w = walls[i];
  // This end on another wall's face, at that wall's end.
  const WallEnd& mine = ends[i][static_cast<std::size_t>(end)];
  if (mine.partner && mine.joined) {
    const WallBand& o = walls[*mine.partner];
    if (std::abs(mine.onPartner) <= mine.reach || std::abs(mine.onPartner - o.length) <= mine.reach) out.push_back(*mine.partner);
  }
  // Another wall's end on this wall's face, at this end.
  const double s = end == 0 ? 0.0 : w.length;
  for (std::size_t j = 0; j < walls.size(); ++j) {
    if (j == i) continue;
    for (const WallEnd& theirs : ends[j]) {
      if (!theirs.partner || *theirs.partner != i || !theirs.joined) continue;
      if (std::abs(theirs.onPartner - s) > theirs.reach) continue;
      if (std::find(out.begin(), out.end(), j) == out.end()) out.push_back(j);
    }
  }
  return out;
}

// Wall `w` with its ends at s0 and s1 along its line (its openings must stay in it): into
// `object` (a copy of it), rebuilt. False when it cannot be.
bool reshapeWallBand(const Document& document, const WallBand& w, double s0, double s1, Object& object,
                     const RegenContext& context) {
  if (s1 - s0 < tolerance::kMinDimension) return false;
  Recipe recipe = w.recipe;
  auto& rect = std::get<RectangleProfile>(recipe.base);
  const gp_Pnt p0 = w.start.Translated(gp_Vec(w.dir) * s0), p1 = w.start.Translated(gp_Vec(w.dir) * s1);
  rect.plane.origin = p0.Translated(gp_Vec(w.left) * -w.right).Transformed(w.toWorld.Inverted());
  rect.width = s1 - s0;
  if (w.path.empty()) object.recipe = std::move(recipe);
  else replaceOperand(object.recipe, w.path, 0, std::move(recipe));
  for (const OpeningExtent& e : openingExtents(*w.object, document.worldTransform(*w.object))) {
    const gp_Vec u(gp::DZ().Crossed(e.normal));
    const double a = gp_Vec(p0.XYZ()).Dot(u), b = gp_Vec(p1.XYZ()).Dot(u);
    if (e.u0 < std::min(a, b) - tolerance::kLinear || e.u1 > std::max(a, b) + tolerance::kLinear) return false;
  }
  return static_cast<bool>(rebuild(object, context));
}

// `displaced`: how each moved wall goes (plan). `primary`: walls moved by the user as they
// are (never reshaped here). `keepShape`: walls whose own change is being made by the caller.
std::string roofSpecText(const geom::RoofSpec& s);                                            // roofs, below
Result<std::pair<geom::RoofSpec, gp_Trsf>> storedRoofOf(const Document& document, const Object* o);  // roofs, below
geom::RoofSpec roofInFrame(geom::RoofSpec s, const gp_Trsf& t);                                // roofs, below

void settleWallJoins(const Document& document, ChangeSet& changes, const RegenContext& context,
                     const std::vector<WallBand>& walls, const std::vector<std::array<WallEnd, 2>>& ends,
                     std::map<std::size_t, gp_Vec> displaced, const std::set<std::size_t>& primary,
                     const std::set<std::size_t>& keepShape, const std::set<ObjectId>& movedObjects, JoinFollow& report) {
  // An end on another wall's face stays on that face, whichever of the two walls goes: the end
  // is put where this wall's line meets the face as both are after the move (docs/107). At
  // right-angled corners this changes only the walls joined to a displaced wall; at other
  // angles the displaced wall itself (also one the user moved) is made longer or shorter so
  // that the corner stays closed.
  for (std::size_t k = 0; k < walls.size(); ++k) {
    if (keepShape.count(k)) continue;
    const WallBand& w = walls[k];
    const gp_Vec own = displaced.count(k) ? displaced.at(k) : gp_Vec();
    double s0 = 0.0, s1 = w.length;
    bool touched = false;
    for (int end = 0; end < 2; ++end) {
      const WallEnd& e = ends[k][static_cast<std::size_t>(end)];
      if (!e.partner || !e.joined || (!displaced.count(*e.partner) && !displaced.count(k))) continue;
      const gp_Vec relative = (displaced.count(*e.partner) ? displaced.at(*e.partner) : gp_Vec()) - own;  // how the partner goes, seen from this wall
      if (relative.Magnitude() < tolerance::kLinear) continue;
      const WallBand& o = walls[*e.partner];
      const double s = end == 0 ? 0.0 : w.length;
      const auto a = meet(w.start, w.dir, o.start.Translated(relative + gp_Vec(o.left) * o.leftExt), o.dir);
      const auto b = meet(w.start, w.dir, o.start.Translated(relative + gp_Vec(o.left) * -o.right), o.dir);
      if (!a || !b) continue;
      // Still beside the partner where it goes? (Gone past this wall: let go.) Not asked at a
      // corner, where the partner's own end follows this wall (docs/159: a slanted wall moved
      // outwards used to let go of both corners and stick out through them).
      const auto there = meet(w.start, w.dir, o.start.Translated(relative), o.dir);
      const bool corner = std::abs(e.onPartner) <= e.reach || std::abs(e.onPartner - o.length) <= e.reach;
      if (!there || (!corner && (there->second < -e.reach || there->second > o.length + e.reach))) continue;
      const double value = e.far ? std::max(a->first, b->first) : std::min(a->first, b->first);
      if (std::abs(value - s) < 1e-6) continue;
      (end == 0 ? s0 : s1) = value;
      touched = true;
    }
    if (!touched) continue;
    const Object& was = *w.object;
    auto stays = [&] { report.left.push_back(was.name); };
    if (was.locked || s1 - s0 < tolerance::kMinDimension) {
      stays();
      continue;
    }
    const Object* pending = changes.pending(was.id);
    Object object = pending ? *pending : was;
    Recipe recipe = w.recipe;
    auto& rect = std::get<RectangleProfile>(recipe.base);
    const gp_Pnt p0 = w.start.Translated(gp_Vec(w.dir) * s0), p1 = w.start.Translated(gp_Vec(w.dir) * s1);
    rect.plane.origin = p0.Translated(gp_Vec(w.left) * -w.right).Transformed(w.toWorld.Inverted());
    rect.width = s1 - s0;
    if (w.path.empty()) object.recipe = std::move(recipe);
    else replaceOperand(object.recipe, w.path, 0, std::move(recipe));
    // Its openings stay where they are (in the wall's own frame): all must still be in the wall.
    bool inside = true;
    for (const OpeningExtent& e : openingExtents(was, document.worldTransform(was))) {
      const gp_Vec u(gp::DZ().Crossed(e.normal));
      const double a = gp_Vec(p0.XYZ()).Dot(u), b = gp_Vec(p1.XYZ()).Dot(u);
      inside &= e.u0 >= std::min(a, b) - tolerance::kLinear && e.u1 <= std::max(a, b) + tolerance::kLinear;
    }
    if (!inside || !rebuild(object, context)) {
      stays();
      continue;
    }
    changes.modify(object);
    ++report.followed;
  }
  // Slabs: an outline corner standing in a displaced wall goes with its sideways move.
  for (const Object& o : document.objects()) {
    const auto kind = o.attributes.find("kind");
    if (kind == o.attributes.end() || kind->second != "slab" || o.locked || movedObjects.count(o.id)) continue;
    const Recipe* core = slabCore(o.recipe);
    const auto* polygon = core ? std::get_if<PolygonProfile>(&core->base) : nullptr;
    if (!polygon || core->modifiers.empty() || !std::holds_alternative<ExtrudeOp>(core->modifiers.front())) continue;
    const Bnd_Box box = document.worldBounds(o);
    if (box.IsVoid()) continue;
    const double top = box.CornerMax().Z(), bottom = box.CornerMin().Z();
    const gp_Trsf world = document.worldTransform(o);
    std::vector<gp_Pnt> points = polygon->points;
    bool any = false;
    for (gp_Pnt& local : points) {
      const gp_Pnt p = local.Transformed(world);
      gp_Vec delta(0, 0, 0);
      std::vector<gp_Dir> counted;
      for (const auto& [index, d] : displaced) {
        const WallBand& w = walls[index];
        if (top < w.z0 - 1.0 || bottom > w.z1 + 1.0) continue;  // not at this wall's height
        const gp_Vec sideways = d - gp_Vec(w.dir) * d.Dot(gp_Vec(w.dir));
        if (sideways.Magnitude() < tolerance::kLinear) continue;
        const gp_Vec to(w.start, gp_Pnt(p.X(), p.Y(), w.start.Z()));
        const double along = to.Dot(gp_Vec(w.dir)), across = to.Dot(gp_Vec(w.left));
        const double thick = w.right + w.leftExt + 1.0;
        if (along < -thick || along > w.length + thick || across < -w.right - 1.0 || across > w.leftExt + 1.0) continue;
        if (std::any_of(counted.begin(), counted.end(), [&](const gp_Dir& c) { return c.IsParallel(w.dir, 1e-6); })) continue;
        counted.push_back(w.dir);
        delta += sideways;
      }
      if (delta.Magnitude() < tolerance::kLinear) continue;
      local = p.Translated(delta).Transformed(world.Inverted());
      any = true;
    }
    if (!any) continue;
    const Object* pending = changes.pending(o.id);
    Object slab = pending ? *pending : o;
    const Recipe* own = slabCore(slab.recipe);
    if (!own || !std::holds_alternative<PolygonProfile>(own->base)) continue;
    Recipe moved = *own;
    std::get<PolygonProfile>(moved.base).points = points;
    setSlabCore(slab.recipe, std::move(moved));
    slab.faceMaterials.clear();
    slab.hiddenEdges.clear();
    if (!rebuild(slab, context)) {
      report.left.push_back(o.name);
      continue;
    }
    changes.modify(slab);
    ++report.slabs;
  }
  // docs/169: the sideways move of the displaced walls a plan point stands in (within a wall's
  // thickness, and at a corner the other wall's too; two walls of one direction count once),
  // when `z0`..`z1` is within `reach` of the wall's height.
  auto sidewaysAt = [&](const gp_Pnt& p, double z0, double z1, double above) {
    gp_Vec delta(0, 0, 0);
    std::vector<gp_Dir> counted;
    for (const auto& [index, d] : displaced) {
      const WallBand& w = walls[index];
      if (z1 < w.z0 - 1.0 || z0 > w.z1 + above) continue;
      const gp_Vec sideways = d - gp_Vec(w.dir) * d.Dot(gp_Vec(w.dir));
      if (sideways.Magnitude() < tolerance::kLinear) continue;
      const gp_Vec to(w.start, gp_Pnt(p.X(), p.Y(), w.start.Z()));
      const double along = to.Dot(gp_Vec(w.dir)), across = to.Dot(gp_Vec(w.left));
      const double thick = w.right + w.leftExt + 1.0;
      if (along < -thick || along > w.length + thick || across < -w.right - 1.0 || across > w.leftExt + 1.0) continue;
      if (std::any_of(counted.begin(), counted.end(), [&](const gp_Dir& c) { return c.IsParallel(w.dir, 1e-6); })) continue;
      counted.push_back(w.dir);
      delta += sideways;
    }
    return delta;
  };
  // Columns standing in a wall, and beams lying along one (on it or in it): moved with its
  // sideways move, as they are. Beams across a wall (their length would change) stay.
  for (const Object& o : document.objects()) {
    if (o.locked || movedObjects.count(o.id) || changes.erased(o.id)) continue;
    const auto* m = std::get_if<LinearMember>(&o.recipe.base);
    if (!m) continue;
    const gp_Trsf world = document.worldTransform(o);
    const gp_Pnt a = m->start.Transformed(world), b = m->end.Transformed(world);
    const gp_Vec axis(a, b);
    if (axis.Magnitude() < tolerance::kLinear) continue;
    const gp_Dir dir(axis);
    gp_Vec delta(0, 0, 0);
    if (std::abs(dir.Z()) > 0.999) {
      // A column: its axis in the wall, its height overlapping the wall's.
      delta = sidewaysAt(a, std::min(a.Z(), b.Z()), std::max(a.Z(), b.Z()), 1.0);
    } else if (std::abs(dir.Z()) < 1e-3) {
      // A beam: both ends in the same walls, along them (on the wall: up to 600 over its top).
      const gp_Vec da = sidewaysAt(a, a.Z(), a.Z(), 600.0), db = sidewaysAt(b, b.Z(), b.Z(), 600.0);
      if (da.Magnitude() < tolerance::kLinear || (da - db).Magnitude() > tolerance::kLinear) continue;
      // Along a displaced wall: the move is across the beam, not along it.
      if (std::abs(gp_Vec(dir).Dot(da.Normalized())) > 1e-3) continue;
      delta = da;
    }
    if (delta.Magnitude() < tolerance::kLinear) continue;
    const Object* pending = changes.pending(o.id);
    Object member = pending ? *pending : o;
    gp_Trsf shift;
    shift.SetTranslation(delta);
    member.transform = localDelta(document.parentTransform(o), shift) * member.transform;
    changes.modify(member);
    ++report.members;
  }
  // Roofs: an outline corner standing in a displaced wall (the roof bearing on it) goes with it.
  for (const Object& o : document.objects()) {
    const auto kind = o.attributes.find("kind");
    if (kind == o.attributes.end() || kind->second != "roof" || o.locked || movedObjects.count(o.id) || changes.erased(o.id)) continue;
    const auto stored = storedRoofOf(document, &o);
    if (!stored) continue;
    geom::RoofSpec spec = roofInFrame(stored->first, stored->second);
    bool any = false;
    for (gp_Pnt& p : spec.outline) {
      const gp_Vec delta = sidewaysAt(p, spec.height, spec.height, 1.0);
      if (delta.Magnitude() < tolerance::kLinear) continue;
      p.Translate(gp_Vec(delta.X(), delta.Y(), 0));
      any = true;
    }
    if (!any) continue;
    const geom::RoofSpec local = roofInFrame(spec, stored->second.Inverted());
    const auto roof = geom::makeRoof(local);
    if (!roof) {
      report.left.push_back(o.name);
      continue;
    }
    const Object* pending = changes.pending(o.id);
    Object object = pending ? *pending : o;
    object.recipe.base = ImportedShape{roof->solid, "roof"};
    object.attributes["roof_spec"] = roofSpecText(local);
    object.faceMaterials.clear();
    object.hiddenEdges.clear();
    if (!rebuild(object, context)) {
      report.left.push_back(o.name);
      continue;
    }
    changes.modify(object);
    ++report.roofs;
  }
}

// Carries the walls at the corners of displaced walls along with the part of the move that
// runs along those walls. `ends`: which ends of each wall moved along its line (both for a
// slide, one for a change of length).
void carryCorners(const Document& document, ChangeSet& changes, const std::vector<WallBand>& walls,
                  const std::vector<std::array<WallEnd, 2>>& ends, const std::map<std::size_t, gp_Vec>& along,
                  const std::map<std::size_t, std::array<bool, 2>>& movedEnds, std::map<std::size_t, gp_Vec>& displaced,
                  const std::set<std::size_t>& primary, JoinFollow& report) {
  for (const auto& [i, p] : along) {
    if (p.Magnitude() < tolerance::kLinear) continue;
    for (int end = 0; end < 2; ++end) {
      if (!movedEnds.at(i)[static_cast<std::size_t>(end)]) continue;
      for (std::size_t j : cornerPartners(walls, ends, i, end)) {
        if (primary.count(j) || displaced.count(j)) continue;  // moved by the user, or carried already
        const Object& was = *walls[j].object;
        if (was.locked) {
          report.left.push_back(was.name);
          continue;
        }
        gp_Trsf t;
        t.SetTranslation(p);
        const Object* pending = changes.pending(was.id);
        Object o = pending ? *pending : was;
        o.transform = localDelta(document.parentTransform(o), t) * o.transform;
        changes.modify(o);
        displaced[j] = p;
        ++report.carried;
      }
    }
  }
}

std::optional<gp_Dir> wallLeftSide(const Document& document, const Object& o) {
  const auto band = wallBand(document, o);
  return band ? std::optional<gp_Dir>(band->left) : std::nullopt;
}

std::vector<WallBand> allWallBands(const Document& document) {
  std::vector<WallBand> walls;
  for (const Object& o : document.objects())
    if (auto w = wallBand(document, o)) walls.push_back(std::move(*w));
  return walls;
}

}  // namespace

namespace {
const FloorLevel* levelAtHeight(const std::vector<FloorLevel>& levels, double z);  // with the level helpers
}  // namespace

Result<Modeler::GridPinning> Modeler::pinToGrid(const std::vector<ObjectId>& ids, const std::string& grid,
                                                double tolerance) {
  using R = Result<GridPinning>;
  // The grid: the one named, or the only one.
  const Object* gridObject = nullptr;
  if (!grid.empty()) {
    gridObject = document_.find(grid);
    if (!gridObject || !gridSpec(grid)) return R::failure("No grid '" + grid + "'");
  } else {
    for (const Object& o : document_.objects()) {
      if (!gridSpec(o.id)) continue;
      if (gridObject) return R::failure("There are several grids: name the one to pin to");
      gridObject = &o;
    }
    if (!gridObject) return R::failure("There is no grid to pin to");
  }
  const GridSpec& spec = *gridSpec(gridObject->id);
  const gp_Trsf gridWorld = document_.worldTransform(*gridObject);
  auto crossingAt = [&](const gp_Pnt& p) -> std::optional<std::pair<std::string, std::string>> {
    for (const GridAxis& x : spec.x)
      for (const GridAxis& y : spec.y) {
        const auto c = crossingOf(spec, gridWorld, x.id, y.id);
        if (c && std::hypot(c->X() - p.X(), c->Y() - p.Y()) <= tolerance) return std::make_pair(x.id, y.id);
      }
    return std::nullopt;
  };
  std::vector<ObjectId> targets = ids;
  if (targets.empty())
    for (const Object& o : document_.objects())
      if ((std::holds_alternative<LinearMember>(o.recipe.base) && !o.gridPin) ||
          (attributeOf(o, "kind") == "wall" && !o.attributes.count(kWallGridPin)))
        targets.push_back(o.id);
  if (targets.empty()) return R::failure("No members to pin");
  GridPinning out;
  ChangeSet changes(document_);
  for (const ObjectId& id : targets) {
    const Object* o = document_.find(id);
    if (!o) return R::failure("Object not found");
    // A wall (docs/129): each end of its line pinned to the nearest crossing within its thickness
    // (+ `tolerance`) in plan, the two ends to two crossings.
    if (attributeOf(*o, "kind") == "wall" && !o->locked) {
      const auto w = wallBand(document_, *o);
      if (!w) {
        out.left.push_back(o->name);
        continue;
      }
      const double reach = w->right + w->leftExt + tolerance;
      auto nearest = [&](const gp_Pnt& p) -> std::optional<std::pair<std::string, std::string>> {
        std::optional<std::pair<std::string, std::string>> best;
        double gap = reach;
        for (const GridAxis& x : spec.x)
          for (const GridAxis& y : spec.y) {
            const auto c = crossingOf(spec, gridWorld, x.id, y.id);
            if (!c) continue;
            const double g = std::hypot(c->X() - p.X(), c->Y() - p.Y());
            if (g <= gap) gap = g, best = std::make_pair(x.id, y.id);
          }
        return best;
      };
      const auto ca = nearest(w->start), cb = nearest(w->start.Translated(gp_Vec(w->dir) * w->length));
      if (!ca || !cb || *ca == *cb) {
        out.left.push_back(o->name);
        continue;
      }
      Object pinned = *o;
      pinned.attributes[kWallGridPin] =
          nlohmann::json{{"grid", gridObject->id}, {"ends", nlohmann::json::array({nlohmann::json::array({ca->first, ca->second}), nlohmann::json::array({cb->first, cb->second})})}}.dump();
      changes.modify(pinned);
      ++out.pinned;
      ++out.walls;
      continue;
    }
    const auto* member = std::get_if<LinearMember>(&o->recipe.base);
    if (!member || o->locked) {
      out.left.push_back(o->name);
      continue;
    }
    const gp_Trsf world = document_.worldTransform(*o);
    const gp_Pnt a = member->start.Transformed(world), b = member->end.Transformed(world);
    const auto ca = crossingAt(a), cb = crossingAt(b);
    if (!ca || !cb) {
      out.left.push_back(o->name);
      continue;
    }
    GridPin pin;
    pin.grid = gridObject->id;
    pin.startX = ca->first;
    pin.startY = ca->second;
    pin.endX = cb->first;
    pin.endY = cb->second;
    if (const FloorLevel* l = levelAtHeight(document_.settings().levels, a.Z())) pin.startLevel = l->id;
    if (const FloorLevel* l = levelAtHeight(document_.settings().levels, b.Z())) pin.endLevel = l->id;
    Object pinned = *o;
    pinned.gridPin = pin;
    changes.modify(pinned);
    ++out.pinned;
  }
  if (out.pinned == 0) return R::failure("No member or wall has both ends on crossings of the grid (nothing was changed)");
  history_.execute(changes.build("Pin To Grid"));
  return out;
}

Result<int> Modeler::unpinFromGrid(const std::vector<ObjectId>& ids) {
  using R = Result<int>;
  ChangeSet changes(document_);
  int count = 0;
  auto letGo = [&](const Object& o) {
    if (!o.gridPin && !o.attributes.count(kWallGridPin)) return;
    if (o.locked) return;
    Object free = o;
    free.gridPin.reset();
    free.attributes.erase(kWallGridPin);
    changes.modify(free);
    ++count;
  };
  if (ids.empty()) {
    for (const Object& o : document_.objects()) letGo(o);
  } else {
    for (const ObjectId& id : ids) {
      const Object* o = document_.find(id);
      if (!o) return R::failure("Object not found");
      letGo(*o);
    }
  }
  if (count == 0) return R::failure("Nothing here is pinned to a grid");
  history_.execute(changes.build("Unpin From Grid"));
  return count;
}

Result<Modeler::StoreyCopy> Modeler::copyStorey(const std::string& fromLevel, const std::string& toLevel) {
  using R = Result<StoreyCopy>;
  const std::vector<FloorLevel>& levels = document_.settings().levels;
  const auto level = [&](const std::string& id) -> const FloorLevel* {
    for (const FloorLevel& l : levels)
      if (l.id == id || l.name == id) return &l;
    return nullptr;
  };
  const FloorLevel* from = level(fromLevel);
  const FloorLevel* to = level(toLevel);
  if (!from || !to) return R::failure("No such level");
  const double dz = to->elevation - from->elevation;
  if (std::abs(dz) < 1.0) return R::failure("The two levels are at one height");
  auto ceilingOf = [&](double floor) {
    std::optional<double> next;
    for (const FloorLevel& l : levels)
      if (l.elevation > floor + 1e-6 && (!next || l.elevation < *next)) next = l.elevation;
    return next;
  };
  auto inStorey = [&](double z0, double floor) {
    const auto ceiling = ceilingOf(floor);
    return z0 >= floor - 300.0 && (!ceiling || z0 < *ceiling - 300.0);
  };
  auto bottom = [&](const Object& o) -> std::optional<double> {
    const Bnd_Box box = document_.worldBounds(o);
    if (box.IsVoid()) return std::nullopt;
    return box.CornerMin().Z() + box.GetGap();
  };
  auto kindOf = [](const Object& o) { const auto k = o.attributes.find("kind"); return k == o.attributes.end() ? std::string() : k->second; };
  // What stands in each storey now.
  std::vector<ObjectId> picked;
  bool targetHasSlab = false;
  for (const Object& o : document_.objects()) {
    if (!o.parent.empty() || o.attributes.count("grid_part")) continue;
    const std::string kind = kindOf(o);
    if (kind == "room") {
      const auto* note = std::get_if<TextNote>(&o.recipe.base);
      if (!note) continue;
      const double z = note->position.Transformed(document_.worldTransform(o)).Z();
      if (inStorey(z, from->elevation)) picked.push_back(o.id);
      continue;
    }
    const ObjectKind k = o.kind();
    if (k != ObjectKind::Solid && k != ObjectKind::Shell && k != ObjectKind::Instance && k != ObjectKind::Group) continue;
    const auto z0 = bottom(o);
    if (!z0) continue;
    if (inStorey(*z0, to->elevation)) {
      if (kind == "wall") return R::failure("Walls already stand on '" + to->name + "': copying would double them (nothing was changed)");
      if (kind == "slab") targetHasSlab = true;
    }
    if (inStorey(*z0, from->elevation)) picked.push_back(o.id);
  }
  StoreyCopy out;
  std::vector<ObjectId> roots;
  for (const ObjectId& id : picked) {
    const Object* o = document_.find(id);
    if (kindOf(*o) == "slab" && targetHasSlab) {
      out.slabsLeft = true;
      continue;
    }
    roots.push_back(id);
  }
  if (roots.empty()) return R::failure("Nothing stands on '" + from->name + "' to copy");
  history_.beginGroup("Copy Storey");
  gp_Trsf up;
  up.SetTranslation(gp_Vec(0, 0, dz));
  const auto made = copy(roots, up);
  if (!made) {
    history_.abortGroup();
    return R::failure(made.error());
  }
  // Levels: what was on a level goes onto the one as far above, or loses it.
  auto shifted = [&](const std::string& id) -> std::optional<std::string> {
    for (const FloorLevel& l : levels)
      if (l.id == id) {
        for (const FloorLevel& m : levels)
          if (std::abs(m.elevation - (l.elevation + dz)) < 1e-6) return m.id;
        return std::nullopt;
      }
    return std::nullopt;
  };
  // docs/186: each room copied, by the room it was copied from (the same place, dz higher).
  std::map<ObjectId, ObjectId> roomCopies;
  auto notePoint = [&](const Object& o) -> std::optional<gp_Pnt> {
    const auto* note = std::get_if<TextNote>(&o.recipe.base);
    if (!note) return std::nullopt;
    return note->position.Transformed(document_.worldTransform(o));
  };
  for (const ObjectId& id : *made) {
    const Object* copied = document_.find(id);
    if (!copied || kindOf(*copied) != "room") continue;
    const auto at = notePoint(*copied);
    if (!at) continue;
    for (const ObjectId& original : roots) {
      const Object* was = document_.find(original);
      if (!was || kindOf(*was) != "room" || roomCopies.count(original)) continue;
      const auto from = notePoint(*was);
      if (from && from->Translated(gp_Vec(0, 0, dz)).Distance(*at) < 1e-6) {
        roomCopies[original] = id;
        break;
      }
    }
  }
  ChangeSet changes(document_);
  bool any = false;
  for (const ObjectId& id : *made) {
    const Object* copied = document_.find(id);
    if (!copied) continue;
    Object o = *copied;
    bool changed = false;
    if (kindOf(o) == "ceiling") {
      // It belongs to the copied room, and was made dz higher (so it still follows that room).
      const auto room = o.attributes.find("room");
      if (room != o.attributes.end() && roomCopies.count(room->second)) room->second = roomCopies.at(room->second), changed = true;
      nlohmann::json madeAs = nlohmann::json::parse(attributeOf(o, "ceiling_made"), nullptr, false);
      if (madeAs.is_object() && madeAs.contains("underside")) {
        madeAs["underside"] = madeAs.value("underside", 0.0) + dz;
        o.attributes["ceiling_made"] = madeAs.dump();
        changed = true;
      }
    }
    for (const char* key : {"base_level", "top_level"}) {
      const auto it = o.attributes.find(key);
      if (it == o.attributes.end()) continue;
      const auto now = shifted(it->second);
      if (now) it->second = *now;
      else o.attributes.erase(it);
      changed = true;
    }
    if (changed) changes.modify(o), any = true;
    const std::string kind = kindOf(o);
    if (kind == "wall") ++out.walls;
    else if (kind == "slab") ++out.slabs;
    else if (kind == "room") ++out.rooms;
    else ++out.others;
    out.made.push_back(id);
  }
  if (any) history_.execute(changes.build("Copy Storey"));
  history_.endGroup();
  return out;
}

namespace {
// A wall's layer lines across it (from its outer face inwards), empty when it has no layers
// (or they no longer add up to its thickness).
std::vector<double> layerAcross(const WallBand& w) {
    std::vector<double> across;
    const auto layersText = w.object->attributes.find("wall_layers");
    const auto outside = w.object->attributes.find("wall_outside");
    if (layersText == w.object->attributes.end() || outside == w.object->attributes.end()) return across;
    const nlohmann::json layers = nlohmann::json::parse(layersText->second, nullptr, false);
    if (!layers.is_array() || layers.size() < 2) return across;
    double total = 0.0;
    for (const auto& l : layers) total += l.value("thickness", 0.0);
    if (std::abs(total - (w.right + w.leftExt)) > 1.0) return across;  // changed since: not the layers any more
    const bool outRight = outside->second == "right";
    double run = 0.0;
    for (std::size_t i = 0; i + 1 < layers.size(); ++i) {
      run += layers[i].value("thickness", 0.0);
      across.push_back(outRight ? -w.right + run : w.leftExt - run);
    }
    return across;
}

// --- mitred corners (docs/159) and T joints (docs/177) of walls -------------------------------
constexpr const char* kMitre = "corner_mitre";           // on a wall: its corners are mitred where the other wall's are too
constexpr const char* kMitreMade = "corner_mitre_made";  // what was made for it: made again only when this changes

// A wall's layer edges across it (its left coordinate), from its outer face inwards, and which
// layer is its core. None without layers that add up, or without a core.
struct LayerEdges {
  std::vector<double> edges;  // layer k lies between edges[k] and edges[k + 1]
  std::size_t core = 0;
};

std::optional<LayerEdges> layerEdges(const WallBand& w) {
  if (layerAcross(w).empty()) return std::nullopt;
  const nlohmann::json layers = nlohmann::json::parse(attributeOf(*w.object, "wall_layers"), nullptr, false);
  if (!layers.is_array()) return std::nullopt;
  const bool outRight = attributeOf(*w.object, "wall_outside") == "right";
  LayerEdges out;
  double e = outRight ? -w.right : w.leftExt;
  out.edges.push_back(e);
  bool found = false;
  for (std::size_t k = 0; k < layers.size(); ++k) {
    e += (outRight ? 1.0 : -1.0) * layers[k].value("thickness", 0.0);
    out.edges.push_back(e);
    if (!found && layers[k].value("role", std::string()) == "core") {
      out.core = k;
      found = true;
    }
  }
  if (!found) return std::nullopt;
  return out;
}

// A partition `p` whose end `end` meets wall `e` at a T (docs/177): p's core goes on through e's
// layers on that side up to e's core, and e gives those layers up over p's core, so that the
// boards turn the corner. In plan (world).
struct TeePieces {
  std::vector<gp_Pnt> fill;  // for p: its core from just inside its end up to e's core face
  std::vector<gp_Pnt> trim;  // for e: its layers on that side over p's core (and a little beyond its face)
  double faceAcross = 0.0, coreAcross = 0.0;  // e's face p meets and e's core face there (e's left coordinate)
  double core[2] = {0.0, 0.0};                // p's core edges (p's left coordinate)
};

std::optional<TeePieces> teeAt(const WallBand& p, int end, const WallBand& e) {
  if (std::abs(gp_Vec(p.dir).Crossed(gp_Vec(e.dir)).Z()) < 0.25) return std::nullopt;
  const auto pe = layerEdges(p), ee = layerEdges(e);
  if (!pe || !ee) return std::nullopt;
  const gp_Pnt at = p.start.Translated(gp_Vec(p.dir) * (end == 0 ? 0.0 : p.length));
  const double across = gp_Vec(e.start, gp_Pnt(at.X(), at.Y(), e.start.Z())).Dot(gp_Vec(e.left));
  const bool leftFace = std::abs(across - e.leftExt) < std::abs(across + e.right);
  TeePieces out;
  out.faceAcross = leftFace ? e.leftExt : -e.right;
  const double k0 = ee->edges[ee->core], k1 = ee->edges[ee->core + 1];
  out.coreAcross = std::abs(k0 - out.faceAcross) < std::abs(k1 - out.faceAcross) ? k0 : k1;
  if (std::abs(out.coreAcross - out.faceAcross) < 1.0) return std::nullopt;  // no layers on that side
  out.core[0] = pe->edges[pe->core];
  out.core[1] = pe->edges[pe->core + 1];
  // Where p's core edge k crosses e's line at `eAcross`.
  auto point = [&](int k, double eAcross) -> std::optional<gp_Pnt> {
    const gp_Pnt a = p.start.Translated(gp_Vec(p.left) * out.core[k]);
    const auto m = meet(a, p.dir, e.start.Translated(gp_Vec(e.left) * eAcross), e.dir);
    if (!m) return std::nullopt;
    return a.Translated(gp_Vec(p.dir) * m->first);
  };
  const double outward = leftFace ? 50.0 : -50.0;
  gp_Pnt coreAt[2], faceAt[2], beyond[2];
  for (int k = 0; k < 2; ++k) {
    const auto c = point(k, out.coreAcross), f = point(k, out.faceAcross), b = point(k, out.faceAcross + outward);
    if (!c || !f || !b) return std::nullopt;
    coreAt[k] = *c;
    faceAt[k] = *f;
    beyond[k] = *b;
  }
  const gp_Vec in = gp_Vec(p.dir) * (end == 0 ? 10.0 : -10.0);  // the fill overlaps p's own body by this much
  out.fill = {coreAt[0], coreAt[1], faceAt[1].Translated(in), faceAt[0].Translated(in)};
  out.trim = {coreAt[0], coreAt[1], beyond[1], beyond[0]};
  return out;
}

// The T joints between walls that both have the mark: a wall's end on another's face away from
// that wall's ends (where it is a corner).
struct Tee {
  std::size_t p = 0;
  int end = 0;
  std::size_t e = 0;
  TeePieces pieces;
};

std::vector<Tee> teeJoins(const std::vector<WallBand>& walls, const std::vector<std::array<WallEnd, 2>>& ends) {
  std::vector<Tee> out;
  for (std::size_t k = 0; k < walls.size(); ++k) {
    if (!walls[k].object->attributes.count(kMitre)) continue;
    for (int end = 0; end < 2; ++end) {
      const WallEnd& we = ends[k][static_cast<std::size_t>(end)];
      if (!we.partner || !we.joined || !walls[*we.partner].object->attributes.count(kMitre)) continue;
      const auto corner = cornerPartners(walls, ends, k, end);
      if (std::find(corner.begin(), corner.end(), *we.partner) != corner.end()) continue;
      if (const auto pieces = teeAt(walls[k], end, walls[*we.partner])) out.push_back({k, end, *we.partner, *pieces});
    }
  }
  return out;
}
}  // namespace

std::optional<Modeler::LayerUsage> Modeler::layerUsage(const Document& document, const Object& element) {
  const std::string kind = attributeOf(element, "kind");
  LayerUsage usage;
  if (kind == "wall") {
    const auto w = wallBand(document, element);
    if (!w || layerAcross(*w).empty()) return std::nullopt;  // none, or they no longer add up
    const nlohmann::json layers = nlohmann::json::parse(attributeOf(element, "wall_layers"), nullptr, false);
    for (const auto& l : layers) usage.layers.push_back({l.value("name", std::string()), l.value("thickness", 0.0)});
    usage.wall = true;
    usage.setName = attributeOf(element, "wall_type_name");
    if (usage.setName.empty()) usage.setName = attributeOf(element, "wall_type");
    // From its outer face: on the left of the drawn line the layers go right, and the other way.
    const bool outRight = attributeOf(element, "wall_outside") == "right";
    usage.offset = outRight ? -w->right : w->leftExt;
    usage.towardsLeft = outRight;
    return usage;
  }
  if (kind != "slab" && kind != "roof") return std::nullopt;
  if (typeLayerDepths(document, element).empty()) return std::nullopt;
  const std::string name = attributeOf(element, kTypeName);
  const nlohmann::json reg = typeRegister(document);
  for (const auto& l : reg.at(name).at("layers")) usage.layers.push_back({l.value("name", std::string()), l.value("thickness", 0.0)});
  usage.setName = name;
  return usage;
}

std::vector<std::pair<gp_Pnt, gp_Dir>> Modeler::wallLayerPlanes(const Document& document, const Object& wall) {
  std::vector<std::pair<gp_Pnt, gp_Dir>> out;
  const auto w = wallBand(document, wall);
  if (!w) return out;
  for (const double a : layerAcross(*w)) out.emplace_back(w->start.Translated(gp_Vec(w->left) * a), w->left);
  return out;
}

std::vector<std::pair<gp_Pnt, gp_Pnt>> Modeler::wallLayerLines(const Document& document, double z,
                                                               const std::set<ObjectId>& only) {
  std::vector<std::pair<gp_Pnt, gp_Pnt>> out;
  const std::vector<WallBand> walls = allWallBands(document);
  const auto acrossOf = [](const WallBand& w) { return layerAcross(w); };
  const auto ends = wallEnds(walls);
  const auto tees = teeJoins(walls, ends);
  for (std::size_t wi = 0; wi < walls.size(); ++wi) {
    const WallBand& w = walls[wi];
    if (!only.empty() && !only.count(w.object->id)) continue;
    if (z < w.z0 || z > w.z1) continue;
    const std::vector<double> across = acrossOf(w);
    if (across.empty()) continue;
    // Corners with a wall of the same build-up (docs/140): each layer line ends where it meets
    // the same layer's line of the other wall (counted from the outer faces), as a mitre.
    std::vector<std::array<double, 2>> endAt(across.size(), {0.0, w.length});
    for (int end = 0; end < 2; ++end) {
      for (const std::size_t j : cornerPartners(walls, ends, wi, end)) {
        const WallBand& o = walls[j];
        if (o.object->attributes.count("wall_layers") == 0 ||
            o.object->attributes.at("wall_layers") != w.object->attributes.at("wall_layers"))
          continue;
        const std::vector<double> other = acrossOf(o);
        if (other.size() != across.size()) continue;
        // The outer faces must meet at the outside of the corner: the outermost lines' meeting
        // point is the farthest from the inside of the corner, i.e. along each wall it lies
        // beyond the innermost lines' meeting point.
        std::vector<double> at(across.size());
        bool fits = true;
        for (std::size_t k = 0; k < across.size() && fits; ++k) {
          const auto m = meet(w.start.Translated(gp_Vec(w.left) * across[k]), w.dir, o.start.Translated(gp_Vec(o.left) * other[k]), o.dir);
          if (!m) fits = false;
          else at[k] = m->first;
        }
        if (!fits) continue;
        const double s = end == 0 ? 0.0 : w.length, reach = w.right + w.leftExt + o.right + o.leftExt;
        bool near = true;
        for (const double a : at) near = near && std::abs(a - s) <= reach;
        // Outermost beyond innermost (seen from the wall's middle): the outer faces are on one side.
        const bool outward = end == 0 ? at.front() <= at.back() + 1e-6 : at.front() >= at.back() - 1e-6;
        if (!near || !outward) continue;
        for (std::size_t k = 0; k < across.size(); ++k) endAt[k][static_cast<std::size_t>(end)] = at[k];
        break;
      }
    }
    // T joints (docs/177): this partition's core lines go on to the other wall's core face; that
    // wall's lines between its face and its core break over the partition's core.
    std::vector<std::vector<std::pair<double, double>>> lineGaps(across.size());
    for (const Tee& t : tees) {
      if (t.p == wi) {
        const WallBand& e = walls[t.e];
        for (std::size_t k = 0; k < across.size(); ++k) {
          if (std::abs(across[k] - t.pieces.core[0]) > 1e-6 && std::abs(across[k] - t.pieces.core[1]) > 1e-6) continue;
          const auto m = meet(w.start.Translated(gp_Vec(w.left) * across[k]), w.dir, e.start.Translated(gp_Vec(e.left) * t.pieces.coreAcross), e.dir);
          if (m) endAt[k][static_cast<std::size_t>(t.end)] = m->first;
        }
      }
      if (t.e == wi) {
        const WallBand& p = walls[t.p];
        const double lo = std::min(t.pieces.faceAcross, t.pieces.coreAcross), hi = std::max(t.pieces.faceAcross, t.pieces.coreAcross);
        for (std::size_t k = 0; k < across.size(); ++k) {
          // docs/196 (owner, checklist 2-8): the line of its core face breaks too, so the
          // partition's core runs into this wall's core with no line across the partition.
          const bool coreFace = std::abs(across[k] - t.pieces.coreAcross) <= 1e-6;
          if (!coreFace && (across[k] <= lo + 1e-6 || across[k] >= hi - 1e-6)) continue;
          const gp_Pnt line = w.start.Translated(gp_Vec(w.left) * across[k]);
          const auto m0 = meet(line, w.dir, p.start.Translated(gp_Vec(p.left) * t.pieces.core[0]), p.dir);
          const auto m1 = meet(line, w.dir, p.start.Translated(gp_Vec(p.left) * t.pieces.core[1]), p.dir);
          if (m0 && m1) lineGaps[k].emplace_back(std::min(m0->first, m1->first), std::max(m0->first, m1->first));
        }
      }
    }
    // Along the wall: its length less the openings the plan passes through.
    std::vector<std::pair<double, double>> gaps;
    if (const auto openings = openingsIn(document, w.object->id)) {
      for (const OpeningInfo& o : *openings) {
        if (std::abs(o.normal.Z()) > 1e-6 || z < o.bottom || z > o.bottom + o.height) continue;
        const double c = gp_Vec(w.start, gp_Pnt(o.center.X(), o.center.Y(), w.start.Z())).Dot(gp_Vec(w.dir));
        gaps.emplace_back(c - o.along / 2, c + o.along / 2);
      }
    }
    auto runsOf = [&](std::vector<std::pair<double, double>> cuts) {
      std::sort(cuts.begin(), cuts.end());
      std::vector<std::pair<double, double>> runs;
      double from = 0.0;
      for (const auto& [g0, g1] : cuts) {
        if (g0 > from + 1e-6) runs.emplace_back(from, std::min(g0, w.length));
        from = std::max(from, g1);
      }
      if (from < w.length - 1e-6) runs.emplace_back(from, w.length);
      return runs;
    };
    for (std::size_t k = 0; k < across.size(); ++k) {
      std::vector<std::pair<double, double>> cuts = gaps;
      cuts.insert(cuts.end(), lineGaps[k].begin(), lineGaps[k].end());
      for (const auto& [r0, r1] : runsOf(std::move(cuts))) {
        const double a = across[k];
        // The wall's own ends moved to the mitre (a run starting / ending at an opening stays).
        const double s0 = r0 <= 1e-6 ? endAt[k][0] : r0, s1 = r1 >= w.length - 1e-6 ? endAt[k][1] : r1;
        if (s1 - s0 < 1e-6) continue;
        gp_Pnt p0 = w.start.Translated(gp_Vec(w.dir) * s0 + gp_Vec(w.left) * a);
        gp_Pnt p1 = w.start.Translated(gp_Vec(w.dir) * s1 + gp_Vec(w.left) * a);
        p0.SetZ(z);
        p1.SetZ(z);
        out.emplace_back(p0, p1);
      }
    }
  }
  return out;
}

Result<std::string> Modeler::pasteOpening(const ObjectId& host, const std::string& openingId, const gp_Pnt& point) {
  using R = Result<std::string>;
  const auto list = openings(host);
  if (!list) return R::failure("The copied opening is no longer there");
  const auto original = std::find_if(list->begin(), list->end(), [&](const OpeningInfo& o) { return o.id == openingId; });
  if (original == list->end()) return R::failure("The copied opening is no longer there");
  // The point as given (a face of a wall in 3D), else at the opening's own height (a plan).
  std::optional<ObjectId> into = wallAt(point);
  if (!into) into = wallAt(gp_Pnt(point.X(), point.Y(), original->center.Z()));
  if (!into) return R::failure("Click on a wall to paste the opening into");
  if (*into == host) {
    const gp_Vec along(point.X() - original->center.X(), point.Y() - original->center.Y(), 0.0);
    return copyOpening(host, original->index, along);
  }
  return copyOpening(host, original->index, gp_Vec(point.XYZ()), *into);
}

std::optional<ObjectId> Modeler::wallAt(const gp_Pnt& p) const {
  std::optional<ObjectId> found;
  double best = 1e300;
  for (const WallBand& w : allWallBands(document_)) {
    if (p.Z() < w.z0 - tolerance::kLinear || p.Z() > w.z1 + tolerance::kLinear) continue;
    const gp_Vec to(w.start, gp_Pnt(p.X(), p.Y(), w.start.Z()));
    const double along = to.Dot(gp_Vec(w.dir)), across = to.Dot(gp_Vec(w.left));
    if (along < -tolerance::kLinear || along > w.length + tolerance::kLinear) continue;
    if (across < -w.right - tolerance::kLinear || across > w.leftExt + tolerance::kLinear) continue;
    const double off = std::abs(across - (w.leftExt - w.right) / 2);
    if (off < best) best = off, found = w.object->id;
  }
  return found;
}

std::vector<std::pair<std::size_t, std::string>> Modeler::wallsMeetingOpenings(const ObjectId& host) const {
  std::vector<std::pair<std::size_t, std::string>> out;
  const auto list = openings(host);
  if (!list || list->empty()) return out;
  const std::vector<WallBand> walls = allWallBands(document_);
  std::optional<std::size_t> hostIndex;
  for (std::size_t i = 0; i < walls.size(); ++i)
    if (walls[i].object->id == host) hostIndex = i;
  if (!hostIndex) return out;
  const auto ends = wallEnds(walls);
  for (std::size_t k = 0; k < walls.size(); ++k) {
    if (k == *hostIndex) continue;
    const WallBand& w = walls[k];
    for (int end = 0; end < 2; ++end) {
      const WallEnd& e = ends[k][static_cast<std::size_t>(end)];
      // Its end at the host: on a face of it, or inside it (drawn to the host's line).
      if (!e.partner || *e.partner != *hostIndex) continue;
      // Where its line meets the host, and how wide it is along the host.
      const gp_Pnt at = w.start.Translated(gp_Vec(w.dir) * (end == 0 ? 0.0 : w.length));
      const double half = (w.right + w.leftExt) / 2;
      for (const OpeningInfo& o : *list) {
        if (std::abs(o.normal.Z()) > 1e-6) continue;
        const gp_Vec u(gp::DZ().Crossed(o.normal));
        const double du = gp_Vec(o.center, gp_Pnt(at.X(), at.Y(), o.center.Z())).Dot(u);
        if (std::abs(du) >= o.along / 2 + half - tolerance::kLinear) continue;
        if (w.z1 <= o.bottom + tolerance::kLinear || w.z0 >= o.bottom + o.height - tolerance::kLinear) continue;
        out.emplace_back(o.index, w.object->name);
      }
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

namespace {

void followWallJoins(const Document& document, ChangeSet& changes, const RegenContext& context,
                     const std::vector<ObjectId>& roots, const gp_Vec& d, JoinFollow& report) {
  const gp_Vec flat(d.X(), d.Y(), 0);
  if (flat.Magnitude() < tolerance::kLinear) return;
  std::set<ObjectId> moved;
  for (const ObjectId& rootId : roots)
    for (const ObjectId& id : document.subtree(rootId)) moved.insert(id);
  const std::vector<WallBand> walls = allWallBands(document);
  std::map<std::size_t, gp_Vec> displaced, along;
  std::map<std::size_t, std::array<bool, 2>> movedEnds;
  std::set<std::size_t> primary;
  for (std::size_t i = 0; i < walls.size(); ++i) {
    if (!moved.count(walls[i].object->id)) continue;
    primary.insert(i);
    displaced[i] = flat;
    along[i] = gp_Vec(walls[i].dir) * flat.Dot(gp_Vec(walls[i].dir));
    movedEnds[i] = {true, true};
  }
  if (primary.empty()) return;
  const auto ends = wallEnds(walls);
  carryCorners(document, changes, walls, ends, along, movedEnds, displaced, primary, report);
  settleWallJoins(document, changes, context, walls, ends, displaced, primary, {}, moved, report);
}

// A wall's far end moved along its line by `delta` (its length changed): the walls at that
// corner are carried, and what is joined to them follows. `wall`'s own shape is the caller's.
void followWallLength(const Document& document, ChangeSet& changes, const RegenContext& context, const ObjectId& wall,
                      double delta, JoinFollow& report) {
  if (std::abs(delta) < tolerance::kLinear) return;
  const std::vector<WallBand> walls = allWallBands(document);
  std::optional<std::size_t> index;
  for (std::size_t i = 0; i < walls.size(); ++i)
    if (walls[i].object->id == wall) index = i;
  if (!index) return;
  const auto ends = wallEnds(walls);
  std::map<std::size_t, gp_Vec> displaced;
  const std::map<std::size_t, gp_Vec> along = {{*index, gp_Vec(walls[*index].dir) * delta}};
  const std::map<std::size_t, std::array<bool, 2>> movedEnds = {{*index, {false, true}}};
  carryCorners(document, changes, walls, ends, along, movedEnds, displaced, {}, report);
  settleWallJoins(document, changes, context, walls, ends, displaced, {}, {*index}, {wall}, report);
}

void wallPinUp(const Document& document, ChangeSet& changes, const RegenContext& context) {
  std::vector<WallBand> walls;
  bool any = false;
  for (const Object& o : document.objects()) any |= o.attributes.count(kWallGridPin) > 0;
  if (!any) return;
  walls = allWallBands(document);
  // A grid's world placement after `changes`.
  std::function<gp_Trsf(const ObjectId&)> after = [&](const ObjectId& id) -> gp_Trsf {
    const Object* p = changes.pending(id);
    const Object* o = p ? p : document.find(id);
    if (!o) return gp_Trsf();
    return o->parent.empty() ? o->transform : after(o->parent) * o->transform;
  };
  std::map<std::size_t, gp_Vec> displaced;
  std::set<std::size_t> pinned;
  std::set<ObjectId> moved;
  for (std::size_t i = 0; i < walls.size(); ++i) {
    const WallBand& w = walls[i];
    const Object& was = *w.object;
    const std::string text = attributeOf(was, kWallGridPin);
    if (text.empty() || was.locked || changes.erased(was.id) || changes.pending(was.id)) continue;
    const nlohmann::json pin = nlohmann::json::parse(text, nullptr, false);
    if (!pin.is_object() || !pin.contains("grid") || !pin.contains("ends") || pin.at("ends").size() != 2) continue;
    const std::string gridId = pin.value("grid", std::string());
    const Object* gridBefore = document.find(gridId);
    const Object* gridAfter = changes.erased(gridId) ? nullptr : changes.pending(gridId);
    if (!gridBefore || !gridAfter) continue;  // the grid is as it was (or gone)
    const GridSpec* specBefore = gridOf(gridBefore);
    const GridSpec* specAfter = gridOf(gridAfter);
    if (!specBefore || !specAfter) continue;
    gp_Vec d[2];
    bool found = true;
    for (int k = 0; k < 2; ++k) {
      const nlohmann::json& e = pin.at("ends")[static_cast<std::size_t>(k)];
      if (!e.is_array() || e.size() < 2 || !e[0].is_string() || !e[1].is_string()) {
        found = false;
        break;
      }
      const auto from = crossingOf(*specBefore, document.worldTransform(*gridBefore), e[0].get<std::string>(), e[1].get<std::string>());
      const auto to = crossingOf(*specAfter, after(gridId), e[0].get<std::string>(), e[1].get<std::string>());
      if (!from || !to) {
        found = false;
        break;
      }
      d[k] = gp_Vec(to->X() - from->X(), to->Y() - from->Y(), 0.0);
    }
    if (!found || (d[0].Magnitude() < tolerance::kLinear && d[1].Magnitude() < tolerance::kLinear)) continue;
    // Across its line both ends go alike (else it would turn: left as it is); along it, each its own way.
    const double across0 = d[0].Dot(gp_Vec(w.left)), across1 = d[1].Dot(gp_Vec(w.left));
    if (std::abs(across0 - across1) > tolerance::kLinear) continue;
    const double s0 = d[0].Dot(gp_Vec(w.dir)), s1 = w.length + d[1].Dot(gp_Vec(w.dir));
    if (s1 - s0 < tolerance::kMinDimension) continue;
    Object object = was;
    if (std::abs(s0) > tolerance::kLinear || std::abs(s1 - w.length) > tolerance::kLinear) {
      Recipe recipe = w.recipe;
      auto& rect = std::get<RectangleProfile>(recipe.base);
      const gp_Pnt p0 = w.start.Translated(gp_Vec(w.dir) * s0), p1 = w.start.Translated(gp_Vec(w.dir) * s1);
      rect.plane.origin = p0.Translated(gp_Vec(w.left) * -w.right).Transformed(w.toWorld.Inverted());
      rect.width = s1 - s0;
      if (w.path.empty()) object.recipe = std::move(recipe);
      else replaceOperand(object.recipe, w.path, 0, std::move(recipe));
      // Its openings stay where they are along it: all must still be in the wall.
      bool inside = true;
      for (const OpeningExtent& e : openingExtents(was, document.worldTransform(was))) {
        const gp_Vec u(gp::DZ().Crossed(e.normal));
        const double a = gp_Vec(p0.XYZ()).Dot(u), b = gp_Vec(p1.XYZ()).Dot(u);
        inside &= e.u0 >= std::min(a, b) - tolerance::kLinear && e.u1 <= std::max(a, b) + tolerance::kLinear;
      }
      if (!inside || !rebuild(object, context)) continue;
    }
    const gp_Vec sideways = gp_Vec(w.left) * across0;
    if (sideways.Magnitude() > tolerance::kLinear) {
      gp_Trsf t;
      t.SetTranslation(sideways);
      object.transform = localDelta(document.parentTransform(was), t) * object.transform;
      displaced[i] = sideways;
    }
    changes.modify(object);
    pinned.insert(i);
    moved.insert(was.id);
  }
  // Walls (and slab corners) joined to a pinned wall that went sideways follow it.
  if (!displaced.empty()) {
    JoinFollow report;
    settleWallJoins(document, changes, context, walls, wallEnds(walls), displaced, pinned, pinned, moved, report);
  }
}

// --- rooms (docs/90 G-07) ------------------------------------------------------------------
constexpr double kRoomCut = 1100.0;  // a wall bounds rooms on a floor when a plan cut this high meets it
const FloorLevel* levelAtHeight(const std::vector<FloorLevel>& levels, double z);  // below

struct StoreyWalls {
  std::vector<geom::PlanWall> plan;
  std::vector<ObjectId> ids;
  int unreadable = 0;
};

StoreyWalls storeyWalls(const Document& document, double floor) {
  StoreyWalls out;
  const double cut = floor + kRoomCut;
  for (const Object& o : document.objects()) {
    const auto kind = o.attributes.find("kind");
    if (kind == o.attributes.end() || kind->second != "wall" || o.shape.IsNull()) continue;
    const auto w = wallBand(document, o);
    if (!w) {
      const Bnd_Box box = document.worldBounds(o);
      if (!box.IsVoid() && box.CornerMin().Z() <= cut && cut < box.CornerMax().Z()) ++out.unreadable;
      continue;
    }
    if (!(w->z0 <= cut && cut < w->z1)) continue;
    const gp_Pnt end = w->start.Translated(gp_Vec(w->dir) * w->length);
    out.plan.push_back({{w->start.X(), w->start.Y()}, {end.X(), end.Y()}, w->right, w->leftExt});
    out.ids.push_back(o.id);
  }
  return out;
}

// The walls of a storey and the regions they enclose, worked out once for every room at that
// height (docs/118: per room it grew with the square of the rooms).
struct StoreyRegions {
  StoreyWalls walls;
  geom::RoomRegions regions;
};

StoreyRegions storeyRegions(const Document& document, double floor) {
  StoreyRegions out;
  out.walls = storeyWalls(document, floor);
  if (!out.walls.plan.empty()) out.regions = geom::roomRegions(out.walls.plan);
  return out;
}

RoomInfo regionInfo(const Document& document, const gp_Pnt& at, const StoreyRegions& storey);

RoomInfo regionInfo(const Document& document, const gp_Pnt& at) {
  return regionInfo(document, at, storeyRegions(document, at.Z()));
}

RoomInfo regionInfo(const Document& document, const gp_Pnt& at, const StoreyRegions& storey) {
  RoomInfo info;
  info.at = at;
  if (const FloorLevel* level = levelAtHeight(document.settings().levels, at.Z())) info.level = level->id;
  const StoreyWalls& walls = storey.walls;
  info.unreadableWalls = walls.unreadable;
  if (walls.plan.empty()) {
    info.problem = "No walls made by the wall tool stand at this height";
    return info;
  }
  const geom::RoomRegions& regions = storey.regions;
  const auto found = geom::regionAt(regions, {at.X(), at.Y()});
  if (!found) {
    info.problem = "Not enclosed by walls here";
    // The loose wall ends nearest the place: where the outline is likely open.
    std::vector<gp_Pnt> ends;
    for (const geom::PlanPoint& p : regions.looseEnds) ends.emplace_back(p.x, p.y, at.Z());
    std::sort(ends.begin(), ends.end(), [&](const gp_Pnt& a, const gp_Pnt& b) { return a.Distance(at) < b.Distance(at); });
    if (ends.size() > 4) ends.resize(4);
    info.looseEnds = std::move(ends);
    return info;
  }
  const geom::RoomRegion& region = regions.regions[*found];
  info.closed = true;
  info.centreArea = region.centreArea;
  info.innerKnown = region.innerKnown;
  info.innerArea = region.innerArea;
  info.innerPerimeter = region.innerPerimeter;
  for (const geom::PlanPoint& p : region.centre) info.centre.emplace_back(p.x, p.y, at.Z());
  for (const geom::PlanPoint& p : region.inner) info.inner.emplace_back(p.x, p.y, at.Z());
  for (std::size_t i : region.walls) info.walls.push_back(walls.ids[i]);
  return info;
}

bool isRoomMarker(const Object& o) {
  const auto kind = o.attributes.find("kind");
  return kind != o.attributes.end() && kind->second == "room" && std::holds_alternative<TextNote>(o.recipe.base);
}

// --- walls and slabs on levels (docs/90 G-02) ---------------------------------------------
constexpr const char* kBaseLevel = "base_level";
constexpr const char* kTopLevel = "top_level";

std::string attributeText(const Object& o, const char* key) {
  const auto it = o.attributes.find(key);
  return it == o.attributes.end() ? std::string() : it->second;
}

// The level at height z, if any: one named "...FL" first (GL and 1FL may share a height).
const FloorLevel* levelAtHeight(const std::vector<FloorLevel>& levels, double z) {
  const FloorLevel* found = nullptr;
  for (const FloorLevel& l : levels) {
    if (std::abs(l.elevation - z) > 1e-6) continue;
    const bool floor = l.name.size() >= 2 && l.name.compare(l.name.size() - 2, 2, "FL") == 0;
    if (!found || floor) found = &l;
    if (floor) break;
  }
  return found;
}

// Moves a wall's foot by `dBase` and its top by `dTop` (world Z; its openings stay at their
// height above the foot), or a slab as a whole by `dTop`. `was` is the object in the document,
// `o` the copy being changed. False (and `o` is not to be used) when a wall would be left
// without height, is no longer the pushed-up rectangle its tool made, or cannot be rebuilt.
bool shiftOnLevels(const Document& document, const Object& was, Object& o, double dBase, double dTop,
                   const RegenContext& context) {
  const bool wall = attributeText(was, "kind") == "wall";
  const double move = wall ? dBase : dTop;
  if (std::abs(move) > 1e-9) {
    gp_Trsf t;
    t.SetTranslation(gp_Vec(0, 0, move).Transformed(document.parentTransform(was).Inverted()));
    o.transform = t * o.transform;
  }
  const double grow = wall ? dTop - dBase : 0.0;
  if (std::abs(grow) < 1e-9) return true;
  const auto band = wallBand(document, was);
  if (!band) return false;
  Recipe recipe = band->recipe;
  auto& push = std::get<ExtrudeOp>(recipe.modifiers.front());
  const double height = std::abs(push.distance) + grow;
  if (height < tolerance::kMinDimension) return false;
  push.distance = std::copysign(height, push.distance);
  if (band->path.empty()) o.recipe = std::move(recipe);
  else replaceOperand(o.recipe, band->path, 0, std::move(recipe));
  return static_cast<bool>(rebuild(o, context));
}

void levelUp(const Document& document, ChangeSet& changes, const RegenContext& context,
             const std::vector<FloorLevel>& before, const std::vector<FloorLevel>& after) {
  auto moved = [&](const std::string& level) {
    if (level.empty()) return 0.0;
    bool had = false, has = false;
    const double from = levelHeight(before, level, had), to = levelHeight(after, level, has);
    return had && has ? to - from : 0.0;  // a level that is gone: what stood on it stays put
  };
  std::vector<ObjectId> ids;
  for (const Object& o : document.objects())
    if ((o.attributes.count(kBaseLevel) || o.attributes.count(kTopLevel)) && !changes.erased(o.id)) ids.push_back(o.id);
  for (const ObjectId& id : ids) {
    const Object* was = document.find(id);
    if (!was || was->locked) continue;
    const std::string base = attributeText(*was, kBaseLevel), top = attributeText(*was, kTopLevel);
    const double dBase = moved(base);
    // A top that is on no level goes with the foot: the wall keeps its height.
    const double dTop = top.empty() ? dBase : moved(top);
    if (std::abs(dBase) < 1e-9 && std::abs(dTop) < 1e-9) continue;
    const Object* pending = changes.pending(id);
    Object o = pending ? *pending : *was;
    if (attributeText(*was, "kind") == "stair") {
      if (shiftStair(document, *was, o, dBase, dTop, context)) changes.modify(o);
      continue;
    }
    if (shiftOnLevels(document, *was, o, dBase, dTop, context)) {
      changes.modify(o);
      continue;
    }
    // The height could not follow (an edited wall, or it would vanish): the foot still does.
    Object rigid = pending ? *pending : *was;
    if (std::abs(dBase) > 1e-9 && shiftOnLevels(document, *was, rigid, dBase, dBase, context)) changes.modify(rigid);
  }
}
}  // namespace

// --- roofs (docs/90 G-08) ------------------------------------------------------------------
namespace {
std::string roofSpecText(const geom::RoofSpec& s) {
  nlohmann::json outline = nlohmann::json::array();
  for (const gp_Pnt& p : s.outline) outline.push_back({p.X(), p.Y()});
  nlohmann::json j{{"kind", s.kind}, {"outline", outline}, {"height", s.height}, {"pitch", s.pitch},
                   {"eave", s.eave}, {"gable_eave", s.gableEave}, {"thickness", s.thickness},
                   {"ridge", s.ridge}, {"fall", {s.fallX, s.fallY}}, {"gable_sides", s.gableSides}};
  // docs/160: sides with a pitch / an overhang of their own (left out when there are none).
  nlohmann::json sides = nlohmann::json::array();
  for (const auto& [side, pitch] : s.sidePitch) {
    nlohmann::json one{{"side", side}, {"pitch", pitch}};
    if (const auto it = s.sideEave.find(side); it != s.sideEave.end()) one["eave"] = it->second;
    sides.push_back(one);
  }
  for (const auto& [side, eave] : s.sideEave)
    if (!s.sidePitch.count(side)) sides.push_back({{"side", side}, {"eave", eave}});
  if (!sides.empty()) j["sides"] = sides;
  if (!s.wallSides.empty()) j["wall_sides"] = s.wallSides;  // docs/161
  return j.dump();
}

std::optional<geom::RoofSpec> roofSpecFrom(const std::string& text) {
  try {
    const nlohmann::json j = nlohmann::json::parse(text);
    geom::RoofSpec s;
    s.kind = j.at("kind").get<std::string>();
    for (const auto& p : j.at("outline")) s.outline.emplace_back(p.at(0).get<double>(), p.at(1).get<double>(), 0.0);
    s.height = j.at("height").get<double>();
    s.pitch = j.at("pitch").get<double>();
    s.eave = j.at("eave").get<double>();
    s.gableEave = j.at("gable_eave").get<double>();
    s.thickness = j.at("thickness").get<double>();
    s.ridge = j.at("ridge").get<std::string>();
    s.fallX = j.at("fall").at(0).get<double>();
    s.fallY = j.at("fall").at(1).get<double>();
    s.gableSides = j.value("gable_sides", std::vector<int>{});  // roofs made before docs/138: chosen
    s.wallSides = j.value("wall_sides", std::vector<int>{});
    if (j.contains("sides"))
      for (const auto& one : j.at("sides")) {
        const int side = one.at("side").get<int>();
        if (one.contains("pitch")) s.sidePitch[side] = one.at("pitch").get<double>();
        if (one.contains("eave")) s.sideEave[side] = one.at("eave").get<double>();
      }
    return s;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// The stored roof (in the object's own coordinates) and where the object stands, or why it is
// no longer a roof whose sizes can be read. `o` may be a changed copy not in the document yet.
Result<std::pair<geom::RoofSpec, gp_Trsf>> storedRoofOf(const Document& document, const Object* o) {
  using R = Result<std::pair<geom::RoofSpec, gp_Trsf>>;
  if (!o) return R::failure("Object not found");
  const auto kind = o->attributes.find("kind");
  const auto text = o->attributes.find("roof_spec");
  if (kind == o->attributes.end() || kind->second != "roof" || text == o->attributes.end())
    return R::failure("'" + o->name + "' is not a roof made by the roof tool");
  if (!std::holds_alternative<ImportedShape>(o->recipe.base) || !o->recipe.modifiers.empty())
    return R::failure("'" + o->name + "' was reshaped by hand: its roof sizes can no longer be read or changed");
  const auto spec = roofSpecFrom(text->second);
  if (!spec) return R::failure("'" + o->name + "': its roof sizes cannot be read");
  const gp_Trsf world = document.worldTransform(*o);
  if (std::abs(world.ScaleFactor() - 1.0) > 1e-9 || gp_Vec(0, 0, 1).Transformed(world).Z() < 1.0 - 1e-9)
    return R::failure("'" + o->name + "' was tilted or scaled: its roof sizes can no longer be read or changed");
  return std::make_pair(*spec, world);
}

Result<std::pair<geom::RoofSpec, gp_Trsf>> storedRoof(const Document& document, const ObjectId& id) {
  return storedRoofOf(document, document.find(id));
}

geom::RoofSpec roofInFrame(geom::RoofSpec s, const gp_Trsf& t);

// --- mitred wall corners (docs/159) --------------------------------------------------------

// The mitre of wall `w`'s end (0 start, 1 far end) with `o` at their corner, in plan (world):
// `fill` is the wall's band from just inside its body up to the mitre line (the line through the
// points where the walls' faces meet on the inside and on the outside of the corner), `trim`
// what lies beyond that line. Nothing for turns too shallow to be a corner.
struct MitrePieces {
  std::vector<gp_Pnt> fill, trim;
};

std::optional<MitrePieces> mitreAt(const WallBand& w, int end, const WallBand& o) {
  if (std::abs(gp_Vec(w.dir).Crossed(gp_Vec(o.dir)).Z()) < 0.25) return std::nullopt;
  auto face = [](const WallBand& b, int left) { return b.start.Translated(gp_Vec(b.left) * (left ? b.leftExt : -b.right)); };
  gp_Pnt q[2][2];
  gp_XYZ sum(0, 0, 0);
  for (int a = 0; a < 2; ++a) {
    for (int b = 0; b < 2; ++b) {
      const auto m = meet(face(w, a), w.dir, face(o, b), o.dir);
      if (!m) return std::nullopt;
      q[a][b] = face(w, a).Translated(gp_Vec(w.dir) * m->first);
      sum += q[a][b].XYZ();
    }
  }
  const gp_Pnt centre(sum / 4.0);
  // Into each wall's body from the corner.
  const gp_Vec intoW = gp_Vec(w.dir) * (end == 0 ? 1.0 : -1.0);
  const gp_Vec intoO = gp_Vec(o.dir) * (gp_Vec(o.start, centre).Dot(gp_Vec(o.dir)) < o.length / 2 ? 1.0 : -1.0);
  auto cross = [](const gp_Vec& a, const gp_Vec& b) { return a.X() * b.Y() - a.Y() * b.X(); };
  // The diagonal of the corner's quadrilateral with the two bodies on opposite sides.
  gp_Pnt m0, m1;
  bool found = false;
  for (int k = 0; k < 2 && !found; ++k) {
    m0 = q[0][k];      // on w's right face
    m1 = q[1][1 - k];  // on w's left face
    const gp_Vec d(m0, m1);
    if (d.Magnitude() < 1e-6) return std::nullopt;
    found = cross(d, intoW) * cross(d, intoO) < 0;
  }
  if (!found) return std::nullopt;
  const double p0 = gp_Vec(w.start, m0).Dot(gp_Vec(w.dir)), p1 = gp_Vec(w.start, m1).Dot(gp_Vec(w.dir));
  const double inside = 10.0;  // the fill overlaps the wall's own body by this much
  const double target = end == 1 ? std::min({p0, p1, w.length}) - inside : std::max({p0, p1, 0.0}) + inside;
  if (end == 1 ? target <= 0.0 : target >= w.length) return std::nullopt;  // shorter than its corner
  MitrePieces out;
  out.fill = {m0, m1, m1.Translated(gp_Vec(w.dir) * (target - p1)), m0.Translated(gp_Vec(w.dir) * (target - p0))};
  const gp_Vec along = gp_Vec(m0, m1).Normalized();
  const double thickness = w.right + w.leftExt + o.right + o.leftExt;
  const double at = end == 1 ? w.length : 0.0;
  const double beyond = std::abs(at - p0) + std::abs(at - p1) + 2 * thickness + 500.0;
  const gp_Pnt a = m0.Translated(along * -500.0), b = m1.Translated(along * 500.0);
  out.trim = {a, b, b.Translated(intoW * -beyond), a.Translated(intoW * -beyond)};
  return out;
}

// A prism over a plan polygon (world) from z0 to z1, in the frame `toLocal` leads to.
TopoDS_Shape planPrism(std::vector<gp_Pnt> polygon, double z0, double z1, const gp_Trsf& toLocal) {
  double area2 = 0.0;
  for (std::size_t i = 0; i < polygon.size(); ++i) {
    const gp_Pnt& p = polygon[i];
    const gp_Pnt& n = polygon[(i + 1) % polygon.size()];
    area2 += p.X() * n.Y() - n.X() * p.Y();
  }
  if (std::abs(area2) < 1e-6 || z1 - z0 < 1e-6) return {};
  if (area2 < 0) std::reverse(polygon.begin(), polygon.end());
  BRepBuilderAPI_MakePolygon wire;
  for (const gp_Pnt& p : polygon) wire.Add(gp_Pnt(p.X(), p.Y(), z0));
  wire.Close();
  if (!wire.IsDone()) return {};
  BRepBuilderAPI_MakeFace face(wire.Wire(), Standard_True);
  if (!face.IsDone()) return {};
  BRepPrimAPI_MakePrism prism(face.Face(), gp_Vec(0, 0, z1 - z0));
  if (!prism.IsDone()) return {};
  return BRepBuilderAPI_Transform(prism.Shape(), toLocal, Standard_True).Shape();
}

// The recipe `depth` operands down the first operands.
const Recipe* firstOperandDown(const Recipe& root, std::size_t depth) {
  const Recipe* r = &root;
  for (std::size_t i = 0; i < depth; ++i) {
    const auto* b = std::get_if<BooleanFeature>(&r->base);
    if (!b || b->operands.empty() || !b->operands.front().recipe) return nullptr;
    r = b->operands.front().recipe.get();
  }
  return r;
}

// The wall's own rectangle with these corner pieces (given in its frame), or bare without any.
void setCornerPieces(Object& o, const WallBand& band, const std::vector<TopoDS_Shape>& fills,
                     const std::vector<TopoDS_Shape>& trims) {
  // Where the rectangle (with the corner pieces it has now) sits in the chain of cuts.
  // (Either may be missing: a T joint gives a partition a fill only and its wall a trim only.)
  std::size_t depth = band.path.size();
  if (depth > 0) {
    const Recipe* parent = firstOperandDown(o.recipe, depth - 1);
    if (parent && cornerNode(*parent, BooleanKind::Union, kCornerFill)) --depth;
  }
  if (depth > 0) {
    const Recipe* outer = firstOperandDown(o.recipe, depth - 1);
    if (outer && cornerNode(*outer, BooleanKind::Subtract, kCornerTrim)) --depth;
  }
  Recipe node = band.recipe;
  auto piece = [](const TopoDS_Shape& shape, const char* id, std::size_t i) {
    Recipe r;
    r.base = ImportedShape{shape, id};
    return BooleanOperand{std::make_shared<const Recipe>(std::move(r)), gp_Trsf(), "Corner", std::string(id) + "-" + std::to_string(i)};
  };
  if (!fills.empty()) {
    BooleanFeature filled;
    filled.kind = BooleanKind::Union;
    filled.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(std::move(node)), gp_Trsf(), o.name, {}});
    for (std::size_t i = 0; i < fills.size(); ++i) filled.operands.push_back(piece(fills[i], kCornerFill, i));
    node = Recipe{};
    node.base = std::move(filled);
  }
  if (!trims.empty()) {
    BooleanFeature trimmed;
    trimmed.kind = BooleanKind::Subtract;
    trimmed.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(std::move(node)), gp_Trsf(), o.name, {}});
    for (std::size_t i = 0; i < trims.size(); ++i) trimmed.operands.push_back(piece(trims[i], kCornerTrim, i));
    node = Recipe{};
    node.base = std::move(trimmed);
  }
  if (depth == 0) o.recipe = std::move(node);
  else replaceOperand(o.recipe, std::vector<std::size_t>(depth, 0), 0, std::move(node));
}

// Walls with mitred corners after the changes in `changes` (moved, longer, thicker, their
// neighbour gone, the flag set or taken off): their corner pieces made again. A corner is
// mitred when exactly two walls meet there and both have the flag. Returns the wall ends mitred.
int refitWallCorners(const Document& document, ChangeSet& changes, const RegenContext& context) {
  std::vector<const Object*> objects;
  bool any = false;
  for (const Object& stored : document.objects()) {
    if (changes.erased(stored.id)) continue;
    const Object* now = changes.pending(stored.id);
    if (!now) now = &stored;
    if (attributeOf(*now, "kind") != "wall") continue;
    objects.push_back(now);
    any = any || now->attributes.count(kMitre) || now->attributes.count(kMitreMade);
  }
  if (!any) return 0;
  std::vector<WallBand> bands;
  for (const Object* o : objects)
    if (const auto band = wallBand(document, *o)) bands.push_back(*band);
  const auto ends = wallEnds(bands);
  const auto tees = teeJoins(bands, ends);
  auto pushOf = [](const WallBand& b) {
    const double foot = b.start.Z(), push = std::get<ExtrudeOp>(b.recipe.modifiers.front()).distance;
    return std::make_pair(std::min(foot, foot + push), std::max(foot, foot + push));
  };
  struct Plan {
    ObjectId id;
    std::vector<TopoDS_Shape> fills, trims;
    std::string made;
  };
  std::vector<Plan> plans;
  int mitred = 0;
  for (std::size_t k = 0; k < bands.size(); ++k) {
    const WallBand& w = bands[k];
    const bool flagged = w.object->attributes.count(kMitre) > 0;
    if (!flagged && !w.object->attributes.count(kMitreMade)) continue;
    Plan plan{w.object->id, {}, {}, {}};
    // The wall's own push: from its foot (its present top may be cut by a roof).
    const double foot = w.start.Z();
    const double push = std::get<ExtrudeOp>(w.recipe.modifiers.front()).distance;
    const double z0 = std::min(foot, foot + push), z1 = std::max(foot, foot + push);
    const gp_Trsf toLocal = w.toWorld.Inverted();
    for (int end = 0; end < 2 && flagged; ++end) {
      const auto partners = cornerPartners(bands, ends, k, end);
      if (partners.size() != 1 || !bands[partners.front()].object->attributes.count(kMitre)) continue;
      const auto pieces = mitreAt(w, end, bands[partners.front()]);
      if (!pieces) continue;
      const TopoDS_Shape fill = planPrism(pieces->fill, z0, z1, toLocal);
      const TopoDS_Shape trim = planPrism(pieces->trim, z0 - 50.0, z1 + 50.0, toLocal);
      if (fill.IsNull() || trim.IsNull()) continue;
      plan.fills.push_back(fill);
      plan.trims.push_back(trim);
      char text[200];
      std::snprintf(text, sizeof text, "%d:%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f;", end, pieces->fill[0].X(), pieces->fill[0].Y(),
                    pieces->fill[1].X(), pieces->fill[1].Y(), pieces->fill[2].X(), pieces->fill[2].Y(), z0, z1);
      plan.made += text;
      ++mitred;
    }
    // T joints (docs/177): the partition's core on into the wall, the wall's layers there taken
    // away, both only as high as the two walls stand together.
    for (const Tee& t : tees) {
      if (!flagged || (t.p != k && t.e != k)) continue;
      const auto [pz0, pz1] = pushOf(bands[t.p]);
      const auto [ez0, ez1] = pushOf(bands[t.e]);
      const double lo = std::max(pz0, ez0), hi = std::min(pz1, ez1);
      if (hi - lo < 1.0) continue;
      const bool partition = t.p == k;
      const std::vector<gp_Pnt>& polygon = partition ? t.pieces.fill : t.pieces.trim;
      // The wall's trim reaches past its own foot and top where the partition stands as high.
      const double b0 = partition || lo > ez0 + 1e-6 ? lo : lo - 50.0, b1 = partition || hi < ez1 - 1e-6 ? hi : hi + 50.0;
      const TopoDS_Shape shape = planPrism(polygon, b0, b1, toLocal);
      if (shape.IsNull()) continue;
      (partition ? plan.fills : plan.trims).push_back(shape);
      char text[200];
      std::snprintf(text, sizeof text, "T%c%d:%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f;", partition ? 'p' : 'e', t.end, polygon[0].X(),
                    polygon[0].Y(), polygon[1].X(), polygon[1].Y(), polygon[2].X(), polygon[2].Y(), b0, b1);
      plan.made += text;
    }
    if (plan.made == attributeOf(*w.object, kMitreMade)) continue;  // as it is
    plans.push_back(std::move(plan));
  }
  for (const Plan& plan : plans) {
    const Object* now = changes.pending(plan.id);
    Object wall = now ? *now : *document.find(plan.id);
    if (wall.locked) continue;
    const auto band = wallBand(document, wall);
    if (!band) continue;
    setCornerPieces(wall, *band, plan.fills, plan.trims);
    if (plan.made.empty()) wall.attributes.erase(kMitreMade);
    else wall.attributes[kMitreMade] = plan.made;
    wall.hiddenEdges.clear();
    if (!rebuild(wall, context) || wall.shape.IsNull() || !TopExp_Explorer(wall.shape, TopAbs_SOLID).More()) continue;
    changes.modify(wall);
  }
  return mitred;
}

// --- walls up to a roof (docs/97 N-05) -----------------------------------------------------
constexpr const char* kTopRoof = "top_roof";            // on a wall: the roof it stands up to
constexpr const char* kRoofTrim = "roof-trim";          // id of the operand that takes the wall's top away
constexpr const char* kHeightBefore = "roof_trim_height";  // the wall's own height before it was fitted

// The operand that trims the wall, down the chain of cuts: its path and the frame its
// siblings are given in (that frame -> the object's own coordinates).
bool findRoofTrim(const Recipe& recipe, std::vector<std::size_t>& path, gp_Trsf& frame) {
  const auto* cut = std::get_if<BooleanFeature>(&recipe.base);
  if (!cut || cut->kind != BooleanKind::Subtract || cut->operands.empty() || !recipe.modifiers.empty()) return false;
  for (std::size_t i = 1; i < cut->operands.size(); ++i) {
    if (cut->operands[i].id != kRoofTrim) continue;
    path.push_back(i);
    return true;
  }
  const BooleanOperand& base = cut->operands.front();
  if (!base.recipe) return false;
  const gp_Trsf outer = frame;
  path.push_back(0);
  frame = frame * base.placement;
  if (findRoofTrim(*base.recipe, path, frame)) return true;
  path.pop_back();
  frame = outer;
  return false;
}

// What the roof takes from walls under it (world), and its highest point.
struct RoofCut {
  TopoDS_Shape above;
  double top = 0.0;
};

Result<RoofCut> roofCut(const Document& document, const Object* roof) {
  using R = Result<RoofCut>;
  const auto stored = storedRoofOf(document, roof);
  if (!stored) return R::failure(stored.error());
  const geom::RoofSpec world = roofInFrame(stored->first, stored->second);
  const auto shape = geom::makeRoof(world);
  if (!shape) return R::failure(shape.error());
  const auto above = geom::roofSpaceAbove(world, 100000.0);
  if (!above) return R::failure(above.error());
  return RoofCut{above.value(), shape->ridgeHeight};
}

// `o` (a wall, possibly a changed copy) made tall enough and cut to the roof. False when it
// is not a wall its tool made any more, or nothing of it would be left.
bool fitWallToRoof(const Document& document, Object& o, const RoofCut& cut, const RegenContext& context) {
  const auto band = wallBand(document, o);
  if (!band) return false;
  // Tall enough to reach the roof everywhere (its own height is remembered once).
  Recipe recipe = band->recipe;
  auto& push = std::get<ExtrudeOp>(recipe.modifiers.front());
  const double own = std::abs(push.distance);
  if (!o.attributes.count(kHeightBefore)) {
    char text[32];
    std::snprintf(text, sizeof text, "%.9g", own);
    o.attributes[kHeightBefore] = text;
  }
  // The wall's foot: from its own recipe (its present top may already be cut).
  const gp_Pnt foot = std::get<RectangleProfile>(band->recipe.base).plane.origin.Transformed(band->toWorld);
  const double needed = cut.top - foot.Z();
  if (needed > own) push.distance = std::copysign(needed, push.distance);
  if (band->path.empty()) o.recipe = std::move(recipe);
  else replaceOperand(o.recipe, band->path, 0, std::move(recipe));
  // The cut, in the frame its fellow operands are in.
  const gp_Trsf world = document.worldTransform(o);
  std::vector<std::size_t> path;
  gp_Trsf frame;
  const bool has = findRoofTrim(o.recipe, path, frame);
  const gp_Trsf toLocal = (world * frame).Inverted();
  Recipe trim;
  trim.base = ImportedShape{BRepBuilderAPI_Transform(cut.above, toLocal, Standard_True).Shape(), kRoofTrim};
  if (has) {
    writableOperand(o.recipe, path).recipe = std::make_shared<const Recipe>(std::move(trim));
  } else {
    BooleanFeature feature;
    feature.kind = BooleanKind::Subtract;
    feature.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(o.recipe), gp_Trsf(), o.name, {}});
    feature.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(std::move(trim)), gp_Trsf(), "Roof", kRoofTrim});
    o.recipe = Recipe{};
    o.recipe.base = std::move(feature);
  }
  o.faceMaterials.clear();
  o.hiddenEdges.clear();
  if (!rebuild(o, context) || o.shape.IsNull()) return false;
  return TopExp_Explorer(o.shape, TopAbs_SOLID).More();
}

void refitWallsToRoofs(const Document& document, ChangeSet& changes, const RegenContext& context) {
  std::map<ObjectId, std::optional<RoofCut>> cuts;
  std::vector<ObjectId> walls;
  for (const Object& o : document.objects())
    if (o.attributes.count(kTopRoof) && !changes.erased(o.id)) walls.push_back(o.id);
  for (const ObjectId& id : walls) {
    const Object* was = document.find(id);
    const ObjectId roofId = was->attributes.at(kTopRoof);
    const Object* wallNow = changes.pending(id);
    const Object* roofNow = changes.pending(roofId);
    if (!wallNow && !roofNow) continue;  // neither changed
    if (changes.erased(roofId)) continue;  // the roof is gone: the wall keeps its shape
    const Object* roof = roofNow ? roofNow : document.find(roofId);
    if (!roof) continue;
    if (!cuts.count(roofId)) {
      const auto cut = roofCut(document, roof);
      cuts[roofId] = cut ? std::optional<RoofCut>(cut.value()) : std::nullopt;
    }
    if (!cuts[roofId]) continue;  // a roof reshaped by hand: the walls stay as they are
    Object wall = wallNow ? *wallNow : *was;
    if (wall.locked) continue;
    if (fitWallToRoof(document, wall, *cuts[roofId], context)) changes.modify(wall);
  }
  // Mitred corners (docs/159) after the same changes; after the roofs, which set the walls' heights.
  refitWallCorners(document, changes, context);
}

geom::RoofSpec roofInFrame(geom::RoofSpec s, const gp_Trsf& t) {
  for (gp_Pnt& p : s.outline) {
    p.Transform(t);
    p.SetZ(0);
  }
  s.height += t.TranslationPart().Z();
  const gp_Vec fall = gp_Vec(s.fallX, s.fallY, 0).Transformed(t);
  s.fallX = fall.X();
  s.fallY = fall.Y();
  return s;
}
}  // namespace

Result<ObjectId> Modeler::createRoof(const geom::RoofSpec& spec) {
  using R = Result<ObjectId>;
  const auto roof = geom::makeRoof(spec);
  if (!roof) return R::failure(roof.error());
  Object o;
  o.name = uniqueName("Roof");
  o.recipe.base = ImportedShape{roof->solid, "roof"};
  o.attributes["kind"] = "roof";
  o.attributes["roof_spec"] = roofSpecText(spec);
  if (const FloorLevel* level = levelAtHeight(document_.settings().levels, spec.height))
    o.attributes[kTopLevel] = level->id;
  return addObject(std::move(o), "Roof");
}

Result<RoofInfo> Modeler::roofInfo(const ObjectId& id) const {
  using R = Result<RoofInfo>;
  const auto stored = storedRoof(document_, id);
  if (!stored) return R::failure(stored.error());
  const auto roof = geom::makeRoof(stored->first);
  if (!roof) return R::failure(roof.error());
  RoofInfo info;
  info.spec = roofInFrame(stored->first, stored->second);
  info.planArea = roof->planArea;
  info.slopeArea = roof->slopeArea;
  const double dz = stored->second.TranslationPart().Z();
  info.ridgeHeight = roof->ridgeHeight + dz;
  info.eaveHeight = roof->eaveHeight + dz;
  info.parts = roof->parts;
  info.level = attributeText(*document_.find(id), kTopLevel);
  return info;
}

Status Modeler::changeRoof(const ObjectId& id, const geom::RoofSpec& spec) {
  const auto stored = storedRoof(document_, id);
  if (!stored) return stored.status();
  const Object* current = document_.find(id);
  if (current->locked) return Status::failure("The object is locked");
  // Kept in the object's own coordinates: the roof stays where it was moved to.
  const geom::RoofSpec local = roofInFrame(spec, stored->second.Inverted());
  const auto roof = geom::makeRoof(local);
  if (!roof) return roof.status();
  Object object = *current;
  object.recipe.base = ImportedShape{roof->solid, "roof"};
  object.attributes["roof_spec"] = roofSpecText(local);
  object.faceMaterials.clear();
  object.hiddenEdges.clear();
  // A bearing height typed in: the roof follows the level at that height, or none.
  if (std::abs(spec.height - roofInFrame(stored->first, stored->second).height) > 1e-6) {
    if (const FloorLevel* level = levelAtHeight(document_.settings().levels, spec.height)) object.attributes[kTopLevel] = level->id;
    else object.attributes.erase(kTopLevel);
  }
  if (const Status built = rebuild(object, document_.regenContext()); !built) return built;
  // The walls standing up to it follow, in the same step.
  ChangeSet changes(document_);
  changes.modify(object);
  refitWallsToRoofs(document_, changes, document_.regenContext());
  history_.execute(changes.build("Change Roof"));
  return Status::success();
}

Result<int> Modeler::mitreWallCorners(const std::vector<ObjectId>& walls, bool on) {
  using R = Result<int>;
  std::vector<ObjectId> ids = walls;
  if (ids.empty())
    for (const Object& o : document_.objects())
      if (wallBand(document_, o) && !o.locked) ids.push_back(o.id);
  if (ids.empty()) return R::failure("There are no walls");
  ChangeSet changes(document_);
  for (const ObjectId& id : ids) {
    const Object* current = document_.find(id);
    if (!current) return R::failure("Object not found");
    if (!wallBand(document_, *current)) return R::failure("'" + current->name + "' is not a wall as its tool made it");
    if (current->locked) return R::failure("'" + current->name + "' is locked");
    if ((current->attributes.count(kMitre) > 0) == on) continue;
    Object wall = *current;
    if (on) wall.attributes[kMitre] = "1";
    else wall.attributes.erase(kMitre);
    changes.modify(wall);
  }
  const int ends = refitWallCorners(document_, changes, document_.regenContext());
  history_.execute(changes.build(on ? "Mitre Wall Corners" : "Square Wall Corners"));
  return ends / 2;
}

Status Modeler::fitWallsToRoof(const std::vector<ObjectId>& walls, const ObjectId& roofId) {
  if (walls.empty()) return Status::failure("Choose the walls to fit to the roof");
  const auto cut = roofCut(document_, document_.find(roofId));
  if (!cut) return cut.status();
  ChangeSet changes(document_);
  for (const ObjectId& id : walls) {
    const Object* current = document_.find(id);
    if (!current) return Status::failure("Object not found");
    if (current->locked) return Status::failure("'" + current->name + "' is locked");
    Object wall = *current;
    wall.attributes[kTopRoof] = roofId;
    wall.attributes.erase(kTopLevel);  // its top is the roof's now
    if (!fitWallToRoof(document_, wall, cut.value(), document_.regenContext()))
      return Status::failure("'" + current->name + "' cannot be fitted to the roof: it is no longer the wall its tool made, "
                             "or all of it is above the roof (nothing was changed)");
    changes.modify(wall);
  }
  history_.execute(changes.build("Fit Walls To Roof"));
  return Status::success();
}

// --- stairs (docs/90 G-09, docs/103) --------------------------------------------------------
namespace {
constexpr const char* kStairHeadroom = "opening_headroom";

std::string stairSpecText(const geom::StairSpec& s) {
  nlohmann::json path = nlohmann::json::array();
  for (const gp_Pnt& p : s.path) path.push_back({p.X(), p.Y()});
  return nlohmann::json{{"path", path}, {"base", s.base}, {"rise", s.rise}, {"width", s.width}, {"risers", s.risers},
                        {"riser_limit", s.riserLimit}, {"turns", s.turns}, {"style", s.style}, {"thickness", s.thickness},
                        {"handrail", s.handrail}, {"handrail_height", s.handrailHeight},
                        {"stringer", s.stringer}, {"stringer_thickness", s.stringerThickness},
                        {"stringer_style", s.stringerStyle}, {"riser_boards", s.riserBoards}, {"riser_thickness", s.riserThickness},
                        {"newels", s.newels}, {"newel_size", s.newelSize}, {"nosing", s.nosing},
                        {"shape", s.shape}, {"sweep", s.sweep}}
      .dump();
}

std::optional<geom::StairSpec> stairSpecFrom(const std::string& text) {
  try {
    const nlohmann::json j = nlohmann::json::parse(text);
    geom::StairSpec s;
    for (const auto& p : j.at("path")) s.path.emplace_back(p.at(0).get<double>(), p.at(1).get<double>(), 0.0);
    s.base = j.at("base").get<double>();
    s.rise = j.at("rise").get<double>();
    s.width = j.at("width").get<double>();
    s.risers = j.at("risers").get<int>();
    s.riserLimit = j.at("riser_limit").get<double>();
    s.turns = j.at("turns").get<std::vector<int>>();
    s.style = j.at("style").get<std::string>();
    s.handrail = j.value("handrail", std::string("none"));  // stairs made before docs/116 have none
    s.handrailHeight = j.value("handrail_height", 850.0);
    s.stringer = j.value("stringer", std::string("none"));  // stairs made before docs/128 have none
    s.stringerThickness = j.value("stringer_thickness", 36.0);
    s.stringerStyle = j.value("stringer_style", std::string("side"));  // stairs made before docs/135
    s.riserBoards = j.value("riser_boards", false);
    s.riserThickness = j.value("riser_thickness", 15.0);
    s.newels = j.value("newels", false);
    s.newelSize = j.value("newel_size", 105.0);
    s.nosing = j.value("nosing", 0.0);  // stairs made before docs/137
    s.shape = j.value("shape", std::string("line"));  // before docs/139
    s.sweep = j.value("sweep", 270.0);
    s.thickness = j.at("thickness").get<double>();
    return s;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// The stored stair (in the object's own coordinates) and where the object stands, or why its
// sizes can no longer be read. `o` may be a changed copy not in the document yet.
Result<std::pair<geom::StairSpec, gp_Trsf>> storedStairOf(const Document& document, const Object* o) {
  using R = Result<std::pair<geom::StairSpec, gp_Trsf>>;
  if (!o) return R::failure("Object not found");
  const auto kind = o->attributes.find("kind");
  const auto text = o->attributes.find("stair_spec");
  if (kind == o->attributes.end() || kind->second != "stair" || text == o->attributes.end())
    return R::failure("'" + o->name + "' is not a stair made by the stair tool");
  if (!std::holds_alternative<ImportedShape>(o->recipe.base) || !o->recipe.modifiers.empty())
    return R::failure("'" + o->name + "' was reshaped by hand: its stair sizes can no longer be read or changed");
  const auto spec = stairSpecFrom(text->second);
  if (!spec) return R::failure("'" + o->name + "': its stair sizes cannot be read");
  const gp_Trsf world = document.worldTransform(*o);
  if (std::abs(world.ScaleFactor() - 1.0) > 1e-9 || gp_Vec(0, 0, 1).Transformed(world).Z() < 1.0 - 1e-9)
    return R::failure("'" + o->name + "' was tilted or scaled: its stair sizes can no longer be read or changed");
  return std::make_pair(*spec, world);
}

geom::StairSpec stairInFrame(geom::StairSpec s, const gp_Trsf& t) {
  for (gp_Pnt& p : s.path) {
    p.Transform(t);
    p.SetZ(0);
  }
  s.base += t.TranslationPart().Z();
  return s;
}

// World z of a slab's underside and top, from its own outline and push.
bool slabLevels(const Document& document, const Object& slab, double& bottom, double& top) {
  const Recipe* core = slabCore(slab.recipe);
  const auto* outline = core ? std::get_if<PolygonProfile>(&core->base) : nullptr;
  if (!outline || outline->points.empty() || core->modifiers.empty() || !std::holds_alternative<ExtrudeOp>(core->modifiers.front()))
    return false;
  const double z = outline->points.front().Transformed(document.worldTransform(slab)).Z();
  const double push = std::get<ExtrudeOp>(core->modifiers.front()).distance;
  bottom = std::min(z, z + push);
  top = std::max(z, z + push);
  return true;
}

// What opens `slab` for `stair` (both may be changed copies), in the slab's own coordinates;
// none when the stair's sizes cannot be read or it needs nothing of this floor.
std::optional<Recipe> stairHole(const Document& document, const Object& slab, const Object& stair) {
  const auto stored = storedStairOf(document, &stair);
  double bottom = 0, top = 0;
  if (!stored || !slabLevels(document, slab, bottom, top)) return std::nullopt;
  double headroom = 2000.0;
  if (const auto it = stair.attributes.find(kStairHeadroom); it != stair.attributes.end()) headroom = std::atof(it->second.c_str());
  const auto tool = geom::stairOpening(stairInFrame(stored->first, stored->second), bottom, top, headroom);
  if (!tool) return std::nullopt;
  Recipe hole;
  hole.base = ImportedShape{BRepBuilderAPI_Transform(tool.value(), document.worldTransform(slab).Inverted(), Standard_True).Shape(),
                            "stair-opening"};
  return hole;
}

// `slab` with its opening for the stair set to `hole`, or taken away. False for a slab that
// is no longer the slab its tool made.
bool setStairHole(Object& slab, const ObjectId& stairId, std::optional<Recipe> hole) {
  const Recipe* core = slabCore(slab.recipe);
  if (!core) return false;
  const std::string id = kStairHole + stairId;
  if (core == &slab.recipe) {
    if (!hole) return true;
    BooleanFeature feature;
    feature.kind = BooleanKind::Subtract;
    feature.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(slab.recipe), gp_Trsf(), slab.name, {}});
    feature.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(std::move(*hole)), gp_Trsf(), "Stair", id});
    slab.recipe = Recipe{};
    slab.recipe.base = std::move(feature);
  } else {
    auto& cut = std::get<BooleanFeature>(slab.recipe.base);
    const auto it = std::find_if(cut.operands.begin() + 1, cut.operands.end(), [&](const BooleanOperand& op) { return op.id == id; });
    if (hole) {
      if (it != cut.operands.end()) it->recipe = std::make_shared<const Recipe>(std::move(*hole));
      else cut.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(std::move(*hole)), gp_Trsf(), "Stair", id});
    } else if (it != cut.operands.end()) {
      cut.operands.erase(it);
      if (cut.operands.size() == 1) {
        const Recipe own = *cut.operands.front().recipe;
        slab.recipe = own;
      }
    }
  }
  slab.faceMaterials.clear();
  slab.hiddenEdges.clear();
  return true;
}

// The stairs a slab is opened for.
std::vector<ObjectId> stairsOpening(const Object& slab) {
  std::vector<ObjectId> out;
  const auto* cut = std::get_if<BooleanFeature>(&slab.recipe.base);
  if (!cut || !slabCore(slab.recipe)) return out;
  for (std::size_t i = 1; i < cut->operands.size(); ++i) out.push_back(cut->operands[i].id.substr(std::strlen(kStairHole)));
  return out;
}

void refitStairOpenings(const Document& document, ChangeSet& changes, const RegenContext& context) {
  std::vector<ObjectId> slabs;
  for (const Object& o : document.objects()) {
    if (attributeText(o, "kind") != "slab" || changes.erased(o.id)) continue;
    const Object* now = changes.pending(o.id);
    if (!stairsOpening(now ? *now : o).empty()) slabs.push_back(o.id);
  }
  for (const ObjectId& id : slabs) {
    const Object* was = document.find(id);
    const Object* pending = changes.pending(id);
    Object slab = pending ? *pending : *was;
    if (slab.locked) continue;
    bool any = false;
    for (const ObjectId& stairId : stairsOpening(slab)) {
      if (changes.erased(stairId)) {
        any |= setStairHole(slab, stairId, std::nullopt);  // the stair is gone: the floor closes
        continue;
      }
      const Object* stairNow = changes.pending(stairId);
      if (!pending && !stairNow) continue;  // neither changed
      const Object* stair = stairNow ? stairNow : document.find(stairId);
      if (!stair || !storedStairOf(document, stair)) continue;  // reshaped by hand: the opening stays
      any |= setStairHole(slab, stairId, stairHole(document, slab, *stair));
    }
    if (any && rebuild(slab, context) && !slab.shape.IsNull()) changes.modify(slab);
  }
}

bool shiftStair(const Document& document, const Object& was, Object& o, double dBase, double dTop,
                const RegenContext& context) {
  if (std::abs(dBase) > 1e-9) {
    gp_Trsf t;
    t.SetTranslation(gp_Vec(0, 0, dBase).Transformed(document.parentTransform(was).Inverted()));
    o.transform = t * o.transform;
  }
  const double grow = dTop - dBase;
  if (std::abs(grow) < 1e-9) return true;
  // A stair reshaped by hand, or one that cannot be built to the new height, only moves.
  const auto stored = storedStairOf(document, &was);
  if (!stored) return true;
  geom::StairSpec local = stored->first;
  local.rise += grow;
  const auto stair = geom::makeStair(local);
  if (!stair) return true;
  o.recipe.base = ImportedShape{stair->solid, "stair"};
  o.attributes["stair_spec"] = stairSpecText(local);
  o.faceMaterials.clear();
  o.hiddenEdges.clear();
  return static_cast<bool>(rebuild(o, context));
}
}  // namespace

std::optional<geom::StairSpec> stairInWorld(const Document& document, const Object& object) {
  const auto stored = storedStairOf(document, &object);
  if (!stored) return std::nullopt;
  return stairInFrame(stored->first, stored->second);
}

Result<ObjectId> Modeler::createStair(const geom::StairSpec& spec) {
  using R = Result<ObjectId>;
  const auto stair = geom::makeStair(spec);
  if (!stair) return R::failure(stair.error());
  Object o;
  o.name = uniqueName("Stair");
  o.recipe.base = ImportedShape{stair->solid, "stair"};
  o.attributes["kind"] = "stair";
  o.attributes["stair_spec"] = stairSpecText(spec);
  const auto& levels = document_.settings().levels;
  if (const FloorLevel* level = levelAtHeight(levels, spec.base)) o.attributes[kBaseLevel] = level->id;
  if (const FloorLevel* level = levelAtHeight(levels, spec.base + spec.rise)) o.attributes[kTopLevel] = level->id;
  return addObject(std::move(o), "Stair");
}

Result<StairInfo> Modeler::stairInfo(const ObjectId& id) const {
  using R = Result<StairInfo>;
  const auto stored = storedStairOf(document_, document_.find(id));
  if (!stored) return R::failure(stored.error());
  const auto stair = geom::makeStair(stored->first);
  if (!stair) return R::failure(stair.error());
  StairInfo info;
  info.spec = stairInFrame(stored->first, stored->second);
  info.risers = stair->risers;
  info.riser = stair->riser;
  info.treads = static_cast<int>(stair->treads.size());
  info.goingMin = stair->goingMin;
  info.goingMax = stair->goingMax;
  info.winderGoing = stair->winderGoing;
  info.walkLength = stair->walkLength;
  info.flights = stair->flights;
  info.landings = stair->landings;
  info.winders = stair->winders;
  info.checks = stair->checks;
  const Object& o = *document_.find(id);
  info.baseLevel = attributeText(o, kBaseLevel);
  info.topLevel = attributeText(o, kTopLevel);
  for (const Object& slab : document_.objects()) {
    if (attributeText(slab, "kind") != "slab") continue;
    const auto stairs = stairsOpening(slab);
    if (std::find(stairs.begin(), stairs.end(), id) != stairs.end()) info.openedSlabs.push_back(slab.id);
  }
  return info;
}

Status Modeler::changeStair(const ObjectId& id, const geom::StairSpec& spec) {
  const auto stored = storedStairOf(document_, document_.find(id));
  if (!stored) return stored.status();
  const Object* current = document_.find(id);
  if (current->locked) return Status::failure("The object is locked");
  // Kept in the object's own coordinates: the stair stays where it was moved to.
  const geom::StairSpec local = stairInFrame(spec, stored->second.Inverted());
  const auto stair = geom::makeStair(local);
  if (!stair) return stair.status();
  Object object = *current;
  object.recipe.base = ImportedShape{stair->solid, "stair"};
  object.attributes["stair_spec"] = stairSpecText(local);
  object.faceMaterials.clear();
  object.hiddenEdges.clear();
  // A foot or a top typed in: it follows the level at that height, or none.
  const geom::StairSpec before = stairInFrame(stored->first, stored->second);
  const auto& levels = document_.settings().levels;
  auto follow = [&](const char* key, double was, double now) {
    if (std::abs(was - now) < 1e-6) return;
    if (const FloorLevel* level = levelAtHeight(levels, now)) object.attributes[key] = level->id;
    else object.attributes.erase(key);
  };
  follow(kBaseLevel, before.base, spec.base);
  follow(kTopLevel, before.base + before.rise, spec.base + spec.rise);
  if (const Status built = rebuild(object, document_.regenContext()); !built) return built;
  ChangeSet changes(document_);
  changes.modify(object);
  refitStairOpenings(document_, changes, document_.regenContext());
  history_.execute(changes.build("Change Stair"));
  return Status::success();
}

Result<std::vector<ObjectId>> Modeler::openFloorForStair(const ObjectId& stairId, const std::vector<ObjectId>& slabs,
                                                         double headroom) {
  using R = Result<std::vector<ObjectId>>;
  const auto stored = storedStairOf(document_, document_.find(stairId));
  if (!stored) return R::failure(stored.error());
  if (!(headroom >= 0.0) || !std::isfinite(headroom)) return R::failure("The headroom is 0 or more");
  Object stair = *document_.find(stairId);
  char text[32];
  std::snprintf(text, sizeof text, "%.9g", headroom);
  stair.attributes[kStairHeadroom] = text;
  const geom::StairSpec world = stairInFrame(stored->first, stored->second);
  const double arrives = world.base + world.rise;
  std::vector<ObjectId> candidates = slabs;
  if (slabs.empty()) {
    for (const Object& o : document_.objects()) {
      double bottom = 0, top = 0;
      if (attributeText(o, "kind") != "slab" || o.locked || !slabLevels(document_, o, bottom, top)) continue;
      if (arrives > bottom + 1.0 && arrives < top + 1.0) candidates.push_back(o.id);
    }
  }
  ChangeSet changes(document_);
  changes.modify(stair);
  std::vector<ObjectId> opened;
  for (const ObjectId& id : candidates) {
    const Object* current = document_.find(id);
    if (!current) return R::failure("Object not found");
    const bool named = !slabs.empty();
    if (attributeText(*current, "kind") != "slab" || !slabCore(current->recipe)) {
      if (named) return R::failure("'" + current->name + "' is not a floor made by the slab tool (nothing was changed)");
      continue;
    }
    if (current->locked) {
      if (named) return R::failure("'" + current->name + "' is locked");
      continue;
    }
    Object slab = *current;
    auto hole = stairHole(document_, slab, stair);
    const double before = geom::volume(slab.shape);
    if (!hole || !setStairHole(slab, stairId, std::move(hole)) || !rebuild(slab, document_.regenContext()) || slab.shape.IsNull() ||
        geom::volume(slab.shape) > before - 1.0) {
      const auto already = stairsOpening(*current);
      if (std::find(already.begin(), already.end(), stairId) != already.end()) {
        opened.push_back(id);  // open already and nothing to add
        continue;
      }
      if (named) return R::failure("'" + current->name + "' does not lie over the stair where it needs headroom (nothing was changed)");
      continue;
    }
    changes.modify(slab);
    opened.push_back(id);
  }
  if (opened.empty()) return R::failure("No floor made by the slab tool lies over the top of this stair");
  history_.execute(changes.build("Open Floor For Stair"));
  return opened;
}

Status Modeler::closeFloorForStair(const ObjectId& stairId) {
  ChangeSet changes(document_);
  bool any = false;
  for (const Object& o : document_.objects()) {
    if (attributeText(o, "kind") != "slab") continue;
    const auto stairs = stairsOpening(o);
    if (std::find(stairs.begin(), stairs.end(), stairId) == stairs.end()) continue;
    if (o.locked) return Status::failure("'" + o.name + "' is locked");
    Object slab = o;
    if (!setStairHole(slab, stairId, std::nullopt)) continue;
    if (const Status built = rebuild(slab, document_.regenContext()); !built) return built;
    changes.modify(slab);
    any = true;
  }
  if (!any) return Status::failure("No floor is opened for this stair");
  history_.execute(changes.build("Close Floor For Stair"));
  return Status::success();
}

Status Modeler::releaseWallsFromRoof(const std::vector<ObjectId>& walls) {
  if (walls.empty()) return Status::failure("Choose the walls to release");
  ChangeSet changes(document_);
  for (const ObjectId& id : walls) {
    const Object* current = document_.find(id);
    if (!current) return Status::failure("Object not found");
    if (current->locked) return Status::failure("'" + current->name + "' is locked");
    if (!current->attributes.count(kTopRoof)) return Status::failure("'" + current->name + "' is not fitted to a roof");
    Object wall = *current;
    std::vector<std::size_t> path;
    gp_Trsf frame;
    if (findRoofTrim(wall.recipe, path, frame)) {
      eraseOperand(wall.recipe, path);
      gp_Trsf placement = wall.transform;
      collapseSubtracts(wall.recipe, placement);
      wall.transform = placement;
    }
    // Its own height again.
    const auto band = wallBand(document_, wall);
    const auto before = wall.attributes.find(kHeightBefore);
    if (band && before != wall.attributes.end()) {
      Recipe recipe = band->recipe;
      auto& push = std::get<ExtrudeOp>(recipe.modifiers.front());
      try {
        push.distance = std::copysign(std::stod(before->second), push.distance);
      } catch (const std::exception&) {
      }
      if (band->path.empty()) wall.recipe = std::move(recipe);
      else replaceOperand(wall.recipe, band->path, 0, std::move(recipe));
    }
    wall.attributes.erase(kTopRoof);
    wall.attributes.erase(kHeightBefore);
    wall.faceMaterials.clear();
    wall.hiddenEdges.clear();
    if (const Status built = rebuild(wall, document_.regenContext()); !built)
      return Status::failure("'" + current->name + "': " + built.message());
    changes.modify(wall);
  }
  history_.execute(changes.build("Release Walls From Roof"));
  return Status::success();
}

std::vector<ObjectId> Modeler::wallsFittedToRoof(const ObjectId& roof) const {
  std::vector<ObjectId> out;
  for (const Object& o : document_.objects()) {
    const auto it = o.attributes.find(kTopRoof);
    if (it != o.attributes.end() && it->second == roof) out.push_back(o.id);
  }
  return out;
}

std::vector<ObjectId> Modeler::wallsUnderRoof(const ObjectId& roofId) const {
  std::vector<ObjectId> out;
  const auto stored = storedRoof(document_, roofId);
  if (!stored) return out;
  const geom::RoofSpec roof = roofInFrame(stored->first, stored->second);
  // Within the outline grown a little (walls stand on its lines).
  auto within = [&](const gp_Pnt& p) {
    bool in = false;
    double nearest = 1e300;
    const auto& poly = roof.outline;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
      const gp_Pnt& a = poly[i];
      const gp_Pnt& b = poly[j];
      if ((a.Y() > p.Y()) != (b.Y() > p.Y()) && p.X() < (b.X() - a.X()) * (p.Y() - a.Y()) / (b.Y() - a.Y()) + a.X()) in = !in;
      const gp_Vec ab(a, b), ap(a, gp_Pnt(p.X(), p.Y(), 0));
      const double t = ab.SquareMagnitude() < 1e-12 ? 0.0 : std::clamp(ap.Dot(ab) / ab.SquareMagnitude(), 0.0, 1.0);
      nearest = std::min(nearest, gp_Pnt(p.X(), p.Y(), 0).Distance(a.Translated(ab * t)));
    }
    return in || nearest < 300.0;
  };
  // The storey under the roof (docs/158): the highest level below its bearing height. Walls of
  // the storeys further down are not "under" it (fitted, they would stand up through the storeys
  // between), unless they reach the bearing height themselves (walls through several storeys).
  std::optional<double> storey;
  for (const FloorLevel& l : document_.settings().levels)
    if (l.elevation < roof.height - 1.0 && (!storey || l.elevation > *storey)) storey = l.elevation;
  for (const Object& o : document_.objects()) {
    const auto band = wallBand(document_, o);
    if (!band) continue;
    const gp_Pnt foot = std::get<RectangleProfile>(band->recipe.base).plane.origin.Transformed(band->toWorld);
    if (foot.Z() >= roof.height - 1.0) continue;  // stands on or above the bearing height: another storey
    if (storey && foot.Z() < *storey - 1.0 && band->z1 < roof.height - 1.0) continue;  // a storey further down
    const gp_Pnt mid = band->start.Translated(gp_Vec(band->dir) * (band->length / 2));
    if (within(mid)) out.push_back(o.id);
  }
  return out;
}

std::vector<int> Modeler::roofSidesAtWalls(const geom::RoofSpec& roof) const {
  // The outline without repeated points, as the roof numbers its sides.
  std::vector<gp_Pnt> points;
  for (const gp_Pnt& p : roof.outline) {
    const gp_Pnt flat(p.X(), p.Y(), 0);
    if (points.empty() || points.back().Distance(flat) > 0.01) points.push_back(flat);
  }
  if (points.size() > 1 && points.front().Distance(points.back()) <= 0.01) points.pop_back();
  std::vector<int> out;
  const std::size_t n = points.size();
  if (n < 3) return out;
  // Walls that stand well above the roof's bearing height and start no more than a metre above it.
  std::vector<WallBand> walls;
  for (const Object& o : document_.objects())
    if (const auto band = wallBand(document_, o); band && band->z1 > roof.height + 500.0 && band->z0 < roof.height + 1000.0) walls.push_back(*band);
  for (std::size_t k = 0; k < n; ++k) {
    const gp_Pnt &a = points[k], &b = points[(k + 1) % n];
    const double length = a.Distance(b);
    if (length < 1.0) continue;
    // The parts of the side that lie along a wall, within its thickness.
    std::vector<std::pair<double, double>> parts;
    for (const WallBand& w : walls) {
      const gp_Vec along(w.dir), across(w.left);
      const gp_Pnt start(w.start.X(), w.start.Y(), 0);
      const double ca = gp_Vec(start, a).Dot(across), cb = gp_Vec(start, b).Dot(across);
      if (std::abs(ca - cb) > 1.0 + length * 1e-3) continue;  // not along it
      if (std::min(ca, cb) < -w.right - 1.0 || std::max(ca, cb) > w.leftExt + 1.0) continue;
      double t0 = gp_Vec(start, a).Dot(along), t1 = gp_Vec(start, b).Dot(along);
      const bool back = t0 > t1;
      if (back) std::swap(t0, t1);
      // The wall's own length, and its thickness more at each end (the corner it runs into).
      const double reach = w.right + w.leftExt;
      const double from = std::max(t0, -reach), to = std::min(t1, w.length + reach);
      if (to - from < 1.0) continue;
      parts.push_back(back ? std::make_pair(t1 - to, t1 - from) : std::make_pair(from - t0, to - t0));
    }
    std::sort(parts.begin(), parts.end());
    double covered = 0.0, upTo = 0.0;
    for (const auto& [from, to] : parts) {
      if (to <= upTo) continue;
      covered += to - std::max(from, upTo);
      upTo = to;
    }
    if (covered >= 0.9 * length) out.push_back(static_cast<int>(k));
  }
  return out;
}

// --- balconies (docs/97 N-03, docs/163) ------------------------------------------------------
namespace {
struct BalconyPlace {
  gp_Pnt face;  // on the wall's face, under the middle of the balcony (z 0)
  gp_Vec u, n;  // along the wall, and out from the face
  double behind = 0.0;  // from the wall's drawn line to that face
};

Result<BalconyPlace> balconyPlace(const Document& document, const Modeler::BalconySpec& spec) {
  using R = Result<BalconyPlace>;
  const Object* host = document.find(spec.host);
  if (!host) return R::failure("Object not found");
  const auto band = wallBand(document, *host);
  if (!band) return R::failure("A balcony goes on a wall made by the wall tool");
  if (spec.along < -0.5 || spec.along > band->length + 0.5) return R::failure("The middle of a balcony is within its wall's length");
  BalconyPlace place;
  const bool left = spec.side >= 0;
  place.behind = left ? band->leftExt : band->right;
  place.u = gp_Vec(band->dir);
  place.n = gp_Vec(band->left) * (left ? 1.0 : -1.0);
  const gp_Pnt on = band->start.Translated(place.u * spec.along + place.n * place.behind);
  place.face = gp_Pnt(on.X(), on.Y(), 0.0);
  return place;
}

Status balconySizes(const Modeler::BalconySpec& s) {
  if (!(s.width >= 300.0 && s.width <= 20000.0)) return Status::failure("A balcony is 300 to 20000 wide");
  if (!(s.depth >= 300.0 && s.depth <= 5000.0)) return Status::failure("A balcony stands out 300 to 5000");
  if (!(s.thickness >= 50.0 && s.thickness <= 500.0)) return Status::failure("A balcony's floor is 50 to 500 thick");
  if (s.rail != "wall" && s.rail != "bars" && s.rail != "none") return Status::failure("A balcony's rail: wall, bars or none");
  if (s.rail != "none" && !(s.railHeight >= 300.0 && s.railHeight <= 2000.0)) return Status::failure("A balcony's rail is 300 to 2000 high");
  if (!std::isfinite(s.top) || !std::isfinite(s.along)) return Status::failure("A balcony's height and place are numbers");
  if (!s.floorArea.empty() && s.floorArea != "count" && s.floorArea != "exclude")
    return Status::failure("A balcony's floor area: count, exclude or empty (not evaluated)");
  if (s.posts != 0 && (s.posts < 2 || s.posts > 6)) return Status::failure("A balcony has no posts or 2 to 6");
  if (!s.wrap.empty() && s.wrap != "start" && s.wrap != "end") return Status::failure("A balcony goes round the corner at the wall's start or end");
  if (s.posts > 0 && !(s.postSize >= 50.0 && s.postSize <= 500.0 && s.postSize <= s.depth && s.postSize * s.posts <= s.width))
    return Status::failure("A balcony's posts are 50 to 500 square and fit under its front");
  return Status::success();
}

using Poly = std::vector<gp_Pnt>;

// A balcony's plan (docs/163, docs/168): its floor's outline, the edges of it not on a wall
// (where its rail goes), and the part of it counted in the building area. Straight on one wall,
// or round the outer corner where its wall meets another (an L).
struct BalconyPlan {
  Poly floor;                                   // world, z 0
  std::vector<std::pair<gp_Pnt, gp_Pnt>> free;  // along the floor's outline, not on a wall
  std::vector<gp_Pnt> corners;                  // the far corners (where posts go)
  Poly counted;                                 // in the building area (empty: none)
  bool wraps = false;
};

double polyArea2(const Poly& p) {
  double a = 0.0;
  for (std::size_t i = 0; i < p.size(); ++i) a += p[i].X() * p[(i + 1) % p.size()].Y() - p[(i + 1) % p.size()].X() * p[i].Y();
  return a;
}

std::optional<gp_Pnt> crossing(const gp_Pnt& a, const gp_Vec& da, const gp_Pnt& b, const gp_Vec& db) {
  const auto m = meet(a, gp_Dir(da), b, gp_Dir(db));
  if (!m) return std::nullopt;
  return a.Translated(gp_Vec(gp_Dir(da)) * m->first);
}

Result<BalconyPlan> balconyPlan(const Document& document, const Modeler::BalconySpec& s, const BalconyPlace& at) {
  using R = Result<BalconyPlan>;
  BalconyPlan plan;
  const double half = s.width / 2;
  // How far out from the faces the building area takes it: on posts to their centre line, else
  // back 1 m from its end (counted only beyond the wall's line by 1 m or more).
  const double counted = s.posts > 0 ? s.depth - s.postSize / 2 : (s.depth + at.behind >= 1000.0 ? s.depth - 1000.0 : 0.0);
  if (s.wrap.empty()) {
    const gp_Pnt a0 = at.face.Translated(at.u * -half), a1 = at.face.Translated(at.u * half);
    plan.floor = {a0, a1, a1.Translated(at.n * s.depth), a0.Translated(at.n * s.depth)};
    plan.free = {{a1, a1.Translated(at.n * s.depth)}, {a1.Translated(at.n * s.depth), a0.Translated(at.n * s.depth)}, {a0.Translated(at.n * s.depth), a0}};
    plan.corners = {a0.Translated(at.n * s.depth), a1.Translated(at.n * s.depth)};
    if (counted > 1e-6) plan.counted = {a0, a1, a1.Translated(at.n * counted), a0.Translated(at.n * counted)};
    return plan;
  }
  // Round the corner at the wall's start or end: the wall it meets there.
  if (s.wrap != "start" && s.wrap != "end") return R::failure("A balcony goes round the corner at the wall's start or end");
  const std::vector<WallBand> walls = allWallBands(document);
  std::optional<std::size_t> hi;
  for (std::size_t i = 0; i < walls.size(); ++i)
    if (walls[i].object->id == s.host) hi = i;
  if (!hi) return R::failure("A balcony goes on a wall made by the wall tool");
  const auto ends = wallEnds(walls);
  const int end = s.wrap == "start" ? 0 : 1;
  const auto partners = cornerPartners(walls, ends, *hi, end);
  if (partners.empty()) return R::failure("No wall meets this wall at that end: the balcony cannot go round the corner");
  const WallBand& w = walls[*hi];
  const WallBand& o = walls[partners.front()];
  // Its other face: the one away from this wall's body.
  const gp_Pnt middle = w.start.Translated(gp_Vec(w.dir) * (w.length / 2));
  const double side = gp_Vec(o.start, gp_Pnt(middle.X(), middle.Y(), o.start.Z())).Dot(gp_Vec(o.left));
  const gp_Vec nb = gp_Vec(o.left) * (side > 0 ? -1.0 : 1.0);
  const gp_Pnt oFace = o.start.Translated(gp_Vec(o.left) * (side > 0 ? -o.right : o.leftExt));
  const gp_Vec towards = at.u * (end == 0 ? -1.0 : 1.0);  // along this wall, to the corner
  // Round an outer corner only: the other wall runs back from the corner on this wall's inner side.
  gp_Vec db(o.dir);
  if (db.Dot(at.n) > 0) db.Reverse();
  const gp_Pnt here(at.face.X(), at.face.Y(), 0), oFlat(oFace.X(), oFace.Y(), 0);
  // The other wall's far end lies behind this face (on the building's side): an outer corner.
  const gp_Pnt oEnd0(o.start.X(), o.start.Y(), 0), oEnd1 = oEnd0.Translated(gp_Vec(o.dir) * o.length);
  const double behind0 = gp_Vec(here, oEnd0).Dot(at.n), behind1 = gp_Vec(here, oEnd1).Dot(at.n);
  if (nb.Dot(towards) <= 1e-6 || db.Dot(at.n) > -1e-6 || std::min(behind0, behind1) > -300.0)
    return R::failure("A balcony goes round an outer corner only");
  const auto corner = crossing(here, towards, oFlat, db);
  if (!corner) return R::failure("A balcony goes round an outer corner only");
  const gp_Pnt a0 = here.Translated(towards * -half);  // the far end of the leg along this wall
  if (gp_Vec(a0, *corner).Dot(towards) < 300.0) return R::failure("A balcony round a corner needs 300 or more along its wall up to the corner");
  if (!(s.wrapLength >= 300.0 && s.wrapLength <= 20000.0)) return R::failure("A balcony goes 300 to 20000 round the corner");
  auto outline = [&](double d) -> std::optional<Poly> {
    const auto outer = crossing(a0.Translated(at.n * d), towards, corner->Translated(nb * d), db);
    if (!outer) return std::nullopt;
    const gp_Pnt b1 = corner->Translated(db * s.wrapLength);
    return Poly{a0, a0.Translated(at.n * d), *outer, b1.Translated(nb * d), b1, *corner};
  };
  const auto floor = outline(s.depth);
  if (!floor) return R::failure("The balcony could not be built");
  plan.floor = *floor;
  plan.wraps = true;
  for (std::size_t k = 0; k < 4; ++k) plan.free.push_back({plan.floor[k], plan.floor[k + 1]});
  plan.corners = {plan.floor[1], plan.floor[2], plan.floor[3]};
  if (counted > 1e-6)
    if (const auto c = outline(counted)) plan.counted = *c;
  return plan;
}

// A square post of `size` at a corner of the outline, inside it (its sides along the edges there).
Poly postAt(const Poly& floor, std::size_t k, double size) {
  const std::size_t n = floor.size();
  const gp_Pnt v = floor[k];
  gp_Vec in1(v, floor[(k + n - 1) % n]), in2(v, floor[(k + 1) % n]);
  if (in1.Magnitude() < 1e-9 || in2.Magnitude() < 1e-9) return {};
  in1.Normalize();
  in2.Normalize();
  return {v, v.Translated(in2 * size), v.Translated(in1 * size + in2 * size), v.Translated(in1 * size)};
}

// The solid of a balcony round a corner (docs/168): the floor, a rail along its free edges, posts
// under its far corners.
Result<TopoDS_Shape> wrappedSolid(const BalconyPlan& plan, const Modeler::BalconySpec& s, double postBase) {
  using R = Result<TopoDS_Shape>;
  Poly floor = plan.floor;
  const bool ccw = polyArea2(floor) > 0;
  std::vector<TopoDS_Shape> parts{planPrism(floor, s.top - s.thickness, s.top, gp_Trsf())};
  // A strip inside the floor along an edge, `t` wide.
  auto strip = [&](const gp_Pnt& a, const gp_Pnt& b, double t, double z0, double z1) {
    gp_Vec d(a, b);
    if (d.Magnitude() < 1e-9) return TopoDS_Shape();
    d.Normalize();
    const gp_Vec in = ccw ? gp_Vec(-d.Y(), d.X(), 0) : gp_Vec(d.Y(), -d.X(), 0);
    return planPrism({a, b, b.Translated(in * t), a.Translated(in * t)}, z0, z1, gp_Trsf());
  };
  if (s.rail == "wall") {
    const double t = std::min(100.0, s.depth / 2);
    for (const auto& [a, b] : plan.free) parts.push_back(strip(a, b, t, s.top, s.top + s.railHeight));
  } else if (s.rail == "bars") {
    const double b = 50.0, z1 = s.top + s.railHeight;
    for (const auto& [p, q] : plan.free) {
      parts.push_back(strip(p, q, b, z1 - b, z1));
      // Posts no more than 1000 apart along it, from its start to its end.
      const double length = p.Distance(q);
      const int bays = std::max(1, static_cast<int>(std::ceil((length - b) / 1000.0 - 1e-9)));
      gp_Vec d(p, q);
      d.Normalize();
      for (int i = 0; i <= bays; ++i) {
        const gp_Pnt at = p.Translated(d * ((length - b) * i / bays));
        parts.push_back(strip(at, at.Translated(d * b), b, s.top, z1 - b));
      }
    }
  }
  if (s.posts > 0) {
    for (std::size_t k = 1; k + 2 < floor.size(); ++k) {  // the far corners (not those on the walls)
      const Poly square = postAt(floor, k, s.postSize);
      if (square.empty()) return R::failure("The balcony could not be built");
      parts.push_back(planPrism(square, postBase, s.top - s.thickness, gp_Trsf()));
    }
  }
  for (const TopoDS_Shape& part : parts)
    if (part.IsNull()) return R::failure("The balcony could not be built");
  if (parts.size() == 1) return parts.front();  // the floor alone
  auto all = geom::booleanCombine(geom::BooleanOp::Union, parts);
  if (!all) return R::failure("The balcony could not be built: " + all.error());
  return all.value();
}

// Where a balcony's posts stand (docs/167): the highest level below its floor; else 0 when its
// floor is above it. nullopt: nothing to stand on.
std::optional<double> balconyPostBase(const Document& document, const Modeler::BalconySpec& s) {
  const double bottom = s.top - s.thickness;
  std::optional<double> best;
  for (const FloorLevel& l : document.settings().levels)
    if (l.elevation < bottom - 100.0 && (!best || l.elevation > *best)) best = l.elevation;
  if (!best && bottom > 100.0) best = 0.0;
  return best;
}

// Its floor, its rail on the three sides away from the wall and its posts (from `postBase`), as
// one solid (world).
Result<TopoDS_Shape> balconySolid(const BalconyPlace& at, const Modeler::BalconySpec& s, double postBase = 0.0) {
  using R = Result<TopoDS_Shape>;
  const double half = s.width / 2;
  // A box from (u0, n0) to (u1, n1) in the balcony's frame, z0 to z1.
  auto box = [&](double u0, double n0, double u1, double n1, double z0, double z1) {
    auto p = [&](double u, double n) { return at.face.Translated(at.u * u + at.n * n); };
    return planPrism({p(u0, n0), p(u1, n0), p(u1, n1), p(u0, n1)}, z0, z1, gp_Trsf());
  };
  std::vector<TopoDS_Shape> parts{box(-half, 0, half, s.depth, s.top - s.thickness, s.top)};
  if (s.rail == "wall") {
    const double t = std::min(100.0, std::min(s.depth, s.width / 2) / 2);  // the parapet's thickness
    const double z1 = s.top + s.railHeight;
    parts.push_back(box(-half, s.depth - t, half, s.depth, s.top, z1));
    parts.push_back(box(-half, 0, -half + t, s.depth - t, s.top, z1));
    parts.push_back(box(half - t, 0, half, s.depth - t, s.top, z1));
  } else if (s.rail == "bars") {
    const double b = 50.0;  // top rail and posts, square
    const double z1 = s.top + s.railHeight;
    parts.push_back(box(-half, s.depth - b, half, s.depth, z1 - b, z1));
    parts.push_back(box(-half, 0, -half + b, s.depth - b, z1 - b, z1));
    parts.push_back(box(half - b, 0, half, s.depth - b, z1 - b, z1));
    // Posts: at the corners, at the wall, and no more than 1000 apart along the front.
    const int bays = std::max(1, static_cast<int>(std::ceil((s.width - b) / 1000.0 - 1e-9)));
    for (int i = 0; i <= bays; ++i) {
      const double u0 = -half + (s.width - b) * i / bays;
      parts.push_back(box(u0, s.depth - b, u0 + b, s.depth, s.top, z1 - b));
    }
    for (const double u0 : {-half, half - b}) parts.push_back(box(u0, 0, u0 + b, b, s.top, z1 - b));
  }
  // Posts (docs/167): under the front edge, the outer two at its corners, the others between.
  for (int i = 0; i < s.posts; ++i) {
    const double u0 = -half + (s.width - s.postSize) * i / (s.posts - 1);
    parts.push_back(box(u0, s.depth - s.postSize, u0 + s.postSize, s.depth, postBase, s.top - s.thickness));
  }
  for (const TopoDS_Shape& part : parts)
    if (part.IsNull()) return R::failure("The balcony could not be built");
  if (parts.size() == 1) return parts.front();
  auto all = geom::booleanCombine(geom::BooleanOp::Union, parts);
  if (!all) return R::failure("The balcony could not be built: " + all.error());
  return all.value();
}

nlohmann::json pointsJson(const Poly& p) {
  nlohmann::json out = nlohmann::json::array();
  for (const gp_Pnt& q : p) out.push_back({q.X(), q.Y()});
  return out;
}

std::string balconyText(const Modeler::BalconySpec& s, const BalconyPlace& at, const Bnd_Box& box, double postBase, const BalconyPlan& plan,
                        double within2m) {
  return nlohmann::json{{"wrap", s.wrap}, {"wrap_length", s.wrapLength}, {"outline", pointsJson(plan.floor)}, {"counted", pointsJson(plan.counted)},
                        {"plan_area", std::abs(polyArea2(plan.floor)) / 2}, {"within_2m_area", within2m},
                        {"along", s.along}, {"side", s.side >= 0 ? 1 : -1}, {"width", s.width}, {"depth", s.depth},
                        {"top", s.top}, {"thickness", s.thickness}, {"rail_height", s.railHeight}, {"rail", s.rail},
                        {"floor_area", s.floorArea}, {"posts", s.posts}, {"post_size", s.postSize}, {"post_base", postBase},
                        // As made: where it stands out from (for the building area, and to tell whether it was moved by hand).
                        {"face", {at.face.X(), at.face.Y()}}, {"out", {at.n.X(), at.n.Y()}}, {"behind", at.behind},
                        {"min", {box.CornerMin().X(), box.CornerMin().Y(), box.CornerMin().Z()}},
                        {"max", {box.CornerMax().X(), box.CornerMax().Y(), box.CornerMax().Z()}}}
      .dump();
}
}  // namespace

Result<Modeler::BalconySpec> Modeler::defaultBalcony(const ObjectId& wall) const {
  using R = Result<BalconySpec>;
  const Object* host = document_.find(wall);
  if (!host) return R::failure("Object not found");
  const auto band = wallBand(document_, *host);
  if (!band) return R::failure("A balcony goes on a wall made by the wall tool");
  BalconySpec spec;
  spec.host = wall;
  spec.along = band->length / 2;
  spec.width = std::max(300.0, std::min(spec.width, band->length));
  spec.top = band->z0;
  return spec;
}

Result<std::pair<double, int>> Modeler::balconyNear(const ObjectId& wall, const gp_Pnt& at) const {
  using R = Result<std::pair<double, int>>;
  const Object* host = document_.find(wall);
  if (!host) return R::failure("Object not found");
  const auto band = wallBand(document_, *host);
  if (!band) return R::failure("A balcony goes on a wall made by the wall tool");
  const gp_Vec to(gp_Pnt(band->start.X(), band->start.Y(), 0), gp_Pnt(at.X(), at.Y(), 0));
  const double middle = (band->leftExt - band->right) / 2;  // of the wall's thickness, from its line
  return std::make_pair(std::clamp(to.Dot(gp_Vec(band->dir)), 0.0, band->length), to.Dot(gp_Vec(band->left)) >= middle ? 1 : -1);
}

Result<ObjectId> Modeler::createBalcony(const BalconySpec& spec) {
  using R = Result<ObjectId>;
  if (const Status s = balconySizes(spec); !s) return R::failure(s.message());
  const auto place = balconyPlace(document_, spec);
  if (!place) return R::failure(place.error());
  double postBase = 0.0;
  if (spec.posts > 0) {
    const auto base = balconyPostBase(document_, spec);
    if (!base) return R::failure("A balcony's posts need a floor below it to stand on");
    postBase = *base;
  }
  const auto plan = balconyPlan(document_, spec, place.value());
  if (!plan) return R::failure(plan.error());
  const auto solid = plan->wraps ? wrappedSolid(*plan, spec, postBase) : balconySolid(place.value(), spec, postBase);
  if (!solid) return R::failure(solid.error());
  Object o;
  o.name = uniqueName("Balcony");
  o.recipe.base = ImportedShape{solid.value(), "balcony"};
  o.attributes["kind"] = "balcony";
  o.attributes["host"] = spec.host;
  // docs/175: the part within 2 m of the wall's centre line (the notice on open balconies).
  double within2m = 0.0;
  {
    const double d2 = std::min(spec.depth, 2000.0 - place->behind);
    if (d2 >= spec.depth - 1e-6) {
      within2m = std::abs(polyArea2(plan->floor)) / 2;
    } else if (d2 > 1e-6) {
      BalconySpec near = spec;
      near.depth = d2;
      near.posts = 0;
      if (const auto part = balconyPlan(document_, near, place.value())) within2m = std::abs(polyArea2(part->floor)) / 2;
    }
  }
  o.attributes["balcony"] = balconyText(spec, place.value(), geom::exactBounds(solid.value()), postBase, *plan, within2m);
  return addObject(std::move(o), "Balcony");
}

Result<Modeler::BalconySpec> Modeler::balconySpec(const ObjectId& id) const {
  using R = Result<BalconySpec>;
  const Object* b = document_.find(id);
  if (!b) return R::failure("Object not found");
  if (attributeOf(*b, "kind") != "balcony") return R::failure("It is not a balcony");
  const nlohmann::json j = nlohmann::json::parse(attributeOf(*b, "balcony"), nullptr, false);
  if (!j.is_object()) return R::failure("Its sizes are not kept: make the balcony again");
  BalconySpec spec;
  spec.host = attributeOf(*b, "host");
  spec.along = j.value("along", 0.0);
  spec.side = j.value("side", 1);
  spec.width = j.value("width", spec.width);
  spec.depth = j.value("depth", spec.depth);
  spec.top = j.value("top", 0.0);
  spec.thickness = j.value("thickness", spec.thickness);
  spec.railHeight = j.value("rail_height", spec.railHeight);
  spec.rail = j.value("rail", spec.rail);
  spec.floorArea = j.value("floor_area", std::string());
  spec.posts = j.value("posts", 0);
  spec.postSize = j.value("post_size", spec.postSize);
  spec.wrap = j.value("wrap", std::string());
  spec.wrapLength = j.value("wrap_length", spec.wrapLength);
  return spec;
}

Result<Modeler::BalconyParts> Modeler::balconyParts(const ObjectId& id) const {
  using R = Result<BalconyParts>;
  const auto spec = balconySpec(id);
  if (!spec) return R::failure(spec.error());
  const Object* b = document_.find(id);
  const nlohmann::json j = nlohmann::json::parse(attributeOf(*b, "balcony"), nullptr, false);
  const TopoDS_Shape world = document_.worldShape(*b);
  if (world.IsNull() || !j.contains("min") || !j.contains("max")) return R::failure("Its sizes are not kept: make the balcony again");
  const Bnd_Box box = geom::exactBounds(world);
  const gp_Pnt low(j.at("min")[0].get<double>(), j.at("min")[1].get<double>(), j.at("min")[2].get<double>());
  const gp_Pnt high(j.at("max")[0].get<double>(), j.at("max")[1].get<double>(), j.at("max")[2].get<double>());
  if (box.IsVoid() || box.CornerMin().Distance(low) > 0.5 || box.CornerMax().Distance(high) > 0.5)
    return R::failure("The balcony was moved or reshaped by hand");
  // Cut apart by height: the posts below its floor, the floor, the rail above it.
  const double floorBottom = spec->top - spec->thickness, floorTop = spec->top;
  const double far = box.CornerMin().Distance(box.CornerMax()) + 1000.0;
  auto slab = [&](double z0, double z1) -> TopoDS_Shape {
    if (z1 - z0 < 1e-6) return {};
    const gp_Pnt c((box.CornerMin().X() + box.CornerMax().X()) / 2, (box.CornerMin().Y() + box.CornerMax().Y()) / 2, 0);
    const TopoDS_Shape cutter = planPrism({gp_Pnt(c.X() - far, c.Y() - far, 0), gp_Pnt(c.X() + far, c.Y() - far, 0),
                                           gp_Pnt(c.X() + far, c.Y() + far, 0), gp_Pnt(c.X() - far, c.Y() + far, 0)},
                                          z0, z1, gp_Trsf());
    if (cutter.IsNull()) return {};
    const auto part = geom::booleanCombine(geom::BooleanOp::Intersect, {world, cutter});
    if (!part || geom::volume(part.value()) < 1.0) return {};
    return part.value();
  };
  BalconyParts parts;
  parts.floor = slab(floorBottom, floorTop);
  if (parts.floor.IsNull()) return R::failure("The balcony's floor could not be read");
  if (spec->rail != "none") parts.rail = slab(floorTop, box.CornerMax().Z() + 1.0);
  if (spec->posts > 0) parts.posts = slab(box.CornerMin().Z() - 1.0, floorBottom);
  return parts;
}

Result<ObjectId> Modeler::changeBalcony(const ObjectId& id, const BalconySpec& spec) {
  using R = Result<ObjectId>;
  const Object* b = document_.find(id);
  if (!b) return R::failure("Object not found");
  if (attributeOf(*b, "kind") != "balcony") return R::failure("It is not a balcony");
  if (b->locked) return R::failure("'" + b->name + "' is locked");
  if (const Status s = balconySizes(spec); !s) return R::failure(s.message());
  if (const auto place = balconyPlace(document_, spec); !place) return R::failure(place.error());
  const std::string name = b->name;
  const std::map<std::string, std::string> kept = b->attributes;
  history_.beginGroup("Change Balcony");
  if (const Status s = remove({id}); !s) {
    history_.abortGroup();
    return R::failure(s.message());
  }
  const auto made = createBalcony(spec);
  if (!made) {
    history_.abortGroup();
    return R::failure(made.error());
  }
  // Its name, and what else was put on it (a tag, a material name), stay.
  Object again = *document_.find(*made);
  again.name = name;
  for (const auto& [key, value] : kept)
    if (key != "kind" && key != "host" && key != "balcony") again.attributes[key] = value;
  ChangeSet changes(document_);
  changes.modify(again);
  history_.execute(changes.build("Change Balcony"));
  history_.endGroup();
  return *made;
}

RoomInfo Modeler::roomAt(const gp_Pnt& at) const { return regionInfo(document_, at); }

std::vector<RoomInfo> Modeler::rooms() const { return roomsIn(document_); }

Modeler::EnclosedArea Modeler::enclosedAt(const Document& document, double floor) {
  EnclosedArea out;
  const StoreyWalls walls = storeyWalls(document, floor);
  out.unreadableWalls = walls.unreadable;
  if (walls.plan.empty()) return out;
  for (const geom::RoomRegion& region : geom::roomRegions(walls.plan).regions) {
    out.area += region.centreArea;
    ++out.regions;
  }
  return out;
}

std::vector<std::vector<gp_Pnt>> Modeler::enclosedOutlines(const Document& document, double floor) {
  std::vector<std::vector<gp_Pnt>> out;
  const StoreyWalls walls = storeyWalls(document, floor);
  if (walls.plan.empty()) return out;
  for (const geom::RoomRegion& region : geom::roomRegions(walls.plan).regions) {
    std::vector<gp_Pnt> outline;
    for (const geom::PlanPoint& p : region.centre) outline.emplace_back(p.x, p.y, 0.0);
    if (outline.size() >= 3) out.push_back(std::move(outline));
  }
  return out;
}

std::vector<RoomInfo> Modeler::roomsIn(const Document& document_) {
  std::vector<RoomInfo> out;
  std::map<double, StoreyRegions> storeys;
  for (const Object& o : document_.objects()) {
    if (!isRoomMarker(o)) continue;
    const TextNote& note = std::get<TextNote>(o.recipe.base);
    const gp_Pnt at = note.position.Transformed(document_.worldTransform(o));
    // One working-out of the walls per height the markers stand at.
    auto storey = storeys.find(at.Z());
    if (storey == storeys.end()) storey = storeys.emplace(at.Z(), storeyRegions(document_, at.Z())).first;
    RoomInfo info = regionInfo(document_, at, storey->second);
    info.id = o.id;
    info.name = note.text;
    if (info.closed) {
      // Two markers in one region: the later one says so (areas would be counted twice).
      for (const RoomInfo& earlier : out) {
        if (!earlier.closed || earlier.centre.size() != info.centre.size() ||
            std::abs(earlier.centreArea - info.centreArea) > 1e-6 || std::abs(earlier.at.Z() - info.at.Z()) > 1e-6)
          continue;
        bool same = true;
        for (std::size_t i = 0; same && i < info.centre.size(); ++i) same = earlier.centre[i].Distance(info.centre[i]) < 1e-6;
        if (same) info.problem = "Shares its region with room '" + earlier.name + "'";
      }
    }
    out.push_back(std::move(info));
  }
  return out;
}

const std::vector<FloorRatioPart>& floorRatioParts() {
  static const std::vector<FloorRatioPart> parts = {
      {"garage", "自動車車庫等", 1.0 / 5, "令 2 条 1 項 4 号イ・3 項 1 号"},
      {"stockpile", "備蓄倉庫", 1.0 / 50, "令 2 条 1 項 4 号ロ・3 項 2 号"},
      {"battery", "蓄電池", 1.0 / 50, "令 2 条 1 項 4 号ハ・3 項 3 号"},
      {"generator", "自家発電設備", 1.0 / 100, "令 2 条 1 項 4 号ニ・3 項 4 号"},
      {"water_tank", "貯水槽", 1.0 / 100, "令 2 条 1 項 4 号ホ・3 項 5 号"},
      {"delivery_box", "宅配ボックス", 1.0 / 100, "令 2 条 1 項 4 号ヘ・3 項 6 号"},
      {"elevator", "エレベーターの昇降路", 0.0, "法 52 条 6 項 1 号・令 135 条の 16"},
      {"common_hall", "共同住宅・老人ホーム等の共用の廊下・階段", 0.0, "法 52 条 6 項 2 号"},
      // docs/166: only where the room is in a basement whose ceiling is 1 m or less over the ground.
      {"basement_dwelling", "地階の住宅・老人ホーム等の部分", 1.0 / 3, "法 52 条 3 項"},
  };
  return parts;
}

RoomFinish roomFinishOf(const Object& marker) {
  RoomFinish out;
  const auto stored = marker.attributes.find("room_finish");
  if (stored == marker.attributes.end()) return out;
  const nlohmann::json j = nlohmann::json::parse(stored->second, nullptr, false);
  if (!j.is_object()) return out;
  out.use = j.value("use", std::string());
  out.floor = j.value("floor", std::string());
  out.wall = j.value("wall", std::string());
  out.ceiling = j.value("ceiling", std::string());
  out.ceilingHeight = j.value("ceiling_height", 0.0);
  out.ratioPart = j.value("ratio_part", std::string());
  return out;
}

Result<Modeler::Ceilings> Modeler::makeCeilings(const std::vector<ObjectId>& rooms) {
  using R = Result<Ceilings>;
  constexpr double kBoard = 10.0;
  Ceilings out;
  std::vector<RoomInfo> chosen;
  for (const RoomInfo& r : roomsIn(document_))
    if (rooms.empty() || std::find(rooms.begin(), rooms.end(), r.id) != rooms.end()) chosen.push_back(r);
  if (chosen.empty()) return R::failure("No rooms to make ceilings for");
  history_.beginGroup("Ceilings");
  for (const RoomInfo& r : chosen) {
    const Object* marker = document_.find(r.id);
    const RoomFinish finish = marker ? roomFinishOf(*marker) : RoomFinish{};
    if (finish.ceilingHeight <= 0 || !r.closed || !r.innerKnown || r.inner.size() < 3) {
      out.left.push_back(r.name);
      continue;
    }
    // The ceiling made before for this room goes; only one in this room's storey (a storey copied
    // before docs/186 left the upper ceilings naming the rooms below).
    std::vector<ObjectId> old;
    const double top = r.at.Z() + finish.ceilingHeight;
    for (const Object& o : document_.objects()) {
      const auto kind = o.attributes.find("kind"), of = o.attributes.find("room");
      if (kind == o.attributes.end() || kind->second != "ceiling" || of == o.attributes.end() || of->second != r.id) continue;
      const Bnd_Box box = document_.worldBounds(o);
      if (!box.IsVoid() && (box.CornerMin().Z() < r.at.Z() - 1.0 || box.CornerMin().Z() > top + 1000.0)) continue;  // another storey's
      old.push_back(o.id);
    }
    if (!old.empty()) {
      if (const Status gone = remove(old); !gone) {
        history_.abortGroup();
        return R::failure(gone.message());
      }
    }
    const double underside = r.at.Z() + finish.ceilingHeight;
    std::vector<gp_Pnt> outline;
    for (const gp_Pnt& p : r.inner) outline.emplace_back(p.X(), p.Y(), 0.0);
    const auto made = createSlab(outline, underside + kBoard, kBoard);
    if (!made) {
      history_.abortGroup();
      return R::failure("The ceiling of '" + r.name + "' could not be made: " + made.error());
    }
    Object board = *document_.find(*made);
    board.name = uniqueName("Ceiling");
    board.attributes["kind"] = "ceiling";
    board.attributes["room"] = r.id;
    board.attributes.erase("top_level");
    // What it was made as (docs/142): followed while it is still that.
    nlohmann::json made_ = {{"underside", underside}, {"outline", nlohmann::json::array()}};
    for (const gp_Pnt& p : outline) made_["outline"].push_back({p.X(), p.Y()});
    board.attributes["ceiling_made"] = made_.dump();
    ChangeSet changes(document_);
    changes.modify(board);
    history_.execute(changes.build("Ceiling"));
    out.made.push_back(*made);
  }
  if (out.made.empty()) {
    history_.abortGroup();
    return R::failure("No room has a ceiling height and an outline within its walls (nothing was changed)");
  }
  history_.endGroup();
  return out;
}

void Modeler::followCeilings(const Command& step) {
  if (followingCeilings_) return;
  // Only a step that changed walls or rooms (or their finishes) moves ceilings.
  bool relevant = false;
  for (const ObjectDelta& d : step.deltas) {
    for (const auto* o : {d.before ? &*d.before : nullptr, d.after ? &*d.after : nullptr}) {
      if (!o) continue;
      const std::string kind = attributeOf(*o, "kind");
      relevant |= kind == "wall" || isRoomMarker(*o);
    }
    if (relevant) break;
  }
  if (!relevant) return;
  // Canopies still as made (docs/148) go with their opening: made again over it as it is now.
  struct Canopy {
    ObjectId id, host;
    std::size_t index = 0;
    CanopySpec spec;
  };
  std::vector<Canopy> canopies;
  for (const Object& o : document_.objects()) {
    if (attributeOf(o, "kind") != "canopy" || o.locked) continue;
    const nlohmann::json c = nlohmann::json::parse(attributeOf(o, "canopy"), nullptr, false);
    if (!c.is_object() || !c.contains("top") || !c.contains("face")) continue;  // made before docs/148
    const std::string host = attributeOf(o, "host"), openingId = attributeOf(o, "opening");
    const auto list = openings(host);
    if (!list) continue;
    const auto op = std::find_if(list->begin(), list->end(), [&](const OpeningInfo& x) { return x.id == openingId; });
    if (op == list->end() || std::abs(op->normal.Z()) > 1e-6) continue;  // the opening is gone: it stays
    CanopySpec spec;
    spec.depth = c.value("depth", spec.depth);
    spec.thickness = c.value("thickness", spec.thickness);
    spec.extra = c.value("extra", spec.extra);
    spec.above = c.value("above", spec.above);
    // As made: its bounds those of the board it was made as.
    const gp_Pnt face(c.at("face")[0].get<double>(), c.at("face")[1].get<double>(), 0);
    const gp_Vec n(c.at("out")[0].get<double>(), c.at("out")[1].get<double>(), 0), u = gp_Vec(0, 0, 1).Crossed(n);
    const double half = c.value("width", 0.0) / 2, top = c.value("top", 0.0);
    Bnd_Box was;
    for (const gp_Pnt& p : {face.Translated(u * -half), face.Translated(u * half), face.Translated(u * half + n * spec.depth),
                            face.Translated(u * -half + n * spec.depth)}) {
      was.Add(gp_Pnt(p.X(), p.Y(), top));
      was.Add(gp_Pnt(p.X(), p.Y(), top - spec.thickness));
    }
    const TopoDS_Shape shape = document_.worldShape(o);
    if (shape.IsNull()) continue;
    const Bnd_Box now = geom::exactBounds(shape);
    if (now.IsVoid() || now.CornerMin().Distance(was.CornerMin()) > 0.5 || now.CornerMax().Distance(was.CornerMax()) > 0.5) continue;
    // Where it would be now.
    const gp_Pnt to(op->center.X(), op->center.Y(), 0);
    const double topNow = op->bottom + op->height + spec.above + spec.thickness;
    const bool moved = to.Distance(face) > 0.5 || std::abs(topNow - top) > 0.5 || std::abs(op->width / 2 + spec.extra - half) > 0.5 ||
                       gp_Vec(op->normal.X(), op->normal.Y(), 0).Angle(n) > 1e-6;
    if (moved) canopies.push_back({o.id, host, op->index, spec});
  }
  if (!canopies.empty()) {
    followingCeilings_ = true;
    const bool wasAmending = history_.amending();
    history_.beginAmend();
    history_.beginGroup("Canopies Follow");
    bool ok = true;
    for (const Canopy& c : canopies) {
      if (!remove({c.id}) || !makeCanopy(c.host, c.index, c.spec)) {
        ok = false;
        break;
      }
    }
    if (ok) history_.endGroup();
    else history_.abortGroup();  // as they were
    if (!wasAmending) history_.endAmend();
    followingCeilings_ = false;
  }
  // Balconies still as made (docs/163) go with their wall: made again on it as it is now.
  {
    std::vector<std::pair<ObjectId, BalconySpec>> balconies;
    for (const Object& o : document_.objects()) {
      if (attributeOf(o, "kind") != "balcony" || o.locked) continue;
      const auto spec = balconySpec(o.id);
      const nlohmann::json j = nlohmann::json::parse(attributeOf(o, "balcony"), nullptr, false);
      if (!spec || !j.is_object() || !j.contains("face") || !j.contains("min") || !j.contains("max")) continue;
      BalconySpec now = *spec;
      const Object* host = document_.find(now.host);
      const auto band = host ? wallBand(document_, *host) : std::nullopt;
      if (!band) continue;  // its wall is gone, or no longer read as a wall: it stays
      now.along = std::clamp(now.along, 0.0, band->length);
      const auto place = balconyPlace(document_, now);
      if (!place) continue;
      const gp_Pnt face(j.at("face")[0].get<double>(), j.at("face")[1].get<double>(), 0);
      const gp_Vec out(j.at("out")[0].get<double>(), j.at("out")[1].get<double>(), 0);
      if (j.contains("outline") && j.at("outline").is_array()) {
        // docs/168: as it would be made now (round a corner: the other wall counts too).
        const auto plan = balconyPlan(document_, now, *place);
        if (!plan) continue;  // no longer makes a balcony there: it stays
        bool same = plan->floor.size() == j.at("outline").size() && std::abs(place->behind - j.value("behind", 0.0)) <= 0.5;
        for (std::size_t k = 0; same && k < plan->floor.size(); ++k)
          same = plan->floor[k].Distance(gp_Pnt(j.at("outline")[k][0].get<double>(), j.at("outline")[k][1].get<double>(), 0)) <= 0.5;
        if (same) continue;
      } else if (place->face.Distance(face) <= 0.5 && place->n.Angle(out) <= 1e-6 && std::abs(place->behind - j.value("behind", 0.0)) <= 0.5) {
        continue;
      }
      // As made: its bounds those it was made with.
      const TopoDS_Shape shape = document_.worldShape(o);
      if (shape.IsNull()) continue;
      const Bnd_Box box = geom::exactBounds(shape);
      const gp_Pnt low(j.at("min")[0].get<double>(), j.at("min")[1].get<double>(), j.at("min")[2].get<double>());
      const gp_Pnt high(j.at("max")[0].get<double>(), j.at("max")[1].get<double>(), j.at("max")[2].get<double>());
      if (box.IsVoid() || box.CornerMin().Distance(low) > 0.5 || box.CornerMax().Distance(high) > 0.5) continue;
      balconies.push_back({o.id, now});
    }
    if (!balconies.empty()) {
      followingCeilings_ = true;
      const bool wasAmending = history_.amending();
      history_.beginAmend();
      history_.beginGroup("Balconies Follow");
      bool ok = true;
      for (const auto& [id, spec] : balconies) ok = ok && changeBalcony(id, spec);
      if (ok) history_.endGroup();
      else history_.abortGroup();  // as they were
      if (!wasAmending) history_.endAmend();
      followingCeilings_ = false;
    }
  }
  struct Follow {
    ObjectId ceiling;
    RoomInfo room;
    double underside = 0.0;
  };
  std::vector<Follow> follow;
  std::optional<std::vector<RoomInfo>> rooms;
  for (const Object& o : document_.objects()) {
    if (attributeOf(o, "kind") != "ceiling" || o.locked) continue;
    const nlohmann::json made = nlohmann::json::parse(attributeOf(o, "ceiling_made"), nullptr, false);
    if (!made.is_object() || !made.contains("outline")) continue;  // made before docs/142, or not by makeCeilings
    // Still as made: its plan bounds, height and volume those of the outline it was made over.
    const double underside = made.value("underside", 0.0);
    std::vector<gp_Pnt> was;
    for (const auto& p : made.at("outline")) was.emplace_back(p.at(0).get<double>(), p.at(1).get<double>(), 0.0);
    if (was.size() < 3) continue;
    double twice = 0.0, x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (std::size_t i = 0; i < was.size(); ++i) {
      const gp_Pnt &p = was[i], &q = was[(i + 1) % was.size()];
      twice += p.X() * q.Y() - q.X() * p.Y();
      x0 = std::min(x0, p.X()), x1 = std::max(x1, p.X()), y0 = std::min(y0, p.Y()), y1 = std::max(y1, p.Y());
    }
    const TopoDS_Shape shape = document_.worldShape(o);
    if (shape.IsNull()) continue;
    const Bnd_Box box = geom::exactBounds(shape);
    if (box.IsVoid()) continue;
    const bool asMade = std::abs(box.CornerMin().Z() - underside) < 0.5 && std::abs(box.CornerMax().Z() - underside - 10.0) < 0.5 &&
                        std::abs(box.CornerMin().X() - x0) < 0.5 && std::abs(box.CornerMax().X() - x1) < 0.5 &&
                        std::abs(box.CornerMin().Y() - y0) < 0.5 && std::abs(box.CornerMax().Y() - y1) < 0.5 &&
                        std::abs(geom::volume(shape) - std::abs(twice) / 2 * 10.0) < 1.0 + 1e-6 * std::abs(twice);
    if (!asMade) continue;  // changed by hand: it stays
    if (!rooms) rooms = roomsIn(document_);
    const std::string roomId = attributeOf(o, "room");
    const auto r = std::find_if(rooms->begin(), rooms->end(), [&](const RoomInfo& x) { return x.id == roomId; });
    if (r == rooms->end() || !r->closed || !r->innerKnown || r->inner.size() < 3) continue;  // nothing to follow: stays
    const Object* marker = document_.find(r->id);
    const RoomFinish finish = marker ? roomFinishOf(*marker) : RoomFinish{};
    if (finish.ceilingHeight <= 0) continue;
    const double now = r->at.Z() + finish.ceilingHeight;
    bool same = std::abs(now - underside) < 0.5 && r->inner.size() == was.size();
    for (std::size_t i = 0; same && i < was.size(); ++i)
      same = std::hypot(r->inner[i].X() - was[i].X(), r->inner[i].Y() - was[i].Y()) < 0.5;
    if (!same) follow.push_back({o.id, *r, now});
  }
  if (follow.empty()) return;
  followingCeilings_ = true;
  const bool wasAmending = history_.amending();
  history_.beginAmend();  // into the step just recorded
  std::vector<ObjectId> rooms_;
  for (const Follow& f : follow) rooms_.push_back(f.room.id);
  const auto remade = makeCeilings(rooms_);
  (void)remade;  // a room that cannot have one now keeps the board it had (makeCeilings leaves it)
  if (!wasAmending) history_.endAmend();
  followingCeilings_ = false;
}

Status Modeler::setRoomFinish(const ObjectId& room, const RoomFinish& finish) {
  const Object* marker = document_.find(room);
  if (!marker || !isRoomMarker(*marker)) return Status::failure("Not a room made by the room tool");
  if (marker->locked) return Status::failure("The object is locked");
  for (const std::string* word : {&finish.use, &finish.floor, &finish.wall, &finish.ceiling}) {
    if (word->size() > 180) return Status::failure("A finish is at most 60 characters");
    if (word->find_first_of("\r\n") != std::string::npos) return Status::failure("A finish is one line");
  }
  if (!std::isfinite(finish.ceilingHeight) || finish.ceilingHeight < 0 ||
      (finish.ceilingHeight > 0 && (finish.ceilingHeight < 1000 || finish.ceilingHeight > 10000)))
    return Status::failure("A ceiling height is 1000 to 10000 (or 0: not given)");
  if (!finish.ratioPart.empty()) {
    bool known = false;
    for (const FloorRatioPart& part : floorRatioParts()) known = known || finish.ratioPart == part.code;
    if (!known) return Status::failure("A room's part for the floor area ratio is one of the listed codes");
  }
  Object object = *marker;
  if (finish.empty()) {
    object.attributes.erase("room_finish");
  } else {
    nlohmann::json j = nlohmann::json::object();
    if (!finish.use.empty()) j["use"] = finish.use;
    if (!finish.floor.empty()) j["floor"] = finish.floor;
    if (!finish.wall.empty()) j["wall"] = finish.wall;
    if (!finish.ceiling.empty()) j["ceiling"] = finish.ceiling;
    if (finish.ceilingHeight > 0) j["ceiling_height"] = finish.ceilingHeight;
    if (!finish.ratioPart.empty()) j["ratio_part"] = finish.ratioPart;
    object.attributes["room_finish"] = j.dump();
  }
  ChangeSet changes(document_);
  changes.modify(object);
  history_.execute(changes.build("Room Finish"));
  return Status::success();
}

Result<ObjectId> Modeler::createRoom(const gp_Pnt& at, const std::string& name) {
  using R = Result<ObjectId>;
  if (name.empty()) return R::failure("A room needs a name");
  const RoomInfo region = roomAt(at);
  if (!region.closed) return R::failure(region.problem + " (nothing was closed or added)");
  for (const RoomInfo& other : rooms()) {
    if (!other.closed || std::abs(other.at.Z() - at.Z()) > 1e-6 || other.centre.size() != region.centre.size()) continue;
    bool same = std::abs(other.centreArea - region.centreArea) < 1e-6;
    for (std::size_t i = 0; same && i < region.centre.size(); ++i) same = other.centre[i].Distance(region.centre[i]) < 1e-6;
    if (same) return R::failure("Room '" + other.name + "' is already here");
  }
  Object o;
  o.name = uniqueName("Room");
  o.layer = "Annotations";
  o.recipe.base = TextNote{at, std::nullopt, name};
  o.attributes["kind"] = "room";
  return addObject(std::move(o), "Room");
}

Status Modeler::attachToLevels(const std::vector<ObjectId>& ids, std::optional<std::string> base,
                               std::optional<std::string> top) {
  if (ids.empty()) return Status::failure("Nothing selected");
  if (!base && !top) return Status::failure("Give a base or a top level");
  const std::vector<FloorLevel>& levels = document_.settings().levels;
  auto resolve = [&](const std::string& text) -> const FloorLevel* {
    for (const FloorLevel& l : levels)
      if (l.id == text) return &l;
    for (const FloorLevel& l : levels)
      if (l.name == text) return &l;
    return nullptr;
  };
  const FloorLevel* baseLevel = base && !base->empty() ? resolve(*base) : nullptr;
  const FloorLevel* topLevel = top && !top->empty() ? resolve(*top) : nullptr;
  if (base && !base->empty() && !baseLevel) return Status::failure("No level '" + *base + "'");
  if (top && !top->empty() && !topLevel) return Status::failure("No level '" + *top + "'");
  ChangeSet changes(document_);
  for (const ObjectId& id : ids) {
    const Object* current = document_.find(id);
    if (!current) return Status::failure("Object not found");
    if (current->locked) return Status::failure("'" + current->name + "' is locked");
    const std::string kind = attributeText(*current, "kind");
    if (kind != "wall" && kind != "slab")
      return Status::failure("'" + current->name + "' is not a wall or a slab made by those tools");
    if (kind == "slab" && base && !base->empty())
      return Status::failure("A slab lies with its top on a level: give 'top'");
    const Bnd_Box box = geom::exactBounds(document_.worldShape(*current));
    if (box.IsVoid()) return Status::failure("'" + current->name + "' has no shape");
    const double z0 = box.CornerMin().Z(), z1 = box.CornerMax().Z();
    Object o = *current;
    auto set = [&](const char* key, const std::optional<std::string>& value, const FloorLevel* level) {
      if (!value) return;
      if (level) o.attributes[key] = level->id;
      else o.attributes.erase(key);
    };
    set(kBaseLevel, base, baseLevel);
    set(kTopLevel, top, topLevel);
    const double dBase = baseLevel ? baseLevel->elevation - z0 : 0.0;
    // The top goes to its new level; one staying on a level stays where it is; a free one
    // goes with the foot.
    const double dTop = topLevel ? topLevel->elevation - z1 : o.attributes.count(kTopLevel) ? 0.0 : dBase;
    if (!shiftOnLevels(document_, *current, o, dBase, dTop, document_.regenContext()))
      return Status::failure("'" + current->name + "' cannot be put on those levels: its top would not be above its "
                             "foot, or it is no longer the wall its tool made (nothing was changed)");
    changes.modify(o);
  }
  history_.execute(changes.build("Attach To Levels"));
  return Status::success();
}

Result<std::vector<ObjectId>> Modeler::setWallType(const std::vector<ObjectId>& ids, const std::string& typeId,
                                                  double core, const std::string& outside) {
  using R = Result<std::vector<ObjectId>>;
  const auto type = construction::wallType(typeId, core);
  if (!type) return R::failure(type.error());
  if (outside != "auto" && outside != "left" && outside != "right" && outside != "flip")
    return R::failure("outside: auto, left, right or flip");
  if (ids.empty()) return R::failure("Choose the walls to change");
  // Every wall of the model (neighbours are fitted again too).
  std::vector<WallBand> walls;
  std::map<ObjectId, std::size_t> at;
  for (const Object& o : document_.objects())
    if (auto w = wallBand(document_, o)) {
      at[o.id] = walls.size();
      walls.push_back(std::move(*w));
    }
  std::set<std::size_t> targets;
  for (const ObjectId& id : ids) {
    const Object* o = document_.find(id);
    if (!o) return R::failure("Object not found");
    const auto it = at.find(id);
    if (it == at.end())
      return R::failure("'" + o->name + "' is not a wall drawn as a band (kind = wall, pushed up, not edited since)");
    targets.insert(it->second);
  }
  // Which side is outside.
  auto middle = [&](const std::set<std::size_t>& of) {
    gp_XYZ sum(0, 0, 0);
    for (std::size_t i : of) sum += walls[i].start.Translated(gp_Vec(walls[i].dir) * (walls[i].length / 2)).XYZ();
    return gp_Pnt(sum / static_cast<double>(of.size()));
  };
  std::set<std::size_t> everything;
  for (std::size_t i = 0; i < walls.size(); ++i) everything.insert(i);
  const gp_Pnt centre = middle(targets.size() > 1 ? targets : everything);
  std::map<std::size_t, std::pair<double, double>> before;  // index -> (right, left) as they were
  std::map<std::size_t, bool> outsideRight;
  for (std::size_t i : targets) {
    WallBand& w = walls[i];
    before[i] = {w.right, w.leftExt};
    bool right = outside == "right";
    if (outside == "auto" || outside == "flip") {
      const auto kept = w.object->attributes.find("wall_outside");
      if (kept != w.object->attributes.end()) {
        right = kept->second == "right";
      } else {
        const gp_Pnt mid = w.start.Translated(gp_Vec(w.dir) * (w.length / 2));
        const double side = gp_Vec(mid, centre).Dot(gp_Vec(w.left));
        right = side > 1.0;  // the middle of the walls is on the left: outside to the right
      }
    }
    if (outside == "flip") right = !right;  // the other way round from now
    outsideRight[i] = right;
    w.right = right ? type->outside() : type->inside();
    w.leftExt = right ? type->inside() : type->outside();
  }
  // Ends: [s0, s1] along each wall's line from its start; refit those touching a changed wall.
  std::vector<std::pair<double, double>> ends(walls.size());
  for (std::size_t i = 0; i < walls.size(); ++i) ends[i] = {0.0, walls[i].length};
  std::set<std::size_t> changed(targets.begin(), targets.end());
  for (std::size_t i = 0; i < walls.size(); ++i) {
    const WallBand& w = walls[i];
    for (int end = 0; end < 2; ++end) {
      const double s = end == 0 ? 0.0 : w.length;
      // The node: the nearest other wall line this end reaches.
      std::optional<std::size_t> partner;
      double bestGap = 1e300;
      bool corner = false;
      for (std::size_t j = 0; j < walls.size(); ++j) {
        if (j == i) continue;
        const WallBand& o = walls[j];
        if (std::min(w.z1, o.z1) - std::max(w.z0, o.z0) < 1.0) continue;  // another storey
        const auto m = meet(w.start, w.dir, o.start, o.dir);
        if (!m) continue;
        const double reach = std::max(before.count(j) ? before[j].first + before[j].second : o.right + o.leftExt,
                                      o.right + o.leftExt) + std::max(w.right + w.leftExt, 1.0) + 1.0;
        const double gap = std::abs(m->first - s);
        if (gap > reach || m->second < -reach || m->second > o.length + reach || gap >= bestGap) continue;
        bestGap = gap;
        partner = j;
        corner = std::abs(m->second) <= reach || std::abs(m->second - o.length) <= reach;
      }
      if (!partner) continue;  // a free end stays
      const std::size_t j = *partner;
      if (!targets.count(i) && !targets.count(j)) continue;  // nothing about it changes
      const WallBand& o = walls[j];
      // The partner's two faces across this wall's line.
      double f0 = 0, f1 = 0;
      {
        const auto a = meet(w.start, w.dir, o.start.Translated(gp_Vec(o.left) * o.leftExt), o.dir);
        const auto b = meet(w.start, w.dir, o.start.Translated(gp_Vec(o.left) * -o.right), o.dir);
        if (!a || !b) continue;
        f0 = std::min(a->first, b->first);
        f1 = std::max(a->first, b->first);
      }
      const bool incoming = end == 1;
      bool owner = false;  // runs through the corner square
      if (corner) {
        // The partner's end at the node: incoming if it is its far end.
        const auto m = meet(o.start, o.dir, w.start, w.dir);
        const bool partnerIncoming = m && std::abs(m->first - o.length) < std::abs(m->first);
        owner = incoming != partnerIncoming ? incoming : i < j;
      }
      const double value = owner ? (incoming ? f1 : f0) : (incoming ? f0 : f1);
      if (end == 0) ends[i].first = value;
      else ends[i].second = value;
      changed.insert(i);
    }
  }
  // Rebuild every changed wall.
  Command command{"Wall Type", {}, {}};
  std::vector<ObjectId> out;
  for (std::size_t i : changed) {
    const WallBand& w = walls[i];
    Object object = *w.object;
    Recipe recipe = w.recipe;
    auto& rect = std::get<RectangleProfile>(recipe.base);
    const auto [s0, s1] = ends[i];
    if (s1 - s0 < tolerance::kMinDimension)
      return R::failure("'" + object.name + "' would be shorter than the walls are thick");
    const gp_Trsf toLocal = w.toWorld.Inverted();
    const gp_Pnt origin = w.start.Translated(gp_Vec(w.dir) * s0 + gp_Vec(w.left) * -w.right);
    rect.plane.origin = origin.Transformed(toLocal);
    rect.width = s1 - s0;
    const double oldThick = rect.height;
    rect.height = w.right + w.leftExt;
    if (w.path.empty()) {
      object.recipe = recipe;
    } else {
      replaceOperand(object.recipe, w.path, 0, std::move(recipe));
    }
    if (targets.count(i)) {
      // Openings: from the face they were cut from, through the new thickness.
      const auto [right0, left0] = before.at(i);
      const gp_Trsf hostWorld = document_.worldTransform(*w.object);
      for (const OpeningRef& op : rectangularOpenings(*w.object)) {
        Recipe cutter = *op.operand->recipe;
        auto& face = std::get<RectangleProfile>(cutter.base);
        const gp_Trsf cutterWorld = hostWorld * op.placement;
        const gp_Dir n = face.plane.normal.Transformed(cutterWorld);
        const bool fromLeft = gp_Vec(n).Dot(gp_Vec(w.left)) > 0;
        const double move = fromLeft ? w.leftExt - left0 : w.right - right0;
        face.plane.origin.Translate(gp_Vec(n).Transformed(cutterWorld.Inverted()) * move);
        for (auto& m : cutter.modifiers)
          if (auto* e = std::get_if<ExtrudeOp>(&m))
            e->distance = std::copysign(std::abs(e->distance) + (rect.height - oldThick), e->distance);
        replaceOperand(object.recipe, op.path, 0, std::move(cutter));
      }
      char offset[32];
      std::snprintf(offset, sizeof offset, "%.6g", w.right);
      object.attributes["wall_line"] = "offset";
      object.attributes["wall_line_offset"] = offset;
      object.attributes["wall_type"] = type->id;
      object.attributes["wall_type_name"] = type->name;
      object.attributes["wall_outside"] = outsideRight.at(i) ? "right" : "left";
      object.attributes["wall_layers"] = type->layersJson();
    }
    const Status built = rebuild(object, document_.regenContext());
    if (!built) return R::failure("'" + object.name + "': " + built.message());
    command.deltas.push_back(ObjectDelta{object.id, *w.object, object, document_.indexOf(object.id)});
    out.push_back(object.id);
  }
  // Mitred corners (docs/159) are made again for the new thicknesses, in the same undo step.
  history_.beginGroup(command.name);
  history_.execute(std::move(command));
  ChangeSet corners(document_);
  refitWallCorners(document_, corners, document_.regenContext());
  history_.execute(corners.build("Wall Type"));
  history_.endGroup();
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
  if (const Status settled = settleMoves(document_, changes, document_.regenContext()); !settled) return settled;
  history_.execute(changes.build("Align"));
  return Status::success();
}

Result<std::vector<ObjectId>> Modeler::createWalls(std::vector<gp_Pnt> points, double thickness, double height,
                                                  int justification) {
  const double o = justification < 0 ? 0.0 : justification > 0 ? -thickness : -thickness / 2;
  return createWallBands(std::move(points), thickness, height, o,
                         {{"wall_line", justification < 0 ? "right" : justification > 0 ? "left" : "center"}});
}

Result<std::vector<ObjectId>> Modeler::createTypedWalls(std::vector<gp_Pnt> points, double height,
                                                       const std::string& typeId, double core, bool outsideRight) {
  using R = Result<std::vector<ObjectId>>;
  const auto type = construction::wallType(typeId, core);
  if (!type) return R::failure(type.error());
  // A closed outline drawn anticlockwise has its inside on the left: turn it round so the
  // outside (left) faces out.
  const bool closed = points.size() > 3 && points.front().Distance(points.back()) < tolerance::kLinear;
  if (closed && type->exterior) {
    double area2 = 0.0;
    for (std::size_t i = 0; i + 1 < points.size(); ++i)
      area2 += points[i].X() * points[i + 1].Y() - points[i + 1].X() * points[i].Y();
    if ((area2 > 0) != outsideRight) std::reverse(points.begin(), points.end());
  }
  const double left = outsideRight ? type->inside() : type->outside();
  const double right = outsideRight ? type->outside() : type->inside();
  char offset[32];
  std::snprintf(offset, sizeof offset, "%.6g", right);
  return createWallBands(std::move(points), left + right, height, -right,
                         {{"wall_line", "offset"},
                          {"wall_line_offset", offset},
                          {"wall_type", type->id},
                          {"wall_type_name", type->name},
                          {"wall_outside", outsideRight ? "right" : "left"},
                          {"wall_layers", type->layersJson()}});
}

Result<std::vector<ObjectId>> Modeler::createWallBands(std::vector<gp_Pnt> points, double thickness, double height,
                                                      double o,
                                                      const std::vector<std::pair<std::string, std::string>>& attributes) {
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
  // Butt joints (exact for right angles): at a corner the incoming wall
  // runs on to the far side of the corner square, the outgoing one starts
  // after it. turn = +1 left, -1 right, 0 straight on.
  auto turn = [&](std::size_t in, std::size_t out) {
    const double z = gp_Vec(dirs[in]).Crossed(gp_Vec(dirs[out])).Z();
    return std::abs(z) < 1e-9 ? 0 : (z > 0 ? 1 : -1);
  };
  // docs/178: an open run's first or last point on the free end of another wall's line (drawn on
  // from it): a corner with that wall, butt-joined as within one run. That wall runs on to the
  // far face of the new one; the new one starts at its face.
  struct EndJoin {
    std::size_t wall = 0;  // in `existing` (read before the new walls are made)
    ObjectId id;           // that wall
    double length = 0.0;   // its length along its line
    int end = 0;           // that wall's end
    double run = 0.0;      // where that end goes along its line
    double cut = 0.0;      // the new wall's end from the point, into its body (+) or out (-)
    bool tee = false;      // docs/179: on that wall's line between its ends (that wall stays as it is)
  };
  const std::vector<WallBand> existing = closed ? std::vector<WallBand>{} : allWallBands(document_);
  const auto existingEnds = wallEnds(existing);
  // The new wall's body lies from `at` along `into`.
  auto endJoin = [&](const gp_Pnt& at, const gp_Dir& into) -> std::optional<EndJoin> {
    for (std::size_t k = 0; k < existing.size(); ++k) {
      const WallBand& w = existing[k];
      if (std::min(w.z1, base + height) - std::max(w.z0, base) < 1.0) continue;
      if (std::abs(gp_Vec(w.dir).Crossed(gp_Vec(into)).Z()) < 0.25) continue;
      for (int end = 0; end < 2; ++end) {
        const gp_Pnt e = w.start.Translated(gp_Vec(w.dir) * (end == 0 ? 0.0 : w.length));
        if (std::hypot(e.X() - at.X(), e.Y() - at.Y()) > 0.5) continue;
        if (existingEnds[k][static_cast<std::size_t>(end)].joined) continue;  // already at a wall
        EndJoin j;
        j.wall = k;
        j.id = w.object->id;
        j.length = w.length;
        j.end = end;
        return j;
      }
    }
    // docs/179: on another wall's line between its ends (drawn from its middle): a T joint, the
    // new wall starting at that wall's face.
    for (std::size_t k = 0; k < existing.size(); ++k) {
      const WallBand& w = existing[k];
      if (std::min(w.z1, base + height) - std::max(w.z0, base) < 1.0) continue;
      if (std::abs(gp_Vec(w.dir).Crossed(gp_Vec(into)).Z()) < 0.25) continue;
      const gp_Vec to(w.start, gp_Pnt(at.X(), at.Y(), w.start.Z()));
      const double along = to.Dot(gp_Vec(w.dir)), across = to.Dot(gp_Vec(w.left));
      if (std::abs(across) > 0.5 || along < 1.0 || along > w.length - 1.0) continue;
      EndJoin j;
      j.wall = k;
      j.id = w.object->id;
      j.length = w.length;
      j.tee = true;
      return j;
    }
    return std::nullopt;
  };
  std::optional<EndJoin> firstJoin, lastJoin;
  if (!closed) {
    firstJoin = endJoin(points.front(), dirs.front());
    lastJoin = endJoin(points.back(), gp_Dir(-gp_Vec(dirs.back())));
  }
  // The join's numbers once the new wall's band (offsets o, o + thickness to the left of the
  // segment's direction `segDir`) is known.
  auto settle = [&](EndJoin& j, const gp_Pnt& at, const gp_Dir& segDir, const gp_Dir& into) {
    const WallBand& w = existing[j.wall];
    const gp_Dir left = gp::DZ().Crossed(segDir);
    // That wall runs on to the face of the new one farther from its own body.
    const auto f0 = meet(w.start, w.dir, at.Translated(gp_Vec(left) * o), segDir);
    const auto f1 = meet(w.start, w.dir, at.Translated(gp_Vec(left) * (o + thickness)), segDir);
    // The new one starts at that wall's face on its own side.
    const auto g0 = meet(at, into, w.start.Translated(gp_Vec(w.left) * w.leftExt), w.dir);
    const auto g1 = meet(at, into, w.start.Translated(gp_Vec(w.left) * -w.right), w.dir);
    if (!f0 || !f1 || !g0 || !g1) return false;
    j.run = j.end == 1 ? std::max(f0->first, f1->first) : std::min(f0->first, f1->first);
    j.cut = std::max(g0->first, g1->first);
    return true;
  };
  if (firstJoin && !settle(*firstJoin, points.front(), dirs.front(), dirs.front())) firstJoin.reset();
  if (lastJoin && !settle(*lastJoin, points.back(), dirs.back(), gp_Dir(-gp_Vec(dirs.back())))) lastJoin.reset();
  if (firstJoin && lastJoin && !firstJoin->tee && !lastJoin->tee && firstJoin->wall == lastJoin->wall && firstJoin->end == lastJoin->end)
    lastJoin.reset();
  history_.beginGroup("Walls");
  std::vector<ObjectId> walls;
  for (std::size_t i = 0; i < segments; ++i) {
    const gp_Pnt a = points[i], b = points[(i + 1) % points.size()];
    const double length = a.Distance(b);
    double startCut = 0.0, endRun = 0.0;
    if (i == 0 && firstJoin) startCut = firstJoin->cut;
    if (i + 1 == segments && lastJoin) endRun = -lastJoin->cut;
    // At a corner that is not a right angle the same faces are farther along the line: the
    // distances grow by 1 / sin(turn), so that each end lies on the other wall's face (and
    // the walls count as joined, docs/107). Nearly straight turns keep the square distances.
    auto slant = [&](std::size_t in, std::size_t out) {
      const double sine = std::abs(gp_Vec(dirs[in]).Crossed(gp_Vec(dirs[out])).Z());
      return sine < 0.25 ? 1.0 : 1.0 / sine;
    };
    if (closed || i > 0) {
      const std::size_t before = (i + segments - 1) % segments;
      const int t = turn(before, i);
      startCut = (t > 0 ? o + thickness : t < 0 ? -o : 0.0) * slant(before, i);
    }
    if (closed || i + 1 < segments) {
      const int t = turn(i, (i + 1) % segments);
      endRun = (t > 0 ? -o : t < 0 ? o + thickness : 0.0) * slant(i, (i + 1) % segments);
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
    // docs/193: made as it is, not as a sketch: a loose face at its base (a site outline at GL)
    // is neither cut by it nor merged into it (a wall made of a region of that face is no wall).
    Object profile;
    profile.name = uniqueName("Wall");
    profile.recipe.base = RectangleProfile{plane, wallLength, thickness};
    auto id = addObject(std::move(profile), "Rectangle");
    if (!id || !extrude(*id, height)) {
      history_.abortGroup();
      return R::failure(id ? "Wall could not be extruded" : id.error());
    }
    walls.push_back(*id);
  }
  Status tagged = setAttribute(walls, "kind", "wall");
  // Which face the drawn line is (looking along it): kept when the
  // thickness changes (docs/41); and a build-up's description.
  for (const auto& [key, value] : attributes)
    if (tagged) tagged = setAttribute(walls, key, value);
  // Made at a level's height: the walls stand on it, and reach up to the level their top is at
  // (docs/90 G-02). Elsewhere they are on no level.
  if (const FloorLevel* level = levelAtHeight(document_.settings().levels, base); level && tagged)
    tagged = setAttribute(walls, kBaseLevel, level->id);
  if (const FloorLevel* level = levelAtHeight(document_.settings().levels, base + height); level && tagged)
    tagged = setAttribute(walls, kTopLevel, level->id);
  if (!tagged) {
    history_.abortGroup();
    return R::failure(tagged.message());
  }
  // docs/178: the walls drawn on from run on to the new ones.
  if ((firstJoin && !firstJoin->tee) || (lastJoin && !lastJoin->tee)) {
    // (The bands read before are not used here: their objects may have moved in the document.)
    ChangeSet changes(document_);
    std::map<ObjectId, std::pair<double, double>> runs;
    for (const auto& j : {firstJoin, lastJoin}) {
      if (!j || j->tee) continue;
      auto& r = runs.emplace(j->id, std::make_pair(0.0, j->length)).first->second;
      (j->end == 0 ? r.first : r.second) = j->run;
    }
    int changed = 0;
    for (const auto& [id, r] : runs) {
      const Object* now = document_.find(id);
      if (!now || now->locked) continue;
      const auto band = wallBand(document_, *now);
      Object wall = *now;
      if (band && reshapeWallBand(document_, *band, r.first, r.second, wall, document_.regenContext())) {
        changes.modify(wall);
        ++changed;
      }
    }
    if (changed) {
      refitWallCorners(document_, changes, document_.regenContext());  // its mitres where it is now
      history_.execute(changes.build("Join Walls"));
    }
  }
  // docs/177: mitred from the start (the wall tool), with walls already marked where they meet.
  if (mitreNewWalls_) {
    if (const auto mitred = mitreWallCorners(walls, true); !mitred) {
      history_.abortGroup();
      return R::failure(mitred.error());
    }
  }
  history_.endGroup();
  return walls;
}

// --- linear members -----------------------------------------------------------

namespace {

// How far the shape reaches along the member's axis (its vertices: the
// end faces are flat and every edge ends in one).
double extentAlong(const TopoDS_Shape& shape, const gp_Pnt& origin, const gp_Dir& axis) {
  double lo = 0, hi = 0;
  bool first = true;
  TopTools_IndexedMapOfShape vertices;
  TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);
  for (int i = 1; i <= vertices.Extent(); ++i) {
    const double t = gp_Vec(origin, BRep_Tool::Pnt(TopoDS::Vertex(vertices(i)))).Dot(gp_Vec(axis));
    lo = first ? t : std::min(lo, t);
    hi = first ? t : std::max(hi, t);
    first = false;
  }
  return hi - lo;
}

std::string attributeOr(const Object& o, const std::string& key) {
  const auto it = o.attributes.find(key);
  return it == o.attributes.end() ? std::string() : it->second;
}

}  // namespace

// Walls and slabs made by their tools: an extruded rectangle (walls) or
// plan polygon (slabs), tagged kind = wall / slab.
std::optional<Modeler::MemberInfo> Modeler::wallOrSlabInfo(const Object& o) const {
  const std::string kind = attributeOr(o, "kind");
  if (kind != "wall" && kind != "slab") return std::nullopt;
  // A wall with openings cut into it (docs/90 G-04): its own rectangle and push lie inside the
  // cuts. Its length and height stay editable; its thickness changes with its build-up
  // (set_wall_type), which also refits the openings.
  if (kind == "wall" && std::holds_alternative<BooleanFeature>(o.recipe.base)) {
    const auto band = wallBand(document_, o);
    if (!band || band->path.empty()) return std::nullopt;
    const auto& r = std::get<RectangleProfile>(band->recipe.base);
    MemberInfo cut;
    cut.id = o.id;
    cut.element = kind;
    cut.role = kind;
    cut.materialClass = attributeOr(o, "material_class");
    cut.fields.push_back({"length", "wall length along its line", "mm", std::abs(r.width), "parameter", true,
                          "its openings stay where they are and must stay inside the wall"});
    cut.fields.push_back({"thickness", "wall thickness", "mm", std::abs(r.height), "parameter", false,
                          "a wall with openings changes thickness with its build-up (set_wall_type)"});
    cut.fields.push_back({"height", "wall height from its foot", "mm",
                          std::abs(std::get<ExtrudeOp>(band->recipe.modifiers.front()).distance), "parameter", true,
                          "its openings keep their sill heights and must stay inside the wall"});
    return cut;
  }
  if (kind == "slab" && std::holds_alternative<BooleanFeature>(o.recipe.base)) {
    // A floor opened for a stair: its own outline and thickness lie inside the cut.
    const Recipe* core = slabCore(o.recipe);
    if (!core) return std::nullopt;
    Object inner = o;
    inner.recipe = *core;
    return wallOrSlabInfo(inner);
  }
  if (o.recipe.modifiers.empty() || !std::holds_alternative<ExtrudeOp>(o.recipe.modifiers.front())) return std::nullopt;
  const double extrude = std::get<ExtrudeOp>(o.recipe.modifiers.front()).distance;
  MemberInfo info;
  info.id = o.id;
  info.element = kind;
  info.role = kind;
  info.materialClass = attributeOr(o, "material_class");
  info.edited = o.recipe.modifiers.size() > 1;
  const std::string note = info.edited ? "edits made since are redone on a change, or nothing changes" : "";
  if (kind == "wall") {
    const auto* r = std::get_if<RectangleProfile>(&o.recipe.base);
    if (!r) return std::nullopt;
    info.fields.push_back({"length", "wall length along its line", "mm", std::abs(r->width), "parameter", true, note});
    info.fields.push_back({"thickness", "wall thickness", "mm", std::abs(r->height), "parameter", true,
                           "kept: the wall's " + (attributeOr(o, "wall_line").empty() ? std::string("centre (no line recorded)")
                                                                                       : attributeOr(o, "wall_line") + " line") +
                               (note.empty() ? "" : "; " + note)});
    info.fields.push_back({"height", "wall height from its foot", "mm", std::abs(extrude), "parameter", true, note});
    return info;
  }
  const auto* p = std::get_if<PolygonProfile>(&o.recipe.base);
  if (!p || p->points.empty()) return std::nullopt;
  const double bottom = p->points.front().Z();
  if (info.edited) {
    // Its top or bottom pushed / pulled since: what it is now.
    const Bnd_Box box = geom::exactBounds(o.shape);
    info.fields.push_back({"thickness", "slab thickness (vertical extent now)", "mm",
                           box.IsVoid() ? std::abs(extrude) : box.CornerMax().Z() - box.CornerMin().Z(), "shape", true, note});
    info.fields.push_back({"top", "top level now", "mm", box.IsVoid() ? bottom + extrude : box.CornerMax().Z(), "shape",
                           false, ""});
  } else {
    info.fields.push_back({"thickness", "slab thickness", "mm", extrude, "parameter", true, "keep: top (default), bottom or center"});
    info.fields.push_back({"top", "top level", "mm", bottom + extrude, "parameter", false, ""});
  }
  return info;
}

// A wall's own change committed; when its far end moved along its line by `lengthened`, the
// walls at that corner go with it (docs/102), in the same undo step.
Status Modeler::commitWallChange(Object object, const std::string& commandName, double lengthened) {
  const ObjectId id = object.id;
  if (const Status built = rebuild(object, document_.regenContext()); !built) return built;
  ChangeSet changes(document_);
  changes.modify(object);
  lastJoins_ = JoinFollow{};
  if (keepWallJoins_) followWallLength(document_, changes, document_.regenContext(), id, lengthened, lastJoins_);
  refitWallsToRoofs(document_, changes, document_.regenContext());
  refitStairOpenings(document_, changes, document_.regenContext());
  history_.execute(changes.build(commandName));
  return Status::success();
}

Result<Modeler::MemberInfo> Modeler::changeWallOrSlab(const ObjectId& id, const MemberChange& change) {
  using R = Result<MemberInfo>;
  const Object* current = document_.find(id);
  Object object = *current;
  const std::string kind = attributeOr(object, "kind");
  if (change.catalog || change.width || change.depth || change.web || change.flange || change.radius || change.roll ||
      change.anchor || change.diameter) {
    return R::failure("A " + kind + " has no section: use thickness" + (kind == "wall" ? ", height and length" : "") +
                      " (nothing was changed)");
  }
  auto positive = [](const std::optional<double>& v) { return !v || (std::isfinite(*v) && *v >= tolerance::kMinDimension); };
  if (!positive(change.thickness) || !positive(change.height) || !positive(change.length)) {
    return R::failure("Sizes must be positive (nothing was changed)");
  }
  if (kind == "wall" && std::holds_alternative<BooleanFeature>(object.recipe.base)) {
    // A wall with openings: its rectangle and push inside the cuts change; the openings stay
    // where they are (from the wall's start and foot) and must still lie inside the wall.
    if (change.thickness)
      return R::failure("A wall with openings changes thickness with its build-up (set_wall_type refits the "
                        "openings); nothing was changed");
    const auto band = wallBand(document_, *current);
    if (!band || band->path.empty()) return R::failure("This wall is no longer the wall its tool made (nothing was changed)");
    Recipe recipe = band->recipe;
    auto& r = std::get<RectangleProfile>(recipe.base);
    auto& push = std::get<ExtrudeOp>(recipe.modifiers.front());
    if (change.length) r.width = *change.length;
    if (change.height) push.distance = std::copysign(*change.height, push.distance);
    const double length = r.width, top = band->z0 + std::abs(push.distance);
    for (const OpeningExtent& e : openingExtents(*current, document_.worldTransform(*current))) {
      const gp_Vec u(gp::DZ().Crossed(e.normal));
      const double s = gp_Vec(band->start.XYZ()).Dot(u);
      const bool along = u.Dot(gp_Vec(band->dir)) > 0;
      const double t0 = along ? e.u0 - s : s - e.u1, t1 = along ? e.u1 - s : s - e.u0;
      if (t0 < -tolerance::kLinear || t1 > length + tolerance::kLinear || e.z1 > top + tolerance::kLinear)
        return R::failure("Opening " + std::to_string(e.index) +
                          " would no longer be inside the wall: move or remove it first (nothing was changed)");
    }
    replaceOperand(object.recipe, band->path, 0, std::move(recipe));
    const Status committed = commitWallChange(std::move(object), "Change wall", change.length ? *change.length - band->length : 0.0);
    if (!committed) return R::failure("The wall was not changed: " + committed.message());
    return memberInfo(id);
  }
  if (kind == "slab" && std::holds_alternative<BooleanFeature>(object.recipe.base)) {
    // A floor opened for a stair: the slab inside the cut changes, the opening is made again.
    const Recipe* core = slabCore(object.recipe);
    if (!core || !std::holds_alternative<PolygonProfile>(core->base) || core->modifiers.empty() ||
        !std::holds_alternative<ExtrudeOp>(core->modifiers.front()))
      return R::failure("This slab is no longer the slab its tool made (nothing was changed)");
    if (change.length) return R::failure("A slab's outline changes by editing it directly (nothing was changed)");
    if (change.height) return R::failure("A slab has a thickness, not a height (nothing was changed)");
    Recipe inner = *core;
    if (change.thickness) {
      auto& push = std::get<ExtrudeOp>(inner.modifiers.front());
      const std::string keep = change.keep.value_or("top");
      if (keep != "top" && keep != "bottom" && keep != "center") return R::failure("keep: top, bottom or center");
      const double before = push.distance, after = *change.thickness;
      const double lift = keep == "bottom" ? 0.0 : keep == "top" ? before - after : (before - after) / 2;
      for (gp_Pnt& q : std::get<PolygonProfile>(inner.base).points) q.SetZ(q.Z() + lift);
      push.distance = after;
    }
    setSlabCore(object.recipe, std::move(inner));
    if (const Status built = rebuild(object, document_.regenContext()); !built)
      return R::failure("The slab was not changed: " + built.message());
    ChangeSet changes(document_);
    changes.modify(object);
    refitStairOpenings(document_, changes, document_.regenContext());
    history_.execute(changes.build("Change slab"));
    return memberInfo(id);
  }
  auto& extrude = std::get<ExtrudeOp>(object.recipe.modifiers.front());
  if (kind == "wall") {
    auto& r = std::get<RectangleProfile>(object.recipe.base);
    if (change.length) r.width = (r.width < 0 ? -1 : 1) * *change.length;
    if (change.height) extrude.distance = (extrude.distance < 0 ? -1 : 1) * *change.height;
    if (change.thickness) {
      // The band runs from the origin line to the left (+yDir) by the
      // rectangle's height: keep the recorded line.
      const std::string keep = change.keep.value_or(attributeOr(object, "wall_line").empty() ? "center"
                                                                                               : attributeOr(object, "wall_line"));
      // A build-up wall (line at an offset): its right face, and so its line, stay.
      if (keep != "left" && keep != "right" && keep != "center" && keep != "offset")
        return R::failure("keep: left, right or center");
      const double before = r.height, after = (r.height < 0 ? -1 : 1) * *change.thickness;
      const double shift = keep == "right" || keep == "offset" ? 0.0 : keep == "left" ? before - after : (before - after) / 2;
      r.plane.origin.Translate(gp_Vec(r.plane.yDir()) * shift);
      r.height = after;
    }
  } else {
    if (change.length) return R::failure("A slab's outline changes by editing it directly (nothing was changed)");
    if (change.height) return R::failure("A slab has a thickness, not a height (nothing was changed)");
    if (change.thickness) {
      auto& p = std::get<PolygonProfile>(object.recipe.base);
      const std::string keep = change.keep.value_or("top");
      if (keep != "top" && keep != "bottom" && keep != "center") return R::failure("keep: top, bottom or center");
      const double before = extrude.distance, after = *change.thickness;
      const double lift = keep == "bottom" ? 0.0 : keep == "top" ? before - after : (before - after) / 2;
      for (gp_Pnt& q : p.points) q.SetZ(q.Z() + lift);
      extrude.distance = after;
    }
  }
  double lengthened = 0.0;
  if (kind == "wall" && change.length) {
    if (const auto band = wallBand(document_, *current)) lengthened = *change.length - band->length;
  }
  const Status committed = kind == "wall" ? commitWallChange(std::move(object), "Change wall", lengthened)
                                          : commitModified(std::move(object), "Change " + kind);
  if (!committed) {
    return R::failure("The " + kind + " was not changed: " + committed.message() +
                      " (the edits made to it could not be redone; its shape can still be edited directly)");
  }
  return memberInfo(id);
}

Result<Object> Modeler::memberObject(const MemberSpec& spec) {
  using R = Result<Object>;
  if (const Status valid = geom::validateSection(spec.section); !valid) return R::failure(valid.message());
  if (std::abs(spec.anchor.across) > 1 || std::abs(spec.anchor.up) > 1) return R::failure("Unknown section anchor");
  const gp_Vec along(spec.start, spec.end);
  if (!(along.Magnitude() >= tolerance::kMinDimension)) {
    return R::failure("A member needs two different, finite points");
  }
  auto up = spec.up ? geom::memberUp(gp_Dir(along), *spec.up) : geom::memberUp(gp_Dir(along));
  if (!up) return R::failure(up.error());
  LinearMember member{spec.start, spec.end, *up, spec.roll, spec.section, spec.anchor, spec.section.catalog};
  Object o;
  o.name = spec.name.empty() ? uniqueName(spec.role == "column" ? "Column" : spec.role == "beam" ? "Beam" : "Member")
                             : spec.name;
  o.recipe.base = member;
  // Meaning in the same step (no attribute pass afterwards).
  if (!spec.role.empty()) o.attributes["kind"] = spec.role;
  if (!spec.materialClass.empty()) o.attributes["material_class"] = spec.materialClass;
  return o;
}

Result<ObjectId> Modeler::createMember(const MemberSpec& spec) {
  auto o = memberObject(spec);
  if (!o) return Result<ObjectId>::failure(o.error());
  return addObject(*o, "Member");
}

Result<Modeler::MemberInfo> Modeler::memberInfo(const ObjectId& id) const {
  using R = Result<MemberInfo>;
  const Object* o = document_.find(id);
  if (!o) return R::failure("Object not found");
  const auto expanded = construction::frameMemberObject(document_, *o);
  if (expanded) o = &*expanded;
  const auto* m = std::get_if<LinearMember>(&o->recipe.base);
  if (!m) {
    if (auto other = wallOrSlabInfo(*o)) return *other;
    return R::failure("Not a member, wall or slab made by those tools (no basic sizes to edit; its shape can still be "
                      "edited directly)");
  }
  MemberInfo info;
  info.element = "member";
  info.id = id;
  info.section = geom::sectionName(m->section);
  info.catalog = m->section.catalog;
  info.origin = m->origin;
  info.kind = m->section.kind;
  info.anchor = m->anchor;
  info.edited = !o->recipe.modifiers.empty();
  info.role = attributeOr(*o, "kind");
  info.materialClass = attributeOr(*o, "material_class");
  const double parameterLength = m->start.Distance(m->end);
  if (info.edited) {
    // After other edits the member's own length is what it was made with;
    // what it is now is measured along its axis.
    const double now = extentAlong(o->shape, m->start, gp_Dir(gp_Vec(m->start, m->end)));
    info.fields.push_back({"length", "length along the member axis", "mm", now, "shape", true,
                           "measured on the edited shape; changing it moves the end (a column: its top)"});
  } else {
    info.fields.push_back({"length", "length along the member axis", "mm", parameterLength, "parameter", true, ""});
  }
  const std::string sectionNote =
      info.edited ? "section as made; the edits made since are redone on a changed section, or nothing changes" : "";
  const bool catalogOutline = m->section.kind == geom::Section::Kind::Profile;
  const std::string sizeNote = catalogOutline ? "the catalog section's overall size; change it by choosing another "
                                                "catalog section"
                                              : sectionNote;
  if (m->section.kind == geom::Section::Kind::Circle) {
    info.fields.push_back({"diameter", "section diameter", "mm", m->section.width, "parameter", true, sectionNote});
  } else {
    info.fields.push_back({"width", "section width (across)", "mm", m->section.width, "parameter", !catalogOutline, sizeNote});
    info.fields.push_back({"depth", "section depth (up)", "mm", m->section.depth, "parameter", !catalogOutline, sizeNote});
  }
  if (m->section.kind == geom::Section::Kind::H) {
    info.fields.push_back({"web", "web thickness", "mm", m->section.web, "parameter", true, sectionNote});
    info.fields.push_back({"flange", "flange thickness", "mm", m->section.flange, "parameter", true, sectionNote});
    info.fields.push_back({"radius", "fillet radius", "mm", m->section.radius, "parameter", true, sectionNote});
  }
  info.fields.push_back({"roll", "rotation about the axis", "deg", m->roll * 180.0 / M_PI, "parameter", true, ""});
  return info;
}

Result<Modeler::MemberInfo> Modeler::changeMember(const ObjectId& id, const MemberChange& change) {
  using R = Result<MemberInfo>;
  const Object* current = document_.find(id);
  if (!current) return R::failure("Object not found");
  const auto expanded = construction::frameMemberObject(document_, *current);
  if (expanded) current = &*expanded;
  if (!std::holds_alternative<LinearMember>(current->recipe.base)) {
    if (wallOrSlabInfo(*current)) return changeWallOrSlab(id, change);
    return R::failure("Not a member, wall or slab made by those tools (no basic sizes to edit; its shape can still be "
                      "edited directly)");
  }
  if (change.thickness || change.height || change.keep) {
    return R::failure("A member has no thickness or height: use width, depth and length (nothing was changed)");
  }
  Object object = *current;
  auto& m = std::get<LinearMember>(object.recipe.base);
  if (change.section) {
    if (const Status valid = geom::validateSection(*change.section); !valid) return R::failure(valid.message());
    m.section = *change.section;
    m.origin = change.section->catalog;
  }
  if (change.catalog) {
    const geom::CatalogSection* entry = geom::findSection(*change.catalog);
    if (!entry) return R::failure("No section '" + *change.catalog + "' in the catalog (nothing was changed)");
    m.section = entry->section;
    m.origin = entry->id;
  }
  // The section's own dimensions, all at once (one check of the result).
  geom::Section section = m.section;
  const bool h = section.kind == geom::Section::Kind::H;
  bool own = false;
  auto set = [&](const std::optional<double>& v, double& field, bool applies, const char* name) -> Status {
    if (!v) return Status::success();
    if (!applies) return Status::failure(std::string("A ") + geom::sectionName(section) + " section has no " + name);
    if (std::abs(field - *v) > 1e-12) own = true;
    field = *v;
    return Status::success();
  };
  const bool round = section.kind == geom::Section::Kind::Circle;
  const bool outline = section.kind == geom::Section::Kind::Profile;
  if (outline && (change.width || change.depth || change.web || change.flange || change.radius)) {
    return R::failure("A catalog section (" + geom::sectionName(section) +
                      ") changes size by choosing another catalog section; nothing was changed");
  }
  if (change.diameter) {
    if (!round) return R::failure("Only a round section has a diameter");
    if (std::abs(section.width - *change.diameter) > 1e-12) own = true;
    section.width = section.depth = *change.diameter;
  }
  for (const Status& s : {set(change.width, section.width, !round, "width"), set(change.depth, section.depth, !round, "depth"),
                          set(change.web, section.web, h, "web"), set(change.flange, section.flange, h, "flange"),
                          set(change.radius, section.radius, h, "fillet radius")}) {
    if (!s) return R::failure(s.message());
  }
  if (own) section.catalog.clear();
  if (const Status valid = geom::validateSection(section); !valid) return R::failure(valid.message());
  m.section = section;
  if (change.roll) {
    if (!std::isfinite(*change.roll)) return R::failure("The rotation must be finite");
    m.roll = *change.roll;
  }
  if (change.anchor) {
    if (std::abs(change.anchor->across) > 1 || std::abs(change.anchor->up) > 1) return R::failure("Unknown section anchor");
    // The member stays; its axis line moves to the new anchor.
    auto frame = geom::memberFrame(m.start, m.end, m.up, m.roll);
    if (!frame) return R::failure(frame.error());
    const auto [u0, v0] = geom::anchorOffset(m.section, m.anchor);
    const auto [u1, v1] = geom::anchorOffset(m.section, *change.anchor);
    const gp_Vec shift = gp_Vec(frame->XDirection()) * (u1 - u0) + gp_Vec(frame->YDirection()) * (v1 - v0);
    m.start.Translate(shift);
    m.end.Translate(shift);
    m.anchor = *change.anchor;
  }
  if (change.length) {
    double target = *change.length;
    if (!object.recipe.modifiers.empty()) {
      // Edited since: the measured length is what was asked about.
      const double now = extentAlong(current->shape, std::get<LinearMember>(current->recipe.base).start,
                                     gp_Dir(gp_Vec(m.start, m.end)));
      target = m.start.Distance(m.end) + (*change.length - now);
    }
    if (const Status s = ::sbr::doc::setParameter(object.recipe, "length", target); !s) return R::failure(s.message());
  }
  const Status committed = commitModified(std::move(object), "Change Member");
  if (!committed) {
    return R::failure("The member was not changed: " + committed.message() +
                      " (the edits made to it could not be redone; its shape can still be edited directly)");
  }
  return memberInfo(id);
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
  // docs/193: made as it is (as the walls): a loose face where it starts keeps no hole of it.
  Object profile;
  profile.name = uniqueName("Slab");
  profile.recipe.base = PolygonProfile{pts};
  auto id = addObject(std::move(profile), "Face");
  if (!id || !extrude(*id, thickness) || !setAttribute({*id}, "kind", "slab")) {
    history_.abortGroup();
    return Result<ObjectId>::failure(id ? "Slab could not be extruded" : id.error());
  }
  // Its top at a level's height: it lies on that level (docs/90 G-02).
  if (const FloorLevel* level = levelAtHeight(document_.settings().levels, top))
    setAttribute({*id}, kTopLevel, level->id);
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

Result<ObjectId> Modeler::withFitting(const ObjectId& hostId, const std::string& fitting,
                                      const std::function<Result<ObjectId>()>& cut) {
  using R = Result<ObjectId>;
  const auto checked = draw::normalizeFitting(fitting);
  if (!checked) return R::failure(checked.status().message());
  const bool none = nlohmann::json::parse(*checked).at("type") == "none";
  if (none) return cut();
  const auto before = openings(hostId);
  const std::size_t had = before ? before->size() : 0;
  history_.beginGroup("Opening");
  const auto made = cut();
  if (!made) {
    history_.abortGroup();
    return made;
  }
  const auto after = openings(*made);
  std::vector<std::size_t> fresh;
  if (after)
    for (std::size_t i = had + 1; i <= after->size(); ++i) fresh.push_back(i);
  if (fresh.empty()) {
    history_.endGroup();
    return made;
  }
  const Status fitted = setOpeningFittings(*made, fresh, *checked);
  if (!fitted) {
    history_.abortGroup();
    return R::failure(fitted.message());
  }
  history_.endGroup();
  return made;
}

Result<ObjectId> Modeler::placeOpeningAt(const ObjectId& hostId, const gp_Dir& faceNormal, const gp_Pnt& at,
                                         double width, double height, double sill, const std::string& fitting) {
  return withFitting(hostId, fitting, [&] { return placeOpeningAt(hostId, faceNormal, at, width, height, sill); });
}

Result<ObjectId> Modeler::placeOpenings(const ObjectId& hostId, const gp_Dir& faceNormal, int count, double width,
                                        double height, double sill, std::optional<double> spacing,
                                        const std::string& fitting) {
  return withFitting(hostId, fitting,
                     [&] { return placeOpenings(hostId, faceNormal, count, width, height, sill, spacing); });
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
  // Openings already there get ids first (and their fittings move to them), so the numbers
  // they had are not needed again.
  Object hostObject = *host;
  ensureOpeningIds(hostObject);
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
    feature.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(std::move(tool)), toParent, name,
                                              "opening-" + generateObjectId()});
  }
  // The wall stays the same object (its id, tags, what follows it): it only gains openings.
  Object result = hostObject;
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
  changes.modify(result);
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
  // What the copies would make in all (each copy repeats the whole content
  // of groups), counted before anything is made; an enclosing step (an AI
  // batch, edit_each) shares one budget with its earlier commands.
  std::size_t perCopy = 0;
  for (const ObjectId& id : roots) perCopy += document_.subtree(id).size();
  const std::size_t spent = history_.groupObjectChanges();
  const std::size_t room = spent < kMaxObjectsPerStep ? kMaxObjectsPerStep - spent : 0;
  if (perCopy * static_cast<std::size_t>(count) > room) {
    return Result<std::vector<ObjectId>>::failure(
        "The copies would make " + std::to_string(perCopy * static_cast<std::size_t>(count)) + " objects (" +
        std::to_string(perCopy) + " per copy x " + std::to_string(count) + "), more than the " +
        std::to_string(room) + " one step may still make; nothing was changed");
  }

  ChangeSet changes(document_);
  std::vector<ObjectId> created;
  std::vector<std::pair<std::map<ObjectId, ObjectId>, std::vector<ObjectId>>> copies;  // per copy k
  gp_Trsf step;  // worldDelta^k
  for (int k = 1; k <= count; ++k) {
    step = worldDelta * step;
    // A copy landing back on the original (a polar array going full
    // circle, e.g. 30 degrees x 12) is not made: it would only double it.
    if (landsOnItself(step)) continue;
    copies.emplace_back();
    for (const ObjectId& id : roots) {
      std::vector<Object> clones = cloneSubtree(id, document_.find(id)->parent, &copies.back().first);
      Object& root = clones.front();
      root.transform = localDelta(document_.parentTransform(root), step) * root.transform;
      created.push_back(root.id);
      copies.back().second.push_back(root.id);
      for (const Object& c : clones) changes.add(c);
    }
  }
  history_.beginGroup(count == 1 ? "Copy" : "Array");
  history_.execute(changes.build(count == 1 ? "Copy" : "Array"));
  for (const auto& [copyOf, made] : copies) refollowCopies(copyOf, made);
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
  refollowCopies(mapping, created);
  // Pasted together they are as they were copied (settled already): only
  // what is there already can overlap them. Nothing there near them: no
  // settling (pasting thousands far off checked each against everything).
  Bnd_Box pastedBox;
  std::unordered_set<ObjectId> pasted;
  for (const ObjectId& id : created) {
    for (const ObjectId& part : document_.subtree(id)) {
      pasted.insert(part);
      if (const Object* o = document_.find(part)) pastedBox.Add(document_.worldBounds(*o));
    }
  }
  pastedBox.Enlarge(tolerance::kMinDimension);
  bool near = false;
  for (const Object& o : document_.objects()) {
    if (o.parent != activeGroup_ || pasted.count(o.id) || o.shape.IsNull()) continue;
    if (!document_.worldBounds(o).IsOut(pastedBox)) {
      near = true;
      break;
    }
  }
  if (!near) {
    history_.endGroup();
    return created;
  }
  auto settled = settleOverlaps(created);
  if (!settled) {
    history_.abortGroup();
    return Result<std::vector<ObjectId>>::failure(settled.error());
  }
  history_.endGroup();
  return settledIds(created, *settled);
}

Status Modeler::setPosition(const ObjectId& id, const gp_Vec& translation) {
  const Object* current = document_.find(id);
  if (!current) return Status::failure("Object not found");
  Object o = *current;
  o.transform.SetTranslationPart(translation);
  ChangeSet changes(document_);
  changes.modify(o);
  if (const Status settled = settleMoves(document_, changes, document_.regenContext()); !settled) return settled;
  history_.execute(changes.build("Set Position"));
  return Status::success();
}

// --- following (docs/42) ------------------------------------------------------

Status Modeler::setFollow(const std::vector<ObjectId>& ids, const std::optional<ObjectId>& host) {
  if (ids.empty()) return Status::failure("Nothing selected");
  const Object* h = host ? document_.find(*host) : nullptr;
  if (host && !h) return Status::failure("The object to follow was not found");
  ChangeSet changes(document_);
  for (const ObjectId& id : ids) {
    const Object* current = document_.find(id);
    if (!current) return Status::failure("Object not found");
    Object o = *current;
    if (!host) {
      if (!o.follow) continue;
      o.follow.reset();
      changes.modify(o);
      continue;
    }
    if (id == *host) return Status::failure("An object cannot follow itself");
    // Inside the host (or holding it): it moves with it already.
    for (const ObjectId& part : document_.subtree(id)) {
      if (part == *host) return Status::failure("'" + o.name + "' contains the object to follow");
    }
    for (const ObjectId& part : document_.subtree(*host)) {
      if (part == id) return Status::failure("'" + o.name + "' is inside the object to follow; it moves with it already");
    }
    // Following round in a circle: refused, both stay as they are.
    std::set<ObjectId> seen{id};
    for (const Object* next = h; next && next->follow;) {
      if (!seen.insert(next->follow->host).second || next->follow->host == id) {
        return Status::failure("'" + o.name + "' would end up following itself (through '" + next->name + "')");
      }
      next = document_.find(next->follow->host);
    }
    o.follow = Follow{*host, document_.worldTransform(*h).Inverted() * document_.worldTransform(o), false};
    changes.modify(o);
  }
  history_.execute(changes.build(host ? "Follow" : "Stop Following"));
  return Status::success();
}

Modeler::FollowState Modeler::followState(const ObjectId& id) const {
  const Object* o = document_.find(id);
  if (!o || !o->follow) return FollowState::None;
  return document_.find(o->follow->host) ? FollowState::Active : FollowState::Suspended;
}

// Copies and pasted objects that follow something: their copy keeps
// following - the copied host when it was copied with them, otherwise the
// same host from where the copy was put.
void Modeler::refollowCopies(const std::map<ObjectId, ObjectId>& copyOf, const std::vector<ObjectId>& created) {
  ChangeSet changes(document_);
  bool any = false;
  for (const ObjectId& root : created) {
    for (const ObjectId& id : document_.subtree(root)) {
      const Object* c = document_.find(id);
      if (!c || (!c->follow && !c->gridPin)) continue;
      Object o = *c;
      // Pinned to a grid copied with it: to the copy. Copied alone: the
      // same grid (docs/43).
      bool touched = false;
      if (o.gridPin) {
        if (const auto it = copyOf.find(o.gridPin->grid); it != copyOf.end()) {
          o.gridPin->grid = it->second;
          touched = true;
        }
      }
      if (o.follow) {
        touched = true;
        if (const auto it = copyOf.find(o.follow->host); it != copyOf.end()) {
          o.follow->host = it->second;
        } else if (const Object* h = document_.find(o.follow->host)) {
          o.follow->offset = document_.worldTransform(*h).Inverted() * document_.worldTransform(o);
          o.follow->rebase = false;
        } else {
          o.follow->rebase = true;
        }
      }
      if (!touched) continue;
      changes.modify(o);
      any = true;
    }
  }
  if (any) history_.execute(changes.build("Follow"));
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

Status Modeler::saveCopy(const std::string& path) const {
  const std::filesystem::path p = std::filesystem::u8path(path);
  std::string extension = p.extension().u8string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return std::tolower(c); });
  if (extension != ".sbr") return Status::failure("A copy is saved as a .sbr file");
  if (!p.is_absolute()) return Status::failure("Give the copy's full path");
  std::error_code error;
  if (std::filesystem::exists(p, error)) return Status::failure("A file is there already: a copy never replaces one");
  if (!std::filesystem::is_directory(p.parent_path(), error)) return Status::failure("The folder for the copy does not exist");
  return saveDocument(document_, path);
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

Status Modeler::exportGltf(const std::string& path, const std::vector<ObjectId>& ids,
                           const geom::GltfExportOptions& options) const {
  for (const ObjectId& id : ids) {
    if (!document_.find(id)) return Status::failure("Object not found");
  }
  std::vector<ObjectId> wanted;
  if (ids.empty()) {
    for (const Object& o : document_.objects()) wanted.push_back(o.id);
  } else {
    for (const ObjectId& root : topmost(ids)) {
      for (const ObjectId& id : document_.subtree(root)) wanted.push_back(id);
    }
  }
  std::vector<geom::GltfPart> parts;
  for (const ObjectId& id : wanted) {
    const Object* o = document_.find(id);
    if (!o || o->shape.IsNull() || !document_.isVisible(id)) continue;
    // Only what has faces: lines, dimensions and guides do not render.
    if (o->kind() == ObjectKind::Annotation || o->kind() == ObjectKind::Guide || o->kind() == ObjectKind::Curve) continue;
    geom::GltfPart part;
    part.shape = document_.worldShape(*o);
    part.name = o->name;
    const Color c = document_.displayColor(*o);  // per-face materials: not yet
    part.color = {c.r, c.g, c.b, document_.displayOpacity(*o)};
    if (const Material* m = document_.findMaterial(document_.inheritedMaterial(*o))) {
      part.materialName = m->name;
      part.metallic = m->metallic;
      part.roughness = m->roughness;
    }
    parts.push_back(part);
  }
  if (parts.empty()) return Status::failure("Nothing to export");
  return geom::writeGltf(parts, path, options);
}

Status Modeler::export3dm(const std::string& path, const std::vector<ObjectId>& ids) const {
  const auto parts = exportParts(ids);
  if (parts.empty()) return Status::failure("Nothing to export");
  return geom::write3dm(parts, path);
}

Status Modeler::exportCollada(const std::string& path, const std::vector<ObjectId>& ids,
                              const geom::ColladaExportOptions& options) const {
  const auto parts = colouredParts(ids);
  if (!parts) return parts.status();
  return geom::writeCollada(*parts, path, options);
}

Status Modeler::exportSkp(const std::string& path, const std::vector<ObjectId>& ids, const geom::SkpExportOptions& options) const {
  const auto parts = colouredParts(ids);
  if (!parts) return parts.status();
  return geom::writeSkp(*parts, path, options);
}

Result<std::vector<geom::ColladaPart>> Modeler::colouredParts(const std::vector<ObjectId>& ids) const {
  using R = Result<std::vector<geom::ColladaPart>>;
  for (const ObjectId& id : ids) {
    if (!document_.find(id)) return R::failure("Object not found");
  }
  std::vector<ObjectId> wanted;
  if (ids.empty()) {
    for (const Object& o : document_.objects()) wanted.push_back(o.id);
  } else {
    for (const ObjectId& root : topmost(ids)) {
      for (const ObjectId& id : document_.subtree(root)) wanted.push_back(id);
    }
  }
  auto colorOf = [&](const std::string& materialId, const Color& fallback) {
    if (const Material* m = document_.findMaterial(materialId)) return geom::ColladaColor{m->color.r, m->color.g, m->color.b, m->opacity};
    return geom::ColladaColor{fallback.r, fallback.g, fallback.b, 1.0};
  };
  std::vector<geom::ColladaPart> parts;
  for (const ObjectId& id : wanted) {
    const Object* o = document_.find(id);
    if (!o || o->shape.IsNull() || !document_.isVisible(id) || o->kind() == ObjectKind::Annotation ||
        o->kind() == ObjectKind::Guide || o->kind() == ObjectKind::Image) {
      continue;
    }
    geom::ColladaPart part;
    part.shape = document_.worldShape(*o);
    part.name = o->name;
    // docs/183: its tag and information (for SketchUp; COLLADA leaves them out).
    part.layer = o->layer;
    part.attributes = o->attributes;
    part.attributes["sbr_id"] = o->id;
    part.color = colorOf(document_.inheritedMaterial(*o), document_.displayColor(*o));
    std::map<int, geom::ColladaColor> faceColors;
    if (const auto* instance = std::get_if<ComponentInstance>(&o->recipe.base)) {
      const auto scale = componentUniformScale(*instance);
      const ComponentDefinition* d = document_.findDefinition(instance->definitionId);
      if (scale && d) {
        if (o->faceMaterials.empty()) {
          part.definition = d->id;
          part.definitionName = d->name;
          part.localShape = d->shape;
          part.placement = document_.worldTransform(*o) * *scale;
        }

        for (const auto& [index, material] : componentFaceMaterials(*d, o->shape))
          faceColors[index] = colorOf(material, o->color);
      }
    }
    for (const auto& [index, material] : resolveFaceMaterials(*o).faces)
      faceColors[index] = colorOf(material, document_.displayColor(*o));
    part.faceColors.assign(faceColors.begin(), faceColors.end());
    parts.push_back(std::move(part));
  }
  if (parts.empty()) return R::failure("Nothing to export");
  return parts;
}

Result<std::vector<ObjectId>> Modeler::importCollada(const std::string& path, geom::ColladaReport* report) {
  auto parts = geom::readCollada(path, report);
  if (!parts) return Result<std::vector<ObjectId>>::failure(parts.error());
  return addColouredParts(*parts, path, "Import COLLADA");
}

Result<std::vector<ObjectId>> Modeler::importSkp(const std::string& path, geom::SkpReport* report) {
  auto parts = geom::readSkp(path, report);
  if (!parts) return Result<std::vector<ObjectId>>::failure(parts.error());
  return addColouredParts(*parts, path, "Import SketchUp");
}

namespace {
// A placement written by writeSkp (the 3 x 4 matrix row by row); none when unreadable.
std::optional<gp_Trsf> placementFrom(const std::string& text) {
  std::istringstream in(text);
  double v[12];
  for (double& x : v)
    if (!(in >> x)) return std::nullopt;
  try {
    gp_Trsf t;
    t.SetValues(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
    return t;
  } catch (...) {
    return std::nullopt;
  }
}

bool nearlySame(const gp_Trsf& a, const gp_Trsf& b) {
  for (int r = 1; r <= 3; ++r) {
    for (int c = 1; c <= 3; ++c)
      if (std::abs(a.Value(r, c) - b.Value(r, c)) > 1e-7) return false;
    if (std::abs(a.Value(r, 4) - b.Value(r, 4)) > 0.01) return false;
  }
  return true;
}

// The same shape, as far as a mesh read back can tell: the same box (1 mm) and, for solids,
// the same volume (2 %: curved faces come back as facets).
bool sameShape(const TopoDS_Shape& a, const TopoDS_Shape& b) {
  const Bnd_Box ba = geom::exactBounds(a), bb = geom::exactBounds(b);
  if (ba.IsVoid() || bb.IsVoid()) return ba.IsVoid() == bb.IsVoid();
  double a0, a1, a2, a3, a4, a5, b0, b1, b2, b3, b4, b5;
  ba.Get(a0, a1, a2, a3, a4, a5);
  bb.Get(b0, b1, b2, b3, b4, b5);
  const double d[6] = {a0 - b0, a1 - b1, a2 - b2, a3 - b3, a4 - b4, a5 - b5};
  for (const double x : d)
    if (std::abs(x) > 1.0) return false;
  const double va = geom::volume(a), vb = geom::volume(b);
  if (std::abs(va) > 1.0 || std::abs(vb) > 1.0)
    if (std::abs(va - vb) > 0.02 * std::max(std::abs(va), std::abs(vb))) return false;
  return true;
}
}  // namespace

Result<Modeler::SkpSync> Modeler::syncSkp(const std::string& path, bool replaceEdited, bool removeMissing) {
  using R = Result<SkpSync>;
  SkpSync out;
  auto parts = geom::readSkp(path, &out.report);
  if (!parts) return R::failure(parts.error());
  // First decide everything against the model as it is; then change it.
  std::vector<geom::ColladaPart> fresh;
  std::vector<std::pair<gp_Trsf, std::vector<ObjectId>>> moves;  // objects moved alike, together
  std::vector<std::pair<ObjectId, const geom::ColladaPart*>> replacements;
  std::set<ObjectId> seen;
  for (const geom::ColladaPart& part : *parts) {
    const auto source = part.attributes.find("sbr_id");
    const Object* o = source == part.attributes.end() ? nullptr : document_.find(source->second);
    if (!o || seen.count(o->id)) {
      fresh.push_back(part);
      continue;
    }
    seen.insert(o->id);
    if (o->locked) {
      out.locked.push_back(o->id);
      continue;
    }
    gp_Trsf delta;
    const auto written = part.attributes.find("sbr_placement");
    const auto was = written == part.attributes.end() ? std::nullopt : placementFrom(written->second);
    if (part.placed && was) delta = part.placement * was->Inverted();
    // Not turned (only the rounding of SketchUp's inches): a plain move, so that joined walls
    // follow as they do for a move (they do not for a turn).
    {
      bool turned = false;
      for (int r = 1; r <= 3 && !turned; ++r)
        for (int c = 1; c <= 3; ++c)
          if (std::abs(delta.Value(r, c) - (r == c ? 1.0 : 0.0)) > 1e-7) turned = true;
      if (!turned) {
        const gp_XYZ shift = delta.TranslationPart();
        delta = gp_Trsf();
        delta.SetTranslation(gp_Vec(shift));
      }
    }
    // A different scale is an edit. Only a rigid delta can update the existing placement.
    const bool rigidDelta = std::abs(delta.ScaleFactor() - 1.0) < 1e-7;
    const TopoDS_Shape back = rigidDelta ? BRepBuilderAPI_Transform(part.shape, delta.Inverted(), Standard_True).Shape()
                                         : TopoDS_Shape();
    if (!rigidDelta || !sameShape(back, document_.worldShape(*o))) {
      if (replaceEdited) replacements.emplace_back(o->id, &part);
      else out.edited.push_back(o->id);
      continue;
    }
    if (nearlySame(delta, gp_Trsf())) {
      out.unchanged.push_back(o->id);
      continue;
    }
    auto group = std::find_if(moves.begin(), moves.end(), [&](const auto& m) { return nearlySame(m.first, delta); });
    if (group == moves.end()) moves.emplace_back(delta, std::vector<ObjectId>{o->id});
    else group->second.push_back(o->id);
  }
  for (const std::string& id : out.report.exportedIds)
    if (!seen.count(id) && document_.find(id)) out.missing.push_back(id);

  history_.beginGroup("Update from SketchUp");
  for (const auto& [delta, ids] : moves) {
    if (const Status moved = transform(ids, delta, "Move (SketchUp)"); !moved) {
      history_.abortGroup();
      return R::failure(moved.message());
    }
    out.moved.insert(out.moved.end(), ids.begin(), ids.end());
  }
  if (!replacements.empty()) {
    ChangeSet changes(document_);
    for (const auto& [id, part] : replacements) {
      Object o = *document_.find(id);
      // The shape from SketchUp in the object's own frame; what it was kept as information.
      const gp_Trsf toLocal = document_.worldTransform(o).Inverted();
      nlohmann::json carried = nlohmann::json::object();
      for (const auto& [key, value] : o.attributes) carried[key] = value;
      o.attributes.clear();
      o.attributes["skp_attributes"] = carried.dump();
      o.attributes["skp_source_id"] = id;
      o.recipe = Recipe{};
      o.recipe.base = ImportedShape{BRepBuilderAPI_Transform(part->shape, toLocal, Standard_True).Shape(), path};
      o.faceMaterials.clear();
      o.hiddenEdges.clear();
      o.gridPin.reset();
      o.follow.reset();
      if (const Status built = rebuild(o); !built) {
        history_.abortGroup();
        return R::failure(built.message());
      }
      changes.modify(o);
      out.replaced.push_back(id);
    }
    history_.execute(changes.build("Replace From SketchUp"));
  }
  if (removeMissing && !out.missing.empty()) {
    if (const Status gone = remove(out.missing); !gone) {
      history_.abortGroup();
      return R::failure(gone.message());
    }
    out.removed = std::move(out.missing);
    out.missing.clear();
  }
  if (!fresh.empty()) {
    const auto added = addColouredParts(fresh, path, "Import SketchUp");
    if (!added) {
      history_.abortGroup();
      return R::failure(added.error());
    }
    out.added = *added;
  }
  history_.endGroup();
  return out;
}

Result<std::vector<ObjectId>> Modeler::addColouredParts(const std::vector<geom::ColladaPart>& parts, const std::string& path,
                                                        const std::string& commandName) {
  using R = Result<std::vector<ObjectId>>;
  ChangeSet changes(document_);
  std::vector<ObjectId> ids;
  std::map<std::string, ComponentDefinition> definitions;
  for (const geom::ColladaPart& part : parts) {
    Object o;
    o.id = generateObjectId();
    o.name = part.name.empty() ? uniqueName("Imported") : part.name;
    // docs/183: the tag it came with; the information it carried kept as it was (not as the
    // object's own: the imported shape is not the wall, roof... its tool made), and the id of
    // the object it was written from.
    if (!part.layer.empty()) o.layer = part.layer;
    if (!part.attributes.empty()) {
      nlohmann::json carried = nlohmann::json::object();
      for (const auto& [key, value] : part.attributes) carried[key] = value;
      o.attributes["skp_attributes"] = carried.dump();
      const auto source = part.attributes.find("sbr_id");
      if (source != part.attributes.end()) o.attributes["skp_source_id"] = source->second;
    }
    o.color = Color{part.color.r, part.color.g, part.color.b};
    if (TopExp_Explorer(part.shape, TopAbs_FACE).More()) o.material = changes.colorMaterialFor(o.color);
    auto applyFaceColors = [&](Object& target, const TopoDS_Shape& shape) {
      if (part.faceColors.empty()) return;
      TopTools_IndexedMapOfShape faces;
      TopExp::MapShapes(shape, TopAbs_FACE, faces);
      for (const auto& [index, color] : part.faceColors) {
        if (index < 1 || index > faces.Extent()) continue;
        if (auto fm = FaceMaterial::of(TopoDS::Face(faces(index)),
                                      changes.colorMaterialFor(Color{color.r, color.g, color.b}))) {
          target.faceMaterials.push_back(std::move(*fm));
        }
      }
    };
    if (!part.definition.empty() && !part.localShape.IsNull()) {
      auto entry = definitions.find(part.definition);
      if (entry == definitions.end()) {
        Object leaf;
        leaf.id = generateObjectId();
        leaf.name = part.definitionName;
        leaf.color = o.color;
        leaf.material = o.material;
        leaf.recipe.base = ImportedShape{part.localShape, path};
        if (const Status built = rebuild(leaf); !built) return R::failure(built.message());
        applyFaceColors(leaf, leaf.shape);

        ComponentDefinition definition;
        definition.id = generateObjectId();
        definition.name = part.definitionName;
        definition.objects.push_back(std::move(leaf));
        const auto shape = buildDefinitionShape(definition);
        if (!shape) return R::failure(shape.error());
        definition.shape = *shape;
        changes.putDefinition(definition);
        entry = definitions.emplace(part.definition, std::move(definition)).first;
      }
      const ComponentDefinition& definition = entry->second;
      ComponentInstance instance{definition.id, {}, false};
      o.transform = part.placement;
      const double scale = o.transform.ScaleFactor();
      if (std::abs(scale - 1.0) >= 1e-9) {
        instance.stretches.push_back(StretchOp{gp::Origin(), gp::DX(), gp::DY(), scale, scale, scale});
        o.transform.SetScaleFactor(1.0);
      }
      const auto shape = buildInstanceShape(definition, instance);
      if (!shape) return R::failure(shape.error());
      o.shape = *shape;
      o.recipe.base = std::move(instance);
    } else {
      o.recipe.base = ImportedShape{part.shape, path};
      if (const Status built = rebuild(o); !built) return R::failure(built.message());
      applyFaceColors(o, o.shape);
    }
    ids.push_back(o.id);
    changes.add(o);
  }
  history_.execute(changes.build(commandName));
  return ids;
}

Result<TopoDS_Shape> Modeler::exportGeometry(const std::vector<ObjectId>& ids, bool facesOnly) const {
  for (const auto& id : ids) if (!document_.find(id)) return Result<TopoDS_Shape>::failure("Object not found");
  std::vector<ObjectId> wanted;
  if (ids.empty()) {
    for (const auto& o : document_.objects()) wanted.push_back(o.id);
  } else {
    for (const auto& root : topmost(ids)) {
      const auto children = document_.subtree(root);
      wanted.insert(wanted.end(), children.begin(), children.end());
    }
  }
  BRep_Builder builder;
  TopoDS_Compound shape; builder.MakeCompound(shape);
  for (const auto& id : wanted) {
    const auto* o = document_.find(id);
    if (!o || o->shape.IsNull() || !document_.isVisible(id) || o->kind()==ObjectKind::Image ||
        o->kind()==ObjectKind::Annotation || o->kind()==ObjectKind::Guide) continue;
    const auto part = document_.worldShape(*o);
    if (!facesOnly || TopExp_Explorer(part, TopAbs_FACE).More()) {
      // Adding a child freezes its TShape; never freeze a canonical object.
      builder.Add(shape, BRepBuilderAPI_Copy(part, Standard_False, Standard_False).Shape());
    }
  }
  return TopoDS_Shape(shape);
}

Status Modeler::exportStl(const std::string& path, const std::vector<ObjectId>& ids,
                         const geom::StlExportOptions& options) const {
  auto shape=exportGeometry(ids,true);
  return shape ? geom::writeStl(*shape,path,options) : shape.status();
}

Status Modeler::exportBrep(const std::string& path, const std::vector<ObjectId>& ids) const {
  auto shape=exportGeometry(ids);
  return shape ? geom::writeBrep(*shape,path) : shape.status();
}

Status Modeler::exportIges(const std::string& path, const std::vector<ObjectId>& ids) const {
  auto shape=exportGeometry(ids);
  return shape ? geom::writeIges(*shape,path) : shape.status();
}

Result<std::vector<ObjectId>> Modeler::importBrep(const std::string& path) {
  auto shape=geom::readBrep(path);
  if(!shape)return Result<std::vector<ObjectId>>::failure(shape.error());
  return addImported({{*shape,"BRep",std::nullopt,{}}},path,"Import BRep");
}

Result<std::vector<ObjectId>> Modeler::importIges(const std::string& path) {
  auto parts=geom::readIges(path);
  return parts ? addImported(*parts,path,"Import IGES") : Result<std::vector<ObjectId>>::failure(parts.error());
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

Result<std::vector<ObjectId>> Modeler::importDxf(const std::string& path, const geom::DxfImportOptions& options,
                                                geom::DxfImportReport* report) {
  geom::DxfImportReport local;
  auto& found = report ? *report : local;
  auto parts = geom::readDxf(path, options, &found);
  if (!parts) return Result<std::vector<ObjectId>>::failure(parts.error());
  if (found.incomplete() && !options.allowPartial)
    return Result<std::vector<ObjectId>>::failure("DXF contains unsupported entities; nothing imported. " + found.summary());
  return addImported(*parts, path, "Import DXF");
}

Result<std::vector<ObjectId>> Modeler::importJww(const std::string& path, const geom::JwwImportOptions& options,
                                                geom::JwwImportReport* report) {
  auto parts = geom::readJww(path, options, report);
  if (!parts) return Result<std::vector<ObjectId>>::failure(parts.error());
  return addImported(*parts, path, "Import JWW");
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
  std::map<std::string, int> nextSuffix;
  for (const geom::NamedShape& part : parts) {
    Object o;
    o.id = generateObjectId();
    if (!part.layer.empty()) o.layer = part.layer;
    std::string name = part.name.empty() ? uniqueName("Imported") : part.name;
    // Keep names unique within the batch as well.
    const std::string baseName = name;
    auto& suffix = nextSuffix[baseName];
    if (suffix < 2) suffix = 2;
    while (usedNames.count(name)) name = baseName + " (" + std::to_string(suffix++) + ")";
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

namespace {
// What a picked curve becomes in a recipe: the object's own recipe (whole
// curve), or a copy of one edge.
Result<BooleanOperand> curveOperand(const Document& d, const Modeler::CurvePick& pick) {
  using R = Result<BooleanOperand>;
  const Object* o = d.find(pick.id);
  if (!o) return R::failure("Curve not found");
  if (pick.edge) {
    Recipe r;
    r.base = ImportedShape{*pick.edge, "edge of " + o->name};
    return BooleanOperand{std::make_shared<const Recipe>(std::move(r)), gp_Trsf(), o->name + " (edge)"};
  }
  if (o->kind() != ObjectKind::Curve && o->kind() != ObjectKind::Face) {
    return R::failure("'" + o->name + "' is not a curve (pick a curve, or an edge of it)");
  }
  return BooleanOperand{std::make_shared<const Recipe>(o->recipe), d.worldTransform(*o), o->name};
}
}  // namespace

Result<ObjectId> Modeler::sweep2(const CurvePick& rail1, const CurvePick& rail2, const std::vector<CurvePick>& profiles) {
  using R = Result<ObjectId>;
  if (profiles.empty()) return R::failure("Sweep2 needs at least one profile");
  Sweep2Feature feature;
  std::vector<CurvePick> all{rail1, rail2};
  all.insert(all.end(), profiles.begin(), profiles.end());
  for (const CurvePick& p : all) {
    auto operand = curveOperand(document_, p);
    if (!operand) return R::failure(operand.error());
    feature.inputs.push_back(*operand);
  }
  Object o;
  o.name = uniqueName("Sweep2");
  o.recipe.base = std::move(feature);
  return addObject(std::move(o), "Sweep2");
}

Result<ObjectId> Modeler::surfaceFromCurves(const std::vector<CurvePick>& curves, bool patch) {
  using R = Result<ObjectId>;
  if (!patch && (curves.size() < 2 || curves.size() > 4)) return R::failure("Edge surface needs 2, 3 or 4 curves");
  if (curves.empty()) return R::failure("Patch needs a closed outline of curves");
  FillSurfaceFeature feature;
  feature.patch = patch;
  for (const CurvePick& p : curves) {
    auto operand = curveOperand(document_, p);
    if (!operand) return R::failure(operand.error());
    feature.curves.push_back(*operand);
  }
  Object o;
  o.name = uniqueName(patch ? "Patch" : "Edge Surface");
  o.recipe.base = std::move(feature);
  return addObject(std::move(o), patch ? "Patch" : "Edge Surface");
}

Result<std::string> Modeler::roundEdges(const ObjectId& id, const std::vector<TopoDS_Edge>& worldEdges, double size,
                                        bool chamfer) {
  using R = Result<std::string>;
  const Object* current = document_.find(id);
  if (!current) return R::failure("Object not found");
  if (current->locked) return R::failure("'" + current->name + "' is locked");
  if (current->kind() != ObjectKind::Solid) return R::failure(chamfer ? "Only edges of a solid can be chamfered" : "Only edges of a solid can be rounded");
  if (worldEdges.empty()) return R::failure("Choose the edges first");
  Object object = *current;
  const gp_Trsf toLocal = document_.worldTransform(object).Inverted();
  FilletOp op;
  op.distance = size;
  op.chamfer = chamfer;
  for (const TopoDS_Edge& e : worldEdges) {
    const auto [point, direction] = geom::edgeKey(TopoDS::Edge(e.Moved(TopLoc_Location(toLocal))));
    // An edge picked twice counts once.
    if (std::any_of(op.edges.begin(), op.edges.end(), [&](const EdgeRef& r) { return r.point.Distance(point) < 1e-6; })) continue;
    op.edges.push_back({point, direction});
  }
  object.recipe.modifiers.push_back(op);
  const std::string key = opKey(object.recipe.modifiers.size() - 1);
  if (const Status s = commitModified(std::move(object), chamfer ? "Chamfer" : "Fillet"); !s) return R::failure(s.message());
  return key;
}

Result<std::string> Modeler::roundCorners(const ObjectId& id, const std::vector<gp_Pnt>& worldCorners, double size,
                                          bool chamfer) {
  using R = Result<std::string>;
  const Object* current = document_.find(id);
  if (!current) return R::failure("Object not found");
  if (current->locked) return R::failure("'" + current->name + "' is locked");
  if (current->kind() != ObjectKind::Face) return R::failure("Only the corners of a flat face can be rounded here");
  if (worldCorners.empty()) return R::failure("Choose the corners first");
  Object object = *current;
  const gp_Trsf toLocal = document_.worldTransform(object).Inverted();
  FilletOp op;
  op.distance = size;
  op.chamfer = chamfer;
  for (const gp_Pnt& c : worldCorners) {
    const gp_Pnt local = c.Transformed(toLocal);
    if (std::any_of(op.corners.begin(), op.corners.end(), [&](const gp_Pnt& p) { return p.Distance(local) < 1e-6; })) continue;
    op.corners.push_back(local);
  }
  object.recipe.modifiers.push_back(op);
  const std::string key = opKey(object.recipe.modifiers.size() - 1);
  if (const Status s = commitModified(std::move(object), chamfer ? "Chamfer" : "Fillet"); !s) return R::failure(s.message());
  return key;
}

Result<std::optional<ObjectId>> Modeler::filletLines(const LinePick& a, const LinePick& b, double size, bool chamfer) {
  using R = Result<std::optional<ObjectId>>;
  if (!(size >= 0) || !std::isfinite(size)) return R::failure("The size must be 0 or more");
  struct Side {
    const LinePick* pick;
    Object object;
    std::vector<gp_Pnt> world;  // the line object's points
    gp_Pnt s0, s1;              // the segment
    gp_Dir kept;                // from the meeting point along the part kept
    std::size_t moved = 0;      // index of the point that moves
    double room = 0.0;          // length of the part kept, from the meeting point
  };
  std::vector<Side> sides;
  for (const LinePick* p : {&a, &b}) {
    const Object* o = document_.find(p->id);
    if (!o) return R::failure("Line not found");
    if (o->locked) return R::failure("'" + o->name + "' is locked");
    const auto* line = std::get_if<PolylineCurve>(&o->recipe.base);
    if (!line || !o->recipe.modifiers.empty()) return R::failure("Only straight lines can be joined this way");
    if (p->segment + 1 >= line->points.size()) return R::failure("No such piece of the line");
    Side side{p, *o, {}, {}, {}, gp::DX(), 0, 0.0};
    const gp_Trsf w = document_.worldTransform(*o);
    for (const gp_Pnt& q : line->points) side.world.push_back(q.Transformed(w));
    side.s0 = side.world[p->segment];
    side.s1 = side.world[p->segment + 1];
    if (side.s0.Distance(side.s1) < 1e-6) return R::failure("A line has no length");
    sides.push_back(std::move(side));
  }
  if (a.id == b.id && a.segment == b.segment) return R::failure("Pick two different lines");
  // Where the two lines (extended) meet.
  const gp_Vec u1(sides[0].s0, sides[0].s1), u2(sides[1].s0, sides[1].s1);
  const gp_Vec n = u1.Crossed(u2);
  if (n.Magnitude() < 1e-9 * u1.Magnitude() * u2.Magnitude()) return R::failure("The lines are parallel: they never meet");
  const gp_Vec w0(sides[1].s0, sides[0].s0);
  if (std::abs(w0.Dot(n.Normalized())) > 1e-3) return R::failure("The lines are not in one plane: they never meet");
  // s0 + u1 t = s0' + u2 s, in the plane.
  const double t = gp_Vec(sides[0].s0, sides[1].s0).Crossed(u2).Dot(n) / n.SquareMagnitude();
  const gp_Pnt meet = sides[0].s0.Translated(u1 * t);
  for (Side& side : sides) {
    gp_Vec u(side.s0, side.s1);
    u.Normalize();
    double along = gp_Vec(meet, side.pick->clicked).Dot(u);
    // Clicked right at the meeting point: keep the longer part.
    if (std::abs(along) < 1e-6) {
      along = std::abs(gp_Vec(meet, side.s0).Dot(u)) > std::abs(gp_Vec(meet, side.s1).Dot(u)) ? gp_Vec(meet, side.s0).Dot(u)
                                                                                          : gp_Vec(meet, side.s1).Dot(u);
    }
    side.kept = gp_Dir(along > 0 ? u : -u);
    const double d0 = gp_Vec(meet, side.s0).Dot(gp_Vec(side.kept)), d1 = gp_Vec(meet, side.s1).Dot(gp_Vec(side.kept));
    // The end nearer the meeting point moves; the far end stays.
    side.moved = d0 < d1 ? side.pick->segment : side.pick->segment + 1;
    side.room = std::max(d0, d1);
    if (side.room < 1e-6) return R::failure("The part kept would have no length (click the part to keep)");
  }
  const bool inside = a.id == b.id && sides[0].moved == sides[1].moved;  // a corner of one line object
  for (const Side& side : sides) {
    const std::size_t last = side.world.size() - 1;
    if (!inside && side.moved != 0 && side.moved != last) {
      return R::failure("A line can only be lengthened or shortened at a free end (this corner joins more lines)");
    }
  }
  if (inside && size <= 0) return R::failure("These lines already meet at a corner");
  // Where each line ends now, and what joins them.
  const double angle = gp_Vec(sides[0].kept).Angle(gp_Vec(sides[1].kept));
  if (angle < 1e-6 || angle > M_PI - 1e-6) return R::failure("The lines run straight on: nothing to round");
  double back = 0.0;  // from the meeting point along each line
  if (size > 0) back = chamfer ? size : size / std::tan(angle / 2);
  for (const Side& side : sides) {
    if (back > side.room - 1e-6) {
      char text[128];
      std::snprintf(text, sizeof text, "%s is too large for these lines (up to about %.0f mm)",
                    chamfer ? "The chamfer" : "The radius",
                    std::floor(chamfer ? std::min(sides[0].room, sides[1].room)
                                       : std::min(sides[0].room, sides[1].room) * std::tan(angle / 2)));
      return R::failure(text);
    }
  }
  const gp_Pnt t1 = meet.Translated(gp_Vec(sides[0].kept) * back);
  const gp_Pnt t2 = meet.Translated(gp_Vec(sides[1].kept) * back);
  history_.beginGroup(chamfer ? "Chamfer Lines" : "Fillet Lines");
  auto fail = [&](const std::string& why) {
    history_.abortGroup();
    return R::failure(why);
  };
  auto commitPoints = [&](Object object, const std::vector<gp_Pnt>& world) -> Status {
    const gp_Trsf toLocal = document_.worldTransform(object).Inverted();
    std::vector<gp_Pnt> local;
    for (const gp_Pnt& q : world) local.push_back(q.Transformed(toLocal));
    object.recipe.base = PolylineCurve{local};
    return commitModified(std::move(object), "Line");
  };
  std::vector<ObjectId> touched;
  if (inside) {
    // Split the line object at the corner: before it -> t, t' -> after it.
    const Side& s = sides[0];
    const std::size_t k = s.moved;
    // Which pick runs before the corner.
    const bool firstBefore = sides[0].pick->segment < sides[1].pick->segment;
    const gp_Pnt before = firstBefore ? t1 : t2, after = firstBefore ? t2 : t1;
    std::vector<gp_Pnt> head(s.world.begin(), s.world.begin() + k);
    head.push_back(before);
    std::vector<gp_Pnt> tail{after};
    tail.insert(tail.end(), s.world.begin() + k + 1, s.world.end());
    if (const Status st = commitPoints(s.object, head); !st) return fail(st.message());
    auto rest = createPolyline(tail);
    if (!rest) return fail(rest.error());
    touched = {s.object.id, *rest};
  } else {
    // Each free end moved to where the line now ends.
    std::vector<gp_Pnt> first = sides[0].world;
    first[sides[0].moved] = t1;
    if (a.id == b.id) {
      first[sides[1].moved] = t2;
      if (const Status st = commitPoints(sides[0].object, first); !st) return fail(st.message());
    } else {
      std::vector<gp_Pnt> second = sides[1].world;
      second[sides[1].moved] = t2;
      if (const Status st = commitPoints(sides[0].object, first); !st) return fail(st.message());
      if (const Status st = commitPoints(sides[1].object, second); !st) return fail(st.message());
    }
    touched = {sides[0].object.id};
    if (a.id != b.id) touched.push_back(sides[1].object.id);
  }
  std::optional<ObjectId> joint;
  if (back > 0) {
    if (chamfer) {
      auto cut = createPolyline({t1, t2});
      if (!cut) return fail(cut.error());
      joint = *cut;
    } else {
      // The arc t1 -> t2 tangent to both, its apex towards the meeting point.
      const gp_Dir normal(gp_Vec(sides[0].kept).Crossed(gp_Vec(sides[1].kept)));
      const double radius = size;
      const gp_Vec bis = (gp_Vec(sides[0].kept) + gp_Vec(sides[1].kept)).Normalized();
      const gp_Pnt centre = meet.Translated(bis * (radius / std::sin(angle / 2)));
      const gp_Pnt apex = centre.Translated(gp_Vec(centre, meet).Normalized() * radius);
      const gp_Pnt mid((t1.XYZ() + t2.XYZ()) / 2);
      const gp_Vec side = gp_Vec(normal).Crossed(gp_Vec(t1, t2)).Normalized();
      const double bulge = gp_Vec(mid, apex).Dot(side);
      auto arc = createArc(t1, t2, normal, bulge, false);
      if (!arc) return fail(arc.error());
      joint = *arc;
    }
  }
  history_.endGroup();
  // Closing an outline (as drawing it would): its face.
  if (joint) {
    bool loop = false;
    (void)faceFromLoop(*joint, &loop);
  } else {
    for (const ObjectId& id : touched) {
      bool loop = false;
      if (document_.find(id) && faceFromLoop(id, &loop)) break;
    }
  }
  return joint;
}

Result<std::string> Modeler::shellSolid(const ObjectId& id, const std::vector<TopoDS_Face>& worldFaces, double thickness) {
  using R = Result<std::string>;
  const Object* current = document_.find(id);
  if (!current) return R::failure("Object not found");
  if (current->locked) return R::failure("'" + current->name + "' is locked");
  if (current->kind() != ObjectKind::Solid) return R::failure("Only a solid can be shelled");
  Object object = *current;
  const gp_Trsf toLocal = document_.worldTransform(object).Inverted();
  ShellOp op;
  op.distance = thickness;
  for (const TopoDS_Face& f : worldFaces) {
    auto ref = geom::SurfaceRef::fromFace(TopoDS::Face(f.Moved(TopLoc_Location(toLocal))));
    if (!ref) return R::failure(ref.error());
    op.faces.push_back(*ref);
  }
  object.recipe.modifiers.push_back(op);
  const std::string key = opKey(object.recipe.modifiers.size() - 1);
  if (const Status s = commitModified(std::move(object), "Shell"); !s) return R::failure(s.message());
  return key;
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
