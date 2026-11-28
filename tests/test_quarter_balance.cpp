#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "sbr/ConstructionWall.h"
#include "sbr/doc/Modeler.h"

using namespace sbr;
using namespace sbr::doc;
using nlohmann::json;
namespace {
// 四分割法 by 平成12年告示1352号 (now 告示1100 第四). Plan 10 m x 4 m, one storey;
// Lw = 98 kN / (0.0196 x 50 m2) x 0.2 = 20 cm/m2 (Ai = 1, C0 = 0.2).
// X walls: side parts are the Y-end quarters, 10 m x 1 m = 10 m2 -> required 2.0 m each.
// Y walls: side parts are the X-end quarters, 2.5 m x 4 m = 10 m2 -> required 2.0 m each.
json wall(std::string id, std::string direction, json start, json end, std::string floor = "1") {
  return {{"id", id}, {"floor", floor}, {"direction", direction}, {"spec", "brace_45x90"},
          {"start", start}, {"end", end}, {"height_mm", 3000}, {"verified", true}};
}
json floor(std::string id, double weight, json plan) {
  json f = {{"id", id}, {"name", id + "階"}, {"area_m2", 50}, {"weight_kn", weight}, {"wind_x_m2", 30},
            {"wind_y_m2", 6}, {"walls_complete", true}};
  if (!plan.is_null()) f["plan"] = plan;
  return f;
}
json inputs(json walls, json floors = json()) {
  if (floors.is_null()) floors = json::array({floor("1", 98, {{"x", {0, 10000}}, {"y", {0, 4000}}})});
  return {{"height_m", 6}, {"co_designated", false}, {"wind_coefficient_cm_m2", 50},
          {"input_basis", "計算テストの仮想入力（実建物ではない）"}, {"floors", floors}, {"walls", walls}};
}
struct QuarterBalance : testing::Test {
  Modeler m;
  json rows(const json& in) {
    const auto r = construction::checkWallQuantity(m.document(), json{{"inputs", in}}.dump());
    EXPECT_TRUE(r) << r.error();
    return r ? json::parse(*r).at("quarter_balance") : json::array();
  }
  json row(const json& in, const std::string& floor, const std::string& direction) {
    for (const auto& r : rows(in))
      if (r["floor"] == floor && r["direction"] == direction) return r;
    ADD_FAILURE() << "no row";
    return {};
  }
};
}  // namespace

TEST_F(QuarterBalance, SidePartsRequiredAndExistingMatchHandCalculation) {
  // Low Y end: 2 m of 45x90 (ratio 2) = 4 m -> 2.0; high end: 0.5 m -> 1 m -> 0.5. Wall ratio 0.25.
  const auto r = row(inputs({wall("a", "X", {0, 0}, {2000, 0}), wall("b", "X", {0, 4000}, {500, 4000}),
                             wall("mid", "X", {0, 2000}, {9000, 2000})}),
                     "1", "X");
  ASSERT_EQ(r["strips"].size(), 2u);
  EXPECT_NEAR(r["strips"][0]["area_m2"].get<double>(), 10, 1e-9);
  EXPECT_NEAR(r["strips"][0]["required_m"].get<double>(), 2.0, 1e-9);
  EXPECT_NEAR(r["strips"][0]["existing_m"].get<double>(), 4.0, 1e-9);
  EXPECT_NEAR(r["strips"][1]["existing_m"].get<double>(), 1.0, 1e-9);  // the middle wall is in neither part
  EXPECT_NEAR(r["strips"][0]["ratio"].get<double>(), 2.0, 1e-9);
  EXPECT_NEAR(r["strips"][1]["ratio"].get<double>(), 0.5, 1e-9);
  EXPECT_NEAR(r["wall_ratio"].get<double>(), .25, 1e-9);
  EXPECT_EQ(r["status"], "fail");
}

TEST_F(QuarterBalance, WallRatioOfExactlyHalfPassesAndBothOverOnePasses) {
  // High end 1 m -> 2 m -> ratio 1.0 (not over 1); wall ratio 1.0/2.0 = 0.5 >= 0.5: pass.
  auto r = row(inputs({wall("a", "X", {0, 0}, {2000, 0}), wall("b", "X", {0, 4000}, {1000, 4000})}), "1", "X");
  EXPECT_NEAR(r["wall_ratio"].get<double>(), .5, 1e-9);
  EXPECT_EQ(r["status"], "pass");
  // Both ends over 1 with a small wall ratio still pass (告示1352 三号ただし書).
  r = row(inputs({wall("a", "X", {0, 0}, {9000, 0}), wall("b", "X", {0, 4000}, {1200, 4000})}), "1", "X");
  EXPECT_LT(r["wall_ratio"].get<double>(), .5);
  EXPECT_EQ(r["status"], "pass");
  EXPECT_EQ(r["basis"], "両側端の壁量充足率が1を超える");
}

TEST_F(QuarterBalance, YWallsUseTheXEndQuarters) {
  const auto r = row(inputs({wall("l", "Y", {0, 0}, {0, 1000}), wall("r", "Y", {10000, 0}, {10000, 1000}),
                             wall("in", "Y", {5000, 0}, {5000, 4000})}),
                     "1", "Y");
  EXPECT_NEAR(r["strips"][0]["area_m2"].get<double>(), 10, 1e-9);
  EXPECT_NEAR(r["strips"][0]["existing_m"].get<double>(), 2.0, 1e-9);
  EXPECT_NEAR(r["strips"][1]["existing_m"].get<double>(), 2.0, 1e-9);
  EXPECT_EQ(r["status"], "pass");
}

TEST_F(QuarterBalance, MissingPlanOrUnfinishedWallsAreNotEvaluated) {
  json floors = json::array({floor("1", 98, nullptr)});
  auto r = row(inputs(json::array({wall("a", "X", {0, 0}, {2000, 0})}), floors), "1", "X");
  EXPECT_EQ(r["status"], "not_evaluated");
  EXPECT_TRUE(r["wall_ratio"].is_null());
  auto in = inputs(json::array({wall("a", "X", {0, 0}, {2000, 0})}));
  in["floors"][0]["walls_complete"] = false;
  EXPECT_EQ(row(in, "1", "X")["status"], "not_evaluated");
}

TEST_F(QuarterBalance, SidePartWithoutTheUpperFloorIsNotGuessed) {
  // Second floor over the west half only: the 1F X-direction parts are only partly under it,
  // the 1F Y-direction east part has no floor above (下屋). Neither is guessed.
  json floors = json::array({floor("1", 147, {{"x", {0, 10000}}, {"y", {0, 4000}}}),
                             floor("2", 49, {{"x", {0, 5000}}, {"y", {0, 4000}}})});
  const auto in = inputs({wall("a", "X", {0, 0}, {2000, 0}), wall("b", "X", {0, 4000}, {2000, 4000}),
                          wall("l", "Y", {0, 0}, {0, 2000}), wall("r", "Y", {10000, 0}, {10000, 2000})},
                         floors);
  const auto x = row(in, "1", "X");
  EXPECT_EQ(x["status"], "not_evaluated");
  EXPECT_EQ(x["strips"][0]["upper_floor"], "partial");
  const auto y = row(in, "1", "Y");
  EXPECT_EQ(y["strips"][0]["upper_floor"], "full");
  EXPECT_EQ(y["strips"][1]["upper_floor"], "none");
  EXPECT_EQ(y["status"], "not_evaluated");
}

TEST_F(QuarterBalance, BadPlanIsRefusedAndSideAreasOverrideTheRectangle) {
  auto in = inputs(json::array());
  in["floors"][0]["plan"] = {{"x", {100, 0}}, {"y", {0, 4000}}};
  EXPECT_FALSE(construction::checkWallQuantity(m.document(), json{{"inputs", in}}.dump()));
  in = inputs({wall("a", "X", {0, 0}, {2000, 0}), wall("b", "X", {0, 4000}, {2000, 4000})});
  in["floors"][0]["side_areas_m2"] = {{"X", {5, 20}}, {"Y", {10, 10}}};
  const auto r = row(in, "1", "X");
  EXPECT_NEAR(r["strips"][0]["required_m"].get<double>(), 1.0, 1e-9);
  EXPECT_NEAR(r["strips"][1]["required_m"].get<double>(), 4.0, 1e-9);
  EXPECT_NEAR(r["wall_ratio"].get<double>(), 0.25, 1e-9);  // ratios 4/1 = 4.0 and 4/4 = 1.0 (not over 1)
  EXPECT_EQ(r["status"], "fail");
}

TEST_F(QuarterBalance, RatiosOfExactlyOneAreNotOverOne) {
  // Both parts exactly 1.0 (2 m of ratio-2 wall... halves: 1 m x 2 = 2.0 m over 2.0 m): not "over 1",
  // so the pass comes from the wall ratio (1.0 >= 0.5), not from the proviso.
  const auto r = row(inputs({wall("a", "X", {0, 0}, {1000, 0}), wall("b", "X", {0, 4000}, {1000, 4000})}), "1", "X");
  EXPECT_NEAR(r["strips"][0]["ratio"].get<double>(), 1.0, 1e-9);
  EXPECT_EQ(r["status"], "pass");
  EXPECT_EQ(r["basis"], "壁率比が0.5以上か");
  EXPECT_DOUBLE_EQ(r["comparison_tolerance"].get<double>(), 1e-10);
}

TEST_F(QuarterBalance, BlockersOfTheWholeCalculationAlsoHoldForTheSideParts) {
  // Balanced walls: passes when everything is in order.
  const json walls = {wall("a", "X", {0, 0}, {2000, 0}), wall("b", "X", {0, 4000}, {2000, 4000})};
  auto in = inputs(walls);
  EXPECT_EQ(row(in, "1", "X")["status"], "pass");
  // No basis for the loads and areas: the wall quantity is not evaluated, so neither is this.
  in.erase("input_basis");
  EXPECT_EQ(row(in, "1", "X")["status"], "not_evaluated");
  EXPECT_TRUE(row(in, "1", "X")["wall_ratio"].is_null());
  // A table that is not in force on the date of the calculation.
  const auto early = construction::checkWallQuantity(
      m.document(), json{{"inputs", inputs(walls)}, {"as_of", "2025-03-31"}}.dump());
  ASSERT_TRUE(early) << early.error();
  const json earlyReport = json::parse(*early);
  ASSERT_EQ(earlyReport.at("quarter_balance").size(), 2u);
  for (const auto& r : earlyReport.at("quarter_balance")) EXPECT_EQ(r["status"], "not_evaluated");
  // Inputs saved for a frame that is gone.
  in = inputs(walls);
  in["frame"] = "no-such-group";
  EXPECT_EQ(row(in, "1", "X")["status"], "not_evaluated");
}

// docs/199: the permit workflow's stage 7 (耐力壁) reads the same calculation: per storey and
// direction the wall quantity and the quarter-division balance, the basis, and what is not assessed.
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Schedules.h"
TEST(PermitBraceStage, RowsFollowTheWallQuantityCalculation) {
  Modeler m;
  auto rowOf = [&](const std::string& item) {
    for (const auto& c : doc::permitBraceChecks(m.document()))
      if (c.item == item) return c;
    ADD_FAILURE() << "no row " << item;
    return doc::PermitCheck{};
  };
  EXPECT_EQ(rowOf("壁量の入力").state, "未入力");
  api::CommandApi api(m);
  const json saved = json::parse(api.execute(json{{"command", "set_wall_calculation"},
      {"args", {{"inputs", inputs({wall("a", "X", {0, 0}, {2000, 0}), wall("b", "X", {0, 4000}, {500, 4000}),
                                   wall("c", "Y", {0, 0}, {0, 4000}), wall("d", "Y", {10000, 0}, {10000, 4000})})}}}}.dump()));
  ASSERT_TRUE(saved.value("ok", false)) << saved.dump();
  const auto x = rowOf("1階 X 方向の四分割");
  EXPECT_EQ(x.state, "要確認");  // wall ratio 0.25 (see SidePartsRequiredAndExistingMatchHandCalculation)
  EXPECT_NE(x.details.find("壁率比 0.25"), std::string::npos) << x.details;
  EXPECT_TRUE(x.attention);
  EXPECT_TRUE(x.targets.empty());  // the walls of this calculation are not objects of the model
  const auto quantity = rowOf("1階 X 方向の壁量");
  EXPECT_NE(quantity.details.find("存在壁量"), std::string::npos);
  EXPECT_NE(quantity.state, "");
  EXPECT_NE(rowOf("照らした値").details.find("告示1100号"), std::string::npos);
  EXPECT_NE(rowOf("まだ確かめていない事").details.find("N値"), std::string::npos);
}

// docs/208: the wall quantity forms read the same calculation. X: 2 m + 0.5 m of 45x90 braces
// (ratio 2) = 5 m against 15 m (wind 30 m2 x 50 cm/m2; earthquake 50 m2 x 20 cm/m2 = 10 m);
// Y: 4 m + 4 m x 2 = 16 m against 10 m.
TEST(WallQuantityForms, TablesFollowTheCalculation) {
  Modeler m;
  api::CommandApi api(m);
  const json saved = json::parse(api.execute(json{{"command", "set_wall_calculation"},
      {"args", {{"inputs", inputs({wall("a", "X", {0, 0}, {2000, 0}), wall("b", "X", {0, 4000}, {500, 4000}),
                                   wall("c", "Y", {0, 0}, {0, 4000}), wall("d", "Y", {10000, 0}, {10000, 4000})})}}}}.dump()));
  ASSERT_TRUE(saved.value("ok", false)) << saved.dump();
  const auto quantity = doc::scheduleTable(m.document(), 7);
  EXPECT_EQ(quantity.title, "壁量計算表");
  ASSERT_EQ(quantity.rows.size(), 2u);
  const auto& x = quantity.rows[0];
  EXPECT_EQ(x[0], "1階");
  EXPECT_EQ(x[1], "X");
  EXPECT_EQ(x[2], "50.00");
  EXPECT_EQ(x[4], "1000.00");  // earthquake, cm (docs/332)
  EXPECT_EQ(x[5], "30.00");
  EXPECT_EQ(x[7], "1500.00");  // wind
  EXPECT_EQ(x[8], "1500.00");
  EXPECT_EQ(x[9], "500.00");
  EXPECT_EQ(x[10], "要確認");
  EXPECT_EQ(quantity.rows[1][9], "1600.00");
  EXPECT_EQ(quantity.rows[1][10], "参考値内");
  ASSERT_FALSE(quantity.notes.empty());
  EXPECT_NE(quantity.notes[0].find("告示1100号"), std::string::npos);
  const auto walls = doc::scheduleTable(m.document(), 8);
  ASSERT_EQ(walls.rows.size(), 4u);
  EXPECT_EQ(walls.rows[0][2], "a");
  EXPECT_EQ(walls.rows[0][3], "A");  // docs/332: the wall symbol
  EXPECT_NE(walls.rows[0][4].find("45"), std::string::npos) << walls.rows[0][4];
  EXPECT_EQ(walls.rows[0][5], "200.00");
  EXPECT_EQ(walls.rows[0][8], "400.00");
  // docs/332: the specification list and the existing wall quantity, as the MLIT manual's 耐力壁図 tables.
  const auto specs = doc::scheduleTable(m.document(), 12);
  EXPECT_EQ(specs.title, "耐力壁仕様一覧");
  ASSERT_EQ(specs.rows.size(), 1u);
  EXPECT_EQ(specs.rows[0][0], "耐力壁");
  EXPECT_EQ(specs.rows[0][1], "A");
  EXPECT_EQ(specs.rows[0][3], "2.00");
  const auto existing = doc::scheduleTable(m.document(), 13);
  EXPECT_EQ(existing.title, "存在壁量の算定");
  ASSERT_EQ(existing.rows.size(), 2u);  // X and Y, one symbol each
  EXPECT_EQ(existing.rows[0][0], "1階／X 方向");
  EXPECT_EQ(existing.rows[0][1], "A");
  EXPECT_EQ(existing.rows[0][3], "250.00");  // 2 m + 0.5 m
  EXPECT_EQ(existing.rows[0][4], "500.00");
  EXPECT_EQ(existing.rows[0][5], "500.00");
  EXPECT_EQ(existing.rows[1][5], "1600.00");
  const auto quarters = doc::scheduleTable(m.document(), 9);
  ASSERT_GE(quarters.rows.size(), 4u);  // X and Y, two side parts each
  EXPECT_EQ(quarters.rows[0][0], "1階");
  EXPECT_EQ(quarters.rows[0][6], "2.00");  // low side: 4 m / 2 m
  EXPECT_EQ(quarters.rows[1][6], "0.50");
  EXPECT_EQ(quarters.rows[0][7], "0.25");
  EXPECT_EQ(quarters.rows[0][8], "要確認");
  // The same tables through the commands.
  for (const char* name : {"wall_quantity", "bracing_walls", "quarters", "bracing_specs", "existing_walls"}) {
    const auto path = (std::filesystem::temp_directory_path() / (std::string(name) + "-form.dxf")).string();
    const json r = json::parse(api.execute(json{{"command", "export_schedule"}, {"args", {{"path", path}, {"table", name}}}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << r.dump();
  }
}

// docs/208: the plan of the bracing walls of a storey: the walls counted (thick, with their mark)
// and the side parts (dashed, with what they need and have); a sheet of the drawing set.
#include "sbr/draw/Drawing.h"
#include "sbr/draw/SheetCheck.h"
TEST(WallQuantityForms, BracingPlanShowsTheWallsAndTheSideParts) {
  Modeler m;
  api::CommandApi api(m);
  const json saved = json::parse(api.execute(json{{"command", "set_wall_calculation"},
      {"args", {{"inputs", inputs({wall("a", "X", {0, 0}, {2000, 0}), wall("b", "X", {0, 4000}, {500, 4000}),
                                   wall("c", "Y", {0, 0}, {0, 4000}), wall("d", "Y", {10000, 0}, {10000, 4000})})}}}}.dump()));
  ASSERT_TRUE(saved.value("ok", false)) << saved.dump();
  draw::ViewSpec spec = draw::planView(1100.0);
  spec.bracingFloor = "1";
  const auto view = draw::makeView(m.document(), spec);
  ASSERT_TRUE(view) << view.error();
  int thick = 0, dashedBoxes = 0;
  bool markA = false, side = false;
  for (const auto& i : view->items) {
    if (i.kind == draw::Item::Kind::Line && i.pen == draw::Pen::Cut) ++thick;
    if (i.kind == draw::Item::Kind::Polyline && i.pen == draw::Pen::Hidden && i.closed) ++dashedBoxes;
  }
  for (const auto& mark : view->bracingMarks) {
    markA = markA || mark.text == "A";  // docs/332: the wall symbol only
    side = side || mark.text.find("側端 必要2.00m 存在4.00m") != std::string::npos;
    // docs/349: a side part's note points at its outer edge (the plan's side), not into the plan.
    if (mark.text.find("側端") != std::string::npos) {
      const double u = mark.at.X(), v = mark.at.Y();
      const bool onSide = std::abs(u) < 1 || std::abs(u - 10000) < 1 || std::abs(v) < 1 || std::abs(v - 4000) < 1;
      EXPECT_TRUE(onSide) << mark.text << " at " << u << "," << v;
    }
  }
  EXPECT_EQ(thick, 4);
  EXPECT_EQ(dashedBoxes, 4);  // X and Y, two side parts each
  EXPECT_EQ(view->bracingMarks.size(), 8u);
  EXPECT_TRUE(markA);
  EXPECT_TRUE(side);
  bool legend = false;
  for (const auto& n : view->calculationNotes) legend = legend || n.find("壁記号") != std::string::npos;  // docs/333
  EXPECT_TRUE(legend);
  // Through the command, and in the drawing set.
  const auto path = (std::filesystem::temp_directory_path() / "bracing-plan.dxf").string();
  const json r = json::parse(api.execute(json{{"command", "export_drawing"},
      {"args", {{"path", path}, {"kind", "plan"}, {"bracing_floor", "1"}}}}.dump()));
  EXPECT_TRUE(r.value("ok", false)) << r.dump();
  const auto set = doc::drawingSet(m.document(), doc::DrawingSetSpec{});
  ASSERT_TRUE(set) << set.error();
  EXPECT_NE(std::find(set->names.begin(), set->names.end(), "耐力壁配置図（1階）"), set->names.end());

  // The room marker is on both the Y side part's boundary (x=2500) and the section line.
  ASSERT_TRUE(m.createWalls({{0,0,0}, {8000,0,0}, {8000,6000,0}, {0,6000,0}, {0,0,0}}, 200, 3000));
  const auto room = json::parse(api.execute(R"({"command":"create_room","args":{"at":[2500,2000],"name":"居間"}})"));
  ASSERT_TRUE(room.value("ok", false)) << room.dump();
  spec.cutMarks.push_back({gp_Pnt(2500, 2000, 0), gp_Dir(0, 1, 0), "A", "A-06"});
  const auto furnished = draw::makeView(m.document(), spec);
  ASSERT_TRUE(furnished) << furnished.error();
  ASSERT_EQ(furnished->roomTags.size(), 1u);
  const auto& roomTag = furnished->items[furnished->roomTags.front().item];
  for (const double scale : {50.0, 100.0}) {
    SCOPED_TRACE(scale);
    EXPECT_FALSE(draw::textIsClear(roomTag, view->items, scale, 0.8));
    draw::SheetSpec layout;
    layout.scale = scale;
    const auto sheet = draw::layoutSheet(*furnished, layout);
    ASSERT_TRUE(sheet) << sheet.error();
    EXPECT_TRUE(std::any_of(sheet->items.begin(), sheet->items.end(), [&](const draw::Item& i) {
      return i.kind == draw::Item::Kind::Text && i.text == roomTag.text;
    }));
    for (const auto& mark : furnished->bracingMarks) {
      EXPECT_TRUE(std::any_of(sheet->items.begin(), sheet->items.end(), [&](const draw::Item& i) {
        return i.kind == draw::Item::Kind::Text && i.text == mark.text;
      })) << mark.text;
    }
    for (const auto& finding : draw::checkSheet(*sheet)) {
      const bool structural = std::any_of(furnished->bracingMarks.begin(), furnished->bracingMarks.end(),
                                         [&](const auto& mark) {
                                           return finding.text == mark.text || finding.other == mark.text;
                                         });
      EXPECT_FALSE(structural) << finding.kind << " " << finding.text << " " << finding.other;
      EXPECT_NE(finding.text, roomTag.text) << finding.kind << " " << finding.other;
      EXPECT_NE(finding.other, roomTag.text) << finding.kind << " " << finding.text;
    }
  }
}

// docs/339: 準耐力壁等 (昭和56年告示1100号 別表第十). Board: 0.6 x 別表第二(は) x heights / clear height, twice on both
// faces; lath: 0.5 x heights / clear height. 第三第一号: the 耐力壁 alone give half the required at least unless the
// brittle check is confirmed; 第四: in the side parts only when confirmed. Y here needs 10 m (earthquake 50 m2 x 20).
namespace {
json semiWall(std::string id, std::string direction, json start, json end, json layers, double clear, bool endsContinue = false) {
  json w = {{"id", id}, {"floor", "1"}, {"direction", direction}, {"spec", "準耐力壁（構造用合板）"},
            {"start", start}, {"end", end}, {"verified", true},
            {"semi", {{"layers", layers}, {"clear_height_mm", clear}, {"ends_continue", endsContinue}}}};
  return w;
}
json board(double base, json heights, int faces = 1) {
  return {{"material", "board"}, {"base_ratio", base}, {"base_source", "告示1100号 別表第二(四)項 (は)欄（試験の入力）"},
          {"faces", faces}, {"heights_mm", heights}};
}
json yRow(const json& r) {
  for (const auto& row : r.at("rows"))
    if (row["floor"] == "1" && row["direction"] == "Y") return row;
  return json();
}
}  // namespace
TEST(SemiBearingWalls, RatiosFollowTheTableAndTheHalfRule) {
  Modeler m;
  // One face of board to 2100 of 2600: 0.6 x 2.5 x 2100 / 2600.
  json in = inputs({wall("c", "Y", {0, 0}, {0, 2000}),
                    semiWall("s", "Y", {10000, 0}, {10000, 2100}, json::array({board(2.5, {2100})}), 2600)});
  auto r = json::parse(*construction::checkWallQuantity(m.document(), json{{"inputs", in}}.dump()));
  json s;
  for (const auto& w : r["walls"])
    if (w["id"] == "s") s = w;
  ASSERT_EQ(s["status"], "evaluated") << s.dump();
  EXPECT_NEAR(s["ratio"].get<double>(), 0.6 * 2.5 * 2100 / 2600, 1e-12);
  EXPECT_EQ(s["semi_part"], "準耐力壁");
  // Both faces to the full clear height: 3.0, 2.1 m long -> 6.3 m; with the brace 2 m x 2 = 4 m: 10.3 m >= 10 m,
  // but the 耐力壁 give 4 m < 5 m (half): it fails on 第三第一号.
  in = inputs({wall("c", "Y", {0, 0}, {0, 2000}),
               semiWall("s", "Y", {10000, 0}, {10000, 2100}, json::array({board(2.5, {2600}, 2)}), 2600)});
  r = json::parse(*construction::checkWallQuantity(m.document(), json{{"inputs", in}}.dump()));
  auto y = yRow(r);
  EXPECT_NEAR(y["existing_m"].get<double>(), 10.3, 1e-9);
  EXPECT_NEAR(y["bearing_m"].get<double>(), 4.0, 1e-9);
  EXPECT_NEAR(y["semi_m"].get<double>(), 6.3, 1e-9);
  EXPECT_EQ(y["semi_rule"], "耐力壁だけでは必要壁量の1/2未満");
  EXPECT_EQ(y["status"], "fail");
  // Confirmed not to fail brittle (with its basis): the rule does not apply, it passes.
  in["semi_brittle_checked"] = true;
  in["semi_brittle_basis"] = "試験の入力";
  r = json::parse(*construction::checkWallQuantity(m.document(), json{{"inputs", in}}.dump()));
  EXPECT_EQ(yRow(r)["status"], "pass");
  EXPECT_FALSE(yRow(r).contains("semi_rule"));
  // Without a basis the confirmation is refused.
  json bad = in;
  bad.erase("semi_brittle_basis");
  EXPECT_THROW(construction::normalizeWallInputs(bad.dump()), std::exception);
}
TEST(SemiBearingWalls, WaistAndHangingWallsNeedTheirConditions) {
  Modeler m;
  const auto lath = [](json heights) { return json{{"material", "lath"}, {"heights_mm", heights}}; };
  const auto statusOf = [&](const json& w) {
    const auto r = json::parse(*construction::checkWallQuantity(m.document(), json{{"inputs", inputs(json::array({w}))}}.dump()));
    return r["walls"][0];
  };
  // 腰壁 900 + 垂れ壁 400 of laths, clear 2600: 0.5 x 1300 / 2600 = 0.25, a 腰壁・垂れ壁 (under 8/10).
  auto w = statusOf(semiWall("k", "X", {0, 0}, {1820, 0}, json::array({lath({900, 400})}), 2600, true));
  EXPECT_EQ(w["status"], "evaluated") << w.dump();
  EXPECT_NEAR(w["ratio"].get<double>(), 0.25, 1e-12);
  EXPECT_EQ(w["semi_part"], "腰壁・垂れ壁");
  // The end posts not confirmed to go on into 準耐力壁: not evaluated.
  EXPECT_EQ(statusOf(semiWall("k", "X", {0, 0}, {1820, 0}, json::array({lath({900, 400})}), 2600, false))["status"], "not_evaluated");
  // Posts more than 2 m apart: not evaluated.
  EXPECT_EQ(statusOf(semiWall("k", "X", {0, 0}, {2730, 0}, json::array({lath({900, 400})}), 2600, true))["status"], "not_evaluated");
  // A wall under 36 cm: not evaluated.
  EXPECT_EQ(statusOf(semiWall("k", "X", {0, 0}, {1820, 0}, json::array({lath({900, 300})}), 2600, true))["status"], "not_evaluated");
}
TEST(SemiBearingWalls, SidePartsCountThemOnlyWhenConfirmed) {
  Modeler m;
  // A 準耐力壁 in the low-X side part of Y (x within 0..2500): left out of the side part's existing quantity unless confirmed.
  json in = inputs({wall("c", "Y", {0, 0}, {0, 4000}), wall("d", "Y", {10000, 0}, {10000, 4000}),
                    semiWall("s", "Y", {1000, 0}, {1000, 2000}, json::array({board(2.5, {2600})}), 2600)});
  const auto lowY = [&](const json& input) {
    const auto r = json::parse(*construction::checkWallQuantity(m.document(), json{{"inputs", input}}.dump()));
    for (const auto& row : r["quarter_balance"])
      if (row["floor"] == "1" && row["direction"] == "Y") return row["strips"][0]["existing_m"].get<double>();
    return -1.0;
  };
  EXPECT_NEAR(lowY(in), 8.0, 1e-9);  // c only: 4 m x 2
  in["semi_brittle_checked"] = true;
  in["semi_brittle_basis"] = "試験の入力";
  EXPECT_NEAR(lowY(in), 8.0 + 2.0 * 1.5, 1e-9);  // and s: 2 m x 1.5
}
// docs/339: the spec list names a 準耐力壁等 as such, after the 耐力壁, with its worked-out ratio.
TEST(SemiBearingWalls, SpecListNamesThem) {
  Modeler m;
  api::CommandApi api(m);
  const json saved = json::parse(api.execute(json{{"command", "set_wall_calculation"},
      {"args", {{"inputs", inputs({wall("c", "Y", {0, 0}, {0, 2000}),
                                   semiWall("s", "Y", {10000, 0}, {10000, 2100}, json::array({board(2.5, {2600})}), 2600)})}}}}.dump()));
  ASSERT_TRUE(saved.value("ok", false)) << saved.dump();
  const auto specs = doc::scheduleTable(m.document(), 12);
  ASSERT_EQ(specs.rows.size(), 2u);
  EXPECT_EQ(specs.rows[0][0], "耐力壁");
  EXPECT_EQ(specs.rows[0][1], "A");
  EXPECT_EQ(specs.rows[1][0], "準耐力壁等");
  EXPECT_EQ(specs.rows[1][1], "B");
  EXPECT_EQ(specs.rows[1][3], "1.50");  // 0.6 x 2.5 x 2600 / 2600
}
