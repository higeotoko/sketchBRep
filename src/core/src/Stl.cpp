#include "sbr/geom/ShapeIO.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <BRepCheck_Analyzer.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include "sbr/display/DisplayMesh.h"

namespace sbr::geom {
namespace {
void littleEndian(std::ostream& out, std::uint32_t value) {
  char bytes[4];
  for (int i = 0; i < 4; ++i) bytes[i] = static_cast<char>((value >> (8 * i)) & 255);
  out.write(bytes, 4);
}
void binaryFloat(std::ostream& out, float value) {
  static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
  std::uint32_t bits; std::memcpy(&bits, &value, 4); littleEndian(out, bits);
}
}

Status writeStl(const TopoDS_Shape& canonical, const std::string& path, const StlExportOptions& options) {
  if (!std::isfinite(options.linearDeflection) || options.linearDeflection < 0.001 ||
      !std::isfinite(options.angularDeflection) || options.angularDeflection < 0.01 || options.angularDeflection > 3.0)
    return Status::failure("STL tolerance must be at least 0.001 mm and angle between 0.01 and 3 radians");
  if (canonical.IsNull() || !TopExp_Explorer(canonical, TopAbs_FACE).More())
    return Status::failure("STL export requires faces or solids");
  try {
    if (!BRepCheck_Analyzer(canonical).IsValid()) return Status::failure("Cannot export invalid geometry to STL");
    const auto shape = display::makeDisplayCopy(canonical);
    BRepMesh_IncrementalMesh mesher(shape, options.linearDeflection, Standard_False,
                                   options.angularDeflection, Standard_True);
    if (!mesher.IsDone()) return Status::failure("STL tessellation failed");
    std::vector<std::array<float, 12>> triangles;
    for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
      const auto face = TopoDS::Face(it.Current());
      TopLoc_Location location;
      const auto mesh = BRep_Tool::Triangulation(face, location);
      if (mesh.IsNull() || mesh->NbTriangles() == 0) return Status::failure("STL tessellation failed on a face");
      const auto transform = location.Transformation();
      const bool reverse = (face.Orientation() == TopAbs_REVERSED) != transform.IsNegative();
      const auto faceStart = triangles.size();
      for (int t = 1; t <= mesh->NbTriangles(); ++t) {
        int a,b,c; mesh->Triangle(t).Get(a,b,c);
        if (reverse) std::swap(b,c);
        const std::array<gp_Pnt,3> points{mesh->Node(a).Transformed(transform),
            mesh->Node(b).Transformed(transform),mesh->Node(c).Transformed(transform)};
        // OCCT can emit zero-area triangles at sphere poles. They cover no
        // surface; distinguish them from valid triangles lost to STL floats.
        if (gp_Vec(points[0],points[1]).Crossed(gp_Vec(points[0],points[2])).SquareMagnitude()==0) continue;
        std::array<float, 12> values{};
        int k = 3;
        for (const auto& p : points) {
          for (double coordinate : {p.X(),p.Y(),p.Z()}) {
            if (!std::isfinite(coordinate) || std::abs(coordinate) > std::numeric_limits<float>::max())
              return Status::failure("STL coordinates exceed the supported range");
            values[k++] = static_cast<float>(coordinate);
          }
        }
        const gp_Pnt pa(values[3],values[4],values[5]), pb(values[6],values[7],values[8]), pc(values[9],values[10],values[11]);
        auto normal = gp_Vec(pa,pb).Crossed(gp_Vec(pa,pc));
        if (normal.SquareMagnitude() == 0)
          return Status::failure("STL float precision collapses a triangle; move the model closer to the origin or use STEP");
        normal.Normalize();
        values[0]=static_cast<float>(normal.X()); values[1]=static_cast<float>(normal.Y()); values[2]=static_cast<float>(normal.Z());
        if (triangles.size() == std::numeric_limits<std::uint32_t>::max())
          return Status::failure("STL has too many triangles");
        triangles.push_back(values);
      }
      if (triangles.size()==faceStart) return Status::failure("STL tessellation failed on a face");
    }
    if (triangles.empty()) return Status::failure("STL export requires faces or solids");
    // Complete and validate the mesh before opening the destination file.
    std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!out) return Status::failure("Could not write STL file: " + path);
    std::array<char,80> header{};
    const char label[]="SketchBRep binary STL; coordinates in mm";
    std::memcpy(header.data(),label,sizeof(label)-1);
    out.write(header.data(),header.size()); littleEndian(out,static_cast<std::uint32_t>(triangles.size()));
    for (const auto& triangle : triangles) {
      for (float value : triangle) binaryFloat(out,value);
      const char attributes[2]={0,0}; out.write(attributes,2);
    }
    out.close();
    if (!out) return Status::failure("Could not finish writing STL file: " + path);
    return Status::success();
  } catch (const Standard_Failure& e) {
    return Status::failure(std::string("STL export failed: ") + e.GetMessageString());
  } catch (const std::exception& e) {
    return Status::failure(std::string("STL export failed: ") + e.what());
  }
}
}  // namespace sbr::geom
