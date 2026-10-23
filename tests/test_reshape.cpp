// Moving corners and edges (docs/15_reshape.md): faces stay flat or the move
// is refused, the solid stays valid, painted faces keep their material, and
// the move is a parametric step (re-editable, saved, undone).

#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepGProp.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <nlohmann/json.hpp>

#include "TestHelpers.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"

using namespace sbr;
using namespace sbr::doc;

namespace {

ObjectId cube(Modeler& m, double size = 1000) {
  auto id = m.createRectangle(geom::SketchPlane::ground(), size, size);
  EXPECT_TRUE(m.extrude(*id, size).ok());
  return *id;
}

double volume(const Modeler& m, const ObjectId& id) {
  GProp_GProps props;
  BRepGProp::VolumeProperties(m.document().worldShape(id), props);
  return props.Mass();
}

int faceCount(const TopoDS_Shape& s) {
  int n = 0;
  for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) ++n;
  return n;
}

TopoDS_Face topFace(const Modeler& m, const ObjectId& id) {
  TopoDS_Face best;
  double z = -1e300;
  for (TopExp_Explorer e(m.document().worldShape(id), TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face f = TopoDS::Face(e.Current());
    const Bnd_Box b = geom::exactBounds(f);
    if (b.CornerMax().Z() + b.CornerMin().Z() > z) {
      z = b.CornerMax().Z() + b.CornerMin().Z();
      best = f;
    }
  }
  return best;
}

}  // namespace

TEST(Reshape, LiftingAnEdgeMakesARoofSlope) {
  Modeler m;
  const ObjectId box = cube(m);
  const std::size_t steps = m.history().undoCount();
  // The front top edge goes up 500: the top tilts, the end faces stay flat.
  ASSERT_TRUE(m.moveVertices(box, {gp_Pnt(0, 0, 1000), gp_Pnt(1000, 0, 1000)}, gp_Vec(0, 0, 500)).ok());
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  const TopoDS_Shape s = m.document().worldShape(box);
  EXPECT_TRUE(BRepCheck_Analyzer(s).IsValid());
  EXPECT_EQ(faceCount(s), 6);
  EXPECT_NEAR(volume(m, box), 1e9 + 0.5 * 1000 * 1000 * 500, 1e-3);
  EXPECT_EQ(m.document().find(box)->kind(), ObjectKind::Solid);
  test::expectBounds(s, 0, 0, 0, 1000, 1000, 1500);

  // The move is a step of the recipe: its distance can be re-edited.
  const auto params = m.parameters(box);
  ASSERT_FALSE(params.empty());
  const Parameter& last = params.back();
  ASSERT_TRUE(m.setParameter(box, last.key, 200).ok());
  EXPECT_NEAR(volume(m, box), 1e9 + 0.5 * 1000 * 1000 * 200, 1e-3);

  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  EXPECT_NEAR(volume(m, box), 1e9, 1e-3);
  ASSERT_TRUE(m.redo());
  EXPECT_NEAR(volume(m, box), 1.25e9, 1e-3);
}

TEST(Reshape, MovesThatWouldBendOrFoldAreRefused) {
  Modeler m;
  const ObjectId box = cube(m);
  const std::size_t steps = m.history().undoCount();
  // One corner up: three faces would bend.
  const Status corner = m.moveVertices(box, {gp_Pnt(0, 0, 1000)}, gp_Vec(0, 0, 300));
  EXPECT_FALSE(corner.ok());
  EXPECT_NE(corner.message().find("no longer be flat"), std::string::npos) << corner.message();
  // The top edge pushed through the bottom.
  EXPECT_FALSE(m.moveVertices(box, {gp_Pnt(0, 0, 1000), gp_Pnt(1000, 0, 1000)}, gp_Vec(0, 0, -1500)).ok());
  // Not a corner.
  EXPECT_FALSE(m.moveVertices(box, {gp_Pnt(500, 0, 1000)}, gp_Vec(0, 0, 100)).ok());
  EXPECT_EQ(m.history().undoCount(), steps);
  EXPECT_NEAR(volume(m, box), 1e9, 1e-3);
}

TEST(Reshape, ShearTheTopFaceAndKeepItsPaint) {
  Modeler m;
  const ObjectId box = cube(m);
  auto red = m.createMaterial("Red", {1, 0, 0});
  ASSERT_TRUE(red.ok());
  ASSERT_TRUE(m.paintFaces(box, {topFace(m, box)}, *red).ok());
  const std::vector<gp_Pnt> top{gp_Pnt(0, 0, 1000), gp_Pnt(1000, 0, 1000), gp_Pnt(1000, 1000, 1000),
                                gp_Pnt(0, 1000, 1000)};
  ASSERT_TRUE(m.moveVertices(box, top, gp_Vec(300, 0, 0)).ok());
  EXPECT_NEAR(volume(m, box), 1e9, 1e-3);  // a parallelepiped
  test::expectBounds(m.document().worldShape(box), 0, 0, 0, 1300, 1000, 1000);
  EXPECT_EQ(m.materialAt(box, topFace(m, box))->material, *red);

  // After the shear, lifting only the front edge would bend the side faces.
  EXPECT_FALSE(m.moveVertices(box, {gp_Pnt(300, 0, 1000), gp_Pnt(1300, 0, 1000)}, gp_Vec(0, 0, 400)).ok());

  // A tilted face keeps its paint too.
  const ObjectId other = cube(m);
  ASSERT_TRUE(m.move({other}, gp_Vec(3000, 0, 0)).ok());
  ASSERT_TRUE(m.paintFaces(other, {topFace(m, other)}, *red).ok());
  ASSERT_TRUE(m.moveVertices(other, {gp_Pnt(3000, 0, 1000), gp_Pnt(4000, 0, 1000)}, gp_Vec(0, 0, 400)).ok());
  EXPECT_EQ(m.materialAt(other, topFace(m, other))->material, *red);
}

TEST(Reshape, MovedObjectsAndFlatProfiles) {
  Modeler m;
  const ObjectId box = cube(m);
  ASSERT_TRUE(m.move({box}, gp_Vec(5000, 0, 0)).ok());  // world points, local recipe
  ASSERT_TRUE(m.moveVertices(box, {gp_Pnt(5000, 0, 1000), gp_Pnt(6000, 0, 1000)}, gp_Vec(0, 0, 500)).ok());
  test::expectBounds(m.document().worldShape(box), 5000, 0, 0, 6000, 1000, 1500);

  // A flat face: a corner moves within its plane, not out of it.
  auto face = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(face.ok());
  ASSERT_TRUE(m.moveVertices(*face, {gp_Pnt(1000, 1000, 0)}, gp_Vec(500, 0, 0)).ok());
  EXPECT_NEAR(geom::area(m.document().worldShape(*face)), 1000.0 * 1000 + 0.5 * 500 * 1000, 1e-3);
  EXPECT_FALSE(m.moveVertices(*face, {gp_Pnt(0, 0, 0)}, gp_Vec(0, 0, 100)).ok());
}

TEST(Reshape, GeometryOperationAlone) {
  // Curved faces are refused rather than faceted.
  Modeler m;
  auto c = m.createCircle(geom::SketchPlane::ground(), 500);
  ASSERT_TRUE(m.extrude(*c, 1000).ok());
  EXPECT_FALSE(m.moveVertices(*c, {gp_Pnt(500, 0, 1000)}, gp_Vec(0, 0, 100)).ok());
}

TEST(Reshape, SurvivesSaveReloadAndLaterEdits) {
  Modeler m;
  const ObjectId box = cube(m);
  ASSERT_TRUE(m.moveVertices(box, {gp_Pnt(0, 0, 1000), gp_Pnt(1000, 0, 1000)}, gp_Vec(0, 0, 500)).ok());
  // Push/pull the sloped top afterwards.
  auto pushed = m.pushPull(box, topFace(m, box), 100);
  ASSERT_TRUE(pushed.ok()) << pushed.error();
  const double v = volume(m, box);
  EXPECT_GT(v, 1.25e9);

  const std::string path = test::tempPath("reshape.sbr");
  ASSERT_TRUE(m.save(path).ok());
  Modeler loaded;
  std::vector<std::string> warnings;
  ASSERT_TRUE(loaded.load(path, &warnings, true).ok());
  EXPECT_TRUE(warnings.empty()) << warnings.front();
  EXPECT_NEAR(volume(loaded, box), v, 1e-3);

  // A flat face reshaped, then pulled up into a solid.
  auto face = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(m.move({*face}, gp_Vec(0, 3000, 0)).ok());
  ASSERT_TRUE(m.moveVertices(*face, {gp_Pnt(1000, 4000, 0)}, gp_Vec(500, 0, 0)).ok());
  auto solid = m.pushPull(*face, TopoDS::Face(m.document().worldShape(*face)), 1000);
  ASSERT_TRUE(solid.ok()) << solid.error();
  EXPECT_NEAR(volume(m, *face), 1.25e9, 1e-3);
}

TEST(Reshape, AiCommand) {
  using json = nlohmann::json;
  Modeler m;
  const ObjectId box = cube(m);
  api::CommandApi api(m);
  auto call = [&](const json& args) {
    return json::parse(api.execute(json{{"command", "move_vertices"}, {"args", args}}.dump()));
  };
  // The front top edge by a point on it.
  json r = call({{"id", box}, {"edge", {{"near", {500, 0, 1000}}}}, {"delta", {0, 0, 500}}});
  EXPECT_TRUE(r.value("ok", false)) << r.dump();
  EXPECT_NEAR(volume(m, box), 1.25e9, 1e-3);
  // A single corner would bend faces: refused, nothing changes.
  r = call({{"id", box}, {"points", json::array({json::array({1000, 1000, 1000})})}, {"delta", {0, 0, 100}}});
  EXPECT_FALSE(r.value("ok", true));
  EXPECT_NEAR(volume(m, box), 1.25e9, 1e-3);
}

TEST(HiddenEdges, HideKeepsFacesAndSurvivesEdits) {
  Modeler m;
  const ObjectId box = cube(m);
  // A vertical corner edge of the box.
  TopoDS_Edge vertical;
  for (TopExp_Explorer e(m.document().worldShape(box), TopAbs_EDGE); e.More(); e.Next()) {
    const Bnd_Box b = geom::exactBounds(e.Current());
    if (b.CornerMax().X() < 1 && b.CornerMax().Y() < 1 && b.CornerMax().Z() - b.CornerMin().Z() > 999) {
      vertical = TopoDS::Edge(e.Current());
    }
  }
  ASSERT_FALSE(vertical.IsNull());
  const int faces = faceCount(m.document().worldShape(box));
  ASSERT_TRUE(m.setEdgesHidden(box, {vertical}, true).ok());
  EXPECT_EQ(hiddenEdgeIndices(*m.document().find(box)).size(), 1u);
  EXPECT_EQ(faceCount(m.document().worldShape(box)), faces);  // display only

  // The edge grows with the box: still hidden.
  ASSERT_TRUE(m.setParameter(box, "op0.distance", 1500).ok());
  EXPECT_EQ(hiddenEdgeIndices(*m.document().find(box)).size(), 1u);
  // Scaled with the object, saved and reloaded.
  ASSERT_TRUE(m.scale({box}, gp_Pnt(0, 0, 0), 2.0).ok());
  EXPECT_EQ(hiddenEdgeIndices(*m.document().find(box)).size(), 1u);
  const std::string path = test::tempPath("hidden_edges.sbr");
  ASSERT_TRUE(m.save(path).ok());
  Modeler loaded;
  ASSERT_TRUE(loaded.load(path).ok());
  EXPECT_EQ(hiddenEdgeIndices(*loaded.document().find(box)).size(), 1u);

  // Show again: by the edge, or everything; undo brings it back.
  ASSERT_TRUE(m.showHiddenEdges({box}).ok());
  EXPECT_TRUE(m.document().find(box)->hiddenEdges.empty());
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(hiddenEdgeIndices(*m.document().find(box)).size(), 1u);
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(m.document().worldShape(box), TopAbs_EDGE, edges);
  const int index = hiddenEdgeIndices(*m.document().find(box)).front();
  ASSERT_TRUE(m.setEdgesHidden(box, {TopoDS::Edge(edges(index))}, false).ok());
  EXPECT_TRUE(m.document().find(box)->hiddenEdges.empty());
}

// Openings go through a wall's thickness: from a slab's edge the cut would
// tunnel through all of it, and no opening may split its host.
TEST(Openings, RefusedOnSlabEdgesAndWhenSplitting) {
  Modeler m;
  auto slab = m.createSlab({gp_Pnt(0, 0, 0), gp_Pnt(4000, 0, 0), gp_Pnt(4000, 3000, 0), gp_Pnt(0, 3000, 0)}, 0, 200);
  ASSERT_TRUE(slab.ok());
  const std::size_t steps = m.history().undoCount();
  for (const auto& [h, sill] : std::vector<std::pair<double, double>>{{100, 0}, {200, 0}, {100, 50}}) {
    const auto r = m.placeOpeningAt(*slab, gp_Dir(0, -1, 0), gp_Pnt(2000, 0, -100), 900, h, sill);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.error().find("edge of a slab"), std::string::npos) << r.error();
  }
  EXPECT_EQ(m.history().undoCount(), steps);
  EXPECT_NEAR(volume(m, *slab), 4000.0 * 3000 * 200, 1e-3);

  // A block (no slab attribute) deeper than it is tall: refused the same way.
  auto block = m.createRectangle(geom::SketchPlane::ground(), 4000, 3000);
  ASSERT_TRUE(m.extrude(*block, 200).ok());
  EXPECT_FALSE(m.placeOpeningAt(*block, gp_Dir(0, -1, 0), gp_Pnt(2000, 0, 100), 900, 100, 0).ok());

  // A real wall still takes its opening, in one piece.
  auto walls = m.createWalls({gp_Pnt(0, 5000, 0), gp_Pnt(4000, 5000, 0)}, 200, 2500);
  ASSERT_TRUE(walls.ok());
  const auto cut = m.placeOpeningAt(walls->front(), gp_Dir(0, -1, 0), gp_Pnt(2000, 4900, 1000), 900, 2100, 0);
  ASSERT_TRUE(cut.ok()) << cut.error();
  int solids = 0;
  for (TopExp_Explorer e(m.document().worldShape(*cut), TopAbs_SOLID); e.More(); e.Next()) ++solids;
  EXPECT_EQ(solids, 1);

  // Full height: the wall would fall into two pieces; refused, nothing changes.
  const std::size_t before = m.history().undoCount();
  const auto split = m.placeOpeningAt(*cut, gp_Dir(0, -1, 0), gp_Pnt(800, 4900, 1000), 600, 2500, 0);
  EXPECT_FALSE(split.ok());
  EXPECT_NE(split.error().find("in two"), std::string::npos) << split.error();
  EXPECT_EQ(m.history().undoCount(), before);
  EXPECT_TRUE(m.document().find(*cut));
}

// SketchUp: lines that cross close areas into faces, not only end to end.
namespace {
int countKind(const Modeler& m, ObjectKind kind) {
  int n = 0;
  for (const Object& o : m.document().objects()) n += o.kind() == kind;
  return n;
}
}  // namespace

TEST(CrossingLines, HashMakesTheMiddleSquareAndKeepsTails) {
  Modeler m;
  ASSERT_TRUE(m.createPolyline({gp_Pnt(-500, 0, 0), gp_Pnt(1500, 0, 0)}).ok());
  ASSERT_TRUE(m.createPolyline({gp_Pnt(-500, 1000, 0), gp_Pnt(1500, 1000, 0)}).ok());
  ASSERT_TRUE(m.createPolyline({gp_Pnt(0, -500, 0), gp_Pnt(0, 1500, 0)}).ok());
  EXPECT_FALSE(m.closesByCrossing({gp_Pnt(-1000, 2000, 0), gp_Pnt(2000, 2000, 0)}));  // crosses nothing
  EXPECT_TRUE(m.closesByCrossing({gp_Pnt(1000, -500, 0), gp_Pnt(1000, 1500, 0)}));
  auto last = m.createPolyline({gp_Pnt(1000, -500, 0), gp_Pnt(1000, 1500, 0)});
  ASSERT_TRUE(last.ok());
  const std::size_t steps = m.history().undoCount();
  auto faces = m.facesFromCrossings(*last);
  ASSERT_TRUE(faces.ok()) << faces.error();
  ASSERT_EQ(faces->size(), 1u);
  EXPECT_NEAR(geom::area(m.document().worldShape(faces->front())), 1e6, 1e-3);
  // Normal up, like faces drawn with the rectangle tool.
  const auto n = geom::planarFaceNormal(TopoDS::Face(m.document().worldShape(faces->front())));
  ASSERT_TRUE(n);
  EXPECT_GT(n->Z(), 0.99);
  EXPECT_EQ(countKind(m, ObjectKind::Curve), 8);  // two tails per line
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(countKind(m, ObjectKind::Curve), 4);
  EXPECT_EQ(countKind(m, ObjectKind::Face), 0);
}

TEST(CrossingLines, ChainCrossingItselfAndThreeSeparateLines) {
  Modeler m;
  // A chain that comes back across its own first segment.
  auto chain = m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp_Pnt(1000, 1000, 0), gp_Pnt(500, 1000, 0),
                                 gp_Pnt(500, -500, 0)});
  ASSERT_TRUE(chain.ok());
  auto faces = m.facesFromCrossings(*chain);
  ASSERT_TRUE(faces.ok()) << faces.error();
  ASSERT_EQ(faces->size(), 1u);
  EXPECT_NEAR(geom::area(m.document().worldShape(faces->front())), 500.0 * 1000, 1e-3);
  EXPECT_EQ(countKind(m, ObjectKind::Curve), 2);  // the two tails

  // Three lines, each crossing the others: a triangle.
  Modeler t;
  ASSERT_TRUE(t.createPolyline({gp_Pnt(5000, 0, 0), gp_Pnt(7000, 0, 0)}).ok());
  ASSERT_TRUE(t.createPolyline({gp_Pnt(5500, -500, 0), gp_Pnt(5500, 1500, 0)}).ok());
  auto third = t.createPolyline({gp_Pnt(6700, -200, 0), gp_Pnt(5300, 1200, 0)});
  ASSERT_TRUE(third.ok());
  auto tri = t.facesFromCrossings(*third);
  ASSERT_TRUE(tri.ok()) << tri.error();
  ASSERT_EQ(tri->size(), 1u);
  EXPECT_NEAR(geom::area(t.document().worldShape(tri->front())), 0.5 * 1000 * 1000, 1e-3);

  // Lines that do not close anything: nothing changes.
  Modeler n;
  ASSERT_TRUE(n.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0)}).ok());
  auto x = n.createPolyline({gp_Pnt(500, -500, 0), gp_Pnt(500, 500, 0)});
  const std::size_t steps = n.history().undoCount();
  auto none = n.facesFromCrossings(*x);
  ASSERT_TRUE(none.ok());
  EXPECT_TRUE(none->empty());
  EXPECT_EQ(n.history().undoCount(), steps);
  EXPECT_EQ(countKind(n, ObjectKind::Curve), 2);
}

// The eraser works on lines (SketchUp): docs/15_reshape.md.
namespace {
TopoDS_Edge edgeNear(const Modeler& m, const ObjectId& id, const gp_Pnt& p) {
  TopoDS_Edge best;
  double d = 1e300;
  for (TopExp_Explorer e(m.document().worldShape(id), TopAbs_EDGE); e.More(); e.Next()) {
    BRepExtrema_DistShapeShape dist(BRepBuilderAPI_MakeVertex(p).Vertex(), e.Current());
    if (dist.IsDone() && dist.Value() < d) {
      d = dist.Value();
      best = TopoDS::Edge(e.Current());
    }
  }
  return best;
}
}  // namespace

TEST(EraseLines, RoundedCornerFromAnLAndAnArc) {
  Modeler m;
  auto l = m.createPolyline({gp_Pnt(0, 1000, 0), gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0)});
  ASSERT_TRUE(l.ok());
  // Fillet arc (radius 300) touching both legs.
  auto arc = m.createArc(gp_Pnt(0, 300, 0), gp_Pnt(300, 0, 0), gp::DZ(), -(300 - 300 / std::sqrt(2.0)));
  ASSERT_TRUE(arc.ok()) << arc.error();
  // The corner part of the vertical leg: from the corner to where the arc ends.
  auto first = m.eraseLinePiece(*l, edgeNear(m, *l, gp_Pnt(0, 100, 0)), gp_Pnt(0, 100, 0));
  ASSERT_TRUE(first.ok()) << first.error();
  ASSERT_EQ(first->size(), 2u);  // the leg above the arc, and the other leg
  ObjectId other;
  for (const ObjectId& id : *first) {
    if (geom::exactBounds(m.document().worldShape(id)).CornerMax().X() > 500) other = id;
  }
  ASSERT_FALSE(other.empty());
  auto second = m.eraseLinePiece(other, edgeNear(m, other, gp_Pnt(100, 0, 0)), gp_Pnt(100, 0, 0));
  ASSERT_TRUE(second.ok()) << second.error();
  // Left: the leg (0,300)-(0,1000), the arc, the leg (300,0)-(1000,0).
  int curves = 0;
  double length = 0;
  for (const Object& o : m.document().objects()) {
    if (o.kind() != ObjectKind::Curve) continue;
    ++curves;
    GProp_GProps props;
    BRepGProp::LinearProperties(m.document().worldShape(o), props);
    length += props.Mass();
  }
  EXPECT_EQ(curves, 3);
  EXPECT_NEAR(length, 700 + 700 + M_PI / 2 * 300, 1e-3);
  // Two undo steps bring the L back.
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  EXPECT_TRUE(m.document().find(*l));
}

TEST(EraseLines, FaceOutlineEdgeLeavesTheOtherSides) {
  Modeler m;
  auto rect = m.createRectangle(geom::SketchPlane::ground(), 1000, 500);
  ASSERT_TRUE(rect.ok());
  auto left = m.eraseLinePiece(*rect, edgeNear(m, *rect, gp_Pnt(500, 0, 0)), gp_Pnt(500, 0, 0));
  ASSERT_TRUE(left.ok()) << left.error();
  EXPECT_FALSE(m.document().find(*rect));
  ASSERT_EQ(left->size(), 1u);  // one line round the three other sides
  GProp_GProps props;
  BRepGProp::LinearProperties(m.document().worldShape(left->front()), props);
  EXPECT_NEAR(props.Mass(), 500 + 1000 + 500, 1e-3);

  // An arc segment face: erasing its chord leaves the arc.
  auto seg = m.createArc(gp_Pnt(3000, 0, 0), gp_Pnt(4000, 0, 0), gp::DZ(), 300, true);
  ASSERT_TRUE(seg.ok());
  auto arcLeft = m.eraseLinePiece(*seg, edgeNear(m, *seg, gp_Pnt(3500, 0, 0)), gp_Pnt(3500, 0, 0));
  ASSERT_TRUE(arcLeft.ok()) << arcLeft.error();
  ASSERT_EQ(arcLeft->size(), 1u);
  EXPECT_TRUE(std::holds_alternative<ArcCurve>(m.document().find(arcLeft->front())->recipe.base));
}

TEST(EraseLines, CornerEdgeOfABoxTakesItsTwoFaces) {
  Modeler m;
  const ObjectId box = cube(m);
  const TopoDS_Edge corner = edgeNear(m, box, gp_Pnt(500, 0, 1000));  // front top edge
  ASSERT_EQ(m.facesAtEdge(box, corner).size(), 2u);
  ASSERT_TRUE(m.eraseCornerEdge(box, corner).ok());
  EXPECT_EQ(m.document().find(box)->kind(), ObjectKind::Shell);
  EXPECT_EQ(faceCount(m.document().worldShape(box)), 4);
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(m.document().find(box)->kind(), ObjectKind::Solid);
}
