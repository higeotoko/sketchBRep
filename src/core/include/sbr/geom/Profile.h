#pragma once
// Construction of planar sketch profiles (wires / faces).

#include <utility>
#include <vector>

#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>

#include "sbr/Types.h"

namespace sbr::geom {

// A local 2D frame on which sketches are drawn.
struct SketchPlane {
  gp_Pnt origin{0, 0, 0};
  gp_Dir normal{0, 0, 1};
  gp_Dir xDir{1, 0, 0};

  // The ground plane (world XY, normal +Z).
  static SketchPlane ground();
  // Builds a plane through `origin` with the given normal. The x direction is
  // chosen so that rectangles drawn on walls stay aligned with world axes:
  // horizontal planes use world X, other planes use the horizontal direction.
  static SketchPlane fromNormal(const gp_Pnt& origin, const gp_Dir& normal);

  gp_Dir yDir() const { return normal.Crossed(xDir); }
  gp_Ax3 ax3() const { return gp_Ax3(origin, normal, xDir); }
  gp_Pln plane() const { return gp_Pln(ax3()); }
  // Local (u, v) coordinates of a world point projected onto the plane.
  void toLocal(const gp_Pnt& p, double& u, double& v) const;
  gp_Pnt toWorld(double u, double v) const;
};

// Rectangle with one corner at plane.origin, spanning `width` along xDir and
// `height` along yDir. Negative values flip the direction. The resulting face
// normal is always plane.normal.
Result<TopoDS_Face> makeRectangleFace(const SketchPlane& plane, double width, double height);

// Circle centred at plane.origin; face normal = plane.normal.
Result<TopoDS_Face> makeCircleFace(const SketchPlane& plane, double radius);

// Closed planar polygon (the closing segment is implicit). Fails for
// non-planar, degenerate or self-intersecting input.
Result<TopoDS_Face> makePolygonFace(const std::vector<gp_Pnt>& points);

// Open polyline. Consecutive duplicate points are rejected.
Result<TopoDS_Wire> makePolylineWire(const std::vector<gp_Pnt>& points);

// The middle of the arc from (ax, ay) to (bx, by) whose bulge (DXF) is
// tan(sweep / 4), positive counter-clockwise; on the chord when it is 0.
// A bulge is no distance: never pass it on as one (ArcCurve's bulge is).
std::pair<double, double> bulgeArcMiddle(double ax, double ay, double bx, double by, double bulge);

// Best-fit plane test: true when all points lie within tolerance of a plane.
bool arePointsCoplanar(const std::vector<gp_Pnt>& points, gp_Pln* planeOut = nullptr);

}  // namespace sbr::geom
