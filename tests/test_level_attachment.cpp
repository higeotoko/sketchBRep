// Walls and slabs on levels (docs/90 B-02, G-02): made at a level's height they stand on it;
// when a storey height changes they follow, openings keep their sill heights, and what was
// drawn by other means never moves.
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
struct LevelAttachment : testing::Test {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  std::string l1, l2, lr;  // level ids: 1FL 0, 2FL 3000, RFL 6000
  json call(const std::string& command, json args = json::object()) {
    return json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
  }
  json ok(const std::string& command, json args = json::object()) {
    json r = call(command, args);
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  }
  bool refused(const std::string& command, json args) { return !call(command, args).value("ok", true); }
  void SetUp() override {
    ASSERT_TRUE(modeler.setLevels({{"", "1FL", 0}, {"", "2FL", 3000}, {"", "RFL", 6000}}));
    for (const auto& l : modeler.levels()) (l.name == "1FL" ? l1 : l.name == "2FL" ? l2 : lr) = l.id;
  }
  void heights(double first, double second, double roof) {
    ASSERT_TRUE(modeler.setLevels({{l1, "1FL", first}, {l2, "2FL", second}, {lr, "RFL", roof}}));
  }
  std::string attr(const std::string& id, const std::string& key) {
    const auto& a = modeler.document().find(id)->attributes;
    const auto it = a.find(key);
    return it == a.end() ? std::string() : it->second;
  }
  std::pair<double, double> z(const std::string& id) {
    const auto box = sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(id)));
    return {box.CornerMin().Z(), box.CornerMax().Z()};
  }
  std::string wall(double base, double height, double y = 0) {
    return ok("create_wall", {{"points", {{0, y}, {6000, y}}}, {"thickness", 200}, {"height", height},
                              {"base", base}})["ids"][0];
  }
  std::string slab(double top) {
    return ok("create_slab", {{"outline", {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}}}, {"top", top},
                              {"thickness", 200}})["id"];
  }
};
#define EXPECT_Z(id, lo, hi)                 \
  do {                                       \
    const auto range = z(id);                \
    EXPECT_NEAR(range.first, lo, 1e-6);      \
    EXPECT_NEAR(range.second, hi, 1e-6);     \
  } while (false)
}  // namespace

TEST_F(LevelAttachment, WallsAndSlabsMadeAtALevelStandOnIt) {
  const std::string storey = wall(0, 3000);     // 1FL up to 2FL
  const std::string upper = wall(3000, 2700);   // on 2FL, its top at no level
  const std::string loose = wall(500, 1000);    // at no level
  const std::string floor = slab(3000);
  const std::string low = slab(1234);
  EXPECT_EQ(attr(storey, "base_level"), l1);
  EXPECT_EQ(attr(storey, "top_level"), l2);
  EXPECT_EQ(attr(upper, "base_level"), l2);
  EXPECT_EQ(attr(upper, "top_level"), "");
  EXPECT_EQ(attr(loose, "base_level"), "");
  EXPECT_EQ(attr(loose, "top_level"), "");
  EXPECT_EQ(attr(floor, "top_level"), l2);
  EXPECT_EQ(attr(low, "top_level"), "");
  // A shape drawn another way is on no level, even at a level's height.
  const std::string box =
      ok("create_box", {{"origin", {0, 5000, 0}}, {"width", 1000}, {"depth", 200}, {"height", 3000}})["id"];
  EXPECT_EQ(attr(box, "base_level"), "");
}

TEST_F(LevelAttachment, RaisingAStoreyTakesWallsSlabsAndOpeningsAlong) {
  const std::string storey = wall(0, 3000);
  const std::string upper = wall(3000, 2700);
  const std::string loose = wall(500, 1000, 2000);
  const std::string floor = slab(3000);
  const std::string box =
      ok("create_box", {{"origin", {0, 5000, 0}}, {"width", 1000}, {"depth", 200}, {"height", 3000}})["id"];
  ok("place_openings", {{"host", storey}, {"face_normal", {0, -1, 0}}, {"count", 1}, {"width", 1600},
                        {"height", 1200}, {"sill", 900}});
  const double volume = sbr::geom::volume(modeler.document().find(storey)->shape);

  heights(0, 3100, 6000);  // 2FL up by 100
  EXPECT_Z(storey, 0, 3100);     // its top went up with 2FL
  EXPECT_Z(upper, 3100, 5800);   // the wall on 2FL went up whole, keeping its height
  EXPECT_Z(floor, 2900, 3100);   // the floor too, keeping its thickness
  EXPECT_Z(loose, 500, 1500);    // on no level: stays
  EXPECT_Z(box, 0, 3000);        // not made by the wall tool: stays
  const json opening = ok("list_openings", {{"ids", {storey}}})["hosts"][0]["openings"][0];
  EXPECT_NEAR(opening["sill_height"].get<double>(), 900, 1e-6);  // the window stays where it was
  EXPECT_NEAR(opening["sill"].get<double>(), 900, 1e-6);
  EXPECT_NEAR(opening["height"].get<double>(), 1200, 1e-9);
  EXPECT_NEAR(sbr::geom::volume(modeler.document().find(storey)->shape), volume + 6000.0 * 200 * 100, 1);

  // One undo step brings the level and everything on it back.
  ASSERT_TRUE(modeler.undo());
  EXPECT_Z(storey, 0, 3000);
  EXPECT_Z(upper, 3000, 5700);
  EXPECT_Z(floor, 2800, 3000);
  EXPECT_NEAR(modeler.levels()[1].elevation, 3000, 1e-9);

  // The ground floor up by 200: the wall's foot follows, its top stays on 2FL; the window goes
  // up with the foot (same sill height).
  heights(200, 3000, 6000);
  EXPECT_Z(storey, 200, 3000);
  const json lifted = ok("list_openings", {{"ids", {storey}}})["hosts"][0]["openings"][0];
  EXPECT_NEAR(lifted["sill_height"].get<double>(), 900, 1e-6);
  EXPECT_NEAR(lifted["sill"].get<double>(), 1100, 1e-6);
  EXPECT_Z(upper, 3000, 5700);
}

TEST_F(LevelAttachment, AttachingByHandMovesTheEndsNowAndKeepsThemThere) {
  const std::string w = wall(3000, 2700);  // on 2FL, free top at 5700
  // Top to the roof level: the height changes, the foot stays.
  json r = ok("attach_to_levels", {{"ids", {w}}, {"top", "RFL"}})["objects"][0];
  EXPECT_EQ(r["base_level"], l2);
  EXPECT_EQ(r["top_level"], lr);
  EXPECT_Z(w, 3000, 6000);
  heights(0, 3000, 6500);
  EXPECT_Z(w, 3000, 6500);
  // Foot to 1FL: the top is on RFL, so it stays and the wall grows downwards.
  ok("attach_to_levels", {{"ids", {w}}, {"base", l1}});
  EXPECT_Z(w, 0, 6500);
  // Letting the top go: the wall keeps its height and moves whole with its foot.
  r = ok("attach_to_levels", {{"ids", {w}}, {"top", nullptr}})["objects"][0];
  EXPECT_TRUE(r["top_level"].is_null());
  heights(300, 3000, 6500);
  EXPECT_Z(w, 300, 6800);
  // Letting the foot go too: nothing follows any more.
  ok("attach_to_levels", {{"ids", {w}}, {"base", nullptr}});
  heights(0, 3000, 6000);
  EXPECT_Z(w, 300, 6800);

  // A slab's top goes onto a level; a slab has no foot to attach.
  const std::string s = slab(1234);
  ok("attach_to_levels", {{"ids", {s}}, {"top", "2FL"}});
  EXPECT_Z(s, 2800, 3000);
  EXPECT_TRUE(refused("attach_to_levels", {{"ids", {s}}, {"base", "1FL"}}));
}

TEST_F(LevelAttachment, RefusalsChangeNothing) {
  const std::string w = wall(3000, 2700);
  const std::string box =
      ok("create_box", {{"origin", {0, 5000, 0}}, {"width", 1000}, {"depth", 200}, {"height", 3000}})["id"];
  const std::string saved = sbr::doc::serializeDocument(modeler.document());
  EXPECT_TRUE(refused("attach_to_levels", {{"ids", {w}}, {"top", "1FL"}}));          // below its foot
  EXPECT_TRUE(refused("attach_to_levels", {{"ids", {w}}, {"top", "no such level"}}));
  EXPECT_TRUE(refused("attach_to_levels", {{"ids", {box}}, {"base", "1FL"}}));       // not a wall
  EXPECT_TRUE(refused("attach_to_levels", {{"ids", {w, box}}, {"top", "RFL"}}));     // all or nothing
  EXPECT_TRUE(refused("attach_to_levels", {{"ids", {w}}}));
  EXPECT_EQ(sbr::doc::serializeDocument(modeler.document()), saved);
}

TEST_F(LevelAttachment, ALevelTakenAwayOrAWallThatCannotFollowIsLeftInPlace) {
  const std::string storey = wall(0, 3000);
  const std::string upper = wall(3000, 2700);
  // 2FL taken away: what stood on it stays where it is.
  ASSERT_TRUE(modeler.setLevels({{l1, "1FL", 0}, {lr, "RFL", 6000}}));
  EXPECT_Z(storey, 0, 3000);
  EXPECT_Z(upper, 3000, 5700);
  ASSERT_TRUE(modeler.undo());
  // 2FL brought down below the ground floor: the storey wall cannot have no height; it stays
  // as it is rather than being broken, and the level change itself still happens.
  heights(0, -500, 6000);
  EXPECT_Z(storey, 0, 3000);
  EXPECT_Z(upper, -500, 2200);
  // A locked wall does not move.
  ASSERT_TRUE(modeler.undo());
  ASSERT_TRUE(modeler.setLocked({upper}, true));
  heights(0, 3200, 6000);
  EXPECT_Z(upper, 3000, 5700);
  EXPECT_Z(storey, 0, 3200);
}

TEST_F(LevelAttachment, SurvivesSaveLoadAndAMovedWallKeepsItsOffset) {
  const std::string storey = wall(0, 3000);
  // Moved by hand 50 up: it keeps that offset from its levels afterwards.
  ok("move", {{"ids", {storey}}, {"vector", {0, 0, 50}}});
  EXPECT_Z(storey, 50, 3050);
  const std::string text = sbr::doc::serializeDocument(modeler.document());
  sbr::doc::Document loaded;
  ASSERT_TRUE(sbr::doc::deserializeDocument(text, loaded));
  EXPECT_EQ(loaded.find(storey)->attributes.at("base_level"), l1);
  EXPECT_EQ(loaded.find(storey)->attributes.at("top_level"), l2);
  heights(0, 3300, 6000);
  EXPECT_Z(storey, 50, 3350);
}
