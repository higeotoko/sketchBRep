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

}  // namespace sbr::geom
