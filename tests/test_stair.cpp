// Stairs along a walking line (docs/90 G-09, docs/103). Sizes are checked against hand
// calculations: treads share the plan of the line's width out among them; a block stair's
// tread stands on (riser + thickness) of solid, the first one on the floor.
#include <cmath>
#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <BRepCheck_Analyzer.hxx>
#include <TopExp_Explorer.hxx>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/draw/Drawing.h"
#include "sbr/geom/Stair.h"

using json = nlohmann::json;
using sbr::api::CommandApi;
using sbr::geom::makeStair;
using sbr::geom::StairSpec;

namespace {
StairSpec along(std::vector<gp_Pnt> path) {
  StairSpec s;
  s.path = std::move(path);
  return s;  // width 800, rise 2800, risers automatic (limit 200), turns 3, block, 30
}

double area(const std::vector<gp_Pnt>& p) {
  double twice = 0;
  for (std::size_t i = 0; i < p.size(); ++i) {
    const gp_Pnt &a = p[i], &b = p[(i + 1) % p.size()];
    twice += a.X() * b.Y() - b.X() * a.Y();
  }
  return std::abs(twice) / 2;
}

int solids(const TopoDS_Shape& s) {
  int n = 0;
  for (TopExp_Explorer e(s, TopAbs_SOLID); e.More(); e.Next()) ++n;
  return n;
}

std::string state(const sbr::geom::StairShape& s, const std::string& item) {
  for (const auto& c : s.checks)
    if (c.item == item) return c.state;
  return "";
}
}  // namespace

TEST(Stair, AStraightStairSharesItsLineOutInEvenTreads) {
  const auto made = makeStair(along({gp_Pnt(0, 0, 0), gp_Pnt(2730, 0, 0)}));
  ASSERT_TRUE(made) << made.error();
  EXPECT_EQ(made->risers, 14);  // 2800 / 200
  EXPECT_NEAR(made->riser, 200, 1e-9);
  ASSERT_EQ(made->treads.size(), 13u);  // the upper floor is the last riser's tread
  EXPECT_NEAR(made->goingMin, 210, 1e-9);
  EXPECT_NEAR(made->goingMax, 210, 1e-9);
  EXPECT_EQ(solids(made->solid), 1);
  EXPECT_TRUE(BRepCheck_Analyzer(made->solid).IsValid());
  // The first tread stands on the floor (200), the others on riser + thickness (230).
  EXPECT_NEAR(sbr::geom::volume(made->solid), 210.0 * 800 * (200 + 12 * 230), 1.0);
  const auto b = sbr::geom::exactBounds(made->solid);
  EXPECT_NEAR(b.CornerMin().X(), 0, 1e-6);
  EXPECT_NEAR(b.CornerMax().X(), 2730, 1e-6);
  EXPECT_NEAR(b.CornerMin().Y(), -400, 1e-6);
  EXPECT_NEAR(b.CornerMax().Y(), 400, 1e-6);
  EXPECT_NEAR(b.CornerMin().Z(), 0, 1e-6);
  EXPECT_NEAR(b.CornerMax().Z(), 2600, 1e-6);  // the top tread, one riser under the upper floor
  EXPECT_NEAR(made->treads[4].outline.front().Z(), 1000, 1e-9);
  EXPECT_EQ(state(*made, "riser"), "ok");
  EXPECT_EQ(state(*made, "going"), "ok");
  EXPECT_EQ(state(*made, "width"), "ok");
  EXPECT_EQ(state(*made, "landing"), "ok");
  EXPECT_EQ(state(*made, "handrail"), "not_evaluated");  // not made here: never a pass
  EXPECT_EQ(state(*made, "winder_going"), "");            // no winders

  // A number of risers given: the riser follows the rise.
  StairSpec fixed = along({gp_Pnt(0, 0, 0), gp_Pnt(2730, 0, 0)});
  fixed.risers = 12;
  const auto steep = makeStair(fixed);
  ASSERT_TRUE(steep);
  EXPECT_NEAR(steep->riser, 2800.0 / 12, 1e-9);
  EXPECT_EQ(state(*steep, "riser"), "ng");  // 233.3 > 230
  // Boards only: 13 boards of 30.
  fixed.risers = 0;
  fixed.style = "boards";
  const auto boards = makeStair(fixed);
  ASSERT_TRUE(boards);
  EXPECT_EQ(solids(boards->solid), 13);
  EXPECT_NEAR(sbr::geom::volume(boards->solid), 2730.0 * 800 * 30, 1.0);
}

TEST(Stair, ATurnIsCutIntoTriangularWindersFromItsInnerCorner) {
  // East 2000, then north 1500: flights of 1600 and 1100 either side of an 800 square.
  StairSpec spec = along({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp_Pnt(2000, 1500, 0)});
  const auto made = makeStair(spec);
  ASSERT_TRUE(made) << made.error();
  EXPECT_EQ(made->risers, 14);
  EXPECT_EQ(made->winders, 3);
  EXPECT_EQ(made->flights, 2);
  // 10 straight treads: 6 on 1600 and 4 on 1100 (the evenest the line allows).
  EXPECT_NEAR(made->goingMin, 1600.0 / 6, 1e-9);
  EXPECT_NEAR(made->goingMax, 1100.0 / 4, 1e-9);
  // The three winders (30 degrees each): triangle, kite, triangle, about the inner corner.
  ASSERT_EQ(made->treads.size(), 13u);
  const auto &w1 = made->treads[6], &w2 = made->treads[7], &w3 = made->treads[8];
  EXPECT_EQ(w1.kind, "winder");
  EXPECT_EQ(w3.kind, "winder");
  EXPECT_EQ(made->treads[5].kind, "tread");
  EXPECT_EQ(made->treads[9].kind, "tread");
  EXPECT_EQ(w1.outline.size(), 3u);
  EXPECT_EQ(w2.outline.size(), 4u);
  EXPECT_EQ(w3.outline.size(), 3u);
  const double triangle = 800.0 * 800 * std::tan(std::acos(-1.0) / 6) / 2;
  EXPECT_NEAR(area(w1.outline), triangle, 1e-6);
  EXPECT_NEAR(area(w2.outline), 800.0 * 800 - 2 * triangle, 1e-6);
  EXPECT_NEAR(area(w3.outline), triangle, 1e-6);
  for (const auto* w : {&w1, &w2, &w3}) {  // all start at the inner corner (1600, 400)
    EXPECT_NEAR(w->outline.front().X(), 1600, 1e-9);
    EXPECT_NEAR(w->outline.front().Y(), 400, 1e-9);
  }
  EXPECT_NEAR(w2.outline[2].X(), 2400, 1e-9);  // the kite holds the outer corner
  EXPECT_NEAR(w2.outline[2].Y(), -400, 1e-9);
  EXPECT_NEAR(w2.outline.front().Z() - w1.outline.front().Z(), 200, 1e-9);  // each a riser higher
  // 300 from the narrow end: the chord of 30 degrees.
  EXPECT_NEAR(made->winderGoing, 600 * std::sin(std::acos(-1.0) / 12), 1e-9);
  EXPECT_EQ(state(*made, "winder_going"), "ok");  // 155.3 >= 150
  EXPECT_EQ(solids(made->solid), 1);
  EXPECT_TRUE(BRepCheck_Analyzer(made->solid).IsValid());
  // As boards: the whole plan of the line's width, 30 thick.
  spec.style = "boards";
  const auto boards = makeStair(spec);
  ASSERT_TRUE(boards);
  EXPECT_NEAR(sbr::geom::volume(boards->solid), (1600.0 * 800 + 800.0 * 800 + 1100.0 * 800) * 30, 1.0);

  // Two winders (45 degrees): two triangles; four (22.5 degrees): too narrow at 300.
  spec.turns = {2};
  const auto two = makeStair(spec);
  ASSERT_TRUE(two);
  EXPECT_EQ(two->winders, 2);
  EXPECT_EQ(two->treads[7].outline.size(), 3u);
  EXPECT_NEAR(area(two->treads[7].outline), 320000, 1e-6);
  spec.turns = {4};
  const auto four = makeStair(spec);
  ASSERT_TRUE(four);
  EXPECT_NEAR(four->winderGoing, 600 * std::sin(std::acos(-1.0) / 16), 1e-9);
  EXPECT_EQ(state(*four, "winder_going"), "ng");  // 117 < 150
  // A landing instead: one flat tread over the square.
  spec.turns = {1};
  const auto landing = makeStair(spec);
  ASSERT_TRUE(landing);
  EXPECT_EQ(landing->landings, 1);
  EXPECT_EQ(landing->winders, 0);
  EXPECT_EQ(state(*landing, "winder_going"), "");
  // A left turn and a right turn are mirror images.
  spec.turns = {3};
  spec.path = {gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp_Pnt(2000, -1500, 0)};
  const auto right = makeStair(spec);
  ASSERT_TRUE(right);
  EXPECT_NEAR(right->treads[6].outline.front().Y(), -400, 1e-9);
  EXPECT_NEAR(sbr::geom::volume(right->solid), sbr::geom::volume(boards->solid), 1.0);
}

TEST(Stair, AUTurnIsTwoTurnsAboutOnePointOrAHalfLanding) {
  // Up 1500, across by the width, back 1500: the two corner squares touch.
  StairSpec spec = along({gp_Pnt(0, 0, 0), gp_Pnt(1500, 0, 0), gp_Pnt(1500, 800, 0), gp_Pnt(0, 800, 0)});
  const auto winders = makeStair(spec);
  ASSERT_TRUE(winders) << winders.error();
  EXPECT_EQ(winders->winders, 6);  // six triangles about (1100, 400)
  EXPECT_EQ(winders->flights, 2);
  EXPECT_EQ(winders->risers, 14);
  ASSERT_EQ(winders->treads.size(), 13u);  // 4 + 6 + 3
  EXPECT_NEAR(winders->goingMin, 1100.0 / 4, 1e-9);
  EXPECT_NEAR(winders->goingMax, 1100.0 / 3, 1e-9);
  for (int i = 4; i < 10; ++i) {
    EXPECT_EQ(winders->treads[i].kind, "winder");
    EXPECT_NEAR(winders->treads[i].outline.front().X(), 1100, 1e-9);
    EXPECT_NEAR(winders->treads[i].outline.front().Y(), 400, 1e-9);
  }
  EXPECT_EQ(solids(winders->solid), 1);
  EXPECT_TRUE(BRepCheck_Analyzer(winders->solid).IsValid());
  const auto b = sbr::geom::exactBounds(winders->solid);
  EXPECT_NEAR(b.CornerMax().X(), 1900, 1e-6);
  EXPECT_NEAR(b.CornerMax().Y(), 1200, 1e-6);
  EXPECT_NEAR(b.CornerMin().Y(), -400, 1e-6);

  // Landings at both turns with nothing between: one half landing, 800 x 1600.
  spec.turns = {1};
  const auto half = makeStair(spec);
  ASSERT_TRUE(half) << half.error();
  EXPECT_EQ(half->landings, 1);
  ASSERT_EQ(half->treads.size(), 13u);  // 6 + 1 + 6
  EXPECT_EQ(half->treads[6].kind, "landing");
  EXPECT_NEAR(area(half->treads[6].outline), 800.0 * 1600, 1e-6);
  EXPECT_NEAR(half->goingMin, 1100.0 / 6, 1e-9);
  EXPECT_EQ(solids(half->solid), 1);
  // A landing, then winders; and a longer middle part with treads of its own.
  spec.turns = {1, 3};
  const auto mixed = makeStair(spec);
  ASSERT_TRUE(mixed);
  EXPECT_EQ(mixed->landings, 1);
  EXPECT_EQ(mixed->winders, 3);
  spec.turns = {3};
  spec.path = {gp_Pnt(0, 0, 0), gp_Pnt(1500, 0, 0), gp_Pnt(1500, 1800, 0), gp_Pnt(0, 1800, 0)};
  const auto wide = makeStair(spec);
  ASSERT_TRUE(wide);
  EXPECT_EQ(wide->flights, 3);
  EXPECT_EQ(solids(wide->solid), 1);
}

TEST(Stair, WhatCannotBeBuiltIsRefusedWithTheReason) {
  auto error = [](const StairSpec& s) {
    const auto made = makeStair(s);
    return made ? std::string() : made.error();
  };
  EXPECT_NE(error(along({gp_Pnt(0, 0, 0)})).find("needs a line"), std::string::npos);
  EXPECT_NE(error(along({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp_Pnt(3000, 1500, 0)})).find("right angles"), std::string::npos);
  // The part between two turns shorter than the width.
  EXPECT_NE(error(along({gp_Pnt(0, 0, 0), gp_Pnt(1500, 0, 0), gp_Pnt(1500, 500, 0), gp_Pnt(0, 500, 0)})).find("shorter"), std::string::npos);
  StairSpec few = along({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp_Pnt(2000, 1500, 0)});
  few.risers = 5;  // 3 winders + 2 flights need 6
  EXPECT_NE(error(few).find("at least 6 risers"), std::string::npos);
  StairSpec thick = along({gp_Pnt(0, 0, 0), gp_Pnt(2730, 0, 0)});
  thick.thickness = 250;
  EXPECT_NE(error(thick).find("thickness"), std::string::npos);
  // A point on a straight part is not a turn.
  const auto straight = makeStair(along({gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp_Pnt(2730, 0, 0)}));
  ASSERT_TRUE(straight);
  EXPECT_EQ(straight->flights, 1);
  // Taller than 4000 without looking at its landings: not judged.
  StairSpec tall = along({gp_Pnt(0, 0, 0), gp_Pnt(6000, 0, 0)});
  tall.rise = 4500;
  const auto high = makeStair(tall);
  ASSERT_TRUE(high);
  EXPECT_EQ(state(*high, "landing"), "not_evaluated");
}

TEST(Stair, TheFloorIsOpenedWhereTheHeadroomIsShort) {
  const StairSpec spec = along({gp_Pnt(0, 0, 0), gp_Pnt(2730, 0, 0)});
  // Underside at 2600: treads higher than 600 (the 4th to the 13th) have less than 2000.
  const auto hole = sbr::geom::stairOpening(spec, 2600, 2800, 2000);
  ASSERT_TRUE(hole) << hole.error();
  const auto b = sbr::geom::exactBounds(hole.value());
  EXPECT_NEAR(b.CornerMin().X(), 630, 1e-6);
  EXPECT_NEAR(b.CornerMax().X(), 2730, 1e-6);
  EXPECT_NEAR(sbr::geom::volume(hole.value()), 2100.0 * 800 * 220, 1.0);
  EXPECT_FALSE(sbr::geom::stairOpening(spec, 5000, 5200, 2000));  // far above: nothing needs it
}

namespace {
struct Stairs : testing::Test {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  json call(const std::string& command, json args = json::object()) {
    return json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
  }
  json ok(const std::string& command, json args = json::object()) {
    json r = call(command, args);
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  }
  bool refused(const std::string& command, json args) { return !call(command, args).value("ok", true); }
  double volume(const std::string& id) { return sbr::geom::volume(modeler.document().find(id)->shape); }
  double top(const std::string& id) {
    return sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(id))).CornerMax().Z();
  }
  std::string slab;
  void SetUp() override {
    ASSERT_TRUE(modeler.setLevels({{"", "1FL", 0}, {"", "2FL", 2800}}));
    slab = ok("create_slab", {{"outline", {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}}}, {"top", 2800}, {"thickness", 200}})["id"];
  }
  void raiseSecondFloor(double to) {
    auto levels = modeler.document().settings().levels;
    for (auto& l : levels)
      if (l.name == "2FL") l.elevation = to;
    ASSERT_TRUE(modeler.setLevels(levels));
  }
};
const double kFloor = 6000.0 * 4000;
const double kTread = 210.0 * 800;
}  // namespace

TEST_F(Stairs, AStairOpensTheFloorOverItAndBothStayWhatTheyAre) {
  const auto steps = modeler.history().undoCount();
  const json made = ok("create_stair", {{"path", {{1000, 1000}, {3730, 1000}}}, {"rise", 2800}, {"open_floor", true}});
  const std::string id = made["id"];
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);  // the stair and its opening: one step
  EXPECT_EQ(made["risers"], 14);
  EXPECT_TRUE(made["risers_automatic"].get<bool>());
  EXPECT_NEAR(made["riser"].get<double>(), 200, 1e-9);
  EXPECT_NEAR(made["going_min"].get<double>(), 210, 1e-9);
  EXPECT_FALSE(made["checks_all_ok"].get<bool>());  // the handrail is not looked at: never all ok
  EXPECT_FALSE(made["base_level"].get<std::string>().empty());
  EXPECT_FALSE(made["top_level"].get<std::string>().empty());
  ASSERT_EQ(made["opened_slabs"].size(), 1u);
  EXPECT_EQ(made["opened_slabs"][0], slab);
  EXPECT_EQ(modeler.document().find(id)->attributes.at("kind"), "stair");
  // Open over the 4th to the 13th tread (less than 2000 under the slab's underside at 2600).
  EXPECT_NEAR(volume(slab), (kFloor - 10 * kTread) * 200, 10.0);

  // Still a slab: thicker (top kept), and the opening is made again for the lower underside.
  const json info = ok("member_info", {{"id", slab}});
  ASSERT_TRUE(info.contains("fields")) << info.dump();
  ok("change_member", {{"id", slab}, {"thickness", 250}});
  EXPECT_NEAR(top(slab), 2800, 1e-6);
  EXPECT_NEAR(volume(slab), (kFloor - 11 * kTread) * 250, 10.0);  // underside 2550: from the 3rd tread
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(volume(slab), (kFloor - 10 * kTread) * 200, 10.0);

  // The stair wider, turned into an L with winders, moved: the opening follows each time.
  auto before = modeler.history().undoCount();
  ok("change_stair", {{"id", id}, {"width", 900}});
  EXPECT_EQ(modeler.history().undoCount(), before + 1);
  EXPECT_NEAR(volume(slab), (kFloor - 10 * 210.0 * 900) * 200, 10.0);
  ASSERT_TRUE(modeler.undo());
  ok("move", {{"ids", {id}}, {"vector", {500, 700, 0}}});
  EXPECT_NEAR(volume(slab), (kFloor - 10 * kTread) * 200, 10.0);
  EXPECT_NEAR(ok("stair_info", {{"id", id}})["path"][0][1].get<double>(), 1700, 1e-6);  // its line went with it
  ASSERT_TRUE(modeler.undo());
  const json turned = ok("change_stair", {{"id", id}, {"path", {{1000, 1000}, {3000, 1000}, {3000, 2500}}}});
  EXPECT_EQ(turned["winders"], 3);
  EXPECT_NEAR(turned["winder_going"].get<double>(), 600 * std::sin(std::acos(-1.0) / 12), 1e-9);
  EXPECT_LT(volume(slab), kFloor * 200 - 1e6);
  ASSERT_TRUE(modeler.undo());

  // Closed and opened again by hand; a taller headroom opens more.
  ok("close_floor_for_stair", {{"stair", id}});
  EXPECT_NEAR(volume(slab), kFloor * 200, 1.0);
  EXPECT_TRUE(ok("stair_info", {{"id", id}})["opened_slabs"].empty());
  EXPECT_TRUE(refused("close_floor_for_stair", {{"stair", id}}));
  ok("open_floor_for_stair", {{"stair", id}, {"headroom", 2200}});
  EXPECT_NEAR(volume(slab), (kFloor - 11 * kTread) * 200, 10.0);  // treads above 400: from the 3rd

  // The stair deleted: the floor closes in the same step, and comes back with it.
  before = modeler.history().undoCount();
  ok("delete", {{"ids", {id}}});
  EXPECT_EQ(modeler.history().undoCount(), before + 1);
  EXPECT_NEAR(volume(slab), kFloor * 200, 1.0);
  EXPECT_TRUE(ok("member_info", {{"id", slab}}).contains("fields"));
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(volume(slab), (kFloor - 11 * kTread) * 200, 10.0);
}

TEST_F(Stairs, TheRisersAreWorkedOutAgainWhenTheStoreyHeightChanges) {
  const std::string id = ok("create_stair", {{"path", {{1000, 1000}, {3730, 1000}}}, {"rise", 2800}, {"open_floor", true}})["id"];
  const std::string fixed = ok("create_stair", {{"path", {{1000, 3000}, {3730, 3000}}}, {"rise", 2800}, {"risers", 14}})["id"];
  const auto steps = modeler.history().undoCount();
  raiseSecondFloor(3000);
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  // Automatic: 15 risers of 200, 14 treads of 195; the top tread one riser under the floor.
  json info = ok("stair_info", {{"id", id}});
  EXPECT_EQ(info["risers"], 15);
  EXPECT_NEAR(info["riser"].get<double>(), 200, 1e-9);
  EXPECT_NEAR(info["going_min"].get<double>(), 195, 1e-9);
  EXPECT_NEAR(info["rise"].get<double>(), 3000, 1e-9);
  EXPECT_NEAR(top(id), 2800, 1e-6);
  // A number given: the same 14, each higher.
  info = ok("stair_info", {{"id", fixed}});
  EXPECT_EQ(info["risers"], 14);
  EXPECT_NEAR(info["riser"].get<double>(), 3000.0 / 14, 1e-9);
  EXPECT_NEAR(top(fixed), 3000 - 3000.0 / 14, 1e-6);
  // The floor went up with its level and is open for the new treads (underside 2800: from the 5th).
  EXPECT_NEAR(top(slab), 3000, 1e-6);
  EXPECT_NEAR(volume(slab), (kFloor - 10 * 195.0 * 800) * 200, 10.0);
  ASSERT_TRUE(modeler.undo());
  EXPECT_EQ(ok("stair_info", {{"id", id}})["risers"], 14);
  EXPECT_NEAR(volume(slab), (kFloor - 10 * kTread) * 200, 10.0);

  // The first floor raised: the foot goes up, the stair gets shorter.
  auto levels = modeler.document().settings().levels;
  for (auto& l : levels)
    if (l.name == "1FL") l.elevation = 300;
  ASSERT_TRUE(modeler.setLevels(levels));
  info = ok("stair_info", {{"id", id}});
  EXPECT_NEAR(info["base"].get<double>(), 300, 1e-9);
  EXPECT_NEAR(info["rise"].get<double>(), 2500, 1e-9);
  EXPECT_EQ(info["risers"], 13);
  ASSERT_TRUE(modeler.undo());

  // Saved and opened: the stair and the opened floor are what they were.
  const auto file = std::filesystem::temp_directory_path() / "sbr_stair_test.sbr";
  ASSERT_TRUE(modeler.save(file.string()));
  sbr::doc::Modeler other;
  ASSERT_TRUE(other.load(file.string()));
  const auto again = other.stairInfo(id);
  ASSERT_TRUE(again) << again.error();
  EXPECT_EQ(again->risers, 14);
  ASSERT_EQ(again->openedSlabs.size(), 1u);
  EXPECT_NEAR(sbr::geom::volume(other.document().find(slab)->shape), (kFloor - 10 * kTread) * 200, 10.0);
}

TEST_F(Stairs, WhatCannotBeDoneIsRefusedAndNothingChanges) {
  const auto steps = modeler.history().undoCount();
  // No floor over it at its top: with open_floor nothing is made.
  EXPECT_TRUE(refused("create_stair", {{"path", {{8000, 1000}, {10730, 1000}}}, {"rise", 2800}, {"open_floor", true}}));
  EXPECT_TRUE(refused("create_stair", {{"path", {{1000, 1000}, {3000, 1000}, {4000, 2500}}}, {"rise", 2800}}));
  EXPECT_EQ(modeler.history().undoCount(), steps);
  EXPECT_EQ(modeler.document().objects().size(), 1u);
  const std::string id = ok("create_stair", {{"path", {{1000, 1000}, {3730, 1000}}}, {"rise", 2800}, {"turns", "landing"}})["id"];
  EXPECT_TRUE(refused("change_stair", {{"id", id}, {"width", 100}}));
  EXPECT_TRUE(refused("stair_info", {{"id", slab}}));
  EXPECT_TRUE(refused("open_floor_for_stair", {{"stair", slab}}));
  // A stair reshaped by hand keeps its shape; its sizes can no longer be read.
  ok("push_pull", {{"id", id}, {"face", {{"normal", {0, -1, 0}}}}, {"distance", 100}});
  EXPECT_TRUE(refused("stair_info", {{"id", id}}));
}

// docs/112: the marks a plan draws on a stair.
TEST(Stair, APlanDrawsTheWalkingLineUpToTheCutAndABreakThere) {
  const StairSpec straight = along({gp_Pnt(0, 0, 0), gp_Pnt(2730, 0, 0)});
  // Cut at 1100: treads 1..5 are below it (top 200..1000), the 6th (1200) starts at 5 x 210.
  const auto marks = sbr::geom::stairPlanMarks(straight, 1100);
  ASSERT_TRUE(marks) << marks.error();
  EXPECT_TRUE(marks->cut);
  ASSERT_EQ(marks->walk.size(), 2u);
  EXPECT_NEAR(marks->walk.front().X(), 0, 1e-9);
  EXPECT_NEAR(marks->walk.back().X(), 1050, 1e-6);
  EXPECT_NEAR(marks->walk.back().Y(), 0, 1e-6);
  ASSERT_EQ(marks->arrow.size(), 2u);
  for (const auto& [a, b] : marks->arrow) {
    EXPECT_NEAR(a.X(), 1050, 1e-6);
    EXPECT_LT(b.X(), 1050);  // pointing up the stair
  }
  ASSERT_GE(marks->breakLine.size(), 2u);
  // Across the whole width (800) and a little more.
  EXPECT_LT(marks->breakLine.front().Y(), -400);
  EXPECT_GT(marks->breakLine.back().Y(), 400);
  EXPECT_LT(marks->label.X(), 0);  // "上" just before the foot
  // Seen from above the top (the floor above): the whole line, no break.
  const auto above = sbr::geom::stairPlanMarks(straight, 4000);
  ASSERT_TRUE(above);
  EXPECT_FALSE(above->cut);
  EXPECT_TRUE(above->breakLine.empty());
  EXPECT_NEAR(above->walk.back().X(), 2730, 1e-6);
  // Below the foot: nothing.
  EXPECT_FALSE(sbr::geom::stairPlanMarks(straight, -10));
  // An L with winders cut on its second flight: the line turns the corner.
  const StairSpec l = along({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp_Pnt(2000, 1500, 0)});
  const auto turned = sbr::geom::stairPlanMarks(l, 2300);  // 11 treads (top 2200) are below
  ASSERT_TRUE(turned);
  EXPECT_TRUE(turned->cut);
  EXPECT_EQ(turned->walk.size(), 3u);
  EXPECT_NEAR(turned->walk.back().X(), 2000, 1e-6);
  EXPECT_GT(turned->walk.back().Y(), 400);
}

TEST_F(Stairs, APlanDrawingCarriesTheStairMarks) {
  ok("create_stair", {{"path", {{1000, 1000}, {3730, 1000}}}, {"rise", 2800}});
  auto count = [&](double cut) {
    const auto view = sbr::draw::makeView(modeler.document(), sbr::draw::planView(cut));
    int up = 0, slanted = 0;
    if (view)
      for (const auto& item : view->items) {
        if (item.kind == sbr::draw::Item::Kind::Text && item.text == "上") ++up;
        if (item.kind == sbr::draw::Item::Kind::Line && item.points.size() == 2 &&
            std::abs(item.points[0].X() - item.points[1].X()) > 1e-6 && std::abs(item.points[0].Y() - item.points[1].Y()) > 1e-6)
          ++slanted;  // arrow strokes and the break line
      }
    return std::make_pair(up, slanted);
  };
  const auto low = count(1100);
  EXPECT_EQ(low.first, 1);
  EXPECT_GE(low.second, 4);
  EXPECT_EQ(count(-500).first, 0);  // below the foot
}

// docs/116: handrails along the stair, and the check they settle.
TEST(Stair, AHandrailRunsOverTheNosingsAndSettlesTheCheck) {
  StairSpec spec = along({gp_Pnt(0, 0, 0), gp_Pnt(2730, 0, 0)});
  const auto bare = makeStair(spec);
  ASSERT_TRUE(bare);
  EXPECT_EQ(state(*bare, "handrail"), "not_evaluated");
  spec.handrail = "right";
  const auto railed = makeStair(spec);
  ASSERT_TRUE(railed) << railed.error();
  // 13 rises of 210 along, 200 up (front corners of the 13 treads, then the top edge).
  EXPECT_NEAR(railed->handrailLength, 13 * std::hypot(210.0, 200.0), 1e-6);
  EXPECT_EQ(solids(railed->solid), 1 + 13);
  EXPECT_NEAR(sbr::geom::volume(railed->solid) - sbr::geom::volume(bare->solid),
              std::acos(-1.0) * 17.5 * 17.5 * railed->handrailLength, 50.0);
  EXPECT_EQ(state(*railed, "handrail"), "ok");
  // On the right (looking up the +x stair: -y), 60 in from the side, 850 over the nosings.
  const auto b = sbr::geom::exactBounds(railed->solid);
  EXPECT_NEAR(b.CornerMin().Y(), -400, 1e-6);  // the stair itself
  EXPECT_NEAR(b.CornerMax().Z(), 2800 + 850 + 17.5, 15.0);  // the rail's top end
  // Both sides: twice as long.
  spec.handrail = "both";
  const auto both = makeStair(spec);
  ASSERT_TRUE(both);
  EXPECT_NEAR(both->handrailLength, 2 * railed->handrailLength, 1e-6);
  // Around a winder turn, on its inner side, the rail goes up at the corner.
  StairSpec turn = along({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp_Pnt(2000, 1500, 0)});
  turn.handrail = "left";
  const auto inner = makeStair(turn);
  ASSERT_TRUE(inner) << inner.error();
  EXPECT_GT(inner->handrailLength, 2000.0);
  turn.handrail = "right";
  ASSERT_TRUE(makeStair(turn));
  // Refused: an unknown side, a height out of range.
  spec.handrail = "middle";
  EXPECT_FALSE(makeStair(spec));
  spec.handrail = "left";
  spec.handrailHeight = 300;
  EXPECT_FALSE(makeStair(spec));
}

TEST_F(Stairs, AHandrailIsChangedOnTheStairAndKept) {
  const std::string id = ok("create_stair", {{"path", {{1000, 1000}, {3730, 1000}}}, {"rise", 2800}, {"handrail", "right"}})["id"];
  json info = ok("stair_info", {{"id", id}});
  EXPECT_EQ(info["handrail"], "right");
  bool settled = false;
  for (const json& c : info["checks"])
    if (c["item"] == "handrail") settled = c["state"] == "ok";
  EXPECT_TRUE(settled);
  ok("change_stair", {{"id", id}, {"handrail", "both"}, {"handrail_height", 900}});
  info = ok("stair_info", {{"id", id}});
  EXPECT_EQ(info["handrail"], "both");
  EXPECT_NEAR(info["handrail_height"].get<double>(), 900, 1e-9);
  // Saved and opened again; and a stair with no handrail key reads as none.
  const auto file = std::filesystem::temp_directory_path() / "sbr_stair_rail.sbr";
  ASSERT_TRUE(modeler.save(file.string()));
  sbr::doc::Modeler other;
  ASSERT_TRUE(other.load(file.string()));
  EXPECT_EQ(other.stairInfo(id)->spec.handrail, "both");
  EXPECT_FALSE(call("change_stair", {{"id", id}, {"handrail_height", 2000}}).value("ok", true));
}
