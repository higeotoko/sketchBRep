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
