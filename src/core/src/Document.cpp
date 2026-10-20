#include "sbr/doc/Document.h"

#include <algorithm>

#include <BRep_Builder.hxx>
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

const Object* Document::find(const ObjectId& id) const { return findIn(objects_, id); }

std::optional<std::size_t> Document::indexOf(const ObjectId& id) const {
  for (std::size_t i = 0; i < objects_.size(); ++i) {
    if (objects_[i].id == id) return i;
  }
  return std::nullopt;
}

std::vector<ObjectId> Document::children(const ObjectId& id) const {
  std::vector<ObjectId> out;
  for (const Object& o : objects_) {
    if (o.parent == id && !id.empty()) out.push_back(o.id);
  }
  return out;
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
    if (!o->visible) return false;
    if (o->parent.empty()) return true;
    o = find(o->parent);
  }
  return o != nullptr;
}

gp_Trsf Document::parentTransform(const Object& object) const {
  const Object* parent = find(object.parent);
  return parent ? worldTransform(*parent) : gp_Trsf();
}

gp_Trsf Document::worldTransform(const Object& object) const { return worldIn(objects_, object); }

TopoDS_Shape Document::worldShape(const Object& object) const {
  return placed(object.shape, worldTransform(object));
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
  return context;
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

void Document::clear() {
  objects_.clear();
  definitions_.clear();
  settings_ = DocumentSettings{};
  notify();
}

void Document::notify() const {
  for (const Listener& listener : listeners_) listener();
}

}  // namespace sbr::doc
