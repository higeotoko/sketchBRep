#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <nlohmann/json.hpp>
#include "sbr/doc/Modeler.h"
#include "sbr/api/CommandApi.h"

using namespace sbr;
namespace {
std::string output(const char* suffix) {
  return ::testing::TempDir()+"stl_"+::testing::UnitTest::GetInstance()->current_test_info()->name()+suffix;
}
struct Triangle { gp_Vec normal; gp_Pnt a,b,c; };
// Independent binary parser: exercises byte order, length, winding and units.
std::vector<Triangle> readStl(const std::string& path) {
  std::ifstream input(std::filesystem::u8path(path),std::ios::binary);
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),{});
  if(bytes.size()<84) throw std::runtime_error("short STL");
  auto u32=[&](size_t i){return std::uint32_t(bytes.at(i))|(std::uint32_t(bytes.at(i+1))<<8)|
    (std::uint32_t(bytes.at(i+2))<<16)|(std::uint32_t(bytes.at(i+3))<<24);};
  const auto count=u32(80);
  if(bytes.size()!=84+std::uint64_t(count)*50)throw std::runtime_error("incorrect STL length");
  std::vector<Triangle> out;
  for(std::uint32_t t=0;t<count;++t){
    float f[12];for(size_t k=0;k<12;++k){auto bits=u32(84+size_t(t)*50+k*4);std::memcpy(&f[k],&bits,4);if(!std::isfinite(f[k]))throw std::runtime_error("nonfinite float");}
    const size_t attr=84+size_t(t)*50+48;if(bytes[attr]||bytes[attr+1])throw std::runtime_error("unexpected attributes");
    out.push_back({gp_Vec(f[0],f[1],f[2]),gp_Pnt(f[3],f[4],f[5]),gp_Pnt(f[6],f[7],f[8]),gp_Pnt(f[9],f[10],f[11])});
  }
  return out;
}
double signedVolume(const std::vector<Triangle>& mesh) {
  double volume=0;
  for(const auto& t:mesh){
    const auto n=gp_Vec(t.a,t.b).Crossed(gp_Vec(t.a,t.c));
    EXPECT_GT(n.SquareMagnitude(),0);EXPECT_NEAR(t.normal.Magnitude(),1,1e-6);
    EXPECT_GT(t.normal.Dot(n),0);
    volume+=gp_Vec(gp_Pnt(0,0,0),t.a).Dot(gp_Vec(gp_Pnt(0,0,0),t.b).Crossed(gp_Vec(gp_Pnt(0,0,0),t.c)))/6;
  }
  return volume;
}
doc::ObjectId box(doc::Modeler& m,double x=0) {
  auto plane=geom::SketchPlane::ground();plane.origin=gp_Pnt(x,0,0);
  auto id=m.createRectangle(plane,10,20);if(!id||!m.extrude(*id,30))throw std::runtime_error("box failed");return *id;
}
}
TEST(Stl, BoxInMillimetresUnicodePathAndCanonicalGeometryUnchanged) {
  doc::Modeler m;const auto id=box(m);
  const auto shape=m.document().find(id)->shape;
  const auto before=geom::shapeToBRepString(shape);const auto history=m.history().undoCount();
  const auto path=output(u8"_模型 output.stl");
  ASSERT_TRUE(m.exportStl(path));const auto mesh=readStl(path);
  EXPECT_EQ(mesh.size(),12u);EXPECT_NEAR(signedVolume(mesh),6000,1e-6);
  for (const auto& triangle : mesh) EXPECT_GT(triangle.normal.Dot(gp_Vec(gp_Pnt(5,10,15),triangle.a)),0);
  std::array<double,3> max{0,0,0};for(const auto& t:mesh)for(const auto& p:{t.a,t.b,t.c})for(int k=0;k<3;++k)max[k]=std::max(max[k],p.Coord(k+1));
  EXPECT_DOUBLE_EQ(max[0],10);EXPECT_DOUBLE_EQ(max[1],20);EXPECT_DOUBLE_EQ(max[2],30);
  EXPECT_EQ(before,geom::shapeToBRepString(shape));EXPECT_EQ(m.history().undoCount(),history);
  for(TopExp_Explorer f(shape,TopAbs_FACE);f.More();f.Next()){TopLoc_Location loc;EXPECT_TRUE(BRep_Tool::Triangulation(TopoDS::Face(f.Current()),loc).IsNull());}
  ASSERT_TRUE(m.exportStl(path));EXPECT_EQ(readStl(path).size(),12u);
}
TEST(Stl, SelectionGroupsVisibilityAndWorldPlacement) {
  doc::Modeler m;auto a=box(m),b=box(m,50);auto g=m.group({a});ASSERT_TRUE(g);
  ASSERT_TRUE(m.move({*g},gp_Vec(100,200,300)));
  auto path=output(".stl");ASSERT_TRUE(m.exportStl(path,{*g,a}));auto mesh=readStl(path);
  ASSERT_EQ(mesh.size(),12u);EXPECT_NEAR(signedVolume(mesh),6000,1e-6);
  double minX=1e9;for(const auto& t:mesh)for(const auto& p:{t.a,t.b,t.c})minX=std::min(minX,p.X());EXPECT_DOUBLE_EQ(minX,100);
  ASSERT_TRUE(m.exportStl(path));EXPECT_EQ(readStl(path).size(),24u);
  ASSERT_TRUE(m.hide({b}));ASSERT_TRUE(m.exportStl(path));EXPECT_EQ(readStl(path).size(),12u);
  ASSERT_TRUE(m.createPolyline({gp_Pnt(0,0,0),gp_Pnt(10,0,0)}));ASSERT_TRUE(m.exportStl(path));EXPECT_EQ(readStl(path).size(),12u);
  auto image=m.addImageUnderlay("image-bytes","plan.png",100,100,geom::SketchPlane::ground(),100);
  ASSERT_TRUE(image);ASSERT_TRUE(m.exportStl(path));EXPECT_EQ(readStl(path).size(),12u);
  EXPECT_FALSE(m.exportStl(path,{*image}));
  auto instance=m.makeComponent({*g},"Unit");ASSERT_TRUE(instance);
  ASSERT_TRUE(m.exportStl(path,{*instance}));EXPECT_EQ(readStl(path).size(),12u);
  EXPECT_NEAR(signedVolume(readStl(path)),6000,1e-6);
  EXPECT_FALSE(m.exportStl(path,{"missing"}));
}
TEST(Stl, CurvedAccuracyChangesMeshWithoutChangingSphere) {
  const auto sphere=BRepPrimAPI_MakeSphere(10).Shape();const auto before=geom::shapeToBRepString(sphere);
  auto path=output(".stl");geom::StlExportOptions options;options.linearDeflection=0.5;
  const auto first=geom::writeStl(sphere,path,options);ASSERT_TRUE(first)<<first.message();auto coarse=readStl(path);
  options.linearDeflection=0.02;ASSERT_TRUE(geom::writeStl(sphere,path,options));auto fine=readStl(path);
  EXPECT_GT(fine.size(),coarse.size());EXPECT_NEAR(signedVolume(fine),4*std::acos(-1)*1000/3,45);
  EXPECT_EQ(before,geom::shapeToBRepString(sphere));
}
TEST(Stl, CavityAndMirroredGeometryHaveCorrectWinding) {
  const auto outer=BRepPrimAPI_MakeBox(20,20,20).Shape();
  const auto inner=BRepPrimAPI_MakeBox(gp_Pnt(5,5,5),10,10,10).Shape();
  const auto hollow=BRepAlgoAPI_Cut(outer,inner).Shape();auto path=output(".stl");
  ASSERT_TRUE(geom::writeStl(hollow,path));EXPECT_NEAR(signedVolume(readStl(path)),7000,1e-5);
  gp_Trsf mirror;mirror.SetMirror(gp_Ax2(gp_Pnt(0,0,0),gp::DX()));
  const auto mirrored=BRepBuilderAPI_Transform(hollow,mirror,true).Shape();
  ASSERT_TRUE(geom::writeStl(mirrored,path));EXPECT_NEAR(signedVolume(readStl(path)),7000,1e-5);
}
TEST(Stl, SurfaceIsAllowedButCurvesInvalidToleranceAndIoFailureAreReported) {
  doc::Modeler m;auto face=m.createRectangle(geom::SketchPlane::ground(),10,20);ASSERT_TRUE(face);
  auto path=output(".stl");ASSERT_TRUE(m.exportStl(path));EXPECT_EQ(readStl(path).size(),2u);
  auto line=m.createPolyline({gp_Pnt(0,0,0),gp_Pnt(5,0,0)});ASSERT_TRUE(line);
  const auto original=std::filesystem::file_size(std::filesystem::u8path(path));
  EXPECT_FALSE(m.exportStl(path,{*line}));EXPECT_EQ(std::filesystem::file_size(std::filesystem::u8path(path)),original);
  geom::StlExportOptions options;
  for(double d:{0.0,-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
    options.linearDeflection=d;EXPECT_FALSE(m.exportStl(path,{},options));
  }
  EXPECT_EQ(std::filesystem::file_size(std::filesystem::u8path(path)),original);
  EXPECT_FALSE(m.exportStl(output("_nonexistent_directory/out.stl")));
}
TEST(Stl, ApiIsExternalIoAndCannotRunInPreviewOrRestrictedBridge) {
  doc::Modeler m;box(m);api::CommandApi commands(m);const auto path=output(".stl");std::filesystem::remove(std::filesystem::u8path(path));
  using nlohmann::json;json request={{"command","export_stl"},{"args",{{"path",path},{"linear_deflection_mm",0.1}}}};
  EXPECT_EQ(commands.effectOf(request.dump()),api::CommandApi::Effect::ExternalIo);
  api::CommandApi restricted(m,api::CommandApi::Options{false});EXPECT_FALSE(json::parse(restricted.execute(request.dump()))["ok"].get<bool>());
  json preview={{"command","preview"},{"args",{{"commands",json::array({request})}}}};
  EXPECT_FALSE(json::parse(commands.execute(preview.dump()))["ok"].get<bool>());EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(path)));
  EXPECT_TRUE(json::parse(commands.execute(request.dump()))["ok"].get<bool>());EXPECT_EQ(readStl(path).size(),12u);
}

TEST(Stl, FarFromOriginFloatCollapseIsReportedBeforeOverwriting) {
  const auto path=output(".stl");
  { std::ofstream sentinel(std::filesystem::u8path(path)); sentinel<<"preserve existing file"; }
  const auto remote=BRepPrimAPI_MakeBox(gp_Pnt(1e10,1e10,1e10),10,20,30).Shape();
  const auto status=geom::writeStl(remote,path);EXPECT_FALSE(status);
  EXPECT_NE(status.message().find("precision collapses"),std::string::npos);
  std::ifstream input(std::filesystem::u8path(path));std::string text;std::getline(input,text);
  EXPECT_EQ(text,"preserve existing file");
}
