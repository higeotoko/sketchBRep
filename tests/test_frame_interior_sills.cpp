#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "sbr/ConstructionFoundation.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"

using namespace sbr;
using namespace sbr::doc;
using nlohmann::json;
namespace {
// Interior sills in the timber recipe: under bearing walls inside the house, so a
// foundation plan can follow them. 4×4 bays of 910 (girders on y=1 and y=3).
struct FrameInteriorSills : testing::Test {
  Modeler m;
  api::CommandApi api{m};
  json call(const char* command, json args) {
    return json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
  }
  json frame(json inputs, json extra = json::object()) {
    json base = {{"x_bays", 4}, {"y_bays", 4}, {"storeys", 1}};
    base.merge_patch(inputs);
    extra["inputs"] = base; extra["preview"] = false; extra["expected_revision"] = m.history().revision();
    const auto r = call("generate_frame", extra);
    EXPECT_TRUE(r.value("ok", false)) << r.dump();
    return r.value("result", json::object());
  }
  std::string refused(json inputs) {
    json base = {{"x_bays", 4}, {"y_bays", 4}, {"storeys", 1}};
    base.merge_patch(inputs);
    const auto r = call("generate_frame", {{"inputs", base}});
    EXPECT_FALSE(r.value("ok", true)) << r.dump();
    return r.value("error", std::string());
  }
  // Plan centres (x, y) of the objects of one kind (members and display-only markers).
  std::vector<gp_Pnt> centres(const std::string& kind) {
    std::vector<gp_Pnt> out;
    for (const auto& o : m.document().objects()) {
      if (!o.attributes.count("kind") || o.attributes.at("kind") != kind) continue;
      const auto b = m.document().worldBounds(o);
      if (b.IsVoid()) continue;
      out.push_back(gp_Pnt((b.CornerMin().X() + b.CornerMax().X()) / 2, (b.CornerMin().Y() + b.CornerMax().Y()) / 2, 0));
    }
    return out;
  }
  int countAtY(const std::vector<gp_Pnt>& points, double y) {
    int n = 0; for (const auto& p : points) n += std::abs(p.Y() - y) < 1; return n;
  }
  bool warned(const json& made, const char* text) {
    for (const auto& w : made.value("warnings", json::array())) if (w.get<std::string>().find(text) != std::string::npos) return true;
    return false;
  }
};
}  // namespace

TEST_F(FrameInteriorSills, ExplicitRunAddsOneSillPerBayWithBolts) {
  const auto plain = frame(json::object());
  const auto sillsBefore = centres("sill").size();
  ASSERT_TRUE(m.undo());
  const auto made = frame({{"interior_sills", {{{"from", {0, 2}}, {"to", {4, 2}}}}}});
  const auto sills = centres("sill");
  EXPECT_EQ(sills.size(), sillsBefore + 4);
  EXPECT_EQ(countAtY(sills, 1820), 4);
  EXPECT_FALSE(warned(made, "筋かい"));
  (void)plain;
}

TEST_F(FrameInteriorSills, GirderOnTheSameLineGivesWayAndLosesItsPostsThere) {
  frame(json::object());
  EXPECT_EQ(countAtY(centres("floor_girder"), 910), 1);
  const int postsBefore = countAtY(centres("floor_post"), 910);
  EXPECT_EQ(postsBefore, 3);  // x = 0, 1820, 3640
  ASSERT_TRUE(m.undo());
  frame({{"interior_sills", {{{"from", {0, 1}}, {"to", {2, 1}}}}}});
  const auto girders = centres("floor_girder");
  ASSERT_EQ(countAtY(girders, 910), 1);  // only the run x 2..4 remains
  for (const auto& g : girders) if (std::abs(g.Y() - 910) < 1) EXPECT_NEAR(g.X(), 2730, 1);
  // Posts at x=0 and 1820 touch the sill bays; x=3640 still carries the girder.
  EXPECT_EQ(countAtY(centres("floor_post"), 910), 1);
  EXPECT_EQ(countAtY(centres("sill"), 910), 2);
}

TEST_F(FrameInteriorSills, BracesOnInteriorLinesWarnOrGetSills) {
  const json braces = {{{"storey", 1}, {"from", {1, 2}}, {"to", {2, 2}}}, {{"storey", 1}, {"from", {0, 0}}, {"to", {1, 0}}}};
  auto made = frame({{"braces", braces}});
  EXPECT_TRUE(warned(made, "内部の筋かい壁 1 か所"));
  const auto before = centres("sill").size();
  ASSERT_TRUE(m.undo());
  made = frame({{"braces", braces}, {"sills_under_braces", true}});
  EXPECT_FALSE(warned(made, "筋かい壁"));
  EXPECT_EQ(centres("sill").size(), before + 1);  // the perimeter brace already has its sill
}

TEST_F(FrameInteriorSills, PerimeterOrOffGridRunsAreRefused) {
  EXPECT_NE(refused({{"interior_sills", {{{"from", {0, 0}}, {"to", {4, 0}}}}}}).find("perimeter"), std::string::npos);
  refused({{"interior_sills", {{{"from", {0, 1}}, {"to", {2, 2}}}}}});
  refused({{"interior_sills", {{{"from", {0, 2}}, {"to", {9, 2}}}}}});
}

TEST_F(FrameInteriorSills, MatFoundationFollowsInteriorSillsAndTakesAPassage) {
  const auto made = frame({{"interior_sills", {{{"from", {0, 2}}, {"to", {4, 2}}}}}});
  const std::string id = made["frame"];
  auto r = m.generateFoundation(json{{"inputs", {{"type", "mat"}, {"frame", id}, {"edge_depth", 0},
                                                 {"passages", {{{"at", {1365, 1820}}}}}}}}.dump());
  ASSERT_TRUE(r) << r.error();
  const auto plan = json::parse(*r);
  // Perimeter 3640×3640 plus one interior run of 3640, stem 150×400, minus one 600×330 passage.
  const double perimeter = (3.79 * 3.79 - 3.49 * 3.49), inner = 3.49 * .15;
  EXPECT_NEAR(plan["quantities"]["foundation_stem"]["volume_m3"].get<double>(), (perimeter + inner) * .4 - .6 * .15 * .33, 1e-6);
  const auto c = construction::checkFoundation(m.document(), json{{"foundation", plan["foundation"]}}.dump());
  ASSERT_TRUE(c);
  EXPECT_EQ(json::parse(*c)["plans"][0]["status"], "current");
}
