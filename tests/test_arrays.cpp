// Copying selected edges and arrays of them (request "線の移動コピー・等間隔
// 配列・等分割", ARR-01..05): only the edges are copied, as lines, faces
// they cross are divided, copies on existing edges are not made twice;
// the same through the API.

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>

#include <set>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"

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

// The edge of `id` through `p`.
TopoDS_Edge edgeAt(const Modeler& m, const ObjectId& id, const gp_Pnt& p) {
  for (const TopoDS_Edge& e : edgesOf(m, id)) {
    BRepAdaptor_Curve c(e);
    const gp_Pnt mid = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
    if (mid.Distance(p) < 1e-6) return e;
  }
  return {};
}

// X positions of the vertical lines (Curve objects).
std::multiset<long> lineXs(const Modeler& m) {
  std::multiset<long> xs;
  for (const Object& o : m.document().objects()) {
    if (o.kind() != ObjectKind::Curve) continue;
    for (const TopoDS_Edge& e : edgesOf(m, o.id)) {
      xs.insert(std::lround(BRep_Tool::Pnt(TopExp::FirstVertex(e)).X()));
    }
  }
  return xs;
}

}  // namespace

// A01 / A02 on a lone vertical line, through the shared modeler call.
TEST(EdgeArrays, CopiesAndDivisions) {
  {
    Modeler m;
    const ObjectId line = *m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(0, 1000, 0)});
    gp_Trsf step;
    step.SetTranslation(gp_Vec(600, 0, 0));
    const std::size_t steps = m.history().undoCount();
    auto made = m.copyEdges(edgesOf(m, line), step, 5);
    ASSERT_TRUE(made.ok()) << made.error();
    EXPECT_EQ(made->size(), 5u);
    EXPECT_EQ(lineXs(m), (std::multiset<long>{0, 600, 1200, 1800, 2400, 3000}));
    EXPECT_EQ(m.history().undoCount(), steps + 1);
    ASSERT_TRUE(m.undo());
    EXPECT_EQ(lineXs(m), (std::multiset<long>{0}));
  }
  {
    Modeler m;
    const ObjectId line = *m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(0, 1000, 0)});
    gp_Trsf step;
    step.SetTranslation(gp_Vec(-3000.0 / 3, 0, 0));  // A15: the negative way
    ASSERT_TRUE(m.copyEdges(edgesOf(m, line), step, 3).ok());
    EXPECT_EQ(lineXs(m), (std::multiset<long>{-3000, -2000, -1000, 0}));
  }
}

// A07: one edge of a solid copied: the solid stays as it was, only a line
// is made. A08: one edge of a polyline: not the whole polyline.
TEST(EdgeArrays, OnlyTheSelectedEdge) {
  Modeler m;
  auto base = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(m.extrude(*base, 1000).ok());
  const double volume = geom::volume(m.document().worldShape(*base));
  const TopoDS_Edge top = edgeAt(m, *base, gp_Pnt(500, 0, 1000));
  ASSERT_FALSE(top.IsNull());
  gp_Trsf step;
  step.SetTranslation(gp_Vec(0, -2000, 0));
  auto made = m.copyEdges({top}, step, 1);
  ASSERT_TRUE(made.ok()) << made.error();
  ASSERT_EQ(made->size(), 1u);
  EXPECT_EQ(m.document().find(made->front())->kind(), ObjectKind::Curve);
  EXPECT_NEAR(geom::volume(m.document().worldShape(*base)), volume, 1e-6);
  EXPECT_EQ(m.document().objects().size(), 2u);

  const ObjectId zigzag = *m.createPolyline({gp_Pnt(5000, 0, 0), gp_Pnt(6000, 0, 0), gp_Pnt(6000, 1000, 0)});
  auto one = m.copyEdges({edgeAt(m, zigzag, gp_Pnt(5500, 0, 0))}, step, 1);
  ASSERT_TRUE(one.ok());
  ASSERT_EQ(one->size(), 1u);
  EXPECT_EQ(edgesOf(m, one->front()).size(), 1u);  // one edge, not the three-point polyline
}

// A10: the left edge of a 3000 wide face copied to the right edge and
// divided in three: two dividing lines at 1000 and 2000, three faces; the
// copy on the right edge is not made twice.
TEST(EdgeArrays, DividingAFace) {
  Modeler m;
  auto face = m.createRectangle(geom::SketchPlane::ground(), 3000, 1000);
  ASSERT_TRUE(face.ok());
  const TopoDS_Edge left = edgeAt(m, *face, gp_Pnt(0, 500, 0));
  ASSERT_FALSE(left.IsNull());
  gp_Trsf step;
  step.SetTranslation(gp_Vec(1000, 0, 0));
  auto made = m.copyEdges({left}, step, 3);
  ASSERT_TRUE(made.ok()) << made.error();
  EXPECT_TRUE(made->empty());  // all went into dividing the face / were already there
  int faces = 0;
  double area = 0;
  for (const Object& o : m.document().objects()) {
    EXPECT_NE(o.kind(), ObjectKind::Curve);
    if (o.kind() != ObjectKind::Face) continue;
    ++faces;
    area += geom::area(m.document().worldShape(o));
    EXPECT_NEAR(geom::area(m.document().worldShape(o)), 1e6, 1e-3);
  }
  EXPECT_EQ(faces, 3);
  EXPECT_NEAR(area, 3e6, 1e-3);
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(m.document().objects().size(), 1u);
}

// Arcs stay exact arcs.
TEST(EdgeArrays, ArcsStayArcs) {
  Modeler m;
  const ObjectId arc = *m.createArc(gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp::DZ(), 300);
  gp_Trsf step;
  step.SetTranslation(gp_Vec(0, 2000, 0));
  auto made = m.copyEdges(edgesOf(m, arc), step, 2);
  ASSERT_TRUE(made.ok());
  ASSERT_EQ(made->size(), 2u);
  for (const ObjectId& id : *made) {
    for (const TopoDS_Edge& e : edgesOf(m, id)) EXPECT_EQ(BRepAdaptor_Curve(e).GetType(), GeomAbs_Circle);
  }
}

// ARR-05: the API copies edges and divides a distance; invalid counts are
// refused without changing anything (A12).
TEST(EdgeArrays, Api) {
  Modeler m;
  api::CommandApi api(m);
  const ObjectId line = *m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(0, 1000, 0)});
  auto call = [&](const nlohmann::json& args) {
    return nlohmann::json::parse(api.execute(nlohmann::json{{"command", "copy_array"}, {"args", args}}.dump()));
  };
  auto r = call({{"edges", {{{"id", line}, {"edge", 1}}}}, {"vector", {3000, 0, 0}}, {"count", 3}, {"divide", true}});
  ASSERT_TRUE(r["ok"].get<bool>()) << r.dump();
  EXPECT_EQ(lineXs(m), (std::multiset<long>{0, 1000, 2000, 3000}));
  for (int bad : {0, -1, 10001}) {
    r = call({{"edges", {{{"id", line}, {"edge", 1}}}}, {"vector", {3000, 0, 0}}, {"count", bad}});
    EXPECT_FALSE(r["ok"].get<bool>());
  }
  EXPECT_EQ(lineXs(m), (std::multiset<long>{0, 1000, 2000, 3000}));
}
