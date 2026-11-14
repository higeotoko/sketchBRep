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
  // Up: nothing follows.
  json moved = ok("move", {{"ids", {south}}, {"vector", {0, 0, 500}}});
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

TEST_F(WallJoins, AWallSlidAlongItsLineCarriesTheWallsAtItsCorners) {
  // docs/102: the south wall 700 along itself: the east and west walls (at its corners) go
  // with it as they are; the north wall, joined to those, follows at both ends; the
  // partition (a T on the south wall, not at a corner) stays.
  const Box s0 = box(south), e0 = box(east), n0 = box(north), w0 = box(west), p0 = box(partition);
  const double total = volume(south) + volume(east) + volume(north) + volume(west) + volume(partition);
  const auto steps = modeler.history().undoCount();
  const json moved = ok("move", {{"ids", {south}}, {"vector", {700, 0, 0}}});
  EXPECT_EQ(moved["joined_walls"]["carried"], 2);
  EXPECT_EQ(moved["joined_walls"]["followed"], 1);
  EXPECT_TRUE(moved["joined_walls"]["left"].empty());
  for (const auto& [was, is] : {std::pair{s0, box(south)}, std::pair{e0, box(east)}, std::pair{w0, box(west)}, std::pair{n0, box(north)}}) {
    EXPECT_NEAR(is.x0, was.x0 + 700, 1e-6);
    EXPECT_NEAR(is.x1, was.x1 + 700, 1e-6);
    EXPECT_NEAR(is.y0, was.y0, 1e-6);
    EXPECT_NEAR(is.y1, was.y1, 1e-6);
  }
  EXPECT_NEAR(box(partition).x0, p0.x0, 1e-6);
  EXPECT_NEAR(volume(south) + volume(east) + volume(north) + volume(west) + volume(partition), total, 10);
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(box(east).x0, e0.x0, 1e-6);
  EXPECT_NEAR(box(north).x0, n0.x0, 1e-6);

  // Slantwise (300 along, 455 out): carried by the part along, made longer by the part across.
  ok("move", {{"ids", {south}}, {"vector", {300, -455, 0}}});
  EXPECT_NEAR(box(south).x0, s0.x0 + 300, 1e-6);
  EXPECT_NEAR(box(south).y0, s0.y0 - 455, 1e-6);
  EXPECT_NEAR(box(east).x0, e0.x0 + 300, 1e-6);
  EXPECT_NEAR(box(east).y0, e0.y0 - 455, 1e-6);
  EXPECT_NEAR(box(east).y1, e0.y1, 1e-6);
  EXPECT_NEAR(box(west).x0, w0.x0 + 300, 1e-6);
  EXPECT_NEAR(box(west).y0, w0.y0 - 455, 1e-6);
  EXPECT_NEAR(box(north).x0, n0.x0 + 300, 1e-6);
  EXPECT_NEAR(box(north).x1, n0.x1 + 300, 1e-6);
  EXPECT_NEAR(box(partition).y0, p0.y0 - 455, 1e-6);  // its T follows the south wall's face
  ASSERT_TRUE(modeler.undo());

  // A locked wall at the corner stays; the rest goes.
  ok("set_locked", {{"ids", {west}}, {"locked", true}});
  const json partly = ok("move", {{"ids", {south}}, {"vector", {700, 0, 0}}});
  EXPECT_EQ(partly["joined_walls"]["carried"], 1);
  ASSERT_EQ(partly["joined_walls"]["left"].size(), 1u);
  EXPECT_NEAR(box(west).x0, w0.x0, 1e-6);
  EXPECT_NEAR(box(east).x0, e0.x0 + 700, 1e-6);
}

TEST_F(WallJoins, AWallMadeLongerCarriesTheWallAtThatCorner) {
  // The south wall runs from x = 0 towards x = 6000: 1000 longer, its far end goes east. The
  // east wall is carried 1000; the north wall, joined to it, gets 1000 longer.
  const Box s0 = box(south), e0 = box(east), n0 = box(north), w0 = box(west), p0 = box(partition);
  const double length = ok("member_info", {{"id", south}})["fields"][0]["value"].get<double>();
  const auto steps = modeler.history().undoCount();
  ok("change_member", {{"id", south}, {"length", length + 1000}});
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  EXPECT_EQ(modeler.lastJoinFollow().carried, 1);
  EXPECT_EQ(modeler.lastJoinFollow().followed, 1);
  EXPECT_NEAR(box(south).x1, s0.x1 + 1000, 1e-6);
  EXPECT_NEAR(box(south).x0, s0.x0, 1e-6);
  EXPECT_NEAR(box(east).x0, e0.x0 + 1000, 1e-6);
  EXPECT_NEAR(box(east).x1, e0.x1 + 1000, 1e-6);
  EXPECT_NEAR(box(east).y0, e0.y0, 1e-6);
  EXPECT_NEAR(box(north).x1, n0.x1 + 1000, 1e-6);
  EXPECT_NEAR(box(north).x0, n0.x0, 1e-6);
  EXPECT_NEAR(box(west).x0, w0.x0, 1e-6);
  EXPECT_NEAR(box(partition).x0, p0.x0, 1e-6);
  // The room is 1000 longer (6000 -> 7000 on the wall lines).
  EXPECT_NEAR(ok("room_at", {{"at", {4500, 2000}}})["area_centre"].get<double>(), (7.0 - 3.0) * 4.0, 1e-9);
  ASSERT_TRUE(modeler.undo());  // one step for all of it
  EXPECT_NEAR(box(east).x0, e0.x0, 1e-6);
  EXPECT_NEAR(box(north).x1, n0.x1, 1e-6);

  // Shorter, with an opening that would be left outside the north wall: the north wall stays
  // (said), the rest goes.
  ok("place_openings", {{"host", north}, {"face_normal", {0, 1, 0}}, {"count", 1}, {"width", 5000}, {"height", 1000}, {"sill", 900}});
  ok("change_member", {{"id", south}, {"length", length - 1000}});
  EXPECT_EQ(modeler.lastJoinFollow().carried, 1);
  ASSERT_EQ(modeler.lastJoinFollow().left.size(), 1u);
  EXPECT_NEAR(box(east).x0, e0.x0 - 1000, 1e-6);
  EXPECT_NEAR(box(north).x1, n0.x1, 1e-6);
  ASSERT_TRUE(modeler.undo());

  // Asked not to keep joints: the wall alone.
  modeler.setKeepWallJoins(false);
  ok("change_member", {{"id", south}, {"length", length + 1000}});
  EXPECT_NEAR(box(south).x1, s0.x1 + 1000, 1e-6);
  EXPECT_NEAR(box(east).x0, e0.x0, 1e-6);
  modeler.setKeepWallJoins(true);
}

TEST_F(WallJoins, ASlabsOutlineGoesWithTheWallsItStandsIn) {
  // A floor on the wall lines, and a deck beside the house that touches no wall.
  const std::string floor = ok("create_slab", {{"outline", {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}}}, {"top", 0}, {"thickness", 200}})["id"];
  const std::string deck = ok("create_slab", {{"outline", {{8000, 0}, {9000, 0}, {9000, 1000}, {8000, 1000}}}, {"top", 0}, {"thickness", 100}})["id"];
  const Box d0 = box(deck);
  EXPECT_NEAR(volume(floor), 6000.0 * 4000 * 200, 1);
  // The south wall 455 out: the floor's two south corners go with it.
  json moved = ok("move", {{"ids", {south}}, {"vector", {0, -455, 0}}});
  EXPECT_EQ(moved["joined_walls"]["slabs"], 1);
  EXPECT_NEAR(box(floor).y0, -455, 1e-6);
  EXPECT_NEAR(box(floor).y1, 4000, 1e-6);
  EXPECT_NEAR(volume(floor), 6000.0 * 4455 * 200, 1);
  EXPECT_NEAR(box(floor).z1, 0, 1e-6);  // its top stays
  EXPECT_NEAR(box(deck).x0, d0.x0, 1e-6);
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(volume(floor), 6000.0 * 4000 * 200, 1);
  // The south wall slid 700: the east and west walls are carried, and the floor with them.
  moved = ok("move", {{"ids", {south}}, {"vector", {700, 0, 0}}});
  EXPECT_EQ(moved["joined_walls"]["slabs"], 1);
  EXPECT_NEAR(box(floor).x0, 700, 1e-6);
  EXPECT_NEAR(box(floor).x1, 6700, 1e-6);
  EXPECT_NEAR(volume(floor), 6000.0 * 4000 * 200, 1);
  ASSERT_TRUE(modeler.undo());
  // The south wall 1000 longer: the floor's east corners go with the east wall.
  const double length = ok("member_info", {{"id", south}})["fields"][0]["value"].get<double>();
  ok("change_member", {{"id", south}, {"length", length + 1000}});
  EXPECT_EQ(modeler.lastJoinFollow().slabs, 1);
  EXPECT_NEAR(box(floor).x1, 7000, 1e-6);
  EXPECT_NEAR(box(floor).x0, 0, 1e-6);
  EXPECT_NEAR(volume(floor), 7000.0 * 4000 * 200, 1);
  ASSERT_TRUE(modeler.undo());
  // The floor moved with the wall: it is not reshaped as well.
  moved = ok("move", {{"ids", {south, floor}}, {"vector", {0, -455, 0}}});
  EXPECT_NEAR(volume(floor), 6000.0 * 4000 * 200, 1);
  EXPECT_NEAR(box(floor).y0, -455, 1e-6);
  EXPECT_NEAR(box(floor).y1, 3545, 1e-6);
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

// docs/107: corners that are not right angles stay closed too. A 8000 x 6000 room with its
// south-east corner cut at 45 degrees: (6000, 0) - (8000, 2000).
TEST(SkewWallJoins, ACornerThatIsNotARightAngleStaysClosed) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  const json ids = ok("create_wall", {{"points", {{0, 0}, {6000, 0}, {8000, 2000}, {8000, 6000}, {0, 6000}, {0, 0}}},
                                      {"thickness", 200}, {"height", 3000}})["ids"];
  ASSERT_EQ(ids.size(), 5u);
  const std::string diagonal = ids[1], east = ids[2];
  auto area = [&] {
    const json r = ok("room_at", {{"at", {3000, 3000}}});
    return r.value("closed", false) ? r["area_centre"].get<double>() : -1.0;
  };
  EXPECT_NEAR(area(), 48.0 - 2.0, 1e-6);
  // The east wall 1000 out: the diagonal runs on to x = 9000 (at y = 3000), the east wall
  // itself starts there, the north wall is longer.
  const auto steps = modeler.history().undoCount();
  ok("move", {{"ids", {east}}, {"vector", {1000, 0, 0}}});
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  EXPECT_NEAR(area(), 54.0 - 4.5, 1e-6);
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(area(), 46.0, 1e-6);
  // The diagonal 500 * sqrt(2) outwards: it gets shorter between the walls it joins,
  // which get longer; the corner cut is 1000 x 1000.
  ok("move", {{"ids", {diagonal}}, {"vector", {500, -500, 0}}});
  EXPECT_NEAR(area(), 48.0 - 0.5, 1e-6);
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(area(), 46.0, 1e-6);
  // Inwards as well.
  ok("move", {{"ids", {diagonal}}, {"vector", {-500, 500, 0}}});
  EXPECT_NEAR(area(), 48.0 - 4.5, 1e-6);
}
