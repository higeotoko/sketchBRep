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
  EXPECT_EQ(x[4], "10.00");  // earthquake
  EXPECT_EQ(x[5], "30.00");
  EXPECT_EQ(x[7], "15.00");  // wind
  EXPECT_EQ(x[8], "15.00");
  EXPECT_EQ(x[9], "5.00");
  EXPECT_EQ(x[10], "要確認");
  EXPECT_EQ(quantity.rows[1][9], "16.00");
  EXPECT_EQ(quantity.rows[1][10], "参考値内");
  ASSERT_FALSE(quantity.notes.empty());
  EXPECT_NE(quantity.notes[0].find("告示1100号"), std::string::npos);
  const auto walls = doc::scheduleTable(m.document(), 8);
  ASSERT_EQ(walls.rows.size(), 4u);
  EXPECT_EQ(walls.rows[0][2], "a");
  EXPECT_NE(walls.rows[0][3].find("45"), std::string::npos) << walls.rows[0][3];
  EXPECT_EQ(walls.rows[0][4], "2.00");
  EXPECT_EQ(walls.rows[0][7], "4.00");
  const auto quarters = doc::scheduleTable(m.document(), 9);
  ASSERT_GE(quarters.rows.size(), 4u);  // X and Y, two side parts each
  EXPECT_EQ(quarters.rows[0][0], "1階");
  EXPECT_EQ(quarters.rows[0][6], "2.00");  // low side: 4 m / 2 m
  EXPECT_EQ(quarters.rows[1][6], "0.50");
  EXPECT_EQ(quarters.rows[0][7], "0.25");
  EXPECT_EQ(quarters.rows[0][8], "要確認");
  // The same tables through the commands.
  for (const char* name : {"wall_quantity", "bracing_walls", "quarters"}) {
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
    markA = markA || mark.text.rfind("a X 倍率2.0 4.00m", 0) == 0;
    side = side || mark.text.find("側端 必要2.00m 存在4.00m") != std::string::npos;
  }
  EXPECT_EQ(thick, 4);
  EXPECT_EQ(dashedBoxes, 4);  // X and Y, two side parts each
  EXPECT_EQ(view->bracingMarks.size(), 8u);
  EXPECT_TRUE(markA);
  EXPECT_TRUE(side);
  bool legend = false;
  for (const auto& n : view->calculationNotes) legend = legend || n.find("太線") != std::string::npos;
  EXPECT_TRUE(legend);
  // Through the command, and in the drawing set.
  const auto path = (std::filesystem::temp_directory_path() / "bracing-plan.dxf").string();
  const json r = json::parse(api.execute(json{{"command", "export_drawing"},
      {"args", {{"path", path}, {"kind", "plan"}, {"bracing_floor", "1"}}}}.dump()));
  EXPECT_TRUE(r.value("ok", false)) << r.dump();
  const auto set = doc::drawingSet(m.document(), doc::DrawingSetSpec{});
  ASSERT_TRUE(set) << set.error();
  EXPECT_NE(std::find(set->names.begin(), set->names.end(), "耐力壁配置図（1階）"), set->names.end());

  // Add actual walls, a room tag and a section line to the same calculation-only example.
  ASSERT_TRUE(m.createWalls({{0,0,0}, {8000,0,0}, {8000,6000,0}, {0,6000,0}, {0,0,0}}, 200, 3000));
  const auto room = json::parse(api.execute(R"({"command":"create_room","args":{"at":[1250,2000],"name":"居間"}})"));
  ASSERT_TRUE(room.value("ok", false)) << room.dump();
  spec.cutMarks.push_back({gp_Pnt(1250, 2000, 0), gp_Dir(0, 1, 0), "A", "A-06"});
  const auto furnished = draw::makeView(m.document(), spec);
  ASSERT_TRUE(furnished) << furnished.error();
  for (const double scale : {50.0, 100.0}) {
    SCOPED_TRACE(scale);
    draw::SheetSpec layout;
    layout.scale = scale;
    const auto sheet = draw::layoutSheet(*furnished, layout);
    ASSERT_TRUE(sheet) << sheet.error();
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
    }
  }
}
