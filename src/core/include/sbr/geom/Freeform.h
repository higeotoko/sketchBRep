#pragma once
// Phase 3 freeform geometry: NURBS curves and surface/solid generators.

#include <vector>

#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>

#include "sbr/Types.h"

namespace sbr::geom {

// Exact circular arc from start to end. The apex lies 'bulge' away from the
// chord's midpoint, on the side normal x (end - start) (negative = other
// side). closed = segment face (arc + chord), oriented along 'normal'.
Result<TopoDS_Shape> makeArc(const gp_Pnt& start, const gp_Pnt& end, const gp_Dir& normal, double bulge,
                             bool closed);
// Apex of that arc (for previews and snapping).
gp_Pnt arcApex(const gp_Pnt& start, const gp_Pnt& end, const gp_Dir& normal, double bulge);

// Planar face bounded by curves (lines, arcs, NURBS) that chain into one
// closed loop, in any order and direction. Faces upwards / towards +Y / +X.
Result<TopoDS_Face> faceFromCurves(const std::vector<TopoDS_Shape>& curves);

// Cubic B-spline (NURBS) curve through the points (closed = periodic).
Result<TopoDS_Wire> makeInterpolatedCurve(const std::vector<gp_Pnt>& points, bool closed);
// Loft through section wires; solid = closed sections capped into a solid.
Result<TopoDS_Shape> loft(const std::vector<TopoDS_Wire>& sections, bool solid);
// Revolves a planar face (or wire) around an axis by angle (radians).
Result<TopoDS_Shape> revolve(const TopoDS_Shape& profile, const gp_Ax1& axis, double angle);
// Sweeps a profile (face or wire) along a spine wire.
Result<TopoDS_Shape> sweep(const TopoDS_Shape& profile, const TopoDS_Wire& spine);

// One path from loose edges (Follow Me with edges selected first): they
// must join end to end (within 1e-4 mm, nothing further apart is bridged)
// into a single chain, open or closed, without branches or an edge twice.
// The order and direction they come in do not matter; an open path starts
// at the end nearer to `near` (the profile), a closed one at its vertex
// nearest to it. Refusals say what is wrong (branch, gap, duplicate).
Result<TopoDS_Wire> pathFromEdges(const std::vector<TopoDS_Edge>& edges, const gp_Pnt& near);

}  // namespace sbr::geom
