#include "sbr/doc/Document.h"

#include "sbr/Codec.h"
#include "sbr/geom/Operations.h"

#include <algorithm>
#include <unordered_set>

#include <sstream>

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRep_Builder.hxx>
#include <gp_Ax2.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>

namespace sbr::doc {
namespace {

const Object* findIn(const std::vector<Object>& objects, const ObjectId& id) {
  for (const Object& o : objects) {
    if (o.id == id) return &o;
  }
  return nullptr;
}

gp_Trsf worldIn(const std::vector<Object>& objects, const Object& object) {
  gp_Trsf t = object.transform;
  const Object* parent = findIn(objects, object.parent);
  int guard = 0;
  while (parent && guard++ < 1000) {
    t = parent->transform * t;
    parent = findIn(objects, parent->parent);
  }
  return t;
}

TopoDS_Shape placed(const TopoDS_Shape& shape, const gp_Trsf& t) {
  if (shape.IsNull() || t.Form() == gp_Identity) return shape;
  return shape.Moved(TopLoc_Location(t));
}

}  // namespace

Result<TopoDS_Shape> buildDefinitionShape(const ComponentDefinition& definition,
                                          const RegenContext& context) {
  (void)context;
  TopoDS_Compound compound;
  BRep_Builder builder;
  builder.MakeCompound(compound);
  int count = 0;
  for (const Object& o : definition.objects) {
    if (o.shape.IsNull()) continue;
    builder.Add(compound, placed(o.shape, worldIn(definition.objects, o)));
    ++count;
  }
  if (count == 0) return Result<TopoDS_Shape>::failure("Component definition is empty");
  return TopoDS_Shape(compound);
}

Result<ComponentDefinition> applyOverrides(const ComponentDefinition& definition,
                                           const std::vector<ParameterOverride>& overrides,
                                           const RegenContext& context,
                                           std::vector<ParameterOverride>* dropped) {
  ComponentDefinition result = definition;
  std::vector<std::string> touched;
  for (const ParameterOverride& p : overrides) {
    Object* target = nullptr;
    for (Object& o : result.objects) {
      if (o.id == p.object) target = &o;
    }
    if (!target || !setParameter(target->recipe, p.key, p.value)) {
      if (dropped) dropped->push_back(p);
      continue;
    }
    if (std::find(touched.begin(), touched.end(), p.object) == touched.end()) touched.push_back(p.object);
  }
  for (Object& o : result.objects) {
    if (std::find(touched.begin(), touched.end(), o.id) == touched.end()) continue;
    const Status built = rebuild(o, context);
    if (!built) return Result<ComponentDefinition>::failure("'" + o.name + "': " + built.message());
  }
  if (!touched.empty()) {
    auto shape = buildDefinitionShape(result, context);
    if (!shape) return Result<ComponentDefinition>::failure(shape.error());
    result.shape = shape.value();
  }
  return result;
}

gp_Trsf componentMirror() {
  gp_Trsf m;
  m.SetMirror(gp_Ax2(gp::Origin(), gp::DX()));
  return m;
}

Result<TopoDS_Shape> buildInstanceShape(const ComponentDefinition& definition, const ComponentInstance& instance,
                                        const RegenContext& context) {
  if (definition.shape.IsNull()) return Result<TopoDS_Shape>::failure("Component definition is empty");
  TopoDS_Shape shape = definition.shape;
  if (!instance.overrides.empty()) {
    auto variant = applyOverrides(definition, instance.overrides, context);
    if (!variant) return Result<TopoDS_Shape>::failure(variant.error());
    shape = variant.value().shape;
  }
  if (instance.mirrored) {
    // A copy, so the geometry itself is mirrored (faces stay outward).
    BRepBuilderAPI_Transform mirror(shape, componentMirror(), true);
    if (!mirror.IsDone()) return Result<TopoDS_Shape>::failure("The component could not be mirrored");
    shape = mirror.Shape();
  }
  for (const StretchOp& op : instance.stretches) {
    auto stretched =
        geom::stretchShape(shape, geom::stretchTransform(op.center, op.xDir, op.yDir, op.fx, op.fy, op.fz));
    if (!stretched) return Result<TopoDS_Shape>::failure("The placement could not be stretched: " + stretched.error());
    shape = stretched.value();
  }
  return shape;
}

std::string placementKey(const ComponentInstance& instance) {
  std::ostringstream key;
  key.precision(17);
  key << instance.mirrored;
  for (const ParameterOverride& p : instance.overrides) key << '|' << p.object << '/' << p.key << '=' << p.value;
  for (const StretchOp& op : instance.stretches) {
    key << "|s" << op.center.X() << ',' << op.center.Y() << ',' << op.center.Z() << ',' << op.xDir.X() << ','
        << op.xDir.Y() << ',' << op.xDir.Z() << ',' << op.yDir.X() << ',' << op.yDir.Y() << ',' << op.yDir.Z() << ','
        << op.fx << ',' << op.fy << ',' << op.fz;
  }
  return key.str();
}

void Document::invalidateIndex() const {
  indexValid_ = false;
  childrenValid_ = false;
  index_.clear();
  children_.clear();
}

void Document::ensureIndex() const {
  if (indexValid_) return;
  index_.clear();
  index_.reserve(objects_.size());
  for (std::size_t i = 0; i < objects_.size(); ++i) index_.emplace(objects_[i].id, i);
  indexValid_ = true;
}

void Document::ensureChildren() const {
  if (childrenValid_) return;
  children_.clear();
  for (const Object& o : objects_) {
    if (!o.parent.empty()) children_[o.parent].push_back(o.id);
  }
  childrenValid_ = true;
}

const Object* Document::find(const ObjectId& id) const {
  const auto i = indexOf(id);
  return i ? &objects_[*i] : nullptr;
}

std::optional<std::size_t> Document::indexOf(const ObjectId& id) const {
  if (id.empty()) return std::nullopt;
  ensureIndex();
  const auto it = index_.find(id);
  if (it == index_.end()) return std::nullopt;
  return it->second;
}

std::vector<ObjectId> Document::children(const ObjectId& id) const {
  if (id.empty()) return {};
  ensureChildren();
  const auto it = children_.find(id);
  return it == children_.end() ? std::vector<ObjectId>{} : it->second;
}

std::vector<ObjectId> Document::subtree(const ObjectId& id) const {
  std::vector<ObjectId> out;
  if (!find(id)) return out;
  out.push_back(id);
  for (std::size_t i = 0; i < out.size(); ++i) {
    for (const ObjectId& c : children(out[i])) out.push_back(c);
  }
  return out;
}

ObjectId Document::rootOf(const ObjectId& id) const {
  const Object* o = find(id);
  int guard = 0;
  while (o && !o->parent.empty() && guard++ < 1000) {
    const Object* p = find(o->parent);
    if (!p) break;
    o = p;
  }
  return o ? o->id : ObjectId();
}

bool Document::isAncestor(const ObjectId& ancestor, const ObjectId& id) const {
  const Object* o = find(id);
  int guard = 0;
  while (o && !o->parent.empty() && guard++ < 1000) {
    if (o->parent == ancestor) return true;
    o = find(o->parent);
  }
  return false;
}

bool Document::isVisible(const ObjectId& id) const {
  const Object* o = find(id);
  int guard = 0;
  while (o && guard++ < 1000) {
    if (!o->visible || settings_.hiddenTags.count(o->layer)) return false;
    if (o->parent.empty()) return true;
    o = find(o->parent);
  }
  return o != nullptr;
}

gp_Trsf Document::parentTransform(const Object& object) const {
  const Object* parent = find(object.parent);
  return parent ? worldTransform(*parent) : gp_Trsf();
}

gp_Trsf Document::worldTransform(const Object& object) const {
  gp_Trsf t = object.transform;
  const Object* parent = find(object.parent);
  int guard = 0;
  while (parent && guard++ < 1000) {
    t = parent->transform * t;
    parent = find(parent->parent);
  }
  return t;
}

TopoDS_Shape Document::worldShape(const Object& object) const {
  return placed(object.shape, worldTransform(object));
}

Bnd_Box Document::worldBounds(const Object& object) const {
  const TopoDS_Shape world = worldShape(object);
  if (world.IsNull()) return Bnd_Box();
  const gp_Trsf t = world.Location().Transformation();
  std::array<double, 12> placement{};
  for (int r = 1; r <= 3; ++r) {
    for (int c = 1; c <= 4; ++c) placement[static_cast<std::size_t>((r - 1) * 4 + c - 1)] = t.Value(r, c);
  }
  KeptBounds& kept = bounds_[object.id];
  if (kept.shape.IsNull() || kept.shape.TShape() != world.TShape() || kept.placement != placement) {
    kept.shape = world;
    kept.placement = placement;
    kept.box = Bnd_Box();
    BRepBndLib::Add(world, kept.box);
  }
  return kept.box;
}

TopoDS_Shape Document::worldShape(const ObjectId& id) const {
  const Object* o = find(id);
  return o ? worldShape(*o) : TopoDS_Shape();
}

const ComponentDefinition* Document::findDefinition(const std::string& id) const {
  for (const ComponentDefinition& d : definitions_) {
    if (d.id == id) return &d;
  }
  return nullptr;
}

std::size_t Document::instanceCount(const std::string& definitionId) const {
  std::size_t n = 0;
  for (const Object& o : objects_) {
    const auto* c = std::get_if<ComponentInstance>(&o.recipe.base);
    if (c && c->definitionId == definitionId) ++n;
  }
  return n;
}

RegenContext Document::regenContext() const {
  RegenContext context;
  context.definitionShape = [this](const std::string& id) -> std::optional<TopoDS_Shape> {
    const ComponentDefinition* d = findDefinition(id);
    if (!d || d->shape.IsNull()) return std::nullopt;
    return d->shape;
  };
  context.instanceShape = [this](const ComponentInstance& instance) -> Result<TopoDS_Shape> {
    const ComponentDefinition* d = findDefinition(instance.definitionId);
    if (!d) return Result<TopoDS_Shape>::failure("Unknown component definition " + instance.definitionId);
    std::ostringstream key;
    key << instance.definitionId << '|' << placementKey(instance);
    auto it = variants_.find(key.str());
    if (it != variants_.end() && it->second.definitionShape.IsSame(d->shape)) return it->second.shape;
    RegenContext plain;
    plain.definitionShape = [this](const std::string& id) -> std::optional<TopoDS_Shape> {
      const ComponentDefinition* def = findDefinition(id);
      if (!def || def->shape.IsNull()) return std::nullopt;
      return def->shape;
    };
    auto shape = buildInstanceShape(*d, instance, plain);
    if (shape) variants_[key.str()] = VariantEntry{d->shape, shape.value()};
    return shape;
  };
  return context;
}

void Document::insert(Object object, std::optional<std::size_t> index) {
  const std::size_t at = std::min(index.value_or(objects_.size()), objects_.size());
  const ObjectId id = object.id;
  const ObjectId parent = object.parent;
  if (at == objects_.size() && indexValid_) {
    // Appending keeps the indexes valid (children stay in list order).
    index_[id] = objects_.size();
    if (!parent.empty() && childrenValid_) children_[parent].push_back(id);
  } else {
    invalidateIndex();
  }
  objects_.insert(objects_.begin() + static_cast<std::ptrdiff_t>(at), std::move(object));
  touch(id);
  notify();
}

void Document::replace(const Object& object) {
  const auto i = indexOf(object.id);
  if (!i) return;
  Object& o = objects_[*i];
  if (o.parent != object.parent) childrenValid_ = false;  // positions stay
  o = object;
  touch(object.id);
  notify();
}

void Document::remove(const ObjectId& id) {
  const auto i = indexOf(id);
  if (!i) return;
  objects_.erase(objects_.begin() + static_cast<std::ptrdiff_t>(*i));
  bounds_.erase(id);
  invalidateIndex();
  touch(id);
  notify();
}

void Document::removeMany(const std::vector<ObjectId>& ids) {
  if (ids.empty()) return;
  const std::unordered_set<ObjectId> going(ids.begin(), ids.end());
  objects_.erase(std::remove_if(objects_.begin(), objects_.end(),
                                [&](const Object& o) { return going.count(o.id) > 0; }),
                 objects_.end());
  invalidateIndex();
  for (const ObjectId& id : ids) {
    bounds_.erase(id);
    touch(id);
  }
  notify();
}

void Document::insertMany(std::vector<std::pair<Object, std::optional<std::size_t>>> objects) {
  if (objects.empty()) return;
  bool appended = true;
  for (auto& [object, index] : objects) {
    const std::size_t at = std::min(index.value_or(objects_.size()), objects_.size());
    appended = appended && at == objects_.size();
    const ObjectId id = object.id;
    objects_.insert(objects_.begin() + static_cast<std::ptrdiff_t>(at), std::move(object));
    if (appended && indexValid_) {
      index_[id] = objects_.size() - 1;
      if (!objects_.back().parent.empty() && childrenValid_) children_[objects_.back().parent].push_back(id);
    }
    touch(id);
  }
  if (!appended) invalidateIndex();
  notify();
}

void Document::replaceMany(const std::vector<const Object*>& objects) {
  if (objects.empty()) return;
  for (const Object* object : objects) {
    const auto i = indexOf(object->id);
    if (!i) continue;
    Object& o = objects_[*i];
    if (o.parent != object->parent) childrenValid_ = false;
    o = *object;
    touch(object->id);
  }
  notify();
}

void Document::touch(const ObjectId& id) {
  ++sequence_;
  log_.push_back({sequence_, id});
  // Bound the log: consumers further behind resynchronise fully.
  constexpr std::size_t kMaxLog = 1 << 16;
  if (log_.size() > kMaxLog) log_.erase(log_.begin(), log_.begin() + kMaxLog / 2);
}

std::optional<std::vector<ObjectId>> Document::changedSince(std::uint64_t sequence) const {
  if (sequence < fullChangeSequence_) return std::nullopt;
  if (sequence >= sequence_) return std::vector<ObjectId>{};
  if (log_.empty() || log_.front().sequence > sequence + 1) return std::nullopt;
  std::vector<ObjectId> ids;
  auto it = std::lower_bound(log_.begin(), log_.end(), sequence + 1,
                             [](const LogEntry& e, std::uint64_t s) { return e.sequence < s; });
  for (; it != log_.end(); ++it) ids.push_back(it->id);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

void Document::settingsChanged() {
  fullChangeSequence_ = ++sequence_;
  notify();
}

void Document::putDefinition(const ComponentDefinition& definition) {
  for (ComponentDefinition& d : definitions_) {
    if (d.id == definition.id) {
      d = definition;
      notify();
      return;
    }
  }
  definitions_.push_back(definition);
  notify();
}

void Document::removeDefinition(const std::string& id) {
  const auto it = std::remove_if(definitions_.begin(), definitions_.end(),
                                 [&](const ComponentDefinition& d) { return d.id == id; });
  if (it == definitions_.end()) return;
  definitions_.erase(it, definitions_.end());
  notify();
}

std::string Document::addImage(const std::string& bytes, const std::string& name) {
  const std::string hash = codec::sha256Hex(bytes);
  images_.try_emplace(hash, Image{name, bytes});
  return hash;
}

const Document::Image* Document::findImage(const std::string& hash) const {
  const auto it = images_.find(hash);
  return it == images_.end() ? nullptr : &it->second;
}

const Material* Document::findMaterial(const std::string& id) const {
  if (id.empty()) return nullptr;
  for (const Material& m : materials_) {
    if (m.id == id) return &m;
  }
  return nullptr;
}

std::string Document::inheritedMaterial(const Object& object) const {
  if (!object.material.empty()) return object.material;
  for (ObjectId p = object.parent; !p.empty();) {
    const Object* parent = find(p);
    if (!parent) break;
    if (!parent->material.empty()) return parent->material;
    p = parent->parent;
  }
  return {};
}

Color Document::materialColor(const std::string& id, const Color& fallback) const {
  const Material* m = findMaterial(id);
  return m ? m->color : fallback;
}

Color Document::displayColor(const Object& object) const {
  return materialColor(inheritedMaterial(object), object.color);
}

double Document::materialOpacity(const std::string& id) const {
  const Material* m = findMaterial(id);
  return m ? m->opacity : 1.0;
}

double Document::displayOpacity(const Object& object) const { return materialOpacity(inheritedMaterial(object)); }

void Document::putMaterial(const Material& material) {
  bool found = false;
  for (Material& m : materials_) {
    if (m.id == material.id) {
      m = material;
      found = true;
    }
  }
  if (!found) materials_.push_back(material);
  fullChangeSequence_ = ++sequence_;
  notify();
}

void Document::removeMaterial(const std::string& id) {
  const auto it = std::remove_if(materials_.begin(), materials_.end(), [&](const Material& m) { return m.id == id; });
  if (it == materials_.end()) return;
  materials_.erase(it, materials_.end());
  fullChangeSequence_ = ++sequence_;
  notify();
}

void Document::clear() {
  objects_.clear();
  definitions_.clear();
  materials_.clear();
  images_.clear();
  variants_.clear();
  bounds_.clear();
  settings_ = DocumentSettings{};
  invalidateIndex();
  log_.clear();
  fullChangeSequence_ = ++sequence_;
  notify();
}

void Document::notify() const {
  for (const Listener& listener : listeners_) listener();
}

}  // namespace sbr::doc
