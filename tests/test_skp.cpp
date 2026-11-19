// SketchUp (.skp) through the SketchUp SDK on this PC (docs/181): out and back in. Skipped where
// the SDK is not there (CI, a build without SBR_SKETCHUP_SDK_DIR): the SDK is never shipped.

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <filesystem>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <gp_Ax1.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Skp.h"
#include "sbr/draw/Drawing.h"

using namespace sbr;
using namespace sbr::doc;
using json = nlohmann::json;

namespace {

ObjectId box(Modeler& m, double x, double y, double w, double d, double h) {
  geom::SketchPlane plane = geom::SketchPlane::ground();
  plane.origin = gp_Pnt(x, y, 0);
  const ObjectId id = *m.createRectangle(plane, w, d);
  EXPECT_TRUE(m.extrude(id, h));
  return id;
}

int faceCount(const TopoDS_Shape& s) {
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(s, TopAbs_FACE, faces);
  return faces.Extent();
}

#define SBR_NEEDS_SKETCHUP()                                     \
  do {                                                           \
    std::string why;                                             \
    if (!geom::skpAvailable(&why)) GTEST_SKIP() << why;          \
  } while (0)

}  // namespace

// Without the SDK: export and import say so, and nothing changes.
TEST(Skp, SaysSoWithoutTheSdk) {
  std::string why;
  if (geom::skpAvailable(&why)) GTEST_SKIP() << "the SketchUp SDK is on this PC";
  Modeler m;
  box(m, 0, 0, 1000, 1000, 1000);
  const Status s = m.exportSkp(::testing::TempDir() + "none.skp");
  EXPECT_FALSE(s);
  EXPECT_NE(s.message().find("SketchUp"), std::string::npos) << s.message();
  const auto steps = m.history().undoCount();
  EXPECT_FALSE(m.importSkp(::testing::TempDir() + "none.skp"));
  EXPECT_EQ(m.history().undoCount(), steps);
}

// A box with a hole through its top painted red, a cylinder and a line: out to SketchUp and back.
TEST(Skp, BoxCylinderAndLineRoundTrip) {
  SBR_NEEDS_SKETCHUP();
  Modeler m;
  const ObjectId b = box(m, 0, 0, 1000, 2000, 3000);
  ASSERT_TRUE(m.rename(b, "箱"));
  TopoDS_Face top;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(b)), TopAbs_FACE); e.More(); e.Next()) {
    GProp_GProps p;
    BRepGProp::SurfaceProperties(e.Current(), p);
    if (std::abs(p.CentreOfMass().Z() - 3000) < 1e-6) top = TopoDS::Face(e.Current());
  }
  ASSERT_FALSE(top.IsNull());
  const auto red = m.createMaterial("Red", Color{1, 0, 0});
  ASSERT_TRUE(red);
  ASSERT_TRUE(m.paintFaces(b, {top}, *red));
  // A ring: a box with a square hole through it (faces with inner loops).
  const ObjectId outer = box(m, 3000, 0, 1000, 1000, 500);
  const ObjectId inner = box(m, 3250, 250, 500, 500, 500);
  const auto ring = m.boolean(sbr::doc::BooleanKind::Subtract, {outer, inner});
  ASSERT_TRUE(ring) << ring.error();
  geom::SketchPlane plane = geom::SketchPlane::ground();
  plane.origin = gp_Pnt(6000, 0, 0);
  const ObjectId c = *m.createCircle(plane, 500);
  ASSERT_TRUE(m.extrude(c, 1000));
  ASSERT_TRUE(m.createPolyline({gp_Pnt(0, 3000, 0), gp_Pnt(1000, 3000, 0)}));
  const std::string path = ::testing::TempDir() + "roundtrip.skp";
  const Status out = m.exportSkp(path);
  ASSERT_TRUE(out) << out.message();

  Modeler back;
  geom::SkpReport report;
  const auto ids = back.importSkp(path, &report);
  ASSERT_TRUE(ids) << ids.error();
  EXPECT_EQ(report.groups, 4) << report.summary();
  EXPECT_EQ(report.solids, 3) << report.summary();
  EXPECT_EQ(report.leftOut, 0) << report.summary();
  ASSERT_EQ(ids->size(), 4u) << report.summary();
  std::map<std::string, const Object*> byName;
  for (const ObjectId& id : *ids) byName[back.document().find(id)->name] = back.document().find(id);
  // The box: the same name, 6 faces, its volume exactly (inches and back), the red top.
  ASSERT_TRUE(byName.count("箱")) << report.summary();
  const Object* solid = byName.at("箱");
  EXPECT_EQ(solid->kind(), ObjectKind::Solid);
  const TopoDS_Shape shape = back.document().worldShape(*solid);
  EXPECT_NEAR(geom::volume(shape), 1000.0 * 2000 * 3000, 1.0);
  EXPECT_EQ(faceCount(shape), 6);
  ASSERT_EQ(solid->faceMaterials.size(), 1u);
  const Material* painted = back.document().findMaterial(solid->faceMaterials.front().material);
  ASSERT_TRUE(painted);
  EXPECT_NEAR(painted->color.r, 1.0, 1e-6);
  EXPECT_NEAR(painted->color.g, 0.0, 1e-6);
  // The ring: its hole kept (faces with inner loops), 10 faces, the volume exactly.
  double ringVolume = 0, cylinderVolume = 0;
  int curves = 0;
  for (const auto& [name, o] : byName) {
    if (o->kind() == ObjectKind::Curve) {
      ++curves;
      continue;
    }
    const TopoDS_Shape s = back.document().worldShape(*o);
    const double v = geom::volume(s);
    if (std::abs(v - (1000.0 * 1000 - 500.0 * 500) * 500) < 1.0) {
      ringVolume = v;
      EXPECT_EQ(faceCount(s), 10) << name;
    } else if (name != "箱") {
      cylinderVolume = v;
    }
  }
  EXPECT_GT(ringVolume, 0.0);
  const double exact = M_PI * 500 * 500 * 1000;
  EXPECT_NEAR(cylinderVolume, exact, exact * 0.03);  // facets of about 15 degrees
  EXPECT_EQ(curves, 1);
  // One undo step takes the import away.
  ASSERT_TRUE(back.undo());
  EXPECT_TRUE(back.document().objects().empty());
}

// The AI commands: export_skp and import_skp, with the report.
TEST(Skp, ThroughTheCommands) {
  SBR_NEEDS_SKETCHUP();
  Modeler m;
  sbr::api::CommandApi api{m};
  auto call = [&](const std::string& command, json args) {
    return json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
  };
  ASSERT_TRUE(call("create_box", {{"origin", {0, 0, 0}}, {"width", 910}, {"depth", 910}, {"height", 2400}}).value("ok", false));
  const std::string path = ::testing::TempDir() + "commands.skp";
  const json out = call("export_skp", {{"path", path}});
  ASSERT_TRUE(out.value("ok", false)) << out.dump();
  const json in = call("import_skp", {{"path", path}});
  ASSERT_TRUE(in.value("ok", false)) << in.dump();
  EXPECT_EQ(in["result"]["report"]["solids"], 1) << in.dump();
  EXPECT_EQ(in["result"]["ids"].size(), 1u);
}

// An object of several solids touching (a stair: treads, strings, handrails): each solid in a
// group of its own inside the object's group, so that SketchUp does not join the faces they
// touch with; back in as one object, every solid closed, the same volume.
TEST(Skp, AStairOfManySolidsComesBackWhole) {
  SBR_NEEDS_SKETCHUP();
  Modeler m;
  sbr::api::CommandApi api{m};
  const json made = json::parse(api.execute(json{{"command", "create_stair"},
                                                 {"args", {{"path", {{1000, 1000}, {3000, 1000}, {3000, 2500}}}, {"rise", 3000},
                                                           {"handrail", "both"}, {"stringer", "both"}}}}.dump()));
  ASSERT_TRUE(made.value("ok", false)) << made.dump();
  const Object* stair = m.document().find(made["result"]["id"].get<std::string>());
  ASSERT_TRUE(stair);
  const TopoDS_Shape before = m.document().worldShape(*stair);
  int solids = 0;
  for (TopExp_Explorer s(before, TopAbs_SOLID); s.More(); s.Next()) ++solids;
  ASSERT_GT(solids, 1);
  const std::string path = ::testing::TempDir() + "stair.skp";
  ASSERT_TRUE(m.exportSkp(path));
  Modeler back;
  geom::SkpReport report;
  const auto ids = back.importSkp(path, &report);
  ASSERT_TRUE(ids) << ids.error();
  ASSERT_EQ(ids->size(), 1u) << report.summary();
  EXPECT_EQ(report.groups, 1) << report.summary();
  EXPECT_EQ(report.solids, 1) << report.summary();
  const TopoDS_Shape after = back.document().worldShape(*back.document().find(ids->front()));
  int again = 0;
  for (TopExp_Explorer s(after, TopAbs_SOLID); s.More(); s.Next()) ++again;
  EXPECT_EQ(again, solids);
  const double v = geom::volume(before);
  EXPECT_NEAR(geom::volume(after), v, v * 0.01);  // flat parts exactly; round handrails as facets
}

// docs/183: an object's tag and information go to SketchUp (its tag, a "SketchBRep" attribute
// dictionary) and come back: the tag as the object's, the information kept with the id it came from.
TEST(Skp, TagsAndInformationGoAndComeBack) {
  SBR_NEEDS_SKETCHUP();
  Modeler m;
  sbr::api::CommandApi api{m};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  const json walls = ok("create_wall", {{"points", {{0, 0}, {0, 4000}, {6000, 4000}}}, {"type", "exterior_siding"}, {"height", 2700}})["ids"];
  const std::string wall = walls[0];
  ok("set_tag", {{"id", wall}, {"tag", "1F 外壁"}});
  const std::string block = ok("create_box", {{"origin", {9000, 0, 0}}, {"width", 500}, {"depth", 500}, {"height", 500}})["id"];
  const std::string path = ::testing::TempDir() + "tags.skp";
  ok("export_skp", {{"path", path}});
  Modeler back;
  geom::SkpReport report;
  const auto ids = back.importSkp(path, &report);
  ASSERT_TRUE(ids) << ids.error();
  const Object* fromWall = nullptr;
  const Object* fromBlock = nullptr;
  for (const ObjectId& id : *ids) {
    const Object* o = back.document().find(id);
    const auto source = o->attributes.find("skp_source_id");
    ASSERT_NE(source, o->attributes.end()) << o->name;
    if (source->second == wall) fromWall = o;
    if (source->second == block) fromBlock = o;
  }
  ASSERT_TRUE(fromWall);
  ASSERT_TRUE(fromBlock);
  EXPECT_EQ(fromWall->layer, "1F 外壁");
  EXPECT_EQ(fromBlock->layer, "Default");
  // The wall's build-up came along (kept, not made the object's own: it is a shape now).
  const json carried = json::parse(fromWall->attributes.at("skp_attributes"));
  EXPECT_EQ(carried.value("wall_type", ""), "exterior_siding");
  EXPECT_EQ(carried.value("kind", ""), "wall");
  EXPECT_NE(carried.value("wall_layers", "").find("窯業系サイディング"), std::string::npos);
  EXPECT_FALSE(fromWall->attributes.count("kind"));
  EXPECT_FALSE(fromWall->attributes.count("wall_type"));
  // Written again and read again: the same information (the source id of the first export).
  const std::string again = ::testing::TempDir() + "tags2.skp";
  ASSERT_TRUE(back.exportSkp(again, {fromWall->id}));
  Modeler third;
  const auto more = third.importSkp(again);
  ASSERT_TRUE(more) << more.error();
  ASSERT_EQ(more->size(), 1u);
  EXPECT_EQ(third.document().find(more->front())->layer, "1F 外壁");
}

// docs/183: placements of one component go to SketchUp as one definition and its instances (the
// shape written once), each where it is placed; back in, one definition again.
TEST(Skp, ComponentsAreShared) {
  SBR_NEEDS_SKETCHUP();
  Modeler m;
  sbr::api::CommandApi api{m};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  const std::string post = ok("create_box", {{"origin", {0, 0, 0}}, {"width", 105}, {"depth", 105}, {"height", 2700}})["id"];
  const std::string first = ok("make_component", {{"ids", {post}}, {"name", "柱 105"}})["id"];
  ok("copy_array", {{"ids", {first}}, {"count", 3}, {"vector", {910, 0, 0}}});
  ASSERT_EQ(m.document().objects().size(), 4u);
  gp_Trsf turn;
  turn.SetRotation(gp_Ax1(gp_Pnt(52.5, 52.5, 0), gp_Dir(0, 0, 1)), std::acos(-1.0) / 2);
  ASSERT_TRUE(m.transform({first}, turn, "Rotate component"));
  const std::string path = ::testing::TempDir() + "posts.skp";
  ok("export_skp", {{"path", path}});
  Modeler back;
  geom::SkpReport report;
  const auto ids = back.importSkp(path, &report);
  ASSERT_TRUE(ids) << ids.error();
  EXPECT_EQ(report.instances, 4) << report.summary();
  EXPECT_EQ(report.groups, 0) << report.summary();
  ASSERT_EQ(ids->size(), 4u);
  ASSERT_EQ(back.document().definitions().size(), 1u);
  const auto definition = back.document().definitions().front().id;
  EXPECT_EQ(back.document().definitions().front().name, "柱 105");
  std::vector<double> xs;
  for (const ObjectId& id : *ids) {
    const Object* instance = back.document().find(id);
    ASSERT_NE(instance, nullptr);
    const auto* ref = std::get_if<ComponentInstance>(&instance->recipe.base);
    ASSERT_NE(ref, nullptr);
    EXPECT_EQ(ref->definitionId, definition);
    const auto source = instance->attributes.find("skp_source_id");
    ASSERT_NE(source, instance->attributes.end());
    if (source->second == first) EXPECT_NEAR(instance->transform.Value(1, 2), -1.0, 1e-6);
    const TopoDS_Shape s = back.document().worldShape(*instance);
    EXPECT_NEAR(geom::volume(s), 105.0 * 105 * 2700, 1.0);
    const auto b = geom::exactBounds(s);
    xs.push_back(b.CornerMin().X());
    EXPECT_NEAR(b.CornerMax().Z(), 2700, 1e-6);
  }
  std::sort(xs.begin(), xs.end());
  for (std::size_t i = 0; i < xs.size(); ++i) EXPECT_NEAR(xs[i], 910.0 * i, 1e-6);
  EXPECT_EQ(back.document().find(ids->front())->name.find("柱"), 0u) << back.document().find(ids->front())->name;
  ASSERT_TRUE(back.undo());
  EXPECT_TRUE(back.document().objects().empty());
  EXPECT_TRUE(back.document().definitions().empty());
  ASSERT_TRUE(back.redo());
  ASSERT_EQ(back.document().objects().size(), 4u);
  ASSERT_EQ(back.document().definitions().size(), 1u);

  const auto saved = (::testing::TempDir() + "shared-skp-components.sbr");
  ASSERT_TRUE(back.save(saved));
  Modeler reopened;
  ASSERT_TRUE(reopened.load(saved));
  ASSERT_EQ(reopened.document().definitions().size(), 1u);
  for (const Object& instance : reopened.document().objects()) {
    const auto* ref = std::get_if<ComponentInstance>(&instance.recipe.base);
    ASSERT_NE(ref, nullptr);
    EXPECT_EQ(ref->definitionId, definition);
  }
  std::filesystem::remove(saved);
}

// docs/185: drawings to LayOut: one drawing (export_drawing to .layout) and the drawing set as one
// document, a page per sheet.
TEST(Layout, DrawingsAsLayOutPages) {
  if (std::string why; !draw::layoutAvailable(&why)) GTEST_SKIP() << why;
  Modeler m;
  sbr::api::CommandApi api{m};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  ok("create_wall", {{"points", {{0, 0}, {0, 4000}, {6000, 4000}, {6000, 0}, {0, 0}}}, {"type", "exterior_siding"}, {"height", 2700}});
  ok("place_openings", {{"host", ok("list_objects", json::object())["objects"][0]["id"]}, {"face_normal", {-1, 0, 0}}, {"count", 1},
                        {"width", 1690}, {"height", 1170}, {"sill", 900}});
  const std::string one = ::testing::TempDir() + "plan.layout";
  std::filesystem::remove(std::filesystem::u8path(one));
  ok("export_drawing", {{"path", one}, {"kind", "plan"}, {"cut_z", 1200}, {"scale", 50}});
  const auto pages = draw::layoutPageCount(one);
  ASSERT_TRUE(pages) << pages.error();
  EXPECT_EQ(*pages, 1);
  const std::filesystem::path folder = std::filesystem::u8path(::testing::TempDir()) / "layout-set";
  std::filesystem::remove_all(folder);
  std::filesystem::create_directories(folder);
  const json set = ok("export_drawing_set", {{"folder", folder.u8string()}, {"format", "layout"}});
  const auto all = draw::layoutPageCount(set["path"].get<std::string>());
  ASSERT_TRUE(all) << all.error();
  EXPECT_EQ(*all, static_cast<int>(set["pages"].size()));
  EXPECT_GE(*all, 5);  // the list, a plan, four elevations, sections, tables
  // LayOut reads it back and prints it.
  const std::string pdf = ::testing::TempDir() + "layout-set.pdf";
  const Status printed = draw::layoutToPdf(set["path"].get<std::string>(), pdf);
  ASSERT_TRUE(printed) << printed.message();
  EXPECT_GT(std::filesystem::file_size(std::filesystem::u8path(pdf)), 1000u);
}

// docs/186: a file written by export_skp, changed "in SketchUp" (skpEditGroup: a group moved, one
// erased) and the model changed since, brought back with sync_skp.
TEST(Skp, ChangesInSketchUpComeBack) {
  SBR_NEEDS_SKETCHUP();
  Modeler m;
  sbr::api::CommandApi api{m};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  auto box = [&](const std::string& id) { return geom::exactBounds(m.document().worldShape(*m.document().find(id))); };
  // Drawn as in WallJoins (anticlockwise): moving the south wall sideways makes east and west longer.
  const json room = ok("create_wall", {{"points", {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}, {0, 0}}}, {"thickness", 200}, {"height", 2700}})["ids"];
  const std::string south = room[0], east = room[1], north = room[2], west = room[3];
  const std::string moved = ok("create_box", {{"origin", {9000, 0, 0}}, {"width", 500}, {"depth", 500}, {"height", 500}})["id"];
  const std::string erased = ok("create_box", {{"origin", {10000, 0, 0}}, {"width", 500}, {"depth", 500}, {"height", 500}})["id"];
  const std::string resized = ok("create_box", {{"origin", {11000, 0, 0}}, {"width", 500}, {"depth", 500}, {"height", 500}})["id"];
  const std::string path = ::testing::TempDir() + "roundtrip-sync.skp";
  ok("export_skp", {{"path", path}});
  // "In SketchUp": the box moved 1000 to the north, the other erased, the south wall moved 455 south.
  gp_Trsf north1000;
  north1000.SetTranslation(gp_Vec(0, 1000, 0));
  ASSERT_TRUE(geom::skpEditGroup(path, moved, &north1000, false));
  ASSERT_TRUE(geom::skpEditGroup(path, erased, nullptr, true));
  gp_Trsf south455;
  south455.SetTranslation(gp_Vec(0, -455, 0));
  ASSERT_TRUE(geom::skpEditGroup(path, south, &south455, false));
  // Here meanwhile: the third box made taller (it no longer matches the file).
  ok("set_parameter", {{"id", resized}, {"key", "op0.distance"}, {"value", 800}});
  const double westBefore = sbr::geom::volume(m.document().find(west)->shape);
  const auto steps = m.history().undoCount();
  const json s = ok("sync_skp", {{"path", path}});
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  EXPECT_EQ(s["moved"].size(), 2u) << s.dump();
  EXPECT_EQ(s["missing"], json::array({erased})) << s.dump();
  EXPECT_EQ(s["edited"], json::array({resized})) << s.dump();
  EXPECT_TRUE(s["added"].empty()) << s.dump();
  EXPECT_NEAR(box(moved).CornerMin().Y(), 1000, 1e-6);
  // The wall moved as a wall: still the wall tool's, its neighbours longer by 455 (joins follow).
  EXPECT_NEAR(box(south).CornerMin().Y(), -100 - 455, 1e-6);
  EXPECT_EQ(m.document().find(south)->attributes.at("kind"), "wall");
  EXPECT_NEAR(sbr::geom::volume(m.document().find(west)->shape), westBefore + 455.0 * 200 * 2700, 1.0);
  EXPECT_TRUE(m.document().find(erased));  // only listed
  // One undo: all back.
  ASSERT_TRUE(m.undo());
  EXPECT_NEAR(box(moved).CornerMin().Y(), 0, 1e-6);
  EXPECT_NEAR(box(south).CornerMin().Y(), -100, 1e-6);
  // Again, removing what was erased and taking the edited shape from SketchUp.
  const json t = ok("sync_skp", {{"path", path}, {"remove_missing", true}, {"replace_edited", true}});
  EXPECT_EQ(t["removed"], json::array({erased})) << t.dump();
  EXPECT_EQ(t["replaced"], json::array({resized})) << t.dump();
  EXPECT_FALSE(m.document().find(erased));
  EXPECT_NEAR(box(resized).CornerMax().Z(), 500, 1e-6);  // as SketchUp has it
  EXPECT_TRUE(m.document().find(resized)->attributes.count("skp_attributes"));
  // Objects from another model's file: added.
  Modeler other;
  sbr::api::CommandApi otherApi{other};
  ASSERT_TRUE(json::parse(otherApi.execute(json{{"command", "create_box"}, {"args", {{"origin", {0, 9000, 0}}, {"width", 300}, {"depth", 300}, {"height", 300}}}}.dump()))
                  .value("ok", false));
  const std::string foreign = ::testing::TempDir() + "foreign.skp";
  ASSERT_TRUE(other.exportSkp(foreign));
  const json u = ok("sync_skp", {{"path", foreign}});
  EXPECT_EQ(u["added"].size(), 1u) << u.dump();
}
