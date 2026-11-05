#include <gtest/gtest.h>

#include <map>
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

// Studs (間柱) turn with their wall: the thin side runs along the wall and the deep side
// across it, on X walls and Y (gable) walls alike.
TEST_F(FrameInteriorSills, StudsFaceAcrossTheirWallOnBothDirections) {
  frame(json::object());
  int x = 0, y = 0;
  for (const auto& o : m.document().objects()) {
    if (!o.attributes.count("frame_key")) continue;
    const auto key = o.attributes.at("frame_key");
    if (key.rfind("stud/", 0) != 0) continue;
    const auto b = m.document().worldBounds(o);
    const double dx = b.CornerMax().X() - b.CornerMin().X(), dy = b.CornerMax().Y() - b.CornerMin().Y();
    const bool alongX = key.find("/x/") != std::string::npos;
    if (alongX) { ++x; EXPECT_LT(dx, dy) << key << " " << dx << "x" << dy; }
    else { ++y; EXPECT_LT(dy, dx) << key << " " << dx << "x" << dy; }
  }
  EXPECT_GT(x, 0); EXPECT_GT(y, 0);
}

// Deleting a frame leaves its levels; making a frame again takes those unused levels over
// (no "Level name already used"), and their heights follow the new inputs.
TEST_F(FrameInteriorSills, FrameCanBeMadeAgainAfterItIsDeleted) {
  const auto first = frame(json::object());
  const std::string id = first["frame"];
  const auto levelCount = m.levels().size();
  std::map<std::string, std::string> before;
  for (const auto& l : m.levels()) before[l.name] = l.id;
  const auto del = call("delete", {{"ids", {id}}});
  ASSERT_TRUE(del.value("ok", false)) << del.dump();
  const auto again = frame({{"storey_height", 3000}});
  EXPECT_EQ(again["frame"].is_string(), true);
  EXPECT_EQ(m.levels().size(), levelCount);  // no duplicated levels
  for (const auto& l : m.levels()) EXPECT_EQ(before[l.name], l.id) << l.name;
  // A second frame next to a live one gets its own name and levels.
  const auto second = frame({{"origin", {20000, 0, 0}}});
  EXPECT_GT(m.levels().size(), levelCount);
  (void)second;
}

// Braces run between horizontal members: 1F from the sill top to the 胴差 bottom, 2F from the
// floor beam top (under the 24 mm board, not on it) to the eave beam bottom.
TEST_F(FrameInteriorSills, BracesRunFromMemberTopToMemberBottom) {
  frame({{"storeys", 2}, {"braces", {{{"storey", 1}, {"from", {0, 0}}, {"to", {1, 0}}},
                                      {{"storey", 2}, {"from", {0, 0}}, {"to", {1, 0}}}}}});
  auto range = [&](const std::string& kind, const std::string& keyPrefix) {
    std::pair<double, double> r{1e9, -1e9};
    for (const auto& o : m.document().objects()) {
      if (!o.attributes.count("kind") || o.attributes.at("kind") != kind) continue;
      if (!keyPrefix.empty() && (!o.attributes.count("frame_key") || o.attributes.at("frame_key").rfind(keyPrefix, 0) != 0)) continue;
      const auto b = m.document().worldBounds(o);
      r.first = std::min(r.first, b.CornerMin().Z()); r.second = std::max(r.second, b.CornerMax().Z());
    }
    return r;
  };
  const auto sill = range("sill", ""), rim = range("rim_beam", ""), board = range("floor_sheathing", ""), eave = range("eave_beam", "");
  std::vector<std::pair<double, double>> braces;
  for (const auto& o : m.document().objects())
    if (o.attributes.count("kind") && o.attributes.at("kind") == "brace") {
      // The brace axis ends (its section is centred on them).
      const auto* ref = std::get_if<ComponentInstance>(&o.recipe.base);
      ASSERT_NE(ref, nullptr);
      const auto b = m.document().worldBounds(o);
      braces.push_back({b.CornerMin().Z(), b.CornerMax().Z()});
    }
  ASSERT_EQ(braces.size(), 2u);
  std::sort(braces.begin(), braces.end());
  // The brace solid reaches a little past its axis ends (half its section across the slope),
  // so compare the axis heights through the hardware plates placed at the ends.
  std::vector<double> plates;
  for (const auto& o : m.document().objects())
    if (o.attributes.count("frame_key") && o.attributes.at("frame_key").find("brace/") == 0 &&
        o.attributes.count("kind") && o.attributes.at("kind") == "brace_plate") {
      const auto b = m.document().worldBounds(o);
      plates.push_back(b.CornerMin().Z());  // the plate marker starts at the brace end
    }
  ASSERT_EQ(plates.size(), 4u);
  std::sort(plates.begin(), plates.end());
  EXPECT_NEAR(plates[0], sill.second, 1);    // 1F foot: sill top
  EXPECT_NEAR(plates[1], rim.first, 1);      // 1F head: 胴差 bottom
  EXPECT_NEAR(plates[2], board.first, 1);    // 2F foot: beam top = under the floor board
  EXPECT_NEAR(plates[3], eave.first, 1);     // 2F head: eave beam bottom
}

// A joist first floor can carry a subfloor board: the joists stay on the girders and sills,
// the board sits on the joists and 1FL moves up by its thickness. 0 (the default) adds none.
TEST_F(FrameInteriorSills, FirstFloorBoardOnJoistsIsSelectable) {
  auto zOf = [&](const std::string& kind, bool top) {
    double z = top ? -1e9 : 1e9;
    for (const auto& o : m.document().objects())
      if (o.attributes.count("kind") && o.attributes.at("kind") == kind) {
        const auto b = m.document().worldBounds(o);
        z = top ? std::max(z, b.CornerMax().Z()) : std::min(z, b.CornerMin().Z());
      }
    return z;
  };
  frame(json::object());
  int boards = 0;
  for (const auto& o : m.document().objects()) boards += o.attributes.count("kind") && o.attributes.at("kind") == "floor_sheathing";
  EXPECT_EQ(boards, 0);  // one storey, joist floor, no board by default
  const double joistTop = zOf("joist", true), girderTop = zOf("floor_girder", true);
  ASSERT_TRUE(m.undo());
  frame({{"first_floor_board", 12}});
  EXPECT_NEAR(zOf("joist", true), joistTop, 1e-6);
  EXPECT_NEAR(zOf("floor_girder", true), girderTop, 1e-6);
  EXPECT_NEAR(zOf("floor_sheathing", false), joistTop, 1e-6);
  EXPECT_NEAR(zOf("floor_sheathing", true), joistTop + 12, 1e-6);
  bool level = false;
  for (const auto& l : m.levels())
    if (l.name.size() >= 3 && l.name.substr(l.name.size() - 3) == "1FL") { level = true; EXPECT_NEAR(l.elevation, joistTop + 12, 1e-6); }
  EXPECT_TRUE(level);
  EXPECT_FALSE(call("generate_frame", {{"inputs", {{"x_bays", 4}, {"y_bays", 4}, {"first_floor_board", 80}}}}).value("ok", true));
}
