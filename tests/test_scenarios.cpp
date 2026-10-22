// Spec 07 §11.1 architectural scenarios, automated through the same command
// layer the GUI tools and AI clients use. They guard the whole flow (draw ->
// face -> solid -> edit -> undo), not single functions.

#include <cmath>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"

using nlohmann::json;

namespace {

struct Session {
  sbr::doc::Modeler modeler;
  sbr::api::CommandApi api{modeler};
  json ok(const std::string& command, json args = json::object()) {
    const json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  }
  double volume(const std::string& id) { return ok("get_object", {{"id", id}}).value("volume", 0.0); }
  std::size_t steps() { return modeler.history().undoCount(); }
};

}  // namespace

// Case A: 8,000 x 12,000 floor, 3,000 walls, a 900 x 2,100 door, 1,200 eaves.
TEST(Scenario, A_SmallBuilding) {
  Session s;
  const std::string floor =
      s.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 8000}, {"depth", 12000}, {"height", 200}})["id"];
  // Front wall on the floor.
  std::string wall =
      s.ok("create_box", {{"origin", {0, 0, 200}}, {"width", 8000}, {"depth", 200}, {"height", 3000}})["id"];
  // Door outline drawn on the wall's outer face, pushed through it.
  const std::string door = s.ok("create_rectangle", {{"origin", {3500, 0, 200}},
                                                      {"normal", {0, -1, 0}},
                                                      {"width", 900},
                                                      {"height", 2100}})["id"];
  const std::size_t before = s.steps();
  wall = s.ok("push_pull", {{"id", door}, {"face", 1}, {"distance", -200}})["id"];
  EXPECT_EQ(s.steps(), before + 1);  // one intent, one undo step
  EXPECT_NEAR(s.volume(wall), 8000.0 * 200 * 3000 - 900.0 * 200 * 2100, 1.0);
  // Eaves: a roof slab overhanging by 1,200 on every side.
  const std::string roof = s.ok("create_box", {{"origin", {-1200, -1200, 3200}},
                                               {"width", 10400},
                                               {"depth", 14400},
                                               {"height", 200}})["id"];
  EXPECT_NEAR(s.volume(roof), 10400.0 * 14400 * 200, 1.0);
  // Changing the door height afterwards keeps everything valid.
  const json params = s.ok("get_object", {{"id", wall}})["parameters"];
  std::string doorHeight;
  for (const json& p : params) {
    if (p["value"].get<double>() == 2100) doorHeight = p["key"];
  }
  ASSERT_FALSE(doorHeight.empty()) << params.dump();
  s.ok("set_parameter", {{"id", wall}, {"key", doorHeight}, {"value", 2400}});
  EXPECT_NEAR(s.volume(wall), 8000.0 * 200 * 3000 - 900.0 * 200 * 2400, 1.0);
  EXPECT_NEAR(s.volume(floor), 8000.0 * 12000 * 200, 1.0);
}

// Case B: 400 square columns on grid lines, resized and undone.
TEST(Scenario, B_ColumnGrid) {
  Session s;
  const json r = s.ok("batch", {{"name", "Columns"},
                                {"commands",
                                 {{{"command", "create_box"},
                                   {"args", {{"origin", {-200, -200, 0}}, {"width", 400}, {"depth", 400}, {"height", 3000}}}},
                                  {{"command", "make_component"}, {"args", {{"ids", {"$1.id"}}, {"name", "C1"}}}},
                                  {{"command", "copy_array"}, {"args", {{"ids", {"$2.id"}}, {"vector", {6000, 0, 0}}, {"count", 3}}}}}}});
  EXPECT_EQ(s.ok("list_objects")["objects"].size(), 4u);
  const std::size_t steps = s.steps();
  const std::string instance = r["results"][1]["result"]["id"];
  const auto* ref = std::get_if<sbr::doc::ComponentInstance>(&s.modeler.document().find(instance)->recipe.base);
  ASSERT_TRUE(ref);
  const auto* def = s.modeler.document().findDefinition(ref->definitionId);
  ASSERT_TRUE(def);
  s.ok("set_definition_parameter",
       {{"definition", ref->definitionId}, {"object", def->objects.front().id}, {"key", "op0.distance"}, {"value", 3600}});
  // Keep the reply alive: a range-for over a member of a temporary would
  // iterate freed memory.
  const json grown = s.ok("list_objects");
  for (const json& o : grown["objects"]) {
    EXPECT_NEAR(o["bounds"]["max"][2].get<double>(), 3600, 1e-6);
  }
  s.ok("undo");
  EXPECT_EQ(s.steps(), steps);
  const json restored = s.ok("list_objects");
  for (const json& o : restored["objects"]) {
    EXPECT_NEAR(o["bounds"]["max"][2].get<double>(), 3000, 1e-6);
  }
}

// Case C: an arched opening from a line + arc outline, cut through a wall.
TEST(Scenario, C_ArchedOpening) {
  Session s;
  const std::string wall0 =
      s.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 4000}, {"depth", 300}, {"height", 3500}})["id"];
  // On the wall's outer face (y = 0, facing -Y): arc on top of three lines.
  s.ok("create_arc", {{"start", {1500, 0, 2000}}, {"end", {2500, 0, 2000}}, {"normal", {0, -1, 0}}, {"bulge", 500}});
  const json line = s.ok("create_line", {{"points", {{2500, 0, 2000}, {2500, 0, 0.0001}, {1500, 0, 0.0001}, {1500, 0, 2000}}}});
  ASSERT_TRUE(line["face"].get<bool>()) << line.dump();
  const std::string outline = line["id"];
  const std::string wall = s.ok("push_pull", {{"id", outline}, {"face", 1}, {"distance", -300}})["id"];
  EXPECT_NE(wall, wall0);
  const double opening = 1000.0 * 2000 + M_PI * 500 * 500 / 2;
  EXPECT_NEAR(s.volume(wall), 4000.0 * 300 * 3500 - opening * 300, 50.0);
  EXPECT_EQ(s.ok("list_objects")["objects"].size(), 1u);
}

// Case D: a lofted roof whose section is edited afterwards.
TEST(Scenario, D_LoftedRoof) {
  Session s;
  const std::string eaves = s.ok("create_rectangle", {{"origin", {0, 0, 3000}}, {"width", 8000}, {"height", 6000}})["id"];
  const std::string ridge = s.ok("create_rectangle", {{"origin", {2000, 1500, 4500}}, {"width", 4000}, {"height", 3000}})["id"];
  const std::string roof = s.ok("loft", {{"ids", {eaves, ridge}}})["id"];
  const double v0 = s.volume(roof);
  EXPECT_GT(v0, 4000.0 * 3000 * 1500);
  s.ok("set_parameter", {{"id", roof}, {"key", "in1.width"}, {"value", 6000}});
  EXPECT_GT(s.volume(roof), v0);
}

// Case E: the AI proposes on what the user picked; the user applies; undo.
TEST(Scenario, E_HumanAndAi) {
  Session s;
  const std::string slab =
      s.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 6000}, {"depth", 6000}, {"height", 200}})["id"];
  const json proposal = {{{"command", "push_pull"}, {"args", {{"id", slab}, {"face", {{"normal", {0, 0, 1}}}}, {"distance", 100}}}}};
  const json preview = s.ok("preview", {{"commands", proposal}});
  ASSERT_TRUE(preview["would_succeed"].get<bool>());
  EXPECT_NEAR(s.volume(slab), 6000.0 * 6000 * 200, 1.0);  // nothing yet
  s.ok("batch", {{"commands", proposal}, {"expected_revision", preview["revision"]}});
  EXPECT_NEAR(s.volume(slab), 6000.0 * 6000 * 300, 1.0);
  s.ok("undo");
  EXPECT_NEAR(s.volume(slab), 6000.0 * 6000 * 200, 1.0);
}
