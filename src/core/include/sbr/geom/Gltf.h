#pragma once
// glTF 2.0 for visualization (docs/85_gltf_export): meshes with physically
// based materials (colour, opacity, metallic, roughness), for Blender,
// Twinmotion, Unreal Engine and other renderers. Metres, Y up as glTF wants
// (the model's mm and Z up converted). The B-rep is meshed on a copy: the
// model's shapes are not changed. ".glb" (one binary file) or ".gltf"
// (text with a .bin beside it).

#include <array>
#include <string>
#include <vector>

#include <TopoDS_Shape.hxx>

#include "sbr/Types.h"

namespace sbr::geom {

struct GltfPart {
  TopoDS_Shape shape;  // world placed, mm
  std::string name;
  std::string materialName;            // parts with the same material share it
  std::array<double, 4> color{0.8, 0.8, 0.8, 1.0};  // RGB 0..1 (linear), opacity
  double metallic = 0.0;
  double roughness = 0.6;
};

struct GltfExportOptions {
  double linearDeflection = 1.0;   // mm: how far the mesh may be from the curved faces
  double angularDeflection = 0.26; // radians (15 degrees)
};

Status writeGltf(const std::vector<GltfPart>& parts, const std::string& path, const GltfExportOptions& options = {});

}  // namespace sbr::geom
