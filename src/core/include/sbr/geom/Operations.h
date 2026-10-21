#pragma once
// Solid modelling operations used by the direct-modelling tools.

#include <vector>

#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
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

// Offsets the outer boundary of a planar face within its plane (positive =
// outwards, sharp corners as in architectural plans). Inner holes are not
// supported yet. The face normal is preserved.
Result<TopoDS_Face> offsetFace(const TopoDS_Face& face, double distance);

// SketchUp-style "draw a line across a face": splits a planar face of a
// solid with a polyline whose ends lie on the face boundary. The solid keeps
// its volume; the face becomes two (or more) faces that can be pushed/pulled
// separately. Fails if the line does not cut the face.
Result<TopoDS_Shape> splitFace(const TopoDS_Shape& solid, const TopoDS_Face& face,
                               const std::vector<gp_Pnt>& polyline);

// True if p lies on the face (inside or on its boundary) within tolerance.
bool pointOnFace(const TopoDS_Face& face, const gp_Pnt& p, double tolerance = 1e-3);
// True if p lies on the face boundary.
bool pointOnFaceBoundary(const TopoDS_Face& face, const gp_Pnt& p, double tolerance = 1e-3);

// Outward normal of a (possibly curved) face at the point nearest to p.
std::optional<gp_Dir> faceNormalAt(const TopoDS_Face& face, const gp_Pnt& p);

// Push/pull of a curved face (cylinder side, swept or lofted surface): the
// face is offset by 'distance' along its normal and the neighbouring faces
// are extended or trimmed to meet it. Fails without side effects.
Result<TopoDS_Shape> offsetSolidFace(const TopoDS_Shape& solid, const TopoDS_Face& face, double distance);

enum class BooleanOp { Union, Subtract, Intersect };

// Combines solids: Union/Intersect over all shapes, Subtract removes
// shapes[1..] from shapes[0]. Coplanar faces are merged. The result may
// contain several solids (e.g. a wall cut in two) but must not be empty.
Result<TopoDS_Shape> booleanCombine(BooleanOp op, const std::vector<TopoDS_Shape>& shapes);

}  // namespace sbr::geom
