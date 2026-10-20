#pragma once
// Phase 3 freeform geometry: NURBS curves and surface/solid generators.

#include <vector>

#include <gp_Ax1.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>

#include "sbr/Types.h"

namespace sbr::geom {

// Cubic B-spline (NURBS) curve through the points (closed = periodic).
Result<TopoDS_Wire> makeInterpolatedCurve(const std::vector<gp_Pnt>& points, bool closed);
// Loft through section wires; solid = closed sections capped into a solid.
Result<TopoDS_Shape> loft(const std::vector<TopoDS_Wire>& sections, bool solid);
// Revolves a planar face (or wire) around an axis by angle (radians).
Result<TopoDS_Shape> revolve(const TopoDS_Shape& profile, const gp_Ax1& axis, double angle);
// Sweeps a profile (face or wire) along a spine wire.
Result<TopoDS_Shape> sweep(const TopoDS_Shape& profile, const TopoDS_Wire& spine);

}  // namespace sbr::geom
