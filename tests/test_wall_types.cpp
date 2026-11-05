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
