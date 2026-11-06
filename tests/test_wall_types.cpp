// Timber wall build-ups drawn on the column centre line (WallTypes.h, Modeler::createTypedWalls).
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <Bnd_Box.hxx>

#include "sbr/WallTypes.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"

using json = nlohmann::json;
using sbr::api::CommandApi;

namespace {
struct Api {
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
  Bnd_Box bounds(const json& ids) {
    Bnd_Box all;
    for (const auto& id : ids) all.Add(modeler.document().worldBounds(*modeler.document().find(id.get<std::string>())));
    return all;
  }
};
}  // namespace

TEST(WallTypes, LayersAddUpFromTheCoreCentre) {
  const auto siding = sbr::construction::wallType("exterior_siding");
  ASSERT_TRUE(siding);
  EXPECT_TRUE(siding->exterior);
  EXPECT_DOUBLE_EQ(siding->outside(), 16 + 18 + 9 + 52.5);
  EXPECT_DOUBLE_EQ(siding->inside(), 52.5 + 12.5);
  EXPECT_DOUBLE_EQ(siding->thickness(), 160.5);
  const auto wide = sbr::construction::wallType("exterior_siding", 120);
  ASSERT_TRUE(wide);
  EXPECT_DOUBLE_EQ(wide->outside(), 43 + 60);
  EXPECT_DOUBLE_EQ(wide->inside(), 72.5);
  const auto mortar = sbr::construction::wallType("exterior_lath_mortar");
  ASSERT_TRUE(mortar);
  EXPECT_DOUBLE_EQ(mortar->outside(), 20 + 12 + 18 + 9 + 52.5);
  const auto inner = sbr::construction::wallType("interior_board");
  ASSERT_TRUE(inner);
  EXPECT_FALSE(inner->exterior);
  EXPECT_DOUBLE_EQ(inner->outside(), inner->inside());
  EXPECT_DOUBLE_EQ(inner->thickness(), 130);
  for (const auto& t : sbr::construction::wallTypes()) {
    int cores = 0;
    for (const auto& l : t.layers) cores += l.role == "core";
    EXPECT_EQ(cores, 1) << t.id;
    EXPECT_EQ(json::parse(t.layersJson()).size(), t.layers.size());
  }
  EXPECT_FALSE(sbr::construction::wallType("brick"));
  EXPECT_FALSE(sbr::construction::wallType("interior_board", 10));
}

TEST(WallTypes, OpenLineHasTheOutsideOnItsLeft) {
  Api a;
  const json left = a.ok("create_wall", {{"points", {{0, 0}, {3640, 0}}}, {"height", 2800}, {"type", "exterior_siding"}})["ids"];
  ASSERT_EQ(left.size(), 1u);
  Bnd_Box b = a.bounds(left);
  EXPECT_NEAR(b.CornerMin().Y(), -65, 1e-6);    // inside: board + half the column (right of +X)
  EXPECT_NEAR(b.CornerMax().Y(), 95.5, 1e-6);   // outside: siding, batten, ply + half the column
  EXPECT_NEAR(b.CornerMax().Z(), 2800, 1e-6);
  const auto& o = *a.modeler.document().find(left[0].get<std::string>());
  EXPECT_EQ(o.attributes.at("kind"), "wall");
  EXPECT_EQ(o.attributes.at("wall_type"), "exterior_siding");
  EXPECT_EQ(o.attributes.at("wall_outside"), "left");
  EXPECT_EQ(json::parse(o.attributes.at("wall_layers")).size(), 5u);

  const json right = a.ok("create_wall", {{"points", {{0, 5000}, {3640, 5000}}}, {"height", 2800},
                                          {"type", "exterior_siding"}, {"outside", "right"}, {"core", 120}})["ids"];
  b = a.bounds(right);
  EXPECT_NEAR(b.CornerMin().Y(), 5000 - 103, 1e-6);
  EXPECT_NEAR(b.CornerMax().Y(), 5000 + 72.5, 1e-6);

  const json inner = a.ok("create_wall", {{"points", {{0, 9000}, {3640, 9000}}}, {"height", 2400}, {"type", "interior_board"}})["ids"];
  b = a.bounds(inner);
  EXPECT_NEAR(b.CornerMin().Y(), 9000 - 65, 1e-6);
  EXPECT_NEAR(b.CornerMax().Y(), 9000 + 65, 1e-6);

  // Unknown types and plain walls without a thickness are refused.
  EXPECT_FALSE(a.call("create_wall", {{"points", {{0, 0}, {1000, 0}}}, {"height", 2400}, {"type", "brick"}})["ok"].get<bool>());
  EXPECT_FALSE(a.call("create_wall", {{"points", {{0, 0}, {1000, 0}}}, {"height", 2400}})["ok"].get<bool>());
}

TEST(WallTypes, ClosedOutlineFacesOutEitherWayRound) {
  for (const bool clockwise : {false, true}) {
    Api a;
    json pts = clockwise ? json{{0, 0}, {0, 3640}, {5460, 3640}, {5460, 0}, {0, 0}}
                         : json{{0, 0}, {5460, 0}, {5460, 3640}, {0, 3640}, {0, 0}};
    const json ids = a.ok("create_wall", {{"points", pts}, {"height", 2800}, {"type", "exterior_lath_mortar"}})["ids"];
    ASSERT_EQ(ids.size(), 4u);
    const double out = 20 + 12 + 18 + 9 + 52.5;
    const Bnd_Box b = a.bounds(ids);
    EXPECT_NEAR(b.CornerMin().X(), -out, 1e-6) << clockwise;
    EXPECT_NEAR(b.CornerMin().Y(), -out, 1e-6) << clockwise;
    EXPECT_NEAR(b.CornerMax().X(), 5460 + out, 1e-6) << clockwise;
    EXPECT_NEAR(b.CornerMax().Y(), 3640 + out, 1e-6) << clockwise;
    // One undo step takes them all.
    a.ok("undo");
    for (const auto& id : ids) EXPECT_EQ(a.modeler.document().find(id.get<std::string>()), nullptr);
  }
}

namespace {
double volumeOf(Api& a, const json& ids) {
  double v = 0;
  for (const auto& id : ids) v += a.ok("get_object", {{"id", id}})["volume"].get<double>();
  return v;
}
}  // namespace

TEST(WallTypes, DrawnWallsTakeABuildUpAroundTheirLine) {
  Api a;
  // A plain ring, 200 thick on the lines, drawn anticlockwise, and a partition across it.
  const json ring = a.ok("create_wall", {{"points", {{0, 0}, {5460, 0}, {5460, 3640}, {0, 3640}, {0, 0}}},
                                         {"thickness", 200}, {"height", 2800}})["ids"];
  ASSERT_EQ(ring.size(), 4u);
  const json part = a.ok("create_wall", {{"points", {{2730, 0}, {2730, 3640}}}, {"thickness", 120}, {"height", 2800}})["ids"];
  ASSERT_EQ(part.size(), 1u);
  // A door in the south wall, cut from its outer face.
  const std::string south = ring[0];
  const std::string southWithDoor =
      a.ok("place_openings", {{"host", south}, {"face_normal", {0, -1, 0}}, {"at", {1200, 0, 0}},
                              {"width", 800}, {"height", 2000}})["id"];
  json walls = ring;
  walls[0] = southWithDoor;

  const json changed = a.ok("set_wall_type", {{"ids", walls}, {"type", "exterior_siding"}})["ids"];
  EXPECT_EQ(changed.size(), 5u);  // the ring and the partition's two ends
  const double out = 95.5, in = 65;
  const Bnd_Box ringBox = a.bounds(walls);
  EXPECT_NEAR(ringBox.CornerMin().X(), -out, 1e-6);
  EXPECT_NEAR(ringBox.CornerMin().Y(), -out, 1e-6);
  EXPECT_NEAR(ringBox.CornerMax().X(), 5460 + out, 1e-6);
  EXPECT_NEAR(ringBox.CornerMax().Y(), 3640 + out, 1e-6);
  // No overlaps, no gaps at the corners: the ring's volume, less the door through it.
  const double ringArea = (5460 + 2 * out) * (3640 + 2 * out) - (5460 - 2 * in) * (3640 - 2 * in);
  EXPECT_NEAR(volumeOf(a, walls), (ringArea * 2800 - 800.0 * 2000 * (out + in)), 1.0);
  // The door goes through the new thickness and keeps its place along the wall.
  const json door = a.ok("list_openings", {{"ids", {southWithDoor}}})["hosts"][0]["openings"][0];
  EXPECT_NEAR(door["depth"].get<double>(), out + in, 1e-6);
  EXPECT_NEAR(door["width"].get<double>(), 800, 1e-6);
  EXPECT_NEAR(door["center"][0].get<double>(), 1200, 1e-6);
  const auto& s = *a.modeler.document().find(southWithDoor);
  EXPECT_EQ(s.attributes.at("wall_type"), "exterior_siding");
  EXPECT_EQ(s.attributes.at("wall_outside"), "right");  // the ring was drawn anticlockwise

  // The partition (not changed) now stops at the ring's inner faces.
  Bnd_Box p = a.bounds(part);
  EXPECT_NEAR(p.CornerMin().Y(), in, 1e-6);
  EXPECT_NEAR(p.CornerMax().Y(), 3640 - in, 1e-6);
  EXPECT_NEAR(p.CornerMax().X() - p.CornerMin().X(), 120, 1e-6);
  // Then the partition itself: board both sides around its line.
  a.ok("set_wall_type", {{"ids", part}, {"type", "interior_board"}});
  p = a.bounds(part);
  EXPECT_NEAR(p.CornerMin().X(), 2730 - 65, 1e-6);
  EXPECT_NEAR(p.CornerMax().X(), 2730 + 65, 1e-6);
  EXPECT_NEAR(p.CornerMin().Y(), in, 1e-6);

  // Each change is one undo step.
  a.ok("undo");
  a.ok("undo");
  const Bnd_Box back = a.bounds(walls);
  EXPECT_NEAR(back.CornerMin().X(), -100, 1e-6);
  EXPECT_EQ(a.modeler.document().find(southWithDoor)->attributes.count("wall_type"), 0u);
}

TEST(WallTypes, OutsideSideAndRefusals) {
  Api a;
  const json one = a.ok("create_wall", {{"points", {{0, 0}, {3640, 0}}}, {"thickness", 150}, {"height", 2400},
                                        {"justification", "left"}})["ids"];
  // Drawn with the wall left of the line: the line is its right face.
  a.ok("set_wall_type", {{"ids", one}, {"type", "exterior_metal"}, {"outside", "right"}, {"core", 120}});
  const Bnd_Box b = a.bounds(one);
  EXPECT_NEAR(b.CornerMin().Y(), -(15 + 18 + 9 + 60), 1e-6);
  EXPECT_NEAR(b.CornerMax().Y(), 60 + 12.5, 1e-6);
  // "auto" keeps a typed wall's outside.
  a.ok("set_wall_type", {{"ids", one}, {"type", "exterior_siding"}});
  EXPECT_NEAR(a.bounds(one).CornerMin().Y(), -95.5, 1e-6);
  // "flip" turns it round, keeping the line.
  a.ok("set_wall_type", {{"ids", one}, {"type", "exterior_siding"}, {"outside", "flip"}});
  EXPECT_NEAR(a.bounds(one).CornerMax().Y(), 95.5, 1e-6);
  EXPECT_NEAR(a.bounds(one).CornerMin().Y(), -65, 1e-6);
  const std::string box = a.ok("create_box", {{"origin", {0, 5000, 0}}, {"width", 100}, {"depth", 100}, {"height", 100}})["id"];
  EXPECT_FALSE(a.call("set_wall_type", {{"ids", {box}}, {"type", "interior_board"}})["ok"].get<bool>());
  EXPECT_FALSE(a.call("set_wall_type", {{"ids", one}, {"type", "brick"}})["ok"].get<bool>());
  EXPECT_FALSE(a.call("set_wall_type", {{"ids", one}, {"type", "interior_board"}, {"outside", "up"}})["ok"].get<bool>());
}
