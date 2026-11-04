// Lines running across faces divide them (report: a horizontal line drawn
// or copied over a wall split into vertical strips only lay on top of it):
// a line may start and end outside the faces and run across several faces
// side by side; each part running across a face from edge to edge divides
// that face, the rest stays a line. Lines stopping inside a face, a little
// off its plane, piercing it, or in another group divide nothing.

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>

#include <gtest/gtest.h>
#include <chrono>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"
#include "sbr/geom/Validation.h"

using namespace sbr;
using namespace sbr::doc;

namespace {

geom::SketchPlane at(double x, double y, double z = 0) {
  geom::SketchPlane p = geom::SketchPlane::ground();
  p.origin = gp_Pnt(x, y, z);
  return p;
}

int faces(const Modeler& m, const ObjectId& id) {
  int n = 0;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(id)), TopAbs_FACE); e.More(); e.Next()) ++n;
  return n;
}

int count(const Modeler& m, ObjectKind k) {
  int n = 0;
  for (const Object& o : m.document().objects()) n += o.kind() == k;
  return n;
}

double volumeOf(const Modeler& m, const ObjectId& id) { return geom::volume(m.document().worldShape(*m.document().find(id))); }

struct Api {
  Modeler& m;
  api::CommandApi api{m};
  nlohmann::json line(std::vector<std::array<double, 3>> pts) {
    nlohmann::json points = nlohmann::json::array();
    for (const auto& p : pts) points.push_back({p[0], p[1], p[2]});
    return nlohmann::json::parse(
        api.execute(nlohmann::json{{"command", "create_line"}, {"args", {{"points", points}}}}.dump()));
  }
};

// A box 3000 x 1000 x 3000 whose front (y = 0) is in three strips.
ObjectId stripedBox(Modeler& m) {
  auto base = m.createRectangle(at(0, 0), 3000, 1000);
  EXPECT_TRUE(m.extrude(*base, 3000).ok());
  EXPECT_TRUE(m.splitWithLine({gp_Pnt(1000, 0, 0), gp_Pnt(1000, 0, 3000)}).ok());
  EXPECT_TRUE(m.splitWithLine({gp_Pnt(2000, 0, 0), gp_Pnt(2000, 0, 3000)}).ok());
  EXPECT_EQ(faces(m, *base), 8);
  return *base;
}

TopoDS_Face frontFaceAt(const Modeler& m, const ObjectId& id, const gp_Pnt& p) {
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(id)), TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face f = TopoDS::Face(e.Current());
    if (geom::pointOnFace(f, p) && !geom::pointOnFaceBoundary(f, p)) return f;
  }
  return {};
}

}  // namespace

// A square face crossed by a line sticking out at both ends: two faces; the
// parts outside stay lines.
TEST(SplitAcross, LongerThanTheFace) {
  Modeler m;
  Api a{m};
  auto face = m.createRectangle(at(0, 0), 2000, 1000);
  ASSERT_TRUE(face.ok());
  const std::size_t steps = m.history().undoCount();
  const auto r = a.line({{-500, 400, 0}, {2500, 400, 0}});
  ASSERT_TRUE(r["ok"].get<bool>()) << r.dump();
  EXPECT_EQ(count(m, ObjectKind::Face), 2);
  EXPECT_EQ(count(m, ObjectKind::Curve), 2);  // the two ends outside
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(count(m, ObjectKind::Face), 1);
  EXPECT_EQ(count(m, ObjectKind::Curve), 0);
  ASSERT_TRUE(m.redo());
  EXPECT_EQ(count(m, ObjectKind::Face), 2);
}

// The report: one line across all three strips divides each of them; the
// verticals and the new line meet at real vertices; each part is pushed on
// its own; undo / redo / saved and reloaded.
TEST(SplitAcross, AcrossSeveralFacesSideBySide) {
  Modeler m;
  Api a{m};
  const ObjectId box = stripedBox(m);
  const std::size_t steps = m.history().undoCount();
  const auto r = a.line({{-300, 0, 1000}, {3300, 0, 1000}});
  ASSERT_TRUE(r["ok"].get<bool>()) << r.dump();
  EXPECT_EQ(faces(m, box), 11);  // each strip in two
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  EXPECT_NEAR(volumeOf(m, box), 9e9, 1.0);
  EXPECT_TRUE(geom::validateShape(m.document().worldShape(*m.document().find(box)), true).valid);
  // Push the lower middle part out alone.
  const TopoDS_Face part = frontFaceAt(m, box, gp_Pnt(1500, 0, 500));
  ASSERT_FALSE(part.IsNull());
  EXPECT_NEAR(geom::area(part), 1e6, 1e-3);
  ASSERT_TRUE(m.pushPull(box, part, 300).ok());
  EXPECT_NEAR(volumeOf(m, box), 9e9 + 1e6 * 300, 1.0);
  ASSERT_TRUE(m.undo());
  // In the file, and back.
  const std::string path = ::testing::TempDir() + "split_across.sbr";
  ASSERT_TRUE(m.save(path).ok());
  Modeler back;
  ASSERT_TRUE(back.load(path).ok());
  EXPECT_EQ(faces(back, box), 11);
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(faces(m, box), 8);
  ASSERT_TRUE(m.redo());
  EXPECT_EQ(faces(m, box), 11);
}

// Copies of a line (an array) each divide every strip they run across.
TEST(SplitAcross, CopiedLinesDivideEachRow) {
  Modeler m;
  Api a{m};
  const ObjectId box = stripedBox(m);
  ASSERT_TRUE(a.line({{-300, 0, 500}, {3300, 0, 500}})["ok"].get<bool>());
  EXPECT_EQ(faces(m, box), 11);  // the drawn one divides already
  // The middle strip's new edge at z = 500, copied up.
  TopoDS_Edge edge;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(box)), TopAbs_EDGE); e.More(); e.Next()) {
    BRepAdaptor_Curve c(TopoDS::Edge(e.Current()));
    if (c.Value(0.5 * (c.FirstParameter() + c.LastParameter())).Distance(gp_Pnt(1500, 0, 500)) < 1e-6) {
      edge = TopoDS::Edge(e.Current());
    }
  }
  ASSERT_FALSE(edge.IsNull());
  gp_Trsf up;
  up.SetTranslation(gp_Vec(0, 0, 500));
  ASSERT_TRUE(m.copyEdges({edge}, up, 4).ok());  // z = 1000, 1500, 2000, 2500
  // The middle strip's copied edge spans only it: the middle in 6 rows.
  int middle = 0;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(box)), TopAbs_FACE); e.More(); e.Next()) {
    const Bnd_Box b = geom::exactBounds(e.Current());
    if (std::abs(b.CornerMin().Y()) < 1e-6 && std::abs(b.CornerMax().Y()) < 1e-6 &&
        std::abs(b.CornerMin().X() - 1000) < 1e-6 && std::abs(b.CornerMax().X() - 2000) < 1e-6) {
      ++middle;
    }
  }
  EXPECT_EQ(middle, 6);
  EXPECT_TRUE(geom::validateShape(m.document().worldShape(*m.document().find(box)), true).valid);
}

// What must not divide: a line stopping inside the face, one 0.5 mm in
// front of it, one piercing it, one in another group.
TEST(SplitAcross, NotDividing) {
  Modeler m;
  Api a{m};
  const ObjectId box = stripedBox(m);
  ASSERT_TRUE(a.line({{-300, 0, 1000}, {1500, 0, 1000}})["ok"].get<bool>());  // stops in the middle strip
  EXPECT_EQ(faces(m, box), 9);  // only the first strip: the part in the middle strip is not extended
  ASSERT_TRUE(a.line({{-300, -0.5, 2000}, {3300, -0.5, 2000}})["ok"].get<bool>());  // a little in front
  EXPECT_EQ(faces(m, box), 9);
  ASSERT_TRUE(a.line({{500, -500, 2500}, {500, 500, 2500}})["ok"].get<bool>());  // through the face
  EXPECT_EQ(faces(m, box), 9);
  auto inner = m.createRectangle(at(5000, 0), 1000, 1000);
  auto group = m.group({*inner});
  ASSERT_TRUE(group.ok());
  ASSERT_TRUE(a.line({{4800, 500, 0}, {6200, 500, 0}})["ok"].get<bool>());  // over a face in a group
  EXPECT_EQ(faces(m, *inner), 1);
}

// Lines first, the face after: the face is divided by them.
TEST(SplitAcross, FaceDrawnOverLines) {
  Modeler m;
  ASSERT_TRUE(m.createPolyline({gp_Pnt(-300, 400, 0), gp_Pnt(2300, 400, 0)}).ok());
  auto face = m.createRectangle(at(0, 0), 2000, 1000);
  ASSERT_TRUE(face.ok());
  EXPECT_EQ(count(m, ObjectKind::Face), 2);
  EXPECT_EQ(count(m, ObjectKind::Curve), 2);  // the ends outside
}

// A face with a hole: the line crossing it on both sides of the hole
// divides it without filling the hole.
TEST(SplitAcross, AroundAHole) {
  Modeler m;
  Api a{m};
  auto face = m.createRectangle(at(0, 0), 2000, 1000);
  auto disc = m.createCircle(at(1000, 500), 200);
  ASSERT_TRUE(m.remove({*disc}).ok());
  const double area = 2e6 - M_PI * 200 * 200;
  ASSERT_TRUE(a.line({{-100, 500, 0}, {2100, 500, 0}})["ok"].get<bool>());
  double total = 0;
  for (const Object& o : m.document().objects()) {
    if (o.kind() == ObjectKind::Face) total += geom::area(m.document().worldShape(o));
  }
  EXPECT_NEAR(total, area, 1e-2);  // the hole stays a hole
  EXPECT_EQ(count(m, ObjectKind::Face), 2);
  (void)face;
}

// The whole width copied: each copy runs across all three strips.
TEST(SplitAcross, CopiedAcrossAllStrips) {
  Modeler m;
  Api a{m};
  const ObjectId box = stripedBox(m);
  const auto r = a.line({{-300, 0, 500}, {3300, 0, 500}});
  ASSERT_TRUE(r["ok"].get<bool>());
  // Copy the piece left outside (x < 0) together with the faces' new
  // edges: here simply the drawn line again, as a loose line copied up.
  auto loose = m.createPolyline({gp_Pnt(-300, -2000, 500), gp_Pnt(3300, -2000, 500)});
  ASSERT_TRUE(loose.ok());
  TopoDS_Edge edge = TopoDS::Edge(TopExp_Explorer(m.document().worldShape(*m.document().find(*loose)), TopAbs_EDGE).Current());
  gp_Trsf up;
  up.SetTranslation(gp_Vec(0, 2000, 500));
  ASSERT_TRUE(m.copyEdges({edge}, up, 1).ok());  // lands on the front at z = 1000
  EXPECT_EQ(faces(m, box), 14);  // each strip in three
  EXPECT_TRUE(geom::validateShape(m.document().worldShape(*m.document().find(box)), true).valid);
}

// Report: the 4 top edges of a cube copied down to the bottom and divided
// by /4 drew the lines, but each side stayed one face.
TEST(SplitAcross, TopEdgesCopiedDownDivideEverySide) {
  for (int n : {3, 4, 5}) {
    for (bool all : {true, false}) {
      Modeler m;
      auto base = m.createRectangle(at(0, 0), 1000, 1000);
      ASSERT_TRUE(m.extrude(*base, 1000).ok());
      const ObjectId box = *base;
      const double volume = volumeOf(m, box);
      std::vector<TopoDS_Edge> top;
      for (TopExp_Explorer e(m.document().worldShape(*m.document().find(box)), TopAbs_EDGE); e.More(); e.Next()) {
        const TopoDS_Edge edge = TopoDS::Edge(e.Current());
        const gp_Pnt a = BRep_Tool::Pnt(TopExp::FirstVertex(edge)), b = BRep_Tool::Pnt(TopExp::LastVertex(edge));
        const bool known = std::any_of(top.begin(), top.end(), [&](const TopoDS_Edge& t) { return t.IsSame(edge); });
        if (!known && std::abs(a.Z() - 1000) < 1e-6 && std::abs(b.Z() - 1000) < 1e-6) top.push_back(edge);
      }
      ASSERT_EQ(top.size(), 4u);
      if (!all) top.resize(1);
      gp_Trsf step;
      step.SetTranslation(gp_Vec(0, 0, -1000.0 / n));
      ASSERT_TRUE(m.copyEdges(top, step, n).ok()) << n;
      // 4 sides (or the 1 side) in n strips, top and bottom.
      EXPECT_EQ(faces(m, box), all ? 4 * n + 2 : 3 + n + 2) << n << " " << all;
      EXPECT_NEAR(volumeOf(m, box), volume, 1e-3);
      EXPECT_TRUE(geom::validateShape(m.document().worldShape(*m.document().find(box)), true).valid);
      EXPECT_EQ(count(m, ObjectKind::Curve), 0) << n << " " << all;
      EXPECT_EQ(count(m, ObjectKind::Face), 0) << n << " " << all;
      const int divided = faces(m, box);
      // One undo step; redo divides again.
      ASSERT_TRUE(m.undo());
      EXPECT_EQ(faces(m, box), 6);
      ASSERT_TRUE(m.redo());
      EXPECT_EQ(faces(m, box), divided);
      // Saved and reloaded: still divided.
      const std::string path = ::testing::TempDir() + "split_top_edges.sbr";
      ASSERT_TRUE(m.save(path).ok());
      Modeler back;
      std::vector<std::string> warnings;
      ASSERT_TRUE(back.load(path, &warnings).ok());
      EXPECT_EQ(faces(back, box), divided);
      // One strip of the front (y = 0) on its own: the lowest one, out by 300.
      const TopoDS_Face strip = frontFaceAt(m, box, gp_Pnt(500, 0, 500.0 / n));
      ASSERT_FALSE(strip.IsNull());
      const Bnd_Box b = geom::exactBounds(strip);
      EXPECT_NEAR(b.CornerMax().Z() - b.CornerMin().Z(), 1000.0 / n, 1e-6);
      ASSERT_TRUE(m.pushPull(box, strip, 300).ok());
      EXPECT_NEAR(volumeOf(m, box), volume + 1000.0 * 300 * 1000.0 / n, 1e-3);
      EXPECT_TRUE(geom::validateShape(m.document().worldShape(*m.document().find(box)), true).valid);
    }
  }
}

// Curves used to be displayed on planar faces but worldPolyline() rejected
// every non-line edge, so the tidy path never imprinted them.
namespace {
int circleEdges(const TopoDS_Shape& shape) {
  int n=0;for(TopExp_Explorer e(shape,TopAbs_EDGE);e.More();e.Next())n+=BRepAdaptor_Curve(TopoDS::Edge(e.Current())).GetType()==GeomAbs_Circle;
  return n;
}
double faceArea(const Modeler& m) {
  double area=0;for(const auto& o:m.document().objects())if(o.kind()==ObjectKind::Face)area+=geom::area(o.shape);return area;
}
}
TEST(PlanarCurveSplit, ArcKeepsExactBoundaryAndUndoSaveAndPushPull) {
  Modeler m;auto face=m.createRectangle(at(0,0),100,100);ASSERT_TRUE(face);
  auto material=m.createMaterial("Texture",{0.6,0.7,0.8});ASSERT_TRUE(material);
  const auto image=m.addTextureImage("texture bytes for persistence test","texture.png");
  auto settings=*m.document().findMaterial(*material);settings.texture=MaterialTexture{image,10,20,false};ASSERT_TRUE(m.editMaterial(settings));
  ASSERT_TRUE(m.paintFaces(*face,{TopoDS::Face(m.document().find(*face)->shape)},*material));
  const auto steps=m.history().undoCount();m.history().beginGroup("Arc");
  auto arc=m.createArc(gp_Pnt(0,50,0),gp_Pnt(100,50,0),gp::DZ(),20,false);ASSERT_TRUE(arc);
  auto settled=m.settleOverlaps({*arc});ASSERT_TRUE(settled)<<settled.error();m.history().endGroup();
  ASSERT_EQ(count(m,ObjectKind::Face),2);EXPECT_EQ(count(m,ObjectKind::Curve),0);EXPECT_NEAR(faceArea(m),10000,1e-5);
  int circles=0;for(const auto& o:m.document().objects())circles+=circleEdges(o.shape);EXPECT_GE(circles,2);
  EXPECT_EQ(m.history().undoCount(),steps+1);ASSERT_TRUE(m.undo());EXPECT_EQ(count(m,ObjectKind::Face),1);
  ASSERT_TRUE(m.redo());EXPECT_EQ(count(m,ObjectKind::Face),2);
  const auto file=::testing::TempDir()+"planar_arc.sbr";ASSERT_TRUE(m.save(file));Modeler loaded;ASSERT_TRUE(loaded.load(file));
  EXPECT_EQ(count(loaded,ObjectKind::Face),2);EXPECT_NEAR(faceArea(loaded),10000,1e-5);
  for(const auto& o:loaded.document().objects())EXPECT_EQ(o.material,*material);
  ASSERT_TRUE(loaded.document().findMaterial(*material)->texture);EXPECT_EQ(loaded.document().findMaterial(*material)->texture->image,image);
  const auto& part=loaded.document().objects().front();const auto id=part.id;const double area=geom::area(part.shape);
  ASSERT_TRUE(loaded.extrude(id,10));EXPECT_NEAR(volumeOf(loaded,id),area*10,1e-4);
}
TEST(PlanarCurveSplit, SplineOnTiltedPlaneAndPlacedGroup) {
  Modeler m;auto plane=geom::SketchPlane::fromNormal(gp_Pnt(0,0,0),gp_Dir(0,1,1));
  auto face=m.createRectangle(plane,100,100);ASSERT_TRUE(face);auto group=m.group({*face});ASSERT_TRUE(group);
  ASSERT_TRUE(m.move({*group},gp_Vec(200,300,400)));ASSERT_TRUE(m.setActiveGroup(*group));
  auto point=[&](double x,double y){return plane.origin.Translated(gp_Vec(plane.xDir)*x+gp_Vec(plane.yDir())*y+gp_Vec(200,300,400));};
  auto curve=m.createCurve({point(0,50),point(25,30),point(70,70),point(100,50)},false);ASSERT_TRUE(curve);
  auto settled=m.settleOverlaps({*curve});ASSERT_TRUE(settled)<<settled.error();
  EXPECT_EQ(count(m,ObjectKind::Face),2);EXPECT_EQ(count(m,ObjectKind::Curve),0);EXPECT_NEAR(faceArea(m),10000,1e-4);
  bool spline=false;for(const auto& o:m.document().objects())if(o.kind()==ObjectKind::Face) {
    EXPECT_EQ(o.parent,*group);for(TopExp_Explorer e(o.shape,TopAbs_EDGE);e.More();e.Next())spline|=BRepAdaptor_Curve(TopoDS::Edge(e.Current())).GetType()==GeomAbs_BSplineCurve;
  }
  EXPECT_TRUE(spline);
}
TEST(PlanarCurveSplit, SolidFaceRetainsVolumeAndRecipeAfterReload) {
  Modeler m;auto box=m.createRectangle(at(0,0),100,100);ASSERT_TRUE(box);ASSERT_TRUE(m.extrude(*box,100));
  auto arc=m.createArc(gp_Pnt(0,0,50),gp_Pnt(100,0,50),gp_Dir(0,-1,0),20,false);ASSERT_TRUE(arc);
  auto settled=m.settleOverlaps({*arc});ASSERT_TRUE(settled)<<settled.error();
  EXPECT_EQ(faces(m,*box),7);EXPECT_EQ(count(m,ObjectKind::Curve),0);EXPECT_NEAR(volumeOf(m,*box),1000000,1e-4);
  EXPECT_TRUE(geom::validateShape(m.document().find(*box)->shape,true).valid);
  const auto file=::testing::TempDir()+"solid_arc_split.sbr";ASSERT_TRUE(m.save(file));Modeler loaded;ASSERT_TRUE(loaded.load(file));
  EXPECT_EQ(faces(loaded,*box),7);EXPECT_NEAR(volumeOf(loaded,*box),1000000,1e-4);
  const auto part=frontFaceAt(loaded,*box,gp_Pnt(50,0,25));ASSERT_FALSE(part.IsNull());
  const auto partArea=geom::area(part);ASSERT_TRUE(loaded.pushPull(*box,part,10));
  EXPECT_NEAR(volumeOf(loaded,*box),1000000+partArea*10,1e-3);
}
TEST(PlanarCurveSplit, CurvesStoppingInsideOrOffThePlaneDoNotDivide) {
  for(int mode=0;mode<4;++mode) {
    SCOPED_TRACE(mode);Modeler m;auto face=m.createRectangle(at(0,0),100,100);ASSERT_TRUE(face);
    if(mode==3){ASSERT_TRUE(m.group({*face}));}
    const double z=mode==1?1:0;
    auto arc=m.createArc(gp_Pnt(mode==0?10:0,50,z),gp_Pnt(mode==0?90:100,50,z),mode==2?gp::DY():gp::DZ(),20,false);ASSERT_TRUE(arc);
    auto settled=m.settleOverlaps({*arc});ASSERT_TRUE(settled)<<settled.error();
    EXPECT_EQ(count(m,ObjectKind::Face),1);EXPECT_EQ(count(m,ObjectKind::Curve),1);EXPECT_NEAR(faceArea(m),10000,1e-5);
  }
}
TEST(PlanarCurveSplit, OversizedArcKeepsOnlyTheTwoExactOutsidePortions) {
  Modeler m;ASSERT_TRUE(m.createRectangle(at(0,0),100,100));
  auto arc=m.createArc(gp_Pnt(-50,50,0),gp_Pnt(150,50,0),gp::DZ(),20,false);ASSERT_TRUE(arc);
  auto settled=m.settleOverlaps({*arc});ASSERT_TRUE(settled)<<settled.error();
  EXPECT_EQ(count(m,ObjectKind::Face),2);EXPECT_EQ(count(m,ObjectKind::Curve),2);EXPECT_NEAR(faceArea(m),10000,1e-5);
  for(const auto& o:m.document().objects())if(o.kind()==ObjectKind::Curve){EXPECT_EQ(circleEdges(o.shape),1); }
}

TEST(PlanarCurveSplit, RepeatedStrokesPreserveAreaAndSaveAsOneUndoGroup) {
  Modeler m;ASSERT_TRUE(m.createRectangle(at(0,0),1000,1000));
  const auto steps=m.history().undoCount();m.history().beginGroup("Repeated curves");
  const auto start=std::chrono::steady_clock::now();
  for(int i=0;i<24;++i) {
    auto arc=m.createArc(gp_Pnt(0,20+40*i,0),gp_Pnt(1000,20+40*i,0),gp::DZ(),10,false);ASSERT_TRUE(arc);
    auto result=m.splitWithCurve(*arc);ASSERT_TRUE(result)<<result.error();ASSERT_TRUE(*result);
  }
  m.history().endGroup();
  std::cout<<"24 planar curved strokes: "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<" ms\n";
  EXPECT_EQ(count(m,ObjectKind::Face),25);EXPECT_NEAR(faceArea(m),1000000,1e-3);EXPECT_EQ(m.history().undoCount(),steps+1);
  const auto file=::testing::TempDir()+"repeated_planar_curves.sbr";ASSERT_TRUE(m.save(file));Modeler loaded;ASSERT_TRUE(loaded.load(file));
  EXPECT_EQ(count(loaded,ObjectKind::Face),25);EXPECT_NEAR(faceArea(loaded),1000000,1e-3);
  ASSERT_TRUE(m.undo());EXPECT_EQ(count(m,ObjectKind::Face),1);ASSERT_TRUE(m.redo());EXPECT_EQ(count(m,ObjectKind::Face),25);
}
