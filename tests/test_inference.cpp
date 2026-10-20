#include <gtest/gtest.h>

#include "sbr/geom/Operations.h"
#include "sbr/geom/Profile.h"
#include "sbr/snap/Inference.h"

using namespace sbr;
using namespace sbr::snap;

namespace {

// Orthographic top view: 1 px == 10 mm, looking down -Z.
struct TopView {
  double mmPerPixel = 10.0;
  Projector projector() const {
    return [s = mmPerPixel](const gp_Pnt& p) { return ScreenPoint{p.X() / s, -p.Y() / s}; };
  }
  Ray rayAt(double px, double py) const {
    return Ray{gp_Pnt(px * mmPerPixel, -py * mmPerPixel, 100000), gp_Dir(0, 0, -1)};
  }
  SnapQuery query(double px, double py) const {
    SnapQuery q;
    q.ray = rayAt(px, py);
    q.cursor = ScreenPoint{px, py};
    q.project = projector();
    q.pixelTolerance = 8;
    return q;
  }
};

TopoDS_Shape box() {
  auto face = geom::makeRectangleFace(geom::SketchPlane::ground(), 1000, 1000);
  return geom::extrudeFace(face.value(), 500).value();
}

}  // namespace

TEST(Inference, FallsBackToWorkPlane) {
  InferenceEngine engine;
  TopView view;
  const SnapResult r = engine.snap(view.query(250, -300));
  EXPECT_EQ(r.kind, SnapKind::WorkPlane);
  EXPECT_NEAR(r.point.X(), 2500, 1e-6);
  EXPECT_NEAR(r.point.Y(), 3000, 1e-6);
  EXPECT_NEAR(r.point.Z(), 0, 1e-6);
}

TEST(Inference, SnapsToOrigin) {
  InferenceEngine engine;
  TopView view;
  const SnapResult r = engine.snap(view.query(3, 2));
  EXPECT_EQ(r.kind, SnapKind::Origin);
  EXPECT_NEAR(r.point.Distance(gp_Pnt(0, 0, 0)), 0, 1e-9);
}

TEST(Inference, SnapsToExactVertexNotMesh) {
  InferenceEngine engine;
  engine.setShapes({box()});
  TopView view;
  // Corner (1000, 1000) is at pixel (100, -100); cursor is 3 px away.
  const SnapResult r = engine.snap(view.query(102, -98));
  EXPECT_EQ(r.kind, SnapKind::Endpoint);
  EXPECT_NEAR(r.point.X(), 1000, 1e-9);
  EXPECT_NEAR(r.point.Y(), 1000, 1e-9);
  EXPECT_EQ(r.shapeIndex, 0);
}

TEST(Inference, SnapsToMidpoint) {
  InferenceEngine engine;
  engine.setShapes({box()});
  TopView view;
  const SnapResult r = engine.snap(view.query(51, 1));
  EXPECT_EQ(r.kind, SnapKind::Midpoint);
  EXPECT_NEAR(r.point.X(), 500, 1e-9);
  EXPECT_NEAR(r.point.Y(), 0, 1e-9);
}

TEST(Inference, SnapsOnEdge) {
  InferenceEngine engine;
  engine.setShapes({box()});
  TopView view;
  const SnapResult r = engine.snap(view.query(23, 2));
  EXPECT_EQ(r.kind, SnapKind::OnEdge);
  EXPECT_NEAR(r.point.Y(), 0, 1e-6);
  EXPECT_NEAR(r.point.X(), 230, 1e-6);
}

TEST(Inference, HitsTopFaceWithNormal) {
  InferenceEngine engine;
  engine.setShapes({box()});
  TopView view;
  const SnapResult r = engine.snap(view.query(40, -60));
  EXPECT_EQ(r.kind, SnapKind::OnFace);
  EXPECT_NEAR(r.point.Z(), 500, 1e-6);
  ASSERT_TRUE(r.faceNormal);
  EXPECT_NEAR(r.faceNormal->Z(), 1.0, 1e-9);
  EXPECT_FALSE(r.face.IsNull());
}

TEST(Inference, InfersAxisFromAnchor) {
  InferenceEngine engine;
  TopView view;
  SnapQuery q = view.query(300, -205);  // close to the X axis through (0, 2000)
  q.anchor = gp_Pnt(0, 2000, 0);
  const SnapResult r = engine.snap(q);
  EXPECT_EQ(r.kind, SnapKind::AxisX);
  EXPECT_NEAR(r.point.Y(), 2000, 1e-6);
  EXPECT_NEAR(r.point.X(), 3000, 1e-6);
}

TEST(Inference, ShiftLockConstrainsToDirection) {
  InferenceEngine engine;
  TopView view;
  SnapQuery q = view.query(300, -150);  // far from the axis
  q.anchor = gp_Pnt(0, 0, 0);
  q.lockedDirection = gp_Dir(1, 0, 0);
  const SnapResult r = engine.snap(q);
  EXPECT_EQ(r.kind, SnapKind::Locked);
  EXPECT_NEAR(r.point.Y(), 0, 1e-6);
  EXPECT_NEAR(r.point.X(), 3000, 1e-6);
}

TEST(Inference, ClosestPointOnLineToRay) {
  const Ray ray{gp_Pnt(500, 100, 1000), gp_Dir(0, 0, -1)};
  const gp_Pnt p = closestPointOnLineToRay(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0), ray);
  EXPECT_NEAR(p.X(), 500, 1e-9);
  EXPECT_NEAR(p.Y(), 0, 1e-9);
}
