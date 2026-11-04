#pragma once
// Materials (finishes): a shared library of named materials with stable
// ids, assigned to whole objects and to single faces. See docs/11_materials.md.
//
// Which material a face shows, most specific first:
//   1. the face's own material (FaceMaterial on its object),
//   2. the object's material,
//   3. the material of the nearest container (group, component placement),
//   4. none: the object's legacy colour (default grey).
// Clearing 1 or 2 ("inherit") falls back to the next rule.

#include <optional>
#include <string>
#include <vector>

#include "sbr/geom/FaceRef.h"

namespace sbr::doc {

struct Color {
  double r = 0.86, g = 0.86, b = 0.84;
  bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b; }
  bool operator!=(const Color& o) const { return !(*this == o); }
};

// An image on a material, at its real size: one tile covers width x height
// (mm) of a face, repeated. The picture lives in Document::images.
struct MaterialTexture {
  std::string image;     // sha-256 of the picture (Document::images)
  double width = 1000.0;  // mm per tile
  double height = 1000.0;
  bool colorize = false;  // tinted by the material colour
  bool operator==(const MaterialTexture& o) const {
    return image == o.image && width == o.width && height == o.height && colorize == o.colorize;
  }
  bool operator!=(const MaterialTexture& o) const { return !(*this == o); }
};

struct Material {
  std::string id;    // stable: never reused or renamed; objects refer to it
  std::string name;  // what people see (may change)
  Color color;
  double opacity = 1.0;  // 1 = opaque, 0 = invisible
  std::optional<MaterialTexture> texture;
  // Physically based values for rendering (docs/11_materials.md): kept and
  // edited now, used by a renderer later; the modelling view ignores them.
  double metallic = 0.0;     // 0 dielectric .. 1 metal
  double roughness = 0.5;    // 0 mirror .. 1 matte
  double reflectance = 0.5;  // specular strength of dielectrics (0.5 = 4 %)

  bool operator==(const Material& o) const {
    return id == o.id && name == o.name && color == o.color && opacity == o.opacity && texture == o.texture &&
           metallic == o.metallic && roughness == o.roughness && reflectance == o.reflectance;
  }
  bool operator!=(const Material& o) const { return !(*this == o); }
};

// A face painted on its own. The face is identified like edited faces
// (plane + a point inside + extent; curved faces by a surface point), in the
// object's local coordinates, and found again after each shape change.
struct FaceMaterial {
  std::optional<geom::FaceRef> plane;
  std::optional<geom::SurfaceRef> surface;
  std::string material;

  // Identity of `localFace` (planar or curved); nullopt if it has none.
  static std::optional<FaceMaterial> of(const TopoDS_Face& localFace, const std::string& material);
  // Faces of `shape` that carry this assignment on (see docs/11): the same
  // face, all pieces of it after a split, or the face moved parallel to
  // itself (push/pull, a changed parameter) when that is unambiguous.
  // Empty when it cannot be told: then it is applied to no face at all.
  std::vector<TopoDS_Face> facesIn(const TopoDS_Shape& shape) const;
};

// Deterministic id / name for a material made from a plain RGB colour
// (older files, imported colours, the colour API): the same colour always
// maps to the same material.
std::string colorMaterialId(const Color& color);
Material colorMaterial(const Color& color);

}  // namespace sbr::doc
