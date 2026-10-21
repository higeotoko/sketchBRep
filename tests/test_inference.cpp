#include <cmath>

#include <gtest/gtest.h>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <TopoDS_Edge.hxx>

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

TEST(Inference, FromTrackedPoint) {
  InferenceEngine engine;
  TopView view;
  SnapQuery q = view.query(203, -150);  // near the Y axis line through (2000, 0)
  q.trackedPoints = {gp_Pnt(2000, 0, 0)};
  const SnapResult r = engine.snap(q);
  EXPECT_EQ(r.kind, SnapKind::FromPoint);
  EXPECT_NEAR(r.point.X(), 2000, 1e-6);
  EXPECT_NEAR(r.point.Y(), 1500, 1e-6);
  ASSERT_TRUE(r.referencePoint);
}

TEST(Inference, AnchorAxisMeetsTrackedPoint) {
  InferenceEngine engine;
  TopView view;
  // Drawing along X from the origin, aligned with a point at (2500, 1800).
  SnapQuery q = view.query(252, -3);
  q.anchor = gp_Pnt(0, 0, 0);
  q.trackedPoints = {gp_Pnt(2500, 1800, 0)};
  const SnapResult r = engine.snap(q);
  EXPECT_EQ(r.kind, SnapKind::AxisX);
  EXPECT_NEAR(r.point.X(), 2500, 1e-9);
  EXPECT_NEAR(r.point.Y(), 0, 1e-9);
  ASSERT_TRUE(r.referencePoint);
}

TEST(Inference, ParallelAndPerpendicularToTrackedEdge) {
  InferenceEngine engine;
  TopView view;
  const gp_Dir diagonal(1, 1, 0);
  SnapQuery q = view.query(100, -101);  // on the 45 degree line from the origin
  q.anchor = gp_Pnt(0, 0, 0);
  q.trackedDirections = {diagonal};
  SnapResult r = engine.snap(q);
  EXPECT_EQ(r.kind, SnapKind::Parallel);
  EXPECT_NEAR(r.point.X(), r.point.Y(), 1e-6);

  q = view.query(-100, -99);  // on the -45 degree line
  q.anchor = gp_Pnt(0, 0, 0);
  q.trackedDirections = {diagonal};
  r = engine.snap(q);
  EXPECT_EQ(r.kind, SnapKind::Perpendicular);
  EXPECT_NEAR(r.point.X(), -r.point.Y(), 1e-6);
}

TEST(Inference, EdgeSnapReportsDirection) {
  InferenceEngine engine;
  engine.setShapes({box()});
  TopView view;
  const SnapResult r = engine.snap(view.query(23, 2));
  ASSERT_EQ(r.kind, SnapKind::OnEdge);
  ASSERT_TRUE(r.edgeDirection);
  EXPECT_NEAR(std::abs(r.edgeDirection->X()), 1.0, 1e-12);
}

// Spec 07 §5.1: nearby candidates do not flicker.
TEST(Inference, StickyPointHysteresis) {
  InferenceEngine engine;
  // Two end points 12 px apart on screen.
  // (Their edges run far away, so no midpoint is near.)
  engine.setShapes({TopoDS_Shape(BRepBuilderAPI_MakeEdge(gp_Pnt(2000, 0, 0), gp_Pnt(2000, 3000, 0)).Edge()),
                    TopoDS_Shape(BRepBuilderAPI_MakeEdge(gp_Pnt(2120, 0, 0), gp_Pnt(2120, 3000, 0)).Edge())});
  TopView view;
  SnapQuery q = view.query(207, 0);  // 7 px from A, 5 px from B
  EXPECT_NEAR(engine.snap(q).point.X(), 2120, 1e-6);
  q.stickyPoint = gp_Pnt(2000, 0, 0);  // A was the previous result
  EXPECT_NEAR(engine.snap(q).point.X(), 2000, 1e-6);
  SnapQuery far = view.query(213, 0);  // beyond 1.5x tolerance of A
  far.stickyPoint = gp_Pnt(2000, 0, 0);
  EXPECT_NEAR(engine.snap(far).point.X(), 2120, 1e-6);
}
