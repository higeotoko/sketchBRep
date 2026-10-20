#pragma once
// Snapping and inference evaluated on canonical B-rep geometry.
//
// Candidates come from exact vertices, edge curves and faces rather than from
// display mesh nodes. Screen-space tolerances are applied through a projector
// supplied by the viewport, which keeps this module independent from the GUI.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

namespace sbr::snap {

enum class SnapKind {
  None,
  Endpoint,
  Midpoint,
  OnEdge,
  OnFace,
  Origin,
  AxisX,
  AxisY,
  AxisZ,
  Locked,
  WorkPlane,
};
const char* toString(SnapKind kind);

struct Ray {
  gp_Pnt origin;
  gp_Dir direction;
};

struct ScreenPoint {
  double x = 0.0;
  double y = 0.0;
};

using Projector = std::function<ScreenPoint(const gp_Pnt&)>;

struct SnapQuery {
  Ray ray;
  ScreenPoint cursor;
  Projector project;
  double pixelTolerance = 10.0;
  std::optional<gp_Pnt> anchor;          // enables axis inference
  std::optional<gp_Dir> lockedDirection; // Shift lock: result constrained to anchor + t*dir
  gp_Pln workPlane{gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)};
  bool snapToGeometry = true;
};

struct SnapResult {
  SnapKind kind = SnapKind::None;
  gp_Pnt point;
  std::optional<gp_Dir> faceNormal;  // OnFace: outward normal at the point
  std::optional<gp_Dir> axis;        // Axis*/Locked: inferred direction
  int shapeIndex = -1;               // index into the shapes passed to setShapes
  TopoDS_Face face;                  // OnFace: the hit face

  bool valid() const { return kind != SnapKind::None; }
};

class InferenceEngine {
 public:
  void setShapes(const std::vector<TopoDS_Shape>& shapes);
  SnapResult snap(const SnapQuery& query) const;

 private:
  struct EdgeEntry;
  struct Cache;
  std::vector<TopoDS_Shape> shapes_;
  std::shared_ptr<Cache> cache_;
};

// Closest point on the infinite line (p, dir) to the ray.
gp_Pnt closestPointOnLineToRay(const gp_Pnt& p, const gp_Dir& dir, const Ray& ray);

}  // namespace sbr::snap
