#include <algorithm>
#include <chrono>
#include <cmath>

#include <BRepAdaptor_Surface.hxx>
#include <BRepTools.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include "TestHelpers.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Freeform.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"
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
  SketchPlane aside = SketchPlane::ground();
  aside.origin = gp_Pnt(5000, 5000, 0);  // not on the base (it would divide it)
  auto top = m.createRectangle(aside, 1000, 1000);
  ASSERT_TRUE(m.move({*top}, gp_Vec(-4500, -4500, 3000)).ok());  // placement is captured
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

TEST(Freeform, ExactArcAndSegmentFace) {
  // Semicircle of radius 1000 in the ground plane, bulging towards +Y.
  auto arc = makeArc(gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp::DZ(), 1000, false);
  ASSERT_TRUE(arc.ok()) << arc.error();
  const Bnd_Box b = exactBounds(arc.value());
  EXPECT_NEAR(b.CornerMax().Y(), 1000, 1e-6);
  EXPECT_NEAR(b.CornerMin().Y(), 0, 1e-6);
  // Negative bulge flips the side.
  EXPECT_NEAR(exactBounds(makeArc(gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp::DZ(), -300, false).value())
                  .CornerMin().Y(), -300, 1e-6);

  auto segment = makeArc(gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp::DZ(), 1000, true);
  ASSERT_TRUE(segment.ok());
  EXPECT_NEAR(area(segment.value()), M_PI * 1000 * 1000 / 2, 1e-3);
  const auto n = planarFaceNormal(TopoDS::Face(segment.value()));
  ASSERT_TRUE(n);
  EXPECT_GT(n->Z(), 0.99);

  EXPECT_FALSE(makeArc(gp_Pnt(0, 0, 0), gp_Pnt(0, 0, 0), gp::DZ(), 10, false).ok());
  EXPECT_FALSE(makeArc(gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp::DZ(), 0, false).ok());
  EXPECT_FALSE(makeArc(gp_Pnt(0, 0, 0), gp_Pnt(0, 0, 1000), gp::DZ(), 10, false).ok());  // chord off-plane
}

TEST(Freeform, ArchedOpeningFromSegmentIsParametric) {
  doc::Modeler m;
  // A segment profile on a vertical plane (facing -Y), extruded into a solid.
  auto id = m.createArc(gp_Pnt(0, 0, 2000), gp_Pnt(1000, 0, 2000), -gp::DY(), 300, true);
  ASSERT_TRUE(id.ok()) << id.error();
  const doc::Object* o = m.document().find(*id);
  EXPECT_EQ(o->kind(), doc::ObjectKind::Face);
  EXPECT_GT(exactBounds(o->shape).CornerMax().Z(), 2299.0);  // bulges upwards
  const auto r = m.pushPull(*id, TopoDS::Face(o->shape), 200);
  ASSERT_TRUE(r.ok()) << r.error();
  const double v1 = volume(m.document().worldShape(*id));
  ASSERT_TRUE(m.setParameter(*id, "bulge", 500).ok());
  EXPECT_GT(volume(m.document().worldShape(*id)), v1);

  const std::string text = doc::serializeDocument(m.document());
  doc::Document d;
  ASSERT_TRUE(doc::deserializeDocument(text, d).ok());
  const auto* loaded = std::get_if<doc::ArcCurve>(&d.find(*id)->recipe.base);
  ASSERT_TRUE(loaded);
  EXPECT_DOUBLE_EQ(loaded->bulge, 500);
  EXPECT_TRUE(loaded->closed);
}

TEST(Freeform, LinesAndArcCloseIntoParametricFace) {
  doc::Modeler m;
  // Arched window outline on a vertical plane facing -Y: arc on top, lines below.
  auto arc = m.createArc(gp_Pnt(0, 0, 2000), gp_Pnt(1000, 0, 2000), -gp::DY(), 500, false);
  ASSERT_TRUE(arc.ok()) << arc.error();
  EXPECT_FALSE(m.faceFromLoop(*arc).ok());  // not closed yet
  auto lines = m.createPolyline({gp_Pnt(1000, 0, 2000), gp_Pnt(1000, 0, 0), gp_Pnt(0, 0, 0), gp_Pnt(0, 0, 2000)});
  ASSERT_TRUE(lines.ok());
  EXPECT_TRUE(m.isLooseCurveEnd(gp_Pnt(0, 0, 2000)));
  const std::size_t steps = m.history().undoCount();

  auto face = m.faceFromLoop(*lines);
  ASSERT_TRUE(face.ok()) << face.error();
  EXPECT_EQ(m.document().objects().size(), 1u);  // the curves became the face's edges
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  const doc::Object* o = m.document().find(*face);
  EXPECT_EQ(o->kind(), doc::ObjectKind::Face);
  EXPECT_NEAR(area(o->shape), 1000.0 * 2000 + M_PI * 500 * 500 / 2, 1e-2);

  // The arc stays parametric inside the face.
  ASSERT_TRUE(m.setParameter(*face, "in1.bulge", 300).ok());
  EXPECT_NEAR(exactBounds(m.document().worldShape(*face)).CornerMax().Z(), 2300, 1e-6);
  // Push/pull it into a solid (an arched opening cutter).
  const auto r = m.pushPull(*face, TopoDS::Face(m.document().find(*face)->shape), 200);
  ASSERT_TRUE(r.ok()) << r.error();
  EXPECT_GT(volume(m.document().worldShape(*face)), 1000.0 * 2000 * 200);

  doc::Document d;
  ASSERT_TRUE(doc::deserializeDocument(doc::serializeDocument(m.document()), d).ok());
  EXPECT_NEAR(volume(d.worldShape(*d.find(*face))), volume(m.document().worldShape(*face)), 1e-3);

  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());  // face creation undone in one step: the curves are back
  EXPECT_EQ(m.document().objects().size(), 2u);
}

namespace {
TopoDS_Face firstFaceOfType(const TopoDS_Shape& shape, GeomAbs_SurfaceType type) {
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
    if (BRepAdaptor_Surface(TopoDS::Face(e.Current())).GetType() == type) return TopoDS::Face(e.Current());
  }
  return {};
}
}  // namespace

TEST(Freeform, PushPullCylinderSideChangesRadius) {
  doc::Modeler m;
  auto c = m.createCircle(SketchPlane::ground(), 500);
  ASSERT_TRUE(m.extrude(*c, 1000).ok());
  const TopoDS_Face side = firstFaceOfType(m.document().worldShape(*c), GeomAbs_Cylinder);
  ASSERT_FALSE(side.IsNull());
  auto r = m.pushPull(*c, side, 100);
  ASSERT_TRUE(r.ok()) << r.error();
  EXPECT_EQ(r->parameterKey, "radius");
  EXPECT_NEAR(volume(m.document().worldShape(*c)), M_PI * 600 * 600 * 1000, 1.0);
}

TEST(Freeform, PushPullCurvedFaceOfComplexSolidIsParametric) {
  doc::Modeler m;
  // A slab with a round column on it (union): the column's side is pushed out.
  auto slab = m.createRectangle(SketchPlane::ground(), 2000, 2000);
  ASSERT_TRUE(m.extrude(*slab, 500).ok());
  SketchPlane top = SketchPlane::ground();
  top.origin = gp_Pnt(1000, 1000, 500);
  auto column = m.createCircle(top, 300);
  ASSERT_TRUE(m.extrude(*column, 1000).ok());
  auto u = m.boolean(doc::BooleanKind::Union, {*slab, *column});
  ASSERT_TRUE(u.ok()) << u.error();
  const double v0 = volume(m.document().worldShape(*u));
  const TopoDS_Face side = firstFaceOfType(m.document().worldShape(*u), GeomAbs_Cylinder);
  ASSERT_FALSE(side.IsNull());
  auto r = m.pushPull(*u, side, 50);
  ASSERT_TRUE(r.ok()) << r.error();
  EXPECT_NEAR(volume(m.document().worldShape(*u)) - v0, M_PI * (350.0 * 350 - 300.0 * 300) * 1000, 10.0);

  // Editing the column height upstream keeps the pushed radius.
  ASSERT_TRUE(m.setParameter(*u, "in1.op0.distance", 2000).ok());
  EXPECT_NEAR(volume(m.document().worldShape(*u)), 2000.0 * 2000 * 500 + M_PI * 350 * 350 * 2000, 50.0);
  ASSERT_TRUE(m.setParameter(*u, r->parameterKey, -100).ok());
  EXPECT_NEAR(volume(m.document().worldShape(*u)), 2000.0 * 2000 * 500 + M_PI * 200 * 200 * 2000, 50.0);

  doc::Document d;
  ASSERT_TRUE(doc::deserializeDocument(doc::serializeDocument(m.document()), d).ok());
  EXPECT_NEAR(volume(d.worldShape(*d.find(*u))), volume(m.document().worldShape(*u)), 1e-3);
}

TEST(Freeform, FollowMeSweepsAndConsumesTheProfile) {
  doc::Modeler m;
  // A 100 x 200 profile standing at the start of a 3 m path along X.
  auto profile = m.createRectangle(SketchPlane::fromNormal(gp_Pnt(0, 0, 0), gp::DX()), 100, 200);
  ASSERT_TRUE(profile.ok());
  auto path = m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(3000, 0, 0)});
  ASSERT_TRUE(path.ok());
  const std::size_t steps = m.history().undoCount();
  auto swept = m.followMe(*profile, *path);
  ASSERT_TRUE(swept.ok()) << swept.error();
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  EXPECT_FALSE(m.document().find(*profile));  // consumed
  EXPECT_TRUE(m.document().find(*path));
  EXPECT_NEAR(volume(m.document().worldShape(*swept)), 100.0 * 200 * 3000, 1.0);
  // Still parametric in the profile.
  ASSERT_TRUE(m.setParameter(*swept, "in0.width", 150).ok());
  EXPECT_NEAR(volume(m.document().worldShape(*swept)), 150.0 * 200 * 3000, 1.0);
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  EXPECT_TRUE(m.document().find(*profile));

  // Along the outline of a box's top face (e.g. a cornice).
  auto box = m.createRectangle(SketchPlane::ground(), 2000, 1000);
  ASSERT_TRUE(m.extrude(*box, 500).ok());
  TopoDS_Face top;
  for (TopExp_Explorer e(m.document().worldShape(*box), TopAbs_FACE); e.More(); e.Next()) {
    const auto n = planarFaceNormal(TopoDS::Face(e.Current()));
    if (n && n->Z() > 0.99) top = TopoDS::Face(e.Current());
  }
  auto cornice = m.createRectangle(SketchPlane::fromNormal(gp_Pnt(0, 0, 500), gp::DX()), 100, 100);
  ASSERT_TRUE(cornice.ok());
  auto ring = m.followMe(*cornice, BRepTools::OuterWire(top));
  ASSERT_TRUE(ring.ok()) << ring.error();
  EXPECT_TRUE(validateShape(m.document().worldShape(*ring), true).valid);
  EXPECT_GT(volume(m.document().worldShape(*ring)), 0.0);
}

// A curve joined to a grid of loose lines it does not close against: the
// loop search ends at once (a walk through every route took minutes), and
// a curve that does close takes the shortest way round (the smaller face).
TEST(Freeform, LoopSearchStaysQuickInAGridOfLines) {
  doc::Modeler m;
  constexpr int kCells = 8;
  for (int i = 0; i <= kCells; ++i) {
    for (int j = 0; j < kCells; ++j) {
      ASSERT_TRUE(m.createPolyline({gp_Pnt(j * 1000.0, i * 1000.0, 0), gp_Pnt((j + 1) * 1000.0, i * 1000.0, 0)}).ok());
      ASSERT_TRUE(m.createPolyline({gp_Pnt(i * 1000.0, j * 1000.0, 0), gp_Pnt(i * 1000.0, (j + 1) * 1000.0, 0)}).ok());
    }
  }
  auto outside = m.createArc(gp_Pnt(-3000, -3000, 0), gp_Pnt(0, 0, 0), gp::DZ(), 300, false);
  ASSERT_TRUE(outside.ok()) << outside.error();
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(m.faceFromLoop(*outside).ok());
  EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), 2.0);

  // Two cells side by side, open on the left: closing it makes one cell.
  doc::Modeler n;
  for (const auto& [a, b] : std::vector<std::pair<gp_Pnt, gp_Pnt>>{
           {gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0)}, {gp_Pnt(1000, 0, 0), gp_Pnt(2000, 0, 0)},
           {gp_Pnt(2000, 0, 0), gp_Pnt(2000, 1000, 0)}, {gp_Pnt(2000, 1000, 0), gp_Pnt(1000, 1000, 0)},
           {gp_Pnt(1000, 1000, 0), gp_Pnt(0, 1000, 0)}, {gp_Pnt(1000, 0, 0), gp_Pnt(1000, 1000, 0)}}) {
    ASSERT_TRUE(n.createPolyline({a, b}).ok());
  }
  auto closing = n.createArc(gp_Pnt(0, 1000, 0), gp_Pnt(0, 0, 0), gp::DZ(), -1e-9, false);
  if (!closing.ok()) closing = n.createPolyline({gp_Pnt(0, 1000, 0), gp_Pnt(0, 0, 0)});
  ASSERT_TRUE(closing.ok()) << closing.error();
  auto face = n.faceFromLoop(*closing);
  ASSERT_TRUE(face.ok()) << face.error();
  EXPECT_NEAR(area(n.document().find(*face)->shape), 1e6, 1e-3);
}

// Loose ends: the ends of lines no other line meets (an end shared by two
// lines is not one).
TEST(Freeform, OpenCurveEndsAreTheUnsharedOnes) {
  doc::Modeler m;
  ASSERT_TRUE(m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0)}).ok());
  ASSERT_TRUE(m.createPolyline({gp_Pnt(1000, 0, 0), gp_Pnt(1000, 1000, 0)}).ok());
  ASSERT_TRUE(m.createPolyline({gp_Pnt(5000, 0, 0), gp_Pnt(6000, 0, 0)}).ok());
  const auto ends = m.openCurveEnds();
  ASSERT_EQ(ends.size(), 4u);
  auto has = [&](const gp_Pnt& p) {
    return std::any_of(ends.begin(), ends.end(), [&](const gp_Pnt& e) { return e.Distance(p) < 1e-6; });
  };
  EXPECT_TRUE(has(gp_Pnt(0, 0, 0)));
  EXPECT_TRUE(has(gp_Pnt(1000, 1000, 0)));
  EXPECT_TRUE(has(gp_Pnt(5000, 0, 0)));
  EXPECT_TRUE(has(gp_Pnt(6000, 0, 0)));
  EXPECT_FALSE(has(gp_Pnt(1000, 0, 0)));
}

// PERF-01: a long slanted line registers the cells it passes through, not
// every cell of its bounding box (a 1 km diagonal covered a million cells).
TEST(Freeform, LongSlantedLinesStayQuickToCross) {
  for (const double length : {100e3, 1e6}) {
    doc::Modeler m;
    ASSERT_TRUE(m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(length, length * 0.7, 0)}).ok());
    ASSERT_TRUE(m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(length * 0.7, length, 0)}).ok());
    // Across both near their start: a triangle between the three.
    auto across = m.createPolyline({gp_Pnt(1000, 2000, 0), gp_Pnt(2000, 1000, 0)});
    ASSERT_TRUE(across.ok());
    const auto start = std::chrono::steady_clock::now();
    auto faces = m.facesFromCrossings(*across);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("long line %.0f mm: %.3f s\n", length, seconds);
    ASSERT_TRUE(faces.ok()) << faces.error();
    ASSERT_EQ(faces->size(), 1u);
    // The triangle (0,0), (1235.3,1764.7), (1764.7,1235.3).
    EXPECT_NEAR(area(m.document().find(faces->front())->shape), 0.5 * std::abs(1235.294 * 1235.294 - 1764.706 * 1764.706), 1.0);
    EXPECT_LT(seconds, 1.0);
  }
}
