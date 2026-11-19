// Rooms from walls (docs/90 G-07, docs/98). Areas are checked against hand calculations:
// 壁芯 (within the lines the walls were drawn on) and 内法 (within the wall faces).
#include <cmath>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/RoomRegions.h"

using json = nlohmann::json;
using sbr::api::CommandApi;
using sbr::geom::PlanPoint;
using sbr::geom::PlanWall;
using sbr::geom::regionAt;
using sbr::geom::roomRegions;

namespace {
// Walls along a closed outline, drawn counter-clockwise (the room on their left), `t` thick on
// their line.
std::vector<PlanWall> ring(const std::vector<PlanPoint>& points, double t) {
  std::vector<PlanWall> walls;
  for (std::size_t i = 0; i < points.size(); ++i) walls.push_back({points[i], points[(i + 1) % points.size()], t / 2, t / 2});
  return walls;
}
const std::vector<PlanPoint> kRect = {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}};

TEST(RoomRegions, ARectangleOfWalls) {
  const auto found = roomRegions(ring(kRect, 200));
  ASSERT_EQ(found.regions.size(), 1u);
  const auto& r = found.regions[0];
  EXPECT_NEAR(r.centreArea, 6000.0 * 4000, 1e-6);
  ASSERT_TRUE(r.innerKnown);
  EXPECT_NEAR(r.innerArea, 5800.0 * 3800, 1e-6);
  EXPECT_NEAR(r.innerPerimeter, 2 * (5800.0 + 3800), 1e-6);
  EXPECT_EQ(r.walls.size(), 4u);
  EXPECT_TRUE(found.looseEnds.empty());
  EXPECT_TRUE(regionAt(found, {3000, 2000}));
  EXPECT_FALSE(regionAt(found, {7000, 2000}));
  // Drawn the other way round (the room on the walls' right): the same room.
  std::vector<PlanPoint> back(kRect.rbegin(), kRect.rend());
  const auto other = roomRegions(ring(back, 200));
  ASSERT_EQ(other.regions.size(), 1u);
  EXPECT_NEAR(other.regions[0].innerArea, 5800.0 * 3800, 1e-6);
}

TEST(RoomRegions, WallsOfDifferentThicknessOnEachSide) {
  // South wall 300 with 250 of it inside the line; the others 200 centred.
  auto walls = ring(kRect, 200);
  walls[0].left = 250;
  walls[0].right = 50;
  const auto found = roomRegions(walls);
  ASSERT_EQ(found.regions.size(), 1u);
  EXPECT_NEAR(found.regions[0].centreArea, 6000.0 * 4000, 1e-6);
  ASSERT_TRUE(found.regions[0].innerKnown);
  EXPECT_NEAR(found.regions[0].innerArea, 5800.0 * (4000 - 250 - 100), 1e-6);
}

TEST(RoomRegions, APartitionButtingAgainstTheWallFacesDividesTheRoom) {
  auto walls = ring(kRect, 200);
  walls.push_back({{3000, 100}, {3000, 3900}, 50, 50});  // stops at the faces, as drawn in a plan
  const auto found = roomRegions(walls);
  ASSERT_EQ(found.regions.size(), 2u);
  for (const auto& r : found.regions) {
    EXPECT_NEAR(r.centreArea, 3000.0 * 4000, 1e-6);
    ASSERT_TRUE(r.innerKnown);
    EXPECT_NEAR(r.innerArea, (3000.0 - 100 - 50) * 3800, 1e-6);
    EXPECT_EQ(r.walls.size(), 4u);  // south, north, one side and the partition
  }
  const auto west = regionAt(found, {1500, 2000}), east = regionAt(found, {4500, 2000});
  ASSERT_TRUE(west && east);
  EXPECT_NE(*west, *east);
  EXPECT_TRUE(found.looseEnds.empty());
}

TEST(RoomRegions, TwoWallsInLineWithDifferentThicknessMakeAStep) {
  // The south side in two pieces: the east half reaches 200 into the room, the west half 100.
  std::vector<PlanWall> walls = {{{0, 0}, {3000, 0}, 100, 100},      {{3000, 0}, {6000, 0}, 100, 200},
                                 {{6000, 0}, {6000, 4000}, 100, 100}, {{6000, 4000}, {0, 4000}, 100, 100},
                                 {{0, 4000}, {0, 0}, 100, 100}};
  const auto found = roomRegions(walls);
  ASSERT_EQ(found.regions.size(), 1u);
  EXPECT_NEAR(found.regions[0].centreArea, 6000.0 * 4000, 1e-6);
  ASSERT_TRUE(found.regions[0].innerKnown);
  EXPECT_NEAR(found.regions[0].innerArea, 5800.0 * 3800 - 2900.0 * 100, 1e-6);
}

TEST(RoomRegions, AnLShapedRoom) {
  const std::vector<PlanPoint> outline = {{0, 0}, {6000, 0}, {6000, 2000}, {3000, 2000}, {3000, 4000}, {0, 4000}};
  const auto found = roomRegions(ring(outline, 200));
  ASSERT_EQ(found.regions.size(), 1u);
  EXPECT_NEAR(found.regions[0].centreArea, 6000.0 * 2000 + 3000.0 * 2000, 1e-6);
  ASSERT_TRUE(found.regions[0].innerKnown);
  EXPECT_NEAR(found.regions[0].innerArea, 5800.0 * 1800 + 2800.0 * 2000, 1e-6);
  EXPECT_TRUE(regionAt(found, {1500, 3000}));
  EXPECT_FALSE(regionAt(found, {4500, 3000}));  // the notch is outside
}

TEST(RoomRegions, AnOutlineWithAWallMissingIsNotARoom) {
  auto walls = ring(kRect, 200);
  walls.erase(walls.begin() + 1);  // no east wall
  const auto found = roomRegions(walls);
  EXPECT_TRUE(found.regions.empty());
  EXPECT_EQ(found.looseEnds.size(), 2u);
  EXPECT_FALSE(regionAt(found, {3000, 2000}));
  // A gap in a wall too (a wall that stops short): nothing is bridged.
  auto gap = ring(kRect, 200);
  gap[1].b = {6000, 3000};  // 1000 short of the corner
  const auto open = roomRegions(gap);
  EXPECT_TRUE(open.regions.empty());
  EXPECT_EQ(open.looseEnds.size(), 2u);
}

TEST(RoomRegions, WallsStandingFreeInsideARoomAreTakenOut) {
  auto walls = ring({{0, 0}, {10000, 0}, {10000, 8000}, {0, 8000}}, 200);
  const auto core = ring({{4000, 3000}, {6000, 3000}, {6000, 5000}, {4000, 5000}}, 200);
  walls.insert(walls.end(), core.begin(), core.end());
  const auto found = roomRegions(walls);
  ASSERT_EQ(found.regions.size(), 2u);
  const auto big = regionAt(found, {1000, 1000}), small = regionAt(found, {5000, 4000});
  ASSERT_TRUE(big && small);
  ASSERT_NE(*big, *small);
  EXPECT_NEAR(found.regions[*big].centreArea, 10000.0 * 8000 - 2000.0 * 2000, 1e-6);
  ASSERT_TRUE(found.regions[*big].innerKnown);
  EXPECT_NEAR(found.regions[*big].innerArea, 9800.0 * 7800 - 2200.0 * 2200, 1e-6);
  EXPECT_EQ(found.regions[*big].walls.size(), 8u);
  EXPECT_NEAR(found.regions[*small].centreArea, 2000.0 * 2000, 1e-6);
  EXPECT_NEAR(found.regions[*small].innerArea, 1800.0 * 1800, 1e-6);
}

TEST(RoomRegions, AWallStickingIntoARoomDoesNotDivideItAndTheInnerAreaIsNotGuessed) {
  auto walls = ring(kRect, 200);
  walls.push_back({{3000, 100}, {3000, 2000}, 50, 50});  // a wing wall from the south wall
  const auto found = roomRegions(walls);
  ASSERT_EQ(found.regions.size(), 1u);
  EXPECT_NEAR(found.regions[0].centreArea, 6000.0 * 4000, 1e-6);
  EXPECT_FALSE(found.regions[0].innerKnown);  // its thickness is in the room: not evaluated
  EXPECT_EQ(found.regions[0].walls.size(), 5u);
  EXPECT_EQ(found.looseEnds.size(), 1u);
}

TEST(RoomRegions, CrossingWallsDivideEachOther) {
  auto walls = ring(kRect, 200);
  walls.push_back({{3000, 100}, {3000, 3900}, 50, 50});
  walls.push_back({{100, 2000}, {5900, 2000}, 50, 50});
  const auto found = roomRegions(walls);
  ASSERT_EQ(found.regions.size(), 4u);
  double total = 0.0;
  for (const auto& r : found.regions) {
    EXPECT_NEAR(r.centreArea, 3000.0 * 2000, 1e-6);
    ASSERT_TRUE(r.innerKnown);
    EXPECT_NEAR(r.innerArea, 2850.0 * 1850, 1e-6);
    total += r.centreArea;
  }
  EXPECT_NEAR(total, 6000.0 * 4000, 1e-6);
}

TEST(RoomRegions, ASlantedRoom) {
  // A right triangle with legs 4000 and 3000 (hypotenuse 5000), walls 200 centred.
  const auto found = roomRegions(ring({{0, 0}, {4000, 0}, {0, 3000}}, 200));
  ASSERT_EQ(found.regions.size(), 1u);
  EXPECT_NEAR(found.regions[0].centreArea, 6e6, 1e-6);
  ASSERT_TRUE(found.regions[0].innerKnown);
  // Inner triangle: the same shape with inradius 1000 - 100 = 900, so scaled by 0.9.
  EXPECT_NEAR(found.regions[0].innerArea, 6e6 * 0.81, 1e-3);
  EXPECT_NEAR(found.regions[0].innerPerimeter, 12000 * 0.9, 1e-6);
}

struct Rooms : testing::Test {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  json call(const std::string& command, json args = json::object()) {
    return json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
  }
  json ok(const std::string& command, json args = json::object()) {
    json r = call(command, args);
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  }
  std::string error(const std::string& command, json args) {
    const json r = call(command, args);
    EXPECT_FALSE(r.value("ok", true)) << command << " was accepted";
    return r.value("error", json()).dump();
  }
  json outer() {
    return ok("create_wall", {{"points", {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}, {0, 0}}}, {"thickness", 200}, {"height", 3000}})["ids"];
  }
  json room(std::size_t i = 0) { return ok("list_rooms")["rooms"][i]; }
};

TEST_F(Rooms, ARoomReadsItsAreaFromTheWallsAsTheyAre) {
  const json walls = outer();
  const std::string id = ok("create_room", {{"at", {3000, 2000}}, {"name", "LDK"}})["id"];
  json r = room();
  EXPECT_EQ(r["id"], id);
  EXPECT_EQ(r["name"], "LDK");
  EXPECT_TRUE(r["closed"].get<bool>());
  EXPECT_NEAR(r["area_centre"].get<double>(), 24.0, 1e-9);
  EXPECT_NEAR(r["area_inner"].get<double>(), 5.8 * 3.8, 1e-9);
  EXPECT_NEAR(r["inner_perimeter"].get<double>(), 19200, 1e-6);
  EXPECT_EQ(r["walls"].size(), 4u);

  // A window does not open the room.
  ok("place_openings", {{"host", walls[0]}, {"face_normal", {0, -1, 0}}, {"count", 1}, {"width", 1600}, {"height", 1200}, {"sill", 900}});
  EXPECT_NEAR(room()["area_centre"].get<double>(), 24.0, 1e-9);

  // The south wall 455 out: its neighbours follow (docs/96), and the room is bigger.
  ok("move", {{"ids", {walls[0]}}, {"vector", {0, -455, 0}}});
  r = room();
  EXPECT_TRUE(r["closed"].get<bool>());
  EXPECT_NEAR(r["area_centre"].get<double>(), 6.0 * 4.455, 1e-9);
  EXPECT_NEAR(r["area_inner"].get<double>(), 5.8 * 4.255, 1e-9);
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(room()["area_centre"].get<double>(), 24.0, 1e-9);

  // A wall taken away: the room says it is open, with no area, and where.
  ok("delete", {{"ids", {walls[1]}}});
  r = room();
  EXPECT_FALSE(r["closed"].get<bool>());
  EXPECT_FALSE(r.contains("area_centre"));
  EXPECT_FALSE(r["problem"].get<std::string>().empty());
  EXPECT_EQ(r["loose_ends"].size(), 2u);
  const json all = ok("list_rooms");
  EXPECT_EQ(all["not_counted"], 1);
  EXPECT_NEAR(all["total_area_centre"].get<double>(), 0.0, 1e-12);
  ASSERT_TRUE(modeler.undo());
  EXPECT_TRUE(room()["closed"].get<bool>());

  // Renamed like any text; removed like any object.
  ok("set_text", {{"id", id}, {"text", "居間"}});
  EXPECT_EQ(room()["name"], "居間");
  ok("delete", {{"ids", {id}}});
  EXPECT_TRUE(ok("list_rooms")["rooms"].empty());
}

TEST_F(Rooms, NothingIsClosedForTheUser) {
  outer();
  const auto steps = modeler.history().undoCount();
  EXPECT_NE(error("create_room", {{"at", {9000, 2000}}, {"name", "外"}}).find("Not enclosed"), std::string::npos);
  EXPECT_NE(error("create_room", {{"at", {3000, 2000}}, {"base", 3000}, {"name", "2 階"}}).find("No walls"), std::string::npos);
  error("create_room", {{"at", {3000, 2000}}, {"name", ""}});
  EXPECT_EQ(modeler.history().undoCount(), steps);
  const json outside = ok("room_at", {{"at", {9000, 2000}}});
  EXPECT_FALSE(outside["closed"].get<bool>());
  // One room per region.
  ok("create_room", {{"at", {3000, 2000}}, {"name", "A"}});
  EXPECT_NE(error("create_room", {{"at", {1000, 1000}}, {"name", "B"}}).find("already here"), std::string::npos);
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
}

TEST_F(Rooms, APartitionMakesTwoRoomsAndTakingItAwayMakesThemShare) {
  outer();
  const std::string partition =
      ok("create_wall", {{"points", {{3000, 100}, {3000, 3900}}}, {"thickness", 100}, {"height", 3000}})["ids"][0];
  ok("create_room", {{"at", {1500, 2000}}, {"name", "西"}});
  ok("create_room", {{"at", {4500, 2000}}, {"name", "東"}});
  json all = ok("list_rooms");
  ASSERT_EQ(all["rooms"].size(), 2u);
  for (const json& r : all["rooms"]) {
    EXPECT_NEAR(r["area_centre"].get<double>(), 12.0, 1e-9);
    EXPECT_NEAR(r["area_inner"].get<double>(), 2.85 * 3.8, 1e-9);
    EXPECT_TRUE(r["problem"].get<std::string>().empty());
  }
  EXPECT_NEAR(all["total_area_centre"].get<double>(), 24.0, 1e-9);
  // The partition moved 910: one room grows, the other shrinks, the total stays.
  ok("move", {{"ids", {partition}}, {"vector", {910, 0, 0}}});
  all = ok("list_rooms");
  EXPECT_NEAR(all["rooms"][0]["area_centre"].get<double>(), 3.91 * 4, 1e-9);
  EXPECT_NEAR(all["rooms"][1]["area_centre"].get<double>(), 2.09 * 4, 1e-9);
  EXPECT_NEAR(all["total_area_centre"].get<double>(), 24.0, 1e-9);
  // Without it both markers stand in one region: the second says so and is not counted twice.
  ok("delete", {{"ids", {partition}}});
  all = ok("list_rooms");
  EXPECT_TRUE(all["rooms"][0]["problem"].get<std::string>().empty());
  EXPECT_NE(all["rooms"][1]["problem"].get<std::string>().find("Shares"), std::string::npos);
  EXPECT_NEAR(all["total_area_centre"].get<double>(), 24.0, 1e-9);
  EXPECT_EQ(all["not_counted"], 1);
}

TEST_F(Rooms, TimberWallsOnTheColumnCentreAndWallsNoLongerWalls) {
  const json walls = ok("create_wall", {{"points", {{0, 0}, {5460, 0}, {5460, 3640}, {0, 3640}, {0, 0}}},
                                        {"type", "exterior_siding"}, {"height", 2800}})["ids"];
  ok("create_room", {{"at", {2000, 2000}}, {"name", "室"}});
  json r = room();
  // 壁芯 = the column centre lines the walls were drawn on.
  EXPECT_NEAR(r["area_centre"].get<double>(), 5.46 * 3.64, 1e-9);
  ASSERT_FALSE(r["area_inner"].is_null());
  EXPECT_LT(r["area_inner"].get<double>(), r["area_centre"].get<double>());
  EXPECT_GT(r["area_inner"].get<double>(), 5.2 * 3.4);
  // Another floor's height: these walls do not stand there.
  EXPECT_FALSE(ok("room_at", {{"at", {2000, 2000}}, {"base", 2800}})["closed"].get<bool>());
  // A wall reshaped by hand is no longer read as a wall: the room is open, and says why it may be.
  ok("push_pull", {{"id", walls[2]}, {"face", {{"normal", {0, 0, 1}}}}, {"distance", -300}});
  r = room();
  if (!r["closed"].get<bool>()) EXPECT_GE(r["unreadable_walls"].get<int>(), 1);
}

// docs/193: walls and slabs drawn over a loose face at their base (a site outline traced at GL)
// are made as they are: the face is not cut by them and they do not become regions of it.
TEST_F(Rooms, WallsAndSlabsOverAFaceLeaveItAlone) {
  const std::string site =
      ok("create_polygon", {{"points", {{-5000, -5000, 0}, {15000, -5000, 0}, {15000, 10000, 0}, {-5000, 10000, 0}}}})["id"];
  const json before = ok("get_object", {{"id", site}});
  const json walls = outer();
  ok("create_slab", {{"outline", {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}}}, {"top", 0}, {"thickness", 150}});
  ok("create_room", {{"at", {3000, 2000}}, {"name", "居間"}});
  const json r = room();
  EXPECT_TRUE(r["closed"].get<bool>()) << r.dump();
  EXPECT_EQ(r["unreadable_walls"], 0);
  EXPECT_NEAR(r["area_centre"].get<double>(), 24.0, 1e-9);
  for (const json& id : walls) {
    const json wall = ok("get_object", {{"id", id}});
    EXPECT_EQ(wall["history"].get<std::string>().rfind("Rectangle", 0), 0u) << wall.dump();
  }
  // The site face is the one that was drawn, whole, and nothing else became a face.
  const json after = ok("get_object", {{"id", site}});
  EXPECT_EQ(after["history"], before["history"]);
  EXPECT_NEAR(after["area"].get<double>(), before["area"].get<double>(), 1e-6) << after.dump();
  int faces = 0;
  const json objects = ok("list_objects")["objects"];
  for (const json& o : objects) faces += o["kind"] == "Face";
  EXPECT_EQ(faces, 1);
}
}  // namespace
