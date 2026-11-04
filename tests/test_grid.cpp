// Editable grids (docs/43): a grid is a group that remembers its lines;
// spacing changed, a line moved or erased by hand, the labels and
// dimensions follow, and members pinned to its crossings go with them.

#include <gtest/gtest.h>
#include <cmath>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Section.h"

using namespace sbr;
using namespace sbr::doc;
using json = nlohmann::json;

namespace {

gp_Trsf shift(double x, double y, double z) {
  gp_Trsf t;
  t.SetTranslation(gp_Vec(x, y, z));
  return t;
}

// The world line of a grid line found by name.
const Object* lineNamed(const Modeler& m, const std::string& name) {
  for (const Object& o : m.document().objects()) {
    if (o.kind() == ObjectKind::Guide && o.name == name) return &o;
  }
  return nullptr;
}

gp_Pnt linePoint(const Modeler& m, const std::string& name) {
  const Object* o = lineNamed(m, name);
  EXPECT_TRUE(o);
  return std::get<GuideLine>(o->recipe.base).point.Transformed(m.document().worldTransform(*o));
}

std::vector<const Object*> parts(const Modeler& m, const ObjectId& grid, const std::string& kind) {
  std::vector<const Object*> out;
  for (const ObjectId& id : m.document().children(grid)) {
    const Object* o = m.document().find(id);
    const auto it = o->attributes.find("grid_part");
    if (it != o->attributes.end() && it->second == kind) out.push_back(o);
  }
  return out;
}

std::vector<double> dimensionLengths(const Modeler& m, const ObjectId& grid) {
  std::vector<double> out;
  for (const Object* d : parts(m, grid, "dimension")) {
    const auto& dim = std::get<LinearDimension>(d->recipe.base);
    out.push_back(dim.start.Distance(dim.end));
  }
  std::sort(out.begin(), out.end());
  return out;
}

const LinearMember& memberOf(const Modeler& m, const ObjectId& id) {
  return std::get<LinearMember>(m.document().find(id)->recipe.base);
}

gp_Pnt worldStart(const Modeler& m, const ObjectId& id) {
  const Object* o = m.document().find(id);
  return memberOf(m, id).start.Transformed(m.document().worldTransform(*o));
}
gp_Pnt worldEnd(const Modeler& m, const ObjectId& id) {
  const Object* o = m.document().find(id);
  return memberOf(m, id).end.Transformed(m.document().worldTransform(*o));
}

void expectPoint(const gp_Pnt& p, double x, double y, double z) {
  EXPECT_NEAR(p.X(), x, 1e-6);
  EXPECT_NEAR(p.Y(), y, 1e-6);
  EXPECT_NEAR(p.Z(), z, 1e-6);
}

Modeler::MemberSpec timber(const std::string& role, int up) {
  Modeler::MemberSpec s;
  s.section.width = s.section.depth = 300;
  s.role = role;
  s.anchor.up = up;
  return s;
}

// A grid 2 x 1 spans (X1..X3 at 0/6000/12000, Y1..Y2 at 0/8000) at
// (1000, 2000, 0), with columns and beams both ways.
struct Frame {
  Modeler m;
  Modeler::GridLines grid;
  Modeler::GridMembers members;
  Frame() {
    grid = *m.createGridLines(gp_Pnt(1000, 2000, 0), {6000, 6000}, {8000});
    Modeler::GridMemberOptions o;
    o.height = 3500;
    o.columns = timber("column", 0);
    o.xBeams = timber("beam", 1);
    o.yBeams = timber("beam", 1);
    members = *m.placeMembersOnGrid(grid.grid, o);
  }
  const GridSpec& spec() const { return *m.gridSpec(grid.grid); }
  // The column standing on these lines (names).
  ObjectId columnAt(const std::string& x, const std::string& y) const {
    for (const ObjectId& id : members.columns) {
      const GridPin& pin = *m.document().find(id)->gridPin;
      if (spec().find(pin.startX)->name == x && spec().find(pin.startY)->name == y) return id;
    }
    return {};
  }
};

}  // namespace

TEST(Grid, CreatedAsOneGroupThatRemembersItsLines) {
  Modeler m;
  const auto grid = m.createGridLines(gp_Pnt(1000, 2000, 0), {3600, 3600, 3600}, {3600, 3600});
  ASSERT_TRUE(grid) << grid.error();
  const Object* g = m.document().find(grid->grid);
  ASSERT_TRUE(g);
  EXPECT_EQ(g->kind(), ObjectKind::Group);
  const GridSpec* spec = m.gridSpec(grid->grid);
  ASSERT_TRUE(spec);
  ASSERT_EQ(spec->x.size(), 4u);
  ASSERT_EQ(spec->y.size(), 3u);
  EXPECT_EQ(spec->x[2].name, "X3");
  EXPECT_DOUBLE_EQ(spec->x[2].offset, 7200);
  EXPECT_EQ(grid->lines.size(), 7u);
  EXPECT_EQ(grid->labels.size(), 7u);
  EXPECT_EQ(grid->dimensions.size(), 5u);
  for (const ObjectId& id : grid->lines) EXPECT_EQ(m.document().find(id)->parent, grid->grid);
  // Where the lines are in the world: from the grid's origin.
  expectPoint(linePoint(m, "X3"), 8200, 2000, 0);
  EXPECT_NEAR(linePoint(m, "Y2").Y(), 5600, 1e-9);
  // One undo step takes the whole grid away.
  ASSERT_TRUE(m.undo());
  EXPECT_FALSE(m.document().find(grid->grid));
  EXPECT_FALSE(lineNamed(m, "X1"));
}

TEST(Grid, SpacingChangedMovesLinesLabelsAndDimensions) {
  Modeler m;
  const auto grid = *m.createGridLines(gp_Pnt(0, 0, 0), {3600, 3600}, {5000});
  const ObjectId x2 = grid.lines[1];
  GridSpec spec = *m.gridSpec(grid.grid);
  spec.x[1].offset = 4000;  // X1-X2 4000, X2-X3 3200
  spec.x[2].name = "C";
  ASSERT_TRUE(m.setGrid(grid.grid, spec));
  EXPECT_NEAR(linePoint(m, "X2").X(), 4000, 1e-9);
  EXPECT_TRUE(lineNamed(m, "C"));
  EXPECT_FALSE(lineNamed(m, "X3"));
  // The same line object (ids kept), its label with it.
  EXPECT_EQ(lineNamed(m, "X2")->id, x2);
  bool labelled = false;
  for (const Object* l : parts(m, grid.grid, "label")) {
    const auto& note = std::get<TextNote>(l->recipe.base);
    if (note.text == "C") {
      labelled = true;
      EXPECT_NEAR(note.position.X(), 7200, 1e-9);
    }
  }
  EXPECT_TRUE(labelled);
  const std::vector<double> lengths = dimensionLengths(m, grid.grid);
  ASSERT_EQ(lengths.size(), 3u);
  EXPECT_NEAR(lengths[0], 3200, 1e-9);
  EXPECT_NEAR(lengths[1], 4000, 1e-9);
  EXPECT_NEAR(lengths[2], 5000, 1e-9);
  // Undo: back as it was, one step.
  ASSERT_TRUE(m.undo());
  EXPECT_NEAR(linePoint(m, "X2").X(), 3600, 1e-9);
  EXPECT_TRUE(lineNamed(m, "X3"));
}

TEST(Grid, LinesAddedAndTakenAway) {
  Modeler m;
  const auto grid = *m.createGridLines(gp_Pnt(0, 0, 0), {3600}, {3600});
  GridSpec spec = *m.gridSpec(grid.grid);
  spec.x.push_back({"new", "X3", 9000});
  spec.y.erase(spec.y.begin() + 1);
  spec.y.push_back({"new2", "Y9", -2000});
  ASSERT_TRUE(m.setGrid(grid.grid, spec));
  EXPECT_NEAR(linePoint(m, "X3").X(), 9000, 1e-9);
  EXPECT_FALSE(lineNamed(m, "Y2"));
  EXPECT_NEAR(linePoint(m, "Y9").Y(), -2000, 1e-9);
  EXPECT_EQ(parts(m, grid.grid, "line").size(), 5u);
  EXPECT_EQ(parts(m, grid.grid, "label").size(), 5u);
  EXPECT_EQ(parts(m, grid.grid, "dimension").size(), 3u);
  // Refused: a direction without lines, names or places twice.
  GridSpec bad = *m.gridSpec(grid.grid);
  bad.y.clear();
  EXPECT_FALSE(m.setGrid(grid.grid, bad));
  bad = *m.gridSpec(grid.grid);
  bad.x[1].name = bad.x[0].name;
  EXPECT_FALSE(m.setGrid(grid.grid, bad));
  bad = *m.gridSpec(grid.grid);
  bad.x[1].offset = bad.x[0].offset;
  EXPECT_FALSE(m.setGrid(grid.grid, bad));
}

TEST(Grid, LineMovedByHandIsWhereTheGridPutsIt) {
  Modeler m;
  const auto grid = *m.createGridLines(gp_Pnt(0, 0, 0), {3600, 3600}, {5000});
  const ObjectId x2 = grid.lines[1];
  // Moved inside the grid (its group open), sideways and a little along.
  ASSERT_TRUE(m.setActiveGroup(grid.grid));
  ASSERT_TRUE(m.transform({x2}, shift(500, 300, 0), "Move"));
  const GridSpec& spec = *m.gridSpec(grid.grid);
  EXPECT_NEAR(spec.x[1].offset, 4100, 1e-9);
  // The line itself sits where the grid says (placement put back).
  const Object* line = m.document().find(x2);
  EXPECT_TRUE(line->transform.Form() == gp_Identity);
  EXPECT_NEAR(linePoint(m, "X2").X(), 4100, 1e-9);
  const std::vector<double> lengths = dimensionLengths(m, grid.grid);
  EXPECT_NEAR(lengths[0], 3100, 1e-9);
  EXPECT_NEAR(lengths[1], 4100, 1e-9);
  // Erased: the grid without it (its label goes too).
  ASSERT_TRUE(m.remove({x2}));
  EXPECT_EQ(m.gridSpec(grid.grid)->x.size(), 2u);
  EXPECT_EQ(parts(m, grid.grid, "label").size(), 4u);
  EXPECT_EQ(dimensionLengths(m, grid.grid).size(), 2u);
  // Not the last line in a direction.
  EXPECT_FALSE(m.remove({lineNamed(m, "Y1")->id, lineNamed(m, "Y2")->id}));
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(m.gridSpec(grid.grid)->x.size(), 3u);
}

TEST(Grid, MembersPlacedOnCrossingsArePinned) {
  Frame f;
  EXPECT_EQ(f.members.columns.size(), 6u);
  EXPECT_EQ(f.members.beams.size(), 3u + 2u * 2u);  // 3 x lines x 1 span, 2 y lines x 2 spans
  const ObjectId c = f.columnAt("X2", "Y2");
  ASSERT_FALSE(c.empty());
  expectPoint(worldStart(f.m, c), 7000, 10000, 0);
  expectPoint(worldEnd(f.m, c), 7000, 10000, 3500);
  EXPECT_EQ(f.m.document().find(c)->attributes.at("kind"), "column");
  // Beams on their top edge at the columns' tops.
  const ObjectId b = f.members.beams.front();
  EXPECT_NEAR(worldStart(f.m, b).Z(), 3500, 1e-9);
  EXPECT_EQ(memberOf(f.m, b).anchor.up, 1);
  // Placed again: nothing twice.
  Modeler::GridMemberOptions again;
  again.columns = timber("column", 0);
  EXPECT_FALSE(f.m.placeMembersOnGrid(f.grid.grid, again));
}

TEST(Grid, PinnedMembersFollowSpacingAndMoves) {
  Frame f;
  const ObjectId c = f.columnAt("X2", "Y1");
  const ObjectId c3 = f.columnAt("X3", "Y1");
  ObjectId beam;  // on Y1 from X2 to X3
  for (const ObjectId& id : f.members.beams) {
    const GridPin& pin = *f.m.document().find(id)->gridPin;
    if (pin.startX == f.spec().x[1].id && pin.endX == f.spec().x[2].id && pin.startY == f.spec().y[0].id) beam = id;
  }
  ASSERT_FALSE(beam.empty());
  // A column moved by hand 100 up keeps that offset.
  ASSERT_TRUE(f.m.transform({c3}, shift(0, 0, 100), "Move"));
  GridSpec spec = f.spec();
  spec.x[1].offset = 7000;  // X2 1000 further
  spec.x[2].offset = 13000;
  ASSERT_TRUE(f.m.setGrid(f.grid.grid, spec));
  expectPoint(worldStart(f.m, c), 8000, 2000, 0);
  expectPoint(worldEnd(f.m, c), 8000, 2000, 3500);
  expectPoint(worldStart(f.m, c3), 14000, 2000, 100);
  expectPoint(worldStart(f.m, beam), 8000, 2000, 3500);
  expectPoint(worldEnd(f.m, beam), 14000, 2000, 3500);
  // One undo step takes grid and members back.
  ASSERT_TRUE(f.m.undo());
  expectPoint(worldStart(f.m, c), 7000, 2000, 0);
  expectPoint(worldEnd(f.m, beam), 13000, 2000, 3500);
  // The whole grid moved: everything pinned goes along once.
  ASSERT_TRUE(f.m.transform({f.grid.grid}, shift(0, 1000, 0), "Move"));
  expectPoint(worldStart(f.m, c), 7000, 3000, 0);
  expectPoint(worldEnd(f.m, beam), 13000, 3000, 3500);
  // Grid and a column moved together: the column once, not twice.
  ASSERT_TRUE(f.m.transform({f.grid.grid, c}, shift(500, 0, 0), "Move"));
  expectPoint(worldStart(f.m, c), 7500, 3000, 0);
  // A line moved by hand inside the grid: same as editing it.
  ASSERT_TRUE(f.m.setActiveGroup(f.grid.grid));
  ASSERT_TRUE(f.m.transform({lineNamed(f.m, "X2")->id}, shift(-1000, 0, 0), "Move"));
  ASSERT_TRUE(f.m.setActiveGroup(""));
  expectPoint(worldStart(f.m, c), 6500, 3000, 0);
  expectPoint(worldStart(f.m, beam), 6500, 3000, 3500);
}

TEST(Grid, MemberEditsKeptWhenFollowing) {
  Frame f;
  const ObjectId c = f.columnAt("X1", "Y1");
  // The column made taller (length 4000, foot kept): moving the grid keeps it.
  ASSERT_TRUE(f.m.setParameter(c, "length", 4000));
  ASSERT_TRUE(f.m.transform({f.grid.grid}, shift(250, 0, 0), "Move"));
  expectPoint(worldStart(f.m, c), 1250, 2000, 0);
  expectPoint(worldEnd(f.m, c), 1250, 2000, 4000);
}

TEST(Grid, LineTakenAwayLeavesItsMembersWhereTheyAre) {
  Frame f;
  const ObjectId c = f.columnAt("X3", "Y2");
  GridSpec spec = f.spec();
  spec.x.pop_back();  // X3
  spec.x[0].offset = -500;
  ASSERT_TRUE(f.m.setGrid(f.grid.grid, spec));
  expectPoint(worldStart(f.m, c), 13000, 10000, 0);
  // Others still follow.
  expectPoint(worldStart(f.m, f.columnAt("X1", "Y2")), 500, 10000, 0);
}

TEST(Grid, SavedAndCopied) {
  Frame f;
  const std::string path = ::testing::TempDir() + "grid_saved.sbr";
  ASSERT_TRUE(f.m.save(path));
  Modeler loaded;
  ASSERT_TRUE(loaded.load(path));
  ASSERT_TRUE(loaded.gridSpec(f.grid.grid));
  EXPECT_EQ(*loaded.gridSpec(f.grid.grid), f.spec());
  const ObjectId c = f.columnAt("X2", "Y1");
  ASSERT_TRUE(loaded.document().find(c)->gridPin);
  GridSpec spec = *loaded.gridSpec(f.grid.grid);
  spec.y[0].offset = -1000;
  ASSERT_TRUE(loaded.setGrid(f.grid.grid, spec));
  expectPoint(worldStart(loaded, c), 7000, 1000, 0);

  // Grid and a column copied together: the copy follows the copied grid.
  const auto copies = f.m.copy({f.grid.grid, c}, shift(0, 20000, 0));
  ASSERT_TRUE(copies) << copies.error();
  ObjectId gridCopy, columnCopy;
  for (const ObjectId& id : *copies) {
    if (f.m.gridSpec(id)) gridCopy = id;
    else columnCopy = id;
  }
  ASSERT_FALSE(gridCopy.empty());
  ASSERT_FALSE(columnCopy.empty());
  EXPECT_EQ(f.m.document().find(columnCopy)->gridPin->grid, gridCopy);
  spec = *f.m.gridSpec(gridCopy);
  spec.x[1].offset = 5000;
  ASSERT_TRUE(f.m.setGrid(gridCopy, spec));
  expectPoint(worldStart(f.m, columnCopy), 6000, 22000, 0);
  expectPoint(worldStart(f.m, c), 7000, 2000, 0);  // the original stays
}

TEST(Grid, Api) {
  Modeler m;
  api::CommandApi commands(m);
  struct Runner {
    api::CommandApi& c;
    json execute(const json& command) { return json::parse(c.execute(command.dump())); }
  } api{commands};
  const json made = api.execute({{"command", "create_grid"}, {"args", {{"x_spacings", {6000}}, {"y_spacings", {7000}}}}});
  ASSERT_TRUE(made.value("ok", false)) << made.dump();
  const std::string grid = made["result"]["grid"];
  const json placed = api.execute({{"command", "place_members_on_grid"},
                                   {"args", {{"grid", grid},
                                             {"height", 3000},
                                             {"columns", {{"section", {{"kind", "rectangle"}, {"width", 500}, {"depth", 500}}}}},
                                             {"y_beams", {{"section", "H-400x200x8x13"}}}}}});
  ASSERT_TRUE(placed.value("ok", false)) << placed.dump();
  EXPECT_EQ(placed["result"]["columns"].size(), 4u);
  EXPECT_EQ(placed["result"]["beams"].size(), 2u);
  const std::string beam = placed["result"]["beams"][0];
  EXPECT_EQ(m.document().find(beam)->attributes.at("material_class"), "steel");
  const json object = api.execute({{"command", "get_object"}, {"args", {{"id", grid}}}});
  ASSERT_TRUE(object.value("ok", false)) << object.dump();
  const json x = object["result"]["grid"]["x"];
  ASSERT_EQ(x.size(), 2u);
  EXPECT_EQ(x[1]["spacing"], 6000);
  // Spacing changed by name; a new line added after it.
  const json edited = api.execute(
      {{"command", "set_grid"},
       {"args", {{"grid", grid},
                 {"x", {{{"id", x[0]["id"]}, {"position", 0}}, {{"id", x[1]["id"]}, {"spacing", 7200}},
                        {{"name", "X3"}, {"spacing", 3600}}}}}}});
  ASSERT_TRUE(edited.value("ok", false)) << edited.dump();
  EXPECT_EQ(edited["result"]["lines"]["x"].size(), 3u);
  const json pinned = api.execute({{"command", "get_object"}, {"args", {{"id", beam}}}});
  EXPECT_TRUE(pinned["result"].contains("grid_pin"));
  EXPECT_NEAR(worldEnd(m, beam).X(), 7200, 1e-6);
  // Old lookups by line name still work.
  const json column = api.execute({{"command", "create_member"},
                                   {"args", {{"section", {{"kind", "rectangle"}, {"width", 300}, {"depth", 300}}},
                                             {"start", "X3/Y1"}, {"length", 3000}}}});
  ASSERT_TRUE(column.value("ok", false)) << column.dump();
  EXPECT_NEAR(worldStart(m, column["result"]["id"]).X(), 10800, 1e-6);
}
