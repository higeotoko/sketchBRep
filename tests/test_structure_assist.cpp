#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "sbr/ConstructionAssist.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"

using namespace sbr;
using namespace sbr::doc;
using nlohmann::json;
namespace {
// Figures read from the model's shape (docs/87). Hand-checkable models: a box for
// the wind projection, a slab, posts and a beam.
struct StructureAssist : testing::Test {
  Modeler m;
  api::CommandApi api{m};
  json call(const std::string& command, const json& args) {
    const auto r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  }
  json assist(const json& options = json::object()) {
    auto r = construction::structureAssist(m.document(), options.dump());
    EXPECT_TRUE(r) << r.error();
    return r ? json::parse(*r) : json::object();
  }
  void levels() {
    call("set_levels", {{"levels", {{{"name", "1FL"}, {"elevation", 0}}, {{"name", "2FL"}, {"elevation", 3000}}}}});
  }
  json member(const json& start, const json& end, const std::string& role, const json& extra = json::object()) {
    json a = {{"section", "TIMBER-105x105"}, {"start", start}, {"end", end}, {"role", role}, {"material_class", "timber"}};
    a.update(extra);
    return call("create_member", a);
  }
};
}  // namespace

TEST_F(StructureAssist, WindAreasLeaveOutTheBottomOfEachStorey) {
  levels();
  call("create_box", {{"origin", {0, 0, 0}}, {"width", 6000}, {"depth", 4000}, {"height", 6000}});
  const auto before = serializeDocument(m.document());
  const auto r = assist();
  ASSERT_EQ(r["storeys"].size(), 2u);
  ASSERT_EQ(r["wind"].size(), 2u);
  // X (force along X): the YZ silhouette 4000 wide; Y: the XZ silhouette 6000 wide.
  EXPECT_NEAR(r["wind"][0]["wind_x_m2"].get<double>(), 4.0 * (6.0 - 1.35), 0.05);
  EXPECT_NEAR(r["wind"][0]["wind_y_m2"].get<double>(), 6.0 * (6.0 - 1.35), 0.05);
  EXPECT_NEAR(r["wind"][1]["wind_x_m2"].get<double>(), 4.0 * (6.0 - 4.35), 0.05);
  EXPECT_NEAR(r["wind"][1]["wind_y_m2"].get<double>(), 6.0 * (6.0 - 4.35), 0.05);
  EXPECT_EQ(serializeDocument(m.document()), before);  // read-only
}

TEST_F(StructureAssist, FloorSlabAreaAndPlanPerStorey) {
  levels();
  call("create_slab", {{"outline", {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}}}, {"top", 3000}, {"thickness", 28}});
  // L-shaped addition at the same floor: the union counts once where they overlap.
  call("create_slab", {{"outline", {{5000, 0}, {8000, 0}, {8000, 2000}, {5000, 2000}}}, {"top", 3000}, {"thickness", 28}});
  const auto r = assist();
  const auto& f2 = r["floor_slabs"][1];
  ASSERT_EQ(f2["slabs"].size(), 2u);
  EXPECT_NEAR(f2["area_m2"].get<double>(), 24.0 + 3.0 * 2.0 - 1.0 * 2.0, 0.05);
  EXPECT_NEAR(f2["plan"]["x"][1].get<double>(), 8000, 30);
  EXPECT_TRUE(r["floor_slabs"][0]["area_m2"].is_null());  // nothing at 1FL
  ASSERT_EQ(r["suggested_wall_floors"].size(), 2u);
  EXPECT_NEAR(r["suggested_wall_floors"][1]["area_m2"].get<double>(), 28.0, 0.05);
}

TEST_F(StructureAssist, ColumnAlignmentListsPostsStandingOnBeams) {
  levels();
  member({0, 0, 0}, {0, 0, 2760}, "column");
  member({6000, 0, 0}, {6000, 0, 2760}, "column");
  member({0, 0, 3000}, {0, 0, 5700}, "column");
  member({3000, 0, 3000}, {3000, 0, 5700}, "column");  // stands on the beam
  member({0, 0, 3000}, {6000, 0, 3000}, "floor_beam", {{"section", "TIMBER-105x240"}, {"anchor", {{"up", "top"}}}});
  const auto r = assist();
  ASSERT_EQ(r["column_alignment"].size(), 1u);
  const auto& a = r["column_alignment"][0];
  EXPECT_EQ(a["posts"], 2);
  EXPECT_EQ(a["with_post_below"], 1);
  EXPECT_DOUBLE_EQ(a["ratio"].get<double>(), 0.5);
  ASSERT_EQ(a["floating"].size(), 1u);
  EXPECT_NEAR(a["floating"][0]["xy"][0].get<double>(), 3000, 1);
  EXPECT_EQ(a["floating"][0]["status"], "on_beam");
  ASSERT_EQ(a["floating"][0]["stands_on"].size(), 1u);
  EXPECT_EQ(a["floating"][0]["stands_on"][0]["role"], "floor_beam");
}

TEST_F(StructureAssist, BeamSpanBetweenPostsAndEndSupports) {
  levels();
  member({0, 0, 0}, {0, 0, 2760}, "column");
  member({6000, 0, 0}, {6000, 0, 2760}, "column");
  member({0, 0, 3000}, {6000, 0, 3000}, "floor_beam", {{"section", "TIMBER-105x240"}, {"anchor", {{"up", "top"}}}});
  // A beam along Y that butts into the side of the first one at its start.
  member({3000, 52.5, 3000}, {3000, 4000, 3000}, "floor_beam", {{"section", "TIMBER-105x240"}, {"anchor", {{"up", "top"}}}});
  auto r = assist();
  json main, side;
  for (const auto& b : r["beams"]) {
    if (b["axis"] == "X") main = b;
    else side = b;
  }
  ASSERT_FALSE(main.is_null());
  EXPECT_EQ(main["status"], "measured");
  EXPECT_NEAR(main["max_span"].get<double>(), 6000, 1);  // post centre to post centre
  EXPECT_NEAR(main["span_to_depth"].get<double>(), 6000.0 / 240, 0.01);
  ASSERT_FALSE(side.is_null());
  EXPECT_EQ(side["status"], "single_support");  // only its start is held
  EXPECT_FALSE(side["cantilever_end"].is_null());
  // A limit needs its basis; then the beam is judged.
  auto bad = construction::structureAssist(m.document(), json{{"span_depth_limit", 15}}.dump());
  EXPECT_FALSE(bad);
  r = assist({{"span_depth_limit", 15}, {"limit_basis", "テスト用の仮の値"}});
  for (const auto& b : r["beams"])
    if (b["axis"] == "X") EXPECT_EQ(b["status"], "over_limit");
}

TEST_F(StructureAssist, WithoutLevelsAsksForStoreys) {
  call("create_box", {{"origin", {0, 0, 0}}, {"width", 1000}, {"depth", 1000}, {"height", 1000}});
  auto r = assist();
  EXPECT_TRUE(r["storeys"].empty());
  EXPECT_TRUE(r["wind"].empty());
  EXPECT_NE(r["notes"].dump().find("storeys"), std::string::npos);
  r = assist({{"storeys", {{{"id", "1F"}, {"fl", 0}}}}});
  ASSERT_EQ(r["wind"].size(), 1u);
  EXPECT_NEAR(r["wind"][0]["wind_x_m2"].get<double>(), 0.0, 1e-9);  // a 1 m box is below FL + 1.35 m
  // the command is read-only and listed
  const auto out = json::parse(api.execute(R"({"command":"structure_assist","args":{}})"));
  EXPECT_TRUE(out["ok"]);
}
