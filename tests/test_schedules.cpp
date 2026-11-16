// Tables and tags read from the model (docs/90 G-11, docs/97 N-10 / N-11, docs/104): the door
// and window schedule, the room schedule, areas by storey, and the marks on plans.
#include <cmath>
#include <map>
#include <gtest/gtest.h>
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
  EXPECT_NE(csv.find("W1,窓,引違い,1600,1200,2,900,,\r\n"), std::string::npos) << csv;
  EXPECT_NE(csv.find("合計,,,,,5,,,"), std::string::npos);
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

TEST_F(Schedules, AMarkGivenByHandStaysAndWorkedOutNumbersSkipIt) {
  // The two 引違い windows become AW-1 with a product number; the plain window takes "W1" by hand.
  const auto steps = modeler.history().undoCount();
  ok("set_opening_mark", {{"host", south}, {"mark", "AW-1"}, {"product", "APW330-16513"}});
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  ok("set_opening_mark", {{"host", north}, {"openings", {1}}, {"mark", "W1"}});
  json s = ok("opening_schedule", {{"include_csv", true}});
  std::map<std::string, json> byMark;
  for (const json& t : s["types"]) byMark[t["mark"].get<std::string>()] = t;
  ASSERT_EQ(byMark.size(), 4u) << s["types"].dump();
  EXPECT_EQ(byMark["AW-1"]["count"], 2);
  EXPECT_TRUE(byMark["AW-1"]["own_mark"].get<bool>());
  EXPECT_EQ(byMark["AW-1"]["product"], "APW330-16513");
  EXPECT_FALSE(byMark["AW-1"]["mixed"].get<bool>());
  EXPECT_EQ(byMark["W1"]["fitting"], "窓");  // the one given by hand
  EXPECT_TRUE(byMark.count("D1") && byMark.count("O1"));
  EXPECT_NE(s["csv"].get<std::string>().find("AW-1,窓,引違い,1600,1200,2,900,APW330-16513,手で決めた符号\r\n"), std::string::npos) << s["csv"];
  // The plan carries them.
  const auto view = sbr::draw::makeView(modeler.document(), sbr::draw::planView(1100));
  ASSERT_TRUE(view);
  int aw = 0;
  for (const auto& item : view->items)
    if (item.kind == sbr::draw::Item::Kind::Text && item.text == "AW-1") ++aw;
  EXPECT_EQ(aw, 2);

  // Another window of a new size: it is W2, not W1 (taken by hand); AW-1 stays.
  ok("place_openings", {{"host", west}, {"face_normal", {-1, 0, 0}}, {"count", 1}, {"width", 700}, {"height", 700}, {"sill", 1200}});
  ok("set_opening_fittings", {{"host", west}, {"fitting", {{"type", "window"}}}});
  s = ok("opening_schedule");
  bool w2 = false;
  for (const json& t : s["types"]) {
    if (t["mark"] == "W2") w2 = t["width"].get<double>() == 700.0;
    if (t["mark"] == "W1") EXPECT_NEAR(t["width"].get<double>(), 1600, 1e-6);
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
  // A thicker roof by hand no longer adds up: no lines.
  auto spec = modeler.roofInfo(roof)->spec;
  spec.thickness = 150;
  ASSERT_TRUE(modeler.changeRoof(roof, spec));
  EXPECT_TRUE(sbr::doc::Modeler::typeLayerDepths(modeler.document(), *modeler.document().find(roof)).empty());
  EXPECT_EQ(sloped().size(), before);
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

// docs/113: a table on a sheet of its own.
TEST_F(Schedules, ATableGoesOnASheetOfItsOwn) {
  const sbr::draw::Table table = sbr::doc::scheduleTable(modeler.document(), 0);
  EXPECT_EQ(table.title, "建具表");
  ASSERT_EQ(table.rows.size(), 5u);  // W1, W2, D1, O1 and the total
  EXPECT_EQ(table.rows.front().front(), "W1");
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
  EXPECT_EQ(texts["W1"], 1);
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
