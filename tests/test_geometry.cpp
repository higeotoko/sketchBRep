#include <cmath>

#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include "TestHelpers.h"
#include "sbr/geom/FaceRef.h"
#include "sbr/geom/Operations.h"
#include "sbr/geom/Profile.h"
#include "sbr/geom/ShapeIO.h"
#include "sbr/geom/Validation.h"

using namespace sbr;
using namespace sbr::geom;

namespace {

TopoDS_Shape makeBox(double w, double h, double d) {
  auto face = makeRectangleFace(SketchPlane::ground(), w, h);
  EXPECT_TRUE(face.ok()) << face.error();
  auto solid = extrudeFace(face.value(), d);
  EXPECT_TRUE(solid.ok()) << solid.error();
  return solid.value();
}

TopoDS_Face faceWithNormal(const TopoDS_Shape& shape, const gp_Dir& n) {
  for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
    const TopoDS_Face f = TopoDS::Face(it.Current());
    const auto fn = planarFaceNormal(f);
    if (fn && fn->Angle(n) < 1e-9) return f;
  }
  return {};
}

}  // namespace

TEST(Profile, RectangleFaceNormalFollowsPlane) {
  for (double w : {1000.0, -1000.0}) {
    for (double h : {500.0, -500.0}) {
      auto face = makeRectangleFace(SketchPlane::ground(), w, h);
      ASSERT_TRUE(face.ok());
      auto n = planarFaceNormal(face.value());
      ASSERT_TRUE(n);
      EXPECT_NEAR(n->Z(), 1.0, 1e-12) << w << "," << h;
      EXPECT_NEAR(area(face.value()), 500000.0, 1e-6);
    }
  }
}

TEST(Profile, RejectsDegenerateInput) {
  EXPECT_FALSE(makeRectangleFace(SketchPlane::ground(), 0, 100).ok());
  EXPECT_FALSE(makePolygonFace({gp_Pnt(0, 0, 0), gp_Pnt(1, 0, 0)}).ok());
  // Non planar quad.
  EXPECT_FALSE(makePolygonFace({gp_Pnt(0, 0, 0), gp_Pnt(100, 0, 0), gp_Pnt(100, 100, 10),
                                gp_Pnt(0, 100, 0)})
                   .ok());
  // Bow tie (self intersecting).
  EXPECT_FALSE(makePolygonFace({gp_Pnt(0, 0, 0), gp_Pnt(100, 100, 0), gp_Pnt(100, 0, 0),
                                gp_Pnt(0, 100, 0)})
                   .ok());
}

TEST(Profile, PolygonOnVerticalPlane) {
  auto face = makePolygonFace({gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp_Pnt(1000, 0, 2000),
                               gp_Pnt(0, 0, 2000)});
  ASSERT_TRUE(face.ok()) << face.error();
  EXPECT_NEAR(area(face.value()), 2e6, 1e-6);
}

TEST(Profile, SketchPlaneOnWallIsAxisAligned) {
  const SketchPlane p = SketchPlane::fromNormal(gp_Pnt(0, 0, 0), gp_Dir(0, -1, 0));
  EXPECT_NEAR(std::abs(p.xDir.X()), 1.0, 1e-12);
  EXPECT_NEAR(std::abs(p.yDir().Z()), 1.0, 1e-12);
}

TEST(Geometry, BoxHasExactVolumeAndTopology) {
  const TopoDS_Shape box = makeBox(1000, 2000, 3000);
  const ValidationReport r = validateShape(box, true);
  EXPECT_TRUE(r.valid) << r.summary();
  EXPECT_EQ(r.faces, 6);
  EXPECT_EQ(r.edges, 12);
  EXPECT_EQ(r.vertices, 8);
  EXPECT_DOUBLE_EQ(volume(box), 6e9);
  test::expectBounds(box, 0, 0, 0, 1000, 2000, 3000, 1e-9);
}

TEST(Geometry, NegativeExtrusionIsStillValid) {
  const TopoDS_Shape box = makeBox(1000, 1000, -500);
  const ValidationReport r = validateShape(box, true);
  EXPECT_TRUE(r.valid) << r.summary();
  EXPECT_NEAR(volume(box), 5e8, 1e-3);
  test::expectBounds(box, 0, 0, -500, 1000, 1000, 0, 1e-9);
}

TEST(Geometry, ZeroExtrusionFails) {
  auto face = makeRectangleFace(SketchPlane::ground(), 100, 100);
  EXPECT_FALSE(extrudeFace(face.value(), 0.0).ok());
}

TEST(Geometry, PushPullSideFaceOutwards) {
  const TopoDS_Shape box = makeBox(1000, 1000, 1000);
  const TopoDS_Face side = faceWithNormal(box, gp_Dir(1, 0, 0));
  ASSERT_FALSE(side.IsNull());
  auto result = pushPullFace(box, side, 500);
  ASSERT_TRUE(result.ok()) << result.error();
  EXPECT_NEAR(volume(result.value()), 1.5e9, 1e-3);
  test::expectBounds(result.value(), 0, 0, 0, 1500, 1000, 1000);
  // Coplanar faces are merged back into a clean box.
  EXPECT_EQ(validateShape(result.value(), true).faces, 6);
}

TEST(Geometry, PushPullTopFaceInwards) {
  const TopoDS_Shape box = makeBox(1000, 1000, 1000);
  const TopoDS_Face top = faceWithNormal(box, gp_Dir(0, 0, 1));
  auto result = pushPullFace(box, top, -400);
  ASSERT_TRUE(result.ok()) << result.error();
  EXPECT_NEAR(volume(result.value()), 6e8, 1e-3);
  test::expectBounds(result.value(), 0, 0, 0, 1000, 1000, 600);
}

TEST(Geometry, PushPullThroughSolidFailsWithoutSideEffects) {
  const TopoDS_Shape box = makeBox(1000, 1000, 1000);
  const TopoDS_Face top = faceWithNormal(box, gp_Dir(0, 0, 1));
  auto result = pushPullFace(box, top, -1000);
  EXPECT_FALSE(result.ok());
  EXPECT_DOUBLE_EQ(volume(box), 1e9);
  EXPECT_TRUE(validateShape(box, true).valid);
}

TEST(Geometry, ValidationRejectsOpenShapes) {
  auto face = makeRectangleFace(SketchPlane::ground(), 100, 100);
  const ValidationReport r = validateShape(face.value(), true);
  EXPECT_FALSE(r.valid);
  EXPECT_FALSE(validateShape(TopoDS_Shape(), false).valid);
}

TEST(FaceRefTest, ResolvesAfterUpstreamChange) {
  const TopoDS_Shape box = makeBox(1000, 1000, 1000);
  auto ref = FaceRef::fromFace(faceWithNormal(box, gp_Dir(1, 0, 0)));
  ASSERT_TRUE(ref.ok());
  // Same side plane, taller box: still found.
  const TopoDS_Shape taller = makeBox(1000, 1000, 3000);
  auto resolved = ref->resolve(taller);
  ASSERT_TRUE(resolved.ok()) << resolved.error();
  EXPECT_NEAR(planarFaceNormal(resolved.value())->X(), 1.0, 1e-12);
  // Wider box: the +X plane moved, so the reference cannot resolve.
  const TopoDS_Shape wider = makeBox(2000, 1000, 1000);
  EXPECT_FALSE(ref->resolve(wider).ok());
}

TEST(ShapeIOTest, BRepStringRoundTripIsExact) {
  const TopoDS_Shape box = makeBox(1234.5678, 2000.125, 3000.0625);
  const std::string text = shapeToBRepString(box);
  EXPECT_EQ(text.find("Triangulations 1"), std::string::npos);
  auto back = shapeFromBRepString(text);
  ASSERT_TRUE(back.ok()) << back.error();
  EXPECT_DOUBLE_EQ(volume(back.value()), volume(box));
  test::expectBounds(back.value(), 0, 0, 0, 1234.5678, 2000.125, 3000.0625, 1e-9);
}

TEST(ShapeIOTest, StepRoundTripPreservesDimensions) {
  const TopoDS_Shape box = makeBox(1000, 2000, 3000);
  const std::string path = test::tempPath("box.step");
  ASSERT_TRUE(writeStep(box, path).ok());
  auto back = readStep(path);
  ASSERT_TRUE(back.ok()) << back.error();
  EXPECT_TRUE(validateShape(back.value(), true).valid);
  EXPECT_NEAR(volume(back.value()), 6e9, 6e9 * 1e-9);
  test::expectBounds(back.value(), 0, 0, 0, 1000, 2000, 3000, 1e-6);
}

TEST(ShapeIOTest, ReadingMissingStepFails) {
  EXPECT_FALSE(readStep(test::tempPath("does_not_exist.step")).ok());
}
