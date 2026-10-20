#pragma once
// Solid modelling operations used by the direct-modelling tools.

#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include "sbr/Types.h"

namespace sbr::geom {

// True if the face lies on a plane; optionally returns the plane.
bool isPlanarFace(const TopoDS_Face& face, gp_Pln* planeOut = nullptr);

// Normal of a planar face taking the face orientation into account, i.e. the
// outward normal when the face belongs to a correctly oriented solid.
std::optional<gp_Dir> planarFaceNormal(const TopoDS_Face& face);

// Sweeps a planar face along `direction * distance` into a solid. The distance
// is signed relative to the face normal. The result is validated.
Result<TopoDS_Shape> extrudeFace(const TopoDS_Face& face, double distance);

// SketchUp-style push/pull of a planar face of a solid: the face is moved by
// `distance` along its outward normal by fusing (distance > 0) or cutting
// (distance < 0) a prism. Coplanar faces are merged afterwards. Fails without
// side effects if the result is not a single valid solid.
Result<TopoDS_Shape> pushPullFace(const TopoDS_Shape& solid, const TopoDS_Face& face, double distance);

}  // namespace sbr::geom
