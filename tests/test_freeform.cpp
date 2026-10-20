#include <cmath>

#include <BRepBuilderAPI_MakePolygon.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include "TestHelpers.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Freeform.h"
#include "sbr/geom/Measure.h"
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

  EXPECT_EQ(*revolved, *ring);  // face profiles are revolved in place
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(m.document().find(*ring)->kind(), doc::ObjectKind::Face);
  const std::size_t count = m.document().objects().size();
  ASSERT_TRUE(m.undo());  // the sweep
  EXPECT_EQ(m.document().objects().size(), count - 1);
  EXPECT_FALSE(m.loftObjects({*base}, true).ok());
}

TEST(Freeform, RevolveIsParametricAndSaved) {
  doc::Modeler m;
  SketchPlane wall = SketchPlane::fromNormal(gp_Pnt(0, 0, 0), gp_Dir(0, -1, 0));
  auto profile = m.createRectangle(wall, 500, 2000);  // x 0..500, z 0..2000
  ASSERT_TRUE(profile.ok());
  ASSERT_TRUE(m.move({*profile}, gp_Vec(0, 0, 100)).ok());  // axis given in world space
  auto id = m.revolveObject(*profile, gp_Ax1(gp_Pnt(0, 0, 0), gp::DZ()), 2 * M_PI);
  ASSERT_TRUE(id.ok()) << id.error();
  const double full = M_PI * 500 * 500 * 2000;
  EXPECT_NEAR(volume(m.document().worldShape(*id)), full, 1.0);

  bool found = false;
  for (const doc::Parameter& p : m.parameters(*id)) {
    if (p.key == "op0.angle") {
      found = true;
      EXPECT_NEAR(p.value, 360.0, 1e-9);
      EXPECT_FALSE(p.isLength);
    }
  }
  ASSERT_TRUE(found);
  ASSERT_TRUE(m.setParameter(*id, "op0.angle", 90).ok());
  EXPECT_NEAR(volume(m.document().worldShape(*id)), full / 4, 1.0);
  test::expectBounds(m.document().worldShape(*id), 0, 0, 100, 500, 500, 2100, 1e-6);

  const std::string path = test::tempPath("revolve.sbr");
  ASSERT_TRUE(m.save(path).ok());
  doc::Modeler loaded;
  std::vector<std::string> warnings;
  ASSERT_TRUE(loaded.load(path, &warnings).ok());
  EXPECT_TRUE(warnings.empty());
  ASSERT_TRUE(loaded.setParameter(*id, "op0.angle", 180).ok());
  EXPECT_NEAR(volume(loaded.document().worldShape(*id)), full / 2, 1.0);
}

TEST(Freeform, LoftIsParametricAndSaved) {
  doc::Modeler m;
  auto base = m.createRectangle(SketchPlane::ground(), 2000, 2000);
  auto top = m.createRectangle(SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(m.move({*top}, gp_Vec(500, 500, 3000)).ok());  // placement is captured
  auto loftId = m.loftObjects({*base, *top}, true);
  ASSERT_TRUE(loftId.ok()) << loftId.error();
  EXPECT_EQ(m.document().objects().size(), 3u);  // sections are kept
  auto frustum = [](double a1, double a2) { return 3000.0 / 3 * (a1 + a2 + std::sqrt(a1 * a2)); };
  EXPECT_NEAR(volume(m.document().worldShape(*loftId)), frustum(4e6, 1e6), 1.0);

  // Section parameters stay editable through the loft: top 1000 -> 2000 square.
  ASSERT_TRUE(m.setParameter(*loftId, "in1.width", 2000).ok());
  ASSERT_TRUE(m.setParameter(*loftId, "in1.height", 2000).ok());
  EXPECT_NEAR(volume(m.document().worldShape(*loftId)), 2000.0 * 2000 * 3000, 1.0);

  const std::string path = test::tempPath("loft.sbr");
  ASSERT_TRUE(m.save(path).ok());
  doc::Modeler loaded;
  std::vector<std::string> warnings;
  ASSERT_TRUE(loaded.load(path, &warnings).ok());
  EXPECT_TRUE(warnings.empty());
  ASSERT_TRUE(loaded.setParameter(*loftId, "in0.width", 1000).ok());
  EXPECT_TRUE(validateShape(loaded.document().worldShape(*loftId), true).valid);
}

TEST(Freeform, SweepIsParametricAndSaved) {
  doc::Modeler m;
  auto path = m.createCurve({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0)}, false);  // straight spline
  SketchPlane yz = SketchPlane::fromNormal(gp_Pnt(0, 0, 0), gp::DX());
  auto profile = m.createRectangle(yz, 100, 200);
  auto id = m.sweepObject(*profile, *path);
  ASSERT_TRUE(id.ok()) << id.error();
  EXPECT_NEAR(volume(m.document().worldShape(*id)), 100.0 * 200 * 2000, 1.0);
  ASSERT_TRUE(m.setParameter(*id, "in0.width", 300).ok());
  EXPECT_NEAR(volume(m.document().worldShape(*id)), 300.0 * 200 * 2000, 1.0);
  EXPECT_FALSE(m.sweepObject(*path, *profile).ok());  // path must be a curve

  const std::string file = test::tempPath("sweep.sbr");
  ASSERT_TRUE(m.save(file).ok());
  doc::Modeler loaded;
  std::vector<std::string> warnings;
  ASSERT_TRUE(loaded.load(file, &warnings).ok());
  EXPECT_TRUE(warnings.empty());
  EXPECT_NEAR(volume(loaded.document().worldShape(*id)), 300.0 * 200 * 2000, 1.0);
}

TEST(Freeform, CurveStoresThroughPoints) {
  doc::Modeler m;
  const std::vector<gp_Pnt> pts = {gp_Pnt(0, 0, 0), gp_Pnt(1500, 800, 0), gp_Pnt(3000, 0, 500)};
  auto curve = m.createCurve(pts, false);
  ASSERT_TRUE(curve.ok()) << curve.error();
  const doc::Object* o = m.document().find(*curve);
  ASSERT_EQ(o->kind(), doc::ObjectKind::Curve);
  const auto* recipe = std::get_if<doc::InterpolatedCurve>(&o->recipe.base);
  ASSERT_TRUE(recipe);
  EXPECT_EQ(recipe->points.size(), 3u);

  const std::string text = doc::serializeDocument(m.document());
  EXPECT_NE(text.find("\"type\": \"curve\""), std::string::npos);
  doc::Document d;
  ASSERT_TRUE(doc::deserializeDocument(text, d).ok());
  const auto* loaded = std::get_if<doc::InterpolatedCurve>(&d.find(*curve)->recipe.base);
  ASSERT_TRUE(loaded);
  EXPECT_NEAR(loaded->points[1].Y(), 800, 1e-12);
  EXPECT_FALSE(m.createCurve({gp_Pnt(0, 0, 0), gp_Pnt(0, 0, 0)}, false).ok());
}

TEST(Freeform, CurvePointsAreEditable) {
  doc::Modeler m;
  auto curve = m.createCurve({gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp_Pnt(2000, 0, 0)}, false);
  ASSERT_TRUE(curve.ok());
  ASSERT_TRUE(m.setCurvePoints(*curve, {gp_Pnt(0, 0, 0), gp_Pnt(1000, 800, 0), gp_Pnt(2000, 0, 0)}).ok());
  EXPECT_NEAR(geom::exactBounds(m.document().worldShape(*curve)).CornerMax().Y(), 800, 1.0);
  EXPECT_EQ(m.history().undoName(), "Edit Curve");
  // Invalid edits are rejected without changing anything.
  EXPECT_FALSE(m.setCurvePoints(*curve, {gp_Pnt(0, 0, 0)}).ok());
  ASSERT_TRUE(m.undo());
  EXPECT_NEAR(geom::exactBounds(m.document().worldShape(*curve)).CornerMax().Y(), 0, 1.0);
  auto rect = m.createRectangle(SketchPlane::ground(), 10, 10);
  EXPECT_FALSE(m.setCurvePoints(*rect, {gp_Pnt(0, 0, 0), gp_Pnt(1, 0, 0)}).ok());
}
