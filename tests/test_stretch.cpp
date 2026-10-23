// Scale per direction (SketchUp's scale grips): width, depth or height on
// their own, exact B-rep (a circle becomes an ellipse), painted faces and
// holes kept, saved, undone; rotated objects along their own axes; one
// component placement on its own; the API does the same.

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"
#include "sbr/geom/Validation.h"

using namespace sbr;
using namespace sbr::doc;

namespace {

ObjectId cube(Modeler& m, double size = 1000) {
  auto id = m.createRectangle(geom::SketchPlane::ground(), size, size);
  EXPECT_TRUE(m.extrude(*id, size).ok());
  return *id;
}

Bnd_Box boundsOf(const Modeler& m, const ObjectId& id) {
  return geom::exactBounds(m.document().worldShape(*m.document().find(id)));
}

}  // namespace

// The required check: a 1000 cube made 2000 wide only, then 3000 high
// only; the other sizes stay; the opposite side stays where it was.
TEST(Stretch, WidthOnlyThenHeightOnly) {
  Modeler m;
  const ObjectId box = cube(m);
  const std::size_t steps = m.history().undoCount();
  ASSERT_TRUE(m.stretch({box}, gp_Pnt(0, 0, 0), gp::DX(), gp::DY(), 2.0, 1.0, 1.0).ok());
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  Bnd_Box b = boundsOf(m, box);
  EXPECT_NEAR(b.CornerMin().X(), 0, 1e-6);
  EXPECT_NEAR(b.CornerMax().X(), 2000, 1e-6);
  EXPECT_NEAR(b.CornerMax().Y(), 1000, 1e-6);
  EXPECT_NEAR(b.CornerMax().Z(), 1000, 1e-6);
  ASSERT_TRUE(m.stretch({box}, gp_Pnt(0, 0, 0), gp::DX(), gp::DY(), 1.0, 1.0, 3.0).ok());
  b = boundsOf(m, box);
  EXPECT_NEAR(b.CornerMax().X(), 2000, 1e-6);
  EXPECT_NEAR(b.CornerMax().Z(), 3000, 1e-6);
  const TopoDS_Shape s = m.document().worldShape(*m.document().find(box));
  EXPECT_TRUE(geom::validateShape(s, true).valid);
  EXPECT_NEAR(geom::volume(s), 2000.0 * 1000 * 3000, 1e-2);
  // Planes stay planes.
  for (TopExp_Explorer f(s, TopAbs_FACE); f.More(); f.Next()) {
    EXPECT_EQ(BRepAdaptor_Surface(TopoDS::Face(f.Current())).GetType(), GeomAbs_Plane);
  }
  // Lines stay lines.
  for (TopExp_Explorer e(s, TopAbs_EDGE); e.More(); e.Next()) {
    EXPECT_EQ(BRepAdaptor_Curve(TopoDS::Edge(e.Current())).GetType(), GeomAbs_Line);
  }
  // Still a normal box to keep working on: push/pull the top.
  TopoDS_Face top;
  for (TopExp_Explorer f(s, TopAbs_FACE); f.More(); f.Next()) {
    const auto n = geom::planarFaceNormal(TopoDS::Face(f.Current()));
    if (n && n->Z() > 0.99) top = TopoDS::Face(f.Current());
  }
  ASSERT_TRUE(m.pushPull(box, top, 500).ok());
  EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(box))), 2000.0 * 1000 * 3500, 1e-2);
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(box))), 1e9, 1e-3);
  ASSERT_TRUE(m.redo());
  EXPECT_NEAR(boundsOf(m, box).CornerMax().X(), 2000, 1e-6);
}

// A cylinder stretched along x: its circle becomes an ellipse (exact: the
// area is pi a b), not a polygon.
TEST(Stretch, CircleBecomesEllipse) {
  Modeler m;
  auto disc = m.createCircle(geom::SketchPlane::ground(), 500);
  ASSERT_TRUE(m.extrude(*disc, 1000).ok());
  { const Status st = m.stretch({*disc}, gp_Pnt(0, 0, 0), gp::DX(), gp::DY(), 2.0, 1.0, 1.0); ASSERT_TRUE(st.ok()) << st.message(); }
  const TopoDS_Shape s = m.document().worldShape(*m.document().find(*disc));
  EXPECT_TRUE(geom::validateShape(s, true).valid);
  EXPECT_NEAR(geom::volume(s), M_PI * 1000 * 500 * 1000, 1e-3 * M_PI * 1000 * 500 * 1000);
  Bnd_Box b = geom::exactBounds(s);
  EXPECT_NEAR(b.CornerMax().X() - b.CornerMin().X(), 2000, 1e-3);
  EXPECT_NEAR(b.CornerMax().Y() - b.CornerMin().Y(), 1000, 1e-3);
}

// A hole and a painted face come through; saved and reloaded the same.
TEST(Stretch, HolesPaintAndFiles) {
  Modeler m;
  auto face = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  geom::SketchPlane centre = geom::SketchPlane::ground();
  centre.origin = gp_Pnt(500, 500, 0);
  auto disc = m.createCircle(centre, 200);
  ASSERT_TRUE(m.remove({*disc}).ok());
  ASSERT_TRUE(m.extrude(*face, 500).ok());
  auto red = m.createMaterial("Red", {1, 0, 0});
  TopoDS_Face top;
  for (TopExp_Explorer f(m.document().worldShape(*m.document().find(*face)), TopAbs_FACE); f.More(); f.Next()) {
    const auto n = geom::planarFaceNormal(TopoDS::Face(f.Current()));
    if (n && n->Z() > 0.99) top = TopoDS::Face(f.Current());
  }
  ASSERT_TRUE(m.paintFaces(*face, {top}, *red).ok());
  ASSERT_TRUE(m.stretch({*face}, gp_Pnt(0, 0, 0), gp::DX(), gp::DY(), 1.0, 3.0, 1.0).ok());
  const TopoDS_Shape s = m.document().worldShape(*m.document().find(*face));
  EXPECT_NEAR(geom::volume(s), (1000.0 * 1000 - M_PI * 200 * 200) * 3 * 500, 1e-6 * geom::volume(s));
  const auto painted = resolveFaceMaterials(*m.document().find(*face));
  ASSERT_EQ(painted.faces.size(), 1u);
  const std::string path = ::testing::TempDir() + "stretch.sbr";
  ASSERT_TRUE(m.save(path).ok());
  Modeler back;
  ASSERT_TRUE(back.load(path).ok());
  EXPECT_NEAR(geom::volume(back.document().worldShape(*back.document().find(*face))), geom::volume(s), 1e-3);
  EXPECT_EQ(resolveFaceMaterials(*back.document().find(*face)).faces.size(), 1u);
}

// A box rotated 45 degrees, stretched along its own (rotated) x: longer
// along that direction, its width across unchanged.
TEST(Stretch, AlongARotatedObjectsOwnAxis) {
  Modeler m;
  const ObjectId box = cube(m);
  ASSERT_TRUE(m.rotate({box}, gp_Ax1(gp_Pnt(0, 0, 0), gp::DZ()), M_PI / 4).ok());
  const gp_Dir along(1, 1, 0), across(-1, 1, 0);
  ASSERT_TRUE(m.stretch({box}, gp_Pnt(0, 0, 0), along, across, 2.0, 1.0, 1.0).ok());
  const TopoDS_Shape s = m.document().worldShape(*m.document().find(box));
  EXPECT_NEAR(geom::volume(s), 2e9, 1e-2);
  EXPECT_TRUE(geom::validateShape(s, true).valid);
  // The far corner along the box's own x went from ~1414 to ~2828 away.
  Bnd_Box b = geom::exactBounds(s);
  EXPECT_NEAR(b.CornerMax().Y(), (2000.0 + 1000.0) / std::sqrt(2.0), 1e-3);
}

// XFORM-05, SC10: one of two placements of the same component made wider
// on its own: that placement only; the other and the shared definition
// stay; saved and reloaded; one undo step. Editing the component in place
// from the stretched placement is refused (it shows the shared shape);
// mirroring (a negative factor) is refused.
TEST(Stretch, OnePlacementOnItsOwn) {
  Modeler m;
  const ObjectId box = cube(m);
  auto component = m.makeComponent({box});
  ASSERT_TRUE(component.ok());
  gp_Trsf away;
  away.SetTranslation(gp_Vec(3000, 0, 0));
  auto copies = m.copy({*component}, away, 1);
  ASSERT_TRUE(copies.ok());
  const ObjectId other = copies->front();
  const std::string definition = std::get<ComponentInstance>(m.document().find(*component)->recipe.base).definitionId;
  const double definitionVolume = geom::volume(m.document().findDefinition(definition)->shape);
  const std::size_t steps = m.history().undoCount();
  const Status s = m.stretch({*component}, gp_Pnt(0, 0, 0), gp::DX(), gp::DY(), 2.0, 1.0, 1.0);
  ASSERT_TRUE(s.ok()) << s.message();
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  Bnd_Box b = boundsOf(m, *component);
  EXPECT_NEAR(b.CornerMax().X() - b.CornerMin().X(), 2000, 1e-6);
  EXPECT_NEAR(b.CornerMax().Y() - b.CornerMin().Y(), 1000, 1e-6);
  Bnd_Box o = boundsOf(m, other);
  EXPECT_NEAR(o.CornerMax().X() - o.CornerMin().X(), 1000, 1e-6);
  EXPECT_NEAR(geom::volume(m.document().findDefinition(definition)->shape), definitionVolume, 1e-6);
  EXPECT_EQ(std::get<ComponentInstance>(m.document().find(other)->recipe.base).definitionId, definition);
  // In the file, and back.
  const std::string path = ::testing::TempDir() + "stretch_placement.sbr";
  ASSERT_TRUE(m.save(path).ok());
  Modeler back;
  ASSERT_TRUE(back.load(path).ok());
  b = boundsOf(back, *component);
  EXPECT_NEAR(b.CornerMax().X() - b.CornerMin().X(), 2000, 1e-6);
  o = boundsOf(back, other);
  EXPECT_NEAR(o.CornerMax().X() - o.CornerMin().X(), 1000, 1e-6);
  // Edited in place from the stretched one: refused with the reason.
  auto edit = m.beginComponentEdit(*component);
  EXPECT_FALSE(edit.ok());
  // Mirroring stays a separate operation.
  EXPECT_FALSE(m.stretch({box}, gp_Pnt(0, 0, 0), gp::DX(), gp::DY(), -1.0, 1.0, 1.0).ok());
  ASSERT_TRUE(m.undo());
  b = boundsOf(m, *component);
  EXPECT_NEAR(b.CornerMax().X() - b.CornerMin().X(), 1000, 1e-6);
  EXPECT_TRUE(std::get<ComponentInstance>(m.document().find(*component)->recipe.base).plain());
}

TEST(Stretch, Api) {
  Modeler m;
  api::CommandApi api(m);
  const ObjectId box = cube(m);
  const auto r = nlohmann::json::parse(api.execute(nlohmann::json{
      {"command", "scale"},
      {"args", {{"ids", {box}}, {"center", {0, 0, 0}}, {"factors", {2, 1, 3}}}}}.dump()));
  ASSERT_TRUE(r["ok"].get<bool>()) << r.dump();
  Bnd_Box b = boundsOf(m, box);
  EXPECT_NEAR(b.CornerMax().X(), 2000, 1e-6);
  EXPECT_NEAR(b.CornerMax().Z(), 3000, 1e-6);
  // The uniform form still works.
  const auto u = nlohmann::json::parse(api.execute(nlohmann::json{
      {"command", "scale"}, {"args", {{"ids", {box}}, {"center", {0, 0, 0}}, {"factor", 0.5}}}}.dump()));
  ASSERT_TRUE(u["ok"].get<bool>()) << u.dump();
  EXPECT_NEAR(boundsOf(m, box).CornerMax().X(), 1000, 1e-6);
}
