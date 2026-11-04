// Following (docs/42): an object follows another only when asked; the host
// moved or turned takes it along once, keeping its place on the host; moved
// by hand it keeps following from there; the host gone, it stays put.

#include <Bnd_Box.hxx>

#include <gtest/gtest.h>
#include <cmath>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"

using namespace sbr;
using namespace sbr::doc;
using json = nlohmann::json;

namespace {

gp_Pnt lowCorner(const Modeler& m, const ObjectId& id) {
  return geom::exactBounds(m.document().worldShape(*m.document().find(id))).CornerMin();
}

void expectAt(const Modeler& m, const ObjectId& id, double x, double y, double z) {
  const gp_Pnt p = lowCorner(m, id);
  EXPECT_NEAR(p.X(), x, 1e-6);
  EXPECT_NEAR(p.Y(), y, 1e-6);
  EXPECT_NEAR(p.Z(), z, 1e-6);
}

ObjectId box(Modeler& m, double x, double y, double z, double w, double d, double h) {
  geom::SketchPlane plane = geom::SketchPlane::ground();
  plane.origin = gp_Pnt(x, y, z);
  const ObjectId id = *m.createRectangle(plane, w, d);
  EXPECT_TRUE(m.extrude(id, h));
  return id;
}

gp_Trsf shift(double x, double y, double z) {
  gp_Trsf t;
  t.SetTranslation(gp_Vec(x, y, z));
  return t;
}

// A beam and a bracket (hardware) under it, off to one side.
struct BeamAndBracket {
  Modeler m;
  ObjectId beam, bracket;
  BeamAndBracket() {
    beam = box(m, 0, 0, 3000, 4000, 200, 400);
    bracket = box(m, 1000, 0, 2900, 100, 200, 100);
  }
};

}  // namespace

TEST(Follow, NothingFollowsUnlessAsked) {
  BeamAndBracket s;
  ASSERT_TRUE(s.m.move({s.beam}, gp_Vec(0, 500, 0)));
  expectAt(s.m, s.bracket, 1000, 0, 2900);  // UX06: not asked, not moved
  EXPECT_EQ(s.m.followState(s.bracket), Modeler::FollowState::None);
}

TEST(Follow, HostMovedOrTurnedTakesItAlongOnce) {
  BeamAndBracket s;
  const auto steps = s.m.history().undoCount();
  ASSERT_TRUE(s.m.setFollow({s.bracket}, s.beam));
  EXPECT_EQ(s.m.history().undoCount(), steps + 1);
  EXPECT_EQ(s.m.followState(s.bracket), Modeler::FollowState::Active);
  ASSERT_TRUE(s.m.move({s.beam}, gp_Vec(0, 500, 200)));  // UX07
  expectAt(s.m, s.bracket, 1000, 500, 3100);
  // One undo: the beam and the bracket back together.
  ASSERT_TRUE(s.m.undo());
  expectAt(s.m, s.beam, 0, 0, 3000);
  expectAt(s.m, s.bracket, 1000, 0, 2900);
  ASSERT_TRUE(s.m.redo());
  expectAt(s.m, s.bracket, 1000, 500, 3100);
  // Turned a quarter about the beam's start: the bracket turns with it.
  ASSERT_TRUE(s.m.rotate({s.beam}, gp_Ax1(gp_Pnt(0, 500, 0), gp::DZ()), M_PI / 2));
  const Bnd_Box b = geom::exactBounds(s.m.document().worldShape(*s.m.document().find(s.bracket)));
  EXPECT_NEAR(b.CornerMin().X(), -200, 1e-6);
  EXPECT_NEAR(b.CornerMin().Y(), 1500, 1e-6);
  EXPECT_NEAR(b.CornerMax().Y(), 1600, 1e-6);
}

TEST(Follow, MovedByHandKeepsFollowingFromThere) {
  BeamAndBracket s;
  ASSERT_TRUE(s.m.setFollow({s.bracket}, s.beam));
  // UX08 / UX30: moved off the beam by hand: put there, still following.
  ASSERT_TRUE(s.m.move({s.bracket}, gp_Vec(300, 0, -1000)));
  expectAt(s.m, s.bracket, 1300, 0, 1900);
  EXPECT_EQ(s.m.followState(s.bracket), Modeler::FollowState::Active);
  ASSERT_TRUE(s.m.move({s.beam}, gp_Vec(0, 0, 100)));
  expectAt(s.m, s.bracket, 1300, 0, 2000);
  // Typed into the properties' position: the same.
  ASSERT_TRUE(s.m.setPosition(s.bracket, gp_Vec(500, 0, 0)));
  const gp_Pnt put = lowCorner(s.m, s.bracket);
  ASSERT_TRUE(s.m.move({s.beam}, gp_Vec(10, 0, 0)));
  EXPECT_NEAR(lowCorner(s.m, s.bracket).X(), put.X() + 10, 1e-6);
}

TEST(Follow, MovedTogetherMovesOnce) {
  BeamAndBracket s;
  ASSERT_TRUE(s.m.setFollow({s.bracket}, s.beam));
  ASSERT_TRUE(s.m.move({s.beam, s.bracket}, gp_Vec(0, 1000, 0)));  // UX29
  expectAt(s.m, s.bracket, 1000, 1000, 2900);
  // Grouped together and the group moved: once as well.
  auto group = s.m.group({s.beam, s.bracket});
  ASSERT_TRUE(group);
  ASSERT_TRUE(s.m.move({*group}, gp_Vec(0, 0, 500)));
  expectAt(s.m, s.bracket, 1000, 1000, 3400);
  expectAt(s.m, s.beam, 0, 1000, 3500);
}

TEST(Follow, HostGoneItStaysAndRebasesOnReturn) {
  BeamAndBracket s;
  ASSERT_TRUE(s.m.setFollow({s.bracket}, s.beam));
  ASSERT_TRUE(s.m.remove({s.beam}));  // UX09
  expectAt(s.m, s.bracket, 1000, 0, 2900);
  EXPECT_EQ(s.m.followState(s.bracket), Modeler::FollowState::Suspended);
  // Moved meanwhile: where it is put (no host to follow).
  ASSERT_TRUE(s.m.move({s.bracket}, gp_Vec(0, 0, -100)));
  expectAt(s.m, s.bracket, 1000, 0, 2800);
  EXPECT_TRUE(s.m.document().find(s.bracket)->follow->rebase);

  // The beam back (undo of the delete): following again.
  BeamAndBracket t;
  ASSERT_TRUE(t.m.setFollow({t.bracket}, t.beam));
  const ObjectId beamId = t.beam;
  ASSERT_TRUE(t.m.remove({t.beam}));
  ASSERT_TRUE(t.m.undo());  // the beam back
  EXPECT_EQ(t.m.followState(t.bracket), Modeler::FollowState::Active);
  ASSERT_TRUE(t.m.move({beamId}, gp_Vec(0, 0, 50)));
  expectAt(t.m, t.bracket, 1000, 0, 2950);
}

TEST(Follow, CirclesAndContainmentAreRefused) {
  BeamAndBracket s;
  ASSERT_TRUE(s.m.setFollow({s.bracket}, s.beam));
  EXPECT_FALSE(s.m.setFollow({s.beam}, s.bracket));  // UX32
  EXPECT_FALSE(s.m.setFollow({s.beam}, s.beam));
  EXPECT_EQ(s.m.followState(s.beam), Modeler::FollowState::None);
  // Chains are fine: a bolt following the bracket goes along too.
  const ObjectId bolt = box(s.m, 1040, 80, 2850, 20, 20, 50);
  ASSERT_TRUE(s.m.setFollow({bolt}, s.bracket));
  EXPECT_FALSE(s.m.setFollow({s.beam}, bolt));
  ASSERT_TRUE(s.m.move({s.beam}, gp_Vec(100, 0, 0)));
  expectAt(s.m, bolt, 1140, 80, 2850);
  // Inside a group being followed: it moves with it already.
  auto group = s.m.group({s.beam});
  ASSERT_TRUE(group);
  const ObjectId inner = box(s.m, 0, 0, 0, 10, 10, 10);
  EXPECT_TRUE(s.m.setFollow({inner}, *group));
  // Stop following: the host then moves alone.
  ASSERT_TRUE(s.m.setFollow({s.bracket}, std::nullopt));
  ASSERT_TRUE(s.m.move({*group}, gp_Vec(0, 100, 0)));
  expectAt(s.m, s.bracket, 1100, 0, 2900);
}

TEST(Follow, HostShapeChangesMoveNothing) {
  BeamAndBracket s;
  ASSERT_TRUE(s.m.setFollow({s.bracket}, s.beam));
  // Made deeper (its own shape) or stretched: pose only, so the bracket
  // stays (UX39: never distorted or dragged by a shape change).
  ASSERT_TRUE(s.m.setParameter(s.beam, "op0.distance", 600));
  expectAt(s.m, s.bracket, 1000, 0, 2900);
  const double v = geom::volume(s.m.document().worldShape(*s.m.document().find(s.bracket)));
  ASSERT_TRUE(s.m.stretch({s.beam}, gp_Pnt(0, 0, 0), gp::DX(), gp::DY(), 2, 1, 1));
  EXPECT_NEAR(geom::volume(s.m.document().worldShape(*s.m.document().find(s.bracket))), v, 1e-6);
  expectAt(s.m, s.bracket, 1000, 0, 2900);
}

TEST(Follow, CopiesFollowTheirCopiedHostOrTheSameOne) {
  BeamAndBracket s;
  ASSERT_TRUE(s.m.setFollow({s.bracket}, s.beam));
  // Both copied: the bracket's copy follows the beam's copy.
  auto both = s.m.copy({s.beam, s.bracket}, shift(0, 1000, 0));
  ASSERT_TRUE(both);
  ASSERT_EQ(both->size(), 2u);
  const ObjectId beamCopy = (*both)[0], bracketCopy = (*both)[1];
  EXPECT_EQ(s.m.document().find(bracketCopy)->follow->host, beamCopy);
  ASSERT_TRUE(s.m.move({beamCopy}, gp_Vec(0, 0, 100)));
  expectAt(s.m, bracketCopy, 1000, 1000, 3000);
  expectAt(s.m, s.bracket, 1000, 0, 2900);
  // Only the bracket copied: the copy follows the same beam from its place.
  auto one = s.m.copy({s.bracket}, shift(2000, 0, 0));
  ASSERT_TRUE(one);
  ASSERT_TRUE(s.m.move({s.beam}, gp_Vec(0, 0, 10)));
  expectAt(s.m, one->front(), 3000, 0, 2910);
  expectAt(s.m, s.bracket, 1000, 0, 2910);
}

TEST(Follow, SavedAndAskedByTheAi) {
  Modeler m;
  api::CommandApi api(m);
  auto run = [&](json command) { return json::parse(api.execute(command.dump())); };
  const ObjectId beam = box(m, 0, 0, 3000, 4000, 200, 400);
  const ObjectId bracket = box(m, 1000, 0, 2900, 100, 200, 100);
  const json set = run({{"command", "set_follow"}, {"args", {{"ids", {bracket}}, {"host", beam}}}});
  ASSERT_TRUE(set.value("ok", false)) << set.dump();
  const json info = run({{"command", "get_object"}, {"args", {{"id", bracket}}}});
  EXPECT_EQ(info["result"]["follow"]["host"], beam);
  EXPECT_EQ(info["result"]["follow"]["state"], "active");
  const json circle = run({{"command", "set_follow"}, {"args", {{"ids", {beam}}, {"host", bracket}}}});
  EXPECT_FALSE(circle.value("ok", true));
  const std::string file = ::testing::TempDir() + "follow.sbr";
  ASSERT_TRUE(m.save(file));
  Modeler loaded;
  ASSERT_TRUE(loaded.load(file));
  ASSERT_TRUE(loaded.move({beam}, gp_Vec(0, 0, -100)));
  expectAt(loaded, bracket, 1000, 0, 2800);
  const json stop = run({{"command", "set_follow"}, {"args", {{"ids", {bracket}}, {"host", nullptr}}}});
  ASSERT_TRUE(stop.value("ok", false)) << stop.dump();
  EXPECT_FALSE(m.document().find(bracket)->follow);
}
