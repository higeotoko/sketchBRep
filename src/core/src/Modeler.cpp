#include "sbr/doc/Modeler.h"

#include <algorithm>
#include <cmath>

#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

#include "sbr/doc/NativeFormat.h"
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
    if (!taken) return candidate;
  }
}

Result<ObjectId> Modeler::addObject(Object object, const std::string& commandName) {
  if (object.id.empty()) object.id = generateObjectId();
  const Status built = rebuild(object);
  if (!built) return Result<ObjectId>::failure(built.message());
  const ObjectId id = object.id;
  history_.execute(Command{commandName, {ObjectDelta{id, std::nullopt, object, std::nullopt}}});
  return id;
}

Status Modeler::commitModified(Object object, const std::string& commandName) {
  const Object* current = document_.find(object.id);
  if (!current) return Status::failure("Object not found");
  // Regenerate the full recipe; on failure the document is left untouched.
  const Status built = rebuild(object);
  if (!built) return built;
  history_.execute(Command{commandName,
                           {ObjectDelta{object.id, *current, object, document_.indexOf(object.id)}}});
  return Status::success();
}

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

Result<PushPullResult> Modeler::pushPull(const ObjectId& id, const TopoDS_Face& worldFace,
                                         double distance) {
  const Object* current = document_.find(id);
  if (!current) return Result<PushPullResult>::failure("Object not found");
  if (!std::isfinite(distance) || std::abs(distance) < tolerance::kMinDimension) {
    return Result<PushPullResult>::failure("Distance must be non-zero");
  }
  Object object = *current;
  const TopoDS_Face face =
      TopoDS::Face(worldFace.Moved(TopLoc_Location(object.transform.Inverted())));
  const auto pickedNormal = geom::planarFaceNormal(face);
  if (!pickedNormal) return Result<PushPullResult>::failure("Only planar faces can be pushed/pulled");

  auto& mods = object.recipe.modifiers;
  std::string key;

  if (mods.empty() && object.kind() == ObjectKind::Face) {
    // Profile face -> parametric extrusion.
    const auto profileNormal = geom::planarFaceNormal(TopoDS::Face(object.shape));
    if (!profileNormal) return Result<PushPullResult>::failure("Profile is not planar");
    const double sign = profileNormal->Dot(*pickedNormal) >= 0 ? 1.0 : -1.0;
    mods.push_back(ExtrudeOp{distance * sign});
    key = opKey(0);
  } else if (mods.size() == 1 && std::holds_alternative<ExtrudeOp>(mods[0])) {
    // Pulling the cap of a plain extrusion edits the extrusion distance.
    auto& extrude = std::get<ExtrudeOp>(mods[0]);
    Recipe baseOnly{object.recipe.base, {}};
    const auto profile = regenerate(baseOnly);
    gp_Pnt profileCentroid, pickedCentroid;
    std::optional<gp_Dir> profileNormal;
    if (profile && profile->ShapeType() == TopAbs_FACE) {
      profileNormal = geom::planarFaceNormal(TopoDS::Face(profile.value()));
    }
    bool isCap = false;
    if (profileNormal && faceCentroid(profile.value(), profileCentroid) &&
        faceCentroid(face, pickedCentroid)) {
      const double sign = extrude.distance >= 0 ? 1.0 : -1.0;
      const gp_Dir capNormal = sign > 0 ? *profileNormal : profileNormal->Reversed();
      const double offset = gp_Vec(profileCentroid, pickedCentroid).Dot(gp_Vec(*profileNormal));
      isCap = pickedNormal->Angle(capNormal) < 1e-6 &&
              std::abs(offset - extrude.distance) < 1e-3;
      if (isCap) {
        extrude.distance += sign * distance;
        key = opKey(0);
      }
    }
    if (!isCap) {
      auto ref = geom::FaceRef::fromFace(face);
      if (!ref) return Result<PushPullResult>::failure(ref.error());
      mods.push_back(PushPullOp{ref.value(), distance});
      key = opKey(mods.size() - 1);
    }
  } else if (object.kind() == ObjectKind::Solid) {
    auto ref = geom::FaceRef::fromFace(face);
    if (!ref) return Result<PushPullResult>::failure(ref.error());
    mods.push_back(PushPullOp{ref.value(), distance});
    key = opKey(mods.size() - 1);
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
  if (current->kind() != ObjectKind::Face || !current->recipe.modifiers.empty()) {
    return Result<PushPullResult>::failure("Only profile faces can be extruded");
  }
  Object object = *current;
  object.recipe.modifiers.push_back(ExtrudeOp{distance});
  const Status committed = commitModified(std::move(object), "Extrude");
  if (!committed) return Result<PushPullResult>::failure(committed.message());
  return PushPullResult{id, opKey(0)};
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

Status Modeler::rename(const ObjectId& id, const std::string& name) {
  const Object* current = document_.find(id);
  if (!current) return Status::failure("Object not found");
  if (name.empty()) return Status::failure("Name must not be empty");
  Object object = *current;
  object.name = name;
  history_.execute(Command{"Rename", {ObjectDelta{id, *current, object, document_.indexOf(id)}}});
  return Status::success();
}

Status Modeler::setVisible(const ObjectId& id, bool visible) {
  const Object* current = document_.find(id);
  if (!current) return Status::failure("Object not found");
  Object object = *current;
  object.visible = visible;
  history_.execute(Command{visible ? "Show" : "Hide",
                           {ObjectDelta{id, *current, object, document_.indexOf(id)}}});
  return Status::success();
}

Status Modeler::remove(const std::vector<ObjectId>& ids) {
  Command command{"Delete", {}};
  for (const ObjectId& id : ids) {
    const Object* current = document_.find(id);
    if (!current) return Status::failure("Object not found");
    command.deltas.push_back(ObjectDelta{id, *current, std::nullopt, document_.indexOf(id)});
  }
  if (command.deltas.empty()) return Status::failure("Nothing selected");
  history_.execute(std::move(command));
  return Status::success();
}

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
  for (const Object& o : loaded.objects()) document_.insert(o);
  return Status::success();
}

Status Modeler::exportStep(const std::string& path, const std::vector<ObjectId>& ids) const {
  std::vector<TopoDS_Shape> shapes;
  for (const Object& o : document_.objects()) {
    const bool wanted = ids.empty() ? o.visible
                                    : std::find(ids.begin(), ids.end(), o.id) != ids.end();
    if (wanted && !o.shape.IsNull()) shapes.push_back(o.worldShape());
  }
  if (shapes.empty()) return Status::failure("Nothing to export");
  if (shapes.size() == 1) return geom::writeStep(shapes.front(), path);
  TopoDS_Compound compound;
  BRep_Builder builder;
  builder.MakeCompound(compound);
  for (const TopoDS_Shape& s : shapes) builder.Add(compound, s);
  return geom::writeStep(compound, path);
}

Result<std::vector<ObjectId>> Modeler::importStep(const std::string& path) {
  auto shape = geom::readStep(path);
  if (!shape) return Result<std::vector<ObjectId>>::failure(shape.error());

  std::vector<TopoDS_Shape> parts;
  for (TopExp_Explorer it(shape.value(), TopAbs_SOLID); it.More(); it.Next()) {
    parts.push_back(it.Current());
  }
  if (parts.empty()) parts.push_back(shape.value());

  Command command{"Import STEP", {}};
  std::vector<ObjectId> ids;
  for (const TopoDS_Shape& part : parts) {
    Object o;
    o.id = generateObjectId();
    o.name = uniqueName("Imported");
    // uniqueName() only sees committed objects; disambiguate within this batch.
    if (!ids.empty()) o.name += "." + std::to_string(ids.size());
    o.recipe.base = ImportedShape{part, path};
    const Status built = rebuild(o);
    if (!built) return Result<std::vector<ObjectId>>::failure(built.message());
    ids.push_back(o.id);
    command.deltas.push_back(ObjectDelta{o.id, std::nullopt, o, std::nullopt});
  }
  history_.execute(std::move(command));
  return ids;
}

std::vector<ObjectSummary> Modeler::listObjects() const {
  std::vector<ObjectSummary> out;
  for (const Object& o : document_.objects()) {
    out.push_back({o.id, o.name, o.kind(), describe(o.recipe),
                   o.kind() == ObjectKind::Solid ? geom::volume(o.shape) : 0.0});
  }
  return out;
}

std::vector<Parameter> Modeler::parameters(const ObjectId& id) const {
  const Object* o = document_.find(id);
  return o ? doc::parameters(o->recipe) : std::vector<Parameter>{};
}

}  // namespace sbr::doc
