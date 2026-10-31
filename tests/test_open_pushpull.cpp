// Push/pull on an open shape (faces deleted, or faces drawn edge to edge),
// as on a SketchUp surface: the face moves along its normal and each of its
// edges gets a side face back to where it was; a side face on the plane of
// the face beyond the edge extends it (pulled away) or divides it (pushed
// along into it). The shape stays one object; nothing is filled.

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

Bnd_Box boundsOf(const Modeler& m, const ObjectId& id) { return geom::exactBounds(m.document().worldShape(*m.document().find(id))); }

}  // namespace


// A side of the open box (top missing) pulled out 500: the floor and the
// two sides beside it grow, a 500 strip closes the top over the new part;
// one object, still open; one undo step; the distance typed again; files.
TEST(OpenPushPull, SidePulledOutGrowsTheSurface) {
  Modeler m;
  const ObjectId box = openBox(m);
  const std::size_t steps = m.history().undoCount();
  auto r = m.pushPull(box, facing(m, box, gp_Dir(-1, 0, 0)), 500);
  ASSERT_TRUE(r.ok()) << r.error();
  EXPECT_EQ(r->id, box);
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  EXPECT_EQ(m.document().objects().size(), 1u);
  const Object* o = m.document().find(box);
  EXPECT_EQ(o->kind(), ObjectKind::Shell);  // still open, not filled
  EXPECT_EQ(faceCount(o->shape), 6);        // floor, 2 grown sides, far side, moved side, top strip
  EXPECT_TRUE(geom::validateShape(o->shape, false).drawingValid);
  EXPECT_NEAR(boundsOf(m, box).CornerMin().X(), -500, 1e-6);
  EXPECT_NEAR(boundsOf(m, box).CornerMax().X(), 1000, 1e-6);
  // The floor grew rather than getting a second face beside it.
  int floors = 0;
  for (TopExp_Explorer e(m.document().worldShape(*o), TopAbs_FACE); e.More(); e.Next()) {
    const Bnd_Box b = geom::exactBounds(e.Current());
    if (std::abs(b.CornerMax().Z()) < 1e-6) {
      ++floors;
      EXPECT_NEAR(b.CornerMin().X(), -500, 1e-6);
    }
  }
  EXPECT_EQ(floors, 1);
  // Typed again right after: a parameter of the shape.
  ASSERT_TRUE(m.setParameter(box, r->parameterKey, 300).ok());
  EXPECT_NEAR(boundsOf(m, box).CornerMin().X(), -300, 1e-6);

  const std::string path = ::testing::TempDir() + "open_pushpull.sbr";
  ASSERT_TRUE(m.save(path).ok());
  Modeler back;
  ASSERT_TRUE(back.load(path).ok());
  EXPECT_EQ(back.document().find(box)->kind(), ObjectKind::Shell);
  EXPECT_NEAR(boundsOf(back, box).CornerMin().X(), -300, 1e-6);

  ASSERT_TRUE(m.undo());  // the retyped distance
  ASSERT_TRUE(m.undo());  // the push/pull
  EXPECT_EQ(faceCount(m.document().worldShape(*m.document().find(box))), 5);
  EXPECT_NEAR(boundsOf(m, box).CornerMin().X(), 0, 1e-6);
  ASSERT_TRUE(m.redo());
  EXPECT_NEAR(boundsOf(m, box).CornerMin().X(), -500, 1e-6);
}

// Ctrl (keep the starting face): the side gives a 1000 x 1000 x 500 solid
// of its own beside the box; the box keeps its four other faces, still
// open; one undo step, the distance typed again, saved and reloaded.
TEST(OpenPushPull, KeepingTheStartFaceMakesAVolume) {
  Modeler m;
  const ObjectId box = openBox(m);
  const std::size_t steps = m.history().undoCount();
  auto r = m.pushPull(box, facing(m, box, gp_Dir(-1, 0, 0)), 500, true);
  ASSERT_TRUE(r.ok()) << r.error();
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  const Object* volume = m.document().find(r->id);
  ASSERT_TRUE(volume);
  EXPECT_NE(r->id, box);
  EXPECT_EQ(volume->kind(), ObjectKind::Solid);
  EXPECT_TRUE(geom::validateShape(m.document().worldShape(*volume), true).valid);
  EXPECT_NEAR(geom::volume(m.document().worldShape(*volume)), 1000.0 * 1000 * 500, 1e-3);
  EXPECT_NEAR(boundsOf(m, r->id).CornerMin().X(), -500, 1e-6);  // outwards
  EXPECT_NEAR(boundsOf(m, r->id).CornerMax().X(), 0, 1e-6);
  EXPECT_EQ(m.document().find(box)->kind(), ObjectKind::Shell);  // still open, not filled
  EXPECT_EQ(faceCount(m.document().worldShape(*m.document().find(box))), 4);
  EXPECT_TRUE(facing(m, box, gp_Dir(-1, 0, 0)).IsNull());  // no face left under the volume's
  ASSERT_TRUE(m.setParameter(r->id, r->parameterKey, 300).ok());
  EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(r->id))), 1000.0 * 1000 * 300, 1e-3);
  const std::string path = ::testing::TempDir() + "open_pushpull_keep.sbr";
  ASSERT_TRUE(m.save(path).ok());
  Modeler back;
  ASSERT_TRUE(back.load(path).ok());
  EXPECT_NEAR(geom::volume(back.document().worldShape(*back.document().find(r->id))), 1000.0 * 1000 * 300, 1e-3);
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  EXPECT_FALSE(m.document().find(r->id));
  EXPECT_EQ(faceCount(m.document().worldShape(*m.document().find(box))), 5);
}

// The floor pushed up inside (negative: against its downward normal): it
// moves up, and the four sides are divided at its new height.
TEST(OpenPushPull, FloorPushedUpDividesTheSides) {
  Modeler m;
  const ObjectId box = openBox(m);
  auto r = m.pushPull(box, facing(m, box, gp_Dir(0, 0, -1)), -200);
  ASSERT_TRUE(r.ok()) << r.error();
  const Object* o = m.document().find(box);
  EXPECT_EQ(o->kind(), ObjectKind::Shell);
  EXPECT_EQ(faceCount(o->shape), 9);  // the floor and each side in two
  const TopoDS_Face floor = facing(m, box, gp_Dir(0, 0, -1));
  ASSERT_FALSE(floor.IsNull());
  EXPECT_NEAR(geom::exactBounds(floor).CornerMin().Z(), 200, 1e-6);
  EXPECT_TRUE(geom::validateShape(o->shape, false).drawingValid);
}

// A top with a hole pulled up: the hole goes along (its wall grows too).
TEST(OpenPushPull, FaceWithAHole) {
  Modeler m;
  auto face = m.createRectangle(geom::SketchPlane::ground(), 2000, 2000);
  geom::SketchPlane centre = geom::SketchPlane::ground();
  centre.origin = gp_Pnt(1000, 1000, 0);
  auto disc = m.createCircle(centre, 400);
  ASSERT_TRUE(m.remove({*disc}).ok());
  ASSERT_TRUE(m.pushPull(*face, TopoDS::Face(m.document().worldShape(*face)), 1000).ok());
  ASSERT_TRUE(m.deleteFaces(*face, {facing(m, *face, gp_Dir(1, 0, 0))}).ok());  // open
  auto r = m.pushPull(*face, facing(m, *face, gp::DZ()), 300);
  ASSERT_TRUE(r.ok()) << r.error();
  EXPECT_EQ(m.document().find(*face)->kind(), ObjectKind::Shell);
  EXPECT_NEAR(boundsOf(m, *face).CornerMax().Z(), 1300, 1e-6);
  const TopoDS_Face top = facing(m, *face, gp::DZ());
  ASSERT_FALSE(top.IsNull());
  EXPECT_NEAR(geom::area(top), 2000.0 * 2000 - M_PI * 400 * 400, 1.0);
  EXPECT_TRUE(geom::validateShape(m.document().find(*face)->shape, false).drawingValid);
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
  EXPECT_NE(r.error().find("Curved"), std::string::npos) << r.error();
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
  EXPECT_EQ(m.document().objects().size(), 1u);
  EXPECT_EQ(m.document().find(box)->kind(), ObjectKind::Shell);
  EXPECT_NEAR(boundsOf(m, box).CornerMin().X(), -500, 1e-6);
  // keep_start_face: a volume of its own from the opposite side.
  const auto k = nlohmann::json::parse(api.execute(nlohmann::json{
      {"command", "push_pull"},
      {"args", {{"id", box}, {"face", {{"normal", {1, 0, 0}}}}, {"distance", 200}, {"keep_start_face", true}}}}.dump()));
  ASSERT_TRUE(k["ok"].get<bool>()) << k.dump();
  const ObjectId volume = k["result"]["id"].get<std::string>();
  EXPECT_NE(volume, box);
  EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(volume))), 1000.0 * 1000 * 200, 1e-3);
}

namespace {

// Runs a non-atomic batch; the results of its steps.
nlohmann::json runSteps(api::CommandApi& api, const nlohmann::json& steps) {
  const auto r = nlohmann::json::parse(
      api.execute(nlohmann::json{{"command", "batch"}, {"args", {{"atomic", false}, {"commands", steps}}}}.dump()));
  EXPECT_TRUE(r.value("ok", false)) << r.dump();
  return r["result"]["results"];
}

std::vector<double> areasFacing(const nlohmann::json& object, int axis, double sign) {
  std::vector<double> out;
  for (const auto& f : object["faces"]) {
    if (f["normal"][axis].get<double>() * sign > 0.99) out.push_back(f["area"].get<double>());
  }
  return out;
}

}  // namespace

// A rectangle drawn over a notch and pushed down to fill it becomes part of
// the solid it overlaps: one box again, no seams to erase (it stayed a
// second solid on top, whose outline could not be erased).
TEST(OpenPushPull, FillingANotchJoinsTheSolid) {
  Modeler m;
  api::CommandApi api(m);
  using nlohmann::json;
  const json steps = json::array({
      {{"command", "create_box"}, {"args", {{"origin", {0, 0, 0}}, {"width", 4000}, {"depth", 4000}, {"height", 2000}}}},
      {{"command", "split_face"}, {"args", {{"points", {{3000, 4000, 2000}, {4000, 3000, 2000}}}}}},
      {{"command", "push_pull"},
       {"args", {{"id", "$1.id"}, {"face", {{"normal", {0, 0, 1}}, {"near", {3800, 3800, 2000}}}}, {"distance", -2000}}}},
      {{"command", "create_rectangle"}, {"args", {{"origin", {2500, 2500, 2000}}, {"width", 1500}, {"height", 1500}}}},
      {{"command", "push_pull"},
       {"args", {{"id", "$4.id"}, {"face", {{"normal", {0, 0, 1}}, {"near", {3900, 3900, 2000}}}}, {"distance", -2000}}}},
      {{"command", "get_object"}, {"args", {{"id", "$5.id"}}}},
  });
  const json results = runSteps(api, steps);
  for (const json& r : results) ASSERT_TRUE(r.value("ok", false)) << r.dump();
  EXPECT_EQ(m.document().objects().size(), 1u);
  const json box = results[5]["result"];
  EXPECT_EQ(box["faces"].size(), 6u);
  EXPECT_NEAR(box["volume"].get<double>(), 4000.0 * 4000 * 2000, 1.0);
}

// A triangle drawn on the ground against a box and pulled up joins it; its
// top pushed down later (after a line divides the top) leaves the box's
// front whole, without the line where the triangle met it.
TEST(OpenPushPull, VolumeAgainstABoxJoinsItAndLeavesNoSeam) {
  Modeler m;
  api::CommandApi api(m);
  using nlohmann::json;
  const json steps = json::array({
      {{"command", "create_box"}, {"args", {{"origin", {0, 0, 0}}, {"width", 3000}, {"depth", 2000}, {"height", 2000}}}},
      {{"command", "create_polygon"}, {"args", {{"points", {{0, 0, 0}, {1200, 0, 0}, {-200, -1200, 0}}}}}},
      {{"command", "push_pull"},
       {"args", {{"id", "$2.id"}, {"face", {{"normal", {0, 0, -1}}, {"near", {300, -300, 0}}}}, {"distance", -2000}}}},
      {{"command", "split_face"}, {"args", {{"points", {{0, 0, 2000}, {1200, 0, 2000}}}}}},
      {{"command", "push_pull"},
       {"args", {{"id", "$3.id"}, {"face", {{"normal", {0, 0, 1}}, {"near", {300, -300, 2000}}}}, {"distance", -800}}}},
      {{"command", "get_object"}, {"args", {{"id", "$3.id"}}}},
  });
  const json results = runSteps(api, steps);
  for (const json& r : results) ASSERT_TRUE(r.value("ok", false)) << r.dump();
  EXPECT_EQ(m.document().objects().size(), 1u);
  const json solid = results[5]["result"];
  // The box's front: one face (1800 x 2000 beside the triangle, 1200 x 800
  // above it).
  const auto front = areasFacing(solid, 1, -1);
  ASSERT_EQ(front.size(), 1u);
  EXPECT_NEAR(front.front(), 1800.0 * 2000 + 1200.0 * 800, 1.0);
}

// Touching along an edge only, or apart: the volume stays an object of its own.
TEST(OpenPushPull, VolumeTouchingAlongAnEdgeStaysApart) {
  Modeler m;
  api::CommandApi api(m);
  using nlohmann::json;
  const json steps = json::array({
      {{"command", "create_box"}, {"args", {{"origin", {0, 0, 0}}, {"width", 1000}, {"depth", 1000}, {"height", 1000}}}},
      {{"command", "create_rectangle"}, {"args", {{"origin", {1000, 1000, 0}}, {"width", 1000}, {"height", 1000}}}},
      {{"command", "push_pull"},
       {"args", {{"id", "$2.id"}, {"face", {{"normal", {0, 0, 1}}, {"near", {1500, 1500, 0}}}}, {"distance", 1000}}}},
      {{"command", "create_rectangle"}, {"args", {{"origin", {3000, 0, 0}}, {"width", 1000}, {"height", 1000}}}},
      {{"command", "push_pull"},
       {"args", {{"id", "$4.id"}, {"face", {{"normal", {0, 0, 1}}, {"near", {3500, 500, 0}}}}, {"distance", 1000}}}},
  });
  const json results = runSteps(api, steps);
  for (const json& r : results) ASSERT_TRUE(r.value("ok", false)) << r.dump();
  EXPECT_EQ(m.document().objects().size(), 3u);
}
