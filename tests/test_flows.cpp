// W5 / W6 flows from the integration spec (docs/reviews/2026-10-23_ux_
// integration_108.md §7), end to end through the shared Modeler: a frame
// wall (SC01), an open shape given thickness (SC07), a roof (SC08), a face
// divided by a copied line, an arc kept exact, materials, then everything
// saved, reloaded and edited further (SC19).

#include <BRepAdaptor_Curve.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <gtest/gtest.h>

#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"
#include "sbr/geom/Validation.h"

using namespace sbr;
using namespace sbr::doc;

namespace {

geom::SketchPlane at(double x, double y, double z = 0) {
  geom::SketchPlane p = geom::SketchPlane::ground();
  p.origin = gp_Pnt(x, y, z);
  return p;
}

TopoDS_Face faceFacing(const Modeler& m, const ObjectId& id, const gp_Dir& d, std::optional<gp_Pnt> near = {}) {
  TopoDS_Face best;
  double bestDistance = 1e300;
  for (TopExp_Explorer f(m.document().worldShape(*m.document().find(id)), TopAbs_FACE); f.More(); f.Next()) {
    const TopoDS_Face face = TopoDS::Face(f.Current());
    const auto n = geom::planarFaceNormal(face);
    if (!n || n->Angle(d) > 1e-6) continue;
    const double distance = near ? geom::exactBounds(face).CornerMin().Distance(*near) : 0.0;
    if (distance < bestDistance) {
      bestDistance = distance;
      best = face;
    }
  }
  return best;
}

double volumeOf(const Modeler& m, const ObjectId& id) { return geom::volume(m.document().worldShape(*m.document().find(id))); }
double areaOf(const Modeler& m, const ObjectId& id) { return geom::area(m.document().worldShape(*m.document().find(id))); }

}  // namespace

TEST(Flows, SC01_FrameWall) {
  Modeler m;
  auto outline = m.createRectangle(at(0, 0), 4000, 3000);
  ASSERT_TRUE(outline.ok());
  auto inner = m.offset(*outline, -200);
  ASSERT_TRUE(inner.ok()) << inner.error();
  // The frame (the original, now with the inner outline as its hole).
  EXPECT_NEAR(areaOf(m, *outline), 2'640'000.0, 1e-3);
  EXPECT_NEAR(areaOf(m, inner->id), 3600.0 * 2600, 1e-3);
  auto wall = m.pushPull(*outline, faceFacing(m, *outline, gp::DZ()), 3000);
  ASSERT_TRUE(wall.ok()) << wall.error();
  EXPECT_NEAR(volumeOf(m, *outline), 7'920'000'000.0, 1.0);
  EXPECT_TRUE(geom::validateShape(m.document().worldShape(*m.document().find(*outline)), true).valid);
  // The centre is not filled: still its own face on the floor.
  EXPECT_EQ(m.document().find(inner->id)->kind(), ObjectKind::Face);
}

TEST(Flows, SC07_SC08_SC19_EditSaveReloadEdit) {
  Modeler m;
  auto red = m.createMaterial("Red", {1, 0, 0});
  // SC07: a box with its top deleted, the front given 200 of thickness.
  auto box = m.createRectangle(at(0, 0), 1000, 1000);
  ASSERT_TRUE(m.extrude(*box, 1000).ok());
  ASSERT_TRUE(m.deleteFaces(*box, {faceFacing(m, *box, gp::DZ())}, {}).ok());
  EXPECT_EQ(m.document().find(*box)->kind(), ObjectKind::Shell);
  auto thick = m.pushPull(*box, faceFacing(m, *box, gp_Dir(0, -1, 0)), 200);
  ASSERT_TRUE(thick.ok()) << thick.error();
  ASSERT_FALSE(thick->id.empty());
  EXPECT_NEAR(volumeOf(m, thick->id), 1e6 * 200, 1e-2);
  EXPECT_EQ(m.document().find(*box)->kind(), ObjectKind::Shell);  // the rest stays open
  // SC08: a 6000 x 4000 x 3000 block, top painted, split, ridge up 1500.
  auto house = m.createRectangle(at(5000, 0), 6000, 4000);
  ASSERT_TRUE(m.extrude(*house, 3000).ok());
  ASSERT_TRUE(m.paintFaces(*house, {faceFacing(m, *house, gp::DZ())}, *red).ok());
  ASSERT_TRUE(m.splitWithLine({gp_Pnt(5000, 2000, 3000), gp_Pnt(11000, 2000, 3000)}).ok());
  ASSERT_TRUE(m.moveVertices(*house, {gp_Pnt(5000, 2000, 3000), gp_Pnt(11000, 2000, 3000)}, gp_Vec(0, 0, 1500)).ok());
  const double houseVolume = 6000.0 * 4000 * 3000 + 0.5 * 4000 * 1500 * 6000;
  EXPECT_NEAR(volumeOf(m, *house), houseVolume, 1.0);
  auto paintedFaces = [&](const Modeler& mm) { return resolveFaceMaterials(*mm.document().find(*house)).faces.size(); };
  EXPECT_EQ(paintedFaces(m), 2u);  // both roof slopes keep the paint
  // A floor divided by a copied line in three; an arc kept exact.
  auto floor = m.createRectangle(at(0, 6000), 3000, 1000);
  TopoDS_Edge left;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(*floor)), TopAbs_EDGE); e.More(); e.Next()) {
    BRepAdaptor_Curve c(TopoDS::Edge(e.Current()));
    if (c.Value(0.5 * (c.FirstParameter() + c.LastParameter())).Distance(gp_Pnt(0, 6500, 0)) < 1e-6) {
      left = TopoDS::Edge(e.Current());
    }
  }
  gp_Trsf step;
  step.SetTranslation(gp_Vec(1000, 0, 0));
  ASSERT_TRUE(m.copyEdges({left}, step, 3).ok());
  auto arc = m.createArc(gp_Pnt(0, 9000, 0), gp_Pnt(2000, 9000, 0), gp::DZ(), 600);
  ASSERT_TRUE(arc.ok());
  auto countKind = [](const Modeler& mm, ObjectKind k) {
    int n = 0;
    for (const Object& o : mm.document().objects()) n += o.kind() == k;
    return n;
  };
  const int faces = countKind(m, ObjectKind::Face), solids = countKind(m, ObjectKind::Solid),
            shells = countKind(m, ObjectKind::Shell), curves = countKind(m, ObjectKind::Curve);
  EXPECT_EQ(faces, 3);

  // SC19: saved and reloaded, everything as it was.
  const std::string path = ::testing::TempDir() + "flows.sbr";
  ASSERT_TRUE(m.save(path).ok());
  Modeler back;
  std::vector<std::string> warnings;
  ASSERT_TRUE(back.load(path, &warnings).ok());
  EXPECT_TRUE(warnings.empty());
  EXPECT_EQ(countKind(back, ObjectKind::Face), faces);
  EXPECT_EQ(countKind(back, ObjectKind::Solid), solids);
  EXPECT_EQ(countKind(back, ObjectKind::Shell), shells);
  EXPECT_EQ(countKind(back, ObjectKind::Curve), curves);
  EXPECT_NEAR(volumeOf(back, *house), houseVolume, 1.0);
  EXPECT_NEAR(volumeOf(back, thick->id), 1e6 * 200, 1e-2);
  EXPECT_EQ(back.document().find(*box)->kind(), ObjectKind::Shell);
  EXPECT_EQ(paintedFaces(back), 2u);
  bool circle = false;
  for (TopExp_Explorer e(back.document().worldShape(*back.document().find(*arc)), TopAbs_EDGE); e.More(); e.Next()) {
    circle |= BRepAdaptor_Curve(TopoDS::Edge(e.Current())).GetType() == GeomAbs_Circle;
  }
  EXPECT_TRUE(circle);
  // And edited further: the middle third of the floor pushed up.
  ObjectId middle;
  for (const Object& o : back.document().objects()) {
    if (o.kind() != ObjectKind::Face) continue;
    const Bnd_Box b = geom::exactBounds(back.document().worldShape(o));
    if (std::abs(b.CornerMin().X() - 1000) < 1e-6 && std::abs(b.CornerMax().X() - 2000) < 1e-6) middle = o.id;
  }
  ASSERT_FALSE(middle.empty());
  ASSERT_TRUE(back.pushPull(middle, faceFacing(back, middle, gp::DZ()), 500).ok());
  EXPECT_NEAR(volumeOf(back, middle), 1e6 * 500, 1e-2);
  ASSERT_TRUE(back.undo());
}

// SC13 / PATH-04/05: a hollow section (a square with a square hole) swept
// along a line and an arc picked as edges, in either order: the hole runs
// through (not filled), the arc stays exact; one undo step.
TEST(Flows, SC13_HollowSectionAlongLineAndArc) {
  for (bool reversed : {false, true}) {
    Modeler m;
    auto a = m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0)});
    auto c = m.createArc(gp_Pnt(2000, 0, 0), gp_Pnt(2000, 2000, 0), gp::DZ(), -1000);  // bulging on, tangent
    ASSERT_TRUE(a.ok() && c.ok());
    // The section at the start, facing along +X: 200 x 200 with a 100 x 100 hole.
    auto outer = m.createRectangle(geom::SketchPlane::fromNormal(gp_Pnt(0, -100, -100), gp::DX()), 200, 200);
    auto hole = m.createRectangle(geom::SketchPlane::fromNormal(gp_Pnt(0, -50, -50), gp::DX()), 100, 100);
    ASSERT_TRUE(outer.ok() && hole.ok());
    ASSERT_TRUE(m.remove({*hole}).ok());  // the hole stays in the outer face
    const double section = geom::area(m.document().worldShape(*m.document().find(*outer)));
    ASSERT_NEAR(section, 200.0 * 200 - 100.0 * 100, 1e-3);
    std::vector<TopoDS_Edge> path;
    for (const ObjectId& id : reversed ? std::vector<ObjectId>{*c, *a} : std::vector<ObjectId>{*a, *c}) {
      for (TopExp_Explorer e(m.document().worldShape(*m.document().find(id)), TopAbs_EDGE); e.More(); e.Next()) {
        path.push_back(TopoDS::Edge(e.Current()));
      }
    }
    const std::size_t steps = m.history().undoCount();
    auto swept = m.followMe(*outer, path);
    ASSERT_TRUE(swept.ok()) << swept.error();
    EXPECT_EQ(m.history().undoCount(), steps + 1);
    const double length = 2000.0 + M_PI * 1000;
    const double v = volumeOf(m, *swept);
    EXPECT_NEAR(v, section * length, 0.02 * section * length);
    EXPECT_LT(v, 0.9 * 200.0 * 200 * length);  // the hole runs through
    EXPECT_TRUE(geom::validateShape(m.document().worldShape(*m.document().find(*swept)), true).valid);
  }
}

// PATH-05: the same hollow section around a sharp (90 degree) corner: the
// corner is mitred and the hole still runs through (it used to be refused).
TEST(Flows, SC13_HollowSectionAroundASharpCorner) {
  Modeler m;
  auto a = m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp_Pnt(2000, 2000, 0)});
  auto outer = m.createRectangle(geom::SketchPlane::fromNormal(gp_Pnt(0, -100, -100), gp::DX()), 200, 200);
  auto hole = m.createRectangle(geom::SketchPlane::fromNormal(gp_Pnt(0, -50, -50), gp::DX()), 100, 100);
  ASSERT_TRUE(a.ok() && outer.ok() && hole.ok());
  ASSERT_TRUE(m.remove({*hole}).ok());
  std::vector<TopoDS_Edge> path;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(*a)), TopAbs_EDGE); e.More(); e.Next()) {
    path.push_back(TopoDS::Edge(e.Current()));
  }
  auto swept = m.followMe(*outer, path);
  ASSERT_TRUE(swept.ok()) << swept.error();
  const double v = volumeOf(m, *swept);
  // Centre line 4000 long; the mitre adds and takes the same at a square corner.
  EXPECT_NEAR(v, 30000.0 * 4000, 0.02 * 30000.0 * 4000);
  EXPECT_TRUE(geom::validateShape(m.document().worldShape(*m.document().find(*swept)), true).valid);
  ASSERT_TRUE(m.undo());
}
