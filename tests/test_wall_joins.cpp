// Wall joints follow a moved wall (docs/90 B-04, G-05): the walls butting against a wall that
// is moved sideways are made longer or shorter so corners and T joints stay; their openings
// stay where they are; a wall that cannot follow stays as it was and the move still happens.
#include <cmath>
#include <gtest/gtest.h>
#include <TopExp_Explorer.hxx>
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

// docs/169: columns standing in a wall, beams along one and a roof bearing on the walls go with
// a wall moved sideways; a beam across the walls stays.
TEST_F(WallJoins, ColumnsBeamsAndRoofsGoWithAWallMovedSideways) {
  auto member = [&](std::vector<double> a, std::vector<double> b, const char* role, double depth) {
    return ok("create_member", {{"start", a}, {"end", b}, {"role", role}, {"section", {{"kind", "rectangle"}, {"width", 105}, {"depth", depth}}}})["id"]
        .get<std::string>();
  };
  const std::string corner = member({0, 0, 0}, {0, 0, 3000}, "column", 105);        // in the south-west corner
  const std::string middle = member({1500, 0, 0}, {1500, 0, 3000}, "column", 105);   // in the south wall
  const std::string inside = member({1500, 2000, 0}, {1500, 2000, 3000}, "column", 105);  // in the room: stays
  const std::string along = member({0, 0, 3000}, {6000, 0, 3000}, "beam", 240);      // on the south wall
  const std::string across = member({1500, 0, 3000}, {1500, 4000, 3000}, "beam", 240);  // from wall to wall: stays
  const std::string roof = ok("create_roof", {{"outline", {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}}}, {"kind", "hip"}, {"height", 3000}})["id"];
  const Box c0 = box(corner), m0 = box(middle), i0 = box(inside), a0 = box(along), x0 = box(across), r0 = box(roof);
  const auto steps = modeler.history().undoCount();
  const json moved = ok("move", {{"ids", {south}}, {"vector", {0, -455, 0}}});
  EXPECT_EQ(moved["joined_walls"]["members"], 3);  // the two columns and the beam along it
  EXPECT_EQ(moved["joined_walls"]["roofs"], 1);
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  EXPECT_NEAR(box(corner).y0, c0.y0 - 455, 1e-6);
  EXPECT_NEAR(box(corner).x0, c0.x0, 1e-6);  // the west wall did not go sideways
  EXPECT_NEAR(box(middle).y0, m0.y0 - 455, 1e-6);
  EXPECT_NEAR(box(inside).y0, i0.y0, 1e-6);
  EXPECT_NEAR(box(along).y0, a0.y0 - 455, 1e-6);
  EXPECT_NEAR(box(across).y0, x0.y0, 1e-6);
  EXPECT_NEAR(box(across).y1, x0.y1, 1e-6);
  // The roof's south eave 455 further south; its north eave where it was.
  EXPECT_NEAR(box(roof).y0, r0.y0 - 455, 1e-6);
  EXPECT_NEAR(box(roof).y1, r0.y1, 1e-6);
  EXPECT_NEAR(ok("roof_info", {{"id", roof}})["outline"][0][1].get<double>(), -455, 1e-6);
  // Back in one step.
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(box(corner).y0, c0.y0, 1e-6);
  EXPECT_NEAR(box(roof).y0, r0.y0, 1e-6);
  // The west wall out by 300: the corner column goes both ways' sum when both walls go (here one).
  ok("move", {{"ids", {west}}, {"vector", {-300, 0, 0}}});
  EXPECT_NEAR(box(corner).x0, c0.x0 - 300, 1e-6);
  EXPECT_NEAR(box(middle).x0, m0.x0, 1e-6);  // not in the west wall
  // Turned off: only the wall moves.
  ASSERT_TRUE(modeler.undo());
  const json alone = ok("move", {{"ids", {south}}, {"vector", {0, -455, 0}}, {"keep_joins", false}});
  EXPECT_NEAR(box(corner).y0, c0.y0, 1e-6);
  EXPECT_NEAR(box(roof).y0, r0.y0, 1e-6);
  (void)alone;
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
  const double diagonalBefore = sbr::geom::volume(modeler.document().find(diagonal)->shape);
  ok("move", {{"ids", {diagonal}}, {"vector", {500, -500, 0}}});
  EXPECT_NEAR(area(), 48.0 - 0.5, 1e-6);
  // The wall itself is half as long (docs/159: it used to keep its length and stick out through both corners).
  EXPECT_NEAR(sbr::geom::volume(modeler.document().find(diagonal)->shape), diagonalBefore / 2, 1.0);
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(area(), 46.0, 1e-6);
  // Inwards as well.
  ok("move", {{"ids", {diagonal}}, {"vector", {-500, 500, 0}}});
  EXPECT_NEAR(area(), 48.0 - 4.5, 1e-6);
}

// docs/159: mitred corners. The walls stay walls as drawn; their solids end on the corner's diagonal.
namespace {
int faces(const sbr::doc::Modeler& m, const std::string& id) {
  int n = 0;
  for (TopExp_Explorer e(m.document().find(id)->shape, TopAbs_FACE); e.More(); e.Next()) ++n;
  return n;
}
}  // namespace

TEST_F(WallJoins, MitredCornersMeetOnTheDiagonal) {
  const Box s0 = box(south), p0 = box(partition);
  const double total = volume(south) + volume(east) + volume(north) + volume(west);
  const double squareSouth = volume(south);
  const auto steps = modeler.history().undoCount();
  // Every wall marked: the four corners (the partition's ends are T joints: as they are).
  EXPECT_EQ(ok("mitre_wall_corners")["corners"], 4);
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  // Each wall a trapezoid in plan: outside face 200 longer than the inside face.
  EXPECT_NEAR(volume(south), (6200.0 + 5800.0) / 2 * 200 * 3000, 1.0);
  EXPECT_NEAR(volume(north), (6200.0 + 5800.0) / 2 * 200 * 3000, 1.0);
  EXPECT_NEAR(volume(east), (4200.0 + 3800.0) / 2 * 200 * 3000, 1.0);
  EXPECT_NEAR(volume(west), (4200.0 + 3800.0) / 2 * 200 * 3000, 1.0);
  EXPECT_NEAR(volume(south) + volume(east) + volume(north) + volume(west), total, 1.0);
  for (const std::string& id : {south, east, north, west}) EXPECT_EQ(faces(modeler, id), 6) << id;  // no seams left
  EXPECT_NEAR(box(south).x0, -100, 1e-6);
  EXPECT_NEAR(box(south).x1, 6100, 1e-6);
  EXPECT_NEAR(box(east).y0, -100, 1e-6);
  EXPECT_NEAR(box(east).y1, 4100, 1e-6);
  EXPECT_NEAR(box(partition).y0, p0.y0, 1e-6);
  EXPECT_NEAR(volume(partition), 100.0 * 3800 * 3000, 1.0);
  // Still walls: the room is found, an opening can be cut, the joins follow a move (one undo step),
  // and the corners are mitred again where the walls are now.
  json room = ok("room_at", {{"at", {1500, 2000}}});
  EXPECT_TRUE(room.value("closed", false));
  ok("place_openings", {{"host", south}, {"face_normal", {0, -1, 0}}, {"count", 1}, {"width", 900}, {"height", 2000}, {"sill", 0}});
  EXPECT_EQ(ok("list_openings", {{"ids", {south}}})["hosts"][0]["openings"].size(), 1u);
  EXPECT_NEAR(volume(south), (6200.0 + 5800.0) / 2 * 200 * 3000 - 900.0 * 2000 * 200, 1.0);
  const auto before = modeler.history().undoCount();
  ok("move", {{"ids", {south}}, {"vector", {0, -455, 0}}});
  EXPECT_EQ(modeler.history().undoCount(), before + 1);
  EXPECT_NEAR(box(south).y0, s0.y0 - 455, 1e-6);
  EXPECT_NEAR(volume(east), (4655.0 + 4255.0) / 2 * 200 * 3000, 1.0);
  EXPECT_NEAR(volume(west), (4655.0 + 4255.0) / 2 * 200 * 3000, 1.0);
  EXPECT_NEAR(box(east).y0, -555, 1e-6);
  EXPECT_EQ(faces(modeler, east), 6);
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(volume(east), (4200.0 + 3800.0) / 2 * 200 * 3000, 1.0);
  // A wall of the corner erased: the other wall's end is square again there.
  ok("delete", {{"ids", {east}}});
  EXPECT_NEAR(box(north).x1, 5900, 1e-6);  // as drawn: it started at the east wall's inner face
  EXPECT_NEAR(volume(north), (6000.0 + 5800.0) / 2 * 200 * 3000, 1.0);  // mitred at the west end only
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(volume(north), (6200.0 + 5800.0) / 2 * 200 * 3000, 1.0);
  // Taken off again: square ends as drawn (the opening stays).
  EXPECT_EQ(ok("mitre_wall_corners", {{"on", false}})["corners"], 0);
  EXPECT_NEAR(volume(south), squareSouth - 900.0 * 2000 * 200, 1.0);
  EXPECT_FALSE(modeler.document().find(south)->attributes.count("corner_mitre_made"));
  EXPECT_EQ(faces(modeler, east), 6);
  // Only one wall of a corner marked: nothing is mitred.
  EXPECT_EQ(ok("mitre_wall_corners", {{"walls", {east}}})["corners"], 0);
  EXPECT_NEAR(volume(east) + volume(north) + volume(west) + volume(south), total - 900.0 * 2000 * 200, 1.0);
  // Refused: not a wall.
  const std::string block = ok("create_box", {{"origin", {9000, 0, 0}}, {"width", 500}, {"depth", 500}, {"height", 500}})["id"];
  EXPECT_FALSE(call("mitre_wall_corners", {{"walls", {block}}}).value("ok", true));
}

// docs/177: walls mitred from the start (the wall tool's default; 'mitre' in create_wall), in the
// same undo step as they are made; walls drawn on from them later are mitred where they meet.
TEST(MitredWalls, MitredAsTheyAreDrawn) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  auto volume = [&](const std::string& id) { return sbr::geom::volume(modeler.document().find(id)->shape); };
  auto bounds = [&](const std::string& id) {
    return sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(id)));
  };
  const auto steps = modeler.history().undoCount();
  // An L of two walls, mitred as drawn: one undo step, both trapezoids reaching the corner's
  // outer point (6100, -100); the open ends square on the drawn points.
  const json l = ok("create_wall", {{"points", {{0, 0}, {6000, 0}, {6000, 4000}}}, {"thickness", 200}, {"height", 3000}, {"mitre", true}})["ids"];
  ASSERT_EQ(l.size(), 2u);
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  EXPECT_FALSE(modeler.mitreNewWalls());  // the setting is put back
  const std::string south = l[0], east = l[1];
  EXPECT_EQ(faces(modeler, south), 6);
  EXPECT_EQ(faces(modeler, east), 6);
  EXPECT_NEAR(volume(south), (6100.0 + 5900.0) / 2 * 200 * 3000, 1.0);
  EXPECT_NEAR(volume(east), (4100.0 + 3900.0) / 2 * 200 * 3000, 1.0);
  EXPECT_NEAR(bounds(east).CornerMin().Y(), -100, 1e-6);  // butt-joined it would start at 100
  ASSERT_TRUE(modeler.undo());
  // A closed room of a build-up, mitred as drawn, and a partition across it: four corners mitred
  // (each wall a trapezoid, no seams); at the T joints the partition's core goes on to the outer
  // walls' core (docs/177).
  const json room = ok("create_wall", {{"points", {{0, 0}, {0, 4000}, {6000, 4000}, {6000, 0}, {0, 0}}}, {"type", "exterior_siding"},
                                       {"height", 2700}, {"mitre", true}})["ids"];
  ASSERT_EQ(room.size(), 4u);
  const std::string inner = ok("create_wall", {{"points", {{3000, 65}, {3000, 3935}}}, {"type", "interior_board"}, {"height", 2700}, {"mitre", true}})["ids"][0];
  for (std::size_t i = 0; i < room.size(); ++i) {
    const json& id = room[i];
    // West and east: no seams (6 faces); north and south: also the notch of the T joint (4 more).
    EXPECT_EQ(faces(modeler, id.get<std::string>()), i % 2 == 0 ? 6 : 10) << id;
    EXPECT_TRUE(modeler.document().find(id.get<std::string>())->attributes.count("corner_mitre_made")) << id;
  }
  EXPECT_TRUE(modeler.document().find(inner)->attributes.count("corner_mitre_made"));
  EXPECT_NEAR(bounds(inner).CornerMin().Y(), 52.5, 1e-6);  // the outer wall's core face (its board 12.5 inside it)
  // Without 'mitre': butt-joined as before.
  const json plain = ok("create_wall", {{"points", {{10000, 0}, {16000, 0}, {16000, 4000}}}, {"thickness", 200}, {"height", 3000}})["ids"];
  EXPECT_NEAR(volume(plain[0].get<std::string>()), 6100.0 * 200 * 3000, 1.0);
  EXPECT_NEAR(volume(plain[1].get<std::string>()), 3900.0 * 200 * 3000, 1.0);
  EXPECT_FALSE(modeler.document().find(plain[0].get<std::string>())->attributes.count("corner_mitre"));
}

// docs/178: a run drawn on from the free end of another wall's line makes a corner with it, as
// within one run (butt-joined; mitred when both are marked), in the same undo step.
TEST(MitredWalls, DrawnOnFromAWallsEnd) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  auto volume = [&](const std::string& id) { return sbr::geom::volume(modeler.document().find(id)->shape); };
  auto bounds = [&](const std::string& id) {
    return sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(id)));
  };
  // Butt-joined: an L, then the north wall drawn on from the east wall's end.
  const json l = ok("create_wall", {{"points", {{0, 0}, {6000, 0}, {6000, 4000}}}, {"thickness", 200}, {"height", 3000}})["ids"];
  const std::string east = l[1];
  EXPECT_NEAR(bounds(east).CornerMax().Y(), 4000, 1e-6);
  const auto steps = modeler.history().undoCount();
  const std::string north = ok("create_wall", {{"points", {{6000, 4000}, {0, 4000}}}, {"thickness", 200}, {"height", 3000}})["ids"][0];
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  // The east wall runs on to the north wall's outer face; the north wall starts at its inner face.
  EXPECT_NEAR(bounds(east).CornerMax().Y(), 4100, 1e-6);
  EXPECT_NEAR(volume(east), 4000.0 * 200 * 3000, 1.0);
  EXPECT_NEAR(bounds(north).CornerMax().X(), 5900, 1e-6);
  EXPECT_NEAR(volume(north), 5900.0 * 200 * 3000, 1.0);
  // A room found where the walls close it (drawn on again, from the north wall's end to the
  // south wall's start: both ends joined).
  const std::string west = ok("create_wall", {{"points", {{0, 4000}, {0, 0}}}, {"thickness", 200}, {"height", 3000}})["ids"][0];
  EXPECT_NEAR(bounds(north).CornerMin().X(), -100, 1e-6);
  EXPECT_NEAR(bounds(west).CornerMax().Y(), 3900, 1e-6);
  EXPECT_NEAR(bounds(west).CornerMin().Y(), 100, 1e-6);
  EXPECT_NEAR(bounds(l[0].get<std::string>()).CornerMin().X(), -100, 1e-6);
  EXPECT_TRUE(ok("room_at", {{"at", {3000, 2000}}}).value("closed", false));
  // One undo takes the west wall and the others' ends back together.
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(bounds(north).CornerMin().X(), 0, 1e-6);
  EXPECT_NEAR(bounds(l[0].get<std::string>()).CornerMin().X(), 0, 1e-6);
  ASSERT_TRUE(modeler.undo());
  ASSERT_TRUE(modeler.undo());
  // Mitred: both reach the corner's outer point, each a trapezoid with 6 faces.
  const json m = ok("create_wall", {{"points", {{0, 0}, {6000, 0}, {6000, 4000}}}, {"thickness", 200}, {"height", 3000}, {"mitre", true}})["ids"];
  const std::string me = m[1];
  const std::string mn = ok("create_wall", {{"points", {{6000, 4000}, {0, 4000}}}, {"thickness", 200}, {"height", 3000}, {"mitre", true}})["ids"][0];
  EXPECT_NEAR(bounds(me).CornerMax().Y(), 4100, 1e-6);
  EXPECT_NEAR(bounds(mn).CornerMax().X(), 6100, 1e-6);
  EXPECT_EQ(faces(modeler, me), 6);
  EXPECT_EQ(faces(modeler, mn), 6);
  EXPECT_NEAR(volume(me), (4200.0 + 3800.0) / 2 * 200 * 3000, 1.0);
  // Not at a free end (the end already at a wall), or along the same line: as before.
  const std::string again = ok("create_wall", {{"points", {{6000, 4000}, {6000, 6000}}}, {"thickness", 200}, {"height", 3000}})["ids"][0];
  EXPECT_NEAR(bounds(me).CornerMax().Y(), 4100, 1e-6);
  EXPECT_NEAR(bounds(again).CornerMin().Y(), 4000, 1e-6);
}

// docs/179: a run drawn from the middle of another wall's line (or to it) is a T joint: it
// starts at that wall's face, as partitions are drawn; that wall stays as it is.
TEST(MitredWalls, DrawnFromTheMiddleOfAWall) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  auto volume = [&](const std::string& id) { return sbr::geom::volume(modeler.document().find(id)->shape); };
  auto bounds = [&](const std::string& id) {
    return sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(id)));
  };
  // Plain walls: a 200 room, a 100 partition drawn from the south wall's line to the north wall's.
  const json room = ok("create_wall", {{"points", {{0, 0}, {0, 4000}, {6000, 4000}, {6000, 0}, {0, 0}}}, {"thickness", 200}, {"height", 3000}})["ids"];
  double walls = 0;
  for (const json& id : room) walls += volume(id.get<std::string>());
  const std::string plain = ok("create_wall", {{"points", {{3000, 0}, {3000, 4000}}}, {"thickness", 100}, {"height", 3000}})["ids"][0];
  EXPECT_NEAR(bounds(plain).CornerMin().Y(), 100, 1e-6);
  EXPECT_NEAR(bounds(plain).CornerMax().Y(), 3900, 1e-6);
  EXPECT_NEAR(volume(plain), 100.0 * 3800 * 3000, 1.0);
  double after = 0;
  for (const json& id : room) after += volume(id.get<std::string>());
  EXPECT_NEAR(after, walls, 1.0);  // the room's walls as they were
  EXPECT_TRUE(ok("room_at", {{"at", {1500, 2000}}}).value("closed", false));
  EXPECT_TRUE(ok("room_at", {{"at", {4500, 2000}}}).value("closed", false));
  // Of a build-up and mitred (the wall tool): it starts at the outer walls' inner face (65 from
  // their line) and its core runs on to their core (docs/177).
  const json sided = ok("create_wall", {{"points", {{10000, 0}, {10000, 4000}, {16000, 4000}, {16000, 0}, {10000, 0}}}, {"type", "exterior_siding"},
                                        {"height", 2700}, {"mitre", true}})["ids"];
  const std::string inner = ok("create_wall", {{"points", {{13000, 0}, {13000, 4000}}}, {"type", "interior_board"}, {"height", 2700}, {"mitre", true}})["ids"][0];
  EXPECT_NEAR(volume(inner), 130.0 * 3870 * 2700 + 2 * 105.0 * 12.5 * 2700, 1.0);
  EXPECT_NEAR(bounds(inner).CornerMin().Y(), 52.5, 1e-6);
  // Drawn to a wall's line from inside the room (the last point on it): the same.
  const std::string half = ok("create_wall", {{"points", {{1500, 2000}, {1500, 4000}}}, {"thickness", 100}, {"height", 3000}})["ids"][0];
  EXPECT_NEAR(bounds(half).CornerMax().Y(), 3900, 1e-6);
  EXPECT_NEAR(bounds(half).CornerMin().Y(), 2000, 1e-6);
}

// docs/177: where a partition of a build-up meets an outer wall at a T (both mitred), the
// partition's core (柱) goes on through the outer wall's board up to its core, and the outer
// wall gives up its board over the partition's core: the boards turn the corner. Together the
// walls are as much as before; the layer lines in plans follow.
TEST(MitredWalls, PartitionsCoreRunsIntoTheWallAtATee) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  auto volume = [&](const std::string& id) { return sbr::geom::volume(modeler.document().find(id)->shape); };
  // Drawn clockwise: west, north, east, south; the partition from the south wall's inner face
  // (core 105 / 2 + board 12.5 = 65) to the north wall's.
  const json room = ok("create_wall", {{"points", {{0, 0}, {0, 4000}, {6000, 4000}, {6000, 0}, {0, 0}}}, {"type", "exterior_siding"}, {"height", 2700}})["ids"];
  const std::string north = room[1], south = room[3];
  const std::string inner = ok("create_wall", {{"points", {{3000, 65}, {3000, 3935}}}, {"type", "interior_board"}, {"height", 2700}})["ids"][0];
  double total = volume(inner);
  for (const json& id : room) total += volume(id.get<std::string>());
  const double partition = volume(inner), southBefore = volume(south);
  // Only the corners' walls marked: the T joints stay as they are.
  ok("mitre_wall_corners", {{"walls", room}});
  EXPECT_NEAR(volume(inner), partition, 1.0);
  const double southMitred = volume(south);
  // All marked: the partition's core 105 wide goes 12.5 into each outer wall.
  const auto steps = modeler.history().undoCount();
  ok("mitre_wall_corners", {{"walls", {inner}}});
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  const double piece = 105.0 * 12.5 * 2700;
  EXPECT_NEAR(volume(inner), partition + 2 * piece, 1.0);
  EXPECT_NEAR(volume(south), southMitred - piece, 1.0);
  EXPECT_NEAR(volume(north), southMitred - piece, 1.0);  // the same as the south wall
  double after = volume(inner);
  for (const json& id : room) after += volume(id.get<std::string>());
  EXPECT_NEAR(after, total, 1.0);
  (void)southBefore;
  // In the plan: the partition's core lines (x 2947.5, 3052.5) run from core face to core face;
  // the outer walls' core-face lines (y 52.5, 3947.5) break over the partition's core, so the
  // cores meet with no line across the partition (docs/196, the owner's checklist 2-8); their
  // board lines stay whole.
  {
    int across = 0, broken = 0;
    for (const auto& [a, b] : sbr::doc::Modeler::wallLayerLines(modeler.document(), 1000.0)) {
      if (std::abs(a.Y() - b.Y()) > 1e-6) continue;
      if (std::abs(a.Y() - 52.5) > 1e-6 && std::abs(a.Y() - 3947.5) > 1e-6) continue;
      const double x0 = std::min(a.X(), b.X()), x1 = std::max(a.X(), b.X());
      if (x0 < 2947.5 + 1e-6 && x1 > 3052.5 - 1e-6) ++across;
      if (std::abs(x1 - 2947.5) < 1e-6 || std::abs(x0 - 3052.5) < 1e-6) ++broken;
    }
    EXPECT_EQ(across, 0);
    EXPECT_EQ(broken, 4);  // each of the two lines ends at both sides of the partition's core
  }
  auto coreLines = [&] {
    double y0 = 1e300, y1 = -1e300;
    int n = 0;
    for (const auto& [a, b] : sbr::doc::Modeler::wallLayerLines(modeler.document(), 1000.0)) {
      if (std::abs(a.X() - b.X()) > 1e-6) continue;
      if (std::abs(a.X() - 2947.5) > 1e-6 && std::abs(a.X() - 3052.5) > 1e-6) continue;
      y0 = std::min({y0, a.Y(), b.Y()});
      y1 = std::max({y1, a.Y(), b.Y()});
      ++n;
    }
    return std::make_tuple(n, y0, y1);
  };
  {
    const auto [n, y0, y1] = coreLines();
    EXPECT_EQ(n, 2);
    EXPECT_NEAR(y0, 52.5, 1e-6);
    EXPECT_NEAR(y1, 3947.5, 1e-6);
  }
  // The partition moved sideways: the pieces go with it (one undo step), the walls still add up.
  ok("move", {{"ids", {inner}}, {"vector", {455, 0, 0}}});
  EXPECT_NEAR(volume(inner), partition + 2 * piece, 1.0);
  EXPECT_NEAR(volume(south), southMitred - piece, 1.0);
  after = volume(inner);
  for (const json& id : room) after += volume(id.get<std::string>());
  EXPECT_NEAR(after, total, 1.0);
  ASSERT_TRUE(modeler.undo());
  // Taken off the partition: as it was.
  ok("mitre_wall_corners", {{"walls", {inner}}, {"on", false}});
  EXPECT_NEAR(volume(inner), partition, 1.0);
  EXPECT_NEAR(volume(south), southMitred, 1.0);
  {
    const auto [n, y0, y1] = coreLines();
    EXPECT_EQ(n, 2);
    EXPECT_NEAR(y0, 65, 1e-6);
    EXPECT_NEAR(y1, 3935, 1e-6);
  }
}

// At corners that are not right angles the mitred walls leave no gap and do not overlap: together
// they are the outline's band (perimeter x thickness), exactly.
TEST(SkewWallJoins, MitredCornersCloseSlantedCorners) {
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
  EXPECT_EQ(ok("mitre_wall_corners", json::object())["corners"], 5);
  double total = 0.0;
  for (const json& id : ids) {
    total += sbr::geom::volume(modeler.document().find(id)->shape);
    EXPECT_EQ(faces(modeler, id), 6) << id;
  }
  const double perimeter = 6000 + 2000 * std::sqrt(2.0) + 4000 + 8000 + 6000;
  EXPECT_NEAR(total, perimeter * 200 * 3000, 10.0);
  // The slanted wall moved out: mitred again, still the band of the new outline
  // (the corner cut is 1000 x 1000: perimeter 7000 + 1000 sqrt 2 + 5000 + 8000 + 6000).
  ok("move", {{"ids", {ids[1]}}, {"vector", {500, -500, 0}}});
  total = 0.0;
  for (const json& id : ids) total += sbr::geom::volume(modeler.document().find(id)->shape);
  EXPECT_NEAR(total, (7000 + 1000 * std::sqrt(2.0) + 5000 + 8000 + 6000) * 200 * 3000, 10.0);
  const json room = ok("room_at", {{"at", {3000, 3000}}});
  EXPECT_TRUE(room.value("closed", false));
  EXPECT_NEAR(room["area_centre"].get<double>(), 48.0 - 0.5, 1e-6);
}

// docs/159: mitred walls under a roof, with a build-up, saved and read again.
TEST(MitredWalls, WithARoofABuildUpAndSavedAgain) {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  auto ok = [&](const std::string& command, json args) {
    json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  auto total = [&](const json& ids) {
    double v = 0.0;
    for (const json& id : ids) v += sbr::geom::volume(modeler.document().find(id)->shape);
    return v;
  };
  // Clockwise: the siding outside. A gable roof over it, the walls fitted to it.
  const json ids = ok("create_wall", {{"points", {{0, 0}, {0, 6000}, {8000, 6000}, {8000, 0}, {0, 0}}},
                                      {"type", "exterior_siding"}, {"height", 3000}})["ids"];
  const std::string roof = ok("create_roof", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"kind", "gable"}, {"height", 3000}})["id"];
  ok("fit_walls_to_roof", {{"roof", roof}});
  const double fitted = total(ids);
  // Mitred: the same material in all (each corner shared out along its diagonal), one solid each.
  EXPECT_EQ(ok("mitre_wall_corners", json::object())["corners"], 4);
  EXPECT_NEAR(total(ids), fitted, 1e-6 * fitted);
  for (const json& id : ids) {
    int solids = 0;
    for (TopExp_Explorer e(modeler.document().find(id)->shape, TopAbs_SOLID); e.More(); e.Next()) ++solids;
    EXPECT_EQ(solids, 1) << id;
  }
  // A steeper roof: the walls are fitted and mitred again (taller corner pieces), still one solid's worth.
  auto spec = modeler.roofInfo(roof)->spec;
  spec.pitch = 6;
  ASSERT_TRUE(modeler.changeRoof(roof, spec));
  const double steeper = total(ids);
  EXPECT_GT(steeper, fitted);
  ok("mitre_wall_corners", {{"on", false}});
  EXPECT_NEAR(total(ids), steeper, 1e-6 * steeper);  // square again: the same material
  ok("mitre_wall_corners", json::object());
  // Another build-up (thicker): mitred again in the same undo step; the layer lines as before (4).
  const auto steps = modeler.history().undoCount();
  ok("set_wall_type", {{"ids", ids}, {"type", "exterior_lath_mortar"}});
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  const double mortar = total(ids);
  ok("mitre_wall_corners", {{"on", false}});
  EXPECT_NEAR(total(ids), mortar, 1e-6 * mortar);
  ASSERT_TRUE(modeler.undo());
  // Saved and read again: the same solids, still walls (a move is followed and mitred again).
  sbr::doc::Modeler again;
  {
    sbr::doc::Document read;
    ASSERT_TRUE(sbr::doc::deserializeDocument(sbr::doc::serializeDocument(modeler.document()), read));
    double v = 0.0;
    for (const json& id : ids) v += sbr::geom::volume(read.worldShape(*read.find(id)));
    EXPECT_NEAR(v, mortar, 1e-6 * mortar);
  }
  const std::string room = ok("create_room", {{"at", {4000, 3000}}, {"name", "A"}})["id"];
  EXPECT_FALSE(room.empty());
  const json moved = ok("move", {{"ids", {ids[0]}}, {"vector", {-500, 0, 0}}});
  EXPECT_EQ(moved["joined_walls"]["followed"], 2);
  const double wider = total(ids);
  ok("mitre_wall_corners", {{"on", false}});
  EXPECT_NEAR(total(ids), wider, 1e-6 * wider);
}
