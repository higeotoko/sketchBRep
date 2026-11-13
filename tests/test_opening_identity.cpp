// Openings as things of their own (docs/90 B-01, G-01): each keeps an id when others are
// removed, its fitting stays with it, it can be moved within its wall and removed, and the
// wall stays the same object throughout.
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <set>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Measure.h"

#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

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
  bool refused(const std::string& command, json args) { return !call(command, args).value("ok", true); }
  json openings(const std::string& host) {
    const json hosts = ok("list_openings", {{"ids", {host}}})["hosts"];
    return hosts.empty() ? json::array() : hosts[0]["openings"];
  }
  double volume(const std::string& id) { return sbr::geom::volume(modeler.document().find(id)->shape); }
};

const double kWall = 6000.0 * 200.0 * 3000.0;
const double kHole = 800.0 * 1200.0 * 200.0;

// A 6 m wall 200 thick along X with three 800 x 1200 windows (sill 900) cut from the y = 0
// face: evenly spread, so their left edges are 900, 2600 and 4300 from the wall's left end.
std::string wallWithThreeWindows(Api& a) {
  const std::string wall =
      a.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 6000}, {"depth", 200}, {"height", 3000}})["id"];
  const std::string after = a.ok("place_openings", {{"host", wall}, {"face_normal", {0, -1, 0}}, {"count", 3},
                                                    {"width", 800}, {"height", 1200}, {"sill", 900}})["id"];
  EXPECT_EQ(after, wall);  // the wall stays the same object
  return after;
}
bool numeric(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}
}  // namespace

TEST(OpeningIdentity, OpeningsHaveIdsAndTheWallKeepsItsOwn) {
  Api a;
  const std::string wall = wallWithThreeWindows(a);
  const json list = a.openings(wall);
  ASSERT_EQ(list.size(), 3u);
  std::set<std::string> ids;
  for (const auto& o : list) {
    const std::string id = o["id"];
    EXPECT_FALSE(numeric(id)) << id;
    ids.insert(id);
    EXPECT_NEAR(o["sill_height"].get<double>(), 900, 1e-6);
  }
  EXPECT_EQ(ids.size(), 3u);
  EXPECT_NEAR(list[0]["offset"].get<double>(), 900, 1e-6);
  EXPECT_NEAR(list[1]["offset"].get<double>(), 2600, 1e-6);
  EXPECT_NEAR(list[2]["offset"].get<double>(), 4300, 1e-6);
  EXPECT_NEAR(a.volume(wall), kWall - 3 * kHole, 1);
  // A second cut keeps the wall and the ids of the openings already there.
  const std::string again = a.ok("place_openings", {{"host", wall}, {"face_normal", {0, -1, 0}},
                                                    {"at", {300, 0, 0}}, {"width", 400}, {"height", 400},
                                                    {"sill", 100}})["id"];
  EXPECT_EQ(again, wall);
  const json more = a.openings(wall);
  ASSERT_EQ(more.size(), 4u);
  for (std::size_t i = 0; i < 3; ++i) EXPECT_EQ(more[i]["id"], list[i]["id"]);
  EXPECT_FALSE(ids.count(more[3]["id"].get<std::string>()));
}

TEST(OpeningIdentity, RemovingAnOpeningLeavesTheOthersWithTheirFittings) {
  Api a;
  const std::string wall = wallWithThreeWindows(a);
  const json before = a.openings(wall);
  const std::string first = before[0]["id"], middle = before[1]["id"], last = before[2]["id"];
  a.ok("set_opening_fittings", {{"host", wall}, {"openings", {first}},
                                {"fitting", {{"type", "swing_single"}, {"hinge", "left"}, {"swing", "in"}}}});
  a.ok("set_opening_fittings", {{"host", wall}, {"openings", {last}}, {"fitting", {{"type", "window"}}}});

  EXPECT_EQ(a.ok("delete_openings", {{"host", wall}, {"openings", {middle}}})["remaining"], 2);
  const json after = a.openings(wall);
  ASSERT_EQ(after.size(), 2u);
  EXPECT_EQ(after[0]["id"], first);
  EXPECT_EQ(after[1]["id"], last);  // now number 2, still itself
  EXPECT_EQ(after[1]["index"], 2);
  EXPECT_EQ(after[0]["fitting"]["type"], "swing_single");
  EXPECT_EQ(after[1]["fitting"]["type"], "window");  // did not slide onto another opening
  EXPECT_NEAR(after[1]["offset"].get<double>(), 4300, 1e-6);
  EXPECT_NEAR(a.volume(wall), kWall - 2 * kHole, 1);  // the wall is whole again there
  // The removed id is gone; an unknown id or number changes nothing.
  EXPECT_TRUE(a.refused("delete_openings", {{"host", wall}, {"openings", {middle}}}));
  EXPECT_TRUE(a.refused("delete_openings", {{"host", wall}, {"openings", {7}}}));
  EXPECT_EQ(a.openings(wall).size(), 2u);

  // One undo brings the opening back, with every id and fitting as before.
  ASSERT_TRUE(a.modeler.undo());
  const json restored = a.openings(wall);
  ASSERT_EQ(restored.size(), 3u);
  EXPECT_EQ(restored[1]["id"], middle);
  EXPECT_FALSE(restored[1].contains("fitting"));
  EXPECT_EQ(restored[2]["fitting"]["type"], "window");
  EXPECT_NEAR(a.volume(wall), kWall - 3 * kHole, 1);
}

TEST(OpeningIdentity, RemovingEveryOpeningGivesThePlainWallBack) {
  Api a;
  const std::string wall = wallWithThreeWindows(a);
  a.ok("move", {{"ids", {wall}}, {"vector", {1000, 2000, 0}}});  // moved after the cut
  a.ok("delete_openings", {{"host", wall}, {"openings", {1, 2, 3}}});
  EXPECT_TRUE(a.openings(wall).empty());
  EXPECT_NEAR(a.volume(wall), kWall, 1);
  const auto box = a.modeler.document().worldBounds(*a.modeler.document().find(wall));
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  EXPECT_NEAR(x0 + box.GetGap(), 1000, 1e-3);  // stays where it was moved to
  EXPECT_NEAR(y0 + box.GetGap(), 2000, 1e-3);
  // It is an ordinary wall again: openings can be cut anew.
  a.ok("place_openings", {{"host", wall}, {"face_normal", {0, -1, 0}}, {"count", 1}, {"width", 800},
                          {"height", 1200}, {"sill", 900}});
  EXPECT_EQ(a.openings(wall).size(), 1u);
}

TEST(OpeningIdentity, MovingAnOpeningKeepsItsIdSizeAndFitting) {
  Api a;
  const std::string wall = wallWithThreeWindows(a);
  const std::string middle = a.openings(wall)[1]["id"];
  a.ok("set_opening_fittings", {{"host", wall}, {"openings", {middle}}, {"fitting", {{"type", "window"}}}});

  // 300 along the wall; the part through the wall (y) is dropped.
  json moved = a.ok("move_opening", {{"host", wall}, {"opening", middle}, {"by", {300, 50, 0}}})["opening"];
  EXPECT_EQ(moved["id"], middle);
  EXPECT_NEAR(moved["offset"].get<double>(), 2900, 1e-6);
  EXPECT_NEAR(moved["sill_height"].get<double>(), 900, 1e-6);
  json o = a.openings(wall)[1];
  EXPECT_NEAR(o["width"].get<double>(), 800, 1e-9);
  EXPECT_NEAR(o["height"].get<double>(), 1200, 1e-9);
  EXPECT_EQ(o["fitting"]["type"], "window");
  EXPECT_NEAR(o["center"][0].get<double>(), 3300, 1e-6);
  EXPECT_NEAR(a.volume(wall), kWall - 3 * kHole, 1);

  // To a position: by number as well as by id.
  moved = a.ok("move_opening", {{"host", wall}, {"opening", 2}, {"offset", 1800}, {"sill_height", 300}})["opening"];
  EXPECT_NEAR(moved["offset"].get<double>(), 1800, 1e-6);
  EXPECT_NEAR(moved["sill_height"].get<double>(), 300, 1e-6);
  EXPECT_EQ(a.openings(wall)[1]["id"], middle);

  // Refused, and nothing changes: onto another opening, out of the wall, no move at all.
  const std::string saved = sbr::doc::serializeDocument(a.modeler.document());
  EXPECT_TRUE(a.refused("move_opening", {{"host", wall}, {"opening", middle}, {"offset", 1000}}));   // over the first
  EXPECT_TRUE(a.refused("move_opening", {{"host", wall}, {"opening", middle}, {"by", {-2500, 0, 0}}}));  // past the end
  EXPECT_TRUE(a.refused("move_opening", {{"host", wall}, {"opening", middle}, {"sill_height", 2000}}));  // above the top
  EXPECT_TRUE(a.refused("move_opening", {{"host", wall}, {"opening", middle}, {"by", {0, 80, 0}}}));  // only through
  EXPECT_TRUE(a.refused("move_opening", {{"host", wall}, {"opening", middle}}));
  EXPECT_TRUE(a.refused("move_opening", {{"host", wall}, {"opening", "no-such-opening"}, {"by", {10, 0, 0}}}));
  EXPECT_EQ(sbr::doc::serializeDocument(a.modeler.document()), saved);

  // Each move is one undo step.
  ASSERT_TRUE(a.modeler.undo());
  EXPECT_NEAR(a.openings(wall)[1]["offset"].get<double>(), 2900, 1e-6);
  ASSERT_TRUE(a.modeler.undo());
  EXPECT_NEAR(a.openings(wall)[1]["offset"].get<double>(), 2600, 1e-6);
}

TEST(OpeningIdentity, OpeningsFromBeforeIdsGetThemWhenEdited) {
  // Pushed through by hand (nested Subtracts, no ids): a door then a window.
  Api a;
  std::string wall =
      a.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 6000}, {"depth", 200}, {"height", 3000}})["id"];
  for (const auto& [x, z, w, h] :
       {std::array<double, 4>{1000, 0, 900, 2100}, std::array<double, 4>{3500, 900, 1600, 1200}}) {
    const std::string profile = a.ok("create_rectangle", {{"origin", {x, 0, z}}, {"normal", {0, -1, 0}},
                                                          {"width", w}, {"height", h}})["id"];
    wall = a.ok("push_pull", {{"id", profile}, {"face", 1}, {"distance", -200}})["id"];
  }
  json list = a.openings(wall);
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(list[0]["id"], "1");  // their numbers, until the wall's openings are edited
  EXPECT_EQ(list[1]["id"], "2");
  // An old file's fitting, stored under the number.
  a.ok("set_attribute", {{"ids", {wall}}, {"key", "opening_fittings"}, {"value", R"({"2":{"type":"window"}})"}});
  EXPECT_EQ(a.openings(wall)[1]["fitting"]["type"], "window");

  // Removing the door: the window keeps its fitting although it is number 1 now.
  a.ok("delete_openings", {{"host", wall}, {"openings", {1}}});
  list = a.openings(wall);
  ASSERT_EQ(list.size(), 1u);
  EXPECT_FALSE(numeric(list[0]["id"].get<std::string>()));
  EXPECT_EQ(list[0]["fitting"]["type"], "window");
  EXPECT_NEAR(list[0]["width"].get<double>(), 1600, 1e-9);
  EXPECT_NEAR(a.volume(wall), kWall - 1600.0 * 1200.0 * 200.0, 1);
}

TEST(OpeningIdentity, IdsAndFittingsSurviveSaveAndLoad) {
  Api a;
  const std::string wall = wallWithThreeWindows(a);
  const std::string last = a.openings(wall)[2]["id"];
  a.ok("set_opening_fittings", {{"host", wall}, {"openings", {last}}, {"fitting", {{"type", "sliding_double"}}}});
  const std::string text = sbr::doc::serializeDocument(a.modeler.document());

  const auto path = std::filesystem::temp_directory_path() / "sbr_opening_identity_test.sbr";
  { std::ofstream(path, std::ios::binary) << text; }
  Api b;
  ASSERT_TRUE(b.modeler.load(path.string()));
  std::filesystem::remove(path);
  const json list = b.openings(wall);
  ASSERT_EQ(list.size(), 3u);
  EXPECT_EQ(list[2]["id"], last);
  EXPECT_EQ(list[2]["fitting"]["type"], "sliding_double");
  b.ok("delete_openings", {{"host", wall}, {"openings", {list[0]["id"]}}});
  EXPECT_EQ(b.openings(wall)[1]["fitting"]["type"], "sliding_double");
}

TEST(OpeningIdentity, DeletingTheFacesInsideAnOpeningRemovesTheOpening) {
  Api a;
  const std::string wall = wallWithThreeWindows(a);
  const json before = a.openings(wall);
  a.ok("set_opening_fittings", {{"host", wall}, {"openings", {before[2]["id"]}}, {"fitting", {{"type", "window"}}}});
  // The faces of the wall: those inside the middle window (x 2600..3400, z 900..2100).
  const auto& m = a.modeler;
  const sbr::doc::Object& object = *m.document().find(wall);
  const TopoDS_Shape world = m.document().worldShape(object);
  std::vector<TopoDS_Face> inside, outside;
  for (TopExp_Explorer e(world, TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face f = TopoDS::Face(e.Current());
    const auto index = m.openingOfFace(wall, f);
    if (index && *index == 2) inside.push_back(f);
    if (!index) outside.push_back(f);
  }
  EXPECT_EQ(inside.size(), 4u);   // two jambs, head and sill
  EXPECT_EQ(outside.size(), 6u);  // the wall's own six faces belong to no opening

  // One of them selected with a wall face: the ordinary delete (not an opening).
  // One jamb on its own: the whole opening goes, and only that one.
  sbr::doc::DeleteTarget target;
  target.id = wall;
  target.faces = {inside.front()};
  const auto outcome = a.modeler.deleteSelection({target});
  ASSERT_TRUE(outcome) << outcome.error();
  EXPECT_EQ(outcome->openings, 1);
  EXPECT_TRUE(outcome->removed.empty());
  const json after = a.openings(wall);
  ASSERT_EQ(after.size(), 2u);
  EXPECT_EQ(after[0]["id"], before[0]["id"]);
  EXPECT_EQ(after[1]["id"], before[2]["id"]);
  EXPECT_EQ(after[1]["fitting"]["type"], "window");
  EXPECT_NEAR(a.volume(wall), kWall - 2 * kHole, 1);
  ASSERT_TRUE(a.modeler.undo());  // one step
  EXPECT_EQ(a.openings(wall).size(), 3u);
}

TEST(OpeningIdentity, AWallWithOpeningsKeepsItsOwnSizesEditable) {
  // A wall made by the wall tool, 6000 long, 200 thick, 3000 high, with a window 800 x 1200
  // (sill 900) in the middle: its left edge 2600 from the wall's start.
  Api a;
  const std::string wall = a.ok("create_wall", {{"points", {{0, 0}, {6000, 0}}}, {"thickness", 200},
                                                {"height", 3000}})["ids"][0];
  a.ok("place_openings", {{"host", wall}, {"face_normal", {0, -1, 0}}, {"count", 1}, {"width", 800},
                          {"height", 1200}, {"sill", 900}});
  auto field = [&](const std::string& key) {
    const json info = a.ok("member_info", {{"id", wall}});
    for (const auto& f : info["fields"])
      if (f["key"] == key) return f;
    ADD_FAILURE() << "no field " << key;
    return json();
  };
  EXPECT_NEAR(field("length")["value"].get<double>(), 6000, 1e-9);
  EXPECT_NEAR(field("height")["value"].get<double>(), 3000, 1e-9);
  EXPECT_NEAR(field("thickness")["value"].get<double>(), 200, 1e-9);
  EXPECT_TRUE(field("height")["editable"].get<bool>());
  EXPECT_FALSE(field("thickness")["editable"].get<bool>());

  // Higher and shorter: the window stays where it was, with its id.
  const std::string id = a.openings(wall)[0]["id"];
  a.ok("change_member", {{"id", wall}, {"height", 3300}, {"length", 5000}});
  EXPECT_NEAR(a.volume(wall), 5000.0 * 200 * 3300 - 800.0 * 1200 * 200, 1);
  const json o = a.openings(wall)[0];
  EXPECT_EQ(o["id"], id);
  EXPECT_NEAR(o["offset"].get<double>(), 2600, 1e-6);
  EXPECT_NEAR(o["sill_height"].get<double>(), 900, 1e-6);
  ASSERT_TRUE(a.modeler.undo());  // one step for both
  EXPECT_NEAR(a.volume(wall), 6000.0 * 200 * 3000 - 800.0 * 1200 * 200, 1);

  // Refused, nothing changed: the window would stick out, or the thickness (the build-up's job).
  const std::string saved = sbr::doc::serializeDocument(a.modeler.document());
  EXPECT_TRUE(a.refused("change_member", {{"id", wall}, {"length", 3000}}));  // ends inside the window
  EXPECT_TRUE(a.refused("change_member", {{"id", wall}, {"height", 2000}}));  // lower than its head
  EXPECT_TRUE(a.refused("change_member", {{"id", wall}, {"thickness", 150}}));
  EXPECT_EQ(sbr::doc::serializeDocument(a.modeler.document()), saved);
  // Exactly up to the window's edges is allowed.
  a.ok("change_member", {{"id", wall}, {"length", 3400}, {"height", 2100}});
  EXPECT_EQ(a.openings(wall).size(), 1u);
}
