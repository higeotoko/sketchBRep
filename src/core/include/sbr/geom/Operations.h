#pragma once
// Solid modelling operations used by the direct-modelling tools.

#include <vector>

#include <gp_GTrsf.hxx>

#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include "sbr/Types.h"

namespace sbr::geom {

// Fills every free boundary (hole) of an open shell that is a flat loop with
// a face and sews them in; a shell closed that way becomes a solid. Holes
// that are not flat are refused.
Result<TopoDS_Shape> capHoles(const TopoDS_Shape& shell);

// Moves the vertices of a polyhedral shape (flat faces, straight edges) found
// at `points` by `delta`, keeping every face flat (SketchUp's move of an
// edge or corner). A face that would bend is refused, or with `autofold`
// divided into flat pieces along new edges (SketchUp's Autofold): the part
// whose corners stay put remains one face on its old plane and the moved
// part is cut into as few flat pieces as possible (triangles where
// needed); faces that stay flat are not divided. Only convex faces without
// holes are folded (others are refused with the reason). Refused anyway
// when a face would fold over, collapse, or the solid would cross itself.
Result<TopoDS_Shape> moveVertices(const TopoDS_Shape& shape, const std::vector<gp_Pnt>& points, const gp_Vec& delta,
                                  bool autofold = false);

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
// keepLines: segments of lines that split faces earlier (SplitOp); edges on
// them are kept, so coplanar parts are not merged back together.
using Segment = std::pair<gp_Pnt, gp_Pnt>;
Result<TopoDS_Shape> pushPullFace(const TopoDS_Shape& solid, const TopoDS_Face& face, double distance,
                                  const std::vector<Segment>& keepLines = {});

// Offsets the outer boundary of a planar face within its plane (positive =
// outwards, sharp corners as in architectural plans). Inner holes are not
// supported yet. The face normal is preserved.
Result<TopoDS_Face> offsetFace(const TopoDS_Face& face, double distance);

// SketchUp-style "draw a line across a face": splits a planar face of a
// solid with a polyline whose ends lie on the face boundary. The solid keeps
// its volume; the face becomes two (or more) faces that can be pushed/pulled
// separately. Fails if the line does not cut the face.
// The non-uniform scale of StretchOp: p -> center + M (p - center), with M
// scaling by fx / fy / fz along xDir / yDir / xDir x yDir.
gp_GTrsf stretchTransform(const gp_Pnt& center, const gp_Dir& xDir, const gp_Dir& yDir, double fx, double fy,
                          double fz);
// Applies it to a shape (exact geometry; curves and surfaces that do not
// keep their kind become NURBS).
Result<TopoDS_Shape> stretchShape(const TopoDS_Shape& shape, const gp_GTrsf& transform);

// Draws `face`'s outline offset by `distance` (negative: inwards; outwards
// would leave the face and is refused) onto the face of `solid`, dividing
// it into a frame and the inner face. The solid stays valid and closed.
Result<TopoDS_Shape> imprintOffset(const TopoDS_Shape& solid, const TopoDS_Face& face, double distance);

Result<TopoDS_Shape> splitFace(const TopoDS_Shape& solid, const TopoDS_Face& face,
                               const std::vector<gp_Pnt>& polyline);

// Same for a single planar face (a profile): returns the parts.
Result<std::vector<TopoDS_Face>> splitPlanarFace(const TopoDS_Face& face, const std::vector<gp_Pnt>& polyline);

// Like splitPlanarFace, but the line's first and last segments are extended
// until they cross the boundary (used when the face grew after the split).
Result<std::vector<TopoDS_Face>> splitPlanarFaceExtended(const TopoDS_Face& face,
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

// Face with 'hole' (a face strictly inside it, same plane) cut out.
Result<TopoDS_Face> cutHole(const TopoDS_Face& face, const TopoDS_Face& hole);
// True if 'inner' lies in the plane of 'outer' and strictly inside it.
bool faceInsideFace(const TopoDS_Face& inner, const TopoDS_Face& outer);

enum class BooleanOp { Union, Subtract, Intersect };

// Combines solids: Union/Intersect over all shapes, Subtract removes
// shapes[1..] from shapes[0]. Coplanar faces are merged. The result may
// contain several solids (e.g. a wall cut in two) but must not be empty.
Result<TopoDS_Shape> booleanCombine(BooleanOp op, const std::vector<TopoDS_Shape>& shapes);

}  // namespace sbr::geom
