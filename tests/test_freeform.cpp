#include <cmath>

#include <BRepBuilderAPI_MakePolygon.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include "TestHelpers.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Freeform.h"
#include "sbr/geom/Profile.h"
#include "sbr/geom/Validation.h"

using namespace sbr;
using namespace sbr::geom;

TEST(Freeform, InterpolatedCurvePassesThroughPoints) {
  auto curve = makeInterpolatedCurve({gp_Pnt(0, 0, 0), gp_Pnt(1000, 500, 0), gp_Pnt(2000, 0, 300)}, false);
  ASSERT_TRUE(curve.ok()) << curve.error();
  test::expectBounds(curve.value(), 0, 0, 0, 2000, 500, 300, 50);  // spline may bulge slightly
  EXPECT_FALSE(makeInterpolatedCurve({gp_Pnt(0, 0, 0)}, false).ok());
}

TEST(Freeform, LoftBetweenSquaresIsFrustum) {
  auto wireAt = [](double half, double z) {
    BRepBuilderAPI_MakePolygon p(gp_Pnt(-half, -half, z), gp_Pnt(half, -half, z), gp_Pnt(half, half, z),
                                 gp_Pnt(-half, half, z), Standard_True);
    return p.Wire();
  };
  auto shape = loft({wireAt(1000, 0), wireAt(500, 3000)}, true);
  ASSERT_TRUE(shape.ok()) << shape.error();
  // Frustum of a square pyramid: h/3 (A1 + A2 + sqrt(A1 A2)).
  const double a1 = 4e6, a2 = 1e6;
  EXPECT_NEAR(volume(shape.value()), 3000.0 / 3 * (a1 + a2 + std::sqrt(a1 * a2)), 1.0);
}

TEST(Freeform, RevolveRectangleIsCylinder) {
  SketchPlane plane = SketchPlane::fromNormal(gp_Pnt(0, 0, 0), gp_Dir(0, -1, 0));
  auto face = makeRectangleFace(plane, 500, 2000);  // x 0..500, z 0..2000
  ASSERT_TRUE(face.ok());
  auto shape = revolve(face.value(), gp_Ax1(gp_Pnt(0, 0, 0), gp::DZ()), 2 * M_PI);
  ASSERT_TRUE(shape.ok()) << shape.error();
  EXPECT_NEAR(volume(shape.value()), M_PI * 500 * 500 * 2000, 1.0);
}

TEST(Freeform, SweepAlongCurve) {
  SketchPlane plane = SketchPlane::fromNormal(gp_Pnt(0, 0, 0), gp::DX());
  auto face = makeRectangleFace(plane, 100, 100);
  auto spine = makeInterpolatedCurve({gp_Pnt(0, 0, 0), gp_Pnt(2000, 500, 0), gp_Pnt(4000, 0, 0)}, false);
  ASSERT_TRUE(face.ok() && spine.ok());
  auto shape = sweep(face.value(), spine.value());
  ASSERT_TRUE(shape.ok()) << shape.error();
  // Profile is not perpendicular to the start tangent, so the effective
  // section is a bit smaller than 100 x 100 along the ~4.2 m spine.
  EXPECT_GT(volume(shape.value()), 3e7);
  EXPECT_TRUE(validateShape(shape.value(), true).valid);
}

TEST(Freeform, ModelerLoftRevolveSweepAreUndoable) {
  doc::Modeler m;
  geom::SketchPlane top = geom::SketchPlane::ground();
  top.origin = gp_Pnt(250, 250, 3000);
  auto base = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  auto cap = m.createRectangle(top, 500, 500);
  auto lofted = m.loftObjects({*base, *cap}, true);
  ASSERT_TRUE(lofted.ok()) << lofted.error();
  EXPECT_GT(volume(m.document().worldShape(*lofted)), 1e9);

  auto curve = m.createCurve({gp_Pnt(0, 0, 0), gp_Pnt(2000, 500, 0), gp_Pnt(4000, 0, 0)}, false);
  ASSERT_TRUE(curve.ok());
  SketchPlane yz = SketchPlane::fromNormal(gp_Pnt(0, 0, 0), gp::DX());
  auto profile = m.createRectangle(yz, 100, 100);
  auto swept = m.sweepObject(*profile, *curve);
  ASSERT_TRUE(swept.ok()) << swept.error();

  SketchPlane wall = SketchPlane::fromNormal(gp_Pnt(1000, 0, 0), gp_Dir(0, -1, 0));
  auto ring = m.createRectangle(wall, 200, 500);
  auto revolved = m.revolveObject(*ring, gp_Ax1(gp_Pnt(0, 0, 0), gp::DZ()), M_PI);
  ASSERT_TRUE(revolved.ok()) << revolved.error();
  EXPECT_TRUE(validateShape(m.document().worldShape(*revolved), true).valid);

  const std::size_t count = m.document().objects().size();
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(m.document().objects().size(), count - 1);
  EXPECT_FALSE(m.loftObjects({*base}, true).ok());
}
