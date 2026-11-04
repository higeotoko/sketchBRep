// Rhino-style operations (docs/47): Sweep2, EdgeSrf, Patch, fillet /
// chamfer and shell, each kept in the recipe (size editable, saved, undone).

#include <gtest/gtest.h>
#include <cmath>

#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Surfaces.h"

using namespace sbr;
using namespace sbr::doc;
using json = nlohmann::json;

namespace {

ObjectId box(Modeler& m, double size) {
  const ObjectId id = *m.createRectangle(geom::SketchPlane::ground(), size, size);
  EXPECT_TRUE(m.extrude(id, size));
  return id;
}

TopoDS_Shape world(const Modeler& m, const ObjectId& id) { return m.document().worldShape(*m.document().find(id)); }

// The box's edge standing at (x, y).
TopoDS_Edge uprightEdge(const Modeler& m, const ObjectId& id, double x, double y) {
  for (TopExp_Explorer e(world(m, id), TopAbs_EDGE); e.More(); e.Next()) {
    GProp_GProps p;
    BRepGProp::LinearProperties(e.Current(), p);
    const gp_Pnt c = p.CentreOfMass();
    if (std::abs(c.X() - x) < 1e-6 && std::abs(c.Y() - y) < 1e-6) return TopoDS::Edge(e.Current());
  }
  return {};
}

TopoDS_Face faceAtZ(const Modeler& m, const ObjectId& id, double z) {
  for (TopExp_Explorer f(world(m, id), TopAbs_FACE); f.More(); f.Next()) {
    GProp_GProps p;
    BRepGProp::SurfaceProperties(f.Current(), p);
    if (std::abs(p.CentreOfMass().Z() - z) < 1e-6) return TopoDS::Face(f.Current());
  }
  return {};
}

double area(const TopoDS_Shape& s) {
  GProp_GProps p;
  BRepGProp::SurfaceProperties(s, p);
  return p.Mass();
}

}  // namespace

TEST(Surfaces, FilletAnEdgeAndChangeItsRadius) {
  Modeler m;
  const ObjectId b = box(m, 1000);
  const auto key = m.roundEdges(b, {uprightEdge(m, b, 1000, 1000)}, 100, false);
  ASSERT_TRUE(key) << key.error();
  const double removed = (100.0 * 100 - M_PI * 100 * 100 / 4) * 1000;
  EXPECT_NEAR(geom::volume(world(m, b)), 1e9 - removed, 1.0);
  // The radius stays editable.
  ASSERT_TRUE(m.setParameter(b, *key, 200));
  EXPECT_NEAR(geom::volume(world(m, b)), 1e9 - 4 * removed, 4.0);
  // Saved and opened again.
  const std::string path = ::testing::TempDir() + "fillet.sbr";
  ASSERT_TRUE(m.save(path));
  Modeler back;
  ASSERT_TRUE(back.load(path));
  EXPECT_NEAR(geom::volume(world(back, b)), 1e9 - 4 * removed, 4.0);
  // Too large: refused, the box as it was.
  const double before = geom::volume(world(m, b));
  EXPECT_FALSE(m.roundEdges(b, {uprightEdge(m, b, 0, 0)}, 1500, false));
  EXPECT_NEAR(geom::volume(world(m, b)), before, 1e-6);
  // Undo: the radius back to 100, then no fillet.
  ASSERT_TRUE(m.undo());
  EXPECT_NEAR(geom::volume(world(m, b)), 1e9 - removed, 1.0);
  ASSERT_TRUE(m.undo());
  EXPECT_NEAR(geom::volume(world(m, b)), 1e9, 1e-3);
}

TEST(Surfaces, ChamferSeveralEdges) {
  Modeler m;
  const ObjectId b = box(m, 1000);
  const auto key = m.roundEdges(b, {uprightEdge(m, b, 0, 0), uprightEdge(m, b, 1000, 0)}, 100, true);
  ASSERT_TRUE(key) << key.error();
  EXPECT_NEAR(geom::volume(world(m, b)), 1e9 - 2 * (0.5 * 100 * 100 * 1000), 1.0);
  EXPECT_TRUE(BRepCheck_Analyzer(world(m, b)).IsValid());
}

TEST(Surfaces, ShellOpenAtTheTop) {
  Modeler m;
  const ObjectId b = box(m, 1000);
  const auto key = m.shellSolid(b, {faceAtZ(m, b, 1000)}, 100);
  ASSERT_TRUE(key) << key.error();
  EXPECT_NEAR(geom::volume(world(m, b)), 1e9 - 800.0 * 800 * 900, 10.0);
  ASSERT_TRUE(m.setParameter(b, *key, 50));
  EXPECT_NEAR(geom::volume(world(m, b)), 1e9 - 900.0 * 900 * 950, 10.0);
  // Nothing open: a closed hollow, walls all round.
  const ObjectId c = box(m, 1000);
  ASSERT_TRUE(m.shellSolid(c, {}, 100));
  EXPECT_NEAR(geom::volume(world(m, c)), 1e9 - 800.0 * 800 * 800, 10.0);
  EXPECT_FALSE(m.shellSolid(c, {}, 600));  // no room inside
}

TEST(Surfaces, Sweep2StretchesBetweenTheRails) {
  Modeler m;
  const ObjectId rail1 = *m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(3000, 0, 0)});
  const ObjectId rail2 = *m.createPolyline({gp_Pnt(0, 1000, 0), gp_Pnt(3000, 2000, 0)});
  // An arch from rail to rail.
  const ObjectId profile = *m.createCurve({gp_Pnt(0, 0, 0), gp_Pnt(0, 500, 400), gp_Pnt(0, 1000, 0)}, false);
  const auto made = m.sweep2({rail1, {}}, {rail2, {}}, {{profile, {}}});
  ASSERT_TRUE(made) << made.error();
  const TopoDS_Shape s = world(m, *made);
  EXPECT_TRUE(BRepCheck_Analyzer(s).IsValid());
  const Bnd_Box b = geom::exactBounds(s);
  EXPECT_NEAR(b.CornerMax().X(), 3000, 1.0);
  EXPECT_NEAR(b.CornerMax().Y(), 2000, 5.0);  // reaches the second rail at its far end
  EXPECT_GT(area(s), 3000.0 * 1000);
  // The curves stay.
  EXPECT_TRUE(m.document().find(rail1) && m.document().find(profile));
  ASSERT_TRUE(m.undo());
  EXPECT_FALSE(m.document().find(*made));
}

TEST(Surfaces, EdgeSurfaceFromTwoAndFourCurves) {
  Modeler m;
  const ObjectId a = *m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0)});
  const ObjectId b = *m.createPolyline({gp_Pnt(0, 1000, 500), gp_Pnt(1000, 1000, 500)});
  const auto ruled = m.surfaceFromCurves({{a, {}}, {b, {}}}, false);
  ASSERT_TRUE(ruled) << ruled.error();
  EXPECT_NEAR(area(world(m, *ruled)), 1000.0 * std::hypot(1000.0, 500.0), 1.0);
  // Four lines of a bent square.
  const ObjectId c = *m.createPolyline({gp_Pnt(1000, 0, 0), gp_Pnt(1000, 1000, 500)});
  const ObjectId d = *m.createPolyline({gp_Pnt(0, 1000, 500), gp_Pnt(0, 0, 0)});
  const auto four = m.surfaceFromCurves({{a, {}}, {c, {}}, {b, {}}, {d, {}}}, false);
  ASSERT_TRUE(four) << four.error();
  EXPECT_TRUE(BRepCheck_Analyzer(world(m, *four)).IsValid());
  EXPECT_FALSE(m.surfaceFromCurves({{a, {}}}, false));  // one curve
}

TEST(Surfaces, PatchOverAClosedOutline) {
  Modeler m;
  // A saddle-like outline: four lines, two corners raised.
  std::vector<Modeler::CurvePick> loop;
  const gp_Pnt p[4] = {gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 300), gp_Pnt(1000, 1000, 0), gp_Pnt(0, 1000, 300)};
  for (int i = 0; i < 4; ++i) loop.push_back({*m.createPolyline({p[i], p[(i + 1) % 4]}), {}});
  const auto patch = m.surfaceFromCurves(loop, true);
  ASSERT_TRUE(patch) << patch.error();
  EXPECT_TRUE(BRepCheck_Analyzer(world(m, *patch)).IsValid());
  EXPECT_GT(area(world(m, *patch)), 1e6);
  // A flat outline: the exact flat face.
  std::vector<Modeler::CurvePick> flat;
  const gp_Pnt q[4] = {gp_Pnt(0, 2000, 0), gp_Pnt(500, 2000, 0), gp_Pnt(500, 2500, 0), gp_Pnt(0, 2500, 0)};
  for (int i = 0; i < 4; ++i) flat.push_back({*m.createPolyline({q[i], q[(i + 1) % 4]}), {}});
  const auto plain = m.surfaceFromCurves(flat, true);
  ASSERT_TRUE(plain) << plain.error();
  EXPECT_NEAR(area(world(m, *plain)), 250000, 1e-3);
  // A gap: refused.
  flat.pop_back();
  EXPECT_FALSE(m.surfaceFromCurves(flat, true));
}

TEST(Surfaces, FromEdgesOfASolid) {
  Modeler m;
  const ObjectId b = box(m, 1000);
  // Two opposite top edges of the box, picked as edges.
  std::vector<Modeler::CurvePick> picks;
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(world(m, b), TopAbs_EDGE, edges);
  for (int i = 1; i <= edges.Extent(); ++i) {
    GProp_GProps p;
    BRepGProp::LinearProperties(edges(i), p);
    const gp_Pnt c = p.CentreOfMass();
    if (std::abs(c.Z() - 1000) < 1e-6 && std::abs(c.X() - 500) < 1e-6) picks.push_back({b, TopoDS::Edge(edges(i))});
  }
  ASSERT_EQ(picks.size(), 2u);
  const auto lid = m.surfaceFromCurves(picks, false);
  ASSERT_TRUE(lid) << lid.error();
  EXPECT_NEAR(area(world(m, *lid)), 1e6, 1e-3);
}

TEST(Surfaces, SavedAndLoadedAgain) {
  Modeler m;
  const ObjectId b = box(m, 1000);
  ASSERT_TRUE(m.shellSolid(b, {faceAtZ(m, b, 1000)}, 100));
  const ObjectId c = box(m, 500);
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(world(m, c), TopAbs_EDGE, edges);
  ASSERT_TRUE(m.roundEdges(c, {TopoDS::Edge(edges(1))}, 50, false));
  const ObjectId rail1 = *m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(3000, 0, 0)});
  const ObjectId rail2 = *m.createPolyline({gp_Pnt(0, 1000, 0), gp_Pnt(3000, 1000, 0)});
  const ObjectId profile = *m.createCurve({gp_Pnt(0, 0, 0), gp_Pnt(0, 500, 400), gp_Pnt(0, 1000, 0)}, false);
  const ObjectId swept = *m.sweep2({rail1, {}}, {rail2, {}}, {{profile, {}}});
  const std::string text = serializeDocument(m.document());
  Document d;
  std::vector<std::string> warnings;
  ASSERT_TRUE(deserializeDocument(text, d, &warnings).ok());
  EXPECT_TRUE(warnings.empty()) << warnings.front();
  for (const ObjectId& id : {b, c, swept}) {
    EXPECT_NEAR(area(d.worldShape(*d.find(id))), area(world(m, id)), 1e-3) << id;
  }
}

TEST(Surfaces, Api) {
  Modeler m;
  api::CommandApi commands(m);
  auto run = [&](const std::string& command, const json& args) {
    return json::parse(commands.execute(json{{"command", command}, {"args", args}}.dump()));
  };
  const ObjectId b = box(m, 1000);
  json args = json::parse(R"({"edges": [{"near": [500, 0, 1000]}], "radius": 100})");
  args["id"] = b;
  const json f = run("fillet_edges", args);
  ASSERT_TRUE(f.value("ok", false)) << f.dump();
  json set = {{"id", b}, {"key", f["result"]["parameter"]}, {"value", 200}};
  ASSERT_TRUE(run("set_parameter", set).value("ok", false));
  args = json::parse(R"({"open": [{"normal": [0, 0, -1]}], "thickness": 50})");
  args["id"] = b;
  const json s = run("shell", args);
  ASSERT_TRUE(s.value("ok", false)) << s.dump();
  const ObjectId l1 = *m.createPolyline({gp_Pnt(0, 3000, 0), gp_Pnt(1000, 3000, 0)});
  const ObjectId l2 = *m.createPolyline({gp_Pnt(0, 4000, 300), gp_Pnt(1000, 4000, 300)});
  const json e = run("edge_surface", json{{"curves", json::array({l1, l2})}});
  ASSERT_TRUE(e.value("ok", false)) << e.dump();
  EXPECT_FALSE(run("patch", json{{"curves", json::array({l1, l2})}}).value("ok", true));  // not closed
  const ObjectId p = *m.createPolyline({gp_Pnt(0, 3000, 0), gp_Pnt(0, 4000, 300)});
  const json sw = run("sweep2", json{{"rail1", l1}, {"rail2", l2}, {"profiles", json::array({p})}});
  ASSERT_TRUE(sw.value("ok", false)) << sw.dump();
}

TEST(Surfaces, TooLargeSaysHowLarge) {
  Modeler m;
  const ObjectId b = box(m, 100);
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(world(m, b), TopAbs_EDGE, edges);
  const auto r = m.roundEdges(b, {TopoDS::Edge(edges(1))}, 500, false);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().find("up to about"), std::string::npos) << r.error();
}

// docs/47 (2D): corners of a flat face rounded, radius editable, then
// pushed up into a box with rounded corners.
TEST(Surfaces, FlatFaceCornersRounded) {
  Modeler m;
  const ObjectId r = *m.createRectangle(geom::SketchPlane::ground(), 1000, 600);
  const std::vector<gp_Pnt> corners{gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp_Pnt(1000, 600, 0), gp_Pnt(0, 600, 0)};
  const auto key = m.roundCorners(r, corners, 100, false);
  ASSERT_TRUE(key) << key.error();
  EXPECT_NEAR(area(world(m, r)), 1000.0 * 600 - (4 - M_PI) * 100 * 100, 1e-3);
  ASSERT_TRUE(m.setParameter(r, *key, 50));
  EXPECT_NEAR(area(world(m, r)), 1000.0 * 600 - (4 - M_PI) * 50 * 50, 1e-3);
  const auto chamfered = m.roundCorners(*m.createRectangle(geom::SketchPlane::ground(), 1000, 600),
                                        {gp_Pnt(0, 0, 0)}, 100, true);
  ASSERT_TRUE(chamfered) << chamfered.error();
  const auto tooLarge = m.roundCorners(*m.createRectangle(geom::SketchPlane::ground(), 1000, 600), {gp_Pnt(0, 0, 0)}, 5000, false);
  ASSERT_FALSE(tooLarge);
  EXPECT_NE(tooLarge.error().find("up to about"), std::string::npos) << tooLarge.error();
  // Saved and loaded again.
  Document d;
  ASSERT_TRUE(deserializeDocument(serializeDocument(m.document()), d).ok());
  EXPECT_NEAR(area(d.worldShape(*d.find(r))), area(world(m, r)), 1e-6);
  // Pushed up: a box with rounded corners.
  ASSERT_TRUE(m.extrude(r, 300));
  EXPECT_NEAR(geom::volume(world(m, r)), (1000.0 * 600 - (4 - M_PI) * 50 * 50) * 300, 1.0);
  const ObjectId plain = *m.createRectangle(geom::SketchPlane::ground(), 10, 10);
  EXPECT_EQ(geom::faceCorners(TopoDS::Face(TopExp_Explorer(world(m, plain), TopAbs_FACE).Current())).size(), 4u);
}

// AutoCAD's fillet of two lines: cut back / extended to meet, joined by an
// arc; 0 just makes them meet; a corner inside one line object splits it;
// when the outline closes, its face is made.
TEST(Surfaces, LinesFilletedTogether) {
  Modeler m;
  auto points = [&](const ObjectId& id) { return std::get<PolylineCurve>(m.document().find(id)->recipe.base).points; };
  // Two lines not touching, at right angles.
  const ObjectId a = *m.createPolyline({gp_Pnt(200, 0, 0), gp_Pnt(1000, 0, 0)});
  const ObjectId b = *m.createPolyline({gp_Pnt(0, 300, 0), gp_Pnt(0, 1000, 0)});
  const std::size_t steps = m.history().undoCount();
  auto arc = m.filletLines({a, 0, gp_Pnt(600, 0, 0)}, {b, 0, gp_Pnt(0, 600, 0)}, 100, false);
  ASSERT_TRUE(arc) << arc.error();
  ASSERT_TRUE(arc->has_value());
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  EXPECT_LT(points(a)[0].Distance(gp_Pnt(100, 0, 0)), 1e-6);
  EXPECT_LT(points(b)[0].Distance(gp_Pnt(0, 100, 0)), 1e-6);
  GProp_GProps p;
  BRepGProp::LinearProperties(world(m, **arc), p);
  EXPECT_NEAR(p.Mass(), M_PI * 100 / 2, 1e-6);  // a quarter circle
  // Its middle bulges towards the corner.
  const Bnd_Box box = geom::exactBounds(world(m, **arc));
  EXPECT_NEAR(box.CornerMin().X(), 0, 1e-6);
  ASSERT_TRUE(m.undo());
  // 0: they just meet.
  auto none = m.filletLines({a, 0, gp_Pnt(600, 0, 0)}, {b, 0, gp_Pnt(0, 600, 0)}, 0, false);
  ASSERT_TRUE(none) << none.error();
  EXPECT_FALSE(none->has_value());
  EXPECT_LT(points(a)[0].Distance(gp_Pnt(0, 0, 0)), 1e-6);
  EXPECT_LT(points(b)[0].Distance(gp_Pnt(0, 0, 0)), 1e-6);
  // Parallel lines never meet; too large is said with how large.
  const ObjectId c = *m.createPolyline({gp_Pnt(0, 2000, 0), gp_Pnt(1000, 2000, 0)});
  EXPECT_FALSE(m.filletLines({a, 0, gp_Pnt(600, 0, 0)}, {c, 0, gp_Pnt(500, 2000, 0)}, 10, false));
  const auto big = m.filletLines({a, 0, gp_Pnt(600, 0, 0)}, {b, 0, gp_Pnt(0, 600, 0)}, 5000, false);
  ASSERT_FALSE(big);
  EXPECT_NE(big.error().find("up to about"), std::string::npos) << big.error();
  // Chamfer: a straight cut.
  const ObjectId e = *m.createPolyline({gp_Pnt(5000, 0, 0), gp_Pnt(6000, 0, 0), gp_Pnt(6000, 1000, 0)});
  auto cut = m.filletLines({e, 0, gp_Pnt(5500, 0, 0)}, {e, 1, gp_Pnt(6000, 500, 0)}, 100, true);
  ASSERT_TRUE(cut) << cut.error();
  ASSERT_TRUE(cut->has_value());
  EXPECT_EQ(points(e).size(), 2u);  // split at the corner
  EXPECT_LT(points(e)[1].Distance(gp_Pnt(5900, 0, 0)), 1e-6);
  EXPECT_FALSE(m.filletLines({e, 0, gp_Pnt(5500, 0, 0)}, {e, 0, gp_Pnt(5600, 0, 0)}, 10, false));
}

TEST(Surfaces, FilletedLinesCloseIntoAFace) {
  Modeler m;
  const ObjectId p1 = *m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(0, 1000, 0), gp_Pnt(1000, 1000, 0)});
  const ObjectId p2 = *m.createPolyline({gp_Pnt(200, 0, 0), gp_Pnt(1000, 0, 0), gp_Pnt(1000, 800, 0)});
  ASSERT_TRUE(m.filletLines({p1, 0, gp_Pnt(0, 500, 0)}, {p2, 0, gp_Pnt(600, 0, 0)}, 0, false));
  auto arc = m.filletLines({p1, 1, gp_Pnt(500, 1000, 0)}, {p2, 1, gp_Pnt(1000, 400, 0)}, 100, false);
  ASSERT_TRUE(arc) << arc.error();
  double faces = 0;
  for (const Object& o : m.document().objects()) {
    if (o.kind() == ObjectKind::Face) faces += area(m.document().worldShape(o));
  }
  EXPECT_NEAR(faces, 1e6 - (1 - M_PI / 4) * 100 * 100, 1e-3);
}

// Fillets, chamfers and shells report progress and stop when the user
// cancels (the progress dialog's 中止): the model stays as it was.
#include "sbr/geom/Progress.h"

namespace {
class CancelEverything : public geom::CancellableProgress {
 protected:
  Standard_Boolean UserBreak() override { return Standard_True; }
  void Show(const Message_ProgressScope&, const Standard_Boolean) override {}
};
}  // namespace

TEST(Surfaces, CancelledLeavesTheSolidAsItWas) {
  Modeler m;
  const ObjectId b = box(m, 1000);
  const std::size_t steps = m.history().undoCount();
  geom::setProgressIndicator(new CancelEverything());
  const auto fillet = m.roundEdges(b, {uprightEdge(m, b, 1000, 1000)}, 100, false);
  const auto chamfer = m.roundEdges(b, {uprightEdge(m, b, 0, 0)}, 100, true);
  const auto shell = m.shellSolid(b, {faceAtZ(m, b, 1000)}, 100);
  geom::setProgressIndicator(nullptr);
  ASSERT_FALSE(fillet);
  EXPECT_NE(fillet.error().find(geom::kCancelledMessage), std::string::npos) << fillet.error();
  ASSERT_FALSE(chamfer);
  EXPECT_NE(chamfer.error().find(geom::kCancelledMessage), std::string::npos) << chamfer.error();
  ASSERT_FALSE(shell);
  EXPECT_NE(shell.error().find(geom::kCancelledMessage), std::string::npos) << shell.error();
  EXPECT_EQ(m.history().undoCount(), steps);
  EXPECT_NEAR(geom::volume(world(m, b)), 1e9, 1e-3);
  // Without the cancel, it works again.
  EXPECT_TRUE(m.roundEdges(b, {uprightEdge(m, b, 1000, 1000)}, 100, false));
}
