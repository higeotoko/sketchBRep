// Tables and tags read from the model (docs/90 G-11, docs/97 N-10 / N-11, docs/104): the door
// and window schedule, the room schedule, areas by storey, and the marks on plans.
#include <cmath>
#include <map>
#include <set>
#include <gtest/gtest.h>
#include <TopExp_Explorer.hxx>
#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/doc/Schedules.h"
#include "sbr/OpeningSymbols.h"
#include "sbr/draw/Drawing.h"
#include "sbr/geom/Measure.h"
#include <filesystem>

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
  EXPECT_EQ(w1["mark"], "AW1");
  EXPECT_EQ(w1["kind"], "window");
  EXPECT_EQ(w1["fitting"], "引違い");
  EXPECT_EQ(w1["count"], 2);
  EXPECT_NEAR(w1["width"].get<double>(), 1600, 1e-6);
  ASSERT_EQ(w1["sills"].size(), 1u);
  EXPECT_NEAR(w1["sills"][0].get<double>(), 900, 1e-6);
  EXPECT_EQ(s["types"][1]["mark"], "AW2");  // the same size, another fitting
  EXPECT_EQ(s["types"][1]["fitting"], "窓");
  EXPECT_EQ(s["types"][1]["count"], 1);
  EXPECT_EQ(s["types"][2]["mark"], "AD1");
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
  EXPECT_NE(csv.find("AW1,窓,引違い,1600,1200,2,900,,\r\n"), std::string::npos) << csv;
  EXPECT_NE(csv.find("合計,,,,,5,,,"), std::string::npos);
  EXPECT_FALSE(ok("opening_schedule").contains("csv"));

  // The marks on the plan are those of the schedule: as many texts as openings cut by it.
  const auto view = sbr::draw::makeView(modeler.document(), sbr::draw::planView(1100));
  ASSERT_TRUE(view) << view.error();
  std::map<std::string, int> drawn;
  for (const auto& item : view->items)
    if (item.kind == sbr::draw::Item::Kind::Text) ++drawn[item.text];
  EXPECT_EQ(drawn["AW1"], 2);
  EXPECT_EQ(drawn["AW2"], 1);
  EXPECT_EQ(drawn["AD1"], 1);
  EXPECT_EQ(drawn["O1"], 1);
  // A plan under the sills (the plain opening kept its 900 too) cuts the door only.
  const auto low = sbr::draw::makeView(modeler.document(), sbr::draw::planView(500));
  ASSERT_TRUE(low);
  drawn.clear();
  for (const auto& item : low->items)
    if (item.kind == sbr::draw::Item::Kind::Text) ++drawn[item.text];
  EXPECT_EQ(drawn["AW1"], 0);
  EXPECT_EQ(drawn["AD1"], 1);
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

TEST_F(Schedules, RoomOpeningInputsSeparateAdjacentRoomsAlongOneWall) {
  const auto unpaired = ok("room_opening_inputs");
  ASSERT_EQ(unpaired["rooms"].size(), 1u);
  ASSERT_EQ(unpaired["rooms"][0]["opening_candidates"].size(), 2u) << unpaired.dump();
  EXPECT_EQ(unpaired["unmatched_openings"].size(), 3u);
  EXPECT_TRUE(unpaired["rooms"][0]["ceiling_height_mm"].is_null());

  const std::string eastRoom = ok("create_room", {{"at", {6000, 3000}}, {"name", "洋室"}})["id"];
  sbr::doc::RoomFinish finish;
  finish.use = "居間";
  finish.ceilingHeight = 2400;
  ASSERT_TRUE(modeler.setRoomFinish(room, finish));
  const auto steps = modeler.history().undoCount();
  const auto inputs = ok("room_opening_inputs");
  ASSERT_EQ(inputs["rooms"].size(), 2u);
  EXPECT_TRUE(inputs["unmatched_openings"].empty()) << inputs.dump();
  EXPECT_EQ(inputs["daylight"], "未評価");
  EXPECT_EQ(inputs["natural_ventilation"], "未評価");
  EXPECT_EQ(inputs["mechanical_ventilation"], "未評価");
  std::map<std::string, json> byRoom;
  for (const auto& row : inputs["rooms"]) byRoom.emplace(row.at("id").get<std::string>(), row);
  const auto& westRoom = byRoom.at(room);
  EXPECT_EQ(westRoom["use"], "居間");
  EXPECT_EQ(westRoom["ceiling_height_mm"], 2400);
  EXPECT_GT(westRoom["inner_area_m2"].get<double>(), 0);
  std::map<std::string, std::string> southWindows;
  for (const auto& [id, row] : byRoom) {
    for (const auto& op : row["opening_candidates"]) {
      if (op["host"] == south) {
        EXPECT_EQ(op["width_mm"], 1600);
        EXPECT_TRUE(southWindows.emplace(id, op.at("id").get<std::string>()).second);
      }
    }
  }
  ASSERT_EQ(southWindows.size(), 2u);
  EXPECT_NE(southWindows.at(room), southWindows.at(eastRoom));
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
  // docs/141: the floor area by art. 2 (1) iii: within the wall lines, on the slab: 48; the
  // building area the same (eaves within 1 m); every note says its basis.
  EXPECT_NEAR(first["legal_floor"].get<double>(), 48.0, 1e-6);
  EXPECT_NEAR(areas["total_legal_floor"].get<double>(), 48.0, 1e-6);
  EXPECT_NEAR(areas["building_area"].get<double>(), 48.0, 1e-6);
  EXPECT_FALSE(areas["legal_notes"].empty());
  EXPECT_NE(areas["csv"].get<std::string>().find("1FL,0,48.00,2,1,1,24.00,22.33,48.00,1,0,48.00,"), std::string::npos) << areas["csv"];

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

// docs/176: the marks' prefixes as drawings usually have them: AW / AD in outer walls (and walls
// without a build-up), WW / WD in walls of an inner build-up.
TEST(OpeningMarks, PrefixesByTheWallsSide) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  const json outer = ok("create_wall", {{"points", {{0, 0}, {0, 4000}, {6000, 4000}, {6000, 0}, {0, 0}}}, {"type", "exterior_siding"}, {"height", 2700}})["ids"];
  const std::string inner = ok("create_wall", {{"points", {{3000, 65}, {3000, 3935}}}, {"type", "interior_board"}, {"height", 2700}})["ids"][0];
  const std::string south = outer[3];  // drawn clockwise: west, north, east, south
  ok("place_openings", {{"host", south}, {"face_normal", {0, -1, 0}}, {"count", 2}, {"width", 1690}, {"height", 1170}, {"sill", 900}});
  ok("set_opening_fittings", {{"host", south}, {"openings", {1}}, {"fitting", {{"type", "sliding_double"}}}});
  ok("set_opening_size", {{"host", south}, {"openings", {2}}, {"width", 780}, {"height", 2000}});
  ok("place_openings", {{"host", inner}, {"face_normal", {1, 0, 0}}, {"count", 2}, {"width", 780}, {"height", 2000}, {"sill", 0}});
  ok("set_opening_fittings", {{"host", inner}, {"openings", {1}}, {"fitting", {{"type", "swing_single"}}}});
  ok("set_opening_fittings", {{"host", inner}, {"openings", {2}}, {"fitting", {{"type", "window"}}}});
  ok("set_opening_size", {{"host", inner}, {"openings", {2}}, {"width", 600, }, {"height", 600}});
  const json s = ok("opening_schedule", json::object());
  std::set<std::string> marks;
  for (const json& t : s["types"]) marks.insert(t["mark"].get<std::string>());
  // The south window AW1, its plain opening O1; the partition's door WD1 and its window WW1.
  EXPECT_TRUE(marks.count("AW1")) << s["types"].dump();
  EXPECT_TRUE(marks.count("O1"));
  EXPECT_TRUE(marks.count("WD1"));
  EXPECT_TRUE(marks.count("WW1"));
  EXPECT_FALSE(marks.count("AD1"));
  // A door in an outer wall (the north one, from the floor): AD1.
  const std::string north = outer[1];
  ok("place_openings", {{"host", north}, {"face_normal", {0, 1, 0}}, {"count", 1}, {"width", 780}, {"height", 2000}, {"sill", 0}});
  ok("set_opening_fittings", {{"host", north}, {"fitting", {{"type", "swing_single"}}}});
  std::set<std::string> again;
  const json next = ok("opening_schedule", json::object());  // kept: a loop over a temporary's member would dangle
  for (const json& t : next["types"]) again.insert(t["mark"].get<std::string>());
  EXPECT_TRUE(again.count("AD1")) << again.size();
}

TEST_F(Schedules, AMarkGivenByHandStaysAndWorkedOutNumbersSkipIt) {
  // The two 引違い windows become AW-1 with a product number; the plain window takes "AW1" by hand (docs/176: worked-out marks are AW.. too).
  const auto steps = modeler.history().undoCount();
  ok("set_opening_mark", {{"host", south}, {"mark", "AW-1"}, {"product", "APW330-16513"}});
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  ok("set_opening_mark", {{"host", north}, {"openings", {1}}, {"mark", "AW1"}});
  json s = ok("opening_schedule", {{"include_csv", true}});
  std::map<std::string, json> byMark;
  for (const json& t : s["types"]) byMark[t["mark"].get<std::string>()] = t;
  ASSERT_EQ(byMark.size(), 4u) << s["types"].dump();
  EXPECT_EQ(byMark["AW-1"]["count"], 2);
  EXPECT_TRUE(byMark["AW-1"]["own_mark"].get<bool>());
  EXPECT_EQ(byMark["AW-1"]["product"], "APW330-16513");
  EXPECT_FALSE(byMark["AW-1"]["mixed"].get<bool>());
  EXPECT_EQ(byMark["AW1"]["fitting"], "窓");  // the one given by hand
  EXPECT_TRUE(byMark.count("AD1") && byMark.count("O1"));
  EXPECT_NE(s["csv"].get<std::string>().find("AW-1,窓,引違い,1600,1200,2,900,APW330-16513,手で決めた符号\r\n"), std::string::npos) << s["csv"];
  // The plan carries them.
  const auto view = sbr::draw::makeView(modeler.document(), sbr::draw::planView(1100));
  ASSERT_TRUE(view);
  int aw = 0;
  for (const auto& item : view->items)
    if (item.kind == sbr::draw::Item::Kind::Text && item.text == "AW-1") ++aw;
  EXPECT_EQ(aw, 2);

  // Another window of a new size: it is AW2, not AW1 (taken by hand); AW-1 stays.
  ok("place_openings", {{"host", west}, {"face_normal", {-1, 0, 0}}, {"count", 1}, {"width", 700}, {"height", 700}, {"sill", 1200}});
  ok("set_opening_fittings", {{"host", west}, {"fitting", {{"type", "window"}}}});
  s = ok("opening_schedule");
  bool w2 = false;
  for (const json& t : s["types"]) {
    if (t["mark"] == "AW2") w2 = t["width"].get<double>() == 700.0;
    if (t["mark"] == "AW1") EXPECT_NEAR(t["width"].get<double>(), 1600, 1e-6);
    if (t["mark"] == "AW-1") EXPECT_EQ(t["count"], 2);
  }
  EXPECT_TRUE(w2) << s["types"].dump();
  // One of the AW-1 windows made narrower: the mark stays, and the table says they differ.
  ok("set_opening_size", {{"host", south}, {"openings", {2}}, {"width", 1200}});
  s = ok("opening_schedule");
  for (const json& t : s["types"])
    if (t["mark"] == "AW-1") {
      EXPECT_EQ(t["count"], 2);
      EXPECT_TRUE(t["mixed"].get<bool>());
    }
  // Taken back: worked out again.
  ok("set_opening_mark", {{"host", south}});
  s = ok("opening_schedule");
  for (const json& t : s["types"]) EXPECT_NE(t["mark"], "AW-1");
  EXPECT_FALSE(modeler.document().find(south)->attributes.count("opening_marks"));
}

// docs/110: a copy of an opening has the same size, fitting and hand-given mark, and a new id.
TEST_F(Schedules, ACopyOfAnOpeningIsTheSameOpeningAgain) {
  const json before = ok("list_openings", {{"ids", {south}}})["hosts"][0]["openings"];
  ASSERT_EQ(before.size(), 2u);
  const std::string firstId = before[0]["id"];
  const double x0 = before[0]["offset"].get<double>();
  ok("set_opening_mark", {{"host", south}, {"openings", {1}}, {"mark", "AW-1"}, {"product", "P-1"}});
  // Within the wall: 1600 wide windows at 1200 and 4400 (from the left end); one copy 1100 to the
  // left of the first would overlap nothing... the wall is 8000: put it between them.
  const auto steps = modeler.history().undoCount();
  json made = ok("copy_opening", {{"host", south}, {"opening", firstId}, {"by", {1600, 0, 0}}});
  ASSERT_EQ(made["ids"].size(), 1u);
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  json after = ok("list_openings", {{"ids", {south}}})["hosts"][0]["openings"];
  ASSERT_EQ(after.size(), 3u);
  const json* copy = nullptr;
  for (const json& o : after)
    if (o["id"] == made["ids"][0]) copy = &o;
  ASSERT_TRUE(copy);
  EXPECT_NEAR((*copy)["offset"].get<double>(), x0 + 1600, 1e-6);
  EXPECT_NEAR((*copy)["width"].get<double>(), 1600, 1e-6);
  EXPECT_NEAR((*copy)["height"].get<double>(), 1200, 1e-6);
  EXPECT_NEAR((*copy)["sill"].get<double>(), 900, 1e-6);
  json s = ok("opening_schedule");
  int aw = 0;
  for (const json& o : s["openings"])
    if (o["mark"] == "AW-1") {
      ++aw;
      EXPECT_EQ(o["fitting"], "引違い");
      EXPECT_EQ(o["product"], "P-1");
    }
  EXPECT_EQ(aw, 2);  // the original and its copy
  ASSERT_TRUE(modeler.undo());
  EXPECT_EQ(ok("list_openings", {{"ids", {south}}})["hosts"][0]["openings"].size(), 2u);

  // Onto an opening already there, or off the wall: refused, nothing changes.
  EXPECT_FALSE(call("copy_opening", {{"host", south}, {"opening", 1}, {"by", {500, 0, 0}}}).value("ok", true));
  EXPECT_FALSE(call("copy_opening", {{"host", south}, {"opening", 1}, {"by", {-5000, 0, 0}}}).value("ok", true));
  EXPECT_FALSE(call("copy_opening", {{"host", south}, {"opening", 1}, {"by", {0, 0, 0}}}).value("ok", true));
  EXPECT_EQ(modeler.history().undoCount(), steps);

  // An array: the door on the east wall twice more, 1000 apart towards the south, one undo step;
  // too many to fit and nothing is made.
  EXPECT_FALSE(call("copy_opening", {{"host", east}, {"opening", 1}, {"by", {0, -1000, 0}}, {"count", 20}}).value("ok", true));
  EXPECT_EQ(modeler.history().undoCount(), steps);
  made = ok("copy_opening", {{"host", east}, {"opening", 1}, {"by", {0, -1000, 0}}, {"count", 2}});
  EXPECT_EQ(made["ids"].size(), 2u);
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  EXPECT_EQ(ok("list_openings", {{"ids", {east}}})["hosts"][0]["openings"].size(), 3u);
  ASSERT_TRUE(modeler.undo());

  // Into another wall: the door into the partition (a 100 wall), centred at y = 2000.
  made = ok("copy_opening", {{"host", east}, {"opening", 1}, {"to_host", partition}, {"at", {4000, 2000}}});
  const json inPartition = ok("list_openings", {{"ids", {partition}}})["hosts"][0]["openings"];
  ASSERT_EQ(inPartition.size(), 1u);
  EXPECT_NEAR(inPartition[0]["width"].get<double>(), 900, 1e-6);
  EXPECT_NEAR(inPartition[0]["height"].get<double>(), 2100, 1e-6);
  EXPECT_NEAR(inPartition[0]["sill"].get<double>(), 0, 1e-6);
  s = ok("opening_schedule");
  int doors = 0;
  for (const json& o : s["openings"])
    if (o["kind"] == "door") ++doors;
  EXPECT_EQ(doors, 2);  // its fitting came with it
  // The room on the west still closed (a door is an opening, not a gap in the wall line).
  EXPECT_TRUE(ok("room_at", {{"at", {2000, 3000}}})["closed"].get<bool>());
}

// docs/126: an opening copied (Ctrl+C) and pasted where clicked (Ctrl+V).
TEST_F(Schedules, ACopiedOpeningIsPastedWhereClicked) {
  ok("set_opening_mark", {{"host", east}, {"openings", {1}}, {"mark", "WD-1"}, {"product", "D-9"}});
  const auto door = modeler.openings(east);
  ASSERT_TRUE(door);
  ASSERT_EQ(door->size(), 1u);
  const std::string id = door->front().id;
  const gp_Pnt c = door->front().center;  // on x = 8000, y = 3000
  const auto steps = modeler.history().undoCount();
  // The same wall, 1500 to the south and somewhere up the wall: along only, the same sill.
  auto made = modeler.pasteOpening(east, id, gp_Pnt(8000, c.Y() - 1500, 2500));
  ASSERT_TRUE(made) << made.error();
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  auto list = modeler.openings(east);
  ASSERT_EQ(list->size(), 2u);
  for (const auto& o : *list) {
    EXPECT_NEAR(o.sill, 0, 1e-6);
    EXPECT_NEAR(o.width, 900, 1e-6);
    if (o.id == *made) EXPECT_NEAR(o.center.Y(), c.Y() - 1500, 1e-6);
  }
  ASSERT_TRUE(modeler.undo());
  // The partition, clicked in a plan at the door's height: centred there, its fitting and mark.
  made = modeler.pasteOpening(east, id, gp_Pnt(4000, 2000, c.Z()));
  ASSERT_TRUE(made) << made.error();
  list = modeler.openings(partition);
  ASSERT_EQ(list->size(), 1u);
  EXPECT_NEAR(list->front().center.Y(), 2000, 1e-6);
  EXPECT_NEAR(list->front().height, 2100, 1e-6);
  int marked = 0;
  const json table = ok("opening_schedule");
  for (const json& o : table["openings"])
    if (o["mark"] == "WD-1") {
      ++marked;
      EXPECT_EQ(o["product"], "D-9");
      EXPECT_EQ(o["kind"], "door");
    }
  EXPECT_EQ(marked, 2);
  // Clicked at the plan's cut height (not the door's): the wall is still found.
  ASSERT_TRUE(modeler.undo());
  made = modeler.pasteOpening(east, id, gp_Pnt(4000, 4000, 1500));
  ASSERT_TRUE(made) << made.error();
  ASSERT_TRUE(modeler.undo());
  // No wall there, or the copied opening gone: refused, nothing changes.
  EXPECT_FALSE(modeler.pasteOpening(east, id, gp_Pnt(2000, 3000, 1000)));
  EXPECT_FALSE(modeler.pasteOpening(east, "no-such-opening", gp_Pnt(8000, 1000, 1000)));
  EXPECT_FALSE(modeler.pasteOpening(east, id, gp_Pnt(8000, c.Y() + 100, 1000)));  // on itself: overlaps
  EXPECT_EQ(modeler.history().undoCount(), steps);
}

// docs/133: openings of one mark changed as a type.
TEST_F(Schedules, AnOpeningTypeIsChangedEverywhereAtOnce) {
  auto rowsOf = [&](const std::string& mark) {
    std::vector<sbr::doc::OpeningRow> out;
    for (const auto& r : sbr::doc::openingSchedule(modeler.document()).rows)
      if (r.mark == mark) out.push_back(r);
    return out;
  };
  // The two 引違い on the south wall share a worked-out mark.
  std::string sliding;
  for (const auto& t : sbr::doc::openingSchedule(modeler.document()).types)
    if (t.fitting == "引違い") sliding = t.mark;
  ASSERT_FALSE(sliding.empty());
  ASSERT_EQ(rowsOf(sliding).size(), 2u);
  const double north0 = modeler.openings(north)->front().width;
  const auto steps = modeler.history().undoCount();
  json r = ok("change_opening_type", {{"mark", sliding}, {"width", 1800}, {"product", "AL-1800"}});
  EXPECT_EQ(r["changed"], 2);
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  for (const auto& o : *modeler.openings(south)) {
    EXPECT_NEAR(o.width, 1800, 1e-6);
    EXPECT_NEAR(o.height, 1200, 1e-6);
    EXPECT_NEAR(o.sill, 900, 1e-6);
  }
  EXPECT_NEAR(modeler.openings(north)->front().width, north0, 1e-6);  // another type: as it was
  int products = 0;
  for (const auto& row : sbr::doc::openingSchedule(modeler.document()).rows) products += row.product == "AL-1800";
  EXPECT_EQ(products, 2);
  ASSERT_TRUE(modeler.undo());
  for (const auto& o : *modeler.openings(south)) EXPECT_NEAR(o.width, 1600, 1e-6);
  // Too wide for the wall with two of them: refused, nothing changed; an unknown mark too.
  EXPECT_FALSE(call("change_opening_type", {{"mark", sliding}, {"width", 4200}}).value("ok", true));
  EXPECT_FALSE(call("change_opening_type", {{"mark", "ZZ-9"}, {"width", 1000}}).value("ok", true));
  EXPECT_EQ(modeler.history().undoCount(), steps);
  for (const auto& o : *modeler.openings(south)) EXPECT_NEAR(o.width, 1600, 1e-6);
  // A mark given by hand to three windows in two walls; one of them changed by hand; the others
  // made like it ('like').
  ok("set_opening_mark", {{"host", south}, {"mark", "AW-1"}});
  ok("set_opening_mark", {{"host", north}, {"openings", {1}}, {"mark", "AW-1"}});
  ASSERT_EQ(rowsOf("AW-1").size(), 3u);
  ok("set_opening_size", {{"host", north}, {"openings", {1}}, {"width", 1200}, {"height", 1100}});
  bool mixed = false;
  for (const auto& t : sbr::doc::openingSchedule(modeler.document()).types) mixed |= t.mark == "AW-1" && t.mixed;
  EXPECT_TRUE(mixed);
  const auto before = modeler.history().undoCount();
  r = ok("change_opening_type", {{"like", {{"host", north}, {"opening", 1}}}});
  EXPECT_EQ(r["changed"], 2);
  EXPECT_EQ(modeler.history().undoCount(), before + 1);
  for (const auto& row : rowsOf("AW-1")) {
    EXPECT_NEAR(row.width, 1200, 1e-6);
    EXPECT_NEAR(row.height, 1100, 1e-6);
    EXPECT_EQ(row.fitting, rowsOf("AW-1").front().fitting);
  }
  mixed = false;
  for (const auto& t : sbr::doc::openingSchedule(modeler.document()).types) mixed |= t.mark == "AW-1" && t.mixed;
  EXPECT_FALSE(mixed);  // one type again
  for (const auto& row : rowsOf("AW-1")) EXPECT_EQ(row.fitting, "窓");  // the north one is a 窓: so are they now (were 引違い)
}

// docs/134: kinds of floors and roofs by a type name.
TEST_F(Schedules, FloorAndRoofTypesChangeTogether) {
  const auto box = [&](const std::string& id) { return sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(id))); };
  std::string slab;
  for (const auto& o : modeler.document().objects())
    if (o.attributes.count("kind") && o.attributes.at("kind") == "slab") slab = o.id;
  ASSERT_FALSE(slab.empty());
  const std::string upper = ok("create_slab", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"top", 3000}, {"thickness", 200}})["id"];
  const std::string roofA = ok("create_roof", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"kind", "gable"}, {"height", 3000}})["id"];
  const std::string roofB = ok("create_roof", {{"outline", {{20000, 0}, {26000, 0}, {26000, 5000}, {20000, 5000}}}, {"kind", "hip"}, {"height", 3000}})["id"];
  // Names: slabs and roofs only.
  EXPECT_FALSE(call("set_type_name", {{"ids", {south}}, {"name", "F1"}}).value("ok", true));
  ok("set_type_name", {{"ids", {slab, upper}}, {"name", "F1"}});
  ok("set_type_name", {{"ids", {roofA, roofB}}, {"name", "R1"}});
  // F1 thicker: both, the tops where they were, one undo step.
  const auto steps = modeler.history().undoCount();
  json r = ok("change_type", {{"name", "F1"}, {"thickness", 250}});
  EXPECT_EQ(r["changed"], 2);
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  EXPECT_NEAR(box(slab).CornerMax().Z(), 0, 1e-6);
  EXPECT_NEAR(box(slab).CornerMin().Z(), -250, 1e-6);
  EXPECT_NEAR(box(upper).CornerMax().Z(), 3000, 1e-6);
  EXPECT_NEAR(box(upper).CornerMin().Z(), 2750, 1e-6);
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(box(upper).CornerMin().Z(), 2800, 1e-6);
  // R1 steeper: both roofs, their other sizes as they were.
  const double eaveB = modeler.roofInfo(roofB)->spec.eave;
  r = ok("change_type", {{"name", "R1"}, {"pitch", 6}});
  EXPECT_EQ(r["changed"], 2);
  EXPECT_NEAR(modeler.roofInfo(roofA)->spec.pitch, 6, 1e-9);
  EXPECT_NEAR(modeler.roofInfo(roofB)->spec.pitch, 6, 1e-9);
  EXPECT_EQ(modeler.roofInfo(roofB)->spec.kind, "hip");
  EXPECT_NEAR(modeler.roofInfo(roofB)->spec.eave, eaveB, 1e-9);
  // One roof changed by hand; the other made like it.
  auto spec = modeler.roofInfo(roofA)->spec;
  spec.eave = 450;
  spec.thickness = 150;
  ASSERT_TRUE(modeler.changeRoof(roofA, spec));
  r = ok("change_type", {{"like", roofA}});
  EXPECT_EQ(r["changed"], 1);
  EXPECT_NEAR(modeler.roofInfo(roofB)->spec.eave, 450, 1e-9);
  EXPECT_NEAR(modeler.roofInfo(roofB)->spec.thickness, 150, 1e-9);
  // A slab made like the other.
  ASSERT_TRUE(modeler.changeMember(upper, [] { sbr::doc::Modeler::MemberChange c; c.thickness = 180.0; return c; }()));
  r = ok("change_type", {{"like", upper}});
  EXPECT_EQ(r["changed"], 1);
  EXPECT_NEAR(box(slab).CornerMin().Z(), -180, 1e-6);
  // Refused: an unknown name; a slab with no name; too thin a roof (nothing changes).
  EXPECT_FALSE(call("change_type", {{"name", "ZZ"}, {"thickness", 100}}).value("ok", true));
  ok("set_type_name", {{"ids", {upper}}, {"name", ""}});
  EXPECT_FALSE(call("change_type", {{"like", upper}}).value("ok", true));
  const auto before = modeler.history().undoCount();
  EXPECT_FALSE(call("change_type", {{"name", "R1"}, {"thickness", -5}}).value("ok", true));
  EXPECT_EQ(modeler.history().undoCount(), before);
  EXPECT_NEAR(modeler.roofInfo(roofB)->spec.thickness, 150, 1e-9);
}

// docs/136: the table of floor and roof types.
TEST_F(Schedules, TheTypesOfFloorsAndRoofsAreListed) {
  // Nothing named yet: no lines, said in the notes; not in the drawing set.
  sbr::draw::Table t = sbr::doc::scheduleTable(modeler.document(), 4);
  EXPECT_EQ(t.title, "種類の表");
  EXPECT_TRUE(t.rows.empty());
  ASSERT_EQ(t.notes.size(), 2u);  // the one slab without a name
  EXPECT_FALSE(sbr::doc::tableHasContent(t, 4));
  std::string slab;
  for (const auto& o : modeler.document().objects())
    if (o.attributes.count("kind") && o.attributes.at("kind") == "slab") slab = o.id;
  const std::string upper = ok("create_slab", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"top", 3000}, {"thickness", 150}})["id"];
  const std::string roof = ok("create_roof", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"kind", "gable"}, {"height", 3000}, {"pitch", 4.5}})["id"];
  ok("set_type_name", {{"ids", {slab, upper}}, {"name", "F1"}});
  ok("set_type_name", {{"ids", {roof}}, {"name", "R1"}});
  t = sbr::doc::scheduleTable(modeler.document(), 4);
  ASSERT_EQ(t.rows.size(), 2u);
  // Floors first: F1, 2 of them, thicknesses differ (200 and 150): said.
  EXPECT_EQ(t.rows[0][0], "F1");
  EXPECT_EQ(t.rows[0][1], "床");
  EXPECT_EQ(t.rows[0][2], "2");
  EXPECT_FALSE(t.rows[0][7].empty());
  EXPECT_EQ(t.rows[1][0], "R1");
  EXPECT_EQ(t.rows[1][1], "屋根");
  EXPECT_EQ(t.rows[1][4], "4.5 寸");
  EXPECT_TRUE(t.rows[1][7].empty());
  ASSERT_EQ(t.notes.size(), 1u);  // every one named
  // Made alike: no longer mixed.
  ok("change_type", {{"name", "F1"}, {"thickness", 200}});
  t = sbr::doc::scheduleTable(modeler.document(), 4);
  EXPECT_EQ(t.rows[0][3], "200");
  EXPECT_TRUE(t.rows[0][7].empty());
  // In the drawing set and as a file.
  const auto set = sbr::doc::drawingSet(modeler.document(), {});
  ASSERT_TRUE(set);
  EXPECT_NE(std::find(set->names.begin(), set->names.end(), "種類の表"), set->names.end());
  const auto dir = std::filesystem::temp_directory_path() / "sbr_type_table";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const json r = ok("export_schedule", {{"path", (dir / "types.dxf").u8string()}, {"table", "types"}});
  EXPECT_EQ(r["rows"], 2);
  std::filesystem::remove_all(dir);
}

// docs/149: registered floor / roof types.
TEST_F(Schedules, RegisteredTypesGiveTheirValues) {
  const auto box = [&](const std::string& id) { return sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(id))); };
  std::string slab;
  for (const auto& o : modeler.document().objects())
    if (o.attributes.count("kind") && o.attributes.at("kind") == "slab") slab = o.id;
  ASSERT_FALSE(slab.empty());
  // Registered with nothing using it yet: one undo step, in the table as "only registered".
  auto steps = modeler.history().undoCount();
  json r = ok("define_type", {{"name", "F2"}, {"part", "floor"}, {"thickness", 240}});
  EXPECT_EQ(r["changed"], 0);
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  ok("define_type", {{"name", "R1"}, {"part", "roof"}, {"pitch", 3}, {"eave", 900}});
  json list = ok("list_types");
  ASSERT_EQ(list["types"].size(), 2u);
  sbr::draw::Table t = sbr::doc::scheduleTable(modeler.document(), 4);
  ASSERT_EQ(t.rows.size(), 2u);
  EXPECT_EQ(t.rows[0][0], "F2");
  EXPECT_EQ(t.rows[0][2], "0");
  EXPECT_EQ(t.rows[0][3], "240");
  EXPECT_EQ(t.rows[0][7], "登録だけ（まだ使っていない）");
  EXPECT_EQ(t.rows[1][4], "3.0 寸");
  // A slab named F2 takes 240, its top kept, in the same step as the name.
  steps = modeler.history().undoCount();
  ok("set_type_name", {{"ids", {slab}}, {"name", "F2"}});
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  EXPECT_NEAR(box(slab).CornerMax().Z(), 0, 1e-6);
  EXPECT_NEAR(box(slab).CornerMin().Z(), -240, 1e-6);
  ASSERT_TRUE(modeler.undo());  // the name and the thickness back
  EXPECT_FALSE(modeler.document().find(slab)->attributes.count("type_name"));
  EXPECT_NEAR(box(slab).CornerMin().Z(), -200, 1e-6);
  ok("set_type_name", {{"ids", {slab}}, {"name", "F2"}});
  // A roof named R1 takes the pitch and eave given, keeps its thickness; a floor type on a roof: refused.
  const std::string roof = ok("create_roof", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"kind", "gable"}, {"height", 3000}})["id"];
  const double thick = modeler.roofInfo(roof)->spec.thickness;
  EXPECT_FALSE(call("set_type_name", {{"ids", {roof}}, {"name", "F2"}}).value("ok", true));
  ok("set_type_name", {{"ids", {roof}}, {"name", "R1"}});
  EXPECT_NEAR(modeler.roofInfo(roof)->spec.pitch, 3, 1e-9);
  EXPECT_NEAR(modeler.roofInfo(roof)->spec.eave, 900, 1e-9);
  EXPECT_NEAR(modeler.roofInfo(roof)->spec.thickness, thick, 1e-9);
  // Changing the type changes the register too; the table agrees.
  ok("change_type", {{"name", "F2"}, {"thickness", 260}});
  t = sbr::doc::scheduleTable(modeler.document(), 4);
  EXPECT_EQ(t.rows[0][3], "260");
  EXPECT_EQ(t.rows[0][7], "登録済み");
  // One changed by hand: said in the table; registered again from it: the register follows.
  ASSERT_TRUE(modeler.changeMember(slab, [] { sbr::doc::Modeler::MemberChange c; c.thickness = 180.0; c.keep = "top"; return c; }()));
  t = sbr::doc::scheduleTable(modeler.document(), 4);
  EXPECT_EQ(t.rows[0][7], "登録済み・登録した値と違う物があります");
  r = ok("define_type", {{"like", slab}});
  EXPECT_EQ(r["changed"], 0);  // no others
  t = sbr::doc::scheduleTable(modeler.document(), 4);
  EXPECT_EQ(t.rows[0][3], "180");
  EXPECT_EQ(t.rows[0][7], "登録済み");
  // Saved and read back with the model.
  sbr::doc::Document read;
  ASSERT_TRUE(sbr::doc::deserializeDocument(sbr::doc::serializeDocument(modeler.document()), read));
  EXPECT_EQ(read.settings().elementTypes, modeler.document().settings().elementTypes);
  // Refused: a roof value on a floor, a used name of the other part, bad values; nothing changes.
  steps = modeler.history().undoCount();
  EXPECT_FALSE(call("define_type", {{"name", "F3"}, {"part", "floor"}, {"pitch", 4}}).value("ok", true));
  EXPECT_FALSE(call("define_type", {{"name", "F2"}, {"part", "roof"}, {"pitch", 4}}).value("ok", true));
  EXPECT_FALSE(call("define_type", {{"name", "R1"}, {"part", "roof"}, {"pitch", 40}}).value("ok", true));
  EXPECT_FALSE(call("define_type", {{"name", "X"}, {"part", "wall"}, {"thickness", 100}}).value("ok", true));
  EXPECT_EQ(modeler.history().undoCount(), steps);
  // Taken off the register: the slab keeps its name and thickness.
  ok("remove_type", {{"name", "F2"}});
  EXPECT_EQ(ok("list_types")["types"].size(), 1u);
  EXPECT_EQ(modeler.document().find(slab)->attributes.at("type_name"), "F2");
  EXPECT_NEAR(box(slab).CornerMin().Z(), -180, 1e-6);
  EXPECT_FALSE(call("remove_type", {{"name", "F2"}}).value("ok", true));
  ASSERT_TRUE(modeler.undo());
  EXPECT_EQ(ok("list_types")["types"].size(), 2u);
}

// docs/151: a floor type's layers; lines where they meet in sections.
TEST_F(Schedules, FloorTypeLayersInSections) {
  std::string slab;
  for (const auto& o : modeler.document().objects())
    if (o.attributes.count("kind") && o.attributes.at("kind") == "slab") slab = o.id;
  ASSERT_FALSE(slab.empty());
  const double top = sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(slab))).CornerMax().Z();
  // Thin horizontal lines across the section, `below` under the slab's top, at least 1000 long.
  auto linesAt = [&](double below) {
    const auto view = sbr::draw::makeView(modeler.document(), sbr::draw::sectionView(gp_Pnt(2000, 3000, 0), gp_Dir(0, -1, 0)));
    int count = 0;
    if (!view) return -1;
    for (const auto& item : view->items)
      if (item.kind == sbr::draw::Item::Kind::Line && item.pen == sbr::draw::Pen::Visible && item.points.size() == 2 &&
          std::abs(item.points[0].Y() - (top - below)) < 1e-6 && std::abs(item.points[1].Y() - (top - below)) < 1e-6 &&
          item.points[0].Distance(item.points[1]) > 1000)
        ++count;
    return count;
  };
  EXPECT_EQ(linesAt(15), 0);
  // Layers given: the thickness is their sum; a slab of the type takes it.
  json r = ok("define_type", {{"name", "F1"}, {"part", "floor"},
                              {"layers", {{{"name", "フローリング"}, {"thickness", 15}}, {{"name", "合板"}, {"thickness", 24}},
                                          {{"name", "床梁"}, {"thickness", 105}}}}});
  json list = ok("list_types");
  ASSERT_EQ(list["types"].size(), 1u);
  EXPECT_NEAR(list["types"][0]["thickness"].get<double>(), 144, 1e-9);
  EXPECT_EQ(list["types"][0]["layers"].size(), 3u);
  ok("set_type_name", {{"ids", {slab}}, {"name", "F1"}});
  EXPECT_NEAR(sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(slab))).CornerMin().Z(), top - 144, 1e-6);
  EXPECT_EQ(sbr::doc::Modeler::typeLayerDepths(modeler.document(), *modeler.document().find(slab)), (std::vector<double>{15, 39}));
  EXPECT_GE(linesAt(15), 1);
  EXPECT_GE(linesAt(39), 1);
  EXPECT_EQ(linesAt(144), 0);  // the bottom is the cut line, not a layer line
  // The build-up in the types table's notes.
  sbr::draw::Table t = sbr::doc::scheduleTable(modeler.document(), 4);
  bool said = false;
  for (const auto& n : t.notes) said |= n.find("F1 の構成（上から）：フローリング 15・合板 24・床梁 105（計 144）") != std::string::npos;
  EXPECT_TRUE(said);
  // Made thicker by hand: no layer lines (they would be wrong), said in the notes once the type changes.
  ASSERT_TRUE(modeler.changeMember(slab, [] { sbr::doc::Modeler::MemberChange c; c.thickness = 200.0; c.keep = "top"; return c; }()));
  EXPECT_TRUE(sbr::doc::Modeler::typeLayerDepths(modeler.document(), *modeler.document().find(slab)).empty());
  EXPECT_EQ(linesAt(15), 0);
  ok("change_type", {{"name", "F1"}, {"thickness", 200}});
  t = sbr::doc::scheduleTable(modeler.document(), 4);
  said = false;
  for (const auto& n : t.notes) said |= n.find("厚さと合いません") != std::string::npos;
  EXPECT_TRUE(said);
  // New layers that add up: lines again. Refused: layers not adding up to a thickness given, a layer
  // of no thickness.
  ok("define_type", {{"name", "F1"}, {"part", "floor"},
                     {"layers", {{{"name", "フローリング"}, {"thickness", 15}}, {{"name", "合板"}, {"thickness", 185}}}}});
  EXPECT_GE(linesAt(15), 1);
  EXPECT_FALSE(call("define_type", {{"name", "F1"}, {"part", "floor"}, {"thickness", 150},
                                    {"layers", {{{"name", "a"}, {"thickness", 100}}}}}).value("ok", true));
  EXPECT_FALSE(call("define_type", {{"name", "F1"}, {"part", "floor"}, {"layers", {{{"name", "a"}, {"thickness", 0}}}}}).value("ok", true));
  // Taken off: no lines; the thickness stays.
  ok("define_type", {{"name", "F1"}, {"part", "floor"}, {"layers", json::array()}});
  EXPECT_EQ(linesAt(15), 0);
  EXPECT_FALSE(ok("list_types")["types"][0].contains("layers"));
}

// docs/152: a roof type's layers, across the slope; mitred at the ridge.
TEST_F(Schedules, RoofTypeLayersAcrossTheSlope) {
  // A gable roof over 8000 x 6000, the ridge along X (the long side), 4 寸, 120 thick.
  const std::string roof = ok("create_roof", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"kind", "gable"},
                                              {"height", 3000}, {"pitch", 4}, {"thickness", 120}})["id"];
  // Cut across the ridge (looking along X): the sloped thin lines, longer than 1000.
  auto sloped = [&] {
    const auto view = sbr::draw::makeView(modeler.document(), sbr::draw::sectionView(gp_Pnt(6000, 3000, 0), gp_Dir(1, 0, 0)));
    std::vector<std::pair<gp_Pnt2d, gp_Pnt2d>> out;
    if (!view) return out;
    for (const auto& item : view->items)
      if (item.kind == sbr::draw::Item::Kind::Line && item.pen == sbr::draw::Pen::Visible && item.points.size() == 2 &&
          std::abs(item.points[0].Y() - item.points[1].Y()) > 100 && item.points[0].Distance(item.points[1]) > 1000)
        out.emplace_back(item.points[0], item.points[1]);
    return out;
  };
  const std::size_t before = sloped().size();
  ok("define_type", {{"name", "R1"}, {"part", "roof"},
                     {"layers", {{{"name", "ガルバリウム"}, {"thickness", 20}}, {{"name", "野地板"}, {"thickness", 12}},
                                 {{"name", "垂木"}, {"thickness", 88}}}}});
  ok("set_type_name", {{"ids", {roof}}, {"name", "R1"}});
  EXPECT_NEAR(modeler.roofInfo(roof)->spec.thickness, 120, 1e-9);
  EXPECT_EQ(sbr::doc::Modeler::typeLayerDepths(modeler.document(), *modeler.document().find(roof)), (std::vector<double>{20, 32}));
  const auto with = sloped();
  ASSERT_EQ(with.size(), before + 4);  // 2 boundaries x 2 slopes
  // Each new line lies 20 or 32 under its slope at right angles, and the two of one depth meet below
  // the ridge (mitred): an end at the middle (Y 3000 in the plan is the sheet's X... found by matching ends).
  const double rise = 0.4, cosine = 1.0 / std::sqrt(1 + rise * rise);
  int met = 0;
  for (std::size_t i = 0; i < with.size(); ++i)
    for (std::size_t j = i + 1; j < with.size(); ++j)
      for (const auto& p : {with[i].first, with[i].second})
        for (const auto& q : {with[j].first, with[j].second})
          met += p.Distance(q) < 1e-6;
  EXPECT_GE(met, 2);  // the 20 pair and the 32 pair each end at one point
  // That point: under the ridge by 20 / cos and 32 / cos.
  const double ridgeTop = modeler.roofInfo(roof)->ridgeHeight;
  int under = 0;
  for (const auto& [a, b] : with)
    for (const auto& p : {a, b})
      for (const double d : {20.0, 32.0}) under += std::abs(p.Y() - (ridgeTop - d / cosine)) < 1e-3;
  EXPECT_GE(under, 4);
  // Cut along the ridge (docs/152's leftover): at y 1500 the south slope is a level band; its
  // layers level lines 20 / cos and 32 / cos under its top, as long as the roof (8000 and the
  // gable overhangs 300 each side).
  {
    const auto view = sbr::draw::makeView(modeler.document(), sbr::draw::sectionView(gp_Pnt(4000, 1500, 0), gp_Dir(0, -1, 0)));
    ASSERT_TRUE(view) << view.error();
    const double top = 3000 + rise * 1500 + 120 / cosine;
    int found = 0;
    for (const double d : {20.0, 32.0})
      for (const auto& item : view->items)
        if (item.kind == sbr::draw::Item::Kind::Line && item.pen == sbr::draw::Pen::Visible && item.points.size() == 2 &&
            std::abs(item.points[0].Y() - item.points[1].Y()) < 1e-6 && std::abs(item.points[0].Y() - (top - d / cosine)) < 1e-3 &&
            std::abs(std::abs(item.points[0].X() - item.points[1].X()) - 8600.0) < 1e-3)
          ++found;
    EXPECT_EQ(found, 2);
  }
  // A thicker roof by hand no longer adds up: no lines.
  auto spec = modeler.roofInfo(roof)->spec;
  spec.thickness = 150;
  ASSERT_TRUE(modeler.changeRoof(roof, spec));
  EXPECT_TRUE(sbr::doc::Modeler::typeLayerDepths(modeler.document(), *modeler.document().find(roof)).empty());
  EXPECT_EQ(sloped().size(), before);
}

// docs/162: the total the floor area ratio is worked out from: rooms marked as parts that are
// left out, each kind up to its share of the total floor area (Order art. 2 (1) iv, (3); Act
// art. 52 (6) i, ii).
TEST(LegalAreas, TheFloorAreaForTheRatioLeavesMarkedRoomsOut) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto call = [&](const std::string& command, json args) { return json::parse(api.execute(json{{"command", command}, {"args", args}}.dump())); };
  auto ok = [&](const std::string& command, json args) {
    json r = call(command, args);
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  ASSERT_TRUE(modeler.setLevels({{"", "1FL", 0}}));
  // 10000 x 6000 cut at x = 4000 and x = 5000: 24, 6 and 30 m2; 60 in all.
  ok("create_wall", {{"points", {{0, 0}, {10000, 0}, {10000, 6000}, {0, 6000}, {0, 0}}}, {"thickness", 120}, {"height", 3000}});
  const std::string cut = ok("create_wall", {{"points", {{4000, 0}, {4000, 6000}}}, {"thickness", 120}, {"height", 3000}})["ids"][0];
  ok("create_wall", {{"points", {{5000, 0}, {5000, 6000}}}, {"thickness", 120}, {"height", 3000}});
  const std::string garage = ok("create_room", {{"at", {2000, 3000}}, {"name", "車庫"}})["id"];
  const std::string store = ok("create_room", {{"at", {4500, 3000}}, {"name", "備蓄倉庫"}})["id"];
  const std::string living = ok("create_room", {{"at", {7500, 3000}}, {"name", "居間"}})["id"];
  // No room marked: the same as the total, and the note says so.
  json areas = ok("area_table", json::object());
  EXPECT_NEAR(areas["total_legal_floor"].get<double>(), 60.0, 1e-6);
  EXPECT_NEAR(areas["ratio_floor_area"].get<double>(), 60.0, 1e-6);
  EXPECT_TRUE(areas["ratio_parts"].empty());
  EXPECT_NE(areas["legal_notes"].dump().find("区分を付けた室が無い"), std::string::npos);
  EXPECT_NE(areas["legal_notes"].dump().find("「GL」という名前のレベルが無い"), std::string::npos);  // basements not looked at (docs/166), said
  // The garage 24 m2: at most 60 / 5 = 12 left out. The store 6 m2: at most 60 / 50 = 1.2.
  const auto steps = modeler.history().undoCount();
  EXPECT_EQ(ok("set_room_finish", {{"id", garage}, {"ratio_part", "garage"}})["ratio_part"], "garage");
  ok("set_room_finish", {{"id", store}, {"ratio_part", "stockpile"}, {"use", "倉庫"}});
  EXPECT_EQ(modeler.history().undoCount(), steps + 2);
  areas = ok("area_table", json::object());
  EXPECT_NEAR(areas["total_legal_floor"].get<double>(), 60.0, 1e-6);  // the total itself is not changed
  EXPECT_NEAR(areas["ratio_floor_area"].get<double>(), 60.0 - 12.0 - 1.2, 1e-6);
  ASSERT_EQ(areas["ratio_parts"].size(), 2u);
  EXPECT_EQ(areas["ratio_parts"][0]["part"], "garage");
  EXPECT_NEAR(areas["ratio_parts"][0]["area"].get<double>(), 24.0, 1e-6);
  EXPECT_NEAR(areas["ratio_parts"][0]["limit"].get<double>(), 12.0, 1e-6);
  EXPECT_NEAR(areas["ratio_parts"][0]["excluded"].get<double>(), 12.0, 1e-6);
  EXPECT_NEAR(areas["ratio_parts"][1]["excluded"].get<double>(), 1.2, 1e-6);
  EXPECT_NE(areas["legal_notes"].dump().find("自動車車庫等 1 室 24.00"), std::string::npos) << areas["legal_notes"].dump();
  // Kept with the room's other words; a part within its share is left out whole.
  EXPECT_EQ(sbr::doc::roomFinishOf(*modeler.document().find(store)).use, "倉庫");
  ok("set_room_finish", {{"id", garage}, {"ratio_part", ""}});
  ok("set_room_finish", {{"id", store}, {"ratio_part", "delivery_box"}});  // 6 > 60 / 100
  areas = ok("area_table", json::object());
  EXPECT_NEAR(areas["ratio_floor_area"].get<double>(), 60.0 - 0.6, 1e-6);
  // A lift shaft and common halls have no share: all of it.
  ok("set_room_finish", {{"id", store}, {"ratio_part", "elevator"}});
  ok("set_room_finish", {{"id", living}, {"ratio_part", "common_hall"}});
  areas = ok("area_table", json::object());
  EXPECT_NEAR(areas["ratio_floor_area"].get<double>(), 60.0 - 6.0 - 30.0, 1e-6);
  EXPECT_TRUE(areas["ratio_parts"][0]["limit"].is_null());
  // The table on a sheet and the CSV carry it.
  const auto sheet = sbr::doc::scheduleTable(modeler.document(), 2);
  ASSERT_GE(sheet.rows.size(), 3u);
  EXPECT_EQ(sheet.rows[sheet.rows.size() - 2].front(), "容積率の算定の延べ面積");
  EXPECT_EQ(sheet.rows[sheet.rows.size() - 2].back(), "24.00");
  EXPECT_EQ(sheet.rows.back().front(), "建築面積");
  for (const std::string& n : sheet.notes) EXPECT_LE(n.size(), 3 * 125u) << n;  // long notes are broken
  const std::string csv = sbr::doc::areaTableCsv(sbr::doc::areaTable(modeler.document()));
  EXPECT_NE(csv.find("容積率の算定の延べ面積"), std::string::npos);
  EXPECT_NE(csv.find("エレベーターの昇降路"), std::string::npos);
  // A code that is not listed is refused.
  EXPECT_FALSE(call("set_room_finish", {{"id", living}, {"ratio_part", "attic"}}).value("ok", true));
  // The marked room no longer a region of its own (the wall between it and the next taken away):
  // not evaluated, and the note names it.
  ok("set_room_finish", {{"id", garage}, {"ratio_part", "garage"}});
  ok("delete", {{"ids", {cut}}});
  areas = ok("area_table", json::object());
  EXPECT_EQ(areas["ratio_floor_area"], "not_evaluated") << areas.dump();
  EXPECT_NE(areas["legal_notes"].dump().find("車庫"), std::string::npos);
  EXPECT_NEAR(areas["total_legal_floor"].get<double>(), 60.0, 1e-6);
}

// docs/166: basements (令 1 条 2 号), with the ground at the level named GL: left out of the building
// area when 1 m or less above the ground (令 2 条 1 項 2 号); a dwelling's basement left out of the
// floor area for the ratio, up to 1/3 (法 52 条 3 項).
TEST(LegalAreas, BasementsAgainstTheGround) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto call = [&](const std::string& command, json args) { return json::parse(api.execute(json{{"command", command}, {"args", args}}.dump())); };
  auto ok = [&](const std::string& command, json args) {
    json r = call(command, args);
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  auto notes = [&](const json& areas) { return areas["legal_notes"].dump(); };
  // A basement 8000 x 6000 from -2500 (walls up to 500), and over it a ground floor 6000 x 6000 from 500.
  ASSERT_TRUE(modeler.setLevels({{"", "B1FL", -2500}, {"", "GL", 0}, {"", "1FL", 500}}));
  ok("create_wall", {{"points", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}, {0, 0}}}, {"thickness", 150}, {"height", 3000}, {"base", -2500}});
  ok("create_wall", {{"points", {{0, 0}, {6000, 0}, {6000, 6000}, {0, 6000}, {0, 0}}}, {"thickness", 150}, {"height", 3000}, {"base", 500}});
  const std::string below = ok("create_room", {{"at", {4000, 3000}}, {"base", -2500}, {"name", "地下室"}})["id"];
  const std::string above = ok("create_room", {{"at", {3000, 3000}}, {"base", 500}, {"name", "居間"}})["id"];
  json areas = ok("area_table", json::object());
  EXPECT_NEAR(areas["ground"].get<double>(), 0.0, 1e-9);
  ASSERT_EQ(areas["levels"].size(), 2u) << areas.dump();
  // B1: the ground 2500 over its floor, its ceiling 3000 (up to 1FL): a basement; its top 500 over
  // the ground: not in the building area. The ground floor only: 36.
  EXPECT_TRUE(areas["levels"][0]["basement"].get<bool>());
  EXPECT_NEAR(areas["levels"][0]["ceiling"].get<double>(), 3000, 1e-6);
  EXPECT_FALSE(areas["levels"][1]["basement"].get<bool>());
  EXPECT_NEAR(areas["building_area"].get<double>(), 36.0, 1e-6);
  EXPECT_NEAR(areas["total_legal_floor"].get<double>(), 48.0 + 36.0, 1e-6);  // floor area all the same
  EXPECT_NE(notes(areas).find("B1FL：地階"), std::string::npos) << notes(areas);
  EXPECT_NE(notes(areas).find("地盤面上 1 m 以下の B1FL は入れていない"), std::string::npos);
  // The ground floor 1500 up: the basement stands 1.5 m out of the ground: in the building area.
  ASSERT_TRUE(modeler.setLevels({{"", "B1FL", -2500}, {"", "GL", 0}, {"", "1FL", 1500}}));
  areas = ok("area_table", json::object());
  EXPECT_NEAR(areas["building_area"].get<double>(), 48.0, 1e-6);
  EXPECT_NE(notes(areas).find("1 m を超えるので入れた"), std::string::npos);
  ASSERT_TRUE(modeler.setLevels({{"", "B1FL", -2500}, {"", "GL", 0}, {"", "1FL", 500}}));
  // Shallow: the ground 900 over the floor; up to the next level 3000 (900 < 1000): not a basement.
  ASSERT_TRUE(modeler.setLevels({{"", "B1FL", -2500}, {"", "GL", -1600}, {"", "1FL", 500}}));
  areas = ok("area_table", json::object());
  EXPECT_FALSE(areas["levels"][0]["basement"].get<bool>());
  EXPECT_NEAR(areas["building_area"].get<double>(), 48.0, 1e-6);
  // With the room's ceiling 2400 (the rooms' ceilings, by area): 900 >= 800: a basement; its top
  // 2100 over the ground: in the building area still. Exactly 1/3 counts (以上).
  ok("set_room_finish", {{"id", below}, {"ceiling_height", 2400}});
  areas = ok("area_table", json::object());
  EXPECT_NEAR(areas["levels"][0]["ceiling"].get<double>(), 2400, 1e-6);
  EXPECT_TRUE(areas["levels"][0]["basement"].get<bool>());
  EXPECT_NEAR(areas["building_area"].get<double>(), 48.0, 1e-6);
  ASSERT_TRUE(modeler.setLevels({{"", "B1FL", -2500}, {"", "GL", -1700}, {"", "1FL", 500}}));
  EXPECT_TRUE(ok("area_table", json::object())["levels"][0]["basement"].get<bool>());  // 800 = 2400 / 3
  ASSERT_TRUE(modeler.setLevels({{"", "B1FL", -2500}, {"", "GL", 0}, {"", "1FL", 500}}));

  // A dwelling's basement, its ceiling (2400 over -2500) under the ground: out of the floor area
  // for the ratio, up to 1/3 of the total (84 / 3 = 28 of its 48).
  EXPECT_EQ(ok("set_room_finish", {{"id", below}, {"ratio_part", "basement_dwelling"}})["ratio_part"], "basement_dwelling");
  areas = ok("area_table", json::object());
  EXPECT_NEAR(areas["ratio_floor_area"].get<double>(), 84.0 - 28.0, 1e-6);
  ASSERT_EQ(areas["ratio_parts"].size(), 1u);
  EXPECT_NEAR(areas["ratio_parts"][0]["limit"].get<double>(), 28.0, 1e-6);
  EXPECT_NE(notes(areas).find("建物全体を住宅等として"), std::string::npos);
  // Its ceiling 3600 over its floor (1.1 m over the ground): not left out, and said.
  ok("set_room_finish", {{"id", below}, {"ceiling_height", 3600}});
  areas = ok("area_table", json::object());
  EXPECT_NEAR(areas["ratio_floor_area"].get<double>(), 84.0, 1e-6);
  EXPECT_NE(notes(areas).find("地下室 は、地階で天井が地盤面から 1 m 以下の室でない"), std::string::npos) << notes(areas);
  // The ground floor's room marked so: not a basement: not left out.
  ok("set_room_finish", {{"id", below}, {"ratio_part", ""}, {"ceiling_height", 0}});
  ok("set_room_finish", {{"id", above}, {"ratio_part", "basement_dwelling"}});
  areas = ok("area_table", json::object());
  EXPECT_NEAR(areas["ratio_floor_area"].get<double>(), 84.0, 1e-6);
  EXPECT_NE(notes(areas).find("居間 は"), std::string::npos);

  // No level named GL: basements are not looked at (said), the areas as before.
  ASSERT_TRUE(modeler.setLevels({{"", "B1FL", -2500}, {"", "1FL", 500}}));
  areas = ok("area_table", json::object());
  EXPECT_EQ(areas["ground"], "not_evaluated");
  EXPECT_FALSE(areas["levels"][0]["basement"].get<bool>());
  EXPECT_NEAR(areas["building_area"].get<double>(), 48.0, 1e-6);
  EXPECT_NE(notes(areas).find("「GL」という名前のレベルが無い"), std::string::npos);
  (void)call;
}

// docs/172: the quantities of the layers: per part, layer name and thickness, the elements, the
// area (volume over thickness) and the volume; all the layers of an element add up to its volume.
TEST(LayerQuantities, PerLayerAreasAndVolumes) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  const json sided = ok("create_wall", {{"points", {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}, {0, 0}}}, {"type", "exterior_siding"}, {"height", 2800}})["ids"];
  const std::string partition = ok("create_wall", {{"points", {{3000, 65}, {3000, 3935}}}, {"type", "interior_board"}, {"height", 2800}})["ids"][0];
  ok("create_wall", {{"points", {{0, -3000}, {4000, -3000}}}, {"thickness", 150}, {"height", 2800}});  // no layers
  ok("define_type", {{"name", "F1"}, {"part", "floor"}, {"layers", {{{"name", "flooring"}, {"thickness", 15}}, {{"name", "ply"}, {"thickness", 24}}, {{"name", "joist"}, {"thickness", 105}}}}});
  ok("batch", {{"commands", {{{"command", "create_slab"}, {"args", {{"outline", {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}}}, {"top", 0}, {"thickness", 144}}}},
                             {{"command", "set_type_name"}, {"args", {{"ids", {"$1.id"}}, {"name", "F1"}}}}}}});
  auto volumeOf = [&](const std::string& id) { return sbr::geom::volume(modeler.document().worldShape(*modeler.document().find(id))); };
  double sidedVolume = 0.0;
  for (const auto& id : sided) sidedVolume += volumeOf(id);
  const double sidedArea = sidedVolume / 160.5, partitionArea = 3870.0 * 2800;  // 160.5 thick; the partition 130 thick
  const json q = ok("layer_quantities", {{"include_csv", true}});
  EXPECT_EQ(q["without_layers"], 1);
  double wallVolume = 0.0, floorVolume = 0.0;
  int board = 0, floorRows = 0;
  for (const json& r : q["rows"]) {
    if (r["part"] == "壁") wallVolume += r["volume"].get<double>();
    if (r["part"] == "床") {
      floorVolume += r["volume"].get<double>();
      ++floorRows;
      EXPECT_NEAR(r["area"].get<double>(), 24.0, 1e-6);
      EXPECT_EQ(r["elements"], 1);
    }
    if (r["part"] == "壁" && std::abs(r["thickness"].get<double>() - 16.0) < 1e-9) {
      // The siding: on the four outer walls only.
      EXPECT_EQ(r["elements"], 4);
      EXPECT_NEAR(r["area"].get<double>(), sidedArea / 1e6, 1e-6);
    }
    if (r["part"] == "壁" && std::abs(r["thickness"].get<double>() - 12.5) < 1e-9) {
      // The board: inside the outer walls and on both faces of the partition (5 walls).
      ++board;
      EXPECT_EQ(r["elements"], 5);
      EXPECT_NEAR(r["area"].get<double>(), (sidedArea + 2 * partitionArea) / 1e6, 1e-6);
    }
  }
  EXPECT_EQ(board, 1);
  EXPECT_EQ(floorRows, 3);
  // All the layers make up the elements: the walls' volume, the floor's.
  EXPECT_NEAR(wallVolume, (sidedVolume + volumeOf(partition)) / 1e9, 1e-6);
  EXPECT_NEAR(floorVolume, 6000.0 * 4000 * 144 / 1e9, 1e-6);
  EXPECT_NE(q["csv"].get<std::string>().find("部材の数"), std::string::npos);  // its headings
  EXPECT_NE(q["notes"].dump().find("概算"), std::string::npos);
  // As a table for a sheet of its own.
  const auto sheet = sbr::doc::scheduleTable(modeler.document(), 5);
  EXPECT_EQ(sheet.title, "層の数量");
  EXPECT_EQ(sheet.rows.size(), q["rows"].size());
}

// docs/141: floor areas, the total and the building area by the Enforcement Order art. 2 (1).
TEST(LegalAreas, FloorAndBuildingAreasByTheOrder) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  ASSERT_TRUE(modeler.setLevels({{"", "1FL", 0}, {"", "2FL", 3000}, {"", "RFL", 6000}}));
  const json rect = {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}, {0, 0}};
  ok("create_wall", {{"points", rect}, {"thickness", 120}, {"height", 3000}});
  ok("create_wall", {{"points", rect}, {"thickness", 120}, {"height", 3000}, {"base", 3000}});
  ok("create_slab", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"top", 0}, {"thickness", 150}});
  // Upstairs the floor covers x 0..6000 only: the strip x 6000..8000 is open (a void) but for a
  // stair arriving there (800 wide, along x 7000 from y 500 to 4000).
  ok("create_slab", {{"outline", {{0, 0}, {6000, 0}, {6000, 6000}, {0, 6000}}}, {"top", 3000}, {"thickness", 200}});
  ok("create_stair", {{"path", {{7000, 500}, {7000, 4000}}}, {"rise", 3000}, {"width", 800}});
  auto table = sbr::doc::areaTable(modeler.document());
  ASSERT_EQ(table.rows.size(), 2u);
  EXPECT_TRUE(table.rows[0].legalKnown);
  EXPECT_NEAR(table.rows[0].legalFloor / 1e6, 48.0, 1e-6);
  EXPECT_TRUE(table.rows[1].legalKnown);
  EXPECT_NEAR(table.rows[1].legalFloor / 1e6, 36.0 + 0.8 * 3.5, 1e-6);  // the floor and the stair, not the void
  EXPECT_NEAR(table.legalTotal / 1e6, 48.0 + 38.8, 1e-6);
  EXPECT_TRUE(table.buildingKnown);
  EXPECT_NEAR(table.buildingArea / 1e6, 48.0, 1e-6);
  bool basis = false, voidSaid = false;
  for (const auto& n : table.legalNotes) {
    basis |= n.find("施行令 第 2 条") != std::string::npos && n.find("2026-11-16") != std::string::npos;
    voidSaid |= n.find("2FL") != std::string::npos && n.find("吹抜け") != std::string::npos;
  }
  EXPECT_TRUE(basis);
  EXPECT_TRUE(voidSaid);
  // A floor with no walls on it (a deck at RFL): 0, said; the total still known.
  ok("create_slab", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"top", 6000}, {"thickness", 150}});
  table = sbr::doc::areaTable(modeler.document());
  ASSERT_EQ(table.rows.size(), 3u);
  EXPECT_TRUE(table.rows[2].legalKnown);
  EXPECT_NEAR(table.rows[2].legalFloor, 0.0, 1e-9);
  EXPECT_NEAR(table.legalTotal / 1e6, 86.8, 1e-6);
  bool deckSaid = false;
  for (const auto& n : table.legalNotes) deckSaid |= n.find("RFL") != std::string::npos && n.find("0 とした") != std::string::npos;
  EXPECT_TRUE(deckSaid);
  // A hip roof with eaves 1200 (200 beyond 1 m): the building area grows by that ring.
  const std::string roof = ok("create_roof", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"kind", "hip"}, {"height", 6000}, {"eave", 1200}})["id"];
  table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.buildingArea / 1e6, 8.4 * 6.4, 1e-6);
  // Gable over the rectangle (ridge along x): only the eaves (south, north) count; the gables (300) do not.
  ok("change_roof", {{"id", roof}, {"kind", "gable"}, {"gable_eave", 1100}});
  table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.buildingArea / 1e6, 8.2 * 6.4, 1e-6);  // gables 1100: 100 each way
  ok("change_roof", {{"id", roof}, {"gable_eave", 300}});
  table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.buildingArea / 1e6, 8.0 * 6.4, 1e-6);
  // Eaves within 1 m: the walls' plan only.
  ok("change_roof", {{"id", roof}, {"eave", 900}});
  table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.buildingArea / 1e6, 48.0, 1e-6);
  // docs/160: one side's eave 1400 (the south side, 8 m long): 400 beyond 1 m along it.
  ok("change_roof", {{"id", roof}, {"sides", {{{"side", 0}, {"eave", 1400}}}}});
  table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.buildingArea / 1e6, 48.0 + 8.0 * 0.4, 1e-6);
  ok("change_roof", {{"id", roof}, {"sides", json::array()}});
  // The API says the same, and the table sheet carries the numbers.
  const json areas = ok("area_table", json::object());
  EXPECT_NEAR(areas["total_legal_floor"].get<double>(), 86.8, 1e-6);
  EXPECT_NEAR(areas["building_area"].get<double>(), 48.0, 1e-6);
  const auto sheet = sbr::doc::scheduleTable(modeler.document(), 2);
  EXPECT_EQ(sheet.rows.back().front(), "建築面積");
  EXPECT_EQ(sheet.rows.back().back(), "48.00");
}

// docs/146: the fittings as solids: frames, glass, door leaves; seen in elevations.
TEST_F(Schedules, FittingsAreSolidsInElevations) {
  const auto solids = sbr::draw::fittingSolids(modeler.document());
  std::map<std::string, int> parts;
  for (const auto& s : solids) ++parts[s.part];
  EXPECT_EQ(parts["frame"], 4);  // south 2 (引違い), north 1 (window), east door; the plain opening none
  EXPECT_EQ(parts["glass"], 5);  // 2 x 2 sliding panes, 1 fixed pane
  EXPECT_EQ(parts["leaf"], 1);   // the door
  // The door's frame: 900 x 2100 through the 200 wall, less the 840 x 2070 inside (no bottom rail).
  for (const auto& s : solids)
    if (s.host == east && s.part == "frame") EXPECT_NEAR(sbr::geom::volume(s.shape), (900.0 * 2100 - 840.0 * 2070) * 200, 1.0);
    else if (s.host == east && s.part == "leaf") EXPECT_NEAR(sbr::geom::volume(s.shape), 840.0 * 2070 * 40, 1.0);
  // docs/157: the door's own sizes: a 50 frame and a 30 leaf.
  {
    ok("set_opening_fittings", {{"host", east}, {"fitting", {{"type", "swing_single"}, {"frame", 50}, {"leaf", 30}}}});
    int found = 0;
    for (const auto& s : sbr::draw::fittingSolids(modeler.document())) {
      if (s.host != east) continue;
      if (s.part == "frame") {
        EXPECT_NEAR(sbr::geom::volume(s.shape), (900.0 * 2100 - 800.0 * 2050) * 200, 1.0);
        ++found;
      }
      if (s.part == "leaf") {
        EXPECT_NEAR(sbr::geom::volume(s.shape), 800.0 * 2050 * 30, 1.0);
        ++found;
      }
    }
    EXPECT_EQ(found, 2);
    // Kept with the fitting, shown in the list of openings.
    const json listed = ok("list_openings", {{"host", east}});
    EXPECT_NE(listed.dump().find("\"frame\":50"), std::string::npos) << listed.dump();
    // Refused: out of range, a size of no known kind.
    EXPECT_FALSE(call("set_opening_fittings", {{"host", east}, {"fitting", {{"type", "window"}, {"frame", 300}}}}).value("ok", true));
    EXPECT_FALSE(call("set_opening_fittings", {{"host", east}, {"fitting", {{"type", "window"}, {"frame", "wide"}}}}).value("ok", true));
    ok("set_opening_fittings", {{"host", east}, {"fitting", {{"type", "swing_single"}}}});
  }
  // The east elevation carries them: more lines than with the door's fitting taken off.
  auto lines = [&] {
    const auto view = sbr::draw::makeView(modeler.document(), sbr::draw::elevationView(modeler.document(), 1));
    EXPECT_TRUE(view);
    return view ? view->items.size() : 0u;
  };
  const std::size_t with = lines();
  ok("set_opening_fittings", {{"host", east}, {"fitting", {{"type", "none"}}}});
  EXPECT_LT(lines(), with);
}

// docs/147: a canopy over the east door; counted in the building area once it stands out 1 m.
TEST_F(Schedules, ACanopyOverADoorAndTheBuildingArea) {
  const double area0 = sbr::doc::areaTable(modeler.document()).buildingArea;
  const auto steps = modeler.history().undoCount();
  std::string canopy = ok("make_canopy", {{"host", east}, {"opening", 1}})["id"];
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  const auto* o = modeler.document().find(canopy);
  ASSERT_TRUE(o);
  EXPECT_EQ(o->attributes.at("kind"), "canopy");
  auto b = sbr::geom::exactBounds(modeler.document().worldShape(*o));
  EXPECT_NEAR(b.CornerMin().X(), 8100, 1e-6);   // the east wall's outer face (8000 + 100)
  EXPECT_NEAR(b.CornerMax().X(), 8700, 1e-6);   // 600 out
  EXPECT_NEAR(b.CornerMax().Y() - b.CornerMin().Y(), 900 + 300, 1e-6);
  EXPECT_NEAR(b.CornerMin().Z(), 2100 + 100, 1e-6);
  EXPECT_NEAR(b.CornerMax().Z(), 2100 + 100 + 60, 1e-6);
  // 600 + 100 (half the wall) from the wall's line: under 1 m, not counted.
  EXPECT_NEAR(sbr::doc::areaTable(modeler.document()).buildingArea, area0, 1e-3);
  ASSERT_TRUE(modeler.undo());
  EXPECT_FALSE(modeler.document().find(canopy));
  // 1500 out: 1600 from the line; back 1 m from its end: 500 out x 1200 counts.
  canopy = ok("make_canopy", {{"host", east}, {"opening", 1}, {"depth", 1500}})["id"];
  const auto t = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(t.buildingArea, area0 + 500.0 * 1200, 1.0);
  bool said = false;
  for (const auto& n : t.legalNotes) said |= n.find("庇") != std::string::npos;
  EXPECT_TRUE(said);
  // docs/148: it goes with its door: the door 600 along the wall (one undo step), then the wall 300 out.
  {
    const auto before = modeler.history().undoCount();
    ASSERT_TRUE(modeler.moveOpening(east, 1, gp_Vec(0, -600, 0)));
    EXPECT_EQ(modeler.history().undoCount(), before + 1);
    const sbr::doc::Object* now = nullptr;
    for (const auto& obj : modeler.document().objects())
      if (obj.attributes.count("kind") && obj.attributes.at("kind") == "canopy") now = &obj;
    ASSERT_TRUE(now);
    const auto door = modeler.openings(east)->front();
    const auto cb = sbr::geom::exactBounds(modeler.document().worldShape(*now));
    EXPECT_NEAR((cb.CornerMin().Y() + cb.CornerMax().Y()) / 2, door.center.Y(), 1e-6);
    EXPECT_NEAR(cb.CornerMin().X(), 8100, 1e-6);
    ASSERT_TRUE(modeler.undo());  // the door and the canopy back in one go
    const sbr::doc::Object* back = nullptr;
    for (const auto& obj : modeler.document().objects())
      if (obj.attributes.count("kind") && obj.attributes.at("kind") == "canopy") back = &obj;
    ASSERT_TRUE(back);
    EXPECT_NEAR((sbr::geom::exactBounds(modeler.document().worldShape(*back)).CornerMin().Y() +
                 sbr::geom::exactBounds(modeler.document().worldShape(*back)).CornerMax().Y()) / 2,
                modeler.openings(east)->front().center.Y(), 1e-6);
    ASSERT_TRUE(modeler.move({east}, gp_Vec(300, 0, 0)));
    for (const auto& obj : modeler.document().objects())
      if (obj.attributes.count("kind") && obj.attributes.at("kind") == "canopy")
        EXPECT_NEAR(sbr::geom::exactBounds(modeler.document().worldShape(obj)).CornerMin().X(), 8400, 1e-6);
    ASSERT_TRUE(modeler.undo());
  }
  // docs/154: its sizes changed: made again over the same door, its name kept, one undo step.
  {
    std::string id, name;
    for (const auto& obj : modeler.document().objects())
      if (obj.attributes.count("kind") && obj.attributes.at("kind") == "canopy") id = obj.id, name = obj.name;
    ASSERT_FALSE(id.empty());
    const auto steps = modeler.history().undoCount();
    const std::string again = ok("change_canopy", {{"id", id}, {"depth", 900}, {"above", 300}})["id"];
    EXPECT_EQ(modeler.history().undoCount(), steps + 1);
    EXPECT_FALSE(modeler.document().find(id));
    const auto* c = modeler.document().find(again);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->name, name);
    const auto spec = modeler.canopySpec(again);
    ASSERT_TRUE(spec);
    EXPECT_NEAR(spec->depth, 900, 1e-9);
    EXPECT_NEAR(spec->above, 300, 1e-9);
    const auto cb = sbr::geom::exactBounds(modeler.document().worldShape(*c));
    const auto door = modeler.openings(east)->front();
    EXPECT_NEAR(cb.CornerMax().X() - cb.CornerMin().X(), 900, 1e-6);
    EXPECT_NEAR(cb.CornerMin().Z(), door.bottom + door.height + 300, 1e-6);
    EXPECT_FALSE(call("change_canopy", {{"id", again}, {"thickness", 900}}).value("ok", true));  // too thick: as it was
    EXPECT_TRUE(modeler.document().find(again));
    ASSERT_TRUE(modeler.undo());
    EXPECT_TRUE(modeler.document().find(id));
    EXPECT_FALSE(modeler.document().find(again));
  }
  // Refused: no such opening, a canopy too deep.
  EXPECT_FALSE(call("make_canopy", {{"host", east}, {"opening", 9}}).value("ok", true));
  EXPECT_FALSE(call("make_canopy", {{"host", east}, {"opening", 1}, {"depth", 9000}}).value("ok", true));
}

// docs/163: a balcony on a wall: its solid, the areas, going with its wall.
TEST_F(Schedules, ABalconyOnAWallAndTheAreas) {
  auto balconies = [&] {
    std::vector<std::string> ids;
    for (const auto& o : modeler.document().objects())
      if (o.attributes.count("kind") && o.attributes.at("kind") == "balcony") ids.push_back(o.id);
    return ids;
  };
  auto bounds = [&](const std::string& id) { return sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(id))); };
  auto solidVolume = [&](const std::string& id) { return sbr::geom::volume(modeler.document().worldShape(*modeler.document().find(id))); };
  const auto steps = modeler.history().undoCount();
  // Pointed at outside the south wall (drawn along +x: its outside is on the right of its line).
  json made = ok("create_balcony", {{"host", south}, {"at", {4000, -500}}});
  std::string id = made["id"];
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  EXPECT_NEAR(made["along"].get<double>(), 3900, 1e-6);  // from the wall's own start (x = 100: it butts against the west wall)
  EXPECT_EQ(made["side"], -1);
  EXPECT_NEAR(made["top"].get<double>(), 0, 1e-6);  // the wall's foot
  EXPECT_EQ(made["floor_area"], "");
  EXPECT_EQ(modeler.document().find(id)->attributes.at("kind"), "balcony");
  auto b = bounds(id);
  EXPECT_NEAR(b.CornerMin().X(), 4000 - 1365, 1e-4);
  EXPECT_NEAR(b.CornerMax().X(), 4000 + 1365, 1e-4);
  EXPECT_NEAR(b.CornerMax().Y(), -100, 1e-4);  // the wall's outer face (200 thick on its line)
  EXPECT_NEAR(b.CornerMin().Y(), -100 - 910, 1e-4);
  EXPECT_NEAR(b.CornerMin().Z(), -150, 1e-4);
  EXPECT_NEAR(b.CornerMax().Z(), 1100, 1e-4);
  // The floor and a parapet 100 thick on three sides.
  EXPECT_NEAR(solidVolume(id), 2730.0 * 910 * 150 + (2730.0 * 100 + 2 * 100.0 * 810) * 1100, 10.0);
  int solids = 0;
  for (TopExp_Explorer e(modeler.document().worldShape(*modeler.document().find(id)), TopAbs_SOLID); e.More(); e.Next()) ++solids;
  EXPECT_EQ(solids, 1);
  // 1010 from the wall's line: not 1 m beyond... it is: but nothing lies back 1 m from its end past the face.
  auto table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.buildingArea / 1e6, 48.0, 1e-6);
  EXPECT_EQ(table.balconies, 1);
  EXPECT_EQ(table.balconiesCounted, 0);
  EXPECT_NEAR(table.rows[0].legalFloor / 1e6, 48.0, 1e-6);  // by the notice: all within 2 m of the wall's line
  bool said = false;
  for (const auto& n : table.legalNotes) said |= n.find("バルコニー 1 個") != std::string::npos && n.find("通達どおりに数えた 1 個") != std::string::npos;
  EXPECT_TRUE(said);
  // 1500 out: the building area takes it up to 1 m back from its end (500 beyond the face).
  made = ok("change_balcony", {{"id", id}, {"depth", 1500}});
  EXPECT_FALSE(modeler.document().find(id));
  id = made["id"];
  table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.buildingArea / 1e6, 48.0 + 2.73 * 0.5, 1e-6);
  // docs/175: by the notice (住指発第 115 号 1 (4)(5)): 3000 out (2900 beyond the wall's face plus
  // its 100 half) - open to the sky: only beyond 2 m from the wall's centre line counts (1100).
  id = ok("change_balcony", {{"id", id}, {"depth", 3000}})["id"];
  table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.rows[0].legalFloor / 1e6, 48.0 + 2.73 * 1.1, 1e-6);
  EXPECT_EQ(table.balconiesCounted, 1);
  // A floor over it 2250 above (its underside): the parapet 1100 leaves 1150 open, 1.1 m or more
  // and half of 2250 or more: the same.
  const std::string over = ok("create_slab", {{"outline", {{0, -3200}, {8000, -3200}, {8000, 0}, {0, 0}}}, {"top", 2400}, {"thickness", 150}})["id"];
  table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.rows[0].legalFloor / 1e6, 48.0 + 2.73 * 1.1, 1e-6);
  // A parapet 1200: 1050 open, less than 1.1 m: all of it counts.
  id = ok("change_balcony", {{"id", id}, {"rail_height", 1200}})["id"];
  table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.rows[0].legalFloor / 1e6, 48.0 + 2.73 * 3.0, 1e-6);
  // A rail on posts (open): from the floor, 2250 open: only beyond 2 m again.
  id = ok("change_balcony", {{"id", id}, {"rail", "bars"}})["id"];
  table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.rows[0].legalFloor / 1e6, 48.0 + 2.73 * 1.1, 1e-6);
  said = false;
  for (const auto& n : table.legalNotes) said |= n.find("住指発第 115 号") != std::string::npos;
  EXPECT_TRUE(said);
  ok("delete", {{"ids", {over}}});
  id = ok("change_balcony", {{"id", id}, {"depth", 1500}, {"rail", "wall"}, {"rail_height", 1100}})["id"];
  // Said to count as floor area: added to the storey its floor is at.
  id = ok("change_balcony", {{"id", id}, {"floor_area", "count"}})["id"];
  table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.rows[0].legalFloor / 1e6, 48.0 + 2.73 * 1.5, 1e-6);
  EXPECT_EQ(table.balconiesCounted, 1);
  EXPECT_NEAR(ok("area_table")["total_legal_floor"].get<double>(), 48.0 + 2.73 * 1.5, 1e-6);
  // Its floor at a height with no storey: not counted, and said.
  id = ok("change_balcony", {{"id", id}, {"top", 1234}})["id"];
  table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.rows[0].legalFloor / 1e6, 48.0, 1e-6);
  said = false;
  for (const auto& n : table.legalNotes) said |= n.find("レベル（階）が無い") != std::string::npos;
  EXPECT_TRUE(said);
  id = ok("change_balcony", {{"id", id}, {"top", 0}, {"floor_area", "exclude"}, {"depth", 910}})["id"];
  EXPECT_EQ(sbr::doc::areaTable(modeler.document()).balconiesExcluded, 1);
  // Bars: a top rail on posts; none: the floor only.
  id = ok("change_balcony", {{"id", id}, {"rail", "bars"}, {"rail_height", 1000}})["id"];
  EXPECT_NEAR(bounds(id).CornerMax().Z(), 1000, 1e-4);
  EXPECT_LT(solidVolume(id), 2730.0 * 910 * 150 + (2730.0 * 100 + 2 * 100.0 * 810) * 1100 * 0.2);
  id = ok("change_balcony", {{"id", id}, {"rail", "none"}})["id"];
  EXPECT_NEAR(solidVolume(id), 2730.0 * 910 * 150, 1.0);
  EXPECT_EQ(ok("balcony_info", {{"id", id}})["rail"], "none");

  // The wall moved 500 outwards: the balcony goes with it, in the same undo step.
  const auto before = modeler.history().undoCount();
  ok("move", {{"ids", {south}}, {"vector", {0, -500, 0}}});
  ASSERT_EQ(balconies().size(), 1u);
  EXPECT_FALSE(modeler.document().find(id));
  std::string followed = balconies().front();
  EXPECT_NEAR(bounds(followed).CornerMax().Y(), -600, 1e-4);
  EXPECT_EQ(modeler.history().undoCount(), before + 1);
  ASSERT_TRUE(modeler.undo());
  ASSERT_TRUE(modeler.document().find(id));
  EXPECT_NEAR(bounds(id).CornerMax().Y(), -100, 1e-4);
  // Moved by hand it is no longer as made: it stays when the wall moves.
  ok("move", {{"ids", {id}}, {"vector", {0, 0, 300}}});
  ok("move", {{"ids", {south}}, {"vector", {0, -500, 0}}});
  ASSERT_TRUE(modeler.document().find(id));
  EXPECT_NEAR(bounds(id).CornerMax().Y(), -100, 1e-4);
  ASSERT_TRUE(modeler.undo());
  ASSERT_TRUE(modeler.undo());
  // On the other face; in the middle of the wall by default.
  made = ok("create_balcony", {{"host", north}, {"side", -1}, {"width", 1820}});
  EXPECT_NEAR((bounds(made["id"]).CornerMin().X() + bounds(made["id"]).CornerMax().X()) / 2, 3900, 1e-4);  // the middle of the wall as it stands (one end butts, the other runs through)
  EXPECT_NEAR(bounds(made["id"]).CornerMin().Y(), 6100, 1e-4);  // north wall drawn along -x: its right is outside
  // Refused: not a wall, too narrow, beyond the wall, a rail that is not known.
  EXPECT_FALSE(call("create_balcony", {{"host", room}}).value("ok", true));
  EXPECT_FALSE(call("create_balcony", {{"host", south}, {"width", 100}}).value("ok", true));
  EXPECT_FALSE(call("create_balcony", {{"host", south}, {"along", 9000}}).value("ok", true));
  EXPECT_FALSE(call("create_balcony", {{"host", south}, {"rail", "glass"}}).value("ok", true));
  EXPECT_FALSE(call("change_balcony", {{"id", id}, {"floor_area", "half"}}).value("ok", true));
  EXPECT_TRUE(modeler.document().find(id));
  // docs/167: on two posts from the level below: the building area takes all of it up to the
  // posts' centre line. The fixture's floors: 1FL 0, 2FL 3000: on 2FL, posts from 1FL.
  id = ok("change_balcony", {{"id", id}, {"top", 3000}, {"posts", 2}, {"rail", "wall"}, {"depth", 910}})["id"];
  EXPECT_EQ(ok("balcony_info", {{"id", id}})["posts"], 2);
  EXPECT_NEAR(bounds(id).CornerMin().Z(), 0, 1e-4);  // the posts stand on 1FL
  EXPECT_NEAR(solidVolume(id), 2730.0 * 910 * 150 + (2730.0 * 100 + 2 * 100.0 * 810) * 1000 + 2 * 105.0 * 105 * 2850, 10.0);  // the rail 1000 high since above
  table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.buildingArea / 1e6, 48.0 + 2.73 * (0.91 - 0.0525), 1e-6);  // face (y -100) to the posts' centre
  said = false;
  for (const auto& n : table.legalNotes) said |= n.find("柱で支えたバルコニー 1 個") != std::string::npos;
  EXPECT_TRUE(said);
  // It goes with its wall, posts and all (the north wall's balcony stays where it is).
  auto onSouth = [&] {
    for (const std::string& b : balconies())
      if (modeler.document().find(b)->attributes.at("host") == south) return b;
    return std::string();
  };
  const auto steps2 = modeler.history().undoCount();
  ok("move", {{"ids", {south}}, {"vector", {0, -300, 0}}});
  ASSERT_FALSE(onSouth().empty());
  EXPECT_NEAR(bounds(onSouth()).CornerMax().Y(), -400, 1e-4);
  EXPECT_NEAR(bounds(onSouth()).CornerMin().Z(), 0, 1e-4);
  EXPECT_EQ(modeler.history().undoCount(), steps2 + 1);
  ASSERT_TRUE(modeler.undo());
  // Refused: one post, posts wider than the balcony, posts with nothing below.
  EXPECT_FALSE(call("change_balcony", {{"id", id}, {"posts", 1}}).value("ok", true));
  EXPECT_FALSE(call("change_balcony", {{"id", id}, {"posts", 6}, {"post_size", 500}}).value("ok", true));
  EXPECT_FALSE(call("change_balcony", {{"id", id}, {"top", 0}}).value("ok", true));
  ok("change_balcony", {{"id", id}, {"posts", 0}});
  // Its wall deleted: it stays.
  ok("delete", {{"ids", {south}}});
  EXPECT_FALSE(onSouth().empty());
}

// docs/168: a balcony round the outer corner where the south wall meets the east one (an L).
TEST_F(Schedules, ABalconyGoesRoundACorner) {
  auto bounds = [&](const std::string& id) { return sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(id))); };
  auto solidVolume = [&](const std::string& id) { return sbr::geom::volume(modeler.document().worldShape(*modeler.document().find(id))); };
  auto only = [&] {
    std::vector<std::string> ids;
    for (const auto& o : modeler.document().objects())
      if (o.attributes.count("kind") && o.attributes.at("kind") == "balcony") ids.push_back(o.id);
    EXPECT_EQ(ids.size(), 1u);
    return ids.empty() ? std::string() : ids.front();
  };
  const auto steps = modeler.history().undoCount();
  // Along the south wall from x 2635 (its middle at 4000, 2730 wide) to the corner (8100, -100),
  // then 1820 along the east wall's outer face (x 8100) northwards; 910 out.
  json made = ok("create_balcony", {{"host", south}, {"at", {4000, -500}}, {"wrap", "end"}});
  std::string id = made["id"];
  EXPECT_EQ(made["wrap"], "end");
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  auto b = bounds(id);
  EXPECT_NEAR(b.CornerMin().X(), 2635, 1e-4);
  EXPECT_NEAR(b.CornerMax().X(), 8100 + 910, 1e-4);
  EXPECT_NEAR(b.CornerMin().Y(), -100 - 910, 1e-4);
  EXPECT_NEAR(b.CornerMax().Y(), -100 + 1820, 1e-4);
  const double floorArea = 6375.0 * 910 + 910.0 * 1820;
  // The floor and a parapet 100 thick along its four free edges (overlapping at three corners).
  const double parapet = 100.0 * (910 + 6375 + 2730 + 910) - 3 * 100.0 * 100;
  EXPECT_NEAR(solidVolume(id), floorArea * 150 + parapet * 1100, 10.0);
  auto table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.balconyArea, floorArea, 1.0);
  EXPECT_NEAR(table.buildingArea / 1e6, 48.0, 1e-6);  // 910 + 100: nothing back 1 m from its end beyond the faces
  // 1500 out: both legs count, 500 beyond the faces.
  id = ok("change_balcony", {{"id", id}, {"depth", 1500}})["id"];
  table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.buildingArea / 1e6, 48.0 + (5965.0 * 500 + 500.0 * 1820) / 1e6, 1e-6);
  id = ok("change_balcony", {{"id", id}, {"depth", 910}})["id"];
  // It goes with both walls: the east wall 300 east, the south wall 300 south.
  auto before = modeler.history().undoCount();
  ok("move", {{"ids", {east}}, {"vector", {300, 0, 0}}});
  EXPECT_NEAR(bounds(only()).CornerMax().X(), 8400 + 910, 1e-4);
  EXPECT_EQ(modeler.history().undoCount(), before + 1);
  ok("move", {{"ids", {south}}, {"vector", {0, -300, 0}}});
  EXPECT_NEAR(bounds(only()).CornerMin().Y(), -400 - 910, 1e-4);
  ASSERT_TRUE(modeler.undo());
  ASSERT_TRUE(modeler.undo());
  id = only();
  EXPECT_NEAR(bounds(id).CornerMax().X(), 8100 + 910, 1e-4);
  // On posts: three, under its far corners, from 1FL (0) up to its floor at 3000.
  id = ok("change_balcony", {{"id", id}, {"top", 3000}, {"posts", 2}, {"rail", "none"}})["id"];
  EXPECT_NEAR(solidVolume(id), floorArea * 150 + 3 * 105.0 * 105 * 2850, 10.0);
  EXPECT_NEAR(bounds(id).CornerMin().Z(), 0, 1e-4);
  table = sbr::doc::areaTable(modeler.document());
  EXPECT_NEAR(table.buildingArea / 1e6, 48.0 + (6322.5 * 857.5 + 857.5 * 1820) / 1e6, 1e-6);  // to the posts' centre line
  // Straight again.
  id = ok("change_balcony", {{"id", id}, {"wrap", ""}, {"posts", 0}})["id"];
  EXPECT_NEAR(bounds(id).CornerMax().X(), 4000 + 1365, 1e-4);
  // Refused, nothing changed: on the inner face, too short round the corner, an end that is not one, a wall alone.
  EXPECT_FALSE(call("change_balcony", {{"id", id}, {"wrap", "end"}, {"side", 1}}).value("ok", true));
  EXPECT_FALSE(call("change_balcony", {{"id", id}, {"wrap", "end"}, {"wrap_length", 100}}).value("ok", true));
  EXPECT_FALSE(call("change_balcony", {{"id", id}, {"wrap", "around"}}).value("ok", true));
  const std::string loose = ok("create_wall", {{"points", {{0, -5000}, {4000, -5000}}}, {"thickness", 200}, {"height", 3000}})["ids"][0];
  EXPECT_FALSE(call("create_balcony", {{"host", loose}, {"wrap", "end"}}).value("ok", true));
  EXPECT_TRUE(modeler.document().find(id));
  // Round the other way: at the south wall's start, onto the west wall.
  made = ok("change_balcony", {{"id", id}, {"wrap", "start"}, {"wrap_length", 910}});
  b = bounds(made["id"]);
  EXPECT_NEAR(b.CornerMin().X(), -100 - 910, 1e-4);
  EXPECT_NEAR(b.CornerMax().X(), 4000 + 1365, 1e-4);
  EXPECT_NEAR(b.CornerMax().Y(), -100 + 910, 1e-4);
}

// docs/171: fitting types: a kind and sizes under a name, given to openings in any wall; defined
// again, all of them change, each keeping its own hands; removed, they stay as they are.
TEST_F(Schedules, FittingTypesAreKindsAndSizesByName) {
  auto fittingOf = [&](const std::string& host, int k) {
    return ok("list_openings", {{"ids", {host}}})["hosts"][0]["openings"][k]["fitting"];
  };
  const auto steps = modeler.history().undoCount();
  EXPECT_EQ(ok("define_fitting_type", {{"name", "AW-1"}, {"fitting", {{"type", "sliding_double"}, {"frame", 40}, {"glass", 8}}}})["changed"], 0);
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  ok("set_opening_fittings", {{"host", south}, {"fitting", {{"type_name", "AW-1"}}}});  // both south windows
  for (int k : {0, 1}) {
    const json f = fittingOf(south, k);
    EXPECT_EQ(f["type"], "sliding_double");
    EXPECT_EQ(f["frame"], 40.0);
    EXPECT_EQ(f["glass"], 8.0);
    EXPECT_EQ(f["type_name"], "AW-1");
  }
  // Placement defaults are independent of existing opening geometry.
  EXPECT_FALSE(ok("list_fitting_types")["types"][0].contains("opening_size"));
  const json size = {{"width", 780}, {"height", 1250}, {"sill", 900}};
  const json shape = {{"type", "sliding_double"}, {"frame", 40}, {"glass", 8}};
  const auto placed = modeler.openings(south);
  ASSERT_TRUE(placed && placed->size() == 2);
  EXPECT_FALSE(call("define_fitting_type", {{"name", "AW-1"}, {"fitting", shape},
                                            {"opening_size", {{"width", 780}, {"height", 1250}, {"sill", -1}}}})
                   .value("ok", true));
  EXPECT_FALSE(ok("list_fitting_types")["types"][0].contains("opening_size"));
  ok("define_fitting_type", {{"name", "AW-1"}, {"fitting", shape}, {"opening_size", size}});
  EXPECT_EQ(ok("list_fitting_types")["types"][0]["opening_size"], size);
  EXPECT_EQ(modeler.openings(south)->front().width, placed->front().width);
  ASSERT_TRUE(modeler.undo());
  EXPECT_FALSE(ok("list_fitting_types")["types"][0].contains("opening_size"));
  ASSERT_TRUE(modeler.redo());

  // Defined again: both change, in one step; undone, both as they were.
  const auto before = modeler.history().undoCount();
  EXPECT_EQ(ok("define_fitting_type", {{"name", "AW-1"}, {"fitting", {{"type", "sliding_double"}, {"frame", 50}, {"glass", 8}}}})["changed"], 2);
  EXPECT_EQ(modeler.history().undoCount(), before + 1);
  EXPECT_EQ(fittingOf(south, 1)["frame"], 50.0);
  EXPECT_EQ(ok("list_fitting_types")["types"][0]["opening_size"], size);
  ASSERT_TRUE(modeler.undo());
  EXPECT_EQ(fittingOf(south, 0)["frame"], 40.0);
  ASSERT_TRUE(modeler.redo());
  // A door type: its hands are each door's own (the type keeps none).
  ok("define_fitting_type", {{"name", "SD"}, {"fitting", {{"type", "swing_single"}, {"hinge", "right"}, {"leaf", 30}}}});
  EXPECT_FALSE(ok("list_fitting_types")["types"][1]["fitting"].contains("hinge"));
  ok("set_opening_fittings", {{"host", east}, {"fitting", {{"type_name", "SD"}, {"hinge", "left"}, {"swing", "out"}}}});
  EXPECT_EQ(fittingOf(east, 0)["hinge"], "left");
  EXPECT_EQ(fittingOf(east, 0)["swing"], "out");
  EXPECT_EQ(fittingOf(east, 0)["leaf"], 30.0);
  ok("define_fitting_type", {{"name", "SD"}, {"fitting", {{"type", "swing_single"}, {"leaf", 35}}}});
  EXPECT_EQ(fittingOf(east, 0)["leaf"], 35.0);
  EXPECT_EQ(fittingOf(east, 0)["hinge"], "left");  // kept
  // The door's leaf in the solids follows the type.
  for (const auto& s : sbr::draw::fittingSolids(modeler.document()))
    if (s.host == east && s.part == "leaf") EXPECT_NEAR(sbr::geom::volume(s.shape), 840.0 * 2070 * 35, 1.0);
  // Not among the floor / roof types.
  for (const auto& t : ok("list_types")["types"]) EXPECT_NE(t["name"], "AW-1");
  // Refused: an unknown name, none as a type, a name that is a floor type.
  EXPECT_FALSE(call("set_opening_fittings", {{"host", north}, {"fitting", {{"type_name", "W-9"}}}}).value("ok", true));
  EXPECT_FALSE(call("define_fitting_type", {{"name", "X"}, {"fitting", {{"type", "none"}}}}).value("ok", true));
  ok("define_type", {{"name", "F1"}, {"part", "floor"}, {"thickness", 150}});
  EXPECT_FALSE(call("define_fitting_type", {{"name", "F1"}, {"fitting", {{"type", "window"}}}}).value("ok", true));
  // Removed: the windows keep their kind and sizes, without the name.
  EXPECT_EQ(ok("remove_fitting_type", {{"name", "AW-1"}})["unnamed"], 2);
  EXPECT_EQ(fittingOf(south, 0)["frame"], 50.0);
  EXPECT_FALSE(fittingOf(south, 0).contains("type_name"));
  EXPECT_EQ(ok("list_fitting_types")["types"].size(), 1u);
}

// docs/174: sliding windows' sashes (stiles and rails round their panes) and hinged doors'
// lever handles on both faces.
TEST_F(Schedules, SashesAndHandles) {
  std::map<std::string, std::vector<double>> parts;  // part -> volumes
  auto read = [&] {
    parts.clear();
    for (const auto& s : sbr::draw::fittingSolids(modeler.document())) parts[s.part].push_back(sbr::geom::volume(s.shape));
  };
  read();
  // The two 引違い windows on the south: two sashes each; the plain window none.
  ASSERT_EQ(parts["sash"].size(), 4u);
  const double sash = (795.0 * 1140 - 725.0 * 1070) * 30;  // 1600 x 1200 in a 30 frame: each sash 795 x 1140
  for (const double v : parts["sash"]) EXPECT_NEAR(v, sash, 1.0);
  int smallPanes = 0;
  for (const double v : parts["glass"]) smallPanes += std::abs(v - 725.0 * 1070 * 6) < 1.0;
  EXPECT_EQ(smallPanes, 4);  // the panes inside the sashes
  // The east door (hinged): a rose and a lever on each face.
  ASSERT_EQ(parts["handle"].size(), 4u);
  std::multiset<long long> sizes;
  for (const double v : parts["handle"]) sizes.insert(std::llround(v));
  EXPECT_EQ(sizes.count(125000), 2u);  // roses 50 x 50 x 50
  EXPECT_EQ(sizes.count(48000), 2u);   // levers 120 x 20 x 20
  // 900 above the floor, outside the leaf's faces, near the edge away from the hinge.
  for (const auto& s : sbr::draw::fittingSolids(modeler.document())) {
    if (s.part != "handle") continue;
    const auto b = sbr::geom::exactBounds(s.shape);
    EXPECT_NEAR((b.CornerMin().Z() + b.CornerMax().Z()) / 2, 900, 1e-6);
  }
  // Wider stiles: 50.
  ok("set_opening_fittings", {{"host", south}, {"fitting", {{"type", "sliding_double"}, {"sash", 50}}}});
  read();
  for (const double v : parts["sash"]) EXPECT_NEAR(v, (795.0 * 1140 - 695.0 * 1040) * 30, 1.0);
  // A double door: a handle pair on each leaf, at the meeting edge.
  ok("set_opening_fittings", {{"host", east}, {"fitting", {{"type", "swing_double"}}}});
  read();
  EXPECT_EQ(parts["handle"].size(), 8u);
  // A sliding door: none (pulls not made yet).
  ok("set_opening_fittings", {{"host", east}, {"fitting", {{"type", "sliding_single"}}}});
  read();
  EXPECT_TRUE(parts["handle"].empty());
  EXPECT_FALSE(call("set_opening_fittings", {{"host", south}, {"fitting", {{"type", "sliding_double"}, {"sash", 5}}}}).value("ok", true));
}

// docs/113: a table on a sheet of its own.
TEST_F(Schedules, ATableGoesOnASheetOfItsOwn) {
  const sbr::draw::Table table = sbr::doc::scheduleTable(modeler.document(), 0);
  EXPECT_EQ(table.title, "建具表");
  ASSERT_EQ(table.rows.size(), 5u);  // AW1, AW2, AD1, O1 and the total
  EXPECT_EQ(table.rows.front().front(), "AW1");
  EXPECT_EQ(table.rows.back()[5], "5");
  sbr::draw::SheetSpec spec;
  spec.title.drawing = table.title;
  const auto sheet = sbr::draw::tableSheet(table, spec);
  ASSERT_TRUE(sheet) << sheet.error();
  EXPECT_EQ(sheet->paper, "A4");
  std::map<std::string, int> texts;
  int rules = 0;
  for (const auto& item : sheet->items) {
    if (item.kind == sbr::draw::Item::Kind::Text) ++texts[item.text];
    if (item.kind == sbr::draw::Item::Kind::Line) ++rules;
  }
  EXPECT_EQ(texts["AW1"], 1);
  EXPECT_EQ(texts["引違い"], 1);
  EXPECT_GE(texts["建具表"], 2);  // over the table and in the title block
  EXPECT_GE(rules, 7 + 10);  // 7 rules across (title, headings, 5 rows), 10 columns
  // Everything inside the paper.
  for (const auto& item : sheet->items)
    for (const auto& p : item.points) {
      EXPECT_GE(p.X() / sheet->scale, -0.1);
      EXPECT_LE(p.X() / sheet->scale, sheet->paperWidth + 0.1);
      EXPECT_GE(p.Y() / sheet->scale, -0.1);
      EXPECT_LE(p.Y() / sheet->scale, sheet->paperHeight + 0.1);
    }
  // The rooms and areas tables too, and as files.
  EXPECT_EQ(sbr::doc::scheduleTable(modeler.document(), 1).rows.front().front(), "居間");
  EXPECT_EQ(sbr::doc::scheduleTable(modeler.document(), 2).rows.front().back(), "48.00");  // docs/141
  const auto dir = std::filesystem::temp_directory_path() / "sbr_schedule_sheet";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  for (const char* which : {"openings", "rooms", "areas"}) {
    for (const char* ext : {".dxf", ".jww"}) {
      const std::string path = (dir / (std::string(which) + ext)).u8string();
      const json r = ok("export_schedule", {{"path", path}, {"table", which}});
      EXPECT_EQ(r["paper"], "A4") << which;
      EXPECT_GT(std::filesystem::file_size(path), 500u) << path;
    }
  }
  EXPECT_FALSE(call("export_schedule", {{"path", (dir / "x.txt").u8string()}, {"table", "openings"}}).value("ok", true));
  EXPECT_FALSE(call("export_schedule", {{"path", (dir / "x.dxf").u8string()}, {"table", "walls"}}).value("ok", true));
  std::filesystem::remove_all(dir);
}

// docs/130: a plan with its tables beside it on one sheet.
TEST_F(Schedules, ADrawingCarriesItsTablesOnTheSameSheet) {
  const auto view = sbr::draw::makeView(modeler.document(), sbr::draw::planView(1200));
  ASSERT_TRUE(view) << view.error();
  sbr::draw::SheetSpec spec;
  spec.title.drawing = "1FL 平面図";
  const auto alone = sbr::draw::layoutSheet(*view, spec);
  ASSERT_TRUE(alone);
  // No tables: the same sheet as on its own.
  const auto same = sbr::draw::layoutSheetWithTables(*view, {}, spec);
  ASSERT_TRUE(same);
  EXPECT_EQ(same->items.size(), alone->items.size());
  EXPECT_EQ(same->paper, alone->paper);
  const sbr::draw::Table openings = sbr::doc::scheduleTable(modeler.document(), 0);
  const sbr::draw::Table rooms = sbr::doc::scheduleTable(modeler.document(), 1);
  const auto sheet = sbr::draw::layoutSheetWithTables(*view, {openings, rooms}, spec);
  ASSERT_TRUE(sheet) << sheet.error();
  EXPECT_TRUE(sheet->notes.empty());
  // Each table's title once over it; the drawing's name in the title block and its caption.
  std::map<std::string, int> texts;
  double drawingRight = -1e300, tableLeft = 1e300;
  for (const auto& item : sheet->items) {
    if (item.kind == sbr::draw::Item::Kind::Text) ++texts[item.text];
    if (item.kind == sbr::draw::Item::Kind::Fill)  // the walls cut by the plan
      for (const auto& p : item.points) drawingRight = std::max(drawingRight, p.X());
    if (item.kind == sbr::draw::Item::Kind::Text && (item.text == openings.headers[0] || item.text == rooms.headers[0]))
      tableLeft = std::min(tableLeft, item.points[0].X());
  }
  EXPECT_EQ(texts["建具表"], 1);
  EXPECT_EQ(texts["室の表"], 1);
  EXPECT_GE(texts["1FL 平面図"], 1);
  EXPECT_EQ(texts["N"], 1);  // the north mark, once
  ASSERT_GT(drawingRight, -1e300);
  EXPECT_GT(tableLeft, drawingRight);  // beside the drawing, not over it
  for (const auto& item : sheet->items)
    for (const auto& p : item.points) {
      EXPECT_GE(p.X() / sheet->scale, -0.1);
      EXPECT_LE(p.X() / sheet->scale, sheet->paperWidth + 0.1);
      EXPECT_GE(p.Y() / sheet->scale, -0.1);
      EXPECT_LE(p.Y() / sheet->scale, sheet->paperHeight + 0.1);
    }
  // At 1:50 on A3 asked for: does not all fit, said.
  spec.scale = 50;
  spec.paper = "A4";
  const auto tight = sbr::draw::layoutSheetWithTables(*view, {openings, rooms}, spec);
  ASSERT_TRUE(tight);
  EXPECT_FALSE(tight->notes.empty());
  // By the API: the finishes table is empty here, left out and said.
  const auto dir = std::filesystem::temp_directory_path() / "sbr_drawing_tables";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "plan.dxf").u8string();
  const json r = ok("export_drawing", {{"path", path}, {"kind", "plan"}, {"tables", {"openings", "finishes"}}});
  EXPECT_EQ(r["tables"], 1);
  bool said = false;
  for (const json& n : r["notes"]) said |= n.get<std::string>().find("finishes") != std::string::npos;
  EXPECT_TRUE(said);
  EXPECT_GT(std::filesystem::file_size(path), 1000u);
  EXPECT_FALSE(call("export_drawing", {{"path", path}, {"kind", "plan"}, {"tables", {"walls"}}}).value("ok", true));
  std::filesystem::remove_all(dir);
}

// docs/115: a room's use, finishes and ceiling height.
TEST_F(Schedules, ARoomCarriesItsFinishesIntoTheFinishSchedule) {
  const auto steps = modeler.history().undoCount();
  json set = ok("set_room_finish", {{"id", room}, {"use", "居間"}, {"floor", "フローリング t15"}, {"ceiling_height", 2400}});
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  // Keys not given stay; an empty word takes one away.
  ok("set_room_finish", {{"id", room}, {"wall", "PB t12.5 クロス貼り"}, {"ceiling", "PB t9.5 クロス貼り"}});
  json rooms = ok("room_schedule", {{"include_csv", true}});
  const json& r = rooms["rooms"][0];
  EXPECT_EQ(r["use"], "居間");
  EXPECT_EQ(r["floor"], "フローリング t15");
  EXPECT_EQ(r["wall"], "PB t12.5 クロス貼り");
  EXPECT_NEAR(r["ceiling_height"].get<double>(), 2400, 1e-9);
  EXPECT_NE(rooms["csv"].get<std::string>().find("閉じている,居間,フローリング t15,PB t12.5 クロス貼り,PB t9.5 クロス貼り,2400"), std::string::npos)
      << rooms["csv"];
  const sbr::draw::Table finishes = sbr::doc::scheduleTable(modeler.document(), 3);
  EXPECT_EQ(finishes.title, "仕上げ表");
  ASSERT_EQ(finishes.rows.size(), 1u);
  EXPECT_EQ(finishes.rows[0][3], "フローリング t15");
  EXPECT_EQ(finishes.rows[0][6], "2400");
  EXPECT_NE(sbr::doc::tableCsv(finishes).find("居間,1FL,居間,フローリング t15"), std::string::npos);
  ok("set_room_finish", {{"id", room}, {"wall", ""}});
  EXPECT_EQ(ok("room_schedule")["rooms"][0]["wall"], "");
  // Refused: a ceiling too low, a word over two lines, not a room. Nothing changes.
  const auto before = modeler.history().undoCount();
  EXPECT_FALSE(call("set_room_finish", {{"id", room}, {"ceiling_height", 500}}).value("ok", true));
  EXPECT_FALSE(call("set_room_finish", {{"id", room}, {"floor", "a\nb"}}).value("ok", true));
  EXPECT_FALSE(call("set_room_finish", {{"id", south}, {"floor", "x"}}).value("ok", true));
  EXPECT_EQ(modeler.history().undoCount(), before);
  // The sheet and the file.
  const auto dir = std::filesystem::temp_directory_path() / "sbr_finish_sheet";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  EXPECT_EQ(ok("export_schedule", {{"path", (dir / "f.dxf").u8string()}, {"table", "finishes"}})["paper"], "A4");
  std::filesystem::remove_all(dir);
  // All taken away: the attribute goes.
  ok("set_room_finish", {{"id", room}, {"use", ""}, {"floor", ""}, {"ceiling", ""}, {"ceiling_height", 0}});
  EXPECT_FALSE(modeler.document().find(room)->attributes.count("room_finish"));
}

// docs/118: an opening where another wall ends is said, not stopped.
TEST_F(Schedules, AnOpeningWhereAnotherWallEndsIsSaid) {
  // The partition (x = 4000, 100 thick) ends on the south and north walls. The south windows are
  // clear of it; a door in the middle of the north wall is not.
  EXPECT_TRUE(modeler.wallsMeetingOpenings(south).empty());
  const json placed = ok("place_openings", {{"host", north}, {"face_normal", {0, 1, 0}}, {"at", {4000, 6000, 0}}, {"width", 900}, {"height", 2000}, {"sill", 0}});
  ASSERT_TRUE(placed.contains("walls_meeting_openings")) << placed.dump();
  EXPECT_EQ(placed["walls_meeting_openings"].size(), 1u);
  EXPECT_EQ(placed["walls_meeting_openings"][0]["wall"], modeler.document().find(partition)->name);
  // The opening was made all the same.
  EXPECT_EQ(ok("list_openings", {{"ids", {north}}})["hosts"][0]["openings"].size(), 3u);
  // Where no wall ends (the middle of the west wall): nothing said.
  ASSERT_TRUE(modeler.undo());
  const json clear = ok("place_openings", {{"host", west}, {"face_normal", {-1, 0, 0}}, {"at", {0, 3000, 0}}, {"width", 900}, {"height", 2000}, {"sill", 0}});
  EXPECT_FALSE(clear.contains("walls_meeting_openings")) << clear.dump();
  EXPECT_TRUE(modeler.wallsMeetingOpenings(west).empty());
}

// docs/121: a storey copied to the level above.
TEST_F(Schedules, AStoreyIsCopiedToTheLevelAbove) {
  // 1FL: 5 walls with openings, the floor slab (top 0), the room. Add a 3FL so 2FL's walls get a top level.
  // (Keeping the levels' ids: the walls stay on 1FL.)
  auto levels = modeler.levels();
  for (auto& l : levels)
    if (l.name == "2FL") l.elevation = 3000;
  levels.push_back({"", "3FL", 6000});
  ASSERT_TRUE(modeler.setLevels(levels));
  const auto steps = modeler.history().undoCount();
  const json copied = ok("copy_storey", {{"from", "1FL"}, {"to", "2FL"}});
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  EXPECT_EQ(copied["walls"], 5);
  EXPECT_EQ(copied["slabs"], 1);
  EXPECT_EQ(copied["rooms"], 1);
  // The copies stand on 2FL: the walls' level follows, the openings and their marks came along.
  std::string upperLevel;
  for (const auto& l : modeler.levels())
    if (l.name == "2FL") upperLevel = l.id;
  int onUpper = 0;
  for (const json& id : copied["ids"]) {
    const auto* o = modeler.document().find(id.get<std::string>());
    ASSERT_TRUE(o);
    if (o->attributes.count("base_level") && o->attributes.at("base_level") == upperLevel) ++onUpper;
  }
  EXPECT_EQ(onUpper, 5);
  EXPECT_EQ(ok("opening_schedule")["count"], 10);
  const json areas = ok("area_table");
  ASSERT_EQ(areas["levels"].size(), 2u) << areas.dump();
  EXPECT_NEAR(areas["levels"][1]["enclosed"].get<double>(), areas["levels"][0]["enclosed"].get<double>(), 1e-6);
  EXPECT_EQ(areas["levels"][1]["rooms"], 1);
  // Again: refused (walls stand there), nothing changes.
  EXPECT_FALSE(call("copy_storey", {{"from", "1FL"}, {"to", "2FL"}}).value("ok", true));
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  ASSERT_TRUE(modeler.undo());
  EXPECT_EQ(ok("opening_schedule")["count"], 5);
  // A slab already on 2FL: the walls go up, the slab stays behind.
  ok("create_slab", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"top", 3000}, {"thickness", 200}});
  const json again = ok("copy_storey", {{"from", "1FL"}, {"to", "2FL"}});
  EXPECT_EQ(again["slabs"], 0);
  EXPECT_TRUE(again["slabs_left"].get<bool>());
  EXPECT_EQ(again["walls"], 5);
  // Nothing to copy from an empty storey.
  EXPECT_FALSE(call("copy_storey", {{"from", "3FL"}, {"to", "1FL"}}).value("ok", true));
}

// docs/122: members drawn before pinned to a grid afterwards, and following it.
TEST(GridPinLater, MembersDrawnBeforeFollowTheGridOncePinned) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto call = [&](const std::string& command, json args) {
    return json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
  };
  auto ok = [&](const std::string& command, json args) {
    json r = call(command, args);
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  const json section = {{"kind", "rectangle"}, {"width", 105}, {"depth", 105}};
  const std::string column = ok("create_member", {{"start", {3640, 0, 0}}, {"end", {3640, 0, 2800}}, {"role", "column"}, {"section", section}})["id"];
  const std::string beam = ok("create_member", {{"start", {0, 0, 2800}}, {"end", {3640, 0, 2800}}, {"role", "beam"},
                                                 {"section", {{"kind", "rectangle"}, {"width", 105}, {"depth", 240}}}})["id"];
  const std::string loose = ok("create_member", {{"start", {1000, 1000, 0}}, {"end", {1000, 1000, 2800}}, {"role", "column"}, {"section", section}})["id"];
  EXPECT_FALSE(call("pin_to_grid", json::object()).value("ok", true));  // no grid yet
  const json grid = ok("create_grid", {{"x_spacings", {3640}}, {"y_spacings", {3640}}});
  const auto steps = modeler.history().undoCount();
  const json pinned = ok("pin_to_grid", json::object());
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  EXPECT_EQ(pinned["pinned"], 2);
  ASSERT_EQ(pinned["left"].size(), 1u);
  EXPECT_TRUE(modeler.document().find(column)->gridPin.has_value());
  EXPECT_TRUE(modeler.document().find(beam)->gridPin.has_value());
  EXPECT_FALSE(modeler.document().find(loose)->gridPin.has_value());
  // The second x line moved out by 910: the column goes with it, the beam gets longer.
  const auto spec = modeler.gridSpec(grid["grid"].get<std::string>());
  ASSERT_TRUE(spec);
  json x = json::array(), y = json::array();
  for (const auto& a : spec->x) x.push_back({{"id", a.id}, {"name", a.name}, {"position", a.offset + (a.offset > 1 ? 910.0 : 0.0)}});
  for (const auto& a : spec->y) y.push_back({{"id", a.id}, {"name", a.name}, {"position", a.offset}});
  ok("set_grid", {{"grid", grid["grid"]}, {"x", x}, {"y", y}});
  const auto box = [&](const std::string& id) { return sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(id))); };
  EXPECT_NEAR((box(column).CornerMin().X() + box(column).CornerMax().X()) / 2, 3640 + 910, 1e-6);
  EXPECT_NEAR(box(beam).CornerMax().X() - box(beam).CornerMin().X(), 3640 + 910, 1e-6);  // from crossing to crossing
  EXPECT_NEAR((box(loose).CornerMin().X() + box(loose).CornerMax().X()) / 2, 1000, 1e-6);  // not pinned: stays
  // docs/144: every one let go at once.
  EXPECT_EQ(ok("unpin_from_grid", json::object())["unpinned"], 2);
  EXPECT_FALSE(modeler.document().find(column)->gridPin.has_value());
  EXPECT_FALSE(call("unpin_from_grid", json::object()).value("ok", true));  // none left
}

// docs/129: walls pinned to a grid follow its spacings; the walls joined to them, their openings,
// the slab and the rooms come along.
TEST(GridPinLater, WallsOnTheGridFollowItsSpacings) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto call = [&](const std::string& command, json args) {
    return json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
  };
  auto ok = [&](const std::string& command, json args) {
    json r = call(command, args);
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  ASSERT_TRUE(modeler.setLevels({{"", "1FL", 0}, {"", "2FL", 3000}}));
  const json grid = ok("create_grid", {{"x_spacings", {3640, 3640}}, {"y_spacings", {3640}}});
  const json walls = ok("create_wall", {{"points", {{0, 0}, {7280, 0}, {7280, 3640}, {0, 3640}, {0, 0}}},
                                        {"thickness", 120}, {"height", 3000}})["ids"];
  const std::string south = walls[0], east = walls[1], north = walls[2];
  const std::string partition = ok("create_wall", {{"points", {{3640, 0}, {3640, 3640}}}, {"thickness", 120}, {"height", 3000}})["ids"][0];
  const std::string loose = ok("create_wall", {{"points", {{1000, 1000}, {2000, 1000}}}, {"thickness", 120}, {"height", 3000}})["ids"][0];
  ok("place_openings", {{"host", east}, {"face_normal", {1, 0, 0}}, {"count", 1}, {"width", 900}, {"height", 1200}, {"sill", 900}});
  ok("place_openings", {{"host", south}, {"face_normal", {0, -1, 0}}, {"count", 1}, {"width", 900}, {"height", 1200}, {"sill", 900}});
  ok("create_slab", {{"outline", {{-60, -60}, {7340, -60}, {7340, 3700}, {-60, 3700}}}, {"top", 0}, {"thickness", 150}});
  const std::string room = ok("create_room", {{"at", {5460, 1820}}, {"name", "east"}})["id"];
  auto area = [&] {
    const json rooms = ok("list_rooms", json::object());
    for (const json& r : rooms["rooms"])
      if (r["id"] == room) return r["area_inner"].get<double>();
    return 0.0;
  };
  const double area0 = area();
  const auto box = [&](const std::string& id) { return sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(id))); };
  const double southLength = box(south).CornerMax().X() - box(south).CornerMin().X();
  const double southWindow = modeler.openings(south)->front().center.X();
  const double eastWindow = modeler.openings(east)->front().center.Y();

  const auto steps = modeler.history().undoCount();
  const json pinned = ok("pin_to_grid", json::object());
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  EXPECT_EQ(pinned["walls"], 5);  // the four of the outline and the partition
  ASSERT_EQ(pinned["left"].size(), 1u);  // the loose wall: not between crossings
  // X3 out by 910.
  ASSERT_TRUE(modeler.gridSpec(grid["grid"].get<std::string>()));
  const sbr::doc::GridSpec original = *modeler.gridSpec(grid["grid"].get<std::string>());
  const auto* spec = &original;
  auto setGrid = [&](double moveX2, double moveX3) {
    json x = json::array(), y = json::array();
    for (const auto& a : spec->x)
      x.push_back({{"id", a.id}, {"name", a.name}, {"position", a.offset + (a.offset > 7000 ? moveX3 : a.offset > 1 ? moveX2 : 0.0)}});
    for (const auto& a : spec->y) y.push_back({{"id", a.id}, {"name", a.name}, {"position", a.offset}});
    ok("set_grid", {{"grid", grid["grid"]}, {"x", x}, {"y", y}});
  };
  setGrid(0, 910);
  EXPECT_EQ(modeler.history().undoCount(), steps + 2);
  EXPECT_NEAR((box(east).CornerMin().X() + box(east).CornerMax().X()) / 2, 7280 + 910, 1e-6);  // sideways
  EXPECT_NEAR(box(south).CornerMax().X() - box(south).CornerMin().X(), southLength + 910, 1e-6);  // longer
  EXPECT_NEAR(box(north).CornerMax().X() - box(north).CornerMin().X(), southLength + 910, 1e-6);
  EXPECT_NEAR(modeler.openings(east)->front().center.X(), 7280 + 910 + 60, 1.0);  // its window with it
  EXPECT_NEAR(modeler.openings(east)->front().center.Y(), eastWindow, 1e-6);
  EXPECT_NEAR(modeler.openings(south)->front().center.X(), southWindow, 1e-6);   // the south window stays
  EXPECT_NEAR((box(partition).CornerMin().X() + box(partition).CornerMax().X()) / 2, 3640, 1e-6);
  EXPECT_NEAR(box(loose).CornerMin().X(), 1000, 1e-6);
  // The slab's corner in the east wall went with it; the room is larger by 910 x its inner depth.
  for (const sbr::doc::Object& o : modeler.document().objects())
    if (o.attributes.count("kind") && o.attributes.at("kind") == "slab") EXPECT_NEAR(box(o.id).CornerMax().X(), 7340 + 910, 1e-6);
  EXPECT_NEAR(area(), area0 + 910.0 * (3640 - 120) / 1e6, 1e-6);
  // X2 (the partition's line) by 300: the partition goes, the south and north walls stay.
  setGrid(300, 910);
  EXPECT_NEAR((box(partition).CornerMin().X() + box(partition).CornerMax().X()) / 2, 3640 + 300, 1e-6);
  EXPECT_NEAR(box(south).CornerMax().X() - box(south).CornerMin().X(), southLength + 910, 1e-6);
  // Undone: as before, one step each.
  ASSERT_TRUE(modeler.undo());
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR((box(east).CornerMin().X() + box(east).CornerMax().X()) / 2, 7280, 1e-6);
  EXPECT_NEAR(area(), area0, 1e-6);
  // docs/144: the east wall let go: it no longer follows; the others still do. One undo step.
  {
    const auto before = modeler.history().undoCount();
    const json freed = ok("unpin_from_grid", {{"ids", {east}}});
    EXPECT_EQ(freed["unpinned"], 1);
    EXPECT_EQ(modeler.history().undoCount(), before + 1);
    EXPECT_FALSE(modeler.document().find(east)->attributes.count("wall_grid_pin"));
    setGrid(0, 455);
    EXPECT_NEAR((box(east).CornerMin().X() + box(east).CornerMax().X()) / 2, 7280, 1e-6);  // stayed
    ASSERT_TRUE(modeler.undo());  // the grid
    ASSERT_TRUE(modeler.undo());  // the letting go
    EXPECT_TRUE(modeler.document().find(east)->attributes.count("wall_grid_pin"));
    EXPECT_FALSE(call("unpin_from_grid", {{"ids", {loose}}}).value("ok", true));  // never pinned
  }
  // Kept in the file.
  const auto file = std::filesystem::temp_directory_path() / "sbr_wall_pin.sbr";
  ASSERT_TRUE(modeler.save(file.string()));
  sbr::doc::Modeler other;
  ASSERT_TRUE(other.load(file.string()));
  EXPECT_TRUE(other.document().find(east)->attributes.count("wall_grid_pin"));
}

// docs/123: a room's ceiling as a board at its ceiling height.
TEST_F(Schedules, ACeilingBoardIsMadeAtTheCeilingHeight) {
  EXPECT_FALSE(call("make_ceilings", json::object()).value("ok", true));  // no ceiling height yet
  ok("set_room_finish", {{"id", room}, {"ceiling_height", 2400}});
  const auto steps = modeler.history().undoCount();
  json made = ok("make_ceilings", json::object());
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  ASSERT_EQ(made["ids"].size(), 1u);
  const std::string board = made["ids"][0];
  const auto* o = modeler.document().find(board);
  ASSERT_TRUE(o);
  EXPECT_EQ(o->attributes.at("kind"), "ceiling");
  EXPECT_EQ(o->attributes.at("room"), room);
  const auto b = sbr::geom::exactBounds(modeler.document().worldShape(*o));
  EXPECT_NEAR(b.CornerMin().Z(), 2400, 1e-6);
  EXPECT_NEAR(b.CornerMax().Z(), 2410, 1e-6);
  // Within the wall faces: the inner area of the room (3.85 x 5.8 m).
  const json r = ok("room_schedule", json::object())["rooms"][0];
  EXPECT_NEAR(sbr::geom::volume(o->shape) / 10.0 / 1e6, r["area_inner"].get<double>(), 1e-6);
  // Made again at a new height: still one board (since docs/142 the board follows the new
  // height by itself; made again by hand it stays one).
  ok("set_room_finish", {{"id", room}, {"ceiling_height", 2600}});
  made = ok("make_ceilings", {{"rooms", {room}}});
  int boards = 0;
  for (const auto& obj : modeler.document().objects())
    if (obj.attributes.count("kind") && obj.attributes.at("kind") == "ceiling") ++boards;
  EXPECT_EQ(boards, 1);
  const auto* again = modeler.document().find(made["ids"][0].get<std::string>());
  EXPECT_NEAR(sbr::geom::exactBounds(modeler.document().worldShape(*again)).CornerMin().Z(), 2600, 1e-6);
  ASSERT_TRUE(modeler.undo());  // making it again
  ASSERT_TRUE(modeler.undo());  // the new height (and the board that followed it)
  EXPECT_TRUE(modeler.document().find(board));  // the first board is back
}

// docs/142: a ceiling board still as made follows its room's walls and ceiling height, in the
// same undo step; one changed by hand stays.
TEST_F(Schedules, ACeilingAsMadeFollowsItsRoom) {
  ok("set_room_finish", {{"id", room}, {"ceiling_height", 2400}});
  ok("make_ceilings", {{"rooms", {room}}});
  auto board = [&]() -> const sbr::doc::Object* {
    for (const auto& o : modeler.document().objects())
      if (o.attributes.count("kind") && o.attributes.at("kind") == "ceiling") return &o;
    return nullptr;
  };
  auto area = [&] { return sbr::geom::volume(modeler.document().worldShape(*board())) / 10.0 / 1e6; };
  auto inner = [&] {
    const json rooms = ok("room_schedule", json::object());
    return rooms["rooms"][0]["area_inner"].get<double>();
  };
  ASSERT_TRUE(board());
  EXPECT_NEAR(area(), inner(), 1e-6);
  const double before = area();
  // The partition (x = 4000) 500 east: the room is larger; the board with it; one undo step.
  const auto steps = modeler.history().undoCount();
  ASSERT_TRUE(modeler.move({partition}, gp_Vec(500, 0, 0)));
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  ASSERT_TRUE(board());
  EXPECT_NEAR(area(), inner(), 1e-6);
  EXPECT_GT(area(), before + 2.5);  // 0.5 m x about 5.8 m more
  int boards = 0;
  for (const auto& o : modeler.document().objects()) boards += o.attributes.count("kind") && o.attributes.at("kind") == "ceiling";
  EXPECT_EQ(boards, 1);
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(area(), before, 1e-6);  // the wall and the board as they were, in one go
  // The ceiling height: the board goes to it.
  ok("set_room_finish", {{"id", room}, {"ceiling_height", 2550}});
  EXPECT_NEAR(sbr::geom::exactBounds(modeler.document().worldShape(*board())).CornerMin().Z(), 2550, 1e-6);
  // Moved by hand (a lowered ceiling): it stays when the walls move.
  ASSERT_TRUE(modeler.move({board()->id}, gp_Vec(0, 0, -150)));
  const std::string kept = board()->id;
  ASSERT_TRUE(modeler.move({partition}, gp_Vec(500, 0, 0)));
  ASSERT_TRUE(board());
  EXPECT_EQ(board()->id, kept);
  EXPECT_NEAR(area(), before, 1e-6);
  EXPECT_NEAR(sbr::geom::exactBounds(modeler.document().worldShape(*board())).CornerMin().Z(), 2400, 1e-6);
}

// docs/186 (found bringing back from SketchUp): a storey with its ceiling copied up, then the
// walls of both storeys moved: each storey's ceiling follows its own room, none goes missing.
TEST_F(Schedules, CeilingsOfACopiedStoreyFollowTheirOwnRooms) {
  auto levels = modeler.levels();
  for (auto& l : levels)
    if (l.name == "2FL") l.elevation = 3000;
  levels.push_back({"", "3FL", 6000});
  ASSERT_TRUE(modeler.setLevels(levels));
  ok("set_room_finish", {{"id", room}, {"ceiling_height", 2400}});
  ok("make_ceilings", {{"rooms", {room}}});
  ok("copy_storey", {{"from", "1FL"}, {"to", "2FL"}});
  auto boards = [&] {
    std::vector<const sbr::doc::Object*> out;
    for (const auto& o : modeler.document().objects())
      if (o.attributes.count("kind") && o.attributes.at("kind") == "ceiling") out.push_back(&o);
    std::sort(out.begin(), out.end(), [&](auto* a, auto* b) {
      return modeler.document().worldBounds(*a).CornerMin().Z() < modeler.document().worldBounds(*b).CornerMin().Z();
    });
    return out;
  };
  ASSERT_EQ(boards().size(), 2u);
  // The upper ceiling names the upper room (not the one it was copied from).
  EXPECT_NE(boards()[1]->attributes.at("room"), room);
  const double lower = sbr::geom::volume(modeler.document().worldShape(*boards()[0]));
  // Both partitions 500 east (as one move): both rooms larger, both ceilings with them.
  std::vector<sbr::doc::ObjectId> partitions;
  for (const auto& o : modeler.document().objects()) {
    const auto b = modeler.document().worldBounds(o);
    if (o.attributes.count("kind") && o.attributes.at("kind") == "wall" && !b.IsVoid() && std::abs(b.CornerMin().X() - 3950) < 1e-6)
      partitions.push_back(o.id);
  }
  ASSERT_EQ(partitions.size(), 2u);
  ASSERT_TRUE(modeler.move(partitions, gp_Vec(500, 0, 0)));
  const auto now = boards();
  ASSERT_EQ(now.size(), 2u);
  EXPECT_NEAR(modeler.document().worldBounds(*now[0]).CornerMin().Z(), 2400, 1e-6);
  EXPECT_NEAR(modeler.document().worldBounds(*now[1]).CornerMin().Z(), 5400, 1e-6);
  for (const auto* b : now) EXPECT_GT(sbr::geom::volume(modeler.document().worldShape(*b)), lower + 2.5e6 * 10);
}

// docs/124: the lines between the layers of a wall of a build-up, in a plan cut.
TEST(WallLayerLines, ALineBetweenEachTwoLayersBrokenAtOpenings) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  // Exterior siding: 5 layers (siding 16, batten, plywood, core 105, board) -> 4 lines.
  const std::string wall = ok("create_wall", {{"points", {{0, 0}, {6000, 0}}}, {"type", "exterior_siding"}, {"height", 3000}})["ids"][0];
  auto lines = sbr::doc::Modeler::wallLayerLines(modeler.document(), 1100);
  ASSERT_EQ(lines.size(), 4u);
  for (const auto& [a, b] : lines) {
    EXPECT_NEAR(a.Distance(b), 6000, 1e-6);  // the wall's whole length
    EXPECT_NEAR(a.Z(), 1100, 1e-9);
  }
  // The first line is 16 in from the outer face (the siding).
  const auto box = sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(wall)));
  std::vector<double> ys;
  for (const auto& l : lines) ys.push_back(l.first.Y());
  std::sort(ys.begin(), ys.end());
  const bool outerLow = std::abs(ys.front() - box.CornerMin().Y() - 16) < 1e-6;
  const bool outerHigh = std::abs(box.CornerMax().Y() - ys.back() - 16) < 1e-6;
  EXPECT_TRUE(outerLow || outerHigh) << box.CornerMin().Y() << " " << ys.front() << " " << ys.back() << " " << box.CornerMax().Y();
  // An opening cut by the plan breaks each line in two; above it, the lines are whole.
  ok("place_openings", {{"host", wall}, {"face_normal", {0, -1, 0}}, {"count", 1}, {"width", 1600}, {"height", 1200}, {"sill", 900}});
  lines = sbr::doc::Modeler::wallLayerLines(modeler.document(), 1100);
  EXPECT_EQ(lines.size(), 8u);
  EXPECT_EQ(sbr::doc::Modeler::wallLayerLines(modeler.document(), 2500).size(), 4u);
  // A plain wall (no build-up): no lines.
  ok("create_wall", {{"points", {{0, 3000}, {6000, 3000}}}, {"thickness", 200}, {"height", 3000}});
  EXPECT_EQ(sbr::doc::Modeler::wallLayerLines(modeler.document(), 2500).size(), 4u);
  // The plan drawing carries them: 4 more lines than without the build-up's.
  const auto view = sbr::draw::makeView(modeler.document(), sbr::draw::planView(2500));
  ASSERT_TRUE(view);
  int along = 0;
  for (const auto& item : view->items)
    if (item.kind == sbr::draw::Item::Kind::Line && item.points.size() == 2 &&
        std::abs(item.points[0].Distance(item.points[1]) - 6000) < 1e-3 && item.pen == sbr::draw::Pen::Visible)
      ++along;
  EXPECT_GE(along, 4);
}

// docs/156: in a section, upright lines where a wall's layers meet, broken at its openings.
TEST(WallLayerLines, UprightLinesInSections) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  const std::string wall = ok("create_wall", {{"points", {{0, 0}, {6000, 0}}}, {"type", "exterior_siding"}, {"height", 3000}})["ids"][0];
  ok("place_openings", {{"host", wall}, {"face_normal", {0, -1, 0}}, {"count", 1}, {"width", 1600}, {"height", 1200}, {"sill", 900}});
  EXPECT_EQ(sbr::doc::Modeler::wallLayerPlanes(modeler.document(), *modeler.document().find(wall)).size(), 4u);
  // Upright thin lines of one length in a section across the wall at x.
  auto uprights = [&](double x, double length) {
    const auto view = sbr::draw::makeView(modeler.document(), sbr::draw::sectionView(gp_Pnt(x, 0, 0), gp_Dir(-1, 0, 0)));
    int count = 0;
    if (!view) return -1;
    for (const auto& item : view->items)
      if (item.kind == sbr::draw::Item::Kind::Line && item.pen == sbr::draw::Pen::Visible && item.points.size() == 2 &&
          std::abs(item.points[0].X() - item.points[1].X()) < 1e-6 && std::abs(item.points[0].Distance(item.points[1]) - length) < 1e-3)
        ++count;
    return count;
  };
  EXPECT_EQ(uprights(1000, 3000), 4);  // away from the window: whole height
  EXPECT_EQ(uprights(3000, 900), 8);   // through it: under the sill and over the head (900 each)
  EXPECT_EQ(uprights(3000, 3000), 0);
  // A plain wall: none.
  const std::string plain = ok("create_wall", {{"points", {{0, 3000}, {6000, 3000}}}, {"thickness", 200}, {"height", 3000}})["ids"][0];
  EXPECT_TRUE(sbr::doc::Modeler::wallLayerPlanes(modeler.document(), *modeler.document().find(plain)).empty());
  EXPECT_EQ(uprights(1000, 3000), 4);
}

// docs/140: at the corners of walls of one build-up the layer lines meet as a mitre.
TEST(WallLayerLines, LayerLinesMeetAsAMitreAtCorners) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  ok("create_wall", {{"points", {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}, {0, 0}}}, {"type", "exterior_siding"}, {"height", 3000}});
  const auto lines = sbr::doc::Modeler::wallLayerLines(modeler.document(), 1100);
  ASSERT_EQ(lines.size(), 16u);  // 4 walls, 4 lines each
  // Every end of every line is the end of exactly one other line (its partner round the corner),
  // and the two meet on the corner's diagonal.
  int paired = 0;
  for (std::size_t i = 0; i < lines.size(); ++i)
    for (const gp_Pnt& p : {lines[i].first, lines[i].second}) {
      int same = 0;
      for (std::size_t j = 0; j < lines.size(); ++j)
        if (j != i && (lines[j].first.Distance(p) < 1e-6 || lines[j].second.Distance(p) < 1e-6)) ++same;
      EXPECT_EQ(same, 1) << p.X() << " " << p.Y();
      paired += same;
      // On a diagonal through a corner of the walls' lines (0/6000, 0/4000): |dx| = |dy|.
      const double cx = p.X() < 3000 ? 0 : 6000, cy = p.Y() < 2000 ? 0 : 4000;
      EXPECT_NEAR(std::abs(p.X() - cx), std::abs(p.Y() - cy), 1e-6) << p.X() << " " << p.Y();
    }
  EXPECT_EQ(paired, 32);
  // A partition meeting a wall (a T) keeps its lines to its own end; walls of other build-ups
  // are not mitred to these.
  ok("create_wall", {{"points", {{3000, 0}, {3000, 4000}}}, {"type", "interior_board"}, {"height", 3000}});
  const auto more = sbr::doc::Modeler::wallLayerLines(modeler.document(), 1100);
  EXPECT_GT(more.size(), lines.size());
}

// docs/165: in a plan, walls of one build-up that touch read as one: the line between them is
// left out (corners butted or mitred, a wall in two pieces in one line); walls of other
// build-ups keep theirs. Checked by the length of the cut lines.
TEST(WallLayerLines, NoSeamBetweenWallsOfOneBuildUpInPlans) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  auto cutLength = [&] {
    const auto view = sbr::draw::makeView(modeler.document(), sbr::draw::planView(1100));
    EXPECT_TRUE(view) << view.error();
    double total = 0.0;
    for (const auto& item : view->items)
      if (item.pen == sbr::draw::Pen::Cut && item.kind == sbr::draw::Item::Kind::Line) total += item.points[0].Distance(item.points[1]);
    return total;
  };
  // exterior_siding: 160.5 thick, its line 95.5 in from the outer face (65 out from the inner).
  const json room = ok("create_wall", {{"points", {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}, {0, 0}}}, {"type", "exterior_siding"}, {"height", 3000}});
  const double outer = 2 * (6191.0 + 4191.0), inner = 2 * (5870.0 + 3870.0);
  EXPECT_NEAR(cutLength(), outer + inner, 0.5);  // butted corners: no seam
  ok("mitre_wall_corners", json::object());
  EXPECT_NEAR(cutLength(), outer + inner, 0.5);  // mitred: no diagonal either
  // No thin line left where a seam was (the top of the cut walls seen from above).
  {
    const auto view = sbr::draw::makeView(modeler.document(), sbr::draw::planView(1100));
    ASSERT_TRUE(view);
    for (const auto& item : view->items) {
      if (item.kind != sbr::draw::Item::Kind::Line || item.pen == sbr::draw::Pen::Cut) continue;
      const gp_Pnt2d &a = item.points[0], &b = item.points[1];
      const bool across = std::abs(a.X() - b.X()) > 1e-6 && std::abs(a.Y() - b.Y()) > 1e-6;  // a diagonal
      EXPECT_FALSE(across) << a.X() << " " << a.Y() << " - " << b.X() << " " << b.Y();
    }
  }
  // A partition of another build-up to the inner face: its own outline, and the wall's inner
  // face stays whole across its end (two build-ups: two parts).
  ok("create_wall", {{"points", {{3000, 65}, {3000, 3935}}}, {"type", "interior_board"}, {"height", 3000}});
  EXPECT_NEAR(cutLength(), outer + inner + 2 * 3870.0 + 2 * 130.0, 0.5);
  // A wall in two pieces in one line, both of one build-up: one wall to read.
  sbr::doc::Modeler two;
  CommandApi api2{two};
  auto ok2 = [&](const std::string& command, json args) {
    json r = json::parse(api2.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  ok2("create_wall", {{"points", {{0, 0}, {3000, 0}}}, {"type", "interior_board"}, {"height", 3000}});
  ok2("create_wall", {{"points", {{3000, 0}, {6000, 0}}}, {"type", "interior_board"}, {"height", 3000}});
  {
    const auto view = sbr::draw::makeView(two.document(), sbr::draw::planView(1100));
    ASSERT_TRUE(view) << view.error();
    double total = 0.0;
    for (const auto& item : view->items)
      if (item.pen == sbr::draw::Pen::Cut && item.kind == sbr::draw::Item::Kind::Line) total += item.points[0].Distance(item.points[1]);
    EXPECT_NEAR(total, 2 * 6000.0 + 2 * 130.0, 0.5);  // the two long faces and the two far ends
  }
  (void)room;
}

// docs/173: the cut layers hatched in drawings at a scale: structural timber (the wall's core, a
// floor's joists) crossed, plywood single lines; finishes and boards not; nothing without a scale.
TEST(WallLayerLines, CutLayersAreHatchedAtAScale) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  ok("create_wall", {{"points", {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}, {0, 0}}}, {"type", "exterior_siding"}, {"height", 3000}});
  ok("define_type", {{"name", "F1"}, {"part", "floor"}, {"layers", {{{"name", "フローリング"}, {"thickness", 15}}, {{"name", "構造用合板"}, {"thickness", 24}}, {{"name", "根太"}, {"thickness", 105}}}}});
  ok("batch", {{"commands", {{{"command", "create_slab"}, {"args", {{"outline", {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}}}, {"top", 0}, {"thickness", 144}}}},
                             {{"command", "set_type_name"}, {"args", {{"ids", {"$1.id"}}, {"name", "F1"}}}}}}});
  // The diagonal thin lines of a view: (rising, falling) with their ends.
  struct Diag {
    gp_Pnt2d a, b;
    bool rising;
  };
  auto diagonals = [&](sbr::draw::ViewSpec spec, double scale) {
    spec.hatchScale = scale;
    const auto view = sbr::draw::makeView(modeler.document(), spec);
    EXPECT_TRUE(view) << view.error();
    std::vector<Diag> out;
    for (const auto& item : view->items) {
      if (item.kind != sbr::draw::Item::Kind::Line || item.pen != sbr::draw::Pen::Visible) continue;
      const double dx = item.points[1].X() - item.points[0].X(), dy = item.points[1].Y() - item.points[0].Y();
      if (std::abs(std::abs(dx) - std::abs(dy)) < 1e-6 && std::abs(dx) > 1e-6) out.push_back({item.points[0], item.points[1], dx * dy > 0});
    }
    return out;
  };
  // Plan 1:20: along the south wall (x 300..5700), the exterior is below y 0 (its line on the core's middle).
  const auto plan = diagonals(sbr::draw::planView(1100), 20);
  int core = 0, ply = 0;
  for (const Diag& d : plan) {
    if (std::min(d.a.X(), d.b.X()) < 300 || std::max(d.a.X(), d.b.X()) > 5700 || std::max(d.a.Y(), d.b.Y()) > 200) continue;
    const double y0 = std::min(d.a.Y(), d.b.Y()), y1 = std::max(d.a.Y(), d.b.Y());
    if (!d.rising) {
      // Falling lines are crossed hatching only: the core 105, y -52.5..52.5.
      EXPECT_GE(y0, -52.5 - 1e-6);
      EXPECT_LE(y1, 52.5 + 1e-6);
      ++core;
    } else if (y1 <= -52.5 + 1e-6) {
      ++ply;  // plywood, outside the core
    }
    // Nothing in the board (inside, 52.5..65) or the siding (outermost 16).
    EXPECT_LE(y1, 52.5 + 1e-6);
    EXPECT_GE(y0, -95.5 + 16 + 18 - 1e-6) << "the batten (18) and the siding (16) are not hatched";
  }
  EXPECT_GT(core, 20);
  EXPECT_GT(ply, 20);
  // 1:50: the same pattern, 2.5 times as far apart.
  const auto coarse = diagonals(sbr::draw::planView(1100), 50);
  EXPECT_LT(coarse.size() * 2, plan.size());
  // Without a scale (the screen): none.
  EXPECT_TRUE(diagonals(sbr::draw::planView(1100), 0).empty());
  // A section across the floor, looking east: the joists (z -144..-39) crossed, the plywood
  // (-39..-15) single lines, the flooring not.
  const auto section = diagonals(sbr::draw::sectionView(gp_Pnt(3000, 2000, 0), gp_Dir(1, 0, 0)), 20);
  int joists = 0, plywood = 0;
  for (const Diag& d : section) {
    const double z0 = std::min(d.a.Y(), d.b.Y()), z1 = std::max(d.a.Y(), d.b.Y());
    if (z1 > 1e-6 || z0 < -144 - 1e-6) continue;  // not in the floor
    if (!d.rising) {
      EXPECT_LE(z1, -39 + 1e-6);
      ++joists;
    } else if (z0 >= -39 - 1e-6) {
      EXPECT_LE(z1, -15 + 1e-6);
      ++plywood;
    }
  }
  EXPECT_GT(joists, 20);
  EXPECT_GT(plywood, 20);
}

// docs/125: a set of drawings with sheet numbers.
TEST_F(Schedules, ASetOfDrawingsCarriesSheetNumbers) {
  sbr::doc::DrawingSetSpec spec;
  spec.title.project = "Test";
  const auto set = sbr::doc::drawingSet(modeler.document(), spec);
  ASSERT_TRUE(set) << set.error();
  // The list, the 1FL plan (2FL has nothing: its plan is left out with a note), 4 elevations,
  // the door / room / area tables (no finishes given).
  ASSERT_GE(set->sheets.size(), 9u);
  EXPECT_EQ(set->numbers.front(), "A-01");
  EXPECT_EQ(set->names.front(), "図面リスト");
  for (std::size_t i = 0; i < set->numbers.size(); ++i) {
    char want[8];
    std::snprintf(want, sizeof want, "A-%02zu", i + 1);
    EXPECT_EQ(set->numbers[i], want);
  }
  auto has = [&](const std::string& name) { return std::find(set->names.begin(), set->names.end(), name) != set->names.end(); };
  EXPECT_TRUE(has("1FL 平面図"));
  EXPECT_TRUE(has("南立面図") && has("西立面図"));
  EXPECT_TRUE(has("建具表") && has("室の表") && has("面積の表"));
  EXPECT_FALSE(has("仕上げ表"));
  // Each sheet's title block carries its number; the list names every sheet.
  for (std::size_t i = 0; i < set->sheets.size(); ++i) {
    bool numbered = false;
    for (const auto& item : set->sheets[i].items) numbered |= item.kind == sbr::draw::Item::Kind::Text && item.text == set->numbers[i];
    EXPECT_TRUE(numbered) << set->numbers[i];
  }
  int listed = 0;
  for (const auto& item : set->sheets.front().items)
    for (const auto& n : set->numbers) listed += item.kind == sbr::draw::Item::Kind::Text && item.text == n ? 1 : 0;
  EXPECT_GE(listed, static_cast<int>(set->numbers.size()));
  // As files: one per sheet; written again they are refused (nothing replaced).
  const auto dir = std::filesystem::temp_directory_path() / "sbr_drawing_set";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const json files = ok("export_drawing_set", {{"folder", dir.u8string()}, {"format", "dxf"}});
  EXPECT_EQ(files["sheets"].size(), set->sheets.size());
  for (const json& f : files["sheets"]) EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(f["path"].get<std::string>())));
  EXPECT_FALSE(call("export_drawing_set", {{"folder", dir.u8string()}}).value("ok", true));
  std::filesystem::remove_all(dir);
}

// docs/131: the set with its tables beside the 1FL plan instead of sheets of their own.
TEST_F(Schedules, ASetCanCarryItsTablesOnThePlan) {
  sbr::doc::DrawingSetSpec spec;
  const auto apart = sbr::doc::drawingSet(modeler.document(), spec);
  ASSERT_TRUE(apart);
  spec.tablesWithPlan = true;
  const auto set = sbr::doc::drawingSet(modeler.document(), spec);
  ASSERT_TRUE(set) << set.error();
  // Three table sheets fewer (openings, rooms, areas).
  EXPECT_EQ(set->sheets.size() + 3, apart->sheets.size());
  const std::string name = "1FL 平面図・建具表・室の表・面積の表";
  ASSERT_EQ(set->names[1], name);
  for (const std::string& n : set->names) EXPECT_EQ(n.find("表") == 0, false) << n;  // no sheet of a table alone
  // On that sheet: each table's title once, the drawing's own caption, the sheet's name and number in the title block.
  std::map<std::string, int> texts;
  for (const auto& item : set->sheets[1].items)
    if (item.kind == sbr::draw::Item::Kind::Text) ++texts[item.text];
  EXPECT_EQ(texts["建具表"], 1);
  EXPECT_EQ(texts["室の表"], 1);
  EXPECT_EQ(texts["面積の表"], 1);
  EXPECT_EQ(texts[name], 1);
  EXPECT_EQ(texts["A-02"], 1);
  bool caption = false;
  for (const auto& [text, n] : texts) caption |= text.rfind("1FL 平面図  S=1:", 0) == 0;
  EXPECT_TRUE(caption);
  // The list: the plan sheet with its scale, the paper big enough for it.
  bool listed = false;
  for (const auto& item : set->sheets.front().items) listed |= item.kind == sbr::draw::Item::Kind::Text && item.text == name;
  EXPECT_TRUE(listed);
  EXPECT_NE(set->sheets[1].paper, "A4");
  // By the API.
  const auto dir = std::filesystem::temp_directory_path() / "sbr_drawing_set_tables";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const json files = ok("export_drawing_set", {{"folder", dir.u8string()}, {"tables_with_plan", true}});
  EXPECT_EQ(files["sheets"].size(), set->sheets.size());
  std::filesystem::remove_all(dir);
}

// docs/132: sections in the set, and where they are cut on the plans.
TEST_F(Schedules, TheSetHasSectionsMarkedOnThePlans) {
  sbr::doc::DrawingSetSpec spec;
  auto set = sbr::doc::drawingSet(modeler.document(), spec);
  ASSERT_TRUE(set) << set.error();
  auto index = [&](const std::string& name) {
    const auto it = std::find(set->names.begin(), set->names.end(), name);
    return it == set->names.end() ? -1 : static_cast<int>(it - set->names.begin());
  };
  // Through the middle: A-A and B-B, after the plans, before the elevations.
  const int a = index("A-A 断面図"), b = index("B-B 断面図"), plan = index("1FL 平面図"), south = index("南立面図");
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  EXPECT_TRUE(plan < a && a < b && b < south);
  // The sections show the walls they cut (filled), each its number in its title block.
  for (int k : {a, b}) {
    int fills = 0;
    bool numbered = false;
    for (const auto& item : set->sheets[static_cast<std::size_t>(k)].items) {
      fills += item.kind == sbr::draw::Item::Kind::Fill;
      numbered |= item.kind == sbr::draw::Item::Kind::Text && item.text == set->numbers[static_cast<std::size_t>(k)];
    }
    EXPECT_GT(fills, 0) << set->names[static_cast<std::size_t>(k)];
    EXPECT_TRUE(numbered);
  }
  // docs/145: each section's caption names the plan sheet it is marked on.
  for (int k : {a, b}) {
    bool named = false;
    for (const auto& item : set->sheets[static_cast<std::size_t>(k)].items)
      named |= item.kind == sbr::draw::Item::Kind::Text && item.text.find("切る位置：" + set->numbers[static_cast<std::size_t>(plan)]) != std::string::npos;
    EXPECT_TRUE(named) << set->names[static_cast<std::size_t>(k)];
  }
  // The plan: each cut's label twice (both ends) and the section's sheet number twice.
  std::map<std::string, int> texts;
  for (const auto& item : set->sheets[static_cast<std::size_t>(plan)].items)
    if (item.kind == sbr::draw::Item::Kind::Text) ++texts[item.text];
  EXPECT_EQ(texts["A"], 2);
  EXPECT_EQ(texts["B"], 2);
  EXPECT_EQ(texts[set->numbers[static_cast<std::size_t>(a)]], 2);
  EXPECT_EQ(texts[set->numbers[static_cast<std::size_t>(b)]], 2);
  // A section plane of the model: that one only.
  sbr::doc::SectionPlane plane;
  plane.name = "S1";
  plane.origin = {2000, 0, 0};
  plane.normal = {1, 0, 0};
  modeler.setSection(plane);
  set = sbr::doc::drawingSet(modeler.document(), spec);
  ASSERT_TRUE(set);
  EXPECT_GE(index("A-A 断面図"), 0);
  EXPECT_LT(index("B-B 断面図"), 0);
  // None asked for: none, and no marks.
  spec.sections = false;
  set = sbr::doc::drawingSet(modeler.document(), spec);
  ASSERT_TRUE(set);
  EXPECT_LT(index("A-A 断面図"), 0);
  // By the API, without sections: the same sheets as above.
  const auto dir = std::filesystem::temp_directory_path() / "sbr_drawing_set_sections";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const json files = ok("export_drawing_set", {{"folder", dir.u8string()}, {"sections", false}});
  EXPECT_EQ(files["sheets"].size(), set->sheets.size());
  std::filesystem::remove_all(dir);
}

// docs/125: drawings written under Japanese names (the app suggests the drawing's name).
TEST_F(Schedules, DrawingsAreWrittenUnderJapaneseNames) {
  const auto dir = std::filesystem::temp_directory_path() / std::filesystem::u8path(u8"図面テスト");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  for (const char* ext : {".dxf", ".jww"}) {
    const std::string path = (dir / std::filesystem::u8path(std::string(u8"1FL 平面図") + ext)).u8string();
    const json r = call("export_drawing", {{"path", path}, {"kind", "plan"}, {"cut_z", 1100}});
    EXPECT_TRUE(r.value("ok", false)) << ext << ": " << r.dump();
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(path))) << ext;
  }
  std::filesystem::remove_all(dir);
}

// docs/188 (from the owner's SketchUp extension): the way a door opens chosen by a point in plan,
// and 親子開き / 折れ戸 with their plan symbols and leaves.
TEST(DoorsInPlan, ChosenByAPointAndTheNewKinds) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  // A wall along x (0..4000), 200 thick, the door cut from its -y face (the face normal -y).
  const std::string wall = ok("create_wall", {{"points", {{0, 0}, {4000, 0}}}, {"thickness", 200}, {"height", 2700}})["ids"][0];
  ok("place_openings", {{"host", wall}, {"face_normal", {0, -1, 0}}, {"count", 1}, {"width", 900}, {"height", 2000}, {"sill", 0}});
  ok("set_opening_fittings", {{"host", wall}, {"openings", {1}}, {"fitting", {{"type", "swing_single"}}}});
  auto point = [&](double x, double y) { return ok("point_opening_fitting", {{"host", wall}, {"opening", 1}, {"point", {x, y}}})["fitting"]; };
  // The opening's middle at x 2000. -y is its face side ("out"); the hinge on the half pointed at.
  json f = point(2300, -600);
  EXPECT_EQ(f["swing"], "out");
  const std::string outHinge = f["hinge"];
  f = point(1700, -600);
  EXPECT_EQ(f["swing"], "out");
  EXPECT_NE(f["hinge"], outHinge);  // the other half: the other hinge
  f = point(2300, 600);
  EXPECT_EQ(f["swing"], "in");
  // The symbol drawn: its hinge (the leaf's line from it, square to the wall) on the half pointed at.
  auto hingeX = [&] {
    const auto edges = sbr::draw::openingSymbols(modeler.document(), {1000.0, nullptr});
    double x = 0;
    for (const auto& e : edges) {
      const Bnd_Box box = sbr::geom::exactBounds(e);
      if (std::abs(box.CornerMax().X() - box.CornerMin().X()) < 1e-6 && box.CornerMax().Y() - box.CornerMin().Y() > 800) x = box.CornerMin().X();
    }
    return x;
  };
  point(2300, 600);
  EXPECT_NEAR(hingeX(), 2450, 1e-6);  // the right jamb (900 wide about 2000), on the +y side
  point(1700, -600);
  EXPECT_NEAR(hingeX(), 1550, 1e-6);
  // 片引き: opens towards the side pointed at.
  ok("set_opening_fittings", {{"host", wall}, {"openings", {1}}, {"fitting", {{"type", "sliding_single"}}}});
  const std::string towards = point(2400, -600)["slide"];
  EXPECT_NE(point(1600, -600)["slide"], towards);
  // 引違い: nothing to choose.
  ok("set_opening_fittings", {{"host", wall}, {"openings", {1}}, {"fitting", {{"type", "sliding_double"}}}});
  EXPECT_FALSE(json::parse(api.execute(json{{"command", "point_opening_fitting"}, {"args", {{"host", wall}, {"opening", 1}, {"point", {2000, -600}}}}}.dump()))
                   .value("ok", true));
  // 親子開き: two arcs (parent 600, child 300), two leaves; 折れ戸: lines, four panels.
  auto symbols = [&] { return sbr::draw::openingSymbols(modeler.document(), {1000.0, nullptr}).size(); };
  auto leaves = [&] {
    int n = 0;
    for (const auto& s : sbr::draw::fittingSolids(modeler.document())) n += s.part == "leaf";
    return n;
  };
  ok("set_opening_fittings", {{"host", wall}, {"openings", {1}}, {"fitting", {{"type", "swing_unequal"}, {"child", 300}}}});
  EXPECT_EQ(symbols(), 4u);  // two leaves, two arcs
  EXPECT_EQ(leaves(), 2);
  f = point(2300, -600);
  EXPECT_EQ(f["type"], "swing_unequal");
  EXPECT_EQ(f["child"], 300.0);
  EXPECT_FALSE(json::parse(api.execute(json{{"command", "set_opening_fittings"}, {"args", {{"host", wall}, {"openings", {1}},
                                                {"fitting", {{"type", "swing_unequal"}, {"child", 100}}}}}}.dump()))
                   .value("ok", true));
  ok("set_opening_fittings", {{"host", wall}, {"openings", {1}}, {"fitting", {{"type", "folding"}}}});
  EXPECT_EQ(symbols(), 3u);  // the closed line and the zigzag
  EXPECT_EQ(leaves(), 4);
  EXPECT_EQ(point(2400, 600)["swing"], "in");
}

// docs/194: the permit workflow's stages 2 (通り芯) and 3 (平面) read from the model.
TEST_F(Schedules, PermitChecksReadGridAndPlanStages) {
  const auto rowOf = [&](int stage, const std::string& item) {
    const json rows = ok("permit_checks", {{"stage", stage}})["rows"];
    for (const json& row : rows)
      if (row["item"] == item) return row;
    ADD_FAILURE() << "no row " << item << " in " << rows.dump();
    return json();
  };
  const auto steps = modeler.history().undoCount();
  EXPECT_EQ(rowOf(2, "階（レベル）")["state"], "入力済み");
  EXPECT_EQ(rowOf(2, "通り芯")["state"], "未入力");
  EXPECT_EQ(rowOf(2, "壁と階")["state"], "入力済み");  // drawn at 1FL
  EXPECT_EQ(rowOf(2, "壁と通り芯")["state"], "通り芯待ち");
  EXPECT_FALSE(call("permit_checks", {{"stage", 4}}).value("ok", true));
  EXPECT_EQ(modeler.history().undoCount(), steps);  // reading only

  // A grid on the wall lines: the walls are not on it until pinned; a column off the lines stays off.
  ok("create_grid", {{"x_spacings", {4000, 4000}}, {"y_spacings", {6000}}});
  const std::string loose = ok("create_member", {{"start", {1000, 1000, 0}}, {"end", {1000, 1000, 2800}}, {"role", "column"},
                                                 {"section", {{"kind", "rectangle"}, {"width", 105}, {"depth", 105}}}})["id"];
  json walls = rowOf(2, "壁と通り芯");
  EXPECT_EQ(walls["state"], "要確認");
  EXPECT_EQ(walls["targets"].size(), 5u);
  EXPECT_TRUE(walls["attention"].get<bool>());
  ok("pin_to_grid", json::object());
  EXPECT_EQ(rowOf(2, "通り芯")["state"], "入力済み");
  EXPECT_EQ(rowOf(2, "壁と通り芯")["state"], "入力済み");
  const json members = rowOf(2, "柱・梁と通り芯");
  EXPECT_EQ(members["state"], "要確認");
  ASSERT_EQ(members["targets"].size(), 1u);
  EXPECT_EQ(members["targets"][0], loose);

  // Plan: the east half has no room; the north wall's second opening has no fitting.
  json storey = rowOf(3, "1FL の平面");
  EXPECT_EQ(storey["state"], "要確認");
  EXPECT_NE(storey["details"].get<std::string>().find("室を置いていない範囲 1"), std::string::npos) << storey.dump();
  const json openings = rowOf(3, "開口");
  EXPECT_EQ(openings["state"], "要確認");
  ASSERT_EQ(openings["targets"].size(), 1u);
  EXPECT_EQ(openings["targets"][0], north);
  ok("create_room", {{"at", {6000, 3000}}, {"name", "洋室"}});
  EXPECT_EQ(rowOf(3, "1FL の平面")["state"], "入力済み");
  // Without the partition both markers stand in one region: the second is to be put right.
  ok("delete", {{"ids", {partition}}});
  const json closing = rowOf(3, "室の閉じ方");
  EXPECT_EQ(closing["state"], "要修正");
  ASSERT_EQ(closing["targets"].size(), 1u);
}

// docs/197: the permit workflow's stage 5 (階段): each stair against the sizes for a house's own
// stair, the basis (read on e-Gov) and what is not compared; never a pass.
TEST_F(Schedules, PermitChecksReadTheStairStage) {
  const auto rows = [&] { return ok("permit_checks", {{"stage", 5}})["rows"]; };
  json none = rows();
  ASSERT_EQ(none.size(), 1u);
  EXPECT_EQ(none[0]["item"], "階段");
  EXPECT_EQ(none[0]["state"], "未入力");
  // A straight stair 800 wide, risers within 200 (the default limit), with handrails.
  const std::string stair = ok("create_stair", {{"path", {{7000, 500}, {7000, 4000}}}, {"rise", 3000}, {"width", 800},
                                                {"handrail", "both"}})["id"];
  json all = rows();
  ASSERT_GE(all.size(), 3u);
  EXPECT_EQ(all[0]["targets"][0], stair);
  EXPECT_EQ(all[0]["state"], "参考値内") << all[0].dump();
  const std::string details = all[0]["details"];
  EXPECT_NE(details.find("蹴上げ"), std::string::npos);
  EXPECT_NE(details.find("（230 以下）　参考値内"), std::string::npos) << details;
  EXPECT_EQ(all[1]["item"], "照らした値");
  EXPECT_NE(all[1]["details"].get<std::string>().find("e-Gov"), std::string::npos);
  EXPECT_EQ(all[2]["state"], "未評価");
  // Fewer, taller risers (250): the line says so and the stair is to be looked at.
  ok("change_stair", {{"id", stair}, {"risers", 12}});
  all = rows();
  EXPECT_EQ(all[0]["state"], "要確認");
  EXPECT_TRUE(all[0]["attention"].get<bool>());
  EXPECT_NE(all[0]["details"].get<std::string>().find("参考値を外れる"), std::string::npos);
  // No handrail: not measured here, never a pass.
  ok("change_stair", {{"id", stair}, {"risers", 0}, {"handrail", "none"}});
  EXPECT_EQ(rows()[0]["state"], "未評価あり");
}

// docs/198: the permit workflow's stage 6 (屋根・庇・バルコニー).
TEST_F(Schedules, PermitChecksReadTheRoofStage) {
  const auto rows = [&] { return ok("permit_checks", {{"stage", 6}})["rows"]; };
  const auto rowOf = [&](const json& all, const std::string& item) {
    for (const json& row : all)
      if (row["item"] == item) return row;
    ADD_FAILURE() << "no row " << item << " in " << all.dump();
    return json();
  };
  json all = rows();
  EXPECT_EQ(rowOf(all, "屋根")["state"], "未入力");
  EXPECT_EQ(rowOf(all, "建築面積")["state"], "参考値");
  EXPECT_EQ(rowOf(all, "まだ確かめていない事")["state"], "未評価");
  // A gable roof with eaves 1200 on the walls' top: no wall up to it yet.
  const std::string roof = ok("create_roof", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"height", 3000}, {"eave", 1200}})["id"];
  const std::string name = modeler.document().find(roof)->name;
  all = rows();
  json row = rowOf(all, name);
  EXPECT_EQ(row["state"], "要確認");
  std::string details = row["details"];
  EXPECT_NE(details.find("切妻"), std::string::npos) << details;
  EXPECT_NE(details.find("軒の出 1200 mm"), std::string::npos) << details;
  EXPECT_NE(details.find("建築面積に入ります"), std::string::npos) << details;
  EXPECT_NE(details.find("屋根まで立ち上げた壁なし"), std::string::npos);
  // The walls under it stood up to it: the row lists them and is done.
  ok("fit_walls_to_roof", {{"roof", roof}});
  row = rowOf(rows(), name);
  EXPECT_EQ(row["state"], "入力済み") << row.dump();
  EXPECT_GE(row["targets"].size(), 5u);  // the roof and its walls
  // A balcony on the north wall: its row says how it counts.
  ok("create_balcony", {{"host", north}});
  const json balcony = rowOf(rows(), "バルコニー");
  EXPECT_NE(balcony["details"].get<std::string>().find("通達どおり"), std::string::npos) << balcony.dump();
}
