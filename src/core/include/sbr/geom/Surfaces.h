#pragma once
// Rhino-style surface and edge operations (docs/47): Sweep2, EdgeSrf,
// Patch, FilletEdge / ChamferEdge and Shell. Exact B-rep throughout; each
// fails with a reason rather than give a broken shape.

#include <utility>
#include <vector>

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include "sbr/Types.h"

namespace sbr::geom {

// A curve (wire, edges in any order, an edge, or a face's outline) as one
// wire; refused when its pieces do not join into one.
Result<TopoDS_Wire> curveWire(const TopoDS_Shape& curve);

// Profiles (faces or curves) swept along rail1 while keeping touch with
// rail2: the profiles turn and stretch to span the two rails (Rhino's
// Sweep2). Closed profiles give a solid when the result closes.
Result<TopoDS_Shape> sweep2(const TopoDS_Wire& rail1, const TopoDS_Wire& rail2,
                            const std::vector<TopoDS_Shape>& profiles);

// A surface from 2, 3 or 4 boundary curves (Rhino's EdgeSrf): two curves
// are joined straight across (ruled); three or four bound a smooth patch,
// and must meet end to end.
Result<TopoDS_Shape> surfaceFromEdges(const std::vector<TopoDS_Shape>& curves);
// A smooth surface over one closed outline of any number of curves
// (Rhino's Patch); a flat outline gives a plain flat face.
Result<TopoDS_Shape> patchSurface(const std::vector<TopoDS_Shape>& curves);

// Edges of a solid rounded (fillet) or bevelled (chamfer, equal distances)
// by `size`. When too large, the failure says about how large they take.
Result<TopoDS_Shape> roundEdges(const TopoDS_Shape& solid, const std::vector<TopoDS_Edge>& edges, double size,
                                bool chamfer);
// Corners of a flat face rounded (fillet) or cut (chamfer, equal
// distances) by `size`; the face stays one flat face. `corners` are points
// at the corners (within 1e-3 mm). When too large, says how large they take.
Result<TopoDS_Shape> roundCorners(const TopoDS_Shape& face, const std::vector<gp_Pnt>& corners, double size,
                                  bool chamfer);
// The corners of a flat face where two edges meet at an angle (not where
// they run on smoothly).
std::vector<gp_Pnt> faceCorners(const TopoDS_Face& face);

// The solid hollowed to walls `thickness` thick, the given faces left open
// (Rhino's Shell). No face to open: a closed hollow inside.
Result<TopoDS_Shape> shellSolid(const TopoDS_Shape& solid, const std::vector<TopoDS_Face>& open, double thickness);

// An edge told again after the shape is made anew: a point in its middle
// and the direction there. Found again as the edge passing through the
// point (within 1e-3 mm) along that direction; refused when none or two.
std::pair<gp_Pnt, gp_Dir> edgeKey(const TopoDS_Edge& edge);
Result<TopoDS_Edge> findEdge(const TopoDS_Shape& shape, const gp_Pnt& point, const gp_Dir& direction);

}  // namespace sbr::geom
