#pragma once
// Display layer: render meshes derived from canonical B-rep geometry.
//
// Tessellation always happens on a private copy of the canonical shape so
// that display resolution never leaks into (or degrades) the model data.

#include <cstdint>
#include <vector>

#include <TopoDS_Shape.hxx>

namespace sbr::display {

struct DisplayQuality {
  double linearDeflection = 1.0;     // mm, max chord deviation
  double angularDeflection = 0.35;   // rad

  static DisplayQuality coarse() { return {20.0, 0.8}; }
  static DisplayQuality normal() { return {1.0, 0.35}; }
  static DisplayQuality fine() { return {0.05, 0.1}; }
};

struct DisplayMesh {
  std::vector<float> positions;  // xyz triples
  std::vector<float> normals;    // xyz triples, per vertex
  std::vector<std::uint32_t> triangles;  // index triples, CCW seen from outside
  std::vector<std::vector<float>> edgePolylines;  // xyz triples per B-rep edge

  std::size_t vertexCount() const { return positions.size() / 3; }
  std::size_t triangleCount() const { return triangles.size() / 3; }
};

// Deep copy of the canonical shape (geometry copied, no mesh) for the viewer.
TopoDS_Shape makeDisplayCopy(const TopoDS_Shape& canonical);

// Tessellates a copy of `canonical` and extracts triangles and edge polylines.
DisplayMesh buildDisplayMesh(const TopoDS_Shape& canonical, const DisplayQuality& quality);

}  // namespace sbr::display
