// Wall joints follow a moved wall (docs/90 B-04, G-05): the walls butting against a wall that
// is moved sideways are made longer or shorter so corners and T joints stay; their openings
// stay where they are; a wall that cannot follow stays as it was and the move still happens.
#include <cmath>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Measure.h"

using json = nlohmann::json;
using sbr::api::CommandApi;

namespace {
struct Box {
  double x0, y0, z0, x1, y1, z1;
};

// A 6000 x 4000 room of 200 walls 3000 high (south, east, north, west) and a 100 partition
// across it at x = 3000, butting against the south and north walls.
struct WallJoins : testing::Test {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  std::string south, east, north, west, partition;
  json call(const std::string& command, json args = json::object()) {
    return json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
  }
  json ok(const std::string& command, json args = json::object()) {
    json r = call(command, args);
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  }
  Box box(const std::string& id) {
    const auto b = sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(id)));
    return {b.CornerMin().X(), b.CornerMin().Y(), b.CornerMin().Z(), b.CornerMax().X(), b.CornerMax().Y(), b.CornerMax().Z()};
  }
  double volume(const std::string& id) { return sbr::geom::volume(modeler.document().find(id)->shape); }
  json opening(const std::string& host) { return ok("list_openings", {{"ids", {host}}})["hosts"][0]["openings"][0]; }
  void SetUp() override {
    const json ids = ok("create_wall", {{"points", {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}, {0, 0}}},
                                        {"thickness", 200}, {"height", 3000}})["ids"];
    ASSERT_EQ(ids.size(), 4u);
    south = ids[0];
    east = ids[1];
    north = ids[2];
    west = ids[3];
    partition = ok("create_wall", {{"points", {{3000, 100}, {3000, 3900}}}, {"thickness", 100}, {"height", 3000}})["ids"][0];
  }
};

TEST_F(WallJoins, CornersAndTJointsFollowAWallMovedSideways) {
  const Box s0 = box(south), e0 = box(east), n0 = box(north), w0 = box(west), p0 = box(partition);
  const double total = volume(south) + volume(east) + volume(north) + volume(west) + volume(partition);
  const auto steps = modeler.history().undoCount();

  // The south wall 455 outwards.
  const json moved = ok("move", {{"ids", {south}}, {"vector", {0, -455, 0}}});
  EXPECT_EQ(moved["joined_walls"]["followed"], 3);
  EXPECT_TRUE(moved["joined_walls"]["left"].empty());
  const Box s1 = box(south), e1 = box(east), n1 = box(north), w1 = box(west), p1 = box(partition);
  EXPECT_NEAR(s1.y0, s0.y0 - 455, 1e-6);
  EXPECT_NEAR(s1.y1, s0.y1 - 455, 1e-6);
  EXPECT_NEAR(s1.x0, s0.x0, 1e-6);
  EXPECT_NEAR(s1.x1, s0.x1, 1e-6);
  for (const auto& [was, is] : {std::pair{e0, e1}, std::pair{w0, w1}, std::pair{p0, p1}}) {
    EXPECT_NEAR(is.y0, was.y0 - 455, 1e-6);  // the end at the moved wall goes with it
    EXPECT_NEAR(is.y1, was.y1, 1e-6);        // the other end stays
    EXPECT_NEAR(is.x0, was.x0, 1e-6);
    EXPECT_NEAR(is.x1, was.x1, 1e-6);
  }
  EXPECT_NEAR(n1.y0, n0.y0, 1e-6);
  EXPECT_NEAR(n1.y1, n0.y1, 1e-6);
  // Longer by exactly the strip between: two 200 walls and the 100 partition, 455 each.
  EXPECT_NEAR(volume(south) + volume(east) + volume(north) + volume(west) + volume(partition),
              total + 455.0 * (200 + 200 + 100) * 3000, 10);
  // Still the walls their tool made: sizes editable, the same objects.
  EXPECT_TRUE(ok("member_info", {{"id", east}}).contains("fields"));

  // One undo step for the wall and its neighbours.
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(box(east).y0, e0.y0, 1e-6);
  EXPECT_NEAR(box(partition).y0, p0.y0, 1e-6);
  EXPECT_NEAR(box(south).y0, s0.y0, 1e-6);

  // Back inwards: they get shorter.
  ok("move", {{"ids", {south}}, {"vector", {0, 300, 0}}});
  EXPECT_NEAR(box(east).y0, e0.y0 + 300, 1e-6);
  EXPECT_NEAR(box(west).y0, w0.y0 + 300, 1e-6);
  EXPECT_NEAR(box(partition).y0, p0.y0 + 300, 1e-6);
}

TEST_F(WallJoins, OpeningsStayWhereTheyAre) {
  // A window in the moved wall goes with it; one in a following wall stays in the world.
  ok("place_openings", {{"host", south}, {"face_normal", {0, -1, 0}}, {"count", 1}, {"width", 800}, {"height", 1000}, {"sill", 900}});
  ok("place_openings", {{"host", east}, {"face_normal", {1, 0, 0}}, {"count", 1}, {"width", 800}, {"height", 1000}, {"sill", 900}});
  const json southBefore = opening(south), eastBefore = opening(east);
  const Box e0 = box(east);
  const json moved = ok("move", {{"ids", {south}}, {"vector", {0, -455, 0}}});
  EXPECT_EQ(moved["joined_walls"]["followed"], 3);
  const json southAfter = opening(south), eastAfter = opening(east);
  EXPECT_EQ(southAfter["id"], southBefore["id"]);
  EXPECT_NEAR(southAfter["offset"].get<double>(), southBefore["offset"].get<double>(), 1e-6);
  EXPECT_NEAR(southAfter["center"][1].get<double>(), southBefore["center"][1].get<double>() - 455, 1e-6);
  EXPECT_EQ(eastAfter["id"], eastBefore["id"]);
  for (int k = 0; k < 3; ++k)
    EXPECT_NEAR(eastAfter["center"][k].get<double>(), eastBefore["center"][k].get<double>(), 1e-6);
  EXPECT_NEAR(eastAfter["sill_height"].get<double>(), 900, 1e-6);
  EXPECT_NEAR(eastAfter["width"].get<double>(), 800, 1e-6);
  EXPECT_NEAR(box(east).y0, e0.y0 - 455, 1e-6);
  EXPECT_NEAR(volume(east), 200.0 * (e0.y1 - e0.y0 + 455) * 3000 - 800.0 * 1000 * 200, 10);
}

TEST_F(WallJoins, AWallThatCannotFollowStaysAndTheMoveStillHappens) {
  // The east wall's window is around y = 2000: the south wall moved to there would leave it
  // outside the east wall. The east wall stays; the others follow.
  ok("place_openings", {{"host", east}, {"face_normal", {1, 0, 0}}, {"count", 1}, {"width", 800}, {"height", 1000}, {"sill", 900}});
  const Box e0 = box(east), w0 = box(west), s0 = box(south);
  const std::string eastName = modeler.document().find(east)->name;
  json moved = ok("move", {{"ids", {south}}, {"vector", {0, 2000, 0}}});
  EXPECT_EQ(moved["joined_walls"]["followed"], 2);
  ASSERT_EQ(moved["joined_walls"]["left"].size(), 1u);
  EXPECT_EQ(moved["joined_walls"]["left"][0], eastName);
  EXPECT_NEAR(box(south).y0, s0.y0 + 2000, 1e-6);
  EXPECT_NEAR(box(east).y0, e0.y0, 1e-6);
  EXPECT_NEAR(box(west).y0, w0.y0 + 2000, 1e-6);
  EXPECT_EQ(ok("list_openings", {{"ids", {east}}})["hosts"][0]["openings"].size(), 1u);
  ASSERT_TRUE(modeler.undo());

  // A locked neighbour stays too.
  ok("set_locked", {{"ids", {west}}, {"locked", true}});
  moved = ok("move", {{"ids", {south}}, {"vector", {0, -455, 0}}});
  EXPECT_EQ(moved["joined_walls"]["followed"], 2);  // east and the partition
  ASSERT_EQ(moved["joined_walls"]["left"].size(), 1u);
  EXPECT_NEAR(box(west).y0, w0.y0, 1e-6);
  EXPECT_NEAR(box(east).y0, e0.y0 - 455, 1e-6);
}

TEST_F(WallJoins, OnlySidewaysMovesOfWallsAreFollowed) {
  const std::string saved = [&] {
    const Box e = box(east), w = box(west), p = box(partition), n = box(north);
    return json{e.x0, e.y0, e.x1, e.y1, w.x0, w.y0, w.x1, w.y1, p.x0, p.y0, p.x1, p.y1, n.x0, n.y0, n.x1, n.y1}.dump();
  }();
  auto others = [&] {
    const Box e = box(east), w = box(west), p = box(partition), n = box(north);
    return json{e.x0, e.y0, e.x1, e.y1, w.x0, w.y0, w.x1, w.y1, p.x0, p.y0, p.x1, p.y1, n.x0, n.y0, n.x1, n.y1}.dump();
  };
  // Along its own line: the wall slides, nothing is stretched.
  json moved = ok("move", {{"ids", {south}}, {"vector", {700, 0, 0}}});
  EXPECT_FALSE(moved.contains("joined_walls"));
  EXPECT_EQ(others(), saved);
  ASSERT_TRUE(modeler.undo());
  // Up: nothing follows.
  moved = ok("move", {{"ids", {south}}, {"vector", {0, 0, 500}}});
  EXPECT_FALSE(moved.contains("joined_walls"));
  EXPECT_EQ(others(), saved);
  ASSERT_TRUE(modeler.undo());
  // Asked not to keep joints: the wall alone.
  moved = ok("move", {{"ids", {south}}, {"vector", {0, -455, 0}}, {"keep_joins", false}});
  EXPECT_FALSE(moved.contains("joined_walls"));
  EXPECT_EQ(others(), saved);
  EXPECT_TRUE(modeler.keepWallJoins());  // the session's setting is as it was
  ASSERT_TRUE(modeler.undo());
  // Everything moved together: nothing to stretch.
  moved = ok("move", {{"ids", {south, east, north, west, partition}}, {"vector", {0, -455, 0}}});
  EXPECT_FALSE(moved.contains("joined_walls"));
  ASSERT_TRUE(modeler.undo());
  // A rotation leaves joints alone.
  ok("rotate", {{"ids", {south}}, {"axis_origin", {0, 0, 0}}, {"angle", 10}});
  EXPECT_EQ(others(), saved);
  ASSERT_TRUE(modeler.undo());
  // Something that is not a wall, moved: nothing follows.
  const std::string block = ok("create_box", {{"origin", {1000, 1000, 0}}, {"width", 500}, {"depth", 500}, {"height", 500}})["id"];
  moved = ok("move", {{"ids", {block}}, {"vector", {0, -455, 0}}});
  EXPECT_FALSE(moved.contains("joined_walls"));
  EXPECT_EQ(others(), saved);
}

TEST_F(WallJoins, TheWallATeeRunsIntoFollowsNothingButItsOwnCornerPartners) {
  // The partition moved along the room (sideways for it): it slides along the south and north
  // walls, which are not joined *to* it; nothing is stretched.
  const Box s0 = box(south), n0 = box(north), p0 = box(partition);
  const json moved = ok("move", {{"ids", {partition}}, {"vector", {910, 0, 0}}});
  EXPECT_FALSE(moved.contains("joined_walls"));
  EXPECT_NEAR(box(partition).x0, p0.x0 + 910, 1e-6);
  EXPECT_NEAR(box(south).x0, s0.x0, 1e-6);
  EXPECT_NEAR(box(south).x1, s0.x1, 1e-6);
  EXPECT_NEAR(box(north).x1, n0.x1, 1e-6);
  // The east wall moved in by 1000: the south and north walls (its corner partners) follow.
  const json east1 = ok("move", {{"ids", {east}}, {"vector", {-1000, 0, 0}}});
  EXPECT_EQ(east1["joined_walls"]["followed"], 2);
  EXPECT_NEAR(box(south).x1, s0.x1 - 1000, 1e-6);
  EXPECT_NEAR(box(north).x1, n0.x1 - 1000, 1e-6);
  EXPECT_NEAR(box(south).x0, s0.x0, 1e-6);
}
}  // namespace
