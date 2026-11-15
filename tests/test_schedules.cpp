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
  EXPECT_EQ(sbr::doc::scheduleTable(modeler.document(), 2).rows.front().back(), "未評価");
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
  // Made again at a new height: still one board.
  ok("set_room_finish", {{"id", room}, {"ceiling_height", 2600}});
  made = ok("make_ceilings", {{"rooms", {room}}});
  int boards = 0;
  for (const auto& obj : modeler.document().objects())
    if (obj.attributes.count("kind") && obj.attributes.at("kind") == "ceiling") ++boards;
  EXPECT_EQ(boards, 1);
  const auto* again = modeler.document().find(made["ids"][0].get<std::string>());
  EXPECT_NEAR(sbr::geom::exactBounds(modeler.document().worldShape(*again)).CornerMin().Z(), 2600, 1e-6);
  ASSERT_TRUE(modeler.undo());
  EXPECT_TRUE(modeler.document().find(board));  // the first board is back
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
