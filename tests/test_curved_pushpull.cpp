// Push / pull of curved faces of every kind of solid (report 2026-11-02:
// pulling a curved face crashed the program). Each must either work or
// refuse with a reason - never crash - and stay quick.

#include <gtest/gtest.h>
#include <chrono>

#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"

using namespace sbr;
using namespace sbr::doc;

namespace {

// The first curved face of the object, pulled out and pushed in.
void pullCurved(Modeler& m, const ObjectId& id, const char* what) {
  const TopoDS_Shape s = m.document().worldShape(*m.document().find(id));
  const double before = geom::volume(s);
  for (TopExp_Explorer x(s, TopAbs_FACE); x.More(); x.Next()) {
    const TopoDS_Face f = TopoDS::Face(x.Current());
    if (BRepAdaptor_Surface(f).GetType() == GeomAbs_Plane) continue;
    for (double d : {10.0, 100.0, -10.0}) {
      const auto t0 = std::chrono::steady_clock::now();
      const auto r = geom::offsetSolidFace(s, f, d);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      ASSERT_TRUE(r) << what << " " << d << ": " << r.error();
      EXPECT_LT(ms, 2000.0) << what;
      if (d > 0) EXPECT_GT(geom::volume(*r), before) << what;
      else EXPECT_LT(geom::volume(*r), before) << what;
    }
    return;
  }
  FAIL() << what << ": no curved face";
}

}  // namespace

TEST(CurvedPushPull, EveryKindOfSolid) {
  Modeler m;
  const ObjectId cylinder = *m.createCircle(geom::SketchPlane::ground(), 500);
  ASSERT_TRUE(m.extrude(cylinder, 1000));
  pullCurved(m, cylinder, "cylinder");

  // A loft of two circles: a cone (this one crashed: "done" with no shape).
  const ObjectId c1 = *m.createCircle(geom::SketchPlane::ground(), 500);
  geom::SketchPlane up = geom::SketchPlane::ground();
  up.origin = gp_Pnt(0, 0, 1000);
  const ObjectId c2 = *m.createCircle(up, 300);
  const auto loft = m.loftObjects({c1, c2}, true);
  ASSERT_TRUE(loft) << loft.error();
  pullCurved(m, *loft, "loft");

  const ObjectId profile = *m.createRectangle(geom::SketchPlane::fromNormal(gp_Pnt(3000, 0, 0), gp::DY()), 300, 500);
  const ObjectId path = *m.createCurve({gp_Pnt(3000, 0, 0), gp_Pnt(4000, 1000, 0), gp_Pnt(5000, 0, 0)}, false);
  const auto sweep = m.sweepObject(profile, path);
  ASSERT_TRUE(sweep) << sweep.error();
  pullCurved(m, *sweep, "sweep");

  const ObjectId ring = *m.createRectangle(geom::SketchPlane::fromNormal(gp_Pnt(8000, 0, 0), gp::DY()), 300, 500);
  const auto revolve = m.revolveObject(ring, gp_Ax1(gp_Pnt(7000, 0, 0), gp::DZ()), 2 * M_PI);
  ASSERT_TRUE(revolve) << revolve.error();
  pullCurved(m, *revolve, "revolve");
}

// Through the modeler (what the tool commits), the loft's cone: one undo step.
TEST(CurvedPushPull, LoftConeCommitted) {
  Modeler m;
  const ObjectId c1 = *m.createCircle(geom::SketchPlane::ground(), 500);
  geom::SketchPlane up = geom::SketchPlane::ground();
  up.origin = gp_Pnt(0, 0, 1000);
  const ObjectId c2 = *m.createCircle(up, 300);
  const ObjectId loft = *m.loftObjects({c1, c2}, true);
  const TopoDS_Shape s = m.document().worldShape(*m.document().find(loft));
  const double before = geom::volume(s);
  for (TopExp_Explorer x(s, TopAbs_FACE); x.More(); x.Next()) {
    if (BRepAdaptor_Surface(TopoDS::Face(x.Current())).GetType() == GeomAbs_Plane) continue;
    const auto r = m.pushPull(loft, TopoDS::Face(x.Current()), 100, false);
    ASSERT_TRUE(r) << r.error();
    break;
  }
  EXPECT_GT(geom::volume(m.document().worldShape(*m.document().find(loft))), before);
  ASSERT_TRUE(m.undo());
  EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(loft))), before, 1e-3);
}
