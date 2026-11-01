// Levels (docs/45): named heights kept with the model (saved, undone);
// columns and beams placed on a grid between levels keep their ends on
// them when a level is raised or lowered.

#include <gtest/gtest.h>
#include <cmath>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"

using namespace sbr;
using namespace sbr::doc;
using json = nlohmann::json;

namespace {

gp_Pnt worldEnd(const Modeler& m, const ObjectId& id, bool end) {
  const Object* o = m.document().find(id);
  const auto& member = std::get<LinearMember>(o->recipe.base);
  return (end ? member.end : member.start).Transformed(m.document().worldTransform(*o));
}

Modeler::MemberSpec timber(const std::string& role, int up) {
  Modeler::MemberSpec s;
  s.section.width = s.section.depth = 300;
  s.role = role;
  s.anchor.up = up;
  return s;
}

std::string idOf(const Modeler& m, const std::string& name) {
  for (const FloorLevel& l : m.levels()) {
    if (l.name == name) return l.id;
  }
  return {};
}

// GL 0, 1FL 150, 2FL 3150; a grid 6000 x 6000 with 1F columns and 2F beams.
struct Storey {
  Modeler m;
  Modeler::GridLines grid;
  Modeler::GridMembers members;
  Storey() {
    EXPECT_TRUE(m.setLevels({{"", "2FL", 3150}, {"", "GL", 0}, {"", "1FL", 150}}));
    grid = *m.createGridLines(gp_Pnt(0, 0, 0), {6000}, {6000});
    Modeler::GridMemberOptions o;
    o.baseLevel = idOf(m, "1FL");
    o.topLevel = idOf(m, "2FL");
    o.columns = timber("column", 0);
    o.xBeams = timber("beam", 1);
    members = *m.placeMembersOnGrid(grid.grid, o);
  }
};

}  // namespace

TEST(Levels, KeptByElevationSavedAndUndone) {
  Modeler m;
  ASSERT_TRUE(m.setLevels({{"", "2FL", 3000}, {"", "GL", 0}}));
  ASSERT_EQ(m.levels().size(), 2u);
  EXPECT_EQ(m.levels()[0].name, "GL");  // by elevation
  EXPECT_FALSE(m.levels()[0].id.empty());
  EXPECT_NE(m.levels()[0].id, m.levels()[1].id);
  // Refused: a name twice, no name.
  EXPECT_FALSE(m.setLevels({{"", "GL", 0}, {"", "GL", 100}}));
  EXPECT_FALSE(m.setLevels({{"", "", 0}}));
  // Renamed and moved: the same level (id kept).
  std::vector<FloorLevel> next = m.levels();
  const std::string id = next[1].id;
  next[1].name = "RFL";
  next[1].elevation = 3300;
  ASSERT_TRUE(m.setLevels(next));
  EXPECT_EQ(m.levels()[1].id, id);
  const std::string path = ::testing::TempDir() + "levels_saved.sbr";
  ASSERT_TRUE(m.save(path));
  Modeler loaded;
  ASSERT_TRUE(loaded.load(path));
  EXPECT_EQ(loaded.levels(), m.levels());
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(m.levels()[1].name, "2FL");
  ASSERT_TRUE(m.undo());
  EXPECT_TRUE(m.levels().empty());
}

TEST(Levels, MembersPlacedBetweenLevels) {
  Storey s;
  ASSERT_EQ(s.members.columns.size(), 4u);
  ASSERT_EQ(s.members.beams.size(), 2u);
  const ObjectId c = s.members.columns.front();
  EXPECT_NEAR(worldEnd(s.m, c, false).Z(), 150, 1e-6);
  EXPECT_NEAR(worldEnd(s.m, c, true).Z(), 3150, 1e-6);
  EXPECT_NEAR(worldEnd(s.m, s.members.beams.front(), false).Z(), 3150, 1e-6);
  // The same storey again: nothing twice; the storey above: new members.
  Modeler::GridMemberOptions again;
  again.baseLevel = idOf(s.m, "1FL");
  again.topLevel = idOf(s.m, "2FL");
  again.columns = timber("column", 0);
  EXPECT_FALSE(s.m.placeMembersOnGrid(s.grid.grid, again));
  std::vector<FloorLevel> levels = s.m.levels();
  levels.push_back({"", "RFL", 6150});
  ASSERT_TRUE(s.m.setLevels(levels));
  again.baseLevel = idOf(s.m, "2FL");
  again.topLevel = idOf(s.m, "RFL");
  const auto upper = s.m.placeMembersOnGrid(s.grid.grid, again);
  ASSERT_TRUE(upper) << upper.error();
  EXPECT_EQ(upper->columns.size(), 4u);
}

TEST(Levels, MembersFollowTheirLevels) {
  Storey s;
  const ObjectId c = s.members.columns.front();
  const ObjectId b = s.members.beams.front();
  // A box drawn by hand stays where it is.
  geom::SketchPlane plane = geom::SketchPlane::ground();
  plane.origin = gp_Pnt(20000, 0, 0);
  const ObjectId box = *s.m.createRectangle(plane, 1000, 1000);
  ASSERT_TRUE(s.m.extrude(box, 3000));
  std::vector<FloorLevel> levels = s.m.levels();
  for (FloorLevel& l : levels) {
    if (l.name == "2FL") l.elevation = 3450;  // the storey 300 taller
  }
  ASSERT_TRUE(s.m.setLevels(levels));
  EXPECT_NEAR(worldEnd(s.m, c, false).Z(), 150, 1e-6);  // its foot on 1FL stays
  EXPECT_NEAR(worldEnd(s.m, c, true).Z(), 3450, 1e-6);
  EXPECT_NEAR(worldEnd(s.m, b, false).Z(), 3450, 1e-6);
  EXPECT_NEAR(worldEnd(s.m, b, true).Z(), 3450, 1e-6);
  EXPECT_TRUE(s.m.document().find(box)->transform.Form() == gp_Identity);
  // One undo step: levels and members back.
  ASSERT_TRUE(s.m.undo());
  EXPECT_NEAR(worldEnd(s.m, c, true).Z(), 3150, 1e-6);
  EXPECT_NEAR(s.m.levels()[2].elevation, 3150, 1e-9);
  // A level taken away: its members stay where they are.
  levels = s.m.levels();
  levels.erase(levels.begin() + 2);  // 2FL
  ASSERT_TRUE(s.m.setLevels(levels));
  EXPECT_NEAR(worldEnd(s.m, c, true).Z(), 3150, 1e-6);
  // The grid still moves them, the levels' heights stay.
  ASSERT_TRUE(s.m.undo());
  gp_Trsf t;
  t.SetTranslation(gp_Vec(1000, 0, 0));
  ASSERT_TRUE(s.m.transform({s.grid.grid}, t, "Move"));
  EXPECT_NEAR(worldEnd(s.m, c, false).X(), 1000, 1e-6);
  EXPECT_NEAR(worldEnd(s.m, c, true).Z(), 3150, 1e-6);
}

TEST(Levels, Api) {
  Modeler m;
  api::CommandApi commands(m);
  auto run = [&](const json& c) { return json::parse(commands.execute(c.dump())); };
  const json set = run({{"command", "set_levels"},
                        {"args", {{"levels", {{{"name", "GL"}, {"elevation", 0}}, {{"name", "1FL"}, {"elevation", 0}},
                                              {{"name", "2FL"}, {"elevation", 3000}}}}}}});
  ASSERT_TRUE(set.value("ok", false)) << set.dump();
  EXPECT_EQ(set["result"]["levels"].size(), 3u);
  const json grid = run({{"command", "create_grid"}, {"args", {{"x_spacings", {5000}}, {"y_spacings", {5000}}}}});
  const json placed = run({{"command", "place_members_on_grid"},
                           {"args", {{"grid", grid["result"]["grid"]},
                                     {"base_level", "1FL"},
                                     {"top_level", "2FL"},
                                     {"columns", {{"section", {{"kind", "rectangle"}, {"width", 400}, {"depth", 400}}}}}}}});
  ASSERT_TRUE(placed.value("ok", false)) << placed.dump();
  const std::string column = placed["result"]["columns"][0];
  json levels = run({{"command", "get_levels"}, {"args", json::object()}})["result"]["levels"];
  for (json& l : levels) {
    if (l["name"] == "2FL") l["elevation"] = 3200;
  }
  ASSERT_TRUE(run({{"command", "set_levels"}, {"args", {{"levels", levels}}}}).value("ok", false));
  EXPECT_NEAR(worldEnd(m, column, true).Z(), 3200, 1e-6);
  const json info = run({{"command", "get_object"}, {"args", {{"id", column}}}});
  EXPECT_EQ(info["result"]["grid_pin"]["levels"].size(), 2u);
  EXPECT_FALSE(run({{"command", "place_members_on_grid"},
                    {"args", {{"grid", grid["result"]["grid"]}, {"base_level", "B1FL"}, {"top_level", "2FL"},
                              {"columns", {{"section", {{"kind", "rectangle"}, {"width", 400}, {"depth", 400}}}}}}}})
                   .value("ok", true));
}
