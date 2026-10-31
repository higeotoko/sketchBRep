#include "sbr/geom/Profile.h"

#include <cmath>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <gp_Vec.hxx>

#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"

namespace sbr::geom {

SketchPlane SketchPlane::ground() { return SketchPlane{}; }

SketchPlane SketchPlane::fromNormal(const gp_Pnt& origin, const gp_Dir& normal) {
  SketchPlane plane;
  plane.origin = origin;
  plane.normal = normal;
  const gp_Dir worldZ(0, 0, 1);
  if (std::abs(normal.Dot(worldZ)) > 0.999) {
    gp_Vec x(1, 0, 0);
    x -= gp_Vec(normal) * x.Dot(gp_Vec(normal));
    plane.xDir = gp_Dir(x);
  } else {
    plane.xDir = worldZ.Crossed(normal);
  }
  return plane;
}

void SketchPlane::toLocal(const gp_Pnt& p, double& u, double& v) const {
  const gp_Vec d(origin, p);
  u = d.Dot(gp_Vec(xDir));
  v = d.Dot(gp_Vec(yDir()));
}

gp_Pnt SketchPlane::toWorld(double u, double v) const {
  return origin.Translated(gp_Vec(xDir) * u + gp_Vec(yDir()) * v);
}

namespace {

// Newell's method; returns false for degenerate polygons.
bool newellNormal(const std::vector<gp_Pnt>& points, gp_Dir& normal, gp_Pnt& centroid) {
  gp_XYZ n(0, 0, 0);
  gp_XYZ c(0, 0, 0);
  for (std::size_t i = 0; i < points.size(); ++i) {
    const gp_XYZ& a = points[i].XYZ();
    const gp_XYZ& b = points[(i + 1) % points.size()].XYZ();
    n += gp_XYZ((a.Y() - b.Y()) * (a.Z() + b.Z()), (a.Z() - b.Z()) * (a.X() + b.X()),
                (a.X() - b.X()) * (a.Y() + b.Y()));
    c += a;
  }
  if (points.empty() || n.Modulus() < 1e-12) return false;
  normal = gp_Dir(n);
  centroid = gp_Pnt(c / static_cast<double>(points.size()));
  return true;
}

std::vector<gp_Pnt> withoutClosingPoint(std::vector<gp_Pnt> points) {
  if (points.size() > 1 && points.front().Distance(points.back()) < tolerance::kLinear) {
    points.pop_back();
  }
  return points;
}

bool hasConsecutiveDuplicates(const std::vector<gp_Pnt>& points, bool closed) {
  for (std::size_t i = 0; i + 1 < points.size(); ++i) {
    if (points[i].Distance(points[i + 1]) < tolerance::kMinDimension) return true;
  }
  return closed && points.size() > 1 &&
         points.back().Distance(points.front()) < tolerance::kMinDimension;
}

}  // namespace

bool arePointsCoplanar(const std::vector<gp_Pnt>& points, gp_Pln* planeOut) {
  gp_Dir normal;
  gp_Pnt centroid;
  if (!newellNormal(points, normal, centroid)) return false;
  const gp_Pln plane(centroid, normal);
  for (const gp_Pnt& p : points) {
    if (plane.Distance(p) > tolerance::kLinear) return false;
  }
  if (planeOut) *planeOut = plane;
  return true;
}

Result<TopoDS_Face> makeRectangleFace(const SketchPlane& plane, double width, double height) {
  if (!std::isfinite(width) || !std::isfinite(height) ||
      std::abs(width) < tolerance::kMinDimension || std::abs(height) < tolerance::kMinDimension) {
    return Result<TopoDS_Face>::failure("Rectangle dimensions must be non-zero");
  }
  std::vector<gp_Pnt> corners = {plane.toWorld(0, 0), plane.toWorld(width, 0),
                                 plane.toWorld(width, height), plane.toWorld(0, height)};
  // Keep the loop counter-clockwise around the plane normal.
  if (width * height < 0) std::swap(corners[1], corners[3]);

  BRepBuilderAPI_MakePolygon polygon;
  for (const gp_Pnt& p : corners) polygon.Add(p);
  polygon.Close();
  if (!polygon.IsDone()) return Result<TopoDS_Face>::failure("Failed to build rectangle wire");

  BRepBuilderAPI_MakeFace makeFace(plane.plane(), polygon.Wire(), Standard_True);
  if (!makeFace.IsDone()) return Result<TopoDS_Face>::failure("Failed to build rectangle face");
  TopoDS_Face face = makeFace.Face();
  const auto n = planarFaceNormal(face);
  if (n && n->Dot(plane.normal) < 0) face.Reverse();
  return face;
}

Result<TopoDS_Face> makeCircleFace(const SketchPlane& plane, double radius) {
  if (!std::isfinite(radius) || radius < tolerance::kMinDimension) {
    return Result<TopoDS_Face>::failure("Circle radius must be positive");
  }
  const gp_Circ circle(gp_Ax2(plane.origin, plane.normal, plane.xDir), radius);
  const TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(circle).Edge();
  const TopoDS_Wire wire = BRepBuilderAPI_MakeWire(edge).Wire();
  BRepBuilderAPI_MakeFace makeFace(plane.plane(), wire, Standard_True);
  if (!makeFace.IsDone()) return Result<TopoDS_Face>::failure("Failed to build circle face");
  TopoDS_Face face = makeFace.Face();
  const auto n = planarFaceNormal(face);
  if (n && n->Dot(plane.normal) < 0) face.Reverse();
  return face;
}

Result<TopoDS_Face> makePolygonFace(const std::vector<gp_Pnt>& input) {
  const std::vector<gp_Pnt> points = withoutClosingPoint(input);
  if (points.size() < 3) return Result<TopoDS_Face>::failure("A face needs at least 3 points");
  if (hasConsecutiveDuplicates(points, true)) {
    return Result<TopoDS_Face>::failure("Polygon has coincident consecutive points");
  }
  gp_Pln plane;
  if (!arePointsCoplanar(points, &plane)) {
    return Result<TopoDS_Face>::failure("Polygon points are not coplanar");
  }

  BRepBuilderAPI_MakePolygon polygon;
  for (const gp_Pnt& p : points) polygon.Add(p);
  polygon.Close();
  if (!polygon.IsDone()) return Result<TopoDS_Face>::failure("Failed to build polygon wire");

  // Newell orientation follows the point order, so the face normal does too.
  BRepBuilderAPI_MakeFace makeFace(plane, polygon.Wire(), Standard_True);
  if (!makeFace.IsDone()) return Result<TopoDS_Face>::failure("Failed to build polygon face");
  TopoDS_Face face = makeFace.Face();

  BRepCheck_Analyzer analyzer(face);
  if (!analyzer.IsValid()) {
    return Result<TopoDS_Face>::failure("Polygon is invalid (self-intersecting?)");
  }
  if (area(face) < tolerance::kMinDimension * tolerance::kMinDimension) {
    return Result<TopoDS_Face>::failure("Polygon has zero area");
  }
  return face;
}

Result<TopoDS_Wire> makePolylineWire(const std::vector<gp_Pnt>& points) {
  if (points.size() < 2) return Result<TopoDS_Wire>::failure("A line needs at least 2 points");
  if (hasConsecutiveDuplicates(points, false)) {
    return Result<TopoDS_Wire>::failure("Line has coincident consecutive points");
  }
  BRepBuilderAPI_MakePolygon polygon;
  for (const gp_Pnt& p : points) polygon.Add(p);
  if (!polygon.IsDone()) return Result<TopoDS_Wire>::failure("Failed to build polyline");
  return polygon.Wire();
}

std::pair<double, double> bulgeArcMiddle(double ax, double ay, double bx, double by, double bulge) {
  // The sagitta point: right of the chord for a counter-clockwise arc;
  // keeps major and clockwise arcs too.
  return {(ax + bx) / 2 + (by - ay) * bulge / 2, (ay + by) / 2 - (bx - ax) * bulge / 2};
}

}  // namespace sbr::geom
