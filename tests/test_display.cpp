#include <gtest/gtest.h>

#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>

#include "sbr/display/DisplayMesh.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/ShapeIO.h"

using namespace sbr;

namespace {

bool hasTriangulation(const TopoDS_Shape& shape) {
  for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
    TopLoc_Location loc;
    if (!BRep_Tool::Triangulation(TopoDS::Face(it.Current()), loc).IsNull()) return true;
  }
  return false;
}

}  // namespace

TEST(DisplayMesh, QualityChangesMeshButNotGeometry) {
  const TopoDS_Shape cylinder = BRepPrimAPI_MakeCylinder(500, 3000).Shape();
  const double exactVolume = geom::volume(cylinder);

  const auto coarse = display::buildDisplayMesh(cylinder, display::DisplayQuality::coarse());
  const auto fine = display::buildDisplayMesh(cylinder, display::DisplayQuality::fine());
  EXPECT_GT(coarse.triangleCount(), 0u);
  EXPECT_GT(fine.triangleCount(), coarse.triangleCount() * 4);
  EXPECT_EQ(coarse.normals.size(), coarse.positions.size());
  EXPECT_EQ(coarse.edgePolylines.size(), 3u);  // two circles + seam

  // The canonical shape never receives a triangulation and keeps exact volume.
  EXPECT_FALSE(hasTriangulation(cylinder));
  EXPECT_DOUBLE_EQ(geom::volume(cylinder), exactVolume);
  EXPECT_EQ(geom::shapeToBRepString(cylinder).find("Triangulations 1"), std::string::npos);
}

TEST(DisplayMesh, DisplayCopyIsIndependent) {
  const TopoDS_Shape cylinder = BRepPrimAPI_MakeCylinder(500, 3000).Shape();
  const TopoDS_Shape copy = display::makeDisplayCopy(cylinder);
  EXPECT_FALSE(copy.IsSame(cylinder));
  EXPECT_DOUBLE_EQ(geom::volume(copy), geom::volume(cylinder));
}
