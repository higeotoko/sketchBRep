// Investigation only: this probe is not connected to the application.
// Exact cylindrical faces, straight generators, and copied circular edges.
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Version.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Circ.hxx>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
  return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
}
int countShapes(const TopoDS_Shape& shape,TopAbs_ShapeEnum kind) {
  int n=0;for(TopExp_Explorer e(shape,kind);e.More();e.Next())++n;return n;
}
double volume(const TopoDS_Shape& shape) {
  GProp_GProps p;BRepGProp::VolumeProperties(shape,p);return p.Mass();
}
double area(const TopoDS_Shape& shape) {
  GProp_GProps p;BRepGProp::SurfaceProperties(shape,p);return p.Mass();
}
int main(int argc,char**argv) {
  const int repeats=argc>1?std::stoi(argv[1]):3;
  if(repeats<1||repeats>10)return 2;
  std::cout<<"# OCCT "<<OCC_VERSION_COMPLETE<<"; milliseconds; no UI, history or rendering\n";
  std::cout<<"cuts,faces,run,split_ms,validate_ms,total_ms,volume_relative_error,area_relative_error,valid\n";
  std::cout<<std::setprecision(10);
  for(int n:{3,6,12,24,48,96})for(int run=1;run<=repeats;++run) {
    const auto base=BRepPrimAPI_MakeCylinder(1000,3000).Shape();
    const auto v0=volume(base),a0=area(base);
    TopTools_ListOfShape args,tools;args.Append(base);
    // Vertical lines lie exactly on the side, rather than chords inside it.
    for(int i=1;i<=n;++i) {
      const double angle=2*std::acos(-1)*i/(n+1);
      const gp_Pnt a(1000*std::cos(angle),1000*std::sin(angle),0);
      const gp_Pnt b(a.X(),a.Y(),3000);
      tools.Append(BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(a,b).Edge()).Wire());
    }
    // Same exact circles as copying the cylinder's top edge down its axis.
    for(int i=1;i<=n;++i) {
      const gp_Circ circle(gp_Ax2(gp_Pnt(0,0,3000.0*i/(n+1)),gp::DZ()),1000);
      tools.Append(BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(circle).Edge()).Wire());
    }
    const auto start=Clock::now();
    BRepAlgoAPI_Splitter splitter;
    splitter.SetArguments(args);splitter.SetTools(tools);splitter.SetNonDestructive(true);splitter.Build();
    const auto splitMs=elapsed(start);
    if(!splitter.IsDone()){std::cerr<<"split failed\n";return 1;}
    const auto result=splitter.Shape();const auto validateStart=Clock::now();
    const auto faceCount=countShapes(result,TopAbs_FACE);
    const auto ve=std::abs(volume(result)-v0)/v0,ae=std::abs(area(result)-a0)/a0;
    const bool valid=BRepCheck_Analyzer(result).IsValid()&&countShapes(result,TopAbs_SOLID)==1&&
        faceCount==(n+1)*(n+1)+2&&ve<1e-8&&ae<1e-8;
    const auto validateMs=elapsed(validateStart);
    std::cout<<2*n<<','<<faceCount<<','<<run<<','<<splitMs<<','<<validateMs<<','
        <<splitMs+validateMs<<','<<ve<<','<<ae<<','<<valid<<std::endl;
    if(!valid)return 1;
  }
}
