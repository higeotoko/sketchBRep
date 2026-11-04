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
struct BayFrameExample {
  ObjectId wall, bottom, top;
  std::vector<ObjectId> posts;
};
BayFrameExample bayFrameExample(Modeler& m) {
  BayFrameExample frame;
  const auto walls = m.createWalls({{0, 0, 0}, {3000, 0, 0}}, 120, 3000);
  if (!walls) throw std::runtime_error(walls.error());
  frame.wall = walls->front();
  auto member = [&](gp_Pnt a, gp_Pnt b, const std::string& role) {
    Modeler::MemberSpec spec;
    spec.start = a; spec.end = b; spec.role = role; spec.materialClass = "timber";
    spec.section.width = spec.section.depth = role == "column" ? 105 : 120;
    spec.up = role == "column" ? gp::DY() : gp::DZ();
    if (role != "column") spec.anchor.up = 1;
    const auto result = m.createMember(spec);
    if (!result) throw std::runtime_error(result.error());
    return *result;
  };
  for (double x : {0., 1000., 2000., 3000.}) frame.posts.push_back(member({x, 0, 0}, {x, 0, 2880}, "column"));
  frame.bottom = member({0, 0, 0}, {3000, 0, 0}, "sill");
  frame.top = member({0, 0, 3000}, {3000, 0, 3000}, "beam");
  return frame;
}
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

TEST_F(WallQuantity, BayCandidatesSubtractWindowWidthsAndMergeVerticallySeparateOpenings) {
  auto walls = m.createWalls({{0, 0, 600}, {6000, 0, 600}}, 120, 3000);
  ASSERT_TRUE(walls);
  auto cut = m.placeOpeningAt(walls->front(), gp::DY(), {2000, 60, 1500}, 1000, 800, 600);
  ASSERT_TRUE(cut);
  cut = m.placeOpeningAt(*cut, gp::DY(), {2000, 60, 2800}, 1000, 500, 1900);
  ASSERT_TRUE(cut);
  cut = m.placeOpeningAt(*cut, gp::DY(), {4500, 60, 1500}, 1000, 2100, 0);
  ASSERT_TRUE(cut);
  const auto before = m.history().undoCount();
  const auto sequence = m.document().sequence();
  auto result = construction::wallBayCandidates(m);
  ASSERT_TRUE(result) << result.error();
  auto data = json::parse(*result);
  ASSERT_EQ(data["bays"].size(), 3);
  const auto& bays = data["bays"];
  EXPECT_DOUBLE_EQ(bays[0]["length_mm"], 1500);
  EXPECT_DOUBLE_EQ(bays[1]["length_mm"], 1500);
  EXPECT_DOUBLE_EQ(bays[2]["length_mm"], 1000);
  EXPECT_EQ(bays[0]["start"], (json{0, 0, 600}));
  EXPECT_EQ(bays[1]["start"], (json{2500, 0, 600}));
  EXPECT_DOUBLE_EQ(bays[2]["t1"], 1.0);
  EXPECT_EQ(bays[0]["status"], "candidate_only");
  EXPECT_FALSE(bays[0]["verified"]);
  EXPECT_FALSE(bays[0].contains("ratio"));
  EXPECT_EQ(m.history().undoCount(), before);
  EXPECT_EQ(m.document().sequence(), sequence);
  EXPECT_EQ(m.document().settings().wallCalculation, "{}");
}

TEST_F(WallQuantity, BayCandidateCoordinatesAndSignaturesFollowRotatedParentAndSave) {
  auto walls = m.createWalls({{0, 0, 0}, {6000, 0, 0}}, 120, 3000, -1);
  ASSERT_TRUE(walls);
  auto group = m.group({walls->front()});
  ASSERT_TRUE(group);
  const auto original = json::parse(*construction::wallBayCandidates(m))["bays"][0];
  gp_Trsf transform;
  transform.SetRotation(gp_Ax1(gp::Origin(), gp::DZ()), .4);
  transform.SetTranslationPart(gp_Vec(100, 200, 3000));
  ASSERT_TRUE(m.transform({*group}, transform, "Move group"));
  auto result = construction::wallBayCandidates(m, R"({"elevation":3000})");
  ASSERT_TRUE(result);
  const auto bay = json::parse(*result)["bays"][0];
  const auto expected = gp_Pnt(6000, 0, 0).Transformed(transform);
  EXPECT_NEAR(bay["start"][0].get<double>(), 100, 1e-6);
  EXPECT_NEAR(bay["start"][1].get<double>(), 200, 1e-6);
  EXPECT_NEAR(bay["end"][0].get<double>(), expected.X(), 1e-6);
  EXPECT_NEAR(bay["length_mm"].get<double>(), 6000, 1e-6);
  EXPECT_EQ(bay["direction"], "diagonal");
  EXPECT_NE(bay["source_signature"], original["source_signature"]);
  Modeler restored;
  ASSERT_TRUE(deserializeDocument(serializeDocument(m.document()), restored.document()));
  EXPECT_EQ(json::parse(*construction::wallBayCandidates(restored))["bays"][0], bay);
}

TEST_F(WallQuantity, BayCandidatesRespectElevationVisibilityAndApiReadOnlyEffect) {
  auto lower = m.createWalls({{0, 0, 0}, {6000, 0, 0}}, 120, 3000);
  auto upper = m.createWalls({{0, 0, 3000}, {6000, 0, 3000}}, 120, 3000);
  ASSERT_TRUE(lower && upper);
  api::CommandApi api(m);
  EXPECT_EQ(api.effectOf(R"({"command":"list_wall_bays"})"), api::CommandApi::Effect::Read);
  const auto count = m.history().undoCount();
  const auto response = json::parse(api.execute(R"({"command":"list_wall_bays","args":{"elevation":3000}})"));
  ASSERT_TRUE(response["ok"]);
  ASSERT_EQ(response["result"]["bays"].size(), 1);
  EXPECT_EQ(response["result"]["bays"][0]["host"], upper->front());
  EXPECT_EQ(m.history().undoCount(), count);
  ASSERT_TRUE(m.setLayer(upper->front(), "hidden"));
  m.setTagVisible("hidden", false);
  const auto data = json::parse(*construction::wallBayCandidates(m, R"({"elevation":3000})"));
  EXPECT_TRUE(data["bays"].empty());
  EXPECT_EQ(data["hidden_walls_omitted"], 1);
  EXPECT_TRUE(json::parse(*construction::wallBayCandidates(m, R"({"ids":[]})"))["bays"].empty());
  EXPECT_FALSE(construction::wallBayCandidates(m, R"({"ids":["missing"]})"));
  EXPECT_FALSE(construction::wallBayCandidates(m, R"({"elevation":"bad"})"));
}

TEST_F(WallQuantity, BayCandidatesDoNotIgnoreUnsupportedCircularCuts) {
  auto walls = m.createWalls({{0, 0, 0}, {6000, 0, 0}}, 120, 3000);
  ASSERT_TRUE(walls);
  geom::SketchPlane plane;
  plane.origin = {3000, 100, 1500}; plane.normal = gp::DY(); plane.xDir = gp::DX();
  auto circle = m.createCircle(plane, 200);
  ASSERT_TRUE(circle);
  auto cylinder = m.extrude(*circle, -200);
  ASSERT_TRUE(cylinder);
  auto cut = m.boolean(BooleanKind::Subtract, {walls->front(), *circle});
  ASSERT_TRUE(cut) << cut.error();
  ASSERT_TRUE(m.setAttribute({*cut}, "kind", "wall"));
  const auto data = json::parse(*construction::wallBayCandidates(m));
  EXPECT_TRUE(data["bays"].empty());
  ASSERT_EQ(data["excluded"].size(), 1);
  EXPECT_EQ(data["excluded"][0]["host"], *cut);
}

TEST_F(WallQuantity, BayResizeRecomputesIntervalsWithoutChangingManualCalculation) {
  auto walls = m.createWalls({{0, 0, 0}, {6000, 0, 0}}, 120, 3000);
  ASSERT_TRUE(walls);
  auto cut = m.placeOpeningAt(walls->front(), gp::DY(), {3000, 60, 1000}, 1000, 1000, 700);
  ASSERT_TRUE(cut);
  ASSERT_TRUE(m.setWallCalculation(sample().dump()));
  const auto inputs = m.document().settings().wallCalculation;
  const auto before = json::parse(*construction::wallBayCandidates(m))["bays"][0];
  ASSERT_TRUE(m.setOpeningSize(*cut, {}, 2000, {}, false));
  const auto after = json::parse(*construction::wallBayCandidates(m))["bays"][0];
  EXPECT_DOUBLE_EQ(before["length_mm"], 2500);
  EXPECT_DOUBLE_EQ(after["length_mm"], 2000);
  EXPECT_NE(after["source_signature"], before["source_signature"]);
  EXPECT_EQ(m.document().settings().wallCalculation, inputs);
}

TEST_F(WallQuantity, BayPostsSplitRealIntervalsAndFourMemberContactsRemainUnverified) {
  const auto frame = bayFrameExample(m);
  const auto count = m.history().undoCount(), sequence = m.document().sequence();
  const auto data = json::parse(*construction::wallBayCandidates(m));
  ASSERT_EQ(data["bays"].size(), 3);
  for (std::size_t i = 0; i < 3; ++i) {
    const auto& bay = data["bays"][i];
    EXPECT_DOUBLE_EQ(bay["length_mm"], 1000);
    EXPECT_EQ(bay["supports"]["start_post"]["ids"], json::array({frame.posts[i]}));
    EXPECT_EQ(bay["supports"]["end_post"]["ids"], json::array({frame.posts[i + 1]}));
    EXPECT_EQ(bay["supports"]["top"]["ids"], json::array({frame.top}));
    EXPECT_EQ(bay["supports"]["bottom"]["ids"], json::array({frame.bottom}));
    EXPECT_EQ(bay["frame_geometry"], "touching");
    EXPECT_FALSE(bay["verified"]);
    EXPECT_FALSE(bay.contains("ratio"));
    for (const auto& distance : bay["joint_distances_mm"]) EXPECT_LE(distance.get<double>(), 1.0);
  }
  EXPECT_EQ(json::parse(*construction::wallBayCandidates(m, R"({"split_at_posts":false})"))["bays"].size(), 1);
  EXPECT_EQ(m.history().undoCount(), count);
  EXPECT_EQ(m.document().sequence(), sequence);
  EXPECT_EQ(m.document().settings().wallCalculation, "{}");
}

TEST_F(WallQuantity, BayOpeningBoundariesAndPostsDoNotCreateCandidatesInsideAnOpening) {
  auto frame = bayFrameExample(m);
  const auto cut = m.placeOpeningAt(frame.wall, gp::DY(), {1000, 60, 1000}, 800, 1000, 700);
  ASSERT_TRUE(cut);
  const auto data = json::parse(*construction::wallBayCandidates(m));
  ASSERT_EQ(data["bays"].size(), 3);
  EXPECT_DOUBLE_EQ(data["bays"][0]["length_mm"], 600);
  EXPECT_DOUBLE_EQ(data["bays"][1]["length_mm"], 600);
  EXPECT_DOUBLE_EQ(data["bays"][2]["length_mm"], 1000);
  EXPECT_EQ(data["bays"][0]["supports"]["end_post"]["status"], "missing");
  EXPECT_EQ(data["bays"][1]["supports"]["start_post"]["status"], "missing");
  EXPECT_EQ(data["bays"][2]["frame_geometry"], "touching");
}

TEST_F(WallQuantity, BayPostMovesAndMemberDeletionChangeReferencesAndSignatures) {
  const auto frame = bayFrameExample(m);
  const auto before = json::parse(*construction::wallBayCandidates(m));
  gp_Trsf shift;
  shift.SetTranslation(gp_Vec(0, 300, 0));
  ASSERT_TRUE(m.transform({frame.posts[1]}, shift, "Move post off wall"));
  const auto moved = json::parse(*construction::wallBayCandidates(m));
  ASSERT_EQ(moved["bays"].size(), 2);
  EXPECT_DOUBLE_EQ(moved["bays"][0]["length_mm"], 2000);
  EXPECT_NE(moved["bays"][0]["source_signature"], before["bays"][0]["source_signature"]);
  ASSERT_TRUE(m.remove({frame.top}));
  const auto removed = json::parse(*construction::wallBayCandidates(m));
  EXPECT_EQ(removed["bays"][0]["supports"]["top"]["status"], "missing");
  EXPECT_EQ(removed["bays"][0]["frame_geometry"], "incomplete");
  EXPECT_NE(removed["bays"][0]["source_signature"], moved["bays"][0]["source_signature"]);
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(json::parse(*construction::wallBayCandidates(m))["bays"], moved["bays"]);
}

TEST_F(WallQuantity, BayPartialColumnsGapsDuplicatesAndHiddenMembersAreExplicit) {
  const auto frame = bayFrameExample(m);
  ASSERT_TRUE(m.setLayer(frame.top, "hidden-frame"));
  m.setTagVisible("hidden-frame", false);
  auto data = json::parse(*construction::wallBayCandidates(m));
  EXPECT_EQ(data["bays"][0]["frame_geometry"], "touching");
  Modeler::MemberChange shortPost;
  shortPost.length = 1000;
  ASSERT_TRUE(m.changeMember(frame.posts[0], shortPost));
  data = json::parse(*construction::wallBayCandidates(m));
  EXPECT_EQ(data["bays"][0]["frame_geometry"], "gaps");
  EXPECT_GT(data["bays"][0]["joint_distances_mm"]["start_top"].get<double>(), 1000);
  ASSERT_TRUE(m.undo());
  Modeler::MemberSpec copy;
  copy.start = {1000, 0, 0}; copy.end = {1000, 0, 2880}; copy.role = "column"; copy.materialClass = "timber";
  copy.section.width = copy.section.depth = 105;
  ASSERT_TRUE(m.createMember(copy));
  data = json::parse(*construction::wallBayCandidates(m));
  ASSERT_EQ(data["bays"].size(), 3);
  EXPECT_EQ(data["bays"][0]["supports"]["end_post"]["status"], "ambiguous");
  EXPECT_EQ(data["bays"][0]["frame_geometry"], "ambiguous");
}

TEST_F(WallQuantity, BaySharedMembersRotatedParentAndDefinitionEditsUseCurrentGeometry) {
  auto frame = bayFrameExample(m);
  const auto shared = m.makeComponent({frame.posts[1]}, "Shared post");
  ASSERT_TRUE(shared);
  frame.posts[1] = *shared;
  std::vector<ObjectId> all{frame.wall, frame.bottom, frame.top};
  all.insert(all.end(), frame.posts.begin(), frame.posts.end());
  const auto group = m.group(all);
  ASSERT_TRUE(group);
  gp_Trsf placement;
  placement.SetRotation(gp_Ax1(gp::Origin(), gp::DZ()), .37);
  placement.SetTranslationPart(gp_Vec(100, 200, 3000));
  ASSERT_TRUE(m.transform({*group}, placement, "Place frame"));
  auto data = json::parse(*construction::wallBayCandidates(m));
  ASSERT_EQ(data["bays"].size(), 3);
  EXPECT_EQ(data["bays"][0]["supports"]["end_post"]["ids"], json::array({*shared}));
  EXPECT_EQ(data["bays"][0]["frame_geometry"], "touching");
  const auto* instance = std::get_if<ComponentInstance>(&m.document().find(*shared)->recipe.base);
  ASSERT_TRUE(instance);
  const auto* definition = m.document().findDefinition(instance->definitionId);
  ASSERT_TRUE(definition);
  const auto signature = data["bays"][0]["source_signature"];
  ASSERT_TRUE(m.setDefinitionParameter(instance->definitionId, definition->objects[0].id, "length", 1000));
  data = json::parse(*construction::wallBayCandidates(m));
  EXPECT_EQ(data["bays"][0]["frame_geometry"], "gaps");
  EXPECT_NE(data["bays"][0]["source_signature"], signature);
  Modeler restored;
  ASSERT_TRUE(deserializeDocument(serializeDocument(m.document()), restored.document()));
  EXPECT_EQ(json::parse(*construction::wallBayCandidates(restored))["bays"], data["bays"]);
}

TEST_F(WallQuantity, BayDecorativeLintelCannotStandInForAnUpperFrameMember) {
  const auto frame = bayFrameExample(m);
  ASSERT_TRUE(m.setAttribute({frame.top}, "kind", "lintel"));
  const auto data = json::parse(*construction::wallBayCandidates(m));
  EXPECT_EQ(data["bays"][0]["supports"]["top"]["status"], "missing");
  EXPECT_EQ(data["bays"][0]["frame_geometry"], "incomplete");
}

TEST_F(WallQuantity, BaySplitUpperMembersRemainPartialUntilTheirJointIsAssessed) {
  const auto frame = bayFrameExample(m);
  ASSERT_TRUE(m.remove({frame.top}));
  std::vector<ObjectId> pieces;
  for (double from : {0., 1500.}) {
    Modeler::MemberSpec beam;
    beam.start = {from, 0, 3000}; beam.end = {from + 1500, 0, 3000};
    beam.section.width = beam.section.depth = 120;
    beam.up = gp::DZ(); beam.anchor.up = 1; beam.role = "beam"; beam.materialClass = "timber";
    auto made = m.createMember(beam);
    ASSERT_TRUE(made); pieces.push_back(*made);
  }
  const auto data = json::parse(*construction::wallBayCandidates(m));
  EXPECT_EQ(data["bays"][0]["frame_geometry"], "touching");
  EXPECT_EQ(data["bays"][1]["supports"]["top"]["status"], "partial");
  EXPECT_EQ(data["bays"][1]["supports"]["top"]["partial_ids"], pieces);
  EXPECT_EQ(data["bays"][1]["frame_geometry"], "incomplete");
  EXPECT_EQ(data["bays"][2]["frame_geometry"], "touching");
}

TEST_F(WallQuantity, BayTaggedArbitrarySolidsAreReportedInsteadOfAssumedToBeMembers) {
  const auto frame = bayFrameExample(m);
  ASSERT_TRUE(m.remove({frame.top}));
  geom::SketchPlane plane;
  plane.origin = {0, -60, 2880};
  auto rectangle = m.createRectangle(plane, 3000, 120);
  ASSERT_TRUE(rectangle);
  ASSERT_TRUE(m.extrude(*rectangle, 120));
  ASSERT_TRUE(m.setAttribute({*rectangle}, "kind", "beam"));
  ASSERT_TRUE(m.setAttribute({*rectangle}, "material_class", "timber"));
  const auto data = json::parse(*construction::wallBayCandidates(m));
  ASSERT_EQ(data["unassessed_members"].size(), 1);
  EXPECT_EQ(data["unassessed_members"][0]["id"], *rectangle);
  EXPECT_EQ(data["bays"][0]["supports"]["top"]["status"], "missing");
  EXPECT_EQ(data["bays"][0]["frame_geometry"], "incomplete");
}

TEST_F(WallQuantity, BayMembersInsideLeftOrRightJustifiedWallAreDetectedAwayFromReferenceEdge) {
  for (int justification : {-1, 1}) {
    Modeler model;
    const auto frame = bayFrameExample(model);
    ASSERT_TRUE(model.remove({frame.wall}));
    ASSERT_TRUE(model.createWalls({{0, 0, 0}, {3000, 0, 0}}, 120, 3000, justification));
    std::vector<ObjectId> members = frame.posts;
    members.push_back(frame.bottom); members.push_back(frame.top);
    gp_Trsf toWallCentre;
    toWallCentre.SetTranslation(gp_Vec(0, justification < 0 ? 60 : -60, 0));
    ASSERT_TRUE(model.transform(members, toWallCentre, "Centre frame inside wall"));
    const auto data = json::parse(*construction::wallBayCandidates(model));
    ASSERT_EQ(data["bays"].size(), 3);
    for (const auto& bay : data["bays"]) EXPECT_EQ(bay["frame_geometry"], "touching");
  }
}
