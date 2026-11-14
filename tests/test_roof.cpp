// Roofs as one solid (docs/90 G-08, docs/99). Sizes are checked against hand calculations:
// seen from above a roof covers its outline grown by the overhangs; its volume is that area
// times its thickness measured upright; its upper surface is that area times sqrt(1 + slope^2).
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

TEST(Roof, WhatCannotBeBuiltIsRefusedWithTheReason) {
  RoofSpec s = rect("gable");
  s.outline[2] = gp_Pnt(8000, 5000, 0);  // a slanted side
  EXPECT_NE(makeRoof(s).error().find("right-angled"), std::string::npos);
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
  EXPECT_TRUE(refused("change_roof", {{"id", id}, {"outline", {{0, 0}, {8000, 0}, {8000, 5000}, {0, 6000}}}}));
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
}  // namespace
