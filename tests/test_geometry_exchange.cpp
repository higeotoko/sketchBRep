#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <IGESControl_Writer.hxx>
#include <IGESControl_Controller.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopExp_Explorer.hxx>
#include <nlohmann/json.hpp>
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/api/CommandApi.h"
using namespace sbr;
namespace {
std::string path(bool iges,const std::string& suffix={}) {
  return ::testing::TempDir()+std::string("exchange_")+::testing::UnitTest::GetInstance()->current_test_info()->name()+suffix+(iges?".igs":".brep");
}
Status write(doc::Modeler& m,bool iges,const std::string& p,const std::vector<doc::ObjectId>& ids={}) {
  return iges?m.exportIges(p,ids):m.exportBrep(p,ids);
}
Result<std::vector<doc::ObjectId>> read(doc::Modeler& m,bool iges,const std::string& p) {
  return iges?m.importIges(p):m.importBrep(p);
}
double volume(const doc::Modeler& m) {
  double value=0;for(const auto& o:m.document().objects()){GProp_GProps p;BRepGProp::VolumeProperties(m.document().worldShape(o),p);value+=p.Mass();}return value;
}
}
TEST(GeometryExchange, ExactGeometryUnicodeRoundtripAndSourceUnchanged) {
  for(bool iges:{false,true}) {
    SCOPED_TRACE(iges);doc::Modeler source;
    ASSERT_TRUE(source.importShape(BRepPrimAPI_MakeBox(10,20,30).Shape(),"Box","test"));
    ASSERT_TRUE(source.importShape(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(100,0,0),gp::DZ()),5,10).Shape(),"Cylinder","test"));
    const auto before=doc::serializeDocument(source.document());const auto history=source.history().undoCount();
    const auto p=path(iges,u8"_模型 exchange");ASSERT_TRUE(write(source,iges,p));
    EXPECT_EQ(before,doc::serializeDocument(source.document()));EXPECT_EQ(source.history().undoCount(),history);
    doc::Modeler imported;auto ids=read(imported,iges,p);ASSERT_TRUE(ids)<<ids.error();
    EXPECT_NEAR(volume(imported),6000+250*std::acos(-1),1e-3);
    int faces=0;bool curved=false;
    for(const auto& o:imported.document().objects())for(TopExp_Explorer f(o.shape,TopAbs_FACE);f.More();f.Next()){
      ++faces;curved|=BRepAdaptor_Surface(TopoDS::Face(f.Current())).GetType()!=GeomAbs_Plane;
    }
    EXPECT_TRUE(curved);EXPECT_LT(faces,20);
  }
}
TEST(GeometryExchange, SelectionGroupPlacementVisibilityAndImageFiltering) {
  for(bool iges:{false,true}) {
    doc::Modeler m;auto a=m.importShape(BRepPrimAPI_MakeBox(10,20,30).Shape(),"A","test");ASSERT_TRUE(a);
    auto b=m.importShape(BRepPrimAPI_MakeBox(20,20,20).Shape(),"B","test");ASSERT_TRUE(b);
    auto g=m.group({*a});ASSERT_TRUE(g);ASSERT_TRUE(m.move({*g},gp_Vec(100,200,300)));
    ASSERT_TRUE(m.hide({*b}));ASSERT_TRUE(m.addImageUnderlay("image","plan.png",100,100,geom::SketchPlane::ground(),100));
    const auto p=path(iges);ASSERT_TRUE(write(m,iges,p));doc::Modeler all;ASSERT_TRUE(read(all,iges,p));EXPECT_NEAR(volume(all),6000,1e-3);
    ASSERT_TRUE(write(m,iges,p,{*g,*a}));doc::Modeler chosen;ASSERT_TRUE(read(chosen,iges,p));EXPECT_NEAR(volume(chosen),6000,1e-3);
    Bnd_Box bounds;for(const auto& o:chosen.document().objects())BRepBndLib::Add(chosen.document().worldShape(o),bounds);
    EXPECT_NEAR(bounds.CornerMin().X(),100,1e-3);EXPECT_NEAR(bounds.CornerMin().Y(),200,1e-3);EXPECT_NEAR(bounds.CornerMin().Z(),300,1e-3);
    EXPECT_FALSE(write(m,iges,p,{*b}));EXPECT_FALSE(write(m,iges,p,{"missing"}));
  }
}
TEST(GeometryExchange, CurvesUndoRedoSaveReopenAndInvalidImportsAreAtomic) {
  for(bool iges:{false,true}) {
    doc::Modeler source;ASSERT_TRUE(source.createPolyline({gp_Pnt(0,0,0),gp_Pnt(10,0,0)}));
    const auto p=path(iges);ASSERT_TRUE(write(source,iges,p));doc::Modeler m;auto ids=read(m,iges,p);ASSERT_TRUE(ids)<<ids.error();
    double length=0;for(const auto& o:m.document().objects()){GProp_GProps props;BRepGProp::LinearProperties(o.shape,props);length+=props.Mass();}
    EXPECT_NEAR(length,10,1e-6);EXPECT_EQ(m.history().undoCount(),1u);
    ASSERT_TRUE(m.history().undo());EXPECT_TRUE(m.listObjects().empty());ASSERT_TRUE(m.history().redo());
    ASSERT_TRUE(m.save(p+".sbr"));doc::Modeler loaded;ASSERT_TRUE(loaded.load(p+".sbr"));EXPECT_EQ(loaded.listObjects().size(),ids->size());
    {std::ofstream bad(std::filesystem::u8path(p));bad<<"invalid exchange file";}
    const auto before=doc::serializeDocument(m.document());const auto history=m.history().undoCount();EXPECT_FALSE(read(m,iges,p));
    EXPECT_EQ(before,doc::serializeDocument(m.document()));EXPECT_EQ(m.history().undoCount(),history);
    EXPECT_FALSE(write(source,iges,p+"/missing/file"));
    doc::Modeler empty;EXPECT_FALSE(write(empty,iges,p));
  }
}
TEST(GeometryExchange, IgesInchHeaderNormalizesToMillimetres) {
  IGESControl_Controller::Init();IGESControl_Writer writer("INCH",1);
  ASSERT_TRUE(writer.AddShape(BRepPrimAPI_MakeBox(25.4,25.4,25.4).Shape()));
  const auto p=path(true);{std::ofstream out(std::filesystem::u8path(p));ASSERT_TRUE(writer.Write(out));}
  doc::Modeler m;auto ids=m.importIges(p);ASSERT_TRUE(ids)<<ids.error();EXPECT_NEAR(volume(m),25.4*25.4*25.4,1e-3);
}
TEST(GeometryExchange, CliDiagnosticsDoNotPolluteJsonAndExternalIoRestrictionsApply) {
  using nlohmann::json;
  for(bool iges:{false,true}) {
    doc::Modeler m;ASSERT_TRUE(m.importShape(BRepPrimAPI_MakeBox(10,20,30).Shape(),"Box","test"));
    api::CommandApi commands(m),restricted(m,api::CommandApi::Options{false});
    const auto p=path(iges);const std::string format=iges?"iges":"brep";
    json request={{"command","export_"+format},{"args",{{"path",p}}}};
    EXPECT_EQ(commands.effectOf(request.dump()),api::CommandApi::Effect::ExternalIo);
    EXPECT_FALSE(json::parse(restricted.execute(request.dump()))["ok"].get<bool>());
    EXPECT_FALSE(json::parse(commands.execute(json{{"command","preview"},{"args",{{"commands",json::array({request})}}}}.dump()))["ok"].get<bool>());
    ASSERT_TRUE(json::parse(commands.execute(request.dump()))["ok"].get<bool>());
    request["command"]="import_"+format;
    testing::internal::CaptureStdout();const auto result=json::parse(commands.execute(request.dump()));const auto stdoutText=testing::internal::GetCapturedStdout();
    EXPECT_TRUE(result["ok"].get<bool>())<<result.dump();EXPECT_TRUE(stdoutText.empty())<<stdoutText;
    EXPECT_FALSE(json::parse(restricted.execute(request.dump()))["ok"].get<bool>());
    {std::ofstream bad(std::filesystem::u8path(p));bad<<"invalid exchange file";}
    testing::internal::CaptureStdout();const auto failed=json::parse(commands.execute(request.dump()));const auto invalidStdout=testing::internal::GetCapturedStdout();
    EXPECT_FALSE(failed["ok"].get<bool>());EXPECT_TRUE(invalidStdout.empty())<<invalidStdout;
  }
}
