// Follow Me along edges selected first (SketchUp, request T3 / FM-01..02):
// lines and arcs of several objects are joined into one path whatever their
// order and direction; branches, gaps and duplicates are refused with the
// reason, and a refusal leaves profile and path as they were.

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Freeform.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Validation.h"

using namespace sbr;
using namespace sbr::doc;

namespace {

std::vector<TopoDS_Edge> edgesOf(const Modeler& m, const ObjectId& id) {
  TopTools_IndexedMapOfShape all;
  TopExp::MapShapes(m.document().worldShape(*m.document().find(id)), TopAbs_EDGE, all);
  std::vector<TopoDS_Edge> out;
  for (int i = 1; i <= all.Extent(); ++i) out.push_back(TopoDS::Edge(all(i)));
  return out;
}

// A 100 x 100 profile standing at the origin, facing +X.
ObjectId profileAtOrigin(Modeler& m) {
  auto p = m.createRectangle(geom::SketchPlane::fromNormal(gp_Pnt(0, -50, 0), gp::DX()), 100, 100);
  EXPECT_TRUE(p.ok());
  return *p;
}

struct LPath {
  ObjectId a, b, c;  // line, line, arc: three separate objects
};

// (0,0,0) -> (2000,0,0) -> (2000,2000,0), then a half circle to (2000,4000,0).
LPath lShapedPath(Modeler& m) {
  LPath p;
  p.a = *m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0)});
  p.b = *m.createPolyline({gp_Pnt(2000, 0, 0), gp_Pnt(2000, 2000, 0)});
  p.c = *m.createArc(gp_Pnt(2000, 2000, 0), gp_Pnt(2000, 4000, 0), gp::DZ(), 1000);
  return p;
}

}  // namespace

TEST(FollowMeEdges, PathJoinsInAnyOrder) {
  Modeler m;
  const LPath p = lShapedPath(m);
  std::vector<TopoDS_Edge> edges;
  for (const ObjectId& id : {p.c, p.a, p.b}) {  // not in path order
    for (const TopoDS_Edge& e : edgesOf(m, id)) edges.push_back(TopoDS::Edge(e.Reversed()));
  }
  auto wire = geom::pathFromEdges(edges, gp_Pnt(0, 0, 0));
  ASSERT_TRUE(wire.ok()) << wire.error();
  TopoDS_Vertex first, last;
  TopExp::Vertices(wire.value(), first, last);
  EXPECT_LT(BRep_Tool::Pnt(first).Distance(gp_Pnt(0, 0, 0)), 1e-6);  // starts at the profile's end
  EXPECT_LT(BRep_Tool::Pnt(last).Distance(gp_Pnt(2000, 4000, 0)), 1e-6);
  // The arc stays an exact circle in the path.
  bool circle = false;
  for (TopExp_Explorer e(wire.value(), TopAbs_EDGE); e.More(); e.Next()) {
    circle |= BRepAdaptor_Curve(TopoDS::Edge(e.Current())).GetType() == GeomAbs_Circle;
  }
  EXPECT_TRUE(circle);
}

TEST(FollowMeEdges, RefusesBranchesGapsAndDuplicates) {
  Modeler m;
  const ObjectId a = *m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0)});
  const ObjectId b = *m.createPolyline({gp_Pnt(1000, 0, 0), gp_Pnt(2000, 0, 0)});
  const ObjectId c = *m.createPolyline({gp_Pnt(1000, 0, 0), gp_Pnt(1000, 1000, 0)});
  const ObjectId far = *m.createPolyline({gp_Pnt(2010, 0, 0), gp_Pnt(3000, 0, 0)});  // 10 mm gap
  auto edges = [&](std::initializer_list<ObjectId> ids) {
    std::vector<TopoDS_Edge> out;
    for (const ObjectId& id : ids) {
      for (const TopoDS_Edge& e : edgesOf(m, id)) out.push_back(e);
    }
    return out;
  };
  auto branch = geom::pathFromEdges(edges({a, b, c}), gp_Pnt());
  ASSERT_FALSE(branch.ok());
  EXPECT_NE(branch.error().find("branch"), std::string::npos) << branch.error();
  auto gap = geom::pathFromEdges(edges({a, b, far}), gp_Pnt());
  ASSERT_FALSE(gap.ok());
  EXPECT_NE(gap.error().find("not connected"), std::string::npos) << gap.error();
  auto twice = geom::pathFromEdges(edges({a, a, b}), gp_Pnt());
  ASSERT_FALSE(twice.ok());
  EXPECT_NE(twice.error().find("twice"), std::string::npos) << twice.error();

  // Refused in the modeler: nothing changes.
  const ObjectId profile = profileAtOrigin(m);
  const std::size_t objects = m.document().objects().size();
  const std::size_t steps = m.history().undoCount();
  EXPECT_FALSE(m.followMe(profile, edges({a, b, c})).ok());
  EXPECT_EQ(m.document().objects().size(), objects);
  EXPECT_EQ(m.history().undoCount(), steps);
}

// Lines and an arc of three objects: one solid along all of them, the same
// whichever order the edges come in; one undo step brings the profile back.
TEST(FollowMeEdges, SweepsAlongSeveralObjects) {
  double first = 0;
  for (bool reversed : {false, true}) {
    Modeler m;
    const LPath p = lShapedPath(m);
    const ObjectId profile = profileAtOrigin(m);
    std::vector<TopoDS_Edge> edges;
    std::vector<ObjectId> order = {p.a, p.b, p.c};
    if (reversed) order = {p.c, p.b, p.a};
    for (const ObjectId& id : order) {
      for (const TopoDS_Edge& e : edgesOf(m, id)) edges.push_back(e);
    }
    const std::size_t steps = m.history().undoCount();
    auto swept = m.followMe(profile, edges);
    ASSERT_TRUE(swept.ok()) << swept.error();
    EXPECT_EQ(m.history().undoCount(), steps + 1);
    const TopoDS_Shape shape = m.document().worldShape(*m.document().find(*swept));
    EXPECT_TRUE(geom::validateShape(shape, true).valid);
    const double volume = geom::volume(shape);
    // Cross-section x centre line (2000 + 2000 + half circle of r 1000).
    EXPECT_NEAR(volume, 100.0 * 100 * (4000 + M_PI * 1000), 0.02 * volume);
    if (!reversed) first = volume;
    else EXPECT_NEAR(volume, first, 1e-6 * first);
    EXPECT_FALSE(m.document().find(profile));
    ASSERT_TRUE(m.undo());
    EXPECT_TRUE(m.document().find(profile));
  }
}

// A closed path (the outline of another face, picked as its edges).
TEST(FollowMeEdges, ClosedLoopOfAFace) {
  Modeler m;
  geom::SketchPlane raised = geom::SketchPlane::ground();
  auto outline = m.createRectangle(raised, 2000, 1000);
  const ObjectId profile = profileAtOrigin(m);
  auto swept = m.followMe(profile, edgesOf(m, *outline));
  ASSERT_TRUE(swept.ok()) << swept.error();
  EXPECT_TRUE(geom::validateShape(m.document().worldShape(*m.document().find(*swept)), true).valid);
}

TEST(FollowMeEdges, ApiPathEdges) {
  Modeler m;
  api::CommandApi api(m);
  const LPath p = lShapedPath(m);
  const ObjectId profile = profileAtOrigin(m);
  const nlohmann::json args = {{"profile", profile},
                               {"path_edges", {{{"id", p.b}}, {{"id", p.c}}, {{"id", p.a}, {"edge", 1}}}}};
  const auto r = nlohmann::json::parse(api.execute(nlohmann::json{{"command", "follow_me"}, {"args", args}}.dump()));
  ASSERT_TRUE(r["ok"].get<bool>()) << r.dump();
  EXPECT_TRUE(m.document().find(r["result"]["id"].get<std::string>()));
}
