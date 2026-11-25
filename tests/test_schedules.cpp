// Tables and tags read from the model (docs/90 G-11, docs/97 N-10 / N-11, docs/104): the door
// and window schedule, the room schedule, areas by storey, and the marks on plans.
#include <cmath>
#include <map>
#include <set>
#include <gtest/gtest.h>
#include <BRep_Tool.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/draw/SheetCheck.h"
#include <iostream>
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
  // docs/209: the sheet list, then the outline of the project, then the plan with its tables.
  EXPECT_EQ(set->sheets.size() + 3, apart->sheets.size());
  const std::string name = "1FL 平面図・建具表・室の表・面積の表";
  ASSERT_EQ(set->names[2], name);
  for (const std::string& n : set->names) EXPECT_EQ(n.find("表") == 0, false) << n;  // no sheet of a table alone
  // On that sheet: each table's title once, the drawing's own caption, the sheet's name and number in the title block.
  std::map<std::string, int> texts;
  for (const auto& item : set->sheets[2].items)
    if (item.kind == sbr::draw::Item::Kind::Text) ++texts[item.text];
  EXPECT_EQ(texts["建具表"], 1);
  EXPECT_EQ(texts["室の表"], 1);
  EXPECT_EQ(texts["面積の表"], 1);
  EXPECT_EQ(texts[name], 1);
  EXPECT_EQ(texts["A-03"], 1);
  bool caption = false;
  for (const auto& [text, n] : texts) caption |= text.rfind("1FL 平面図  S=1:", 0) == 0;
  EXPECT_TRUE(caption);
  // The list: the plan sheet with its scale, the paper big enough for it.
  bool listed = false;
  for (const auto& item : set->sheets.front().items) listed |= item.kind == sbr::draw::Item::Kind::Text && item.text == name;
  EXPECT_TRUE(listed);
  EXPECT_NE(set->sheets[2].paper, "A4");
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

TEST_F(Schedules, DrawingIndexUsesPaperAfterAddingSectionMarks) {
  ok("delete_openings", {{"host", north}, {"openings", {1, 2}}});
  ok("create_room", {{"at", {6000, 3000}}, {"name", "洋室"}});
  const json outline = ok("create_polygon", {{"points", {{-2000, -2000, 0}, {14000, -2000, 0}, {14000, 10000, 0}, {-2000, 10000, 0}}}});
  ok("set_site_boundary", {{"outline", outline["id"]}, {"source", "Test"}});
  const auto plan = [](const auto& set) {
    return std::find(set.names.begin(), set.names.end(), "1FL 平面図") - set.names.begin();
  };
  sbr::doc::DrawingSetSpec spec;
  spec.sections = false;
  const auto without = sbr::doc::drawingSet(modeler.document(), spec);
  ASSERT_TRUE(without);
  ASSERT_LT(plan(*without), static_cast<std::ptrdiff_t>(without->sheets.size()));
  EXPECT_EQ(without->sheets[plan(*without)].paper, "A4");
  spec.sections = true;
  const auto with = sbr::doc::drawingSet(modeler.document(), spec);
  ASSERT_TRUE(with);
  const auto index = plan(*with);
  ASSERT_LT(index, static_cast<std::ptrdiff_t>(with->sheets.size()));
  ASSERT_EQ(with->sheets[index].paper, "A3");
  const auto& items = with->sheets.front().items;
  const auto row = std::find_if(items.begin(), items.end(), [&](const auto& item) {
    return item.kind == sbr::draw::Item::Kind::Text && item.text == with->numbers[index];
  });
  ASSERT_NE(row, items.end());
  EXPECT_TRUE(std::any_of(items.begin(), items.end(), [&](const auto& item) {
    return item.kind == sbr::draw::Item::Kind::Text && item.text == with->sheets[index].paper &&
           std::abs(item.points.front().Y() - row->points.front().Y()) < 1e-6;
  }));
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
  EXPECT_FALSE(call("permit_checks", {{"stage", 13}}).value("ok", true));  // 1-12 (docs/212)
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

// docs/201: the permit workflow's stages 9 (省エネ・設備) and 10 (立面・断面).
TEST_F(Schedules, PermitChecksReadTheEnergyAndElevationStages) {
  const auto rowOf = [&](int stage, const std::string& item) {
    const json all = ok("permit_checks", {{"stage", stage}})["rows"];
    for (const json& row : all)
      if (row["item"] == item) return row;
    ADD_FAILURE() << "no row " << item << " in " << all.dump();
    return json();
  };
  // 9: nothing about energy in the model yet; the plain walls have no build-up.
  EXPECT_EQ(rowOf(9, "適用ルート・地域")["state"], "未入力");
  EXPECT_EQ(rowOf(9, "仕様の無い壁")["targets"].size(), 5u);
  EXPECT_EQ(rowOf(9, "床・屋根の断熱")["state"], "未入力");
  EXPECT_EQ(rowOf(9, "建具の性能")["state"], "未入力");
  EXPECT_EQ(rowOf(9, "設備")["state"], "未入力");
  ok("create_wall", {{"points", {{0, 8000}, {4000, 8000}}}, {"type", "exterior_siding"}, {"height", 3000}});
  const json siding = rowOf(9, "壁の仕様：外壁 窯業系サイディング（通気）");
  EXPECT_EQ(siding["state"], "未入力");  // the presets have no insulation (docs/210: none given)
  EXPECT_NE(siding["details"].get<std::string>().find("断熱の入っていない壁 1 枚"), std::string::npos) << siding.dump();
  // 10: no GL yet; the walls' tops (3000) are the highest point; no sections cut by hand; no true north.
  EXPECT_EQ(rowOf(10, "高さ制限の条件")["state"], "敷地境界待ち");
  const auto site = ok("create_polygon", {{"points", {{-1000,-1000,0},{13000,-1000,0},{13000,10000,0},{-1000,10000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  EXPECT_EQ(rowOf(10, "高さ制限の条件")["state"], "入力不足");
  EXPECT_FALSE(modeler.setSiteHeightConditions("residential", "yokohama_2", "確認用"));
  EXPECT_FALSE(modeler.setSiteHeightConditions("low_1", "yokohama_2", ""));
  ok("set_site_height_conditions", {{"zone", "low_1"}, {"district", "yokohama_2"}, {"source", "架空の都市計画図"}});
  const auto conditions = rowOf(10, "高さ制限の条件");
  EXPECT_EQ(conditions["state"], "入力済み");
  EXPECT_NE(conditions["details"].get<std::string>().find("横浜市 第2種高度地区"), std::string::npos);
  EXPECT_EQ(rowOf(10, "まだ確かめていない事")["state"], "未評価");
  EXPECT_EQ(ok("site_height_conditions", json::object())["zone"], "low_1");
  ASSERT_TRUE(modeler.undo());
  EXPECT_EQ(rowOf(10, "高さ制限の条件")["state"], "入力不足");
  ASSERT_TRUE(modeler.redo());
  EXPECT_EQ(rowOf(10, "高さ制限の条件")["state"], "入力済み");
  const auto otherSite = ok("create_polygon", {{"points", {{20000,0,0},{24000,0,0},{24000,4000,0},{20000,4000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", otherSite}, {"source", "別の敷地"}});
  EXPECT_EQ(rowOf(10, "高さ制限の条件")["state"], "入力不足");
  EXPECT_EQ(rowOf(10, "地盤（GL）")["state"], "未入力");
  EXPECT_NE(rowOf(10, "いちばん高い所（参考）")["details"].get<std::string>().find("z = 0 から 3000 mm"), std::string::npos);
  EXPECT_NE(rowOf(10, "断面の切る位置")["details"].get<std::string>().find("2 つの断面"), std::string::npos);
  EXPECT_EQ(rowOf(10, "立面の向き")["state"], "要確認");
  ASSERT_TRUE(modeler.setLevels({{"", "GL", -500}, {"", "1FL", 0}, {"", "2FL", 3000}}));
  EXPECT_EQ(rowOf(10, "地盤（GL）")["state"], "入力済み");
  EXPECT_NE(rowOf(10, "いちばん高い所（参考）")["details"].get<std::string>().find("GLから 3500 mm"), std::string::npos);
}

TEST_F(Schedules, YokohamaHeightDistrictChoosesTheLowerLimit) {
  const auto site = ok("create_polygon", {{"points", {{0,0,0},{20000,0,0},{20000,30000,0},{0,30000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  const json north = {{"start", {20000,30000,0}}, {"end", {0,30000,0}}};
  json args{{"boundaries", json::array({north})}, {"points", {{10000,27000,200},{10000,0,0},{-10,0,0}}},
            {"ground_z_mm", -500}};
  EXPECT_EQ(call("yokohama_height_district_at", args)["ok"], false);
  const auto setDistrict = [&](const std::string& district) {
    ok("set_site_height_conditions", {{"zone", "mid_high_1"}, {"district", district}, {"source", "架空の計画図"}});
  };
  setDistrict("yokohama_7");
  const auto flat = ok("yokohama_height_district_at", args);
  EXPECT_EQ(flat["samples"][0]["status"], "height_cap");
  EXPECT_EQ(flat["samples"][0]["limit_z_mm"], 30500);
  EXPECT_TRUE(flat["north_slope"].is_null());
  EXPECT_TRUE(flat["samples"][2]["limit_z_mm"].is_null());

  setDistrict("yokohama_4");
  EXPECT_EQ(call("yokohama_height_district_at", args)["ok"], false);
  ASSERT_TRUE(modeler.setSiteNorth({0, "架空の測量図", "2026-11-25"}));
  const auto slope = ok("yokohama_height_district_at", args);
  EXPECT_EQ(slope["height_cap_mm"], 20000);
  EXPECT_EQ(slope["north_rise_mm"], 7500);
  EXPECT_EQ(slope["north_slope"], 0.6);
  EXPECT_EQ(slope["samples"][0]["status"], "north_slant");
  EXPECT_EQ(slope["samples"][0]["limit_z_mm"], 8800);
  EXPECT_EQ(slope["samples"][1]["status"], "height_cap");
  EXPECT_EQ(slope["samples"][1]["slant_limit_z_mm"], 25000);
  EXPECT_EQ(slope["samples"][1]["limit_z_mm"], 19500);
  EXPECT_EQ(slope["samples"][2]["status"], "outside_site");
  EXPECT_EQ(slope["legal_status"], "未評価");

  args["boundaries"] = json::array();
  auto unconfirmed = ok("yokohama_height_district_at", args)["samples"][0];
  EXPECT_EQ(unconfirmed["status"], "boundary_unconfirmed");
  EXPECT_TRUE(unconfirmed["limit_z_mm"].is_null());
  auto road = north;
  road["width_m"] = 6;
  road["source"] = "架空の道路";
  ok("set_site_road", road);
  unconfirmed = ok("yokohama_height_district_at", args)["samples"][0];
  EXPECT_EQ(unconfirmed["status"], "north_road_unconfirmed");
  EXPECT_TRUE(unconfirmed["limit_z_mm"].is_null());
  args["single_north_road_confirmed"] = true;
  const auto halfRoad = ok("yokohama_height_district_at", args)["samples"][0];
  EXPECT_EQ(halfRoad["status"], "north_slant");
  EXPECT_EQ(halfRoad["limit_z_mm"], 10600);
}

TEST_F(Schedules, YokohamaHeightDistrictReferenceValues) {
  const std::array<double, 7> caps{10000, 12000, 15000, 20000, 20000, 20000, 31000};
  const std::array<std::optional<double>, 7> rises{5000, 5000, 7000, 7500, 10000, std::nullopt, std::nullopt};
  for (std::size_t i = 0; i < caps.size(); ++i) {
    const auto rule = sbr::doc::yokohamaHeightDistrictRule("yokohama_" + std::to_string(i + 1));
    ASSERT_TRUE(rule);
    EXPECT_EQ(rule->heightCapMm, caps[i]);
    EXPECT_EQ(rule->northRiseMm, rises[i]);
  }
  EXPECT_FALSE(sbr::doc::yokohamaHeightDistrictRule("none"));
  EXPECT_FALSE(sbr::doc::yokohamaHeightDistrictRule("other"));
}

TEST_F(Schedules, CombinedHeightLimitChangesSourceAndStopsAtTheRoadRange) {
  using sbr::doc::HeightLimitSource;
  using sbr::doc::HeightLimitStudy;
  const auto site = ok("create_polygon", {{"points", {{0,0,0},{20000,0,0},{20000,30000,0},{0,30000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "test site"}});
  ok("set_site_road", {{"start", {0,0,0}}, {"end", {20000,0,0}}, {"source", "test road"}, {"width_m", 6}});
  ok("set_site_height_conditions", {{"zone", "mid_high_1"}, {"district", "yokohama_6"}, {"source", "test map"}});
  ASSERT_TRUE(modeler.setSiteNorth({0, "test survey", "2026-11-25"}));

  HeightLimitStudy study;
  study.road = HeightLimitStudy::Road{6000, 1, {6000, 14000}};
  study.neighbor = HeightLimitStudy::Neighbor{12000, 1};
  study.northRiseMm = 5000;
  study.district = true;
  study.boundaries = {{gp_Pnt(0,30000,0), gp_Pnt(0,0,0)}, {gp_Pnt(20000,30000,0), gp_Pnt(0,30000,0)}};
  const auto sequence = modeler.document().sequence();
  const auto preview = sbr::doc::heightLimitPreview(modeler.document(), study);
  ASSERT_TRUE(preview) << preview.error();
  EXPECT_TRUE(preview->unconfirmed.IsNull());
  EXPECT_NEAR(preview->noLimitAreaMm2, 0, 0.01);
  const auto check = [&](HeightLimitSource source, double x, double y, double z) {
    for (std::size_t i = 0; i < preview->surfaces.size(); ++i) {
      bool found = false;
      for (TopExp_Explorer faces(preview->surfaces[i], TopAbs_FACE); faces.More(); faces.Next()) {
        const auto face = TopoDS::Face(faces.Current());
        const auto plane = BRepAdaptor_Surface(face).Plane();
        const auto normal = plane.Axis().Direction();
        const auto& origin = plane.Location();
        const double height = origin.Z() - (normal.X() * (x - origin.X()) + normal.Y() * (y - origin.Y())) / normal.Z();
        const auto state = BRepClass_FaceClassifier(face, gp_Pnt(x, y, height), 0.001).State();
        if (state != TopAbs_IN && state != TopAbs_ON) continue;
        found = true;
        EXPECT_NEAR(height, z, 0.01);
      }
      EXPECT_EQ(found, i == static_cast<std::size_t>(source)) << "XY " << x << ", " << y << ", source " << i;
    }
  };
  check(HeightLimitSource::Road, 14000, 2500, 8500);
  check(HeightLimitSource::Neighbor, 2000, 12000, 14000);
  check(HeightLimitSource::District, 14000, 12000, 20000);
  check(HeightLimitSource::North, 14000, 26000, 10000);

  study.neighbor.reset();
  study.district = false;
  study.boundaries.clear();
  const auto unconfirmed = sbr::doc::heightLimitPreview(modeler.document(), study);
  ASSERT_TRUE(unconfirmed) << unconfirmed.error();
  EXPECT_NEAR(unconfirmed->unconfirmedAreaMm2, 600e6, 0.1);
  EXPECT_FALSE(unconfirmed->unconfirmed.IsNull());
  for (const auto& surface : unconfirmed->surfaces) EXPECT_TRUE(surface.IsNull());

  study.northRiseMm.reset();
  const auto roadOnly = sbr::doc::heightLimitPreview(modeler.document(), study);
  ASSERT_TRUE(roadOnly) << roadOnly.error();
  EXPECT_NEAR(roadOnly->noLimitAreaMm2, 440e6, 0.1);
  EXPECT_NEAR(sbr::geom::exactBounds(roadOnly->surfaces[0]).CornerMax().Y(), 8000, 0.01);
  EXPECT_EQ(modeler.document().sequence(), sequence);
}

TEST_F(Schedules, YokohamaHeightDistrictPreviewAndPartComparisonShareTheLimit) {
  const auto site = ok("create_polygon", {{"points", {{0,0,0},{20000,0,0},{20000,30000,0},{0,30000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  ok("set_site_height_conditions", {{"zone", "mid_high_1"}, {"district", "yokohama_4"}, {"source", "架空の計画図"}});
  ASSERT_TRUE(modeler.setSiteNorth({0, "架空の測量図", "2026-11-25"}));
  const json edge{{"start", {20000,30000,0}}, {"end", {0,30000,0}}};
  const std::vector<std::array<gp_Pnt, 2>> confirmed{{gp_Pnt(20000,30000,0), gp_Pnt(0,30000,0)}};
  const auto nearPart = ok("create_box", {{"origin", {10000,26000,0}}, {"width",1000}, {"depth",1000}, {"height",9000}})["id"];
  const auto tallPart = ok("create_box", {{"origin", {10000,2000,0}}, {"width",1000}, {"depth",1000}, {"height",20000}})["id"];
  const auto roof = ok("create_polygon", {{"points", {{10000,8000,19500},{11000,8000,19500},
                                                       {11000,10000,19500},{10000,10000,19500}}}})["id"];
  const auto sequence = modeler.document().sequence();
  const auto preview = sbr::doc::yokohamaHeightDistrictPreview(modeler.document(), confirmed, -500, false);
  ASSERT_TRUE(preview);
  EXPECT_TRUE(preview->unconfirmedOutline.IsNull());
  const auto flat = sbr::geom::exactBounds(preview->cap);
  const auto slope = sbr::geom::exactBounds(preview->slant);
  EXPECT_NEAR(flat.CornerMax().Y(), 30000 - 12500 / 0.6, 0.01);
  EXPECT_NEAR(flat.CornerMax().Y(), slope.CornerMin().Y(), 0.01);
  EXPECT_NEAR(flat.CornerMin().Z(), 19500, 0.01);
  EXPECT_NEAR(slope.CornerMax().Z(), 19500, 0.01);
  json args{{"boundaries", json::array({edge})}, {"ids", {nearPart,tallPart,roof}}, {"ground_z_mm", -500}};
  const auto rows = ok("compare_yokohama_height_district", args)["rows"];
  for (const auto& row : rows) {
    const bool cap = row["id"] == tallPart;
    const double excess = cap ? 500 : row["id"] == nearPart ? 200 : 500;
    EXPECT_EQ(row["state"], "超過");
    EXPECT_EQ(row["controlling_surface"], cap ? "height_cap" : "north_slant");
    EXPECT_NEAR(row["excess_mm"].get<double>(), excess, 0.01);
  }
  EXPECT_EQ(modeler.document().sequence(), sequence);

  auto road = edge;
  road["width_m"] = 6;
  road["source"] = "架空の道路";
  ok("set_site_road", road);
  const auto unconfirmed = sbr::doc::yokohamaHeightDistrictPreview(modeler.document(), {}, -500, false);
  ASSERT_TRUE(unconfirmed);
  EXPECT_TRUE(unconfirmed->cap.IsNull());
  EXPECT_TRUE(unconfirmed->slant.IsNull());
  EXPECT_FALSE(unconfirmed->unconfirmedOutline.IsNull());
  EXPECT_NEAR(unconfirmed->unconfirmedAreaMm2, 600e6, 0.01);
  args["boundaries"] = json::array();
  args["ids"] = json::array({nearPart});
  EXPECT_EQ(ok("compare_yokohama_height_district", args)["rows"][0]["state"], "要確認");
  args["single_north_road_confirmed"] = true;
  const auto roadResult = ok("compare_yokohama_height_district", args)["rows"][0];
  EXPECT_EQ(roadResult["state"], "余裕");
  EXPECT_NEAR(roadResult["excess_mm"].get<double>(), -1600, 0.01);
}

TEST_F(Schedules, YokohamaHeightDistrictHorizontalCapExcludesConcaveGap) {
  const auto site = ok("create_polygon", {{"points", {{0,0,0},{12000,0,0},{12000,3000,0},{3000,3000,0},
                                                     {3000,9000,0},{12000,9000,0},{12000,12000,0},{0,12000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  ok("set_site_height_conditions", {{"zone", "mid_high_1"}, {"district", "yokohama_7"}, {"source", "架空の計画図"}});
  const auto gap = ok("create_box", {{"origin", {4000,4500,0}}, {"width",500}, {"depth",500}, {"height",32000}})["id"];
  const auto preview = sbr::doc::yokohamaHeightDistrictPreview(modeler.document(), {}, 0, false);
  ASSERT_TRUE(preview);
  EXPECT_FALSE(preview->cap.IsNull());
  EXPECT_TRUE(preview->slant.IsNull());
  EXPECT_TRUE(preview->unconfirmedOutline.IsNull());
  const auto result = ok("compare_yokohama_height_district",
      {{"boundaries", json::array()}, {"ids", json::array({gap})}, {"ground_z_mm", 0}})["rows"][0];
  EXPECT_EQ(result["state"], "敷地外");
  EXPECT_TRUE(result["excess_mm"].is_null());
}

TEST_F(Schedules, NorthHeightCrossesTheRoadInTrueNorthDirection) {
  const auto site = ok("create_polygon", {{"points", {{0,0,0},{20000,0,0},{20000,30000,0},{0,30000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  const json east = {{"start", {20000,0,0}}, {"end", {20000,30000,0}}};
  const json road = {{"start", {20000,30000,0}}, {"end", {0,30000,0}}, {"source", "架空の道路"}};
  ok("set_site_road", road);
  json args{{"boundaries", json::array({east})}, {"points", {{10000,26000,200}}}, {"ground_z_mm", 0}, {"base_height_mm", 5000}};
  EXPECT_EQ(call("north_height_at", args)["ok"], false);
  ASSERT_TRUE(modeler.setSiteNorth({0, "架空の測量図", "2026-11-25"}));
  const auto missing = ok("north_height_at", args)["samples"][0];
  EXPECT_EQ(missing["status"], "road_width_missing");
  EXPECT_TRUE(missing["limit_z_mm"].is_null());

  auto measured = road;
  measured["width_m"] = 6;
  ok("set_site_road", measured);
  const auto straight = ok("north_height_at", args)["samples"][0];
  EXPECT_EQ(straight["status"], "calculated");
  EXPECT_EQ(straight["exit_point"], json::array({10000,30000,0}));
  EXPECT_EQ(straight["reference_point"], json::array({10000,36000,0}));
  EXPECT_EQ(straight["limit_z_mm"], 17500);
  ASSERT_TRUE(modeler.setSiteNorth({15, "架空の測量図", "2026-11-25"}));
  const auto oblique = ok("north_height_at", args)["samples"][0];
  EXPECT_NEAR(oblique["distance_mm"].get<double>(), 10000 / std::cos(std::acos(-1.0)/12), 0.001);
  EXPECT_NEAR(oblique["reference_point"][1].get<double>(), 36000, 0.001);

  ASSERT_TRUE(modeler.setSiteNorth({45, "架空の測量図", "2026-11-25"}));
  args["points"] = json::array({{10000,20000,0}});
  const auto corner = ok("north_height_at", args)["samples"][0];
  EXPECT_EQ(corner["status"], "vertex_review");
  EXPECT_NEAR(corner["limit_z_mm"].get<double>(), 5000 + 12500 * std::sqrt(2.0), 0.001);
}

TEST_F(Schedules, NorthHeightComparisonClipsAcrossADiscontinuousLimit) {
  const auto site = ok("create_polygon", {{"points", {{0,0,0},{12000,0,0},{12000,3000,0},
                                                     {3000,3000,0},{3000,9000,0},{12000,9000,0},
                                                     {12000,12000,0},{0,12000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  ok("set_site_road", {{"start", {12000,12000,0}}, {"end", {0,12000,0}}, {"source", "架空の道路"}, {"width_m", 6}});
  ASSERT_TRUE(modeler.setSiteNorth({0, "架空の測量図", "2026-11-25"}));
  const json edge = {{"start", {12000,3000,0}}, {"end", {3000,3000,0}}};
  const auto solid = ok("create_box", {{"origin", {2500,1000,0}}, {"width",1000}, {"depth",500}, {"height",8000}})["id"];
  const auto roof = ok("create_polygon", {{"points", {{2500,1000,8000},{3500,1000,8000},
                                                       {3500,1600,8000},{2500,1600,8000}}}})["id"];
  const auto gap = ok("create_box", {{"origin", {4000,4500,0}}, {"width",500}, {"depth",500}, {"height",12000}})["id"];
  const auto unknown = ok("create_box", {{"origin", {4200,1500,0}}, {"width",300}, {"depth",300}, {"height",6500}})["id"];
  json args{{"boundaries", json::array({edge})}, {"ids", {solid, roof, gap}}, {"ground_z_mm", 0}, {"base_height_mm", 5000}};
  const auto sequence = modeler.document().sequence();
  const auto rows = ok("compare_north_height", args)["rows"];
  for (const auto& row : rows) {
    if (row["id"] == gap) {
      EXPECT_EQ(row["state"], "敷地外");
      EXPECT_TRUE(row["excess_mm"].is_null());
    } else {
      EXPECT_EQ(row["state"], "超過");
      EXPECT_NEAR(row["excess_mm"].get<double>(), row["id"] == solid ? 1125 : 1250, 0.01);
    }
  }
  args["boundaries"] = json::array();
  args["ids"] = {solid, unknown};
  const auto partial = ok("compare_north_height", args)["rows"];
  for (const auto& row : partial) {
    EXPECT_EQ(row["state"], "要確認");
    EXPECT_EQ(row["needs_review"], true);
    if (row["id"] == unknown) EXPECT_TRUE(row["excess_mm"].is_null());
    else EXPECT_LT(row["excess_mm"].get<double>(), 0);
  }
  EXPECT_EQ(modeler.document().sequence(), sequence);
}

TEST_F(Schedules, NorthHeightPreviewKeepsTheRoadAndNeighborFacesSeparate) {
  const auto site = ok("create_polygon", {{"points", {{0,0,0},{12000,0,0},{12000,3000,0},
                                                     {3000,3000,0},{3000,9000,0},{12000,9000,0},
                                                     {12000,12000,0},{0,12000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  ok("set_site_road", {{"start", {12000,12000,0}}, {"end", {0,12000,0}}, {"source", "架空の道路"}, {"width_m", 6}});
  ASSERT_TRUE(modeler.setSiteNorth({0, "架空の測量図", "2026-11-25"}));
  const std::vector<std::array<gp_Pnt, 2>> confirmed{{gp_Pnt(12000,3000,0), gp_Pnt(3000,3000,0)}};
  const auto sequence = modeler.document().sequence();
  const auto preview = sbr::doc::northHeightPreview(modeler.document(), confirmed, 0, 5000);
  ASSERT_TRUE(preview);
  EXPECT_NEAR(preview->omittedAreaMm2, 0, 0.01);
  double area = 0;
  int roadFaces = 0, neighborFaces = 0;
  for (TopExp_Explorer faces(preview->surface, TopAbs_FACE); faces.More(); faces.Next()) {
    const auto bounds = sbr::geom::exactBounds(faces.Current());
    const auto lo = bounds.CornerMin(), hi = bounds.CornerMax();
    area += (hi.X() - lo.X()) * (hi.Y() - lo.Y());
    if (std::abs(lo.Z() - 12500) < 0.01) ++roadFaces;
    if (std::abs(lo.Z() - 5000) < 0.01) ++neighborFaces;
    if (lo.X() >= 3000 - 0.01) EXPECT_TRUE(hi.Y() <= 3000 + 0.01 || lo.Y() >= 9000 - 0.01);
  }
  EXPECT_EQ(roadFaces, 2);
  EXPECT_EQ(neighborFaces, 1);
  EXPECT_NEAR(area, 90000000, 0.01);
  EXPECT_NEAR(sbr::geom::exactBounds(preview->referenceLines).CornerMax().Y(), 18000, 0.01);
  const auto partial = sbr::doc::northHeightPreview(modeler.document(), {}, 0, 5000);
  ASSERT_TRUE(partial);
  EXPECT_NEAR(partial->omittedAreaMm2, 27000000, 0.01);
  EXPECT_EQ(modeler.document().sequence(), sequence);
}

TEST_F(Schedules, NorthHeightStopsAtTheFirstExitFromAConcaveSite) {
  const auto site = ok("create_polygon", {{"points", {{0,0,0},{12000,0,0},{12000,3000,0},
                                                     {3000,3000,0},{3000,9000,0},{12000,9000,0},
                                                     {12000,12000,0},{0,12000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  ASSERT_TRUE(modeler.setSiteNorth({0, "架空の測量図", "2026-11-25"}));
  const json lower = {{"start", {12000,3000,0}}, {"end", {3000,3000,0}}};
  const json upper = {{"start", {12000,12000,0}}, {"end", {0,12000,0}}};
  json args{{"boundaries", {lower, upper}}, {"points", {{5000,1000,0},{5000,10000,0},{5000,5000,0},{3000,1000,0}}},
            {"ground_z_mm", 0}, {"base_height_mm", 5000}};
  const auto sequence = modeler.document().sequence();
  const auto samples = ok("north_height_at", args)["samples"];
  EXPECT_EQ(samples[0]["exit_point"], json::array({5000,3000,0}));
  EXPECT_EQ(samples[0]["limit_z_mm"], 7500);
  EXPECT_EQ(samples[1]["exit_point"], json::array({5000,12000,0}));
  EXPECT_EQ(samples[1]["limit_z_mm"], 7500);
  EXPECT_EQ(samples[2]["status"], "outside_site");
  EXPECT_TRUE(samples[2]["limit_z_mm"].is_null());
  EXPECT_EQ(samples[3]["status"], "parallel_review");
  EXPECT_TRUE(samples[3]["limit_z_mm"].is_null());
  args["boundaries"] = json::array({upper});
  const auto unconfirmed = ok("north_height_at", args)["samples"][0];
  EXPECT_EQ(unconfirmed["status"], "boundary_unconfirmed");
  EXPECT_TRUE(unconfirmed["limit_z_mm"].is_null());
  EXPECT_EQ(modeler.document().sequence(), sequence);
}

TEST_F(Schedules, NeighborHeightMatchesTheRectangularReferenceAndRejectsRoads) {
  const auto boundary = ok("create_polygon", {{"points", {{0,0,0},{20000,0,0},{20000,30000,0},{0,30000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", boundary}, {"source", "架空の測量図"}});
  const json road = {{"start", {0,0,0}}, {"end", {20000,0,0}}};
  auto roadArgs = road;
  roadArgs["source"] = "架空の道路";
  ok("set_site_road", roadArgs);
  const json left = {{"start", {0,30000,0}}, {"end", {0,0,0}}};
  const json opposite = {{"start", {20000,30000,0}}, {"end", {0,30000,0}}};
  const json right = {{"start", {20000,0,0}}, {"end", {20000,30000,0}}};
  json args{{"boundaries", {right, opposite, left}}, {"points", {{4000,15000,8000},{0,15000,0}}},
            {"ground_z_mm", 0}, {"base_height_mm", 20000}, {"slope", 1.25}};
  const auto sequence = modeler.document().sequence();
  const auto study = ok("neighbor_height_at", args);
  EXPECT_EQ(study["legal_status"], "未評価");
  const auto samples = study["samples"];
  ASSERT_EQ(samples.size(), 2u);
  EXPECT_EQ(samples[0]["in_site"], true);
  EXPECT_EQ(samples[0]["boundary"], left);
  EXPECT_EQ(samples[0]["closest_boundary_point"], json::array({0,15000,0}));
  EXPECT_NEAR(samples[0]["distance_mm"].get<double>(), 4000, 0.001);
  EXPECT_NEAR(samples[0]["limit_z_mm"].get<double>(), 25000, 0.001);
  EXPECT_EQ(samples[1]["in_site"], true);
  EXPECT_EQ(samples[1]["limit_z_mm"], 20000);
  args["boundaries"].push_back(road);
  EXPECT_EQ(call("neighbor_height_at", args)["ok"], false);
  EXPECT_EQ(modeler.document().sequence(), sequence);
}

TEST_F(Schedules, NeighborHeightUsesFiniteEdgesAndTheConcaveOutline) {
  const json neighbor = {{"start", {8000,6000,0}}, {"end", {8000,10000,0}}};
  const auto concave = ok("create_polygon", {{"points", {{0,0,0},{12000,0,0},{12000,6000,0},
                                                        {8000,6000,0},{8000,10000,0},{0,10000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", concave}, {"source", "架空の測量図"}});
  json args{{"boundaries", {neighbor}}, {"points", {{6000,4500,0},{9000,8000,0}}},
            {"ground_z_mm", 0}, {"base_height_mm", 20000}, {"slope", 1.25}};
  const auto samples = ok("neighbor_height_at", args)["samples"];
  ASSERT_EQ(samples.size(), 2u);
  EXPECT_EQ(samples[0]["closest_boundary_point"], json::array({8000,6000,0}));
  EXPECT_NEAR(samples[0]["distance_mm"].get<double>(), 2500, 0.001);
  EXPECT_NEAR(samples[0]["limit_z_mm"].get<double>(), 23125, 0.001);
  EXPECT_EQ(samples[1]["in_site"], false);
  EXPECT_TRUE(samples[1]["limit_z_mm"].is_null());
  args["boundaries"][0]["end"] = {8000,9000,0};
  EXPECT_EQ(call("neighbor_height_at", args)["ok"], false);
}

TEST_F(Schedules, NeighborHeightPartComparisonBoundsTheMaximumExcess) {
  const auto site = ok("create_polygon", {{"points", {{0,0,0},{12000,0,0},{12000,6000,0},
                                                     {8000,6000,0},{8000,10000,0},{0,10000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  const auto part = [&](double y, double height) {
    return ok("create_box", {{"origin", {4000,y,0}}, {"width",500}, {"depth",500}, {"height",height}})["id"];
  };
  const auto over = part(1000, 27000), touch = part(2000, 25000), below = part(3000, 24000);
  const auto spanning = ok("create_polygon", {{"points", {{-1000,5000,30000},{13000,5000,30000},
                                                          {13000,5200,30000},{-1000,5200,30000}}}})["id"];
  const auto endpoint = ok("create_box", {{"origin", {5500,4000,0}}, {"width",500},
                                          {"depth",500}, {"height",23500}})["id"];
  const json west = {{"start", {0,0,0}}, {"end", {0,10000,0}}};
  const json reentrant = {{"start", {8000,6000,0}}, {"end", {8000,10000,0}}};
  json args{{"boundaries", {west}}, {"ids", {over, touch, below, spanning}},
            {"ground_z_mm", 0}, {"base_height_mm", 20000}, {"slope", 1.25}};
  const auto rows = ok("compare_neighbor_height", args)["rows"];
  const auto rowFor = [](const json& found, const json& id) -> json {
    for (const auto& row : found) if (row["id"] == id) return row;
    return {};
  };
  struct Expected { json id; const char* state; double excess; };
  for (const auto& expected : {Expected{over, "超過", 2000}, Expected{touch, "接触相当", 0}, Expected{below, "余裕", -1000}}) {
    const auto row = rowFor(rows, expected.id);
    ASSERT_TRUE(row.is_object());
    EXPECT_EQ(row["state"], expected.state);
    EXPECT_EQ(row["exact"], true);
    EXPECT_NEAR(row["lower_excess_mm"].get<double>(), expected.excess, 0.001);
    EXPECT_NEAR(row["upper_excess_mm"].get<double>(), expected.excess, 0.001);
  }
  EXPECT_EQ(rowFor(rows, spanning)["state"], "要確認");
  EXPECT_TRUE(rowFor(rows, spanning)["lower_excess_mm"].is_null());
  args["boundaries"] = {west, reentrant};
  args["ids"] = json::array({endpoint});
  const auto corner = ok("compare_neighbor_height", args)["rows"][0];
  EXPECT_EQ(corner["state"], "超過");
  EXPECT_NEAR(corner["lower_excess_mm"].get<double>(), 375, 0.001);
  EXPECT_GE(corner["upper_excess_mm"].get<double>(), 375);
}

TEST_F(Schedules, NeighborHeightPreviewPreservesTheConcaveFootprint) {
  const auto site = ok("create_polygon", {{"points", {{0,0,0},{12000,0,0},{12000,6000,0},
                                                     {8000,6000,0},{8000,10000,0},{0,10000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  const std::vector<std::array<gp_Pnt, 2>> edges{{gp_Pnt(8000,6000,0), gp_Pnt(8000,10000,0)}};
  const auto sequence = modeler.document().sequence();
  const auto result = sbr::doc::neighborHeightPreview(modeler.document(), edges, 0, 20000, 1.25);
  ASSERT_TRUE(result);
  double planArea = 0;
  int triangles = 0;
  for (TopExp_Explorer faces(result->surface, TopAbs_FACE); faces.More(); faces.Next()) {
    std::vector<gp_Pnt> points;
    for (BRepTools_WireExplorer wire(BRepTools::OuterWire(TopoDS::Face(faces.Current()))); wire.More(); wire.Next())
      points.push_back(BRep_Tool::Pnt(wire.CurrentVertex()));
    ASSERT_EQ(points.size(), 3u);
    const gp_Pnt centre((points[0].X() + points[1].X() + points[2].X()) / 3,
                        (points[0].Y() + points[1].Y() + points[2].Y()) / 3, 0);
    EXPECT_FALSE(centre.X() > 8000 && centre.Y() > 6000);
    planArea += std::abs(gp_Vec(points[0], points[1]).Crossed(gp_Vec(points[0], points[2])).Z()) / 2;
    ++triangles;
  }
  EXPECT_GT(triangles, 4);
  EXPECT_LE(triangles, 4096);
  EXPECT_NEAR(planArea, 104000000, 0.01);
  EXPECT_NEAR(sbr::geom::exactBounds(result->surface).CornerMin().Z(), 20000, 0.01);
  EXPECT_EQ(modeler.document().sequence(), sequence);
}

TEST_F(Schedules, RoadHeightPlaneCalculatesTheRoadLevelAndSiteEdgeHeight) {
  json args{{"road_center_z_mm", -5000}, {"site_ground_z_mm", -3000}, {"road_width_mm", 6000}, {"slope", 1.25}};
  const auto sequence = modeler.document().sequence();
  const auto high = ok("road_height_datum", args);
  EXPECT_EQ(high["ground_difference_mm"], 2000);
  EXPECT_EQ(high["road_rise_mm"], 500);
  EXPECT_EQ(high["adopted_road_z_mm"], -4500);
  EXPECT_EQ(high["edge_height_mm"], 3000);
  EXPECT_EQ(high["checked_on"], sbr::doc::kRoadHeightDatumCheckedOn);
  for (const double ground : {-5200.0, -4000.01, -4000.0}) {
    args["site_ground_z_mm"] = ground;
    const auto unadjusted = ok("road_height_datum", args);
    EXPECT_EQ(unadjusted["road_rise_mm"], 0);
    EXPECT_EQ(unadjusted["edge_height_mm"], 2500);
  }
  args["site_ground_z_mm"] = -3999;
  EXPECT_EQ(ok("road_height_datum", args)["road_rise_mm"], 0.5);
  args["road_width_mm"] = 0;
  EXPECT_EQ(call("road_height_datum", args)["ok"], false);
  EXPECT_EQ(modeler.document().sequence(), sequence);
}

TEST_F(Schedules, RoadHeightPlaneReadsTheAppendix3PercentBands) {
  struct Band { double percent; double distanceMm; };
  for (const auto& band : {Band{200, 20000}, Band{200.01, 25000}, Band{300, 25000}, Band{300.01, 30000},
                           Band{400, 30000}, Band{400.01, 35000}}) {
    const auto residential = sbr::doc::roadHeightTableValues("residential_1", band.percent);
    ASSERT_TRUE(residential);
    EXPECT_EQ(residential->applicationDistanceMm, band.distanceMm);
    EXPECT_EQ(residential->slope, 1.25);
    EXPECT_TRUE(residential->hasWideRoadRule);
    const auto industrial = sbr::doc::roadHeightTableValues("exclusive_industrial", band.percent);
    ASSERT_TRUE(industrial);
    EXPECT_EQ(industrial->applicationDistanceMm, band.distanceMm);
    EXPECT_EQ(industrial->item, 3);
    EXPECT_EQ(industrial->slope, 1.5);
    EXPECT_FALSE(industrial->hasWideRoadRule);
  }
  for (const auto& band : {Band{400, 20000}, Band{400.01, 25000}, Band{600, 25000}, Band{600.01, 30000},
                           Band{800, 30000}, Band{800.01, 35000}, Band{1000, 35000}, Band{1000.01, 40000},
                           Band{1100, 40000}, Band{1100.01, 45000}, Band{1200, 45000}, Band{1200.01, 50000}}) {
    const auto commercial = sbr::doc::roadHeightTableValues("commercial", band.percent);
    ASSERT_TRUE(commercial);
    EXPECT_EQ(commercial->applicationDistanceMm, band.distanceMm);
    EXPECT_EQ(commercial->item, 2);
    EXPECT_EQ(commercial->slope, 1.5);
  }
  EXPECT_FALSE(sbr::doc::roadHeightTableValues("none", 100));
  EXPECT_FALSE(sbr::doc::roadHeightTableValues("low_1", 0));

  const auto args = json{{"floor_area_limit_percent", 200}};
  EXPECT_EQ(call("road_height_table_values", args)["ok"], false);
  const auto boundary = ok("create_polygon", {{"points", {{0,0,0},{8000,0,0},{8000,10000,0},{0,10000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", boundary}, {"source", "架空の測量図"}});
  ok("set_site_height_conditions", {{"zone", "low_1"}, {"district", "none"}, {"source", "架空の都市計画図"}});
  const auto row = ok("road_height_table_values", args);
  EXPECT_EQ(row["zone"], "low_1");
  EXPECT_EQ(row["table_item"], 1);
  EXPECT_EQ(row["slope"], 1.25);
  EXPECT_EQ(row["application_distance_mm"], 20000);
  EXPECT_EQ(row["wide_road_rule"], false);
  EXPECT_EQ(row["source"], sbr::doc::kRoadHeightTableSource);
}

TEST_F(Schedules, RoadHeightPlaneMeasuresSetbackAcrossARotatedRoad) {
  const auto boundary = ok("create_polygon", {{"points", {{0,0,0},{8000,8000,0},{0,16000,0},{-8000,8000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", boundary}, {"source", "架空の測量図"}});
  const auto eave = ok("create_polygon", {{"points", {{1000,2000,3000},{2000,3000,3000},
                                                    {1500,3500,3000},{500,2500,3000}}}, {"name", "軒先"}})["id"];
  const auto wall = ok("create_box", {{"origin", {3000,5300,0}}, {"width",500}, {"depth",500}, {"height",2500}})["id"];
  const auto options = json{{"ids", {wall, eave, eave}}};
  EXPECT_EQ(call("measure_road_building_setback", options)["ok"], false);
  ok("set_site_road", {{"start", {8000,8000,0}}, {"end", {0,0,0}}, {"source", "架空の道路"}});
  EXPECT_EQ(call("measure_road_building_setback", {{"ids", {boundary}}})["ok"], false);
  const auto sequence = modeler.document().sequence();
  const auto measured = ok("measure_road_building_setback", options);
  EXPECT_EQ(measured["id"], eave);
  EXPECT_EQ(measured["name"], "軒先");
  EXPECT_NEAR(measured["distance_mm"].get<double>(), 1000 / std::sqrt(2.0), 0.001);
  EXPECT_EQ(modeler.document().sequence(), sequence);
  ok("move", {{"ids", {eave}}, {"vector", {-500,500,0}}});
  const auto moved = ok("measure_road_building_setback", options);
  EXPECT_EQ(moved["id"], wall);
  EXPECT_NEAR(moved["distance_mm"].get<double>(), 1800 / std::sqrt(2.0), 0.001);
  ok("move", {{"ids", {wall}}, {"vector", {2000,0,0}}});
  EXPECT_EQ(ok("measure_road_building_setback", options)["distance_mm"], 0);
}

TEST_F(Schedules, RoadHeightPlaneUsesTheShapeAtItsWorldPlacement) {
  const auto boundary = ok("create_polygon", {{"points", {{-1000,0,0},{10000,0,0},{10000,10000,0},{-1000,10000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", boundary}, {"source", "架空の測量図"}});
  const auto box = ok("create_box", {{"origin", {0,2000,0}}, {"width",500}, {"depth",500}, {"height",6000}})["id"];
  const auto roof = ok("create_polygon", {{"points", {{0,2000,5500},{500,2000,5500},{500,4000,8000},{0,4000,8000}}}})["id"];
  const auto options = json{{"ids", {roof, box}}, {"edge_height_mm", 3000}, {"slope", 1.25}};
  EXPECT_EQ(call("compare_road_height_plane", options)["ok"], false);
  ok("set_site_road", {{"start", {10000,0,0}}, {"end", {-1000,0,0}}, {"source", "架空の道路"}});
  const auto rows = ok("compare_road_height_plane", options)["rows"];
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0]["id"], box);
  EXPECT_NEAR(rows[0]["excess_mm"].get<double>(), 500, 0.001);
  EXPECT_EQ(rows[1]["id"], roof);
  EXPECT_NEAR(rows[1]["excess_mm"].get<double>(), 0, 0.001);
  ok("move", {{"ids", {box}}, {"vector", {0,1000,0}}});
  const auto moved = ok("compare_road_height_plane", options)["rows"];
  EXPECT_EQ(moved[1]["id"], box);
  EXPECT_NEAR(moved[1]["excess_mm"].get<double>(), -750, 0.001);
}

TEST_F(Schedules, RoadHeightPlaneClipsPartsAtTheDistanceFromTheOppositeEdge) {
  const auto boundary = ok("create_polygon", {{"points", {{0,0,0},{8000,0,0},{8000,10000,0},{0,10000,0}}}})["id"];
  ok("set_site_boundary", {{"outline", boundary}, {"source", "架空の測量図"}});
  ok("set_site_road", {{"start", {0,0,0}}, {"end", {8000,0,0}}, {"source", "架空の道路"}});
  const auto across = ok("create_polygon", {{"points", {{0,1000,4000},{500,1000,4000},{500,5000,12000},{0,5000,12000}}}})["id"];
  const auto touches = ok("create_box", {{"origin", {1000,3000,0}}, {"width",500}, {"depth",500}, {"height",6000}})["id"];
  const auto outside = ok("create_box", {{"origin", {2000,3100,0}}, {"width",500}, {"depth",500}, {"height",12000}})["id"];
  json options{{"ids", {across, touches, outside}}, {"edge_height_mm", 3000}, {"slope", 1.25}};
  EXPECT_NEAR(ok("compare_road_height_plane", options)["rows"][1]["excess_mm"].get<double>(), 2750, 0.001);
  options["road_width_mm"] = 6000;
  EXPECT_EQ(call("compare_road_height_plane", options)["ok"], false);
  options["application_distance_mm"] = 9000;
  const auto rows = ok("compare_road_height_plane", options)["rows"];
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0]["id"], across);
  EXPECT_NEAR(rows[0]["excess_mm"].get<double>(), 1250, 0.001);
  EXPECT_EQ(rows[1]["id"], touches);
  ASSERT_TRUE(rows[1]["excess_mm"].is_number());
  EXPECT_NEAR(rows[1]["excess_mm"].get<double>(), -750, 0.001);
  EXPECT_EQ(rows[2]["id"], outside);
  EXPECT_TRUE(rows[2]["excess_mm"].is_null());
  const auto preview = sbr::doc::compareRoadHeightPlane(modeler.document(), {across.get<std::string>()},
                                                       3000, 1.25, sbr::doc::RoadHeightPlaneRange{6000, 9000});
  ASSERT_TRUE(preview);
  EXPECT_NEAR(sbr::geom::exactBounds(preview->plane).CornerMax().Y(), 3000, 0.001);

  options["building_setback_mm"] = -1;
  EXPECT_EQ(call("compare_road_height_plane", options)["ok"], false);
  options["building_setback_mm"] = 1000;
  const auto shiftedRows = ok("compare_road_height_plane", options)["rows"];
  ASSERT_EQ(shiftedRows.size(), 3u);
  EXPECT_EQ(shiftedRows[0]["id"], across);
  EXPECT_NEAR(shiftedRows[0]["excess_mm"].get<double>(), -750, 0.001);
  EXPECT_TRUE(shiftedRows[1]["excess_mm"].is_null());
  EXPECT_TRUE(shiftedRows[2]["excess_mm"].is_null());
  const auto shifted = sbr::doc::compareRoadHeightPlane(modeler.document(), {across.get<std::string>()},
                                                       3000, 1.25, sbr::doc::RoadHeightPlaneRange{6000, 9000, 1000});
  ASSERT_TRUE(shifted);
  ASSERT_TRUE(shifted->siteApplicationEndMm.has_value());
  EXPECT_NEAR(*shifted->siteApplicationEndMm, 2000, 0.001);
  EXPECT_NEAR(shifted->edgeHeightRiseMm, 1250, 0.001);
  const auto shiftedBounds = sbr::geom::exactBounds(shifted->plane);
  ASSERT_FALSE(shiftedBounds.IsVoid());
  EXPECT_NEAR(shiftedBounds.CornerMax().Y(), 2000, 0.001);
  EXPECT_NEAR(shiftedBounds.CornerMin().Z(), 4250, 0.001);

  const auto atBoundary = sbr::doc::compareRoadHeightPlane(modeler.document(), {across.get<std::string>()},
                                                          3000, 1.25, sbr::doc::RoadHeightPlaneRange{6000, 9000, 3000});
  ASSERT_TRUE(atBoundary);
  EXPECT_TRUE(atBoundary->plane.IsNull());
  ASSERT_EQ(atBoundary->rows.size(), 1u);
  EXPECT_FALSE(atBoundary->rows[0].excessMm.has_value());
}

// docs/202: the permit workflow's stages 11 (一式の突合せ) and 12 (申請用出力).
TEST_F(Schedules, PermitChecksSumUpTheStagesAndHoldTheOutput) {
  const auto rowOf = [&](int stage, const std::string& item) {
    const json all = ok("permit_checks", {{"stage", stage}})["rows"];
    for (const json& row : all)
      if (row["item"] == item) return row;
    ADD_FAILURE() << "no row " << item << " in " << all.dump();
    return json();
  };
  // No grid: stage 2 has rows left, named in the summary.
  const json grid = rowOf(11, "2 通り芯");
  EXPECT_EQ(grid["state"].get<std::string>().rfind("残り ", 0), 0u) << grid.dump();
  EXPECT_NE(grid["details"].get<std::string>().find("通り芯（未入力）"), std::string::npos) << grid.dump();
  EXPECT_EQ(rowOf(11, "建具の符号")["state"], "入力済み");
  // A second room of the same storey with the same name.
  ok("create_room", {{"at", {6000, 3000}}, {"name", "居間"}});
  const json twins = rowOf(11, "室名");
  EXPECT_EQ(twins["state"], "要確認");
  EXPECT_EQ(twins["targets"].size(), 2u);
  // The output stays on hold, saying which stages are left; study drawings can be made.
  const json hold = rowOf(12, "申請用出力");
  EXPECT_EQ(hold["state"], "保留");
  EXPECT_NE(hold["details"].get<std::string>().find("2 通り芯"), std::string::npos) << hold.dump();
  EXPECT_EQ(rowOf(12, "検討用の出力")["state"], "出せます");
  EXPECT_FALSE(call("permit_checks", {{"stage", 13}}).value("ok", true));
}

// docs/203: daylight and ventilation of a room from what was given for its candidate openings
// (法 28 条 1・2 項, 令 19 条 3 項 表（三） 1/7, 令 20 条), never a pass.
TEST_F(Schedules, RoomDaylightComesFromTheGivenFactorsAndAreas) {
  const auto west = [&] {
    const json inputs = ok("room_opening_inputs");
    for (const json& r : inputs["rooms"])
      if (r["id"] == room) return r;
    ADD_FAILURE() << inputs.dump();
    return json();
  };
  EXPECT_EQ(west()["daylight_comparison"]["state"], "未入力");
  ok("set_room_daylight", {{"id", room}, {"kind", "dwelling"}});
  json r = west();
  ASSERT_FALSE(r["opening_candidates"].empty());
  EXPECT_EQ(r["daylight_comparison"]["state"], "未評価");  // no factors given yet
  EXPECT_EQ(r["ventilation_comparison"]["state"], "未評価");
  // Factors of 3 (the upper limit) on every opening: the room's window area x 3 is well over 24 m2 / 7.
  ok("set_opening_daylight", {{"host", south}, {"factor", 3.0}, {"basis", "架空の値"}, {"ventilation_m2", 1.0}});
  ok("set_opening_daylight", {{"host", north}, {"factor", 3.0}, {"basis", "架空の値"}, {"ventilation_m2", 1.0}});
  ok("set_opening_daylight", {{"host", east}, {"factor", 3.0}, {"ventilation_m2", 1.0}});
  r = west();
  EXPECT_EQ(r["daylight_comparison"]["state"], "参考値内") << r.dump();
  EXPECT_NE(r["daylight_comparison"]["text"].get<std::string>().find("1/7"), std::string::npos);
  EXPECT_EQ(r["opening_candidates"][0]["daylight_factor"], 3.0);
  EXPECT_EQ(r["opening_candidates"][0]["daylight_basis"], "架空の値");
  // Factor 0 (an opening facing a wall close by): no daylight at all.
  ok("set_opening_daylight", {{"host", south}, {"factor", 0.0}});
  ok("set_opening_daylight", {{"host", north}, {"factor", 0.0}});
  EXPECT_EQ(west()["daylight_comparison"]["state"], "参考値を外れる");
  // Ventilation: the room's candidates open 1 m2 each; 24 m2 / 20 = 1.2.
  const std::size_t n = west()["opening_candidates"].size();
  EXPECT_EQ(west()["ventilation_comparison"]["state"], n * 1.0 >= 1.2 ? "参考値内" : "参考値を外れる");
  ok("set_room_daylight", {{"id", room}, {"kind", "dwelling"}, {"ventilation", "equipment"}});
  EXPECT_EQ(west()["ventilation_comparison"]["state"], "換気設備");
  ok("set_room_daylight", {{"id", room}, {"kind", "none"}});
  EXPECT_EQ(west()["daylight_comparison"]["state"], "対象外");
  // Refused: a factor over 3, a kind that is not one of the listed.
  EXPECT_FALSE(call("set_opening_daylight", {{"host", south}, {"factor", 3.5}}).value("ok", true));
  EXPECT_FALSE(call("set_room_daylight", {{"id", room}, {"kind", "shop"}}).value("ok", true));
  const auto steps = modeler.history().undoCount();
  ASSERT_TRUE(modeler.undo());
  EXPECT_EQ(modeler.history().undoCount(), steps - 1);
}

// docs/205: the daylight factor of each wall opening worked out from the model (令 20 条 2 項).
// A box of walls 8000 x 5000, 120 thick, 5000 tall, on a site whose boundary is 1000 south of the
// wall line and 5000 north of it; one window in the south wall and one in the north wall, their
// centres 1450 up (sill 900, height 1100).
TEST(DaylightFactors, WorkedOutFromTheSiteAndTheBuilding) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  const auto call = [&](const std::string& command, json args = json::object()) {
    return json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
  };
  const auto ok = [&](const std::string& command, json args = json::object()) {
    const json r = call(command, args);
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  const json walls = ok("create_wall", {{"points", {{0, 0}, {8000, 0}, {8000, 5000}, {0, 5000}, {0, 0}}}, {"thickness", 120}, {"height", 5000}})["ids"];
  const std::string south = walls[0], north = walls[2];
  ok("place_openings", {{"host", south}, {"face_normal", {0, -1, 0}}, {"count", 1}, {"width", 1600}, {"height", 1100}, {"sill", 900}});
  ok("place_openings", {{"host", north}, {"face_normal", {0, 1, 0}}, {"count", 1}, {"width", 1600}, {"height", 1100}, {"sill", 900}});
  const auto estimates = [&] {
    std::map<std::string, json> byHost;
    const json rows = ok("daylight_factor_estimates")["openings"];
    for (const json& row : rows) byHost[row["host"].get<std::string>()] = row;
    return byHost;
  };
  EXPECT_NE(estimates()[south]["problem"].get<std::string>().find("敷地境界"), std::string::npos);
  const std::string outline = ok("create_polygon", {{"points", {{-5000, -1000, 0}, {13000, -1000, 0}, {13000, 10000, 0}, {-5000, 10000, 0}}}})["id"];
  ok("set_site_boundary", {{"outline", outline}, {"source", "架空の測量図"}});
  EXPECT_NE(estimates()[south]["problem"].get<std::string>().find("用途地域"), std::string::npos);
  EXPECT_FALSE(call("set_site_zone", {{"zone", "residential"}}).value("ok", true));  // needs its source
  ok("set_site_zone", {{"zone", "residential"}, {"source", "架空の都市計画図"}});
  // South: the wall's top over the opening, 940 + 1 from the boundary, 5000 - 1450 above its centre:
  // 941 / 3550 x 6 - 1.4 = 0.1904 (under 7 m, not negative).
  json s = estimates()[south];
  ASSERT_TRUE(s.contains("factor")) << s.dump();
  EXPECT_NEAR(s["horizontal_mm"].get<double>(), 941.0, 1e-6);
  EXPECT_NEAR(s["vertical_mm"].get<double>(), 3550.0, 1e-6);
  EXPECT_NEAR(s["factor"].get<double>(), 941.0 / 3550.0 * 6 - 1.4, 1e-9);
  EXPECT_FALSE(s["faces_road"].get<bool>());
  EXPECT_NE(s["basis"].get<std::string>().find("令 20 条 2 項 1 号"), std::string::npos);
  // North: 4940 + 1 / 3550 x 6 - 1.4 = 6.95: at most 3.0.
  EXPECT_NEAR(estimates()[north]["factor"].get<double>(), 3.0, 1e-12);
  // The south edge is a road 4 m wide: measured to its far side, 4941 / 3550 -> 3.0.
  ok("set_site_road", {{"start", {-5000, -1000, 0}}, {"end", {13000, -1000, 0}}, {"source", "架空"}, {"width_m", 4.0}});
  s = estimates()[south];
  EXPECT_TRUE(s["faces_road"].get<bool>());
  EXPECT_NEAR(s["horizontal_mm"].get<double>(), 4941.0, 1e-6);
  EXPECT_NEAR(s["factor"].get<double>(), 3.0, 1e-12);
  // Given to the openings in one step; the room check reads them.
  const auto steps = modeler.history().undoCount();
  const json applied = ok("apply_daylight_factors");
  EXPECT_EQ(applied["given"], 2);
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  int withFactor = 0;
  for (const auto& row : sbr::doc::openingSchedule(modeler.document()).rows) withFactor += row.daylightFactor >= 0 && !row.daylightBasis.empty();
  EXPECT_EQ(withFactor, 2);
  ASSERT_TRUE(modeler.undo());
  for (const auto& row : sbr::doc::openingSchedule(modeler.document()).rows) EXPECT_LT(row.daylightFactor, 0.0);
}

// docs/207: the daylight and ventilation table of the rooms given a kind.
TEST_F(Schedules, DaylightTableListsTheRoomsGivenAKind) {
  EXPECT_FALSE(sbr::doc::tableHasContent(sbr::doc::scheduleTable(modeler.document(), 6), 6));  // no kind given yet
  ok("set_room_daylight", {{"id", room}, {"kind", "dwelling"}});
  ok("set_opening_daylight", {{"host", south}, {"factor", 1.0}, {"basis", "架空の係数"}, {"ventilation_m2", 0.5}});
  ok("set_opening_daylight", {{"host", north}, {"factor", 1.0}, {"ventilation_m2", 0.5}});
  ok("set_opening_daylight", {{"host", east}, {"factor", 1.0}, {"ventilation_m2", 0.5}});
  const auto table = sbr::doc::scheduleTable(modeler.document(), 6);
  ASSERT_TRUE(sbr::doc::tableHasContent(table, 6));
  EXPECT_EQ(table.title, "採光・換気計算表");
  ASSERT_GE(table.rows.size(), 2u);
  EXPECT_EQ(table.rows[0][0], "居間");
  EXPECT_EQ(table.rows[0][2], "住宅の居室");
  EXPECT_EQ(table.rows[0][6], "1.00");
  const auto& total = table.rows.back();
  EXPECT_EQ(total[4], "計");
  EXPECT_NE(total[7].find("／必要 3.43"), std::string::npos) << total[7];  // 24 m2 / 7
  EXPECT_NE(total[8].find("／必要 1.20"), std::string::npos) << total[8];  // 24 m2 / 20
  EXPECT_EQ(total[9].rfind("採光 ", 0), 0u);
  bool basis = false;
  for (const auto& r : table.rows) basis = basis || r[10] == "架空の係数";
  EXPECT_TRUE(basis);
  // As a sheet of its own (DXF) and as CSV.
  const auto dir = std::filesystem::temp_directory_path() / "sbr-daylight-table";
  std::filesystem::create_directories(dir);
  const auto path = (dir / "daylight.dxf").string();
  ok("export_schedule", {{"path", path}, {"table", "daylight"}});
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_NE(sbr::doc::tableCsv(table).find("採光補正係数"), std::string::npos);
}

// docs/209: the outline of the project, read from the model's data, first in the drawing set.
TEST_F(Schedules, ProjectOutlineReadsTheSiteAndTheAreas) {
  const auto rowOf = [](const sbr::draw::Table& t, const std::string& item) {
    for (const auto& r : t.rows)
      if (r[0] == item) return r;
    ADD_FAILURE() << "no row " << item;
    return std::vector<std::string>(3);
  };
  auto outline = sbr::doc::scheduleTable(modeler.document(), 10);
  EXPECT_EQ(outline.title, "計画概要");
  EXPECT_EQ(rowOf(outline, "敷地面積")[1], "未入力");
  EXPECT_EQ(rowOf(outline, "階数")[1], "地上 1");
  EXPECT_EQ(rowOf(outline, "延べ面積")[1].substr(0, 5), "48.00");  // the slab of the 8000 x 6000 storey
  const std::string site = ok("create_polygon", {{"points", {{-2000, -2000, 0}, {14000, -2000, 0}, {14000, 10000, 0}, {-2000, 10000, 0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  ok("set_site_zone", {{"zone", "residential"}, {"source", "架空の都市計画図"}});
  outline = sbr::doc::scheduleTable(modeler.document(), 10);
  EXPECT_EQ(rowOf(outline, "敷地面積")[1].substr(0, 6), "192.00");  // 16 m x 12 m
  EXPECT_EQ(rowOf(outline, "用途地域の区分")[1], "住居系");
  EXPECT_EQ(rowOf(outline, "建蔽率（参考）")[1], "25.00 %");  // 48 / 192
  sbr::doc::DrawingSetSpec spec;
  spec.title.project = "架空の家";
  const auto set = sbr::doc::drawingSet(modeler.document(), spec);
  ASSERT_TRUE(set) << set.error();
  ASSERT_GE(set->names.size(), 3u);
  EXPECT_EQ(set->names[0], "図面リスト");
  EXPECT_EQ(set->names[1], "計画概要");
}

// docs/210: energy inputs (route and region, insulation, opening performance), the stage 9 rows
// and the energy specification table; nothing compared with any standard.
TEST_F(Schedules, EnergyInputsShowInStageNineAndTheirTable) {
  const auto rowOf = [&](const std::string& item) {
    const json all = ok("permit_checks", {{"stage", 9}})["rows"];
    for (const json& row : all)
      if (row["item"] == item) return row;
    ADD_FAILURE() << "no row " << item << " in " << all.dump();
    return json();
  };
  EXPECT_FALSE(call("set_energy_route", {{"route", "spec"}, {"region", 6}, {"source", "x"}}).value("ok", true));  // no site yet
  const std::string site = ok("create_polygon", {{"points", {{-2000, -2000, 0}, {14000, -2000, 0}, {14000, 10000, 0}, {-2000, 10000, 0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空"}});
  EXPECT_FALSE(call("set_energy_route", {{"route", "spec"}, {"region", 9}, {"source", "x"}}).value("ok", true));
  EXPECT_FALSE(call("set_energy_route", {{"route", "spec"}, {"region", 6}}).value("ok", true));  // its source
  ok("set_energy_route", {{"route", "spec"}, {"region", 6}, {"source", "架空の地域区分表"}});
  json route = rowOf("適用ルート・地域");
  EXPECT_EQ(route["state"], "入力済み");
  EXPECT_NE(route["details"].get<std::string>().find("仕様基準　6 地域"), std::string::npos) << route.dump();
  // Insulation on all the walls: 100 mm of λ 0.038 -> R 2.63.
  json walls = json::array({south, east, north, west, partition});
  EXPECT_FALSE(call("set_insulation", {{"ids", walls}, {"material", "グラスウール"}, {"thickness_mm", 100}}).value("ok", true));  // its source
  ok("set_insulation", {{"ids", walls}, {"material", "グラスウール 16K"}, {"thickness_mm", 100}, {"conductivity", 0.038},
                        {"placement", "fill"}, {"source", "架空のカタログ"}});
  const json insulated = rowOf("壁の仕様：厚さだけの壁");
  EXPECT_EQ(insulated["state"], "入力済み") << insulated.dump();
  EXPECT_NE(insulated["details"].get<std::string>().find("R 2.63"), std::string::npos) << insulated.dump();
  EXPECT_EQ(rowOf("建具の性能")["state"], "未入力");
  ok("set_opening_performance", {{"host", south}, {"u_value", 2.33}, {"glass", "Low-E 複層"}, {"source", "架空のカタログ"}});
  const auto table = sbr::doc::scheduleTable(modeler.document(), 11);
  EXPECT_EQ(table.title, "省エネ仕様の表");
  bool wall = false, window = false;
  for (const auto& r : table.rows) {
    wall = wall || (r[0] == "外皮：壁" && r[2] == "5" && r[3].find("グラスウール 16K 100 mm（充填）") == 0);
    window = window || (r[0] == "開口部" && r[3].find("熱貫流率 2.33") == 0 && r[4] == "架空のカタログ");
  }
  EXPECT_TRUE(wall);
  EXPECT_TRUE(window);
  EXPECT_FALSE(call("set_insulation", {{"ids", {room}}, {"material", "x"}, {"thickness_mm", 10}, {"source", "x"}}).value("ok", true));  // a room is no part
}

// docs/211: two rooms joined by fusuma as one (法 28 条 4 項), the mechanical ventilation
// (令 20 条の 8: 0.5 x floor x ceiling height for a dwelling's room) and verandas (x 0.7).
TEST_F(Schedules, JoinedRoomsMechanicalVentilationAndVerandas) {
  const std::string east = ok("create_room", {{"at", {6000, 3000}}, {"name", "洋室"}})["id"];
  const auto byName = [&](const std::string& name) {
    const json inputs = ok("room_opening_inputs");
    for (const json& r : inputs["rooms"])
      if (r["name"] == name) return r;
    ADD_FAILURE() << inputs.dump();
    return json();
  };
  ok("set_room_daylight", {{"id", room}, {"kind", "dwelling"}});
  ok("set_room_daylight", {{"id", east}, {"kind", "dwelling"}});
  // Mechanical: no ceiling height yet.
  EXPECT_EQ(byName("居間")["mechanical_comparison"]["state"], "未評価");
  ok("set_room_finish", {{"id", room}, {"ceiling_height", 2400}});
  json r = byName("居間");
  EXPECT_EQ(r["mechanical_comparison"]["state"], "未入力");
  EXPECT_NE(r["mechanical_comparison"]["text"].get<std::string>().find("= 28.8 m³/h"), std::string::npos) << r.dump();
  ok("set_room_ventilation", {{"id", room}, {"fan_m3h", 30}});
  EXPECT_EQ(byName("居間")["mechanical_comparison"]["state"], "参考値内");
  ok("set_room_ventilation", {{"id", room}, {"fan_m3h", 20}});
  EXPECT_EQ(byName("居間")["mechanical_comparison"]["state"], "参考値を外れる");
  // The kind set again keeps the airflow.
  ok("set_room_daylight", {{"id", room}, {"kind", "dwelling"}, {"ventilation", "window"}});
  EXPECT_EQ(byName("居間")["fan_m3h"], 20.0);
  // Joined: both rooms say so, the floor is 48 m2 (24 + 24), the airflow stays.
  ok("set_opening_daylight", {{"host", south}, {"factor", 1.0}, {"ventilation_m2", 1.0}});
  ok("set_opening_daylight", {{"host", north}, {"factor", 1.0}, {"ventilation_m2", 1.0}});
  ok("set_opening_daylight", {{"host", this->east}, {"factor", 1.0}, {"ventilation_m2", 1.0}});
  ok("set_room_ventilation", {{"id", room}, {"joined_with", east}});
  r = byName("居間");
  EXPECT_EQ(r["joined_with"], east);
  EXPECT_EQ(byName("洋室")["joined_with"], room);
  EXPECT_EQ(r["fan_m3h"], 20.0);
  const std::string text = r["daylight_comparison"]["text"];
  EXPECT_NE(text.find("床面積 48.00 m²"), std::string::npos) << text;
  EXPECT_NE(text.find("洋室 と一室として"), std::string::npos) << text;
  // Let go: each alone again.
  ok("set_room_ventilation", {{"id", room}, {"joined_with", ""}});
  EXPECT_EQ(byName("洋室")["joined_with"], "");
  EXPECT_NE(byName("居間")["daylight_comparison"]["text"].get<std::string>().find("床面積 24.00 m²"), std::string::npos);
  EXPECT_FALSE(call("set_room_ventilation", {{"id", room}, {"joined_with", room}}).value("ok", true));
}

// docs/211: a veranda outside the south window: the factor worked out x 0.7 (令 20 条 2 項).
TEST(DaylightFactors, AVerandaTakesSevenTenths) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  const auto ok = [&](const std::string& command, json args = json::object()) {
    const json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  const json walls = ok("create_wall", {{"points", {{0, 0}, {8000, 0}, {8000, 5000}, {0, 5000}, {0, 0}}}, {"thickness", 120}, {"height", 5000}})["ids"];
  const std::string south = walls[0];
  ok("place_openings", {{"host", south}, {"face_normal", {0, -1, 0}}, {"count", 1}, {"width", 1600}, {"height", 1100}, {"sill", 900}});
  const std::string outline = ok("create_polygon", {{"points", {{-5000, -1000, 0}, {13000, -1000, 0}, {13000, 10000, 0}, {-5000, 10000, 0}}}})["id"];
  ok("set_site_boundary", {{"outline", outline}, {"source", "架空"}});
  ok("set_site_zone", {{"zone", "residential"}, {"source", "架空"}});
  ok("set_opening_veranda", {{"host", south}, {"veranda", true}});
  const json rows = ok("daylight_factor_estimates")["openings"];
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_NEAR(rows[0]["factor"].get<double>(), (941.0 / 3550.0 * 6 - 1.4) * 0.7, 1e-9);
  EXPECT_NE(rows[0]["basis"].get<std::string>().find("×0.7"), std::string::npos);
  EXPECT_TRUE(ok("room_opening_inputs")["unmatched_openings"][0]["veranda"].get<bool>());
}

// docs/212: a person's mark on a permit workflow stage goes back to 要再確認 when the model changes
// the stage's rows, and comes back when the change is undone; saved with the file; never a pass.
TEST_F(Schedules, APermitStageMarkGoesBackToRecheckWhenTheModelChanges) {
  const auto mark = [&](int stage) { return ok("permit_checks", {{"stage", stage}})["confirmation"]; };
  EXPECT_EQ(mark(3)["state"], "未確認");
  EXPECT_EQ(ok("confirm_permit_stage", {{"stage", 3}, {"by", "オーナー"}, {"note", "目で見た"}, {"on", "2026-11-21"}})["state"],
            "確認済み");
  json m = mark(3);
  EXPECT_EQ(m["state"], "確認済み");
  EXPECT_EQ(m["on"], "2026-11-21");
  EXPECT_EQ(m["by"], "オーナー");
  EXPECT_EQ(m["note"], "目で見た");
  EXPECT_EQ(mark(2)["state"], "未確認");  // other stages keep their own
  // A room in the east half changes the plan rows.
  ok("create_room", {{"at", {6000, 3000}}, {"name", "洋室"}});
  m = mark(3);
  EXPECT_EQ(m["state"], "要再確認");
  EXPECT_FALSE(m["changed"].empty()) << m.dump();
  ok("undo");
  EXPECT_EQ(mark(3)["state"], "確認済み");
  ok("create_room", {{"at", {6000, 3000}}, {"name", "洋室"}});
  EXPECT_EQ(mark(3)["state"], "要再確認");
  ok("confirm_permit_stage", {{"stage", 3}});
  m = mark(3);
  EXPECT_EQ(m["state"], "確認済み");
  EXPECT_EQ(m["on"].get<std::string>().size(), 10u);  // today, YYYY-MM-DD
  EXPECT_FALSE(m.contains("by"));
  // Saved and read back.
  sbr::doc::Document copy;
  ASSERT_TRUE(sbr::doc::deserializeDocument(sbr::doc::serializeDocument(modeler.document()), copy));
  EXPECT_EQ(sbr::doc::permitConfirmation(copy, 3).state, "確認済み");
  // Stage 12 counts the marks of 1-11.
  json rows = ok("permit_checks", {{"stage", 12}})["rows"];
  bool counted = false;
  for (const json& r : rows)
    if (r["item"] == "人の確認") {
      counted = true;
      EXPECT_EQ(r["state"], "未確認");
      EXPECT_NE(r["details"].get<std::string>().find("印のある工程 1。"), std::string::npos) << r.dump();
      EXPECT_NE(r["details"].get<std::string>().find("未確認：1・2・4・5"), std::string::npos) << r.dump();
    }
  EXPECT_TRUE(counted);
  // Taken away, and back with undo.
  ok("confirm_permit_stage", {{"stage", 3}, {"confirmed", false}});
  EXPECT_EQ(mark(3)["state"], "未確認");
  ok("undo");
  EXPECT_EQ(mark(3)["state"], "確認済み");
  // Wrong input changes nothing.
  EXPECT_FALSE(call("confirm_permit_stage", {{"stage", 13}}).value("ok", true));
  EXPECT_FALSE(call("confirm_permit_stage", {{"stage", 2}, {"on", "21/11/2026"}}).value("ok", true));
  EXPECT_EQ(mark(2)["state"], "未確認");
  // Stages 1 and 4 are read in the core too.
  rows = ok("permit_checks", {{"stage", 1}})["rows"];
  ASSERT_FALSE(rows.empty());
  EXPECT_EQ(rows[0]["item"], "敷地境界");
  EXPECT_EQ(rows[0]["state"], "未入力");
  rows = ok("permit_checks", {{"stage", 4}})["rows"];
  bool living = false;
  for (const json& r : rows) living = living || r["item"].get<std::string>().rfind("居間", 0) == 0;
  EXPECT_TRUE(living) << rows.dump();
}

// docs/213: one ventilation unit serving two rooms is compared with the sum of their required
// airflows (令 20 条の 8 第 1 項第 1 号イ（2））; an airflow given goes to every room of the unit.
TEST_F(Schedules, OneVentilationUnitForTwoRoomsTakesTheSum) {
  const std::string east = ok("create_room", {{"at", {6000, 3000}}, {"name", "洋室"}})["id"];
  const auto byName = [&](const std::string& name) {
    const json inputs = ok("room_opening_inputs");
    for (const json& r : inputs["rooms"])
      if (r["name"] == name) return r;
    ADD_FAILURE() << name;
    return json();
  };
  for (const std::string& id : {room, east}) {
    ok("set_room_daylight", {{"id", id}, {"kind", "dwelling"}, {"ventilation", "equipment"}});
    ok("set_room_finish", {{"id", id}, {"ceiling_height", 2400}});
  }
  ok("set_room_ventilation", {{"id", room}, {"fan_unit", "第3種換気 1"}});
  // One room of the name: compared on its own.
  json r = byName("居間");
  EXPECT_EQ(r["fan_unit"], "第3種換気 1");
  EXPECT_EQ(r["mechanical_comparison"]["state"], "未入力");
  EXPECT_EQ(r["mechanical_comparison"]["text"].get<std::string>().find("換気設備「"), std::string::npos);
  ok("set_room_ventilation", {{"id", east}, {"fan_unit", "第3種換気 1"}});
  r = byName("居間");
  const std::string text = r["mechanical_comparison"]["text"];
  EXPECT_EQ(r["mechanical_comparison"]["state"], "未入力");
  EXPECT_NE(text.find("換気設備「第3種換気 1」（居間・洋室）"), std::string::npos) << text;
  EXPECT_NE(text.find("合計 57.6 m³/h"), std::string::npos) << text;
  // The airflow given on one room goes to both, as one undo step.
  const auto steps = modeler.history().undoCount();
  ok("set_room_ventilation", {{"id", room}, {"fan_m3h", 60}});
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  EXPECT_EQ(byName("洋室")["fan_m3h"], 60.0);
  EXPECT_EQ(byName("居間")["mechanical_comparison"]["state"], "参考値内");
  EXPECT_EQ(byName("洋室")["mechanical_comparison"]["state"], "参考値内");
  EXPECT_NE(byName("洋室")["mechanical_comparison"]["text"].get<std::string>().find("イ（2）"), std::string::npos);
  ok("set_room_ventilation", {{"id", east}, {"fan_m3h", 50}});
  EXPECT_EQ(byName("居間")["fan_m3h"], 50.0);
  EXPECT_EQ(byName("居間")["mechanical_comparison"]["state"], "参考値を外れる");  // 50 < 57.6
  // Different airflows on rooms of one unit: to look at.
  ok("set_room_ventilation", {{"id", east}, {"fan_unit", ""}});
  ok("set_room_ventilation", {{"id", east}, {"fan_m3h", 30}});
  EXPECT_EQ(byName("居間")["fan_m3h"], 50.0);  // not of the unit any more
  ok("set_room_ventilation", {{"id", east}, {"fan_unit", "第3種換気 1"}});
  EXPECT_EQ(byName("居間")["mechanical_comparison"]["state"], "要確認");
  // A room of the unit without a ceiling height: the sum cannot be worked out.
  ok("set_room_ventilation", {{"id", east}, {"fan_m3h", 50}});
  ok("set_room_finish", {{"id", east}, {"ceiling_height", 0}});
  r = byName("居間");
  EXPECT_EQ(r["mechanical_comparison"]["state"], "未評価");
  EXPECT_NE(r["mechanical_comparison"]["text"].get<std::string>().find("洋室 の必要有効換気量を出せません"), std::string::npos);
  EXPECT_FALSE(call("set_room_ventilation", {{"id", room}, {"fan_unit", std::string(200, 'x')}}).value("ok", true));
}

// docs/214: the project's items that are not in the model, given by the person: in the outline
// table, in stage 1, and the name in the title block; saved and undone with the model.
TEST_F(Schedules, ProjectOutlineItemsGivenByThePerson) {
  const auto rowOf = [&](const std::string& item) {
    const auto table = sbr::doc::scheduleTable(modeler.document(), 10);
    for (const auto& row : table.rows)
      if (row[0] == item) return row;
    ADD_FAILURE() << item;
    return std::vector<std::string>(3);
  };
  EXPECT_EQ(rowOf("建築主")[1], "未入力");
  EXPECT_EQ(rowOf("工事種別")[1], "未入力");
  json stage1 = ok("permit_checks", {{"stage", 1}})["rows"];
  EXPECT_EQ(stage1.back()["item"], "計画概要の項目");
  EXPECT_EQ(stage1.back()["state"], "未入力");
  const auto steps = modeler.history().undoCount();
  json given = ok("set_project_outline", {{"items", {{"project_name", "架空邸"}, {"client", "架空 太郎"}, {"work_kind", "new"},
                                                     {"fire_zone", "article22"}, {"main_use", "一戸建ての住宅"}, {"source", "架空の資料"}}}});
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  EXPECT_EQ(given["items"]["client"], "架空 太郎");
  EXPECT_EQ(rowOf("建築主")[1], "架空 太郎");
  EXPECT_EQ(rowOf("工事種別")[1], "新築");
  EXPECT_EQ(rowOf("防火地域")[1], "指定なし（法 22 条区域）");  // docs/231: the form's 防火地域 has no 法 22 条区域
  EXPECT_EQ(rowOf("主要用途")[1], "一戸建ての住宅");
  EXPECT_NE(rowOf("主要用途")[2].find("架空の資料"), std::string::npos);
  stage1 = ok("permit_checks", {{"stage", 1}})["rows"];
  EXPECT_EQ(stage1.back()["state"], "入力不足");
  EXPECT_NE(stage1.back()["details"].get<std::string>().find("地名地番"), std::string::npos);
  // The name goes to the title block when the set has none.
  const auto set = sbr::doc::drawingSet(modeler.document(), {});
  ASSERT_TRUE(set);
  bool named = false;
  for (const auto& item : set->sheets.front().items) named = named || item.text == "架空邸";
  EXPECT_TRUE(named);
  // Read back with labels; one item taken away, the others stay.
  json read = ok("project_outline");
  bool kind = false;
  for (const json& f : read["fields"])
    if (f["key"] == "work_kind") {
      kind = true;
      EXPECT_EQ(f["text"], "新築");
      EXPECT_EQ(f["choices"]["major_remodel"], "大規模の模様替");
    }
  EXPECT_TRUE(kind);
  ok("set_project_outline", {{"items", {{"client", ""}}}});
  EXPECT_EQ(rowOf("建築主")[1], "未入力");
  EXPECT_EQ(rowOf("物件名")[1], "架空邸");
  // Wrong keys and values change nothing.
  EXPECT_FALSE(call("set_project_outline", {{"items", {{"owner", "x"}}}}).value("ok", true));
  EXPECT_FALSE(call("set_project_outline", {{"items", {{"work_kind", "build"}}}}).value("ok", true));
  EXPECT_FALSE(call("set_project_outline", {{"items", {{"address", "a\nb"}}}}).value("ok", true));
  // Saved and read back; undone.
  sbr::doc::Document copy;
  ASSERT_TRUE(sbr::doc::deserializeDocument(sbr::doc::serializeDocument(modeler.document()), copy));
  EXPECT_EQ(sbr::doc::projectOutline(copy).at("main_use"), "一戸建ての住宅");
  ok("undo");
  ok("undo");
  EXPECT_TRUE(sbr::doc::projectOutline(modeler.document()).empty());
}

// docs/216: the insulation of the walls a plan cuts drawn as a zigzag along each, with a legend;
// a plan of it per storey in the drawing set when there is some.
TEST_F(Schedules, InsulationPlansMarkTheWallsWithInsulation) {
  auto names = [&] {
    const auto set = sbr::doc::drawingSet(modeler.document(), {});
    EXPECT_TRUE(set);
    return set ? set->names : std::vector<std::string>{};
  };
  const auto has = [](const std::vector<std::string>& list, const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
  };
  EXPECT_FALSE(has(names(), "断熱範囲図（1FL）"));
  EXPECT_TRUE(sbr::doc::Modeler::insulationRuns(modeler.document(), 1100).empty());
  ok("set_insulation", {{"ids", {south, east}}, {"material", "グラスウール 16K"}, {"thickness_mm", 100}, {"conductivity", 0.038},
                        {"placement", "fill"}, {"source", "架空のカタログ"}});
  const auto runs = sbr::doc::Modeler::insulationRuns(modeler.document(), 1100);
  ASSERT_EQ(runs.size(), 5u);  // broken at the south windows (2) and the east door, which the plan passes through
  for (const auto& run : runs) {
    EXPECT_EQ(run.text, "グラスウール 16K 100 mm（充填）");
    EXPECT_NEAR(run.thickness, 200.0, 1e-6);
    EXPECT_NEAR(run.a.Z(), 1100.0, 1e-6);
  }
  EXPECT_TRUE(sbr::doc::Modeler::insulationRuns(modeler.document(), 4100).empty());  // above the walls
  // The plan: zigzags along the two walls and the legend.
  sbr::draw::ViewSpec plain = sbr::draw::planView(1100.0);
  plain.depth = 1500.0;
  sbr::draw::ViewSpec marked = plain;
  marked.insulation = true;
  const auto before = sbr::draw::makeView(modeler.document(), plain);
  const auto after = sbr::draw::makeView(modeler.document(), marked);
  ASSERT_TRUE(before && after);
  const auto polylines = [](const sbr::draw::View& v) {
    return std::count_if(v.items.begin(), v.items.end(), [](const sbr::draw::Item& i) { return i.kind == sbr::draw::Item::Kind::Polyline; });
  };
  EXPECT_EQ(polylines(*after), polylines(*before) + 5);
  const auto& notes = after->calculationNotes;
  EXPECT_TRUE(has(notes, "波線：壁の断熱　グラスウール 16K 100 mm（充填）（2 枚）"));
  // The drawing set: one insulation plan for 1FL only (2FL has no walls with insulation).
  {
    const auto list = names();
    EXPECT_TRUE(has(list, "断熱範囲図（1FL）"));
    EXPECT_FALSE(has(list, "断熱範囲図（2FL）"));
  }
  // docs/221, docs/222: the floor of the storey above, with insulation, is drawn on 2FL's plan (its
  // own storey), not on 1FL's; 2FL gets an insulation sheet for it.
  const std::string slab = ok("create_slab", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"top", 3000}, {"thickness", 200}})["id"];
  ok("set_insulation", {{"ids", {slab}}, {"material", "押出法ポリスチレンフォーム"}, {"thickness_mm", 50}, {"resistance", 1.8},
                        {"source", "架空のカタログ"}});
  const auto again = sbr::draw::makeView(modeler.document(), marked);
  ASSERT_TRUE(again);
  for (const std::string& note : again->calculationNotes) EXPECT_EQ(note.find("押出法"), std::string::npos) << note;
  const auto list = names();
  EXPECT_TRUE(has(list, "断熱範囲図（1FL）"));
  EXPECT_TRUE(has(list, "断熱範囲図（2FL）"));
}

// docs/221, docs/222: floors and roofs with insulation as ranges on the insulation plans, as the
// examples for 省エネ drawings: each floor on its own storey's plan ("床"), the roof over the top
// storey on its plan and a lower roof (下屋) on the plan of the storey above it ("屋根"); the outline
// and a cross, with the part and the insulation written inside.
TEST_F(Schedules, InsulationAreasOnPlans) {
  const auto has = [](const std::vector<std::string>& list, const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
  };
  std::string ground;
  for (const auto& o : modeler.document().objects())
    if (o.attributes.count("kind") && o.attributes.at("kind") == "slab") ground = o.id;  // top 0
  ASSERT_FALSE(ground.empty());
  const std::string upper = ok("create_slab", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"top", 3000}, {"thickness", 200}})["id"];
  const std::string roof = ok("create_roof", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"kind", "gable"}, {"height", 5800}})["id"];
  EXPECT_TRUE(sbr::doc::Modeler::insulationAreas(modeler.document(), 1100).empty());  // nothing given yet
  ok("set_insulation", {{"ids", {ground, upper}}, {"material", "押出法ポリスチレンフォーム"}, {"thickness_mm", 50}, {"source", "架空のカタログ"}});
  ok("set_insulation", {{"ids", {roof}}, {"material", "フェノールフォーム"}, {"thickness_mm", 60}, {"placement", "outside"}, {"source", "架空のカタログ"}});
  const auto parts = [&](double z) {
    std::map<std::string, std::string> out;
    for (const auto& a : sbr::doc::Modeler::insulationAreas(modeler.document(), z)) {
      EXPECT_FALSE(a.outline.IsNull());
      out[a.part] = a.id;
    }
    return out;
  };
  // 1FL (cut 1100): its floor only; the roof bears above 2FL: not this storey's.
  const auto first = parts(1100);
  EXPECT_EQ(first.size(), 1u);
  EXPECT_EQ(first.count("床") ? first.at("床") : "", ground);
  // 2FL (cut 4100): its floor and the roof (no level above).
  const auto second = parts(4100);
  EXPECT_EQ(second.size(), 2u);
  EXPECT_EQ(second.count("床") ? second.at("床") : "", upper);
  EXPECT_EQ(second.count("屋根") ? second.at("屋根") : "", roof);
  // The plan: dashed outlines and labels, and the legend.
  sbr::draw::ViewSpec spec = sbr::draw::planView(4100.0);
  spec.depth = 1500.0;
  spec.insulation = true;
  const auto view = sbr::draw::makeView(modeler.document(), spec);
  ASSERT_TRUE(view);
  std::set<std::string> labels;
  int diagonals = 0;
  for (const auto& i : view->items) {
    if (i.kind == sbr::draw::Item::Kind::Text) labels.insert(i.text);
    // The crosses: lines corner to corner of the 8000 x 6000 rectangles (10000 long).
    if (i.kind == sbr::draw::Item::Kind::Line && std::abs(i.points[0].Distance(i.points[1]) - 10000.0) < 1.0) ++diagonals;
  }
  EXPECT_EQ(diagonals, 4);  // the floor's and the roof's
  EXPECT_TRUE(labels.count("床断熱"));  // docs/247: the part only; the insulation is in the legend
  EXPECT_TRUE(labels.count("屋根断熱"));
  EXPECT_TRUE(has(view->calculationNotes, "×印の範囲：この階の床の断熱　押出法ポリスチレンフォーム 50 mm（1 個）"));
  EXPECT_TRUE(has(view->calculationNotes, "×印の範囲：屋根の断熱　フェノールフォーム 60 mm（外張り）（1 個）"));
  // A lower roof (下屋) bearing at 2800 north of the house, under a storey whose walls stand on 2FL:
  // on 2FL's plan (seen from above), not on 1FL's.
  ok("create_wall", {{"points", {{0, 6000}, {8000, 6000}}}, {"thickness", 120}, {"height", 2700}, {"base", 3000}});
  const std::string lower = ok("create_roof", {{"outline", {{0, 6000}, {8000, 6000}, {8000, 8000}, {0, 8000}}}, {"kind", "shed"}, {"height", 2800}})["id"];
  ok("set_insulation", {{"ids", {lower}}, {"material", "フェノールフォーム"}, {"thickness_mm", 60}, {"placement", "outside"}, {"source", "架空のカタログ"}});
  std::set<std::string> roofsOnFirst, roofsOnSecond;
  for (const auto& a : sbr::doc::Modeler::insulationAreas(modeler.document(), 1100))
    if (a.part == "屋根") roofsOnFirst.insert(a.id);
  for (const auto& a : sbr::doc::Modeler::insulationAreas(modeler.document(), 4100))
    if (a.part == "屋根") roofsOnSecond.insert(a.id);
  EXPECT_TRUE(roofsOnFirst.empty());
  EXPECT_EQ(roofsOnSecond, (std::set<std::string>{roof, lower}));
}

// docs/217: sections with the insulation of the walls, floors and roofs they cut marked: up a wall's
// middle (broken at its openings), along a floor at half its depth; a sheet per section in the set.
TEST_F(Schedules, InsulationSectionsMarkWallsAndFloors) {
  const auto has = [](const std::vector<std::string>& list, const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
  };
  const auto polylines = [](const sbr::draw::View& v) {
    return std::count_if(v.items.begin(), v.items.end(), [](const sbr::draw::Item& i) { return i.kind == sbr::draw::Item::Kind::Polyline; });
  };
  // Marks added by `insulation` on a section through x, looking west.
  const auto marks = [&](double x, std::vector<std::string>* notes = nullptr) {
    sbr::draw::ViewSpec plain = sbr::draw::sectionView(gp_Pnt(x, 3000, 0), gp_Dir(-1, 0, 0));
    sbr::draw::ViewSpec marked = plain;
    marked.insulation = true;
    const auto before = sbr::draw::makeView(modeler.document(), plain);
    const auto after = sbr::draw::makeView(modeler.document(), marked);
    EXPECT_TRUE(before && after);
    if (!before || !after) return -1L;
    if (notes) *notes = after->calculationNotes;
    return static_cast<long>(polylines(*after) - polylines(*before));
  };
  std::vector<std::string> notes;
  EXPECT_EQ(marks(1000, &notes), 0);
  EXPECT_TRUE(has(notes, "この断面で切る壁・床・屋根に、断熱の入力はありません"));
  ok("set_insulation", {{"ids", {south}}, {"material", "グラスウール 16K"}, {"thickness_mm", 100}, {"placement", "fill"},
                        {"source", "架空のカタログ"}});
  const auto band = sbr::doc::Modeler::insulationBand(modeler.document(), *modeler.document().find(south));
  ASSERT_TRUE(band && band->middle);
  EXPECT_NEAR(band->thickness, 200.0, 1e-6);
  EXPECT_NEAR(std::abs(band->middle->second.Y()), 1.0, 1e-9);  // across the south wall
  // Clear of the windows: one mark up the whole wall.
  double windowX = -1;
  for (const auto& r : sbr::doc::openingSchedule(modeler.document()).rows)
    if (r.host == south && (windowX < 0 || r.center.X() < windowX)) windowX = r.center.X();
  ASSERT_GT(windowX, 1000.0 + 800.0);  // x = 1000 is clear of the first window (1600 wide)
  EXPECT_EQ(marks(1000, &notes), 1);
  EXPECT_TRUE(has(notes, "波線：壁の断熱　グラスウール 16K 100 mm（充填）（1 個）"));
  // Through a window: under its sill and over its head.
  EXPECT_EQ(marks(windowX), 2);
  // The floor: along it at half its depth.
  std::string slab;
  for (const auto& o : modeler.document().objects())
    if (o.attributes.count("kind") && o.attributes.at("kind") == "slab") slab = o.id;
  ASSERT_FALSE(slab.empty());
  ok("set_insulation", {{"ids", {slab}}, {"material", "押出法ポリスチレンフォーム"}, {"thickness_mm", 50}, {"source", "架空のカタログ"}});
  EXPECT_EQ(marks(1000, &notes), 2);
  EXPECT_TRUE(has(notes, "波線：床の断熱　押出法ポリスチレンフォーム 50 mm（1 個）"));
  {
    sbr::draw::ViewSpec spec = sbr::draw::sectionView(gp_Pnt(1000, 3000, 0), gp_Dir(-1, 0, 0));
    spec.insulation = true;
    const auto view = sbr::draw::makeView(modeler.document(), spec);
    ASSERT_TRUE(view);
    // The floor's mark lies at its middle height (top 0, 200 thick: -100), within its swing.
    bool atMiddle = false;
    for (const auto& i : view->items) {
      if (i.kind != sbr::draw::Item::Kind::Polyline || i.points.size() < 3) continue;
      const bool flat = std::abs(i.points.front().Y() - i.points.back().Y()) < 1e-6;
      if (flat && std::abs(i.points.front().Y() + 100.0) < 1e-6) atMiddle = true;
    }
    EXPECT_TRUE(atMiddle);
  }
  // The drawing set: an insulation section for each section that cuts something insulated.
  const auto set = sbr::doc::drawingSet(modeler.document(), {});
  ASSERT_TRUE(set);
  for (const char* label : {"A", "B"}) {
    const std::string l(label);
    if (has(set->names, l + "-" + l + " 断面図")) EXPECT_TRUE(has(set->names, "断熱範囲図（" + l + "-" + l + " 断面）")) << l;
  }
  EXPECT_TRUE(has(set->names, "断熱範囲図（B-B 断面）"));  // across x = 4000: the south wall and the floor
}

// docs/218: with a build-up, the insulation mark goes in the layer: a layer named as insulation,
// else for 充填 the core (柱・間柱); a floor type's layer named 断熱 likewise. Without one: the middle.
TEST_F(Schedules, InsulationMarksSitInTheirLayer) {
  const auto has = [](const std::vector<std::string>& list, const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
  };
  ok("set_wall_type", {{"ids", {south}}, {"type", "exterior_siding"}});
  ok("set_insulation", {{"ids", {south}}, {"material", "グラスウール 16K"}, {"thickness_mm", 100}, {"placement", "fill"},
                        {"source", "架空のカタログ"}});
  const double outer = modeler.document().worldBounds(*modeler.document().find(south)).CornerMin().Y();
  const auto runs = sbr::doc::Modeler::insulationRuns(modeler.document(), 1100);
  ASSERT_FALSE(runs.empty());
  for (const auto& run : runs) {
    EXPECT_EQ(run.place, "core");
    EXPECT_NEAR(run.thickness, 105.0, 1e-6);
    // Siding 16, battens 18, plywood 9 from the outer face, then the core: its middle 95.5 in.
    EXPECT_NEAR(run.a.Y() - outer, 16 + 18 + 9 + 105.0 / 2, 1e-6);
  }
  const auto wallBand = sbr::doc::Modeler::insulationBand(modeler.document(), *modeler.document().find(south));
  ASSERT_TRUE(wallBand && wallBand->middle);
  EXPECT_EQ(wallBand->place, "core");
  EXPECT_NEAR(wallBand->middle->first.Y() - outer, 95.5, 1e-6);
  // 外張り with no insulation layer in the build-up: the middle of the whole wall.
  ok("set_insulation", {{"ids", {south}}, {"material", "フェノールフォーム"}, {"thickness_mm", 25}, {"placement", "outside"},
                        {"source", "架空のカタログ"}});
  const auto outside = sbr::doc::Modeler::insulationRuns(modeler.document(), 1100);
  ASSERT_FALSE(outside.empty());
  EXPECT_EQ(outside.front().place, "middle");
  EXPECT_NEAR(outside.front().thickness, 160.5, 1e-6);
  // A floor type with a layer named 断熱: the mark at that layer's middle depth.
  std::string slab;
  for (const auto& o : modeler.document().objects())
    if (o.attributes.count("kind") && o.attributes.at("kind") == "slab") slab = o.id;
  ASSERT_FALSE(slab.empty());
  ok("define_type", {{"name", "F1"}, {"part", "floor"},
                     {"layers", {{{"name", "フローリング"}, {"thickness", 15}}, {{"name", "合板"}, {"thickness", 24}},
                                 {{"name", "断熱材"}, {"thickness", 50}}}}});
  ok("set_type_name", {{"ids", {slab}}, {"name", "F1"}});
  ok("set_insulation", {{"ids", {slab}}, {"material", "押出法ポリスチレンフォーム"}, {"thickness_mm", 50}, {"source", "架空のカタログ"}});
  const auto floorBand = sbr::doc::Modeler::insulationBand(modeler.document(), *modeler.document().find(slab));
  ASSERT_TRUE(floorBand);
  EXPECT_FALSE(floorBand->middle);
  EXPECT_EQ(floorBand->place, "layer");
  EXPECT_NEAR(floorBand->depth, 15 + 24 + 25.0, 1e-6);
  EXPECT_NEAR(floorBand->thickness, 50.0, 1e-6);
  // The section: the floor's mark at -64 (its top at 0), and the legend says where the marks are.
  sbr::draw::ViewSpec spec = sbr::draw::sectionView(gp_Pnt(1000, 3000, 0), gp_Dir(-1, 0, 0));
  spec.insulation = true;
  const auto view = sbr::draw::makeView(modeler.document(), spec);
  ASSERT_TRUE(view);
  bool atLayer = false;
  for (const auto& i : view->items) {
    if (i.kind != sbr::draw::Item::Kind::Polyline || i.points.size() < 3) continue;
    if (std::abs(i.points.front().Y() - i.points.back().Y()) < 1e-6 && std::abs(i.points.front().Y() + 64.0) < 1e-6) atLayer = true;
  }
  EXPECT_TRUE(atLayer);
  EXPECT_TRUE(has(view->calculationNotes, "仕様（層）に断熱の層がある物は、その層の中に描いています"));
  EXPECT_TRUE(has(view->calculationNotes, "仕様（層）で位置の分からない断熱は、厚さの中ほどに描いた印です（層の位置ではありません）"));
  EXPECT_TRUE(has(view->calculationNotes, "基準の値との比較はしていません"));
}

// docs/219: the insulation zigzags follow the scale they are laid out at: a peak every 1.2 mm on
// paper at least (1:100: 120), else by the band (200 thick: 100), as on the screen.
TEST_F(Schedules, InsulationZigzagsFollowTheScale) {
  ok("set_insulation", {{"ids", {south}}, {"material", "グラスウール 16K"}, {"thickness_mm", 100}, {"placement", "fill"},
                        {"source", "架空のカタログ"}});
  // The smallest step along the longest zigzag of the plan (each step goes half a peak along).
  const auto step = [&](double scale) {
    sbr::draw::ViewSpec spec = sbr::draw::planView(1100.0);
    spec.depth = 1500.0;
    spec.insulation = true;
    spec.paperScale = scale;
    const auto view = sbr::draw::makeView(modeler.document(), spec);
    EXPECT_TRUE(view);
    if (!view) return 0.0;
    const sbr::draw::Item* longest = nullptr;
    double best = 0.0;
    for (const auto& i : view->items) {
      if (i.kind != sbr::draw::Item::Kind::Polyline || i.points.size() < 3) continue;
      const double d = i.points.front().Distance(i.points.back());
      if (d > best) {
        best = d;
        longest = &i;
      }
    }
    if (!longest) return 0.0;
    return best / static_cast<double>(longest->points.size() - 1);
  };
  const double screen = step(0.0), at20 = step(20.0), at100 = step(100.0);
  EXPECT_LE(screen, 100.0 + 1e-6);
  EXPECT_GT(screen, 90.0);
  EXPECT_NEAR(at20, screen, 1e-9);  // 1.2 mm at 1:20 is 24: the band's 100 rules
  EXPECT_LE(at100, 120.0 + 1e-6);
  EXPECT_GT(at100, 110.0);
}

// docs/220: at a corner of two walls with insulation, the marks end where they meet (in the cores
// of a build-up), not at the walls' ends running through the outer layers.
TEST_F(Schedules, InsulationMarksMeetAtCorners) {
  ok("set_wall_type", {{"ids", {south, east, north, west}}, {"type", "exterior_siding"}});
  ok("set_insulation", {{"ids", {south, west}}, {"material", "グラスウール 16K"}, {"thickness_mm", 100}, {"placement", "fill"},
                        {"source", "架空のカタログ"}});
  const auto& d = modeler.document();
  const double southOuter = d.worldBounds(*d.find(south)).CornerMin().Y();
  const double westOuter = d.worldBounds(*d.find(west)).CornerMin().X();
  const double eastOuter = d.worldBounds(*d.find(east)).CornerMax().X();
  const double in = 16 + 18 + 9 + 105.0 / 2;  // the core's middle from the outer face
  double southWest = 1e9, southEast = -1e9, westSouth = 1e9;
  for (const auto& run : sbr::doc::Modeler::insulationRuns(d, 1100)) {
    if (run.wall == south) {
      southWest = std::min({southWest, run.a.X(), run.b.X()});
      southEast = std::max({southEast, run.a.X(), run.b.X()});
    }
    if (run.wall == west) westSouth = std::min({westSouth, run.a.Y(), run.b.Y()});
  }
  // The south-west corner: both marks end at the meeting of the two cores' middles.
  EXPECT_NEAR(southWest, westOuter + in, 1e-6);
  EXPECT_NEAR(westSouth, southOuter + in, 1e-6);
  // The south-east corner: the east wall has no insulation, so the south mark goes to its end
  // as before (past the east wall's core middle).
  EXPECT_GT(southEast, eastOuter - in);
}

// docs/223: the insulation sheets carry the insulation specification table (部位・仕様又は使用材料等・
// 厚さ・性能値・数) beside the drawing, as the examples of 省エネ drawings do.
TEST_F(Schedules, InsulationSheetsCarryTheSpecificationTable) {
  ok("set_insulation", {{"ids", {south, east}}, {"material", "グラスウール 16K"}, {"thickness_mm", 100}, {"conductivity", 0.038},
                        {"placement", "fill"}, {"source", "架空のカタログ"}});
  std::string ground;
  for (const auto& o : modeler.document().objects())
    if (o.attributes.count("kind") && o.attributes.at("kind") == "slab") ground = o.id;
  ok("set_insulation", {{"ids", {ground}}, {"material", "押出法ポリスチレンフォーム"}, {"thickness_mm", 50}, {"resistance", 1.8},
                        {"source", "架空のカタログ"}});
  const auto set = sbr::doc::drawingSet(modeler.document(), {});
  ASSERT_TRUE(set);
  const auto texts = [&](const std::string& name) {
    std::set<std::string> out;
    for (std::size_t i = 0; i < set->names.size(); ++i)
      if (set->names[i].find(name) == 0)
        for (const auto& item : set->sheets[i].items)
          if (item.kind == sbr::draw::Item::Kind::Text) out.insert(item.text);
    return out;
  };
  const auto plan = texts("断熱範囲図（1FL）");
  ASSERT_FALSE(plan.empty());
  EXPECT_TRUE(plan.count("断熱仕様表"));
  EXPECT_TRUE(plan.count("グラスウール 16K（充填）"));
  EXPECT_TRUE(plan.count("R=2.63"));  // 100 mm / 0.038
  EXPECT_TRUE(plan.count("押出法ポリスチレンフォーム"));
  EXPECT_TRUE(plan.count("R=1.80"));
  // An ordinary plan has no such table.
  EXPECT_FALSE(texts("1FL 平面図").count("断熱仕様表"));
}

// docs/224: the building's equipment given as a list (set_equipment / list_equipment), saved with the
// model and undone in one step; stage 9's 設備 row and the 機器表 on the insulation sheets.
TEST_F(Schedules, EquipmentForTheEnergyDocuments) {
  const auto row9 = [&](const std::string& item) {
    const json all = ok("permit_checks", {{"stage", 9}})["rows"];  // not a range over a temporary's member
    for (const json& row : all)
      if (row["item"] == item) return row;
    ADD_FAILURE() << "no row " << item;
    return json();
  };
  EXPECT_EQ(row9("設備")["state"], "未入力");
  const auto steps = modeler.history().undoCount();
  ok("set_equipment", {{"items", {{{"kind", "heating_cooling"}, {"name", "ルームエアコン"}, {"model", "AC-28xxx"},
                                   {"value", "エネルギー消費効率区分（い）"}, {"count", 2}, {"source", "架空のカタログ"}},
                                  {{"kind", "ventilation"}, {"name", "壁付式第三種換気設備"}, {"value", "比消費電力 0.3"}}}}});
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  const json listed = ok("list_equipment")["items"];
  ASSERT_EQ(listed.size(), 2u);
  EXPECT_EQ(listed[0]["label"], "暖冷房設備");
  EXPECT_EQ(listed[0]["count"], 2);
  EXPECT_EQ(listed[1]["count"], 1);
  // Two of the four kinds: still 未入力, naming what is missing.
  const json partial = row9("設備");
  EXPECT_EQ(partial["state"], "未入力");
  EXPECT_NE(partial["details"].get<std::string>().find("給湯設備・照明設備"), std::string::npos);
  // Wrong input changes nothing.
  EXPECT_FALSE(call("set_equipment", {{"items", {{{"kind", "boiler"}, {"name", "x"}}}}}).value("ok", true));
  EXPECT_FALSE(call("set_equipment", {{"items", {{{"kind", "lighting"}, {"name", ""}}}}}).value("ok", true));
  EXPECT_FALSE(call("set_equipment", {{"items", {{{"kind", "lighting"}, {"name", "a\nb"}}}}}).value("ok", true));
  EXPECT_FALSE(call("set_equipment", {{"items", {{{"kind", "lighting"}, {"name", "LED"}, {"count", 0}}}}}).value("ok", true));
  EXPECT_EQ(ok("list_equipment")["items"].size(), 2u);
  // All four kinds: 入力済み.
  ok("set_equipment", {{"items", {{{"kind", "heating_cooling"}, {"name", "ルームエアコン"}},
                                  {{"kind", "ventilation"}, {"name", "壁付式第三種換気設備"}},
                                  {{"kind", "hot_water"}, {"name", "電気ヒートポンプ給湯機"}, {"value", "JIS効率 3.8"}},
                                  {{"kind", "lighting"}, {"name", "LED ダウンライト"}}}}});
  EXPECT_EQ(row9("設備")["state"], "入力済み");
  // Saved and read back.
  sbr::doc::Document copy;
  ASSERT_TRUE(sbr::doc::deserializeDocument(sbr::doc::serializeDocument(modeler.document()), copy));
  const auto read = sbr::doc::Modeler::equipmentOf(copy);
  ASSERT_EQ(read.size(), 4u);
  EXPECT_EQ(read[2].value, "JIS効率 3.8");
  // No insulation: the 機器表 is a sheet of its own; with insulation it goes on the insulation sheets.
  const auto sheetTexts = [&](const std::string& name) {
    const auto set = sbr::doc::drawingSet(modeler.document(), {});
    std::set<std::string> out;
    if (!set) return out;
    for (std::size_t i = 0; i < set->names.size(); ++i)
      if (set->names[i].find(name) == 0)
        for (const auto& item : set->sheets[i].items)
          if (item.kind == sbr::draw::Item::Kind::Text) out.insert(item.text);
    return out;
  };
  EXPECT_TRUE(sheetTexts("機器表").count("電気ヒートポンプ給湯機"));
  ok("set_insulation", {{"ids", {south}}, {"material", "グラスウール 16K"}, {"thickness_mm", 100}, {"source", "架空のカタログ"}});
  const auto onPlan = sheetTexts("断熱範囲図（1FL）");
  EXPECT_TRUE(onPlan.count("機器表"));
  EXPECT_TRUE(onPlan.count("JIS効率 3.8"));
  EXPECT_TRUE(sheetTexts("機器表").empty());  // no sheet of its own any more
  // Undone back to none.
  ok("undo");
  ok("undo");
  ok("undo");
  EXPECT_TRUE(sbr::doc::Modeler::equipmentOf(modeler.document()).empty());
}

// docs/225: with performance given to the outer openings, the insulation sheets carry 開口部仕様表
// (部位・符号・仕様・性能値 U・数・資料); without any, no such table.
TEST_F(Schedules, OpeningSpecificationTableOnTheInsulationSheets) {
  ok("set_insulation", {{"ids", {south}}, {"material", "グラスウール 16K"}, {"thickness_mm", 100}, {"source", "架空のカタログ"}});
  const auto texts = [&]() {
    const auto set = sbr::doc::drawingSet(modeler.document(), {});
    std::vector<std::string> out;
    if (!set) return out;
    for (std::size_t i = 0; i < set->names.size(); ++i)
      if (set->names[i] == "断熱範囲図（1FL）")
        for (const auto& item : set->sheets[i].items)
          if (item.kind == sbr::draw::Item::Kind::Text) out.push_back(item.text);
    return out;
  };
  const auto count = [](const std::vector<std::string>& list, const std::string& text) {
    return std::count(list.begin(), list.end(), text);
  };
  EXPECT_EQ(count(texts(), "開口部仕様表"), 0);
  ok("set_opening_performance", {{"host", south}, {"u_value", 2.33}, {"glass", "Low-E 複層"}, {"source", "架空のカタログ"}});
  const auto after = texts();
  EXPECT_EQ(count(after, "開口部仕様表"), 1);
  EXPECT_EQ(count(after, "U=2.33"), 1);  // the two south windows share a mark: one row
  EXPECT_EQ(count(after, "2"), 1);
  EXPECT_GE(count(after, "未入力"), 1);  // the north window and the east door have none
  EXPECT_GE(count(after, "ドア"), 1);
}

// docs/227: an opening's 日射熱取得率 η given with its U: in the opening specification table and the
// energy specification; out of 0-1 refused; left out, as before.
TEST_F(Schedules, OpeningSolarGainBesideItsU) {
  ok("set_insulation", {{"ids", {south}}, {"material", "グラスウール 16K"}, {"thickness_mm", 100}, {"source", "架空のカタログ"}});
  EXPECT_FALSE(call("set_opening_performance", {{"host", south}, {"u_value", 2.33}, {"solar_gain", 1.5}, {"source", "架空"}}).value("ok", true));
  ok("set_opening_performance", {{"host", south}, {"u_value", 2.33}, {"solar_gain", 0.51}, {"glass", "Low-E 複層"}, {"source", "架空のカタログ"}});
  for (const auto& r : sbr::doc::openingSchedule(modeler.document()).rows)
    if (r.host == south) EXPECT_NEAR(r.solarGain, 0.51, 1e-9);
  const auto set = sbr::doc::drawingSet(modeler.document(), {});
  ASSERT_TRUE(set);
  bool inTable = false;
  for (std::size_t i = 0; i < set->names.size(); ++i)
    if (set->names[i] == "断熱範囲図（1FL）")
      for (const auto& item : set->sheets[i].items)
        if (item.kind == sbr::draw::Item::Kind::Text && item.text == "U=2.33　η=0.51") inTable = true;
  EXPECT_TRUE(inTable);
  const auto energy = sbr::doc::scheduleTable(modeler.document(), 11);
  bool inEnergy = false;
  for (const auto& row : energy.rows)
    for (const auto& cell : row) inEnergy = inEnergy || cell.find("日射熱取得率 0.51") != std::string::npos;
  EXPECT_TRUE(inEnergy);
  // Given again without η: it is gone.
  ok("set_opening_performance", {{"host", south}, {"u_value", 2.33}, {"source", "架空のカタログ"}});
  for (const auto& r : sbr::doc::openingSchedule(modeler.document()).rows)
    if (r.host == south) EXPECT_LT(r.solarGain, 0.0);
}

// docs/228: the insulation specification table compares each insulation with the 仕様基準's thermal
// resistance for its part, method and region (木造軸組, 省エネ基準, for reference): 地域未入力 /
// 工法未入力 / 満たす / 不足 / 基準なし.
TEST_F(Schedules, InsulationComparedWithTheSpecificationValues) {
  const auto texts = [&]() {
    const auto set = sbr::doc::drawingSet(modeler.document(), {});
    std::vector<std::string> out;
    if (!set) return out;
    for (std::size_t i = 0; i < set->names.size(); ++i)
      if (set->names[i] == "断熱範囲図（1FL）")
        for (const auto& item : set->sheets[i].items)
          if (item.kind == sbr::draw::Item::Kind::Text) out.push_back(item.text);
    return out;
  };
  const auto has = [](const std::vector<std::string>& list, const std::string& text) {
    return std::find(list.begin(), list.end(), text) != list.end();
  };
  // Walls: glass wool 100 mm, λ 0.038 (R 2.63), filled. The ground floor: R 1.8, filled.
  ok("set_insulation", {{"ids", {south}}, {"material", "グラスウール 16K"}, {"thickness_mm", 100}, {"conductivity", 0.038},
                        {"placement", "fill"}, {"source", "架空のカタログ"}});
  std::string ground;
  for (const auto& o : modeler.document().objects())
    if (o.attributes.count("kind") && o.attributes.at("kind") == "slab") ground = o.id;
  ok("set_insulation", {{"ids", {ground}}, {"material", "押出法ポリスチレンフォーム"}, {"thickness_mm", 50}, {"resistance", 1.8},
                        {"placement", "fill"}, {"source", "架空のカタログ"}});
  EXPECT_TRUE(has(texts(), "地域未入力"));
  const std::string site = ok("create_polygon", {{"points", {{-2000, -2000, 0}, {14000, -2000, 0}, {14000, 10000, 0}, {-2000, 10000, 0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  ok("set_energy_route", {{"route", "spec"}, {"region", 6}, {"source", "架空の案内"}});
  auto now = texts();
  EXPECT_TRUE(has(now, "R≧2.2"));   // walls, regions 4-7, filled
  EXPECT_TRUE(has(now, "満たす"));  // 2.63
  EXPECT_TRUE(has(now, "不足"));    // the floor's 1.8 < 2.2
  // Region 2: walls need 3.3 (not met), the floor 3.3.
  ok("set_energy_route", {{"route", "spec"}, {"region", 2}, {"source", "架空の案内"}});
  now = texts();
  EXPECT_TRUE(has(now, "R≧3.3"));
  EXPECT_FALSE(has(now, "満たす"));
  // Region 8: walls and floors have no value.
  ok("set_energy_route", {{"route", "spec"}, {"region", 8}, {"source", "架空の案内"}});
  now = texts();
  EXPECT_TRUE(has(now, "基準なし"));
  EXPECT_TRUE(has(now, "対象外"));
  // A method not given: not compared.
  ok("set_insulation", {{"ids", {south}}, {"material", "グラスウール 16K"}, {"thickness_mm", 100}, {"conductivity", 0.038},
                        {"source", "架空のカタログ"}});
  EXPECT_TRUE(has(texts(), "工法未入力"));
}

// docs/229: the opening specification table compares windows with the 仕様基準's heat transmittance
// and (regions 5-8) solar gain, for reference; doors are not looked at.
TEST_F(Schedules, OpeningsComparedWithTheSpecificationValues) {
  const auto texts = [&]() {
    const auto set = sbr::doc::drawingSet(modeler.document(), {});
    std::vector<std::string> out;
    if (!set) return out;
    for (std::size_t i = 0; i < set->names.size(); ++i)
      if (set->names[i] == "断熱範囲図（1FL）")
        for (const auto& item : set->sheets[i].items)
          if (item.kind == sbr::draw::Item::Kind::Text) out.push_back(item.text);
    return out;
  };
  const auto has = [](const std::vector<std::string>& list, const std::string& text) {
    return std::find(list.begin(), list.end(), text) != list.end();
  };
  ok("set_insulation", {{"ids", {south}}, {"material", "グラスウール 16K"}, {"thickness_mm", 100}, {"source", "架空のカタログ"}});
  ok("set_opening_performance", {{"host", south}, {"u_value", 2.33}, {"source", "架空のカタログ"}});
  ok("set_opening_performance", {{"host", east}, {"u_value", 2.9}, {"source", "架空のカタログ"}});
  EXPECT_TRUE(has(texts(), "地域未入力"));
  const std::string site = ok("create_polygon", {{"points", {{-2000, -2000, 0}, {14000, -2000, 0}, {14000, 10000, 0}, {-2000, 10000, 0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  ok("set_energy_route", {{"route", "spec"}, {"region", 6}, {"source", "架空の案内"}});
  auto now = texts();
  EXPECT_TRUE(has(now, "U≦4.7　η≦0.59"));
  EXPECT_TRUE(has(now, "U 満たす・η 未入力"));
  EXPECT_TRUE(has(now, "U≦4.7"));     // docs/230: the east door, by its U only
  EXPECT_TRUE(has(now, "U 満たす"));  // 2.9
  ok("set_opening_performance", {{"host", south}, {"u_value", 2.33}, {"solar_gain", 0.51}, {"source", "架空のカタログ"}});
  EXPECT_TRUE(has(texts(), "U 満たす・η 満たす"));
  ok("set_opening_performance", {{"host", south}, {"u_value", 2.33}, {"solar_gain", 0.7}, {"source", "架空のカタログ"}});
  EXPECT_TRUE(has(texts(), "U 満たす・η 他の方法で"));
  // Region 2: U at most 2.3 (2.33 does not meet it); no solar gain asked.
  ok("set_energy_route", {{"route", "spec"}, {"region", 2}, {"source", "架空の案内"}});
  now = texts();
  EXPECT_TRUE(has(now, "U≦2.3"));
  EXPECT_TRUE(has(now, "U 不足"));
  // docs/230 (告示第266号 1(3)イ itself): region 3 is with 1-2 (2.3), region 4 is 3.5.
  ok("set_energy_route", {{"route", "spec"}, {"region", 3}, {"source", "架空の案内"}});
  EXPECT_TRUE(has(texts(), "U≦2.3"));
  ok("set_energy_route", {{"route", "spec"}, {"region", 4}, {"source", "架空の案内"}});
  now = texts();
  EXPECT_TRUE(has(now, "U≦3.5"));
  EXPECT_FALSE(has(now, "η≦0.59"));  // no solar gain asked in region 4
  // Region 8: no U, η at most 0.53.
  ok("set_energy_route", {{"route", "spec"}, {"region", 8}, {"source", "架空の案内"}});
  now = texts();
  EXPECT_TRUE(has(now, "η≦0.53"));
  EXPECT_TRUE(has(now, "η 他の方法で"));
}

// docs/231: the outline items follow the application form (第二号様式 第二面・第三面): 工事監理者, 構造,
// 都市計画区域 as the form's choices; optional items (代理者・工事施工者・住居表示・その他の区域・用途の区分・
// 許可等) are not counted missing.
TEST_F(Schedules, ProjectOutlineFollowsTheApplicationForm) {
  const auto stage1 = [&]() {
    const json rows = ok("permit_checks", {{"stage", 1}})["rows"];
    return rows.back();
  };
  EXPECT_FALSE(call("set_project_outline", {{"items", {{"city_planning", "市街化区域"}}}}).value("ok", true));  // not a choice
  ok("set_project_outline", {{"items", {{"project_name", "架空邸"}, {"client", "架空 太郎"}, {"designer", "一級建築士 架空 次郎"},
                                        {"address", "架空市 1-2"}, {"city_planning", "urbanization"}, {"fire_zone", "quasi_fire"},
                                        {"main_use", "一戸建ての住宅"}, {"work_kind", "new"}, {"start_date", "2027-04-01"},
                                        {"finish_date", "2027-10-31"}, {"source", "架空の資料"}}}});
  json row = stage1();
  EXPECT_EQ(row["state"], "入力不足");
  const std::string details = row["details"];
  EXPECT_NE(details.find("工事監理者"), std::string::npos);
  EXPECT_NE(details.find("構造"), std::string::npos);
  EXPECT_EQ(details.find("代理者、"), std::string::npos);  // optional: not listed as missing
  ok("set_project_outline", {{"items", {{"supervisor", "未定"}, {"structure", "木造"}}}});
  EXPECT_EQ(stage1()["state"], "入力不足");  // docs/232: 第二面 7・8 still to choose
  EXPECT_NE(stage1()["details"].get<std::string>().find("構造計算適合性判定の申請"), std::string::npos);
  ok("set_project_outline", {{"items", {{"structural_review", "not_required"}, {"energy_plan", "not_yet"}}}});
  EXPECT_EQ(stage1()["state"], "入力済み");
  // docs/232: stage 9 shows the energy plan's state; 未提出 asks for attention.
  {
    const json rows = ok("permit_checks", {{"stage", 9}})["rows"];
    bool found = false;
    for (const json& r : rows)
      if (r["item"] == "省エネ適判（確保計画の提出）") {
        found = true;
        EXPECT_EQ(r["state"], "入力済み");
        EXPECT_NE(r["details"].get<std::string>().find("未提出"), std::string::npos);
        EXPECT_TRUE(r["attention"].get<bool>());
      }
    EXPECT_TRUE(found);
  }
  EXPECT_FALSE(call("set_project_outline", {{"items", {{"energy_plan", "maybe"}}}}).value("ok", true));
  ok("set_project_outline", {{"items", {{"specific_process", "（第1回）2027-06-30 屋根工事"}}}});
  // The outline table: the form's labels and the choice's text.
  const auto outline = sbr::doc::scheduleTable(modeler.document(), 10);
  std::map<std::string, std::string> cells;
  for (const auto& r : outline.rows) cells[r[0]] = r[1];
  EXPECT_EQ(cells["都市計画区域等"], "都市計画区域内（市街化区域）");
  EXPECT_EQ(cells["防火地域"], "準防火地域");
  EXPECT_EQ(cells["工事監理者"], "未定");
  EXPECT_EQ(cells["構造"], "木造");
  EXPECT_EQ(cells["構造計算適合性判定の申請"], "申請不要");  // docs/232
  EXPECT_EQ(cells["建築物エネルギー消費性能確保計画の提出"], "未提出");
  EXPECT_EQ(cells["特定工程"], "（第1回）2027-06-30 屋根工事");
  EXPECT_EQ(cells["工事施工者"], "未入力");
  EXPECT_EQ(cells.count("代理者"), 0u);  // only when there is one
  ok("set_project_outline", {{"items", {{"agent", "一級建築士 架空 三郎"}}}});
  const auto again = sbr::doc::scheduleTable(modeler.document(), 10);
  bool agent = false;
  for (const auto& r : again.rows) agent = agent || (r[0] == "代理者" && r[1] == "一級建築士 架空 三郎");
  EXPECT_TRUE(agent);
}

// docs/233: the vicinity map: its position as a setting (one undo step, saved), the tiles around it
// (国土地理院's URLs, the site's pixel in their picture), and the sheet after the outline: the picture
// given by the app (or a frame saying where the map goes), the site's ring, the 出典 lines.
TEST_F(Schedules, VicinityMapSheet) {
  EXPECT_FALSE(call("set_vicinity_map", {{"lat", 10.0}, {"lon", 139.7}}).value("ok", true));   // not Japan
  EXPECT_FALSE(call("set_vicinity_map", {{"lat", 35.68}, {"lon", 139.77}, {"zoom", 20}}).value("ok", true));
  EXPECT_FALSE(call("set_vicinity_map", {{"lat", 35.68}, {"lon", 139.77}, {"layer", "osm"}}).value("ok", true));
  const auto steps = modeler.history().undoCount();
  const json made = ok("set_vicinity_map", {{"lat", 35.681236}, {"lon", 139.767125}, {"zoom", 16}, {"layer", "pale"}});
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  ASSERT_EQ(made["tiles"].size(), 9u);
  // Web Mercator: x = floor((lon + 180) / 360 * 2^16) = floor(58211.8) = 58211, the middle of the 3 x 3.
  EXPECT_EQ(made["tiles"][4].get<std::string>(), "https://cyberjapandata.gsi.go.jp/xyz/pale/16/58211/25806.png");
  const auto map = sbr::doc::vicinityMapOf(modeler.document());
  ASSERT_TRUE(map);
  const auto tiles = sbr::doc::vicinityTiles(*map);
  EXPECT_GE(tiles.siteX, 256.0);  // in the middle tile
  EXPECT_LT(tiles.siteX, 512.0);
  EXPECT_GE(tiles.siteY, 256.0);
  EXPECT_LT(tiles.siteY, 512.0);
  EXPECT_NEAR(tiles.metresPerPixel, 1.94, 0.02);  // 2.39 m x cos(35.68 deg) at zoom 16
  // Saved and read back.
  sbr::doc::Document copy;
  ASSERT_TRUE(sbr::doc::deserializeDocument(sbr::doc::serializeDocument(modeler.document()), copy));
  ASSERT_TRUE(sbr::doc::vicinityMapOf(copy));
  EXPECT_EQ(sbr::doc::vicinityMapOf(copy)->layer, "pale");
  const auto texts = [](const sbr::draw::Sheet& sheet) {
    std::vector<std::string> out;
    for (const auto& item : sheet.items)
      if (item.kind == sbr::draw::Item::Kind::Text) out.push_back(item.text);
    return out;
  };
  const auto contains = [](const std::vector<std::string>& list, const std::string& part) {
    return std::any_of(list.begin(), list.end(), [&](const std::string& t) { return t.find(part) != std::string::npos; });
  };
  // No picture (as from the command line): the sheet says where the map goes.
  {
    const auto set = sbr::doc::drawingSet(modeler.document(), {});
    ASSERT_TRUE(set);
    ASSERT_GE(set->names.size(), 3u);
    EXPECT_EQ(set->names[2], "付近見取図");  // after the list and the outline
    const auto t = texts(set->sheets[2]);
    EXPECT_TRUE(contains(t, "地図は、画面の「図面一式を書き出す」で"));
    EXPECT_TRUE(contains(t, "出典：国土地理院「地理院タイル」（淡色地図）"));
    EXPECT_TRUE(contains(t, "基本測量成果"));
  }
  // A picture given: an image item over the picture's ground size; the scale fits it.
  sbr::doc::DrawingSetSpec spec;
  sbr::doc::VicinityPicture picture;
  picture.width = 768;
  picture.height = 768;
  picture.rgba = std::make_shared<const std::vector<unsigned char>>(768 * 768 * 4, static_cast<unsigned char>(200));
  picture.siteX = tiles.siteX;
  picture.siteY = tiles.siteY;
  picture.metresPerPixel = tiles.metresPerPixel;
  picture.missingTiles = 1;
  spec.vicinity = picture;
  const auto set = sbr::doc::drawingSet(modeler.document(), spec);
  ASSERT_TRUE(set);
  const auto& sheet = set->sheets[2];
  const auto image = std::find_if(sheet.items.begin(), sheet.items.end(),
                                  [](const sbr::draw::Item& i) { return i.kind == sbr::draw::Item::Kind::Image; });
  ASSERT_NE(image, sheet.items.end());
  EXPECT_EQ(image->imageWidth, 768);
  // 768 px x 1.94 m = 1.49 km each way: 149 mm at 1:10000 is over 140 mm high, so 1:15000 (99 mm).
  EXPECT_EQ(sheet.scale, 15000.0);
  const double paperWidth = (image->points[1].X() - image->points[0].X()) / sheet.scale;
  EXPECT_NEAR(paperWidth, 768 * tiles.metresPerPixel * 1000 / 15000, 0.5);
  const auto t = texts(sheet);
  EXPECT_FALSE(contains(t, "地図は、画面の"));
  EXPECT_TRUE(contains(t, "取れなかった地図タイル 1 枚"));
  EXPECT_TRUE(contains(t, "敷地"));
  // Taken away: no sheet.
  ok("set_vicinity_map", {{"clear", true}});
  const auto without = sbr::doc::drawingSet(modeler.document(), {});
  ASSERT_TRUE(without);
  EXPECT_EQ(std::count(without->names.begin(), without->names.end(), "付近見取図"), 0);
}

// docs/233: the vicinity map's north mark is true north (no 「仮」).
TEST_F(Schedules, VicinityMapNorthIsTrueNorth) {
  ok("set_vicinity_map", {{"lat", 35.0}, {"lon", 135.0}});
  const auto set = sbr::doc::drawingSet(modeler.document(), {});
  ASSERT_TRUE(set);
  const auto at = std::find(set->names.begin(), set->names.end(), "付近見取図");
  ASSERT_NE(at, set->names.end());
  for (const auto& item : set->sheets[static_cast<std::size_t>(at - set->names.begin())].items)
    EXPECT_NE(item.text, "仮");
}

// docs/234: with a site boundary and a true north, the vicinity map shows the boundary turned to north
// (its centre on the map's) when it is 5 mm or more across on paper; else the ring and a note.
TEST_F(Schedules, VicinityMapShowsTheSiteOutline) {
  const std::string site = ok("create_polygon", {{"points", {{0, 0, 0}, {20000, 0, 0}, {20000, 15000, 0}, {0, 15000, 0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  const auto sheetOf = [&]() {
    const auto set = sbr::doc::drawingSet(modeler.document(), {});
    EXPECT_TRUE(set);
    const auto at = std::find(set->names.begin(), set->names.end(), "付近見取図");
    EXPECT_NE(at, set->names.end());
    return set->sheets[static_cast<std::size_t>(at - set->names.begin())];
  };
  const auto noteHas = [](const sbr::draw::Sheet& sheet, const std::string& part) {
    return std::any_of(sheet.items.begin(), sheet.items.end(), [&](const sbr::draw::Item& i) {
      return i.kind == sbr::draw::Item::Kind::Text && i.text.find(part) != std::string::npos;
    });
  };
  // No true north: the ring and a note.
  ok("set_vicinity_map", {{"lat", 35.0}, {"lon", 135.0}, {"zoom", 18}});
  EXPECT_TRUE(noteHas(sheetOf(), "真北が入っていないので"));
  // True north along model +X (90 degrees clockwise from +Y): 20 m north-south, 15 m east-west.
  ASSERT_TRUE(modeler.setSiteNorth({90.0, "架空の測量図", "2026-11-21"}));
  auto sheet = sheetOf();
  EXPECT_EQ(sheet.scale, 3000.0);  // zoom 18: 768 px x 0.49 m = 375 m, 125 mm at 1:3000
  EXPECT_TRUE(noteHas(sheet, "太線：敷地境界"));
  const auto outline = std::find_if(sheet.items.begin(), sheet.items.end(), [](const sbr::draw::Item& i) {
    return i.kind == sbr::draw::Item::Kind::Polyline && i.pen == sbr::draw::Pen::Cut && i.closed && i.points.size() == 4;
  });
  ASSERT_NE(outline, sheet.items.end());
  double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
  for (const auto& p : outline->points) {
    u0 = std::min(u0, p.X());
    u1 = std::max(u1, p.X());
    v0 = std::min(v0, p.Y());
    v1 = std::max(v1, p.Y());
  }
  EXPECT_NEAR((u1 - u0) / sheet.scale, 15000.0 / 3000.0, 0.01);  // east-west 15 m: 5 mm
  EXPECT_NEAR((v1 - v0) / sheet.scale, 20000.0 / 3000.0, 0.01);  // north-south 20 m: 6.7 mm
  // Zoom 16 (1:15000): too small on paper, the ring.
  ok("set_vicinity_map", {{"lat", 35.0}, {"lon", 135.0}, {"zoom", 16}});
  EXPECT_TRUE(noteHas(sheetOf(), "5 mm に満たない"));
}

// docs/235: landmarks on the vicinity map: placed at their latitude / longitude from the site (0.001
// degree north is about 111 m), drawn with their name; those off the map counted in the notes; bad
// ones refused.
TEST_F(Schedules, VicinityMapLandmarks) {
  EXPECT_FALSE(call("set_vicinity_map", {{"lat", 35.0}, {"lon", 135.0}, {"marks", {{{"name", ""}, {"lat", 35.0}, {"lon", 135.0}}}}})
                   .value("ok", true));
  EXPECT_FALSE(call("set_vicinity_map", {{"lat", 35.0}, {"lon", 135.0}, {"marks", {{{"name", "x"}, {"lat", 5.0}, {"lon", 135.0}}}}})
                   .value("ok", true));
  ok("set_vicinity_map", {{"lat", 35.0}, {"lon", 135.0}, {"zoom", 16},
                          {"marks", {{{"name", "架空駅"}, {"lat", 35.001}, {"lon", 135.0}}, {{"name", "遠い所"}, {"lat", 35.2}, {"lon", 135.2}}}}});
  const auto map = sbr::doc::vicinityMapOf(modeler.document());
  ASSERT_TRUE(map);
  ASSERT_EQ(map->marks.size(), 2u);
  const auto [e, n] = sbr::doc::vicinityOffset(*map, 35.001, 135.0);
  EXPECT_NEAR(e, 0.0, 1.0);
  EXPECT_NEAR(n, 111000.0, 1500.0);
  const auto set = sbr::doc::drawingSet(modeler.document(), {});
  ASSERT_TRUE(set);
  const auto at = std::find(set->names.begin(), set->names.end(), "付近見取図");
  ASSERT_NE(at, set->names.end());
  const auto& sheet = set->sheets[static_cast<std::size_t>(at - set->names.begin())];
  bool name = false, far = false, note = false;
  for (const auto& item : sheet.items) {
    if (item.kind != sbr::draw::Item::Kind::Text) continue;
    name = name || item.text == "架空駅";
    far = far || item.text == "遠い所";
    note = note || item.text.find("地図の外の 1 個") != std::string::npos;
  }
  EXPECT_TRUE(name);
  EXPECT_FALSE(far);
  EXPECT_TRUE(note);
}

// docs/236: insulation in a ceiling (天井断熱): given like the others; a range on its storey's insulation
// plan, a mark in sections, a 天井 row in the insulation table compared with the 天井 value (告示 1(2)ロ:
// 充填 5.7 / 4.0 / 4.0 / 4.0 / 0.78).
TEST_F(Schedules, CeilingInsulation) {
  ok("set_room_finish", {{"id", room}, {"ceiling_height", 2400}});
  const json made = ok("make_ceilings", json::object());
  ASSERT_FALSE(made["ids"].empty());
  const std::string ceiling = made["ids"][0];
  ok("set_insulation", {{"ids", {ceiling}}, {"material", "グラスウール 16K"}, {"thickness_mm", 155}, {"conductivity", 0.038},
                        {"placement", "fill"}, {"source", "架空のカタログ"}});
  // On 1FL's plan (cut 1100, the ceiling at 2400 under 2FL), not on 2FL's.
  bool onFirst = false, onSecond = false;
  for (const auto& a : sbr::doc::Modeler::insulationAreas(modeler.document(), 1100)) onFirst = onFirst || (a.part == "天井" && a.id == ceiling);
  for (const auto& a : sbr::doc::Modeler::insulationAreas(modeler.document(), 4100)) onSecond = onSecond || a.part == "天井";
  EXPECT_TRUE(onFirst);
  EXPECT_FALSE(onSecond);
  sbr::draw::ViewSpec plan = sbr::draw::planView(1100.0);
  plan.depth = 1500.0;
  plan.insulation = true;
  const auto view = sbr::draw::makeView(modeler.document(), plan);
  ASSERT_TRUE(view);
  const auto& notes = view->calculationNotes;
  EXPECT_NE(std::find(notes.begin(), notes.end(), "×印の範囲：この階の天井の断熱　グラスウール 16K 155 mm（充填）（1 個）"),
            notes.end())
      << (notes.empty() ? "" : notes.front());
  // A section through it.
  sbr::draw::ViewSpec section = sbr::draw::sectionView(gp_Pnt(1000, 3000, 0), gp_Dir(-1, 0, 0));
  section.insulation = true;
  const auto cut = sbr::draw::makeView(modeler.document(), section);
  ASSERT_TRUE(cut);
  bool marked = false;
  for (const auto& n : cut->calculationNotes) marked = marked || n.find("波線：天井の断熱") != std::string::npos;
  EXPECT_TRUE(marked);
  // The insulation table: 天井, R 4.08 against 4.0 in region 6.
  const std::string site = ok("create_polygon", {{"points", {{-2000, -2000, 0}, {14000, -2000, 0}, {14000, 10000, 0}, {-2000, 10000, 0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  ok("set_energy_route", {{"route", "spec"}, {"region", 6}, {"source", "架空の案内"}});
  const auto set = sbr::doc::drawingSet(modeler.document(), {});
  ASSERT_TRUE(set);
  std::vector<std::string> texts;
  for (std::size_t i = 0; i < set->names.size(); ++i)
    if (set->names[i] == "断熱範囲図（1FL）")
      for (const auto& item : set->sheets[i].items)
        if (item.kind == sbr::draw::Item::Kind::Text) texts.push_back(item.text);
  EXPECT_NE(std::find(texts.begin(), texts.end(), "天井"), texts.end());
  EXPECT_NE(std::find(texts.begin(), texts.end(), "R≧4.0"), texts.end());
  EXPECT_NE(std::find(texts.begin(), texts.end(), "満たす"), texts.end());
}

// docs/237: the opening specification table says whether the windows are few enough to be left out
// (告示 1(3)イ・ロ: at most 2 % / 4 % of the floor area): the outer walls' windows and plain openings
// counted, doors not.
TEST_F(Schedules, SmallWindowsExemption) {
  ok("set_insulation", {{"ids", {south}}, {"material", "グラスウール 16K"}, {"thickness_mm", 100}, {"source", "架空のカタログ"}});
  ok("set_opening_performance", {{"host", south}, {"u_value", 2.33}, {"source", "架空のカタログ"}});
  double windows = 0.0;
  for (const auto& r : sbr::doc::openingSchedule(modeler.document()).rows)
    if (!r.inner && r.kind != "door") windows += r.width * r.height;
  EXPECT_NEAR(windows / 1e6, 2 * 1.6 * 1.2 + 1.6 * 1.2 + 0.8 * 2.0, 1e-6);  // south x 2, north window, north plain opening
  const auto set = sbr::doc::drawingSet(modeler.document(), {});
  ASSERT_TRUE(set);
  std::string note;
  for (std::size_t i = 0; i < set->names.size(); ++i)
    if (set->names[i] == "断熱範囲図（1FL）")
      for (const auto& item : set->sheets[i].items)
        if (item.kind == sbr::draw::Item::Kind::Text && item.text.find("窓の面積の合計") != std::string::npos) note = item.text;
  ASSERT_FALSE(note.empty());
  EXPECT_NE(note.find("7.36 m2"), std::string::npos) << note;
  EXPECT_NE(note.find("2％を超えるので"), std::string::npos) << note;
  EXPECT_NE(note.find("4％を超えるので"), std::string::npos) << note;
}

// docs/238: the 仕様基準's values for equipment (告示第266号 2) under the 機器表, by the site's region.
TEST_F(Schedules, EquipmentSpecificationNotes) {
  ok("set_equipment", {{"items", {{{"kind", "heating_cooling"}, {"name", "ルームエアコン"}},
                                  {{"kind", "ventilation"}, {"name", "壁付式第三種換気設備"}},
                                  {{"kind", "hot_water"}, {"name", "電気ヒートポンプ給湯機"}},
                                  {{"kind", "lighting"}, {"name", "LED ダウンライト"}}}}});
  const auto notes = [&]() {
    std::string all;
    const auto set = sbr::doc::drawingSet(modeler.document(), {});
    if (!set) return all;
    for (std::size_t i = 0; i < set->names.size(); ++i)
      if (set->names[i].find("機器表") == 0)
        for (const auto& item : set->sheets[i].items)
          if (item.kind == sbr::draw::Item::Kind::Text) all += item.text + "\n";
    return all;
  };
  // No region yet: the regional values are not given, the others are.
  const std::string none = notes();
  EXPECT_NE(none.find("暖房：地域の区分が未入力"), std::string::npos) << none;
  EXPECT_NE(none.find("−0.504×冷房能力(kW)＋5.88"), std::string::npos) << none;
  EXPECT_NE(none.find("比消費電力 0.3"), std::string::npos) << none;
  EXPECT_NE(none.find("非居室に白熱灯"), std::string::npos) << none;
  const std::string site = ok("create_polygon", {{"points", {{-2000, -2000, 0}, {14000, -2000, 0}, {14000, 10000, 0}, {-2000, 10000, 0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  ok("set_energy_route", {{"route", "spec"}, {"region", 3}, {"source", "架空の案内"}});
  const std::string three = notes();
  EXPECT_NE(three.find("告示第266号 2、3 地域"), std::string::npos) << three;
  EXPECT_NE(three.find("熱効率 83.0％以上"), std::string::npos) << three;
  EXPECT_NE(three.find("密閉式石油ストーブ"), std::string::npos) << three;
  EXPECT_NE(three.find("年間給湯効率 3.0 以上"), std::string::npos) << three;
  EXPECT_NE(three.find("モード熱効率 81.3％以上"), std::string::npos) << three;
  ok("set_energy_route", {{"route", "spec"}, {"region", 6}, {"source", "架空の案内"}});
  const std::string six = notes();
  EXPECT_NE(six.find("熱効率 87.8％以上"), std::string::npos) << six;
  EXPECT_EQ(six.find("密閉式石油ストーブ"), std::string::npos) << six;
  EXPECT_NE(six.find("モード熱効率 77.8％以上"), std::string::npos) << six;
  ok("set_energy_route", {{"route", "spec"}, {"region", 8}, {"source", "架空の案内"}});
  EXPECT_NE(notes().find("8 地域は基準がありません"), std::string::npos);
  // Only the kinds in the table.
  ok("set_equipment", {{"items", {{{"kind", "lighting"}, {"name", "LED ダウンライト"}}}}});
  const std::string light = notes();
  EXPECT_NE(light.find("非居室に白熱灯"), std::string::npos) << light;
  EXPECT_EQ(light.find("給湯："), std::string::npos) << light;
  EXPECT_EQ(light.find("換気："), std::string::npos) << light;
}

// docs/239: no 浴室・台所・洗面所 among the rooms: the 機器表 notes that the hot-water rule does not apply.
TEST_F(Schedules, HotWaterRuleWithoutWetRooms) {
  ok("set_equipment", {{"items", {{{"kind", "hot_water"}, {"name", "電気ヒートポンプ給湯機"}}}}});
  const auto notes = [&]() {
    std::string all;
    const auto set = sbr::doc::drawingSet(modeler.document(), {});
    if (!set) return all;
    for (std::size_t i = 0; i < set->names.size(); ++i)
      if (set->names[i].find("機器表") == 0)
        for (const auto& item : set->sheets[i].items)
          if (item.kind == sbr::draw::Item::Kind::Text) all += item.text + "\n";
    return all;
  };
  const std::string living = notes();  // only 居間
  EXPECT_NE(living.find("浴室・台所・洗面所が見当たりません"), std::string::npos) << living;
  EXPECT_NE(living.find("2 ただし書き"), std::string::npos) << living;
  ok("create_room", {{"at", {6000, 3000}}, {"name", "キッチン"}});
  const std::string kitchen = notes();
  EXPECT_EQ(kitchen.find("見当たりません"), std::string::npos) << kitchen;
}

// docs/246: renaming a room renames it in the tables (its marker's text).
TEST_F(Schedules, RenamingARoom) {
  ok("rename", {{"id", room}, {"name", "キッチン"}});
  bool renamed = false;
  for (const auto& r : sbr::doc::roomSchedule(modeler.document()).rows) renamed = renamed || (r.id == room && r.name == "キッチン");
  EXPECT_TRUE(renamed);
  EXPECT_FALSE(call("rename", {{"id", room}, {"name", ""}}).value("ok", true));
  ok("rename", {{"id", south}, {"name", "南の壁"}});  // other objects as before
  EXPECT_EQ(modeler.document().find(south)->name, "南の壁");
}

// docs/247: what makes a sheet hard to read, found: words on words, a line through words, words off
// the paper, words too small; and nothing where there is none.
TEST(SheetCheck, FindsWhatMakesASheetHardToRead) {
  using sbr::draw::Item;
  sbr::draw::Sheet sheet;
  sheet.scale = 100;
  sheet.paperWidth = 420;
  sheet.paperHeight = 297;
  const auto text = [&](const std::string& s, double x, double y, double h) {
    Item t;
    t.kind = Item::Kind::Text;
    t.pen = sbr::draw::Pen::Text;
    t.text = s;
    t.points = {gp_Pnt2d(x * 100, y * 100)};
    t.height = h;
    sheet.items.push_back(t);
  };
  const auto line = [&](double x0, double y0, double x1, double y1, sbr::draw::Pen pen) {
    Item l;
    l.kind = Item::Kind::Line;
    l.pen = pen;
    l.points = {gp_Pnt2d(x0 * 100, y0 * 100), gp_Pnt2d(x1 * 100, y1 * 100)};
    sheet.items.push_back(l);
  };
  text("離れた字", 20, 250, 3.5);           // nothing near
  line(20, 245, 60, 245, sbr::draw::Pen::Visible);  // just under it: not through
  text("床断熱　押出法", 100, 100, 3.5);
  line(95, 101.5, 140, 101.5, sbr::draw::Pen::Grid);  // through its middle
  text("重なる一", 200, 150, 3.5);
  text("重なる二", 203, 150.5, 3.5);       // on the first
  text("はみ出す字", 410, 20, 3.5);         // past the right edge
  text("小さい字", 300, 250, 1.2);
  const auto found = sbr::draw::checkSheet(sheet);
  const auto has = [&](const std::string& kind, const std::string& words) {
    return std::any_of(found.begin(), found.end(), [&](const auto& f) { return f.kind == kind && f.text == words; });
  };
  EXPECT_TRUE(has("line_through_text", "床断熱　押出法"));
  EXPECT_TRUE(has("text_on_text", "重なる一"));
  EXPECT_TRUE(has("off_paper", "はみ出す字"));
  EXPECT_TRUE(has("tiny_text", "小さい字"));
  for (const auto& f : found) EXPECT_NE(f.text, "離れた字") << f.kind;
  EXPECT_EQ(found.size(), 4u);
  for (const auto& f : found)
    if (f.kind == "line_through_text") EXPECT_EQ(f.other, "通り芯");
}

// docs/247: the command over the whole drawing set (the counts are printed for a look).
TEST_F(Schedules, CheckDrawingSet) {
  ok("set_insulation", {{"ids", {south, east, north, west}}, {"material", "グラスウール 16K"}, {"thickness_mm", 105}, {"source", "架空"}});
  const json r = ok("check_drawing_set");
  ASSERT_TRUE(r["sheets"].is_array());
  ASSERT_GE(r["sheets"].size(), 5u);
  for (const auto& s : r["sheets"]) {
    std::cout << s["number"].get<std::string>() << " " << s["name"].get<std::string>() << " " << s["counts"].dump() << "\n";
    for (const auto& f : s["findings"])
      std::cout << "    " << f["kind"].get<std::string>() << " [" << f["text"].get<std::string>() << "] " << f["other"].get<std::string>() << "\n";
  }
  const json one = ok("check_drawing_set", {{"sheet", "断熱範囲図（1FL）"}});
  EXPECT_EQ(one["sheets"].size(), 1u);
  EXPECT_FALSE(call("check_drawing_set", {{"sheet", "無い図面"}}).value("ok", true));
}

// docs/247: a line kept clear of words: cut in two around them; untouched when it misses them.
TEST(SheetCheck, LinesKeptClearOfWords) {
  sbr::draw::Item words;
  words.kind = sbr::draw::Item::Kind::Text;
  words.text = "居間  24.00 m²";
  words.points = {gp_Pnt2d(2000, 3000)};
  words.height = 3.5;
  words.hAlign = 1;
  words.vAlign = 1;
  const auto pieces = sbr::draw::clearOfText(gp_Pnt2d(0, 0), gp_Pnt2d(4000, 6000), {&words}, 100, 0.8);
  ASSERT_EQ(pieces.size(), 2u);
  EXPECT_LT(pieces[0].second.Distance(gp_Pnt2d(2000, 3000)), 2000.0);
  EXPECT_GT(pieces[0].second.Distance(gp_Pnt2d(2000, 3000)), 100.0);  // stops short of the words
  const auto missed = sbr::draw::clearOfText(gp_Pnt2d(0, 5000), gp_Pnt2d(4000, 5000), {&words}, 100, 0.8);
  ASSERT_EQ(missed.size(), 1u);
  EXPECT_NEAR(missed[0].first.Distance(gp_Pnt2d(0, 5000)), 0.0, 1e-9);
  EXPECT_NEAR(missed[0].second.Distance(gp_Pnt2d(4000, 5000)), 0.0, 1e-9);
}

// docs/251: an upper storey's plan does not carry the rooms of the storey below (seen through its floor).
TEST_F(Schedules, UpperPlanLeavesTheRoomsBelow) {
  ok("create_wall", {{"points", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}, {0, 0}}}, {"thickness", 200}, {"height", 2700}, {"base", 3000}});
  ok("create_slab", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"top", 3000}, {"thickness", 200}});
  const auto set = sbr::doc::drawingSet(modeler.document(), {});
  ASSERT_TRUE(set);
  const auto words = [&](const std::string& name) {
    std::string all;
    for (std::size_t i = 0; i < set->names.size(); ++i)
      if (set->names[i] == name)
        for (const auto& item : set->sheets[i].items)
          if (item.kind == sbr::draw::Item::Kind::Text) all += item.text + "\n";
    return all;
  };
  EXPECT_NE(words("1FL 平面図").find("居間"), std::string::npos);
  EXPECT_EQ(words("2FL 平面図").find("居間"), std::string::npos) << words("2FL 平面図");
}

// docs/255: with a grid whose lines the cuts lie on, the cuts' marks stay clear of the grid bubbles.
TEST_F(Schedules, CutMarksClearOfGridBubbles) {
  ok("create_grid", {{"origin", {0, 0, 0}}, {"x_spacings", {4000, 4000}}, {"y_spacings", {6000}}});
  const json r = ok("check_drawing_set", {{"sheet", "1FL 平面図"}});
  for (const auto& f : r["sheets"][0]["findings"])
    if (f["kind"] == "text_on_text")
      for (const char* bubble : {"X1", "X2", "X3", "Y1", "Y2"}) {
        EXPECT_NE(f["text"].get<std::string>(), bubble) << f.dump();
        EXPECT_NE(f["other"].get<std::string>(), bubble) << f.dump();
      }
}

// docs/257: a drawing too big for its paper: the sheet check says so, and finds the words left
// beyond the frame.
TEST_F(Schedules, SheetCheckFindsADrawingTooBigForItsPaper) {
  const std::string site = ok("create_polygon", {{"points", {{-2000, -2000, 0}, {14000, -2000, 0}, {14000, 10000, 0}, {-2000, 10000, 0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "架空の測量図"}});
  // 1:5: even the house alone is bigger than A1.
  const json at5 = ok("check_drawing_set", {{"sheet", "1FL 平面図"}, {"scale", 5}});
  const auto& counts = at5["sheets"][0]["counts"];
  EXPECT_EQ(counts.value("does_not_fit", 0), 1) << at5.dump();
  EXPECT_GE(counts.value("off_frame", 0) + counts.value("off_paper", 0), 1) << at5.dump();
  // docs/260: 1:20 leaves out the site's outline, and the plan fits.
  const json at20 = ok("check_drawing_set", {{"sheet", "1FL 平面図"}, {"scale", 20}});
  EXPECT_EQ(at20["sheets"][0]["counts"].value("does_not_fit", 0), 0) << at20.dump();
  EXPECT_EQ(at20["sheets"][0]["counts"].value("off_frame", 0), 0) << at20.dump();
  const json at100 = ok("check_drawing_set", {{"sheet", "1FL 平面図"}});
  EXPECT_EQ(at100["sheets"][0]["counts"].value("does_not_fit", 0), 0);
  EXPECT_EQ(at100["sheets"][0]["counts"].value("off_frame", 0), 0) << at100.dump();
}

// docs/260: a wall without a build-up with rooms on both sides is an inner wall (WD); with a room on
// one side only it stays outer (AD), as do the outer walls' windows (AW).
TEST_F(Schedules, UntypedWallBetweenRoomsIsInner) {
  ok("place_openings", {{"host", partition}, {"face_normal", {1, 0, 0}}, {"count", 1}, {"width", 800}, {"height", 2000}, {"sill", 0}});
  ok("set_opening_fittings", {{"host", partition}, {"fitting", {{"type", "swing_single"}, {"hinge", "left"}, {"swing", "in"}}}});
  const auto markOf = [&](const std::string& host) {
    for (const auto& r : sbr::doc::openingSchedule(modeler.document()).rows)
      if (r.host == host) return r.mark;
    return std::string();
  };
  EXPECT_EQ(markOf(partition).substr(0, 2), "AD");  // a room on the west side only
  ok("create_room", {{"at", {6000, 3000}}, {"name", "洋室"}});
  EXPECT_EQ(markOf(partition), "WD1");
  EXPECT_EQ(markOf(south).substr(0, 2), "AW");
}
