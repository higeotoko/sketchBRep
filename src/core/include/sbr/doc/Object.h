#pragma once
// Document layer object.

#include <array>
#include <map>
#include <string>

#include <gp_Trsf.hxx>
#include <TopoDS_Shape.hxx>

#include "sbr/doc/Material.h"
#include "sbr/doc/Recipe.h"

namespace sbr::doc {

using ObjectId = std::string;

// Generates a random 128-bit identifier in canonical UUID text form.
ObjectId generateObjectId();


// Shell: an open surface of several faces (a solid with faces deleted, an
// open loft). Drawn on, painted and filled again, but not pushed/pulled or
// combined with Booleans until it is closed.
enum class ObjectKind { Empty, Curve, Face, Solid, Shell, Group, Instance, Annotation, Guide, Image, Other };
const char* toString(ObjectKind kind);

struct Object {
  ObjectId id;
  ObjectId parent;  // empty: top level; otherwise a Group object
  std::string name;
  std::string layer = "Default";  // tag
  bool visible = true;
  // Locked: cannot be picked, moved or erased (still drawn and snapped to);
  // e.g. an image traced over. Unlock from the object list.
  bool locked = false;
  Color color;  // legacy colour, shown only where no material applies
  // Material of the whole object ("" = inherit from the container), and
  // faces painted on their own (these win).
  std::string material;
  std::vector<FaceMaterial> faceMaterials;
  gp_Trsf transform;  // local -> parent placement
  // Optional meaning ("kind": "wall", "level": "2F", "grid": "X3"), set by
  // people or agents and used to find objects. Geometry never depends on it.
  std::map<std::string, std::string> attributes;

  Recipe recipe;
  TopoDS_Shape shape;  // canonical shape in local coordinates (cached result of recipe)

  ObjectKind kind() const;
  bool isContainer() const { return kind() == ObjectKind::Group; }
};

// Regenerates `object.shape` from `object.recipe`.
Status rebuild(Object& object, const RegenContext& context = {});

// After `object.shape` changed: finds each painted face again on the new
// shape and re-identifies it there (pieces of a split face each keep the
// material). Assignments whose face cannot be told stay unapplied rather
// than move to another face.
void reanchorFaceMaterials(Object& object);
// Faces painted on their own: 1-based index in TopExp::MapShapes(shape,
// FACE) order -> material id; `unresolved` counts assignments applied to
// no face.
struct FaceMaterialMap {
  std::vector<std::pair<int, std::string>> faces;
  std::size_t unresolved = 0;
};
FaceMaterialMap resolveFaceMaterials(const Object& object);

}  // namespace sbr::doc
