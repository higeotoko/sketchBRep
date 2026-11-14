// Tables and tags read from the model (docs/90 G-11, docs/97 N-10 / N-11, docs/104): the door
// and window schedule, the room schedule, areas by storey, and the marks on plans.
#include <cmath>
#include <map>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/Schedules.h"
#include "sbr/draw/Drawing.h"

using json = nlohmann::json;
using sbr::api::CommandApi;

namespace {
struct Schedules : testing::Test {
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
  std::string south, east, north, west, partition, room;
  // An 8000 x 6000 storey cut in two at x = 4000; a room in the west half only.
  // South: two 引違い windows; north: a plain window of the same size and a plain opening;
  // east: a door.
  void SetUp() override {
    ASSERT_TRUE(modeler.setLevels({{"", "1FL", 0}, {"", "2FL", 3000}}));
    const json walls = ok("create_wall", {{"points", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}, {0, 0}}},
                                          {"thickness", 200}, {"height", 3000}})["ids"];
    south = walls[0];
    east = walls[1];
    north = walls[2];
    west = walls[3];
    partition = ok("create_wall", {{"points", {{4000, 0}, {4000, 6000}}}, {"thickness", 100}, {"height", 3000}})["ids"][0];
    ok("place_openings", {{"host", south}, {"face_normal", {0, -1, 0}}, {"count", 2}, {"width", 1600}, {"height", 1200}, {"sill", 900}});
    ok("set_opening_fittings", {{"host", south}, {"fitting", {{"type", "sliding_double"}}}});
    ok("place_openings", {{"host", north}, {"face_normal", {0, 1, 0}}, {"count", 2}, {"width", 1600}, {"height", 1200}, {"sill", 900}});
    ok("set_opening_fittings", {{"host", north}, {"openings", {1}}, {"fitting", {{"type", "window"}}}});
    ok("set_opening_size", {{"host", north}, {"openings", {2}}, {"width", 800}, {"height", 2000}});
    ok("place_openings", {{"host", east}, {"face_normal", {1, 0, 0}}, {"count", 1}, {"width", 900}, {"height", 2100}, {"sill", 0}});
    ok("set_opening_fittings", {{"host", east}, {"fitting", {{"type", "swing_single"}, {"hinge", "left"}, {"swing", "in"}}}});
    ok("create_slab", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"top", 0}, {"thickness", 200}});
    room = ok("create_room", {{"at", {2000, 3000}}, {"name", "居間"}})["id"];
  }
};
}  // namespace

TEST_F(Schedules, OpeningsOfOneKindSizeAndFittingShareAMark) {
  const json s = ok("opening_schedule", {{"include_csv", true}});
  EXPECT_EQ(s["count"], 5);
  ASSERT_EQ(s["types"].size(), 4u) << s["types"].dump();
  // Windows first (by width, height, fitting), then doors, then plain openings.
  const json& w1 = s["types"][0];
  EXPECT_EQ(w1["mark"], "W1");
  EXPECT_EQ(w1["kind"], "window");
  EXPECT_EQ(w1["fitting"], "引違い");
  EXPECT_EQ(w1["count"], 2);
  EXPECT_NEAR(w1["width"].get<double>(), 1600, 1e-6);
  ASSERT_EQ(w1["sills"].size(), 1u);
  EXPECT_NEAR(w1["sills"][0].get<double>(), 900, 1e-6);
  EXPECT_EQ(s["types"][1]["mark"], "W2");  // the same size, another fitting
  EXPECT_EQ(s["types"][1]["fitting"], "窓");
  EXPECT_EQ(s["types"][1]["count"], 1);
  EXPECT_EQ(s["types"][2]["mark"], "D1");
  EXPECT_EQ(s["types"][2]["kind"], "door");
  EXPECT_EQ(s["types"][2]["fitting"], "片開き 左吊元・内開き");
  EXPECT_EQ(s["types"][3]["mark"], "O1");  // no fitting
  EXPECT_EQ(s["types"][3]["kind"], "opening");
  EXPECT_NEAR(s["types"][3]["width"].get<double>(), 800, 1e-6);
  // Every opening carries its type's mark; the counts agree.
  std::map<std::string, int> counted;
  for (const json& o : s["openings"]) {
    ++counted[o["mark"].get<std::string>()];
    EXPECT_EQ(o["level"], "1FL");
    EXPECT_FALSE(o["id"].get<std::string>().empty());
  }
  for (const json& t : s["types"]) EXPECT_EQ(counted[t["mark"].get<std::string>()], t["count"].get<int>());
  const std::string csv = s["csv"];
  EXPECT_EQ(csv.rfind("\xef\xbb\xbf", 0), 0u);
  EXPECT_NE(csv.find("W1,窓,引違い,1600,1200,2,900\r\n"), std::string::npos) << csv;
  EXPECT_NE(csv.find("合計,,,,,5,"), std::string::npos);
  EXPECT_FALSE(ok("opening_schedule").contains("csv"));

  // The marks on the plan are those of the schedule: as many texts as openings cut by it.
  const auto view = sbr::draw::makeView(modeler.document(), sbr::draw::planView(1100));
  ASSERT_TRUE(view) << view.error();
  std::map<std::string, int> drawn;
  for (const auto& item : view->items)
    if (item.kind == sbr::draw::Item::Kind::Text) ++drawn[item.text];
  EXPECT_EQ(drawn["W1"], 2);
  EXPECT_EQ(drawn["W2"], 1);
  EXPECT_EQ(drawn["D1"], 1);
  EXPECT_EQ(drawn["O1"], 1);
  // A plan under the sills (the plain opening kept its 900 too) cuts the door only.
  const auto low = sbr::draw::makeView(modeler.document(), sbr::draw::planView(500));
  ASSERT_TRUE(low);
  drawn.clear();
  for (const auto& item : low->items)
    if (item.kind == sbr::draw::Item::Kind::Text) ++drawn[item.text];
  EXPECT_EQ(drawn["W1"], 0);
  EXPECT_EQ(drawn["D1"], 1);
  EXPECT_EQ(drawn["O1"], 0);

  // A window made as wide as the others' kind changes: marks are read again from the model.
  ok("set_opening_fittings", {{"host", north}, {"openings", {1}}, {"fitting", {{"type", "sliding_double"}}}});
  const json again = ok("opening_schedule");
  ASSERT_EQ(again["types"].size(), 3u);
  EXPECT_EQ(again["types"][0]["count"], 3);
  // Nothing was changed by reading the tables.
  const auto steps = modeler.history().undoCount();
  ok("opening_schedule");
  ok("room_schedule");
  ok("area_table");
  EXPECT_EQ(modeler.history().undoCount(), steps);
}

TEST_F(Schedules, RoomsAndAreasAreReadAsTheWallsAreNow) {
  json rooms = ok("room_schedule", {{"include_csv", true}});
  ASSERT_EQ(rooms["rooms"].size(), 1u);
  const json& living = rooms["rooms"][0];
  EXPECT_EQ(living["name"], "居間");
  EXPECT_EQ(living["level"], "1FL");
  EXPECT_TRUE(living["counted"].get<bool>());
  EXPECT_NEAR(living["area_centre"].get<double>(), 24.0, 1e-9);  // 4000 x 6000 on the wall lines
  EXPECT_NEAR(living["area_inner"].get<double>(), 3.85 * 5.8, 1e-9);  // less 100 + 50 and 100 + 100
  EXPECT_EQ(living["tag"], "居間  24.00 m²");
  EXPECT_NEAR(rooms["total_area_centre"].get<double>(), 24.0, 1e-9);
  EXPECT_NE(rooms["csv"].get<std::string>().find("居間,1FL,24.00,22.33,"), std::string::npos) << rooms["csv"];

  json areas = ok("area_table", {{"include_csv", true}});
  ASSERT_EQ(areas["levels"].size(), 1u) << areas.dump();  // nothing stands on 2FL
  const json& first = areas["levels"][0];
  EXPECT_EQ(first["level"], "1FL");
  EXPECT_NEAR(first["enclosed"].get<double>(), 48.0, 1e-9);  // both halves, with or without a room
  EXPECT_EQ(first["regions"], 2);
  EXPECT_EQ(first["rooms"], 1);
  EXPECT_EQ(first["regions_without_room"], 1);
  EXPECT_NEAR(first["room_centre"].get<double>(), 24.0, 1e-9);
  EXPECT_NEAR(first["slab"].get<double>(), 48.0, 1e-6);
  EXPECT_EQ(first["slabs"], 1);
  EXPECT_EQ(areas["legal_floor_area"], "not_evaluated");  // never a number
  EXPECT_EQ(areas["building_area"], "not_evaluated");
  EXPECT_NE(areas["csv"].get<std::string>().find("1FL,0,48.00,2,1,1,24.00,22.33,48.00,1,0,未評価,未評価"), std::string::npos) << areas["csv"];

  // The room's tag on a plan drawing is its name and area.
  auto tagOnPlan = [&] {
    const auto view = sbr::draw::makeView(modeler.document(), sbr::draw::planView(1100));
    std::string found;
    if (view)
      for (const auto& item : view->items)
        if (item.kind == sbr::draw::Item::Kind::Text && item.text.rfind("居間", 0) == 0) found = item.text;
    return found;
  };
  EXPECT_EQ(tagOnPlan(), "居間  24.00 m²");
  // The partition 1000 to the east: the room, its tag and the table follow.
  ok("move", {{"ids", {partition}}, {"vector", {1000, 0, 0}}});
  EXPECT_EQ(tagOnPlan(), "居間  30.00 m²");
  areas = ok("area_table");
  EXPECT_NEAR(areas["levels"][0]["enclosed"].get<double>(), 48.0, 1e-9);
  EXPECT_NEAR(areas["levels"][0]["room_centre"].get<double>(), 30.0, 1e-9);
  // A wall taken away: the room is not closed; it is said, and it is not counted.
  ok("delete", {{"ids", {west}}});
  rooms = ok("room_schedule");
  EXPECT_FALSE(rooms["rooms"][0]["closed"].get<bool>());
  EXPECT_FALSE(rooms["rooms"][0].contains("area_centre"));
  EXPECT_EQ(rooms["not_counted"], 1);
  EXPECT_EQ(tagOnPlan(), "居間（閉じていない）");
  areas = ok("area_table");
  EXPECT_EQ(areas["levels"][0]["rooms"], 0);
  EXPECT_EQ(areas["levels"][0]["regions"], 1);  // the east half is still enclosed
  EXPECT_NEAR(areas["levels"][0]["enclosed"].get<double>(), 18.0, 1e-9);
}
