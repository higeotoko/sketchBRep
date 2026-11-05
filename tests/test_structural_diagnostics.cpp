#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "sbr/ConstructionDiagnostics.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"

using namespace sbr;
using namespace sbr::doc;
using nlohmann::json;
namespace {
// The overlay data is read-only and only repeats existing check results.
struct StructuralDiagnostics : testing::Test {
  Modeler m;
  api::CommandApi api{m};
  json diagnose() {
    auto r = construction::structuralDiagnostics(m.document());
    EXPECT_TRUE(r) << r.error();
    return r ? json::parse(*r) : json::object();
  }
  json wallInputs() {
    auto wall = [](std::string id, std::string direction, double length, std::string floor) {
      return json{{"id", id}, {"floor", floor}, {"direction", direction}, {"spec", "brace_45x90"},
                  {"start", {0, 0}}, {"end", direction == "X" ? json{length, 0} : json{0, length}},
                  {"height_mm", 3000}, {"verified", true}};
    };
    return {{"height_m", 6}, {"co_designated", false}, {"wind_coefficient_cm_m2", 50},
            {"input_basis", "計算テストの仮想入力（実建物ではない）"},
            {"floors", {{{"id", "1"}, {"name", "1階"}, {"area_m2", 50}, {"weight_kn", 98}, {"wind_x_m2", 30},
                         {"wind_y_m2", 6}, {"walls_complete", true}}}},
            {"walls", {wall("x", "X", 10000, "1"), wall("y", "Y", 4000, "1")}}};
  }
};
}  // namespace

TEST_F(StructuralDiagnostics, EmptyModelListsUnassessedChecksAndChangesNothing) {
  const auto before = serializeDocument(m.document());
  const auto r = diagnose();
  EXPECT_FALSE(r["verified"]);
  EXPECT_FALSE(r["wall_quantity"]["entered"]);
  EXPECT_TRUE(r["walls"].empty());
  EXPECT_GE(r["unassessed"].size(), 2u);
  EXPECT_NE(r["unassessed"].dump().find("下屋"), std::string::npos);
  EXPECT_NE(r["unassessed"].dump().find("N値"), std::string::npos);
  EXPECT_EQ(serializeDocument(m.document()), before);
}

TEST_F(StructuralDiagnostics, WallRowsAndWallsCarryStatusAndTheLevelElevation) {
  ASSERT_TRUE(m.setWallCalculation(wallInputs().dump()));
  auto r = diagnose();
  ASSERT_TRUE(r["wall_quantity"]["entered"]);
  ASSERT_EQ(r["wall_quantity"]["rows"].size(), 2u);
  EXPECT_EQ(r["wall_quantity"]["rows"][0]["status"], "pass");
  EXPECT_EQ(r["wall_quantity"]["rows"][1]["status"], "fail");
  EXPECT_FALSE(r["wall_quantity"]["notice"].get<std::string>().empty());
  ASSERT_EQ(r["walls"].size(), 2u);
  EXPECT_EQ(r["walls"][0]["row_status"], "pass");
  EXPECT_EQ(r["walls"][1]["row_status"], "fail");
  EXPECT_EQ(r["walls"][0]["start"][2], 0);
  EXPECT_FALSE(r["walls"][0]["elevation_from_level"]);
  // Without a plan the quarter rows are there but not evaluated.
  ASSERT_EQ(r["wall_quantity"]["quarter"].size(), 2u);
  EXPECT_EQ(r["wall_quantity"]["quarter"][0]["status"], "not_evaluated");
  // With a plan, each storey/direction gets its two side parts as bands.
  auto in = wallInputs();
  in["floors"][0]["plan"] = {{"x", {0, 10000}}, {"y", {0, 4000}}};
  ASSERT_TRUE(m.setWallCalculation(in.dump()));
  r = diagnose();
  const auto& q = r["wall_quantity"]["quarter"];
  ASSERT_EQ(q.size(), 2u);
  EXPECT_EQ(q[0]["strips"].size(), 2u);
  EXPECT_EQ(q[0]["status"], "fail");  // the far Y end has no X wall
  EXPECT_EQ(q[0]["strips"][0]["z"], 0);
  EXPECT_FALSE(r["wall_quantity"]["quarter_basis"]["source"].get<std::string>().empty());
  // With a frame, the 1FL level places the wall bands at the first floor.
  ASSERT_TRUE(m.generateFrame(R"({"inputs":{"x_bays":2,"y_bays":2,"storeys":1}})"));
  r = diagnose();
  EXPECT_TRUE(r["walls"][0]["elevation_from_level"]);
  EXPECT_TRUE(r["wall_quantity"]["quarter"][0]["strips"][0]["elevation_from_level"]);
  EXPECT_GT(r["walls"][0]["start"][2].get<double>(), 0);
}

TEST_F(StructuralDiagnostics, FrameAndSupportIssuesBecomeMarkersOnTheirTargets) {
  Modeler::MemberSpec s; s.start = {1500, 1000, 400}; s.end = {1500, 1000, 800};
  s.section.width = s.section.depth = 90; s.role = "floor_post"; s.materialClass = "timber";
  const auto post = m.createMember(s); ASSERT_TRUE(post);
  const auto r = diagnose();
  bool found = false;
  for (const auto& marker : r["markers"])
    if (marker["source"] == "foundation_supports" && marker["targets"][0] == *post) {
      found = true;
      EXPECT_EQ(marker["status"], "missing");
      EXPECT_FALSE(marker["message"].get<std::string>().empty());
    }
  EXPECT_TRUE(found) << r["markers"].dump();
  EXPECT_GE(r["counts"]["support_issues"].get<int>(), 1);
}

TEST_F(StructuralDiagnostics, ApiCommandIsReadOnly) {
  const json request = {{"command", "check_structure"}, {"args", json::object()}};
  EXPECT_EQ(api.effectOf(request.dump()), api::CommandApi::Effect::Read);
  const auto response = json::parse(api.execute(request.dump()));
  ASSERT_TRUE(response["ok"]) << response.dump();
  EXPECT_EQ(response["result"]["version"], "structural-diagnostics-1");
  EXPECT_FALSE(json::parse(api.execute(json{{"command", "check_structure"}, {"args", {{"bogus", 1}}}}.dump()))["ok"]);
}
