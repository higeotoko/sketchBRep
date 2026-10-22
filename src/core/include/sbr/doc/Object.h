#pragma once
// Document layer object.

#include <array>
#include <map>
#include <string>

#include <gp_Trsf.hxx>
#include <TopoDS_Shape.hxx>

#include "sbr/doc/Recipe.h"

namespace sbr::doc {

using ObjectId = std::string;

// Generates a random 128-bit identifier in canonical UUID text form.
ObjectId generateObjectId();

struct Color {
  double r = 0.86, g = 0.86, b = 0.84;
  bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b; }
};

enum class ObjectKind { Empty, Curve, Face, Solid, Group, Instance, Annotation, Guide, Other };
const char* toString(ObjectKind kind);

struct Object {
  ObjectId id;
  ObjectId parent;  // empty: top level; otherwise a Group object
  std::string name;
  std::string layer = "Default";  // tag
  bool visible = true;
  Color color;
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

}  // namespace sbr::doc
