#pragma once
// Exact (non-tessellated) measurements of B-rep shapes.

#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>

namespace sbr::geom {

double volume(const TopoDS_Shape& shape);
// Volume by plain Gauss integration: exact for flat faces, close for curved
// ones, a quarter of the time. For signs and checks, not for reporting.
double roughVolume(const TopoDS_Shape& shape);
double area(const TopoDS_Shape& shape);
// Bounding box computed from the exact geometry (no triangulation used).
Bnd_Box exactBounds(const TopoDS_Shape& shape);

}  // namespace sbr::geom
