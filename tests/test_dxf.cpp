#include <gtest/gtest.h>
#include <fstream>
#include <cmath>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <nlohmann/json.hpp>
#include "sbr/doc/Modeler.h"
#include "sbr/api/CommandApi.h"

using namespace sbr;
namespace {
std::string fixture(const std::string& entities, int units = 4, const std::string& blocks = {}) {
  const std::string path = ::testing::TempDir() + "dxf_" +
      ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".dxf";
  std::ofstream f(path);
  f << "0\nSECTION\n2\nHEADER\n9\n$INSUNITS\n70\n" << units << "\n0\nENDSEC\n";
  if (!blocks.empty()) f << "0\nSECTION\n2\nBLOCKS\n" << blocks << "0\nENDSEC\n";
  f << "0\nSECTION\n2\nENTITIES\n" << entities << "0\nENDSEC\n0\nEOF\n";
  return path;
}
const std::string line = "0\nLINE\n8\nWalls\n10\n1\n20\n2\n11\n4\n21\n6\n";
double length(const TopoDS_Shape& shape) {
  GProp_GProps props; BRepGProp::LinearProperties(shape, props); return props.Mass();
}
}

TEST(Dxf, UnitsTagsUndoRedoAndSaveReopen) {
  const auto path = fixture(line, 6);
  doc::Modeler m;
  geom::DxfImportReport report;
  auto ids = m.importDxf(path, {}, &report);
  ASSERT_TRUE(ids) << ids.error();
  ASSERT_EQ(ids->size(), 1u);
  EXPECT_EQ(m.document().find(ids->front())->layer, "Walls");
  EXPECT_NEAR(length(m.document().find(ids->front())->shape), 5000, 1e-6);
  EXPECT_TRUE(report.unitsKnown);
  EXPECT_EQ(report.scaleToMm, 1000);
  EXPECT_TRUE(m.history().undo()); EXPECT_TRUE(m.listObjects().empty());
  EXPECT_TRUE(m.history().redo()); EXPECT_EQ(m.listObjects().size(), 1u);
  EXPECT_EQ(m.history().undoCount(), 1u);
  ASSERT_TRUE(m.save(path + ".sbr"));
  doc::Modeler loaded; ASSERT_TRUE(loaded.load(path + ".sbr"));
  EXPECT_EQ(loaded.document().find(ids->front())->layer, "Walls");
  EXPECT_NEAR(length(loaded.document().find(ids->front())->shape), 5000, 1e-6);
}

TEST(Dxf, CirclesArcsAndBulgesRemainExact) {
  const auto path = fixture("0\nCIRCLE\n10\n0\n20\n0\n40\n5\n"
      "0\nARC\n10\n0\n20\n0\n40\n10\n50\n350\n51\n10\n"
      "0\nLWPOLYLINE\n90\n2\n70\n0\n10\n0\n20\n0\n42\n-1\n10\n10\n20\n0\n");
  auto parts = geom::readDxf(path);
  ASSERT_TRUE(parts) << parts.error(); ASSERT_EQ(parts->size(), 3u);
  const double pi = std::acos(-1);
  EXPECT_NEAR(length((*parts)[0].shape), 10 * pi, 1e-6);
  EXPECT_NEAR(length((*parts)[1].shape), 10 * pi / 9, 1e-6);
  EXPECT_NEAR(length((*parts)[2].shape), 5 * pi, 1e-6);
  for (const auto& part : *parts) {
    TopExp_Explorer edges(part.shape, TopAbs_EDGE); ASSERT_TRUE(edges.More());
    EXPECT_EQ(BRepAdaptor_Curve(TopoDS::Edge(edges.Current())).GetType(), GeomAbs_Circle);
  }
}

TEST(Dxf, ClosedPolylineIsAWireNotAnAutomaticFace) {
  auto parts = geom::readDxf(fixture("0\nLWPOLYLINE\n90\n4\n70\n1\n8\nOutline\n"
      "10\n0\n20\n0\n10\n10\n20\n0\n10\n10\n20\n20\n10\n0\n20\n20\n"));
  ASSERT_TRUE(parts) << parts.error(); ASSERT_EQ(parts->size(), 1u);
  EXPECT_EQ(parts->front().shape.ShapeType(), TopAbs_WIRE);
  EXPECT_FALSE(TopExp_Explorer(parts->front().shape, TopAbs_FACE).More());
  EXPECT_NEAR(length(parts->front().shape), 60, 1e-6);
}

TEST(Dxf, PartialImportIsExplicitAndAtomic) {
  const auto path = fixture(line + "0\nTEXT\n1\nNote\n10\n0\n20\n0\n" +
      "0\nLINE\n10\nnan\n20\n0\n11\n1\n21\n1\n");
  doc::Modeler m;
  geom::DxfImportReport report;
  EXPECT_FALSE(m.importDxf(path, {}, &report));
  EXPECT_TRUE(m.listObjects().empty()); EXPECT_FALSE(m.history().canUndo());
  EXPECT_EQ(report.imported, 1); EXPECT_EQ(report.omitted.size(), 2u);
  geom::DxfImportOptions options; options.allowPartial = true;
  ASSERT_TRUE(m.importDxf(path, options, &report));
  EXPECT_EQ(m.listObjects().size(), 1u); EXPECT_EQ(m.history().undoCount(), 1u);
  EXPECT_TRUE(m.history().undo()); EXPECT_TRUE(m.listObjects().empty());
}

TEST(Dxf, UnitlessOverrideAndPaperSpace) {
  geom::DxfImportReport report;
  geom::DxfImportOptions options; options.unitlessScaleToMm = 25.4;
  const auto path = fixture(line + "0\nLINE\n67\n1\n10\n0\n20\n0\n11\n1\n21\n0\n", 0);
  auto parts = geom::readDxf(path, options, &report);
  ASSERT_TRUE(parts) << parts.error(); ASSERT_EQ(parts->size(), 1u);
  EXPECT_FALSE(report.unitsKnown); EXPECT_EQ(report.paperSpace, 1);
  EXPECT_NEAR(length(parts->front().shape), 127, 1e-6);
  options.unitlessScaleToMm = -1; EXPECT_FALSE(geom::readDxf(path, options));
  EXPECT_FALSE(geom::readDxf(fixture(line, 999)));
}

TEST(Dxf, InsertUsesBlockBaseRotationScaleAndInheritedLayer) {
  const std::string blocks = "0\nBLOCK\n2\nDoor\n10\n1\n20\n0\n"
      "0\nLINE\n8\n0\n10\n1\n20\n0\n11\n3\n21\n0\n0\nENDBLK\n";
  auto parts = geom::readDxf(fixture("0\nINSERT\n2\nDoor\n8\nDoors\n10\n10\n20\n20\n"
      "41\n2\n42\n2\n43\n2\n50\n90\n", 4, blocks));
  ASSERT_TRUE(parts) << parts.error(); ASSERT_EQ(parts->size(), 1u);
  EXPECT_EQ(parts->front().layer, "Doors");
  Bnd_Box bounds; BRepBndLib::Add(parts->front().shape, bounds);
  double x0,y0,z0,x1,y1,z1; bounds.Get(x0,y0,z0,x1,y1,z1);
  EXPECT_NEAR(x0, 10, 1e-5); EXPECT_NEAR(x1, 10, 1e-5);
  EXPECT_NEAR(y0, 20, 1e-5); EXPECT_NEAR(y1, 24, 1e-5);
}

TEST(Dxf, LegacyPolylineAndUnicodeLayer) {
  auto parts = geom::readDxf(fixture("0\nPOLYLINE\n8\n\\U+58C1\n70\n0\n"
      "0\nVERTEX\n10\n0\n20\n0\n0\nVERTEX\n10\n10\n20\n0\n0\nSEQEND\n"));
  ASSERT_TRUE(parts) << parts.error(); ASSERT_EQ(parts->size(), 1u);
  EXPECT_EQ(parts->front().layer, u8"壁");
  EXPECT_NEAR(length(parts->front().shape), 10, 1e-6);
}

TEST(Dxf, RejectsTruncatedAndBinaryFilesWithoutChangingModel) {
  const auto path = fixture(line);
  for (const auto& content : {std::string("0\nSECTION\n2\nENTITIES\n") + line,
                              std::string("AutoCAD Binary DXF\r\n\x1a\0", 22), std::string("0\n")}) {
    { std::ofstream f(path, std::ios::binary); f << content; }
    doc::Modeler m; EXPECT_FALSE(m.importDxf(path)); EXPECT_FALSE(m.history().canUndo());
  }
}

TEST(Dxf, ApiReportsImportAndRespectsFileAccessPolicy) {
  doc::Modeler m; api::CommandApi api(m);
  const auto path = fixture(line);
  const auto request = nlohmann::json{{"command", "import_dxf"}, {"args", {{"path", path}}}}.dump();
  auto response = nlohmann::json::parse(api.execute(request));
  EXPECT_TRUE(response["ok"].get<bool>()) << response.dump();
  EXPECT_EQ(m.listObjects().size(), 1u);
  doc::Modeler blocked;
  api::CommandApi restricted(blocked, {false});
  auto denied = nlohmann::json::parse(restricted.execute(request));
  EXPECT_FALSE(denied["ok"].get<bool>()) << denied.dump();
  EXPECT_TRUE(blocked.listObjects().empty());
  EXPECT_EQ(api.effectOf(request), api::CommandApi::Effect::ExternalIo);
}

TEST(Dxf, EllipseAndSplineAreAnalyticCurves) {
  auto parts = geom::readDxf(fixture("0\nELLIPSE\n10\n0\n20\n0\n11\n10\n21\n0\n40\n0.5\n"
      "0\nSPLINE\n70\n8\n71\n2\n72\n6\n73\n3\n40\n0\n40\n0\n40\n0\n40\n1\n40\n1\n40\n1\n"
      "10\n0\n20\n0\n30\n0\n10\n5\n20\n0\n30\n0\n10\n10\n20\n0\n30\n0\n"));
  ASSERT_TRUE(parts) << parts.error(); ASSERT_EQ(parts->size(), 2u);
  EXPECT_EQ(BRepAdaptor_Curve(TopoDS::Edge(parts->front().shape)).GetType(), GeomAbs_Ellipse);
  EXPECT_EQ(BRepAdaptor_Curve(TopoDS::Edge(parts->back().shape)).GetType(), GeomAbs_BSplineCurve);
  EXPECT_NEAR(length(parts->back().shape), 10, 1e-6);
}

TEST(Dxf, DoesNotFlatten3DOrInventWidthsAndNonuniformBlocks) {
  const std::string block = "0\nBLOCK\n2\nB\n10\n0\n20\n0\n" + line + "0\nENDBLK\n";
  geom::DxfImportReport report;
  auto parts = geom::readDxf(fixture(line +
      "0\nLINE\n10\n0\n20\n0\n30\n1\n11\n10\n21\n0\n31\n2\n"
      "0\nCIRCLE\n10\n0\n20\n0\n40\n10\n230\n-1\n"
      "0\nLWPOLYLINE\n90\n2\n43\n3\n10\n0\n20\n0\n10\n10\n20\n0\n"
      "0\nINSERT\n2\nB\n10\n0\n20\n0\n41\n2\n42\n1\n", 4, block), {}, &report);
  ASSERT_TRUE(parts) << parts.error(); EXPECT_EQ(parts->size(), 1u);
  EXPECT_EQ(report.omitted.size(), 4u);
}

TEST(Dxf, ReadsIndependentWriterFixture) {
  geom::DxfImportReport report;
  auto parts = geom::readDxf(std::string(SBR_TEST_DATA_DIR) + "/dxf/architectural.dxf", {}, &report);
  ASSERT_TRUE(parts) << parts.error();
  ASSERT_EQ(parts->size(), 7u);
  EXPECT_FALSE(report.incomplete()) << report.summary();
  EXPECT_EQ(report.scaleToMm, 1000);
  EXPECT_EQ(parts->front().layer, u8"壁");
  EXPECT_NEAR(length(parts->front().shape), 4000, 1e-6);
  EXPECT_EQ(BRepAdaptor_Curve(TopoDS::Edge((*parts)[5].shape)).GetType(), GeomAbs_BSplineCurve);
  EXPECT_NEAR(length(parts->back().shape), 1000, 1e-6);
}

TEST(Dxf, LayerBatchingKeepsExactGeometryAndHistory) {
  const auto path = fixture(line + line +
      "0\nARC\n8\nGarden\n10\n0\n20\n0\n40\n10\n50\n0\n51\n90\n");
  auto plain = geom::readDxf(path); ASSERT_TRUE(plain);
  geom::DxfImportOptions options; options.groupByLayer = true;
  geom::DxfImportReport report;
  auto batched = geom::readDxf(path, options, &report); ASSERT_TRUE(batched);
  ASSERT_EQ(batched->size(), 2u); EXPECT_EQ(report.imported, 3);
  double expected = 0, actual = 0;
  for (const auto& p : *plain) expected += length(p.shape);
  int edges = 0, circles = 0;
  for (const auto& p : *batched) {
    actual += length(p.shape);
    for (TopExp_Explorer e(p.shape, TopAbs_EDGE); e.More(); e.Next()) {
      ++edges;
      circles += BRepAdaptor_Curve(TopoDS::Edge(e.Current())).GetType() == GeomAbs_Circle;
    }
  }
  EXPECT_EQ(edges, 3); EXPECT_EQ(circles, 1); EXPECT_NEAR(actual, expected, 1e-8);
  doc::Modeler m; auto ids = m.importDxf(path, options); ASSERT_TRUE(ids); ASSERT_EQ(ids->size(), 2u);
  EXPECT_TRUE(m.history().undo()); EXPECT_TRUE(m.listObjects().empty());
  EXPECT_TRUE(m.history().redo()); ASSERT_TRUE(m.save(path + ".sbr"));
  doc::Modeler loaded; ASSERT_TRUE(loaded.load(path + ".sbr"));
  EXPECT_EQ(loaded.listObjects().size(), 2u);
}

TEST(Dxf, MovingLayerBatchesPreservesOverlapsAndObjectIdentity) {
  const auto path = fixture(line + "0\nLINE\n8\nGarden\n10\n1\n20\n2\n11\n4\n21\n6\n");
  geom::DxfImportOptions options; options.groupByLayer = true;
  doc::Modeler m; auto ids = m.importDxf(path, options); ASSERT_TRUE(ids); ASSERT_EQ(ids->size(), 2u);
  const auto original = m.document().find(ids->front())->shape;
  ASSERT_TRUE(m.move(*ids, gp_Vec(1000, 500, 0)));
  ASSERT_EQ(m.listObjects().size(), 2u);
  for (const auto& id : *ids) {
    const auto* o = m.document().find(id); ASSERT_NE(o, nullptr);
    EXPECT_TRUE(std::holds_alternative<doc::ImportedShape>(o->recipe.base));
    EXPECT_NEAR(length(o->shape), 5, 1e-8);
    EXPECT_NEAR(m.document().worldTransform(*o).TranslationPart().X(), 1000, 1e-8);
  }
  EXPECT_TRUE(m.document().find(ids->front())->shape.IsSame(original));
  ASSERT_TRUE(m.history().undo());
  EXPECT_NEAR(m.document().worldTransform(*m.document().find(ids->front())).TranslationPart().X(), 0, 1e-8);
  ASSERT_TRUE(m.history().redo());
  EXPECT_EQ(m.listObjects().size(), 2u);
}

namespace {
std::string smallDxf(const char* name) { return std::string(SBR_TEST_DATA_DIR) + "/dxf/" + name; }
double faceArea(const TopoDS_Shape& s) { GProp_GProps p; BRepGProp::SurfaceProperties(s,p); return p.Mass(); }
std::vector<doc::DeleteTarget> wholeCurves(const std::vector<doc::ObjectId>& ids) {
  std::vector<doc::DeleteTarget> out; for (const auto& id : ids) out.push_back({id,true,{},{}}); return out;
}
}
TEST(Dxf, ImportedLineEdgesCloseAutomaticallyWithDrawnLastSide) {
  doc::Modeler m;
  auto ids = m.importDxf(smallDxf("small_open_rectangle.dxf")); ASSERT_TRUE(ids); ASSERT_EQ(ids->size(),3u);
  auto closing = m.createPolyline({gp_Pnt(0,1000,0),gp_Pnt(0,0,0)}); ASSERT_TRUE(closing);
  auto face = m.faceFromLoop(*closing); ASSERT_TRUE(face) << face.error();
  EXPECT_EQ(m.listObjects().size(),1u); EXPECT_NEAR(faceArea(m.document().find(*face)->shape),2000000,1e-5);
}
TEST(Dxf, ManualFaceFromSmallClosedLinesPolylineAndArc) {
  for (bool grouped : {false,true}) for (const char* name : {"small_closed_lines.dxf", "small_closed_polyline.dxf", "small_arc_outline.dxf"}) {
    SCOPED_TRACE(name);
    SCOPED_TRACE(grouped);
    doc::Modeler m; geom::DxfImportOptions options; options.groupByLayer=grouped;
    auto ids=m.importDxf(smallDxf(name),options); ASSERT_TRUE(ids);
    for (const auto& id:*ids) EXPECT_EQ(m.document().find(id)->kind(),doc::ObjectKind::Curve);
    const auto count=m.listObjects().size();
    auto face=m.faceFromSelection(wholeCurves(*ids)); ASSERT_TRUE(face)<<face.error();
    const double expected=std::string(name).find("arc")!=std::string::npos ? std::acos(-1)*500000 : 2000000;
    EXPECT_NEAR(faceArea(m.document().find(*face)->shape),expected,1e-5);
    EXPECT_EQ(m.listObjects().size(),count+1); EXPECT_EQ(m.history().undoCount(),2u);
    ASSERT_TRUE(m.history().undo()); EXPECT_EQ(m.listObjects().size(),count);
    ASSERT_TRUE(m.history().redo()); ASSERT_TRUE(m.extrude(*face,100));
    const auto path=::testing::TempDir()+std::string("manual_face.sbr"); ASSERT_TRUE(m.save(path));
    doc::Modeler loaded; ASSERT_TRUE(loaded.load(path));
    EXPECT_EQ(loaded.listObjects().size(),count+1);
  }
}
TEST(Dxf, SelectedEdgesFromMovedLayerKeepUnusedCurves) {
  const auto rectangle=geom::readDxf(smallDxf("small_closed_lines.dxf")); ASSERT_TRUE(rectangle);
  // A fifth, unrelated line makes whole-layer selection unsuitable for one face.
  std::string lines;
  const std::vector<gp_Pnt> p{{0,0,0},{2000,0,0},{2000,1000,0},{0,1000,0},{0,0,0}};
  for(size_t i=1;i<p.size();++i) lines+="0\nLINE\n8\nOutline\n10\n"+std::to_string(p[i-1].X())+"\n20\n"+std::to_string(p[i-1].Y())+"\n11\n"+std::to_string(p[i].X())+"\n21\n"+std::to_string(p[i].Y())+"\n";
  lines+="0\nLINE\n8\nOutline\n10\n5000\n20\n0\n11\n6000\n21\n0\n";
  doc::Modeler m; geom::DxfImportOptions options; options.groupByLayer=true;
  auto ids=m.importDxf(fixture(lines),options); ASSERT_TRUE(ids); ASSERT_EQ(ids->size(),1u);
  ASSERT_TRUE(m.move(*ids,gp_Vec(123,456,0)));
  const auto before=m.history().undoCount();
  EXPECT_FALSE(m.faceFromSelection(wholeCurves(*ids))); EXPECT_EQ(m.history().undoCount(),before);
  doc::DeleteTarget target{ids->front(),false,{},{}};
  for(TopExp_Explorer e(m.document().worldShape(*m.document().find(ids->front())),TopAbs_EDGE); e.More()&&target.edges.size()<4;e.Next()) target.edges.push_back(TopoDS::Edge(e.Current()));
  auto face=m.faceFromSelection({target}); ASSERT_TRUE(face)<<face.error();
  EXPECT_EQ(m.listObjects().size(),2u);
  EXPECT_NEAR(faceArea(m.document().find(*face)->shape),2000000,1e-5);
  EXPECT_NEAR(length(m.document().find(ids->front())->shape),7000,1e-6);
  const auto& loop=std::get<doc::LoopProfile>(m.document().find(*face)->recipe.base);
  ASSERT_EQ(loop.curves.size(),1u);
  EXPECT_NEAR(length(std::get<doc::ImportedShape>(loop.curves.front().recipe->base).shape),6000,1e-6);
}
TEST(Dxf, ManualFaceRejectsInvalidBoundariesWithoutChangingHistory) {
  for(const auto& points : std::vector<std::vector<gp_Pnt>>{
      {{0,0,0},{1000,0,0},{1000,1000,0},{0,0.01,0}},
      {{0,0,0},{1000,0,0},{1000,1000,10},{0,1000,0},{0,0,0}},
      {{0,0,0},{1000,1000,0},{1000,0,0},{0,1000,0},{0,0,0}}}) {
    doc::Modeler m; auto curve=m.createPolyline(points); ASSERT_TRUE(curve);
    EXPECT_FALSE(m.faceFromSelection(wholeCurves({*curve})));
    EXPECT_EQ(m.history().undoCount(),1u); EXPECT_EQ(m.listObjects().size(),1u);
  }
  doc::Modeler m; auto ids=m.importDxf(smallDxf("small_closed_lines.dxf")); ASSERT_TRUE(ids);
  ASSERT_TRUE(m.setLocked({ids->front()},true)); const auto before=m.history().undoCount();
  EXPECT_FALSE(m.faceFromSelection(wholeCurves(*ids))); EXPECT_EQ(m.history().undoCount(),before);
}

TEST(Dxf, ManualFaceRetainsEditableArcRecipe) {
  doc::Modeler m;
  auto arc=m.createArc(gp_Pnt(-1000,0,0),gp_Pnt(1000,0,0),gp::DZ(),1000,false); ASSERT_TRUE(arc);
  auto line=m.createPolyline({gp_Pnt(-1000,0,0),gp_Pnt(1000,0,0)}); ASSERT_TRUE(line);
  auto face=m.faceFromSelection(wholeCurves({*arc,*line})); ASSERT_TRUE(face)<<face.error();
  const auto& loop=std::get<doc::LoopProfile>(m.document().find(*face)->recipe.base);
  ASSERT_TRUE(std::holds_alternative<doc::ArcCurve>(loop.curves[0].recipe->base));
  const double before=faceArea(m.document().find(*face)->shape);
  ASSERT_TRUE(m.setParameter(*face,"in0.bulge",500));
  EXPECT_GT(std::abs(before-faceArea(m.document().find(*face)->shape)),1000);
  EXPECT_DOUBLE_EQ(std::get<doc::ArcCurve>(m.document().find(*arc)->recipe.base).bulge,1000);
}

TEST(Dxf, GroupedImportedOpenOutlineCanBeClosedManually) {
  doc::Modeler m; geom::DxfImportOptions options; options.groupByLayer=true;
  auto ids=m.importDxf(smallDxf("small_open_rectangle.dxf"),options); ASSERT_TRUE(ids);
  auto closing=m.createPolyline({gp_Pnt(0,1000,0),gp_Pnt(0,0,0)}); ASSERT_TRUE(closing);
  // An assembled layer deliberately has no single pair of curve ends.
  EXPECT_FALSE(m.faceFromLoop(*closing));
  auto selected=*ids; selected.push_back(*closing);
  auto face=m.faceFromSelection(wholeCurves(selected)); ASSERT_TRUE(face)<<face.error();
  EXPECT_NEAR(faceArea(m.document().find(*face)->shape),2000000,1e-5);
  EXPECT_EQ(m.listObjects().size(),3u);
}
