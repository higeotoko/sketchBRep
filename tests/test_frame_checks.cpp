#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "sbr/ConstructionCheck.h"
#include "sbr/ConstructionFrame.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"

using namespace sbr;
using namespace sbr::doc;
using nlohmann::json;
namespace {
struct FrameChecks : testing::Test {
  Modeler m;
  api::CommandApi api{m};
  json check(json options = json::object()) {
    const auto result = construction::checkFrame(m.document(), options.dump());
    EXPECT_TRUE(result) << result.error();
    return result ? json::parse(*result) : json::object();
  }
  json make(json inputs = {{"x_bays", 3}, {"y_bays", 3}, {"columns", {{1, 1}}}}) {
    const auto result = m.generateFrame(json{{"inputs", inputs}}.dump());
    EXPECT_TRUE(result) << result.error();
    return result ? json::parse(*result) : json::object();
  }
  std::string status(const json& report, const std::string& rule, const std::string& id = "") {
    for (const auto& row : report.at("results"))
      if (row.at("rule") == rule &&
          (id.empty() ||
           std::find(row.at("targets").begin(), row.at("targets").end(), id) != row.at("targets").end()))
        return row.at("status");
    return "missing";
  }
  ObjectId member(gp_Pnt a, gp_Pnt b, const std::string& role = "beam", double depth = 100) {
    Modeler::MemberSpec spec;
    spec.start = a;
    spec.end = b;
    spec.section.width = 100;
    spec.section.depth = depth;
    spec.role = role;
    spec.materialClass = "timber";
    const auto result = m.createMember(spec);
    EXPECT_TRUE(result) << result.error();
    return result ? *result : "";
  }
  void move(const ObjectId& id, double x, double y, double z = 0) {
    gp_Trsf t;
    t.SetTranslation(gp_Vec(x, y, z));
    ASSERT_TRUE(m.transform({id}, t, "Free move"));
  }
};
}  // namespace
TEST_F(FrameChecks, EmptyAndMissingInformationNeverEarnGreenResults) {
  const auto empty = check();
  EXPECT_EQ(empty["counts"]["pass"], 0);
  const auto id = member({0, 0, 0}, {0, 0, 3000}, "column");
  const auto report = check({{"rules", {"MOD-01", "TMB-01", "LAT-*", "SEC-*", "TMB-04"}}});
  EXPECT_EQ(status(report, "MOD-01", id), "not_evaluated");
  EXPECT_EQ(status(report, "TMB-01", id), "not_evaluated");
  EXPECT_EQ(status(report, "LAT-01"), "not_evaluated");
  EXPECT_EQ(status(report, "TMB-04"), "not_evaluated");
  EXPECT_EQ(status(report, "SEC-01"), "not_applicable");
  for (const auto& row : report["results"]) {
    EXPECT_TRUE(row.contains("reason_code"));
    EXPECT_TRUE(row.contains("measured"));
    EXPECT_TRUE(row.contains("required"));
    EXPECT_TRUE(row.contains("source"));
    EXPECT_EQ(row["checker_version"], "construction-check-1");
  }
}
TEST_F(FrameChecks, UpperColumnMoveChangesGridAndBearingWithoutBlockingAndUndoRestores) {
  make();
  ObjectId id;
  for (const auto& o : m.document().objects())
    if (o.attributes.count("frame_key") && o.attributes.at("frame_key") == "column/1/1/2") id = o.id;
  ASSERT_FALSE(id.empty());
  const json options = {{"rules", {"MOD-01", "TMB-01"}}};
  const auto before = check(options);
  EXPECT_EQ(status(before, "MOD-01", id), "pass");
  EXPECT_EQ(status(before, "TMB-01", id), "pass");
  move(id, 200, 200);
  const auto after = check(options);
  EXPECT_EQ(status(after, "MOD-01", id), "fail");
  EXPECT_EQ(status(after, "TMB-01", id), "fail");
  ASSERT_TRUE(m.undo());
  const auto undone = check(options);
  EXPECT_EQ(status(undone, "MOD-01", id), "pass");
  EXPECT_EQ(status(undone, "TMB-01", id), "pass");
}
TEST_F(FrameChecks, RealSolidIntersectionButNotBoundingBoxOverlapOrEndJoint) {
  const auto a = member({0, 0, 0}, {1000, 0, 0});
  const auto b = member({500, -500, 0}, {500, 500, 0});
  EXPECT_EQ(status(check({{"rules", {"COM-01"}}}), "COM-01", a), "fail");
  move(b, 500, 0);
  const auto end = check({{"rules", {"COM-01"}}});
  EXPECT_EQ(end["counts"]["fail"], 0);
  EXPECT_EQ(status(end, "COM-01"), "not_evaluated");
  // Parallel duplicate members cannot be excused as an end connection.
  member({0, 0, 0}, {1000, 0, 0});
  EXPECT_GT(check({{"rules", {"COM-01"}}})["counts"]["fail"].get<int>(), 0);
}
TEST_F(FrameChecks, CollisionBudgetReportsIncompleteInsteadOfPass) {
  for (int i = 0; i < 4; ++i) member({0, 0, 0}, {1000, 0, 0});
  const auto report = check({{"rules", {"COM-01"}}, {"pair_budget", 1}});
  EXPECT_FALSE(report["complete"]);
  EXPECT_GT(report["counts"]["not_evaluated"].get<int>(), 0);
  EXPECT_EQ(report["counts"]["pass"], 0);
}
TEST_F(FrameChecks, BeamContactStockLengthAndThroughColumnContinuityAreIndependent) {
  const auto column = member({0, 0, -1000}, {0, 0, -50}, "column");
  const auto beam = member({0, 0, 0}, {5000, 0, 0});
  const auto report = check({{"rules", {"COM-02", "COM-03", "TMB-02"}}});
  EXPECT_EQ(status(report, "COM-03", beam), "fail");
  EXPECT_EQ(status(report, "TMB-02", beam), "pass");  // start bears on the column
  bool unsupportedEnd = false;
  for (const auto& row : report["results"])
    if (row["rule"] == "TMB-02" && row["status"] == "fail") unsupportedEnd = true;
  EXPECT_TRUE(unsupportedEnd);
  make();
  ObjectId through;
  for (const auto& o : m.document().objects())
    if (o.attributes.count("kind") && o.attributes.at("kind") == "through_column") {
      through = o.id;
      break;
    }
  EXPECT_EQ(status(check({{"rules", {"TMB-03"}}}), "TMB-03", through), "pass");
  move(through, 0, 0, 2000);
  EXPECT_EQ(status(check({{"rules", {"TMB-03"}}}), "TMB-03", through), "fail");
}
TEST_F(FrameChecks, AllocationChecksMovedAndMissingMembersAndSurvivesSave) {
  make({{"x_bays", 2}, {"y_bays", 2}, {"spacing", {{"joist", {{"mode", "fixed_pitch"}, {"pitch", 303}}}}}});
  ObjectId joist;
  for (const auto& o : m.document().objects())
    if (o.attributes.count("kind") && o.attributes.at("kind") == "joist") {
      joist = o.id;
      break;
    }
  const auto before = check({{"rules", {"MOD-02"}}});
  EXPECT_EQ(status(before, "MOD-02", joist), "pass");
  move(joist, 50, 0);
  EXPECT_EQ(status(check({{"rules", {"MOD-02"}}}), "MOD-02", joist), "fail");
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.remove({joist}));
  auto report = check({{"rules", {"MOD-02"}}});
  EXPECT_GT(report["counts"]["fail"].get<int>(), 0);
  const auto bytes = serializeDocument(m.document());
  Document restored;
  ASSERT_TRUE(deserializeDocument(bytes, restored));
  const auto reopened = construction::checkFrame(restored, R"({"rules":["MOD-02"]})");
  ASSERT_TRUE(reopened);
  EXPECT_EQ(json::parse(*reopened)["counts"], report["counts"]);
}
TEST_F(FrameChecks, APIIsReadOnlyAndRejectsBadOptionsAndCancellation) {
  make({{"x_bays", 2}, {"y_bays", 2}});
  const auto rev = m.history().revision();
  const auto undos = m.history().undoCount();
  const auto bytes = serializeDocument(m.document());
  const std::string request = R"({"command":"check_frame","args":{"rules":["MOD-01","TMB-03"]}})";
  EXPECT_EQ(api.effectOf(request), api::CommandApi::Effect::Read);
  const auto response = json::parse(api.execute(request));
  ASSERT_TRUE(response["ok"]);
  EXPECT_EQ(response["result"]["revision"], rev);
  EXPECT_EQ(m.history().revision(), rev);
  EXPECT_EQ(m.history().undoCount(), undos);
  EXPECT_EQ(serializeDocument(m.document()), bytes);
  EXPECT_FALSE(construction::checkFrame(m.document(), R"({"rules":["MISSING"]})"));
  EXPECT_FALSE(construction::checkFrame(m.document(), R"({"frame":"missing"})"));
  EXPECT_FALSE(construction::checkFrame(m.document(), R"({"system":"rc"})"));
  std::atomic<bool> cancel{true};
  EXPECT_FALSE(construction::checkFrame(m.document(), "{}", &cancel));
}
TEST_F(FrameChecks, StandardThousandPartFrameCompletesAndRecordsTime) {
  make({{"x_bays", 10}, {"y_bays", 8}});
  const auto report = check();
  EXPECT_TRUE(report["complete"]);
  EXPECT_GT(report["members"].get<int>(), 400);
  EXPECT_GT(report["counts"]["not_evaluated"].get<int>(), 0);
  std::cout << "Standard frame check ms: " << report["elapsed_ms"] << " counts: " << report["counts"] << '\n';
}

TEST_F(FrameChecks, OverlappingBoundingBoxesDoNotProveCollision) {
  member({0, 0, 0}, {1000, 1000, 0});
  member({0, 150, 0}, {1000, 1150, 0});
  const auto report = check({{"rules", {"COM-01"}}});
  EXPECT_EQ(report["counts"]["fail"], 0);
  EXPECT_EQ(status(report, "COM-01"), "pass");
}
TEST_F(FrameChecks, RotatedParentAndExplicitModuleExceptionUseDesignCoordinates) {
  const auto made = make();
  gp_Trsf rotation;
  rotation.SetRotation(gp_Ax1(gp::Origin(), gp::DZ()), 0.4);
  ASSERT_TRUE(m.transform({made.at("frame").get<std::string>()}, rotation, "Rotate frame"));
  const auto report = check({{"rules", {"MOD-01", "MOD-02"}}});
  EXPECT_EQ(report["counts"]["fail"], 0);
  ObjectId id;
  for (const auto& o : m.document().objects())
    if (o.attributes.count("kind") && o.attributes.at("kind") == "through_column") {
      id = o.id;
      break;
    }
  ASSERT_TRUE(m.setAttribute({id}, "module_exception", "意匠上の位置を優先"));
  move(id, 100, 100);
  EXPECT_EQ(status(check({{"rules", {"MOD-01"}}}), "MOD-01", id), "not_applicable");
}

TEST_F(FrameChecks, ScopedChecksKeepExternalBearingsAndCollisions) {
  const auto made = make();
  ObjectId upper;
  for (const auto& o : m.document().objects())
    if (o.attributes.count("frame_key") && o.attributes.at("frame_key") == "column/1/1/2") upper = o.id;
  ASSERT_FALSE(upper.empty());
  move(upper, 200, 200);
  const auto expanded = construction::frameMemberObject(m.document(), *m.document().find(upper));
  ASSERT_TRUE(expanded);
  const auto& recipe = std::get<LinearMember>(expanded->recipe.base);
  const auto base =
      recipe.start.Transformed(m.document().parentTransform(*m.document().find(upper)) * expanded->transform);
  const auto external =
      member({base.X() - 500, base.Y(), base.Z() - 50}, {base.X() + 500, base.Y(), base.Z() - 50});
  const auto report = check({{"frame", made.at("frame")}, {"rules", {"TMB-01"}}});
  EXPECT_EQ(status(report, "TMB-01", upper), "pass");
  member({base.X(), base.Y(), base.Z()}, {base.X(), base.Y(), base.Z() + 1000}, "column");
  const auto collision = check({{"frame", made.at("frame")}, {"rules", {"COM-01"}}});
  EXPECT_GT(collision["counts"]["fail"].get<int>(), 0);
}
TEST_F(FrameChecks, LegacyAllocationAndSingleStoreyAreNotGuessed) {
  make({{"x_bays", 2}, {"y_bays", 2}, {"storeys", 1}});
  const auto single = check({{"rules", {"TMB-01"}}});
  EXPECT_EQ(single["counts"]["pass"], 0);
  EXPECT_EQ(single["counts"]["not_evaluated"], 0);
  EXPECT_GT(single["counts"]["not_applicable"].get<int>(), 0);
  for (const auto& source : m.document().objects()) {
    const auto* g = std::get_if<GroupNode>(&source.recipe.base);
    if (!g || g->constructionFrame.empty()) continue;
    auto edited = source;
    auto& group = std::get<GroupNode>(edited.recipe.base);
    auto manifest = json::parse(group.constructionFrame);
    manifest.erase("allocation");
    group.constructionFrame = manifest.dump();
    m.document().replace(edited);
    break;
  }
  const auto legacy = check({{"rules", {"MOD-02"}}});
  EXPECT_EQ(legacy["counts"]["pass"], 0);
  EXPECT_GT(legacy["counts"]["not_evaluated"].get<int>(), 0);
}

TEST_F(FrameChecks, ShallowOverlapStillUsesVolumeThreshold) {
  const auto a = member({0, 0, 0}, {1000, 0, 0});
  member({0, 99.5, 0}, {1000, 99.5, 0});
  const auto report = check({{"rules", {"COM-01"}}});
  EXPECT_TRUE(report["complete"]);
  EXPECT_EQ(status(report, "COM-01", a), "fail");
  EXPECT_GT(report["counts"]["fail"].get<int>(), 0);
}

// The recipe's own frames have no member collisions: rafters sit on their supports,
// eave lines have no duplicate purlins or ties, and second-floor cross beams frame into
// the floor beams bay by bay. Joints at member ends (ridge rafters included) stay
// "end connection, specification not evaluated", never a pass.
TEST_F(FrameChecks, GeneratedFramesHaveNoMemberCollisions) {
  for (const json inputs : {json{{"x_bays", 4}, {"y_bays", 4}}, json{{"x_bays", 6}, {"y_bays", 4}, {"roof", "shed"}},
                            json{{"x_bays", 4}, {"y_bays", 4}, {"storeys", 1}}}) {
    Modeler fresh;
    const auto made = fresh.generateFrame(json{{"inputs", inputs}}.dump());
    ASSERT_TRUE(made) << made.error();
    const auto result = construction::checkFrame(fresh.document(), R"({"rules":["COM-01"]})");
    ASSERT_TRUE(result) << result.error();
    const auto report = json::parse(*result);
    for (const auto& row : report.at("results")) {
      if (!row.at("targets").empty()) ADD_FAILURE() << inputs.dump() << " " << row.dump();
      else {
        EXPECT_EQ(row.at("measured").at("collisions"), 0) << inputs.dump();
        EXPECT_GT(row.at("measured").at("excluded_end_connections").get<int>(), 0) << inputs.dump();
        EXPECT_EQ(row.at("status"), "not_evaluated") << inputs.dump();
      }
    }
  }
}

// Two sloped members meeting at their ends (a ridge): the real shared solid is at the
// ends, so it is an end connection even though their bounding boxes overlap all the way up.
TEST_F(FrameChecks, SlopedMembersMeetingAtTheirEndsAreAnEndConnection) {
  const auto made = make({{"x_bays", 2}, {"y_bays", 2}, {"storeys", 1}});
  const auto report = check({{"rules", {"COM-01"}}});
  for (const auto& row : report.at("results")) EXPECT_TRUE(row.at("targets").empty()) << row.dump();
  // A real mid-span crossing at the same height is still a collision.
  const auto a = member({0, 500, 10000}, {1000, 500, 10000});
  const auto b = member({500, 0, 10000}, {500, 1000, 10000});
  EXPECT_EQ(status(check({{"rules", {"COM-01"}}}), "COM-01", a), "fail");
  (void)made; (void)b;
}
