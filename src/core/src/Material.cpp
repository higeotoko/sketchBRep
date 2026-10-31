#include "sbr/doc/Material.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include "sbr/doc/Object.h"

namespace sbr::doc {
namespace {

int channel(double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); }

std::string hex(const Color& c) {
  char buffer[8];
  std::snprintf(buffer, sizeof buffer, "%02X%02X%02X", channel(c.r), channel(c.g), channel(c.b));
  return buffer;
}

}  // namespace

std::string colorMaterialId(const Color& color) { return "rgb-" + hex(color); }

Material colorMaterial(const Color& color) {
  Material m;
  m.id = colorMaterialId(color);
  m.name = "#" + hex(color);
  m.color = color;
  return m;
}

std::optional<FaceMaterial> FaceMaterial::of(const TopoDS_Face& localFace, const std::string& material) {
  FaceMaterial out;
  out.material = material;
  if (auto plane = geom::FaceRef::identify(localFace)) {
    out.plane = plane.value();
    return out;
  }
  if (auto surface = geom::SurfaceRef::fromFace(localFace)) {
    out.surface = surface.value();
    return out;
  }
  return std::nullopt;
}

std::vector<TopoDS_Face> FaceMaterial::facesIn(const TopoDS_Shape& shape) const {
  if (shape.IsNull()) return {};
  if (plane) return plane->continuations(shape);
  if (surface) {
    auto face = surface->resolve(shape);
    if (face) return {face.value()};
  }
  return {};
}

void reanchorFaceMaterials(Object& object) {
  if (object.faceMaterials.empty() || object.shape.IsNull()) return;
  std::vector<FaceMaterial> out;
  for (const FaceMaterial& fm : object.faceMaterials) {
    const std::vector<TopoDS_Face> faces = fm.facesIn(object.shape);
    if (faces.empty()) {
      out.push_back(fm);  // kept, applied nowhere (no guessing)
      continue;
    }
    for (const TopoDS_Face& f : faces) {
      if (auto again = FaceMaterial::of(f, fm.material)) out.push_back(std::move(*again));
    }
  }
  object.faceMaterials = std::move(out);
}

FaceMaterialMap resolveFaceMaterials(const Object& object) {
  FaceMaterialMap out;
  if (object.faceMaterials.empty() || object.shape.IsNull()) return out;
  // Looking the faces up is geometry work; the display asks on every
  // restyle (a material edited, a section moved), so the answer is kept
  // until the shape or the assignments change.
  std::string key;
  auto add = [&key](double v) { key.append(reinterpret_cast<const char*>(&v), sizeof v); };
  auto addPoint = [&add](const gp_XYZ& p) {
    add(p.X());
    add(p.Y());
    add(p.Z());
  };
  for (const FaceMaterial& fm : object.faceMaterials) {
    key += fm.material;
    key += '\0';
    if (fm.plane) {
      key += 'p';
      addPoint(fm.plane->normal.XYZ());
      addPoint(fm.plane->centroid.XYZ());
      add(fm.plane->area);
      if (fm.plane->inside) addPoint(fm.plane->inside->XYZ());
      key += '|';
      if (fm.plane->extent) {
        addPoint(fm.plane->extent->first.XYZ());
        addPoint(fm.plane->extent->second.XYZ());
      }
    }
    if (fm.surface) {
      key += 's';
      addPoint(fm.surface->point.XYZ());
      addPoint(fm.surface->normal.XYZ());
      add(fm.surface->surfaceType);
    }
    key += ';';
  }
  if (object.faceMaterialsShape.IsSame(object.shape) && object.faceMaterialsKey == key) {
    out.faces = object.faceMaterialsFaces;
    out.unresolved = object.faceMaterialsUnresolved;
    return out;
  }
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(object.shape, TopAbs_FACE, faces);
  std::map<int, std::string> byIndex;  // later assignments win
  for (const FaceMaterial& fm : object.faceMaterials) {
    const std::vector<TopoDS_Face> found = fm.facesIn(object.shape);
    if (found.empty()) ++out.unresolved;
    for (const TopoDS_Face& f : found) {
      const int i = faces.FindIndex(f);
      if (i > 0) byIndex[i] = fm.material;
    }
  }
  out.faces.assign(byIndex.begin(), byIndex.end());
  object.faceMaterialsShape = object.shape;
  object.faceMaterialsKey = std::move(key);
  object.faceMaterialsFaces = out.faces;
  object.faceMaterialsUnresolved = out.unresolved;
  return out;
}

}  // namespace sbr::doc
