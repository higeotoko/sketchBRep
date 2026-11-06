// Door / window fittings of wall openings and their plan symbols (OpeningSymbols.h): drawn to
// the opening's real width and the wall's thickness, in the plan cut and on plan drawings.
#include <cmath>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <TopExp.hxx>
#include <TopoDS_Vertex.hxx>

#include "sbr/OpeningSymbols.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/draw/Drawing.h"

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
};

// A 6 m wall 200 thick along X (y 0..200) with a door 900 x 2100 and a window 1600 x 1200
// (sill 900), both cut from the y = 0 face.
std::string wallWithDoorAndWindow(Api& a) {
  std::string host =
      a.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 6000}, {"depth", 200}, {"height", 3000}})["id"];
  for (const auto& [x, z, w, h] : {std::array<double, 4>{1000, 0, 900, 2100}, std::array<double, 4>{3500, 900, 1600, 1200}}) {
    const std::string profile = a.ok("create_rectangle", {{"origin", {x, 0, z}}, {"normal", {0, -1, 0}},
                                                          {"width", w}, {"height", h}})["id"];
    host = a.ok("push_pull", {{"id", profile}, {"face", 1}, {"distance", -200}})["id"];
  }
  return host;
}

double length(const TopoDS_Edge& e) {
  BRepAdaptor_Curve c(e);
  return GCPnts_AbscissaPoint::Length(c);
}
gp_Pnt first(const TopoDS_Edge& e) { return BRep_Tool::Pnt(TopExp::FirstVertex(e)); }
gp_Pnt last(const TopoDS_Edge& e) { return BRep_Tool::Pnt(TopExp::LastVertex(e)); }
bool isLine(const TopoDS_Edge& e) { return BRepAdaptor_Curve(e).GetType() == GeomAbs_Line; }
}  // namespace

TEST(OpeningSymbols, FittingsAreCheckedStoredAndUndone) {
  Api a;
  const std::string host = wallWithDoorAndWindow(a);
  const json openings = a.ok("list_openings", {{"ids", {host}}})["hosts"][0]["openings"];
  ASSERT_EQ(openings.size(), 2u);
  EXPECT_NEAR(openings[0]["depth"].get<double>(), 200, 1e-9);  // the wall's thickness
  EXPECT_FALSE(openings[0].contains("fitting"));

  // Wrong kinds, sides and indices are refused and change nothing.
  EXPECT_FALSE(a.call("set_opening_fittings", {{"host", host}, {"fitting", {{"type", "revolving"}}}})["ok"].get<bool>());
  EXPECT_FALSE(a.call("set_opening_fittings",
                      {{"host", host}, {"fitting", {{"type", "swing_single"}, {"hinge", "up"}}}})["ok"].get<bool>());
  EXPECT_FALSE(a.call("set_opening_fittings",
                      {{"host", host}, {"openings", {3}}, {"fitting", {{"type", "window"}}}})["ok"].get<bool>());
  EXPECT_FALSE(a.call("set_opening_fittings",
                      {{"host", host}, {"fitting", {{"type", "window"}, {"colour", "red"}}}})["ok"].get<bool>());
  EXPECT_EQ(a.modeler.document().find(host)->attributes.count("opening_fittings"), 0u);

  a.ok("set_opening_fittings", {{"host", host}, {"openings", {1}}, {"fitting", {{"type", "swing_single"}}}});
  a.ok("set_opening_fittings", {{"host", host}, {"openings", {2}}, {"fitting", {{"type", "sliding_double"}}}});
  json listed = a.ok("list_openings", {{"ids", {host}}})["hosts"][0]["openings"];
  // Defaults are written out: left hinge, opening inwards.
  EXPECT_EQ(listed[0]["fitting"], (json{{"type", "swing_single"}, {"hinge", "left"}, {"swing", "in"}}));
  EXPECT_EQ(listed[1]["fitting"], (json{{"type", "sliding_double"}}));

  // "none" removes one; undo brings it back.
  a.ok("set_opening_fittings", {{"host", host}, {"openings", {2}}, {"fitting", {{"type", "none"}}}});
  EXPECT_FALSE(a.ok("list_openings", {{"ids", {host}}})["hosts"][0]["openings"][1].contains("fitting"));
  a.ok("undo");
  EXPECT_EQ(a.ok("list_openings", {{"ids", {host}}})["hosts"][0]["openings"][1]["fitting"],
            (json{{"type", "sliding_double"}}));
  EXPECT_EQ(a.api.effectOf(R"({"command":"set_opening_fittings","args":{}})"), CommandApi::Effect::ModelWrite);
}

TEST(OpeningSymbols, DoorAndWindowAreDrawnToTheRealWidthAndWallThickness) {
  Api a;
  const std::string host = wallWithDoorAndWindow(a);
  const json openings = a.ok("list_openings", {{"ids", {host}}})["hosts"][0]["openings"];
  const double doorX = openings[0]["center"][0], windowX = openings[1]["center"][0];
  a.ok("set_opening_fittings",
       {{"host", host}, {"openings", {1}}, {"fitting", {{"type", "swing_single"}, {"hinge", "left"}, {"swing", "in"}}}});
  a.ok("set_opening_fittings", {{"host", host}, {"openings", {2}}, {"fitting", {{"type", "window"}}}});

  // Cut at 1100: both openings. The door: leaf and quarter arc of radius 900 on the inner
  // face (y = 200, away from the cut side); seen from inside, its left hinge is at +X.
  sbr::draw::SymbolOptions at1100;
  at1100.z = 1100;
  const auto edges = sbr::draw::openingSymbols(a.modeler.document(), at1100);
  ASSERT_EQ(edges.size(), 4u);  // leaf + arc + two glass lines
  int leaves = 0, arcs = 0, glass = 0;
  for (const TopoDS_Edge& e : edges) {
    const gp_Pnt p = first(e), q = last(e);
    EXPECT_NEAR(p.Z(), 1100, 1e-6);
    if (!isLine(e)) {
      ++arcs;
      EXPECT_NEAR(length(e), std::acos(-1.0) / 2 * 900, 1e-3);
      continue;
    }
    if (std::abs(length(e) - 900) < 1e-6) {
      ++leaves;
      const gp_Pnt hinge = std::abs(p.Y() - 200) < 1e-6 ? p : q, open = std::abs(p.Y() - 200) < 1e-6 ? q : p;
      EXPECT_NEAR(hinge.X(), doorX + 450, 1e-6);
      EXPECT_NEAR(hinge.Y(), 200, 1e-6);
      EXPECT_NEAR(open.X(), doorX + 450, 1e-6);
      EXPECT_NEAR(open.Y(), 1100, 1e-6);
    } else {
      ++glass;
      EXPECT_NEAR(length(e), 1600, 1e-6);
      EXPECT_NEAR(std::abs(p.Y() - 100), 15, 1e-6);  // two glass lines around the wall's middle
      EXPECT_NEAR((p.X() + q.X()) / 2, windowX, 1e-6);
    }
  }
  EXPECT_EQ(leaves, 1);
  EXPECT_EQ(arcs, 1);
  EXPECT_EQ(glass, 2);

  // Hinge on the right: the leaf moves to the other jamb. Opening outwards: the cut side.
  a.ok("set_opening_fittings",
       {{"host", host}, {"openings", {1}}, {"fitting", {{"type", "swing_single"}, {"hinge", "right"}, {"swing", "out"}}}});
  for (const TopoDS_Edge& e : sbr::draw::openingSymbols(a.modeler.document(), at1100)) {
    if (!isLine(e) || std::abs(length(e) - 900) > 1e-6) continue;
    const gp_Pnt p = first(e), q = last(e);
    const gp_Pnt hinge = std::abs(p.Y()) < 1e-6 ? p : q, open = std::abs(p.Y()) < 1e-6 ? q : p;
    // Seen from outside (y < 0 looking +Y) the right is +X.
    EXPECT_NEAR(hinge.X(), doorX + 450, 1e-6);
    EXPECT_NEAR(hinge.Y(), 0, 1e-6);
    EXPECT_NEAR(open.Y(), -900, 1e-6);
  }

  // Cut at 2400: above both openings, nothing. At 500: the door only (below the window's sill).
  sbr::draw::SymbolOptions at2400;
  at2400.z = 2400;
  EXPECT_TRUE(sbr::draw::openingSymbols(a.modeler.document(), at2400).empty());
  sbr::draw::SymbolOptions at500;
  at500.z = 500;
  EXPECT_EQ(sbr::draw::openingSymbols(a.modeler.document(), at500).size(), 2u);
  // Hidden hosts draw nothing.
  a.ok("hide", {{"ids", {host}}});
  EXPECT_TRUE(sbr::draw::openingSymbols(a.modeler.document(), at1100).empty());
}

TEST(OpeningSymbols, SlidingDoorsAndThePlanDrawing) {
  Api a;
  const std::string host = wallWithDoorAndWindow(a);
  a.ok("set_opening_fittings", {{"host", host}, {"openings", {2}}, {"fitting", {{"type", "sliding_double"}}}});
  sbr::draw::SymbolOptions at1100;
  at1100.z = 1100;
  // 引違い: two leaves (4 lines each) on two tracks, each with an arrow (3 lines).
  const auto edges = sbr::draw::openingSymbols(a.modeler.document(), at1100);
  ASSERT_EQ(edges.size(), 14u);
  int leafSides = 0;
  for (const TopoDS_Edge& e : edges)
    if (std::abs(length(e) - (800 + 2 * 25)) < 1e-6) ++leafSides;  // half the width + overlaps
  EXPECT_EQ(leafSides, 4);
  for (const auto& type : {"sliding_single", "sliding_bi", "swing_double"}) {
    a.ok("set_opening_fittings", {{"host", host}, {"openings", {2}}, {"fitting", {{"type", type}}}});
    EXPECT_FALSE(sbr::draw::openingSymbols(a.modeler.document(), at1100).empty()) << type;
  }

  // The plan drawing (DXF / JWW / PDF) carries the door's arc at the real radius.
  a.ok("set_opening_fittings", {{"host", host}, {"openings", {1}}, {"fitting", {{"type", "swing_double"}}}});
  const auto view = sbr::draw::makeView(a.modeler.document(), sbr::draw::planView(1100));
  ASSERT_TRUE(view) << view.error();
  int arcs = 0;
  for (const auto& item : view->items)
    if (item.kind == sbr::draw::Item::Kind::Arc && std::abs(item.radius - 450) < 1e-3) ++arcs;
  EXPECT_EQ(arcs, 2);  // 両開き: two leaves of half the width
  // A plan above the openings has no symbols.
  const auto high = sbr::draw::makeView(a.modeler.document(), sbr::draw::planView(2600));
  ASSERT_TRUE(high);
  for (const auto& item : high->items) EXPECT_NE(item.kind, sbr::draw::Item::Kind::Arc);
}

TEST(OpeningSymbols, FittingsStayWhenMoreOpeningsAreMade) {
  Api a;
  std::string host = wallWithDoorAndWindow(a);
  a.ok("set_opening_fittings", {{"host", host}, {"openings", {1}}, {"fitting", {{"type", "sliding_bi"}}}});
  host = a.ok("place_openings", {{"host", host}, {"face_normal", {0, -1, 0}}, {"at", {5550, 0, 0}},
                                 {"width", 600}, {"height", 900}, {"sill", 1000}})["id"];
  const json listed = a.ok("list_openings", {{"ids", {host}}})["hosts"][0]["openings"];
  ASSERT_EQ(listed.size(), 3u);
  EXPECT_EQ(listed[0]["fitting"], (json{{"type", "sliding_bi"}}));
  EXPECT_FALSE(listed[2].contains("fitting"));
}

TEST(OpeningSymbols, PlacedWithAFittingInOneStep) {
  Api a;
  std::string host =
      a.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 6000}, {"depth", 200}, {"height", 3000}})["id"];
  const std::size_t steps = a.modeler.history().undoCount();
  host = a.ok("place_openings", {{"host", host}, {"face_normal", {0, -1, 0}}, {"count", 2}, {"width", 1600},
                                 {"height", 2000}, {"fitting", {{"type", "sliding_double"}}}})["id"];
  EXPECT_EQ(a.modeler.history().undoCount(), steps + 1);
  json listed = a.ok("list_openings", {{"ids", {host}}})["hosts"][0]["openings"];
  ASSERT_EQ(listed.size(), 2u);
  for (const json& o : listed) EXPECT_EQ(o["fitting"], (json{{"type", "sliding_double"}}));
  // A third one by position: only it gets the new kind.
  const std::string twoOpenings = host;
  host = a.ok("place_openings", {{"host", host}, {"face_normal", {0, -1, 0}}, {"at", {5500, 0, 0}}, {"width", 600},
                                 {"height", 900}, {"sill", 1100}, {"fitting", {{"type", "window"}}}})["id"];
  listed = a.ok("list_openings", {{"ids", {host}}})["hosts"][0]["openings"];
  ASSERT_EQ(listed.size(), 3u);
  EXPECT_EQ(listed[0]["fitting"], (json{{"type", "sliding_double"}}));
  EXPECT_EQ(listed[2]["fitting"], (json{{"type", "window"}}));
  // A wrong fitting cuts nothing.
  EXPECT_FALSE(a.call("place_openings", {{"host", host}, {"face_normal", {0, -1, 0}}, {"at", {5500, 0, 2000}},
                                         {"width", 300}, {"height", 300}, {"fitting", {{"type", "door"}}}})["ok"].get<bool>());
  EXPECT_EQ(a.ok("list_openings", {{"ids", {host}}})["hosts"][0]["openings"].size(), 3u);
  // Undo takes the opening and its fitting together.
  a.ok("undo");
  listed = a.ok("list_openings", {{"ids", {twoOpenings}}})["hosts"][0]["openings"];
  EXPECT_EQ(listed.size(), 2u);
  EXPECT_FALSE(listed[1].contains("fitting") && listed[1]["fitting"]["type"] == "window");
}
