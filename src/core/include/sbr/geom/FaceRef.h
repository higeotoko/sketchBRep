#pragma once
// Persistent reference to a planar face of a regenerated solid.
//
// Face indices change whenever the topology is rebuilt (the Topological Naming
// Problem), so edit history never stores them. Instead a face is described by
// its geometry (outward normal, supporting plane and centroid) and resolved
// against the regenerated shape by geometric matching.

#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include "sbr/Types.h"

namespace sbr::geom {

struct FaceRef {
  gp_Dir normal{0, 0, 1};  // outward normal
  gp_Pnt centroid;         // lies on the supporting plane
  double area = 0.0;

  static Result<FaceRef> fromFace(const TopoDS_Face& face);
  // Finds the planar face of `shape` with the same supporting plane and
  // orientation whose centroid is closest to the stored one.
  Result<TopoDS_Face> resolve(const TopoDS_Shape& shape) const;
  FaceRef transformed(const gp_Trsf& trsf) const;
};

// Reference to a curved face: a point inside it, the outward normal there
// and the surface type. Resolves to the face of the same type and facing
// whose surface passes closest to the point.
struct SurfaceRef {
  gp_Pnt point;
  gp_Dir normal{0, 0, 1};
  int surfaceType = 0;  // GeomAbs_SurfaceType

  static Result<SurfaceRef> fromFace(const TopoDS_Face& face);
  Result<TopoDS_Face> resolve(const TopoDS_Shape& shape) const;
};

}  // namespace sbr::geom
