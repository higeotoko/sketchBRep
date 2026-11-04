#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <set>
#include "sbr/ConstructionFrame.h"
#include "sbr/ConstructionQuantity.h"
#include "sbr/ConstructionCheck.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/draw/Drawing.h"

using namespace sbr;
using namespace sbr::doc;
using nlohmann::json;
namespace {
struct TimberWorkflow : testing::Test {
  Modeler m;
  json make(json inputs) {
    const auto made = m.generateFrame(json{{"inputs", inputs}}.dump());
    EXPECT_TRUE(made) << made.error();
    return made ? json::parse(*made) : json::object();
  }
  std::pair<gp_Pnt, gp_Pnt> axis(const Object& o) {
    const auto expanded = construction::frameMemberObject(m.document(), o);
    const Object& part = expanded ? *expanded : o;
    const auto& member = std::get<LinearMember>(part.recipe.base);
    return {member.start.Transformed(m.document().worldTransform(part)),
            member.end.Transformed(m.document().worldTransform(part))};
  }
  json quantity() {
    const auto r = construction::takeOff(m.document());
    EXPECT_TRUE(r) << r.error();
    return r ? json::parse(*r) : json::object();
  }
};
json aperture(int floor = 2) { return {{"storey", floor}, {"x", {2, 4}}, {"y", {1, 3}}}; }
}  // namespace
TEST_F(TimberWorkflow, StairwellRemovesSheetsAndCrossingMembersAndKeepsRecipesEditable) {
  make({{"x_bays", 6}, {"y_bays", 4}, {"floor_openings", json::array({aperture()})}});
  int sheets = 0, trimmers = 0;
  for (const auto& o : m.document().objects()) {
    if (o.layer != "2階床組" || !o.attributes.count("kind")) continue;
    const auto role = o.attributes.at("kind");
    if (role == "floor_sheathing") ++sheets;
    if (role != "floor_sheathing" && role != "floor_beam" && role != "rim_beam") continue;
    const auto [a, b] = axis(o);
    const auto mid = gp_Pnt((a.X() + b.X()) / 2, (a.Y() + b.Y()) / 2, (a.Z() + b.Z()) / 2);
    EXPECT_FALSE(mid.X() > 1820 + 1e-6 && mid.X() < 3640 - 1e-6 && mid.Y() > 910 + 1e-6 &&
                 mid.Y() < 2730 - 1e-6);
    if (o.attributes.at("frame_key").find("floor/opening/") == 0) ++trimmers;
    EXPECT_TRUE(m.memberInfo(o.id));
  }
  EXPECT_EQ(sheets, 20);
  // The grid beams already bound this aperture, so adding another ring would
  // double both geometry and material quantities.
  EXPECT_EQ(trimmers, 0);
  EXPECT_TRUE(quantity()["complete"]);
  ASSERT_TRUE(m.undo());
  EXPECT_TRUE(m.document().objects().empty());
  ASSERT_TRUE(m.redo());
  EXPECT_TRUE(quantity()["complete"]);
}
TEST_F(TimberWorkflow, FirstFloorJoistsSplitAroundApertureAndReceiveBoundaryBeams) {
  make({{"x_bays", 6}, {"y_bays", 4}, {"floor_openings", json::array({aperture(1)})}});
  int split = 0, edges = 0;
  for (const auto& o : m.document().objects()) {
    if (o.layer != "1階床組" || !o.attributes.count("kind")) continue;
    const auto key = o.attributes.at("frame_key");
    if (o.attributes.at("kind") == "joist" && key.find("/opening/") != std::string::npos) {
      ++split;
      const auto [a, b] = axis(o);
      EXPECT_TRUE(std::max(a.Y(), b.Y()) <= 910 + 1e-6 || std::min(a.Y(), b.Y()) >= 2730 - 1e-6);
    }
    if (o.attributes.at("kind") == "floor_girder" && key.find("floor/opening/") == 0) ++edges;
  }
  EXPECT_GT(split, 0);
  EXPECT_GT(edges, 0);
  EXPECT_TRUE(quantity()["complete"]);
}
TEST_F(TimberWorkflow, SecondFloorPerimeterDoesNotDoubleCountRimAndFloorBeams) {
  make({{"x_bays", 4}, {"y_bays", 3}});
  std::vector<std::pair<gp_Pnt, gp_Pnt>> beams;
  for (const auto& o : m.document().objects())
    if (o.layer == "2階床組" && o.attributes.count("kind") &&
        (o.attributes.at("kind") == "floor_beam" || o.attributes.at("kind") == "rim_beam"))
      beams.push_back(axis(o));
  for (std::size_t i = 0; i < beams.size(); ++i)
    for (std::size_t j = i + 1; j < beams.size(); ++j) {
      const auto [a, b] = beams[i];
      const auto [c, d] = beams[j];
      if (std::abs(a.X() - b.X()) < 1e-6 && std::abs(c.X() - d.X()) < 1e-6 && std::abs(a.X() - c.X()) < 1e-6)
        EXPECT_LE(std::min(std::max(a.Y(), b.Y()), std::max(c.Y(), d.Y())) -
                      std::max(std::min(a.Y(), b.Y()), std::min(c.Y(), d.Y())),
                  1e-6);
      if (std::abs(a.Y() - b.Y()) < 1e-6 && std::abs(c.Y() - d.Y()) < 1e-6 && std::abs(a.Y() - c.Y()) < 1e-6)
        EXPECT_LE(std::min(std::max(a.X(), b.X()), std::max(c.X(), d.X())) -
                      std::max(std::min(a.X(), b.X()), std::min(c.X(), d.X())),
                  1e-6);
    }
}
TEST_F(TimberWorkflow, InvalidAperturesDoNotChangeDocumentOrHistory) {
  for (const auto& holes :
       {json::array({aperture(), aperture()}), json::array({{{"storey", 2}, {"x", {2, 7}}, {"y", {1, 3}}}}),
        json::array({{{"storey", 3}, {"x", {2, 4}}, {"y", {1, 3}}}})}) {
    EXPECT_FALSE(
        m.generateFrame(json{{"inputs", {{"x_bays", 6}, {"y_bays", 4}, {"floor_openings", holes}}}}.dump()));
    EXPECT_TRUE(m.document().objects().empty());
    EXPECT_EQ(m.history().undoCount(), 0u);
  }
}
TEST_F(TimberWorkflow, KeepingAnEditedBeamDoesNotOverlayGeneratedApertureSegments) {
  const auto made = make({{"x_bays", 6}, {"y_bays", 4}});
  std::string edited;
  const std::string key = "floor/2/beam/3";
  for (const auto& o : m.document().objects())
    if (o.attributes.count("frame_key") && o.attributes.at("frame_key") == key) edited = o.id;
  ASSERT_FALSE(edited.empty());
  Modeler::MemberChange change;
  change.depth = 300;
  ASSERT_TRUE(m.changeMember(edited, change));
  const auto result = m.generateFrame(json{{"replace", made["frame"]},
                                           {"on_modified", "preserve"},
                                           {"inputs", {{"floor_openings", json::array({aperture()})}}}}
                                          .dump());
  ASSERT_TRUE(result) << result.error();
  ASSERT_NE(m.document().find(edited), nullptr);
  for (const auto& o : m.document().objects())
    if (o.attributes.count("frame_key")) EXPECT_NE(o.attributes.at("frame_key").find(key + "/opening/"), 0u);
}
TEST_F(TimberWorkflow, HouseWithLowerRoofWindowStairwellAndRemovedColumnSurvivesTheWholeWorkflow) {
  const auto walls = m.createWalls({{0, 0, 600}, {7280, 0, 600}}, 120, 2400);
  ASSERT_TRUE(walls);
  const auto opening = m.placeOpeningAt(walls->front(), gp::DY(), {1820, 0, 1800}, 1200, 1000, 700);
  ASSERT_TRUE(opening);
  const auto made = make({{"x_bays", 8},
                          {"y_bays", 6},
                          {"columns", {{2, 2}}},
                          {"second_floor", {{"x", {0, 6}}, {"y", {0, 4}}}},
                          {"opening_hosts", {*opening}},
                          {"floor_openings", json::array({aperture()})},
                          {"braces", json::array({{{"storey", 1}, {"from", {0, 0}}, {"to", {1, 0}}}})}});
  ASSERT_TRUE(made.contains("frame"));
  std::string column;
  bool lowerRoof = false;
  for (const auto& o : m.document().objects())
    if (o.attributes.count("frame_key")) {
      if (o.attributes.at("frame_key") == "column/2/2/2") column = o.id;
      if (o.attributes.at("frame_key").find("roof/lower/") == 0) lowerRoof = true;
    }
  ASSERT_FALSE(column.empty());
  EXPECT_TRUE(lowerRoof);
  const auto before = quantity();
  ASSERT_TRUE(m.remove({column}));
  EXPECT_EQ(quantity()["totals"]["count"].get<int>(), before["totals"]["count"].get<int>() - 1);
  ASSERT_TRUE(m.setOpeningSize(*opening, {}, 1800, std::nullopt));
  const auto regenerated =
      m.generateFrame(json{{"replace", made["frame"]}, {"on_modified", "preserve"}}.dump());
  ASSERT_TRUE(regenerated) << regenerated.error();
  for (const auto& o : m.document().objects())
    if (o.attributes.count("frame_key")) {
      EXPECT_NE(o.attributes.at("frame_key"), "column/2/2/2");
      if (o.attributes.count("kind") && o.attributes.at("kind") == "lintel") {
        const auto [a, b] = axis(o);
        EXPECT_NEAR(a.Distance(b), 1800, 1e-6);
      }
    }
  const auto q = quantity();
  EXPECT_TRUE(q["complete"]);
  const auto checked = construction::checkFrame(m.document());
  ASSERT_TRUE(checked);
  EXPECT_GT(json::parse(*checked)["counts"]["not_evaluated"].get<int>(), 0);
  const auto bytes = serializeDocument(m.document());
  Document reopened;
  ASSERT_TRUE(deserializeDocument(bytes, reopened));
  const auto q2 = construction::takeOff(reopened);
  ASSERT_TRUE(q2);
  EXPECT_EQ(json::parse(*q2)["totals"], q["totals"]);
  for (const auto* preset : {"foundation", "first_floor", "second_floor", "roof"}) {
    const auto spec = draw::framingView(reopened, preset);
    ASSERT_TRUE(spec);
    const auto view = draw::makeView(reopened, *spec);
    ASSERT_TRUE(view);
    const auto sheet = draw::layoutSheet(*view, {});
    ASSERT_TRUE(sheet);
    EXPECT_FALSE(sheet->items.empty());
  }
  EXPECT_EQ(serializeDocument(m.document()), bytes);
}
