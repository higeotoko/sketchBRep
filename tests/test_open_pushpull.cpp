// Push/pull on an open shape (faces deleted, still being modelled): the
// picked flat face gives a new solid of its own; the open shape loses just
// that face (it is the volume's face now: no two faces on top of each
// other) and stays open; nothing is filled or removed besides.

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

TopoDS_Face facing(const Modeler& m, const ObjectId& id, const gp_Dir& n) {
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(id)), TopAbs_FACE); e.More(); e.Next()) {
    const auto normal = geom::planarFaceNormal(TopoDS::Face(e.Current()));
    if (normal && normal->Angle(n) < 1e-6) return TopoDS::Face(e.Current());
  }
  return {};
}

int faceCount(const TopoDS_Shape& s) {
  int n = 0;
  for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) ++n;
  return n;
}

// 1. A square face pushed/pulled into a box; 2. its top deleted (open).
ObjectId openBox(Modeler& m) {
  auto face = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  EXPECT_TRUE(m.pushPull(*face, TopoDS::Face(m.document().worldShape(*face)), 1000).ok());
  EXPECT_TRUE(m.deleteFaces(*face, {facing(m, *face, gp::DZ())}).ok());
  EXPECT_EQ(m.document().find(*face)->kind(), ObjectKind::Shell);
  return *face;
}

ObjectId otherThan(const Modeler& m, const ObjectId& id) {
  for (const Object& o : m.document().objects()) {
    if (o.id != id) return o.id;
  }
  return {};
}

}  // namespace

// 3./4. A side of the open box pulled out 500: a 1000 x 1000 x 500 solid
// beside it; the box keeps its four other faces, still open; one undo
// step, redo, saved and reloaded.
TEST(OpenPushPull, SideFaceMakesAVolume) {
  Modeler m;
  const ObjectId box = openBox(m);
  const std::size_t steps = m.history().undoCount();
  auto r = m.pushPull(box, facing(m, box, gp_Dir(-1, 0, 0)), 500);
  ASSERT_TRUE(r.ok()) << r.error();
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  const Object* volume = m.document().find(r->id);
  ASSERT_TRUE(volume);
  EXPECT_NE(r->id, box);
  EXPECT_EQ(volume->kind(), ObjectKind::Solid);
  EXPECT_TRUE(geom::validateShape(m.document().worldShape(*volume), true).valid);
  EXPECT_NEAR(geom::volume(m.document().worldShape(*volume)), 1000.0 * 1000 * 500, 1e-3);
  Bnd_Box bounds = geom::exactBounds(m.document().worldShape(*volume));
  EXPECT_NEAR(bounds.CornerMin().X(), -500, 1e-6);  // outwards
  EXPECT_NEAR(bounds.CornerMax().X(), 0, 1e-6);
  const Object* rest = m.document().find(box);
  EXPECT_EQ(rest->kind(), ObjectKind::Shell);  // still open, not filled
  EXPECT_EQ(faceCount(m.document().worldShape(*rest)), 4);
  EXPECT_TRUE(facing(m, box, gp_Dir(-1, 0, 0)).IsNull());  // no face left under the volume's
  // The distance can be typed again right after (a parameter of the volume).
  ASSERT_TRUE(m.setParameter(r->id, r->parameterKey, 300).ok());
  EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(r->id))), 1000.0 * 1000 * 300, 1e-3);

  const std::string path = ::testing::TempDir() + "open_pushpull.sbr";
  ASSERT_TRUE(m.save(path).ok());
  Modeler back;
  ASSERT_TRUE(back.load(path).ok());
  EXPECT_EQ(back.document().find(box)->kind(), ObjectKind::Shell);
  EXPECT_NEAR(geom::volume(back.document().worldShape(*back.document().find(r->id))), 1000.0 * 1000 * 300, 1e-3);

  ASSERT_TRUE(m.undo());  // the retyped distance
  ASSERT_TRUE(m.undo());  // the push/pull
  EXPECT_FALSE(m.document().find(r->id));
  EXPECT_EQ(faceCount(m.document().worldShape(*m.document().find(box))), 5);
  ASSERT_TRUE(m.redo());
  EXPECT_TRUE(m.document().find(r->id));
  EXPECT_EQ(faceCount(m.document().worldShape(*m.document().find(box))), 4);
}

// Pushed in (negative): the volume lies inside the box.
TEST(OpenPushPull, NegativeGoesInside) {
  Modeler m;
  const ObjectId box = openBox(m);
  auto r = m.pushPull(box, facing(m, box, gp_Dir(0, 0, -1)), -200);  // the floor, thickened upwards
  ASSERT_TRUE(r.ok()) << r.error();
  Bnd_Box bounds = geom::exactBounds(m.document().worldShape(*m.document().find(r->id)));
  EXPECT_NEAR(bounds.CornerMin().Z(), 0, 1e-6);
  EXPECT_NEAR(bounds.CornerMax().Z(), 200, 1e-6);
  EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(r->id))), 1000.0 * 1000 * 200, 1e-3);
}

// A face with a hole: the volume keeps the hole.
TEST(OpenPushPull, FaceWithAHole) {
  Modeler m;
  auto face = m.createRectangle(geom::SketchPlane::ground(), 2000, 2000);
  geom::SketchPlane centre = geom::SketchPlane::ground();
  centre.origin = gp_Pnt(1000, 1000, 0);
  auto disc = m.createCircle(centre, 400);
  ASSERT_TRUE(m.remove({*disc}).ok());
  ASSERT_TRUE(m.pushPull(*face, TopoDS::Face(m.document().worldShape(*face)), 1000).ok());
  // Open it: delete one side.
  ASSERT_TRUE(m.deleteFaces(*face, {facing(m, *face, gp_Dir(1, 0, 0))}).ok());
  auto r = m.pushPull(*face, facing(m, *face, gp::DZ()), 300);
  ASSERT_TRUE(r.ok()) << r.error();
  EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(r->id))),
              (2000.0 * 2000 - M_PI * 400 * 400) * 300, 1.0);
}

// A curved face of an open shape is refused with the reason; nothing changes.
TEST(OpenPushPull, CurvedFaceRefused) {
  Modeler m;
  auto disc = m.createCircle(geom::SketchPlane::ground(), 500);
  ASSERT_TRUE(m.pushPull(*disc, TopoDS::Face(m.document().worldShape(*disc)), 1000).ok());
  ASSERT_TRUE(m.deleteFaces(*disc, {facing(m, *disc, gp::DZ())}).ok());
  TopoDS_Face side;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(*disc)), TopAbs_FACE); e.More(); e.Next()) {
    if (!geom::planarFaceNormal(TopoDS::Face(e.Current()))) side = TopoDS::Face(e.Current());
  }
  ASSERT_FALSE(side.IsNull());
  const std::size_t steps = m.history().undoCount();
  auto r = m.pushPull(*disc, side, 100);
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.error().find("curved"), std::string::npos) << r.error();
  EXPECT_EQ(m.history().undoCount(), steps);
}

// The AI's push_pull does the same.
TEST(OpenPushPull, Api) {
  Modeler m;
  api::CommandApi api(m);
  const ObjectId box = openBox(m);
  const auto r = nlohmann::json::parse(api.execute(nlohmann::json{
      {"command", "push_pull"},
      {"args", {{"id", box}, {"face", {{"normal", {-1, 0, 0}}}}, {"distance", 500}}}}.dump()));
  ASSERT_TRUE(r["ok"].get<bool>()) << r.dump();
  const ObjectId volume = otherThan(m, box);
  EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(volume))), 1000.0 * 1000 * 500, 1e-3);
  EXPECT_EQ(m.document().find(box)->kind(), ObjectKind::Shell);
}
