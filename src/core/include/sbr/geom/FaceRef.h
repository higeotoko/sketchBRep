#pragma once
// Persistent reference to a planar face of a regenerated solid.
//
// Face indices change whenever the topology is rebuilt (the Topological Naming
// Problem), so edit history never stores them. Instead a face is described by
// its geometry (outward normal, supporting plane and centroid) and resolved
// against the regenerated shape by geometric matching.

#include <optional>
#include <utility>
#include <vector>

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
  // Identity of an edited face (absent in files from before): a point
  // strictly inside it and its bounding box when it was picked.
  std::optional<gp_Pnt> inside;
  std::optional<std::pair<gp_Pnt, gp_Pnt>> extent;  // box min / max

  // Plane and centroid only (cheap; for looking at faces).
  static Result<FaceRef> fromFace(const TopoDS_Face& face);
  // Also records the identity: use for references stored in a recipe.
  static Result<FaceRef> identify(const TopoDS_Face& face);
  // Finds the same face among those of `shape` on the same supporting plane
  // and facing the same way: the one holding the remembered point, else the
  // only one still overlapping the remembered box, else the one on the same
  // side of every split line (`splitLines`) on that plane. Anything else is
  // refused as ambiguous rather than guessed: a valid edit of the wrong
  // face is worse than a failed one. References without the identity
  // (older files) take the face with the closest centroid.
  Result<TopoDS_Face> resolve(const TopoDS_Shape& shape,
                              const std::vector<std::pair<gp_Pnt, gp_Pnt>>& splitLines = {}) const;
  FaceRef transformed(const gp_Trsf& trsf) const;
};

// Reference to a curved face: a point inside it, the outward normal there
// and the surface type. Resolves to the face of the same type and facing
// whose surface passes closest to the point; two equally close are refused
// as ambiguous.
struct SurfaceRef {
  gp_Pnt point;
  gp_Dir normal{0, 0, 1};
  int surfaceType = 0;  // GeomAbs_SurfaceType

  static Result<SurfaceRef> fromFace(const TopoDS_Face& face);
  Result<TopoDS_Face> resolve(const TopoDS_Shape& shape) const;
};

}  // namespace sbr::geom
