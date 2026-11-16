// Roofs as one solid (docs/90 G-08, docs/99). Sizes are checked against hand calculations:
// seen from above a roof covers its outline grown by the overhangs; its volume is that area
// times its thickness measured upright; its upper surface is that area times sqrt(1 + slope^2).
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <BRepCheck_Analyzer.hxx>
#include <TopExp_Explorer.hxx>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Roof.h"

using json = nlohmann::json;
using sbr::api::CommandApi;
using sbr::geom::makeRoof;
using sbr::geom::RoofSpec;

namespace {
const double kSlope = 0.4;                               // pitch 4
const double kRise = 100.0 * std::sqrt(1.0 + 0.4 * 0.4);  // a 100 roof measured upright

RoofSpec rect(const std::string& kind) {
  RoofSpec s;
  s.kind = kind;
  s.outline = {gp_Pnt(0, 0, 0), gp_Pnt(8000, 0, 0), gp_Pnt(8000, 6000, 0), gp_Pnt(0, 6000, 0)};
  s.height = 6000;
  return s;  // pitch 4, eave 600, gable eave 300, thickness 100
}

void expectBounds(const TopoDS_Shape& solid, double x0, double y0, double z0, double x1, double y1, double z1) {
  const auto b = sbr::geom::exactBounds(solid);
  EXPECT_NEAR(b.CornerMin().X(), x0, 1e-4);
  EXPECT_NEAR(b.CornerMin().Y(), y0, 1e-4);
  EXPECT_NEAR(b.CornerMin().Z(), z0, 1e-4);
  EXPECT_NEAR(b.CornerMax().X(), x1, 1e-4);
  EXPECT_NEAR(b.CornerMax().Y(), y1, 1e-4);
  EXPECT_NEAR(b.CornerMax().Z(), z1, 1e-4);
}

TEST(Roof, AGableOverARectangle) {
  const auto roof = makeRoof(rect("gable"));
  ASSERT_TRUE(roof) << roof.error();
  EXPECT_TRUE(BRepCheck_Analyzer(roof->solid).IsValid());
  EXPECT_EQ(roof->parts, 1);
  // Ridge along the long side (x): eaves 600 at y, gable overhang 300 at x.
  const double plan = 8600.0 * 7200.0;
  EXPECT_NEAR(roof->planArea, plan, 1.0);
  EXPECT_NEAR(sbr::geom::volume(roof->solid), plan * kRise, 100.0);
  EXPECT_NEAR(roof->slopeArea, plan * std::sqrt(1.16), 1.0);
  // Underside at the wall line 6000; eave edge 600 out is 240 lower; the ridge 3000 in is 1200 higher.
  expectBounds(roof->solid, -300, -600, 5760, 8300, 6600, 7200 + kRise);
  EXPECT_NEAR(roof->ridgeHeight, 7200 + kRise, 1e-4);
  EXPECT_NEAR(roof->eaveHeight, 5760, 1e-4);

  // The ridge along the short side instead.
  RoofSpec across = rect("gable");
  across.ridge = "short";
  const auto other = makeRoof(across);
  ASSERT_TRUE(other) << other.error();
  EXPECT_NEAR(other->planArea, 9200.0 * 6600.0, 1.0);
  expectBounds(other->solid, -600, -300, 5760, 8600, 6300, 6000 + kSlope * 4000 + kRise);
}

TEST(Roof, AHipOverARectangle) {
  const auto roof = makeRoof(rect("hip"));
  ASSERT_TRUE(roof) << roof.error();
  EXPECT_TRUE(BRepCheck_Analyzer(roof->solid).IsValid());
  const double plan = 9200.0 * 7200.0;  // eaves all round
  EXPECT_NEAR(roof->planArea, plan, 1.0);
  EXPECT_NEAR(sbr::geom::volume(roof->solid), plan * kRise, 100.0);
  expectBounds(roof->solid, -600, -600, 5760, 8600, 6600, 7200 + kRise);
  // Its upper surface: every face at the same slope.
  double top = 0.0;
  for (TopExp_Explorer e(roof->solid, TopAbs_FACE); e.More(); e.Next()) {
    const auto b = sbr::geom::exactBounds(e.Current());
    if (b.CornerMax().Z() > 7200 + kRise - 1) top += sbr::geom::area(e.Current());  // only the four upper faces reach the ridge
  }
  EXPECT_NEAR(top, plan * std::sqrt(1.16), 10.0);
}

TEST(Roof, AShedFallsTheWayAsked) {
  const auto south = makeRoof(rect("shed"));  // falls towards -y
  ASSERT_TRUE(south) << south.error();
  EXPECT_TRUE(BRepCheck_Analyzer(south->solid).IsValid());
  const double plan = 8600.0 * 7200.0;  // 600 at the low and high sides, 300 at the sloping sides
  EXPECT_NEAR(south->planArea, plan, 1.0);
  EXPECT_NEAR(sbr::geom::volume(south->solid), plan * kRise, 100.0);
  // Underside 6000 above the low wall (y = 0); 240 lower at the eave; 0.4 * 6600 higher at the far edge.
  expectBounds(south->solid, -300, -600, 5760, 8300, 6600, 6000 + kSlope * 6600 + kRise);

  RoofSpec east = rect("shed");
  east.fallX = 1;
  east.fallY = 0.2;  // nearer +x
  const auto e = makeRoof(east);
  ASSERT_TRUE(e) << e.error();
  // Low wall at x = 8000; the far edge at x = -600 is 0.4 * 8600 higher.
  expectBounds(e->solid, -600, -300, 5760, 8600, 6300, 6000 + kSlope * 8600 + kRise);
}

TEST(Roof, AnLShapeGetsValleys) {
  RoofSpec s = rect("hip");
  s.outline = {gp_Pnt(0, 0, 0), gp_Pnt(8000, 0, 0), gp_Pnt(8000, 4000, 0), gp_Pnt(4000, 4000, 0), gp_Pnt(4000, 7000, 0), gp_Pnt(0, 7000, 0)};
  const auto hip = makeRoof(s);
  ASSERT_TRUE(hip) << hip.error();
  EXPECT_TRUE(BRepCheck_Analyzer(hip->solid).IsValid());
  EXPECT_EQ(hip->parts, 2);
  // The outline grown 600 all round: area + perimeter x 600 + (5 outer corners - 1 inner) x 600^2.
  const double plan = 44e6 + 30000.0 * 600 + 4 * 600.0 * 600;
  EXPECT_NEAR(hip->planArea, plan, 1.0);
  EXPECT_NEAR(sbr::geom::volume(hip->solid), plan * kRise, 200.0);
  // Both wings are 4000 wide: ridges 2000 in, 800 above the walls.
  expectBounds(hip->solid, -600, -600, 5760, 8600, 7600, 6800 + kRise);

  s.kind = "gable";
  const auto gable = makeRoof(s);
  ASSERT_TRUE(gable) << gable.error();
  EXPECT_TRUE(BRepCheck_Analyzer(gable->solid).IsValid());
  // Wing along x: 8600 x 5200; wing along y: 5200 x 7600; they share 4900 x 4900.
  const double gablePlan = 8600.0 * 5200 + 5200.0 * 7600 - 4900.0 * 4900;
  EXPECT_NEAR(gable->planArea, gablePlan, 1.0);
  expectBounds(gable->solid, -600, -600, 5760, 8300, 7300, 6800 + kRise);
}

TEST(Roof, TurnedInPlanItIsTheSameRoof) {
  const auto straight = makeRoof(rect("hip"));
  RoofSpec turned = rect("hip");
  const double c = std::cos(0.5), s = std::sin(0.5);
  for (gp_Pnt& p : turned.outline) p = gp_Pnt(p.X() * c - p.Y() * s + 1234, p.X() * s + p.Y() * c - 567, 0);
  const auto roof = makeRoof(turned);
  ASSERT_TRUE(straight && roof);
  EXPECT_NEAR(sbr::geom::volume(roof->solid), sbr::geom::volume(straight->solid), 100.0);
  EXPECT_NEAR(roof->ridgeHeight, straight->ridgeHeight, 1e-4);
  EXPECT_NEAR(roof->eaveHeight, straight->eaveHeight, 1e-4);
}

// docs/138: gable and shed roofs over an outline with slanted sides, and a hip roof over one
// with an inward corner.
namespace {
// The outline's area grown by `d` at every side (corners mitred), mm2.
double grownArea(std::vector<gp_Pnt> p, double d) {
  double twice = 0.0;
  for (std::size_t i = 0; i < p.size(); ++i) twice += p[i].X() * p[(i + 1) % p.size()].Y() - p[(i + 1) % p.size()].X() * p[i].Y();
  if (twice < 0) std::reverse(p.begin(), p.end());
  const std::size_t n = p.size();
  std::vector<gp_Pnt> q;
  for (std::size_t i = 0; i < n; ++i) {
    auto line = [&](std::size_t k) {
      const gp_Vec dk = gp_Vec(p[k], p[(k + 1) % n]).Normalized();
      return std::make_pair(p[k].Translated(gp_Vec(dk.Y(), -dk.X(), 0) * d), dk);
    };
    const auto [a, da] = line((i + n - 1) % n);
    const auto [b, db] = line(i);
    const double den = da.X() * db.Y() - da.Y() * db.X();
    const gp_Vec w(a, b);
    q.push_back(a.Translated(da * ((w.X() * db.Y() - w.Y() * db.X()) / den)));
  }
  double area = 0.0;
  for (std::size_t i = 0; i < n; ++i) area += q[i].X() * q[(i + 1) % n].Y() - q[(i + 1) % n].X() * q[i].Y();
  return area / 2;
}
}  // namespace

TEST(Roof, GableAndShedOverSlantedSidesAndAHipWithAnInwardCorner) {
  const double kSlope = 0.4, kRiseUp = 100 * std::sqrt(1.16);
  // 8000 x 6000 with its south-east corner cut 2000 x 2000.
  RoofSpec s;
  s.kind = "gable";
  s.height = 3000;
  s.outline = {gp_Pnt(0, 0, 0), gp_Pnt(6000, 0, 0), gp_Pnt(8000, 2000, 0), gp_Pnt(8000, 6000, 0), gp_Pnt(0, 6000, 0)};
  // Gables chosen: the shorter parallel pair (east and west): ridge along x, over the middle.
  auto roof = makeRoof(s);
  ASSERT_TRUE(roof) << roof.error();
  auto b = sbr::geom::exactBounds(roof->solid);
  EXPECT_NEAR(b.CornerMin().X(), -300, 1e-4);  // the gable overhang
  EXPECT_NEAR(b.CornerMax().X(), 8300, 1e-4);
  EXPECT_NEAR(b.CornerMin().Y(), -600, 1e-4);  // the eaves
  EXPECT_NEAR(b.CornerMax().Y(), 6600, 1e-4);
  EXPECT_NEAR(roof->ridgeHeight, 3000 + kSlope * 3000 + kRiseUp, 1e-3);
  EXPECT_NEAR(roof->eaveHeight, 3000 - kSlope * 600, 1e-3);
  // Drawn the other way round: the same roof.
  RoofSpec back = s;
  std::reverse(back.outline.begin(), back.outline.end());
  const auto same = makeRoof(back);
  ASSERT_TRUE(same);
  EXPECT_NEAR(same->planArea, roof->planArea, 1.0);
  // Gables chosen by hand: south (side 0) and north (side 3): ridge along y, over x = 4000.
  s.gableSides = {0, 3};
  roof = makeRoof(s);
  ASSERT_TRUE(roof) << roof.error();
  b = sbr::geom::exactBounds(roof->solid);
  EXPECT_NEAR(b.CornerMin().Y(), -300, 1e-4);
  EXPECT_NEAR(b.CornerMax().Y(), 6300, 1e-4);
  EXPECT_NEAR(b.CornerMin().X(), -600, 1e-4);
  EXPECT_NEAR(roof->ridgeHeight, 3000 + kSlope * 4000 + kRiseUp, 1e-3);
  // The same numbers drawn the other way round mean the same sides as drawn there.
  back.gableSides = {static_cast<int>((2 * 5 - 2 - 0) % 5), static_cast<int>((2 * 5 - 2 - 3) % 5)};
  const auto sameHand = makeRoof(back);
  ASSERT_TRUE(sameHand) << sameHand.error();
  EXPECT_NEAR(sameHand->ridgeHeight, roof->ridgeHeight, 1e-3);
  s.gableSides = {7};
  EXPECT_FALSE(makeRoof(s));
  // Shed falling south: from the south eave up to the north one; verges at east and west.
  s.kind = "shed";
  s.gableSides.clear();
  s.fallX = 0;
  s.fallY = -1;
  roof = makeRoof(s);
  ASSERT_TRUE(roof) << roof.error();
  b = sbr::geom::exactBounds(roof->solid);
  EXPECT_NEAR(b.CornerMin().X(), -300, 1e-4);
  EXPECT_NEAR(b.CornerMax().Y(), 6600, 1e-4);
  EXPECT_NEAR(roof->eaveHeight, 3000 - kSlope * 600, 1e-3);
  EXPECT_NEAR(roof->ridgeHeight, 3000 + kSlope * 6600 + kRiseUp, 1e-3);
  // Hip over an outline with a slanted side and an inward corner: one solid, valleys; seen from
  // above it covers the outline grown by the eaves.
  RoofSpec dented;
  dented.kind = "hip";
  dented.height = 3000;
  dented.outline = {gp_Pnt(0, 0, 0), gp_Pnt(8000, 0, 0), gp_Pnt(8000, 4000, 0), gp_Pnt(5000, 4000, 0), gp_Pnt(3000, 8000, 0),
                    gp_Pnt(0, 8000, 0)};
  const auto hip = makeRoof(dented);
  ASSERT_TRUE(hip) << hip.error();
  EXPECT_GE(hip->parts, 2);
  EXPECT_NEAR(hip->planArea, grownArea(dented.outline, 600), 1.0);
  EXPECT_NEAR(hip->eaveHeight, 3000 - kSlope * 600, 1e-3);
  EXPECT_NEAR(hip->slopeArea, hip->planArea * std::sqrt(1.16), 1.0);
  // The walls under it can be cut to it too.
  EXPECT_TRUE(sbr::geom::roofSpaceAbove(dented, 100000.0));
  // Five inward corners with slanted sides: refused with the reason.
  RoofSpec star;
  star.kind = "hip";
  star.height = 3000;
  for (int i = 0; i < 12; ++i) {
    const double r = i % 2 == 0 ? 6000 : 3000, a = std::acos(-1.0) / 6 * i;
    star.outline.emplace_back(r * std::cos(a), r * std::sin(a), 0);
  }
  EXPECT_FALSE(makeRoof(star));
}

TEST(Roof, WhatCannotBeBuiltIsRefusedWithTheReason) {
  RoofSpec s = rect("gable");
  // Slanted sides with no parallel opposite pair: the gable sides must be chosen (docs/138).
  s.outline = {gp_Pnt(0, 0, 0), gp_Pnt(8000, 0, 0), gp_Pnt(9000, 5000, 0), gp_Pnt(1000, 7000, 0)};
  EXPECT_NE(makeRoof(s).error().find("parallel"), std::string::npos);
  s = rect("gable");
  s.outline.pop_back();
  EXPECT_FALSE(makeRoof(s));
  s = rect("gable");
  s.pitch = 0;
  EXPECT_FALSE(makeRoof(s));
  s = rect("gable");
  s.thickness = 0;
  EXPECT_FALSE(makeRoof(s));
  s = rect("gable");
  s.eave = -1;
  EXPECT_FALSE(makeRoof(s));
  s = rect("dome");
  EXPECT_FALSE(makeRoof(s));
  s = rect("shed");
  s.fallX = s.fallY = 0;
  EXPECT_FALSE(makeRoof(s));
  // No overhangs at all is fine.
  s = rect("hip");
  s.eave = s.gableEave = 0;
  const auto flush = makeRoof(s);
  ASSERT_TRUE(flush) << flush.error();
  EXPECT_NEAR(flush->planArea, 48e6, 1.0);
  EXPECT_NEAR(flush->eaveHeight, 6000, 1e-4);
}

struct Roofs : testing::Test {
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
  double top(const std::string& id) {
    return sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(id))).CornerMax().Z();
  }
};

TEST_F(Roofs, ARoofIsOnePartWhoseSizesCanBeChanged) {
  ASSERT_TRUE(modeler.setLevels({{"", "1FL", 0}, {"", "軒", 6000}}));
  const json made = ok("create_roof", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"kind", "gable"}, {"height", 6000}});
  const std::string id = made["id"];
  EXPECT_NEAR(made["plan_area"].get<double>(), 8.6 * 7.2, 1e-6);
  EXPECT_NEAR(made["slope_area"].get<double>(), 8.6 * 7.2 * std::sqrt(1.16), 1e-6);
  EXPECT_NEAR(made["ridge_height"].get<double>(), 7200 + kRise, 1e-4);
  EXPECT_FALSE(made["level"].get<std::string>().empty());  // made at the eave level
  EXPECT_EQ(modeler.document().find(id)->attributes.at("kind"), "roof");
  const auto steps = modeler.history().undoCount();

  // Another pitch and kind: the same object, one step.
  json changed = ok("change_roof", {{"id", id}, {"pitch", 5}, {"kind", "hip"}});
  EXPECT_EQ(changed["kind"], "hip");
  EXPECT_NEAR(changed["ridge_height"].get<double>(), 6000 + 0.5 * 3000 + 100 * std::sqrt(1.25), 1e-4);
  EXPECT_NEAR(top(id), 6000 + 0.5 * 3000 + 100 * std::sqrt(1.25), 1e-4);
  EXPECT_NEAR(changed["plan_area"].get<double>(), 9.2 * 7.2, 1e-6);
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(top(id), 7200 + kRise, 1e-4);
  EXPECT_EQ(ok("roof_info", {{"id", id}})["kind"], "gable");

  // The eave level 300 higher: the roof goes with it.
  std::vector<sbr::doc::FloorLevel> levels = modeler.levels();
  for (auto& l : levels)
    if (l.name != "1FL") l.elevation = 6300;
  ASSERT_TRUE(modeler.setLevels(levels));
  EXPECT_NEAR(top(id), 7500 + kRise, 1e-4);
  EXPECT_NEAR(ok("roof_info", {{"id", id}})["height"].get<double>(), 6300, 1e-6);

  // Moved: it reports where it stands, and a change keeps it there.
  ok("move", {{"ids", {id}}, {"vector", {1000, 0, 0}}});
  json info = ok("roof_info", {{"id", id}});
  EXPECT_NEAR(info["outline"][0][0].get<double>(), 1000, 1e-6);
  changed = ok("change_roof", {{"id", id}, {"eave", 900}});
  const auto b = sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(id)));
  EXPECT_NEAR(b.CornerMin().X(), 1000 - 300, 1e-4);
  EXPECT_NEAR(b.CornerMin().Y(), -900, 1e-4);
  EXPECT_NEAR(changed["height"].get<double>(), 6300, 1e-6);

  // Refused changes change nothing.
  const double before = top(id);
  EXPECT_TRUE(refused("change_roof", {{"id", id}, {"pitch", 0}}));
  EXPECT_TRUE(refused("change_roof", {{"id", id}, {"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {4000, 3000}, {0, 6000}}}}));
  EXPECT_NEAR(top(id), before, 1e-9);

  // Reshaped by hand it is no longer read as a roof, and says so.
  ok("push_pull", {{"id", id}, {"face", {{"normal", {1, 0, 0}}}}, {"distance", 200}});
  EXPECT_TRUE(refused("roof_info", {{"id", id}}));
  EXPECT_TRUE(refused("change_roof", {{"id", id}, {"pitch", 6}}));
}

TEST_F(Roofs, OnGridLinesAndNotOnOtherThings) {
  ok("create_grid", {{"x_spacings", {7280}}, {"y_spacings", {5460}}});
  const json made = ok("create_roof", {{"grid", {{"x", {"X1", "X2"}}, {"y", {"Y1", "Y2"}}}}, {"kind", "hip"}, {"height", 5800},
                                       {"eave", 455}, {"pitch", 4.5}});
  EXPECT_NEAR(made["plan_area"].get<double>(), (7.28 + 0.91) * (5.46 + 0.91), 1e-6);
  const std::string box = ok("create_box", {{"origin", {0, 0, 0}}, {"width", 500}, {"depth", 500}, {"height", 500}})["id"];
  EXPECT_TRUE(refused("roof_info", {{"id", box}}));
  EXPECT_TRUE(refused("create_roof", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}}}, {"height", 3000}}));
}

// Walls up to the roof (docs/97 N-05). A house 8000 x 6000: walls 200 thick on the outline,
// 3000 high; a gable roof bearing at 3000 (ridge along x, pitch 4): its underside is
// 3000 + 0.4 x (distance in from the y = 0 / y = 6000 lines) high.
struct WallsUnderRoof : Roofs {
  std::string south, east, north, west, roof;
  struct Box {
    double x0, y0, z0, x1, y1, z1;
  };
  Box box(const std::string& id) {
    const auto b = sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(id)));
    return {b.CornerMin().X(), b.CornerMin().Y(), b.CornerMin().Z(), b.CornerMax().X(), b.CornerMax().Y(), b.CornerMax().Z()};
  }
  double volume(const std::string& id) { return sbr::geom::volume(modeler.document().find(id)->shape); }
  // The volume of a 200 wall along y from y0 to y1 standing from 0 up to the underside.
  static double gableWall(double y0, double y1, double bearing = 3000, double slope = 0.4) {
    auto upTo = [&](double y) {  // integral of the underside's height from 0 to y (0 <= y <= 6000)
      const double a = std::min(y, 3000.0), b = std::max(y - 3000.0, 0.0);
      return bearing * y + slope * (a * a / 2 + (3000.0 * b - b * b / 2));
    };
    return 200.0 * (upTo(y1) - upTo(y0));
  }
  void SetUp() override {
    const json ids = ok("create_wall", {{"points", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}, {0, 0}}},
                                        {"thickness", 200}, {"height", 3000}})["ids"];
    south = ids[0];
    east = ids[1];
    north = ids[2];
    west = ids[3];
    roof = ok("create_roof", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"kind", "gable"}, {"height", 3000}})["id"];
  }
};

TEST_F(WallsUnderRoof, GableEndWallsGetTheirTriangleAndStayWalls) {
  const Box e0 = box(east), s0 = box(south);
  const double southBefore = volume(south);
  const auto steps = modeler.history().undoCount();
  const json fitted = ok("fit_walls_to_roof", {{"roof", roof}});
  EXPECT_EQ(fitted["walls"].size(), 4u);  // every wall under the roof
  EXPECT_EQ(modeler.history().undoCount(), steps + 1);

  // The gable end: up to the underside along its length (the ridge is 1200 above the bearing height).
  const Box e1 = box(east);
  EXPECT_NEAR(e1.y0, e0.y0, 1e-6);
  EXPECT_NEAR(e1.y1, e0.y1, 1e-6);
  EXPECT_NEAR(e1.z1, 4200, 1e-4);
  EXPECT_NEAR(volume(east), gableWall(e0.y0, e0.y1), 50.0);
  EXPECT_NEAR(volume(west), gableWall(box(west).y0, box(west).y1), 50.0);
  // The eave walls: cut along the slope across their thickness (40 up at the inner face,
  // 40 down at the outer one): the same volume.
  const Box s1 = box(south);
  EXPECT_NEAR(s1.z1, 3040, 1e-4);
  EXPECT_NEAR(volume(south), southBefore, 50.0);
  EXPECT_NEAR(s1.x0, s0.x0, 1e-6);

  // Still walls: length and openings as before.
  EXPECT_TRUE(ok("member_info", {{"id", east}}).contains("fields"));
  ok("place_openings", {{"host", east}, {"face_normal", {1, 0, 0}}, {"count", 1}, {"width", 800}, {"height", 1000}, {"sill", 900}});
  EXPECT_EQ(ok("list_openings", {{"ids", {east}}})["hosts"][0]["openings"].size(), 1u);
  EXPECT_NEAR(volume(east), gableWall(e0.y0, e0.y1) - 800.0 * 1000 * 200, 50.0);
  EXPECT_NEAR(box(east).z1, 4200, 1e-4);
  // A room is still found from them.
  EXPECT_TRUE(ok("room_at", {{"at", {4000, 3000}}})["closed"].get<bool>());

  // The roof steeper: the walls follow, in the roof's undo step.
  const auto before = modeler.history().undoCount();
  ok("change_roof", {{"id", roof}, {"pitch", 6}});
  EXPECT_EQ(modeler.history().undoCount(), before + 1);
  EXPECT_NEAR(box(east).z1, 3000 + 0.6 * 3000, 1e-4);
  EXPECT_NEAR(volume(east), gableWall(e0.y0, e0.y1, 3000, 0.6) - 800.0 * 1000 * 200, 50.0);
  ASSERT_TRUE(modeler.undo());
  EXPECT_NEAR(box(east).z1, 4200, 1e-4);

  // The roof 500 higher: the walls reach it.
  ok("change_roof", {{"id", roof}, {"height", 3500}});
  EXPECT_NEAR(box(east).z1, 4700, 1e-4);
  EXPECT_NEAR(box(south).z1, 3540, 1e-4);
  ASSERT_TRUE(modeler.undo());

  // The gable wall moved 1000 in (its neighbours follow): fitted again where it stands.
  ok("move", {{"ids", {east}}, {"vector", {-1000, 0, 0}}});
  EXPECT_NEAR(box(east).x1, e0.x1 - 1000, 1e-6);
  EXPECT_NEAR(box(east).z1, 4200, 1e-4);
  EXPECT_NEAR(volume(east), gableWall(e0.y0, e0.y1) - 800.0 * 1000 * 200, 50.0);
  ASSERT_TRUE(modeler.undo());

  // Released: flat again at its own height, the opening stays.
  ok("release_walls_from_roof", {{"walls", {east}}});
  EXPECT_NEAR(box(east).z1, 3000, 1e-6);
  EXPECT_NEAR(volume(east), 200.0 * (e0.y1 - e0.y0) * 3000 - 800.0 * 1000 * 200, 50.0);
  EXPECT_EQ(ok("list_openings", {{"ids", {east}}})["hosts"][0]["openings"].size(), 1u);
  EXPECT_NEAR(box(west).z1, 4200, 1e-4);  // the others stay fitted
  ok("release_walls_from_roof", {{"roof", roof}});
  EXPECT_NEAR(box(west).z1, 3000, 1e-6);
  EXPECT_NEAR(box(south).z1, 3000, 1e-6);
  EXPECT_NEAR(volume(south), southBefore, 1.0);
  EXPECT_TRUE(refused("release_walls_from_roof", {{"roof", roof}}));
}

// docs/158: with storeys, only the walls of the storey under the roof (and walls reaching its
// bearing height) are under it: the lower storey's walls are not stretched up through the upper one.
TEST_F(WallsUnderRoof, TheLowerStoreysWallsStayAsTheyAre) {
  ok("set_levels", {{"levels", {{{"name", "1FL"}, {"elevation", 0}}, {{"name", "2FL"}, {"elevation", 3000}}}}});
  const json upper = ok("create_wall", {{"points", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}, {0, 0}}},
                                        {"thickness", 200}, {"height", 3000}, {"base", 3000}})["ids"];
  const std::string through = ok("create_wall", {{"points", {{4000, 100}, {4000, 5900}}}, {"thickness", 100}, {"height", 6000}})["ids"][0];
  const std::string knee = ok("create_wall", {{"points", {{2000, 100}, {2000, 5900}}}, {"thickness", 100}, {"height", 2000}, {"base", 3000}})["ids"][0];
  const std::string top = ok("create_roof", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"kind", "gable"}, {"height", 6000}})["id"];
  const json fitted = ok("fit_walls_to_roof", {{"roof", top}});
  // The four upper walls, the wall through both storeys and the low wall on the upper storey; not the four below.
  EXPECT_EQ(fitted["walls"].size(), 6u);
  EXPECT_NEAR(box(south).z1, 3000, 1e-6);
  EXPECT_NEAR(box(east).z1, 3000, 1e-6);
  EXPECT_NEAR(box(upper[1]).z1, 7200, 1e-4);   // the upper gable end wall up to the ridge
  EXPECT_NEAR(box(through).z1, 7200, 1e-4);
  EXPECT_NEAR(box(knee).z1, 7200, 1e-4);       // it runs across under the ridge too
  // The lower roof of the fixture (bearing at 3000) still takes the walls standing on 1FL, its
  // storey: the four and the wall through both storeys.
  EXPECT_EQ(modeler.wallsUnderRoof(roof).size(), 5u);
}

TEST_F(WallsUnderRoof, WhatCannotBeFittedIsRefusedAndNothingChanges) {
  // A wall on the storey above the bearing height is not "under" the roof, and cannot be fitted.
  const std::string high = ok("create_wall", {{"points", {{1000, 3000}, {7000, 3000}}}, {"thickness", 100}, {"height", 500}, {"base", 6000}})["ids"][0];
  const std::string low = ok("create_wall", {{"points", {{4000, 100}, {4000, 5900}}}, {"thickness", 100}, {"height", 2400}})["ids"][0];
  const json fitted = ok("fit_walls_to_roof", {{"roof", roof}});
  EXPECT_EQ(fitted["walls"].size(), 5u);  // the four and the low partition, not the one above
  // The partition stands under the ridge line: up to it.
  const auto b = sbr::geom::exactBounds(modeler.document().worldShape(*modeler.document().find(low)));
  EXPECT_NEAR(b.CornerMax().Z(), 4200, 1e-4);
  const double z = box(high).z1;
  EXPECT_TRUE(refused("fit_walls_to_roof", {{"roof", roof}, {"walls", {high}}}));
  EXPECT_NEAR(box(high).z1, z, 1e-9);
  // Not a roof; a box is not a wall.
  const std::string block = ok("create_box", {{"origin", {1000, 1000, 0}}, {"width", 500}, {"depth", 500}, {"height", 500}})["id"];
  EXPECT_TRUE(refused("fit_walls_to_roof", {{"roof", block}}));
  EXPECT_TRUE(refused("fit_walls_to_roof", {{"roof", roof}, {"walls", {block}}}));
  // The roof deleted: the walls keep the shape they have (they can still be released).
  ok("delete", {{"ids", {roof}}});
  EXPECT_NEAR(box(east).z1, 4200, 1e-4);
  ok("release_walls_from_roof", {{"walls", {east}}});
  EXPECT_NEAR(box(east).z1, 3000, 1e-6);
}
}  // namespace

// docs/108: a hip roof over an outline with slanted sides (no inward corners).
TEST(Roof, AHipRoofCoversAnOutlineWithSlantedSides) {
  // 8000 x 6000 with its south-east corner cut at 45 degrees.
  RoofSpec spec;
  spec.kind = "hip";
  spec.outline = {gp_Pnt(0, 0, 0), gp_Pnt(6000, 0, 0), gp_Pnt(8000, 2000, 0), gp_Pnt(8000, 6000, 0), gp_Pnt(0, 6000, 0)};
  spec.height = 3000;
  const auto roof = makeRoof(spec);
  ASSERT_TRUE(roof) << roof.error();
  EXPECT_TRUE(BRepCheck_Analyzer(roof->solid).IsValid());
  int solids = 0;
  for (TopExp_Explorer e(roof->solid, TopAbs_SOLID); e.More(); e.Next()) ++solids;
  EXPECT_EQ(solids, 1);
  // Seen from above: the outline grown by the eave (600) with mitred corners:
  // area + perimeter x eave + eave^2 x sum of tan(half the turn) (three of 90, two of 45 degrees).
  const double pi = std::acos(-1.0);
  const double grown = 46e6 + (24000 + 2000 * std::sqrt(2.0)) * 600 + 600.0 * 600 * (3 + 2 * std::tan(pi / 8));
  EXPECT_NEAR(roof->planArea, grown, 1.0);
  EXPECT_NEAR(roof->slopeArea, grown * std::sqrt(1.16), 2.0);
  EXPECT_NEAR(sbr::geom::volume(roof->solid), grown * kRise, 200.0);
  EXPECT_NEAR(roof->eaveHeight, 3000 - 0.4 * 600, 1e-4);  // every eave at one height
  // Drawn the other way round: the same roof.
  std::reverse(spec.outline.begin(), spec.outline.end());
  const auto other = makeRoof(spec);
  ASSERT_TRUE(other) << other.error();
  EXPECT_NEAR(other->planArea, grown, 1.0);

  // A regular hexagon (corner radius 4000): a point over its middle, as high as its
  // apothem times the slope.
  RoofSpec hexagon;
  hexagon.kind = "hip";
  hexagon.height = 3000;
  for (int i = 0; i < 6; ++i) hexagon.outline.emplace_back(4000 * std::cos(pi / 3 * i), 4000 * std::sin(pi / 3 * i), 0);
  const auto tent = makeRoof(hexagon);
  ASSERT_TRUE(tent) << tent.error();
  EXPECT_NEAR(tent->ridgeHeight, 3000 + 0.4 * 4000 * std::cos(pi / 6) + kRise, 1e-3);
  const auto b = sbr::geom::exactBounds(tent->solid);
  EXPECT_NEAR(b.CornerMax().X(), 4000 + 600 / std::cos(pi / 6), 1e-3);  // the eave corner, mitred

  // What it takes from walls under it (walls up to the roof) is built the same way.
  EXPECT_TRUE(sbr::geom::roofSpaceAbove(spec, 100000.0));
  // Since docs/138: gable over slanted sides, and a hip with inward corners; a gable with
  // inward corners and slanted sides is still refused.
  spec.kind = "gable";
  ASSERT_TRUE(makeRoof(spec));
  RoofSpec dented;
  dented.kind = "hip";
  dented.height = 3000;
  dented.outline = {gp_Pnt(0, 0, 0), gp_Pnt(8000, 0, 0), gp_Pnt(8000, 6000, 0), gp_Pnt(4000, 3000, 0), gp_Pnt(0, 6000, 0)};
  ASSERT_TRUE(makeRoof(dented)) << makeRoof(dented).error();
  dented.kind = "gable";
  const auto refused = makeRoof(dented);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().find("no inward corners"), std::string::npos);
}
