#include <gtest/gtest.h>
#include <cmath>
#include <nlohmann/json.hpp>
#include "sbr/ConstructionWall.h"
#include "sbr/ConstructionCheck.h"
#include "sbr/ConstructionGuide.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/draw/Drawing.h"

using namespace sbr;
using namespace sbr::doc;
using nlohmann::json;
namespace {
json wall(std::string id, std::string direction, double length = 10000, std::string floor = "1") {
  return {{"id", id},
          {"floor", floor},
          {"direction", direction},
          {"spec", "brace_45x90"},
          {"start", {0, 0}},
          {"end", direction == "X" ? json{length, 0} : json{0, length}},
          {"height_mm", 3000},
          {"verified", true}};
}
json sample() {
  return {{"height_m", 6},
          {"co_designated", false},
          {"wind_coefficient_cm_m2", 50},
          {"input_basis", "計算テストの仮想入力（実建物ではない）"},
          {"floors",
           {{{"id", "1"},
             {"name", "1階"},
             {"area_m2", 50},
             {"weight_kn", 98},
             {"wind_x_m2", 30},
             {"wind_y_m2", 6},
             {"walls_complete", true}}}},
          {"walls", {wall("x", "X"), wall("y", "Y", 4000)}}};
}
struct WallQuantity : testing::Test {
  Modeler m;
  json report(const json& input, json options = json::object()) {
    options["inputs"] = input;
    const auto result = construction::checkWallQuantity(m.document(), options.dump());
    EXPECT_TRUE(result) << result.error();
    return result ? json::parse(*result) : json::object();
  }
};
}  // namespace
TEST_F(WallQuantity, EarthquakeWindAndWallSumsMatchHandCalculation) {
  const auto r = report(sample());
  ASSERT_TRUE(r["complete"]);
  EXPECT_DOUBLE_EQ(r["rows"][0]["earthquake_m"].get<double>(), 10);
  EXPECT_DOUBLE_EQ(r["rows"][0]["wind_m"].get<double>(), 15);
  EXPECT_DOUBLE_EQ(r["rows"][0]["required_m"].get<double>(), 15);
  EXPECT_DOUBLE_EQ(r["rows"][0]["existing_m"].get<double>(), 20);
  EXPECT_EQ(r["rows"][0]["status"], "pass");
  EXPECT_DOUBLE_EQ(r["rows"][1]["required_m"].get<double>(), 10);
  EXPECT_DOUBLE_EQ(r["rows"][1]["existing_m"].get<double>(), 8);
  EXPECT_EQ(r["rows"][1]["status"], "fail");
}
TEST_F(WallQuantity, SecondFloorUsesCumulativeWeightAiAndDesignatedCo) {
  auto in = sample();
  in["co_designated"] = true;
  in["floors"].push_back({{"id", "2"},
                          {"area_m2", 25},
                          {"weight_kn", 49},
                          {"wind_x_m2", 0},
                          {"wind_y_m2", 0},
                          {"walls_complete", true}});
  in["walls"].push_back(wall("2x", "X", 10000, "2"));
  in["walls"].push_back(wall("2y", "Y", 10000, "2"));
  const auto r = report(in);
  const double ai = 1 + (std::sqrt(2.) - .5) * .36 / 1.54;
  EXPECT_NEAR(r["rows"][2]["ai"].get<double>(), ai, 1e-12);
  EXPECT_NEAR(r["rows"][2]["earthquake_m"].get<double>(), ai * .3 * 49 / 1.96, 1e-12);
  EXPECT_NEAR(r["rows"][0]["earthquake_m"].get<double>(), 15, 1e-12);
  in["floors"][1]["weight_kn"] = 99;
  EXPECT_EQ(report(in)["rows"][0]["status"], "not_evaluated");
}
TEST_F(WallQuantity, ExactEqualityPassesWithoutAcceptingAMillimetreShortage) {
  auto in = sample();
  in["walls"][1]["end"] = {0, 5000};
  EXPECT_EQ(report(in)["rows"][1]["status"], "pass");
  in["walls"][1]["end"] = {0, 4999.999};
  EXPECT_EQ(report(in)["rows"][1]["status"], "fail");
}
TEST_F(WallQuantity, BraceHeightCorrectionAndPlywoodHaveDifferentConditions) {
  auto in = sample();
  in["walls"] = json::array({wall("x", "X", 910)});
  in["walls"][0]["height_mm"] = 3500;
  auto r = report(in);
  EXPECT_NEAR(r["walls"][0]["ratio"].get<double>(), 1.82, 1e-12);
  EXPECT_NEAR(r["walls"][0]["effective_m"].get<double>(), .91 * 1.82, 1e-12);
  in["walls"][0]["height_mm"] = 3200;
  EXPECT_DOUBLE_EQ(report(in)["walls"][0]["ratio"].get<double>(), 2);
  in["walls"][0]["spec"] = "plywood_n50_150";
  in["walls"][0]["height_mm"] = nullptr;
  EXPECT_DOUBLE_EQ(report(in)["walls"][0]["ratio"].get<double>(), 2.5);
}
TEST_F(WallQuantity, MissingInputsAndTablesNeverTurnIntoZeroOrGreen) {
  for (const auto* key : {"height_m", "co_designated", "wind_coefficient_cm_m2"}) {
    auto in = sample();
    in.erase(key);
    EXPECT_EQ(report(in)["rows"][0]["status"], "not_evaluated");
  }
  auto in = sample();
  in["table_snapshot"] = nullptr;
  auto r = report(in);
  EXPECT_TRUE(r["rows"][0]["required_m"].is_null());
  EXPECT_EQ(r["rows"][0]["status"], "not_evaluated");
  EXPECT_NE(r["notice"].get<std::string>().find("表がありません"), std::string::npos);
  in["table_snapshot"] = json::parse(construction::wallTables());
  in["table_snapshot"]["earthquake"] = nullptr;
  EXPECT_TRUE(report(in)["rows"][0]["earthquake_m"].is_null());
  in = sample();
  in["floors"][0]["wind_x_m2"] = nullptr;
  r = report(in);
  EXPECT_TRUE(r["rows"][0]["required_m"].is_null());
  EXPECT_EQ(r["rows"][1]["status"], "fail");
}
TEST_F(WallQuantity, UnconfirmedSpecificationsAndIncompleteWallsRemainUnknown) {
  auto in = sample();
  in["walls"][0]["verified"] = false;
  auto r = report(in);
  EXPECT_TRUE(r["rows"][0]["existing_m"].is_null());
  EXPECT_TRUE(r["walls"][0]["effective_m"].is_null());
  EXPECT_EQ(r["rows"][0]["status"], "not_evaluated");
  in = sample();
  in["floors"][0]["walls_complete"] = false;
  EXPECT_EQ(report(in)["rows"][0]["status"], "not_evaluated");
  in = sample();
  in["walls"][0]["spec"] = "unknown";
  EXPECT_EQ(report(in)["rows"][0]["status"], "not_evaluated");
}
TEST_F(WallQuantity, DuplicateAndPartiallyOverlappingIntervalsAreNotDoubleCounted) {
  auto in = sample();
  auto other = wall("overlap", "X");
  other["start"] = {12000, 0};
  other["end"] = {8000, 0};
  in["walls"].push_back(other);
  auto r = report(in);
  EXPECT_TRUE(r["walls"][0]["effective_m"].is_null());
  EXPECT_TRUE(r["walls"][2]["effective_m"].is_null());
  EXPECT_EQ(r["rows"][0]["status"], "not_evaluated");
  in["walls"][2]["start"] = {10000, 0};
  in["walls"][2]["end"] = {12000, 0};
  EXPECT_DOUBLE_EQ(report(in)["rows"][0]["existing_m"].get<double>(), 24);
}
TEST_F(WallQuantity, GridReferencesFollowEditsAndMissingAxesStayUnknown) {
  const auto grid = m.createGridLines({0, 0, 0}, {910}, {910});
  ASSERT_TRUE(grid);
  auto spec = *m.gridSpec(grid->grid);
  auto in = sample();
  auto w = wall("grid-wall", "X");
  w.erase("start");
  w.erase("end");
  w["grid"] = grid->grid;
  w["start_axes"] = {spec.x[0].id, spec.y[0].id};
  w["end_axes"] = {spec.x[1].id, spec.y[0].id};
  in["walls"] = json::array({w});
  EXPECT_DOUBLE_EQ(report(in)["walls"][0]["length_mm"].get<double>(), 910);
  spec.x[1].offset = 1200;
  ASSERT_TRUE(m.setGrid(grid->grid, spec));
  EXPECT_DOUBLE_EQ(report(in)["walls"][0]["length_mm"].get<double>(), 1200);
  in["walls"][0]["end_axes"][0] = "deleted-axis";
  EXPECT_EQ(report(in)["rows"][0]["status"], "not_evaluated");
}
TEST_F(WallQuantity, DiagonalAndDirectionMismatchAreNotCounted) {
  auto in = sample();
  in["walls"][0]["end"] = {1000, 1000};
  EXPECT_TRUE(report(in)["walls"][0]["effective_m"].is_null());
  in["walls"][0]["end"] = {0, 1000};
  EXPECT_TRUE(report(in)["walls"][0]["effective_m"].is_null());
}
TEST_F(WallQuantity, InputsAndPinnedCustomVersionSaveUndoAndReopen) {
  const auto before = m.document().settings().wallCalculation;
  ASSERT_TRUE(m.setWallCalculation(sample().dump()));
  const auto builtin = json::parse(m.document().settings().wallCalculation);
  ASSERT_TRUE(builtin.contains("table_snapshot"));
  EXPECT_TRUE(m.undo());
  EXPECT_EQ(m.document().settings().wallCalculation, before);
  EXPECT_TRUE(m.redo());
  auto custom = builtin;
  custom["table_snapshot"]["version"] = "office-v1";
  custom["table_snapshot"]["normative"] = "user";
  custom["table_snapshot"]["source"]["name"] = "試験用の独自表";
  custom["table_snapshot"]["entries"]["brace_45x90"]["ratio"] = 1;
  ASSERT_TRUE(m.setWallCalculation(custom.dump()));
  Document restored;
  ASSERT_TRUE(deserializeDocument(serializeDocument(m.document()), restored));
  const auto r = construction::checkWallQuantity(restored);
  ASSERT_TRUE(r);
  EXPECT_EQ(json::parse(*r)["table_snapshot"]["version"], "office-v1");
  EXPECT_DOUBLE_EQ(json::parse(*r)["rows"][0]["existing_m"].get<double>(), 10);
  EXPECT_DOUBLE_EQ(json::parse(construction::wallTables())["entries"]["brace_45x90"]["ratio"].get<double>(),
                   2);
}
TEST_F(WallQuantity, InvalidReplacementIsAtomicAndProvenanceIsRequired) {
  ASSERT_TRUE(m.setWallCalculation(sample().dump()));
  const auto saved = serializeDocument(m.document());
  const auto revision = m.history().revision();
  std::vector<json> bad;
  auto in = sample();
  in["floors"][0]["weight_kn"] = -1;
  bad.push_back(in);
  in = sample();
  in["walls"].push_back(in["walls"][0]);
  bad.push_back(in);
  in = sample();
  in["floors"].push_back(in["floors"][0]);
  bad.push_back(in);
  in = sample();
  in["table_snapshot"] = json::parse(construction::wallTables());
  in["table_snapshot"].erase("basis");
  bad.push_back(in);
  in = sample();
  in["unknown"] = 1;
  bad.push_back(in);
  in = sample();
  in["walls"][0]["start"] = {nullptr, 0};
  bad.push_back(in);
  for (const auto& value : bad) {
    EXPECT_FALSE(m.setWallCalculation(value.dump()));
    EXPECT_EQ(m.history().revision(), revision);
    EXPECT_EQ(serializeDocument(m.document()), saved);
  }
}
TEST_F(WallQuantity, StaleTablesWarnAndFutureTablesCannotEarnPass) {
  const auto in = sample();
  auto r = report(in, {{"as_of", "2027-11-05"}});
  EXPECT_TRUE(r["stale_table"]);
  EXPECT_EQ(r["rows"][0]["status"], "pass");
  EXPECT_NE(r["notice"].get<std::string>().find("1年以上"), std::string::npos);
  r = report(in, {{"as_of", "2025-04-01"}});
  EXPECT_EQ(r["rows"][0]["status"], "not_evaluated");
  EXPECT_FALSE(construction::checkWallQuantity(m.document(), R"({"as_of":"2026-02-30"})"));
}
TEST_F(WallQuantity, ReadOnlyApiRevisionCsvAndDiagnosticCarrySameBasis) {
  ASSERT_TRUE(m.setWallCalculation(sample().dump()));
  api::CommandApi api(m);
  const auto saved = serializeDocument(m.document());
  const auto revision = m.history().revision();
  const std::string command = R"({"command":"check_wall_quantity","args":{"include_csv":true}})";
  EXPECT_EQ(api.effectOf(command), api::CommandApi::Effect::Read);
  const auto r = json::parse(api.execute(command));
  ASSERT_TRUE(r["ok"]);
  EXPECT_EQ(r["result"]["revision"], revision);
  const std::string csv = r["result"]["csv"];
  EXPECT_EQ(csv.substr(0, 3), "\xef\xbb\xbf");
  EXPECT_NE(csv.find("2025-04-01"), std::string::npos);
  EXPECT_NE(csv.find("jp-post-beam-2025-04-01-v1"), std::string::npos);
  const auto diagnostic = construction::checkFrame(m.document(), R"({"rules":["LAT-01"]})");
  ASSERT_TRUE(diagnostic);
  const auto rows = json::parse(*diagnostic)["results"];
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0]["status"], "pass");
  EXPECT_EQ(rows[1]["status"], "fail");
  EXPECT_EQ(rows[0]["wall_table"], r["result"]["table_snapshot"]);
  EXPECT_EQ(serializeDocument(m.document()), saved);
  EXPECT_EQ(m.history().revision(), revision);
}
TEST_F(WallQuantity, GeometrySheetsCarryBasisDatesButDoNotClaimWallVerdict) {
  const auto box = m.createRectangle(geom::SketchPlane::ground(), 2000, 2000);
  ASSERT_TRUE(box);
  ASSERT_TRUE(m.extrude(*box, 3000));
  ASSERT_TRUE(m.setWallCalculation(sample().dump()));
  const auto view = draw::makeView(m.document(), draw::planView(1500));
  ASSERT_TRUE(view);
  const auto page = draw::layoutSheet(*view, {});
  ASSERT_TRUE(page);
  std::string notes;
  for (const auto& i : page->items)
    if (i.pen == draw::Pen::Text) notes += i.text;
  EXPECT_NE(notes.find("2025-04-01"), std::string::npos);
  EXPECT_NE(notes.find("2026-11-04"), std::string::npos);
  EXPECT_NE(notes.find("壁量の結果"), std::string::npos);
}
TEST_F(WallQuantity, DeletedOrDifferentFrameCannotKeepGreen) {
  auto in = sample();
  in["frame"] = "deleted-frame";
  EXPECT_EQ(report(in)["rows"][0]["status"], "not_evaluated");
  EXPECT_EQ(report(sample(), {{"frame", "other-frame"}})["rows"][0]["status"], "not_evaluated");
}
