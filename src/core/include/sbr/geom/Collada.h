#pragma once
// COLLADA (.dae) exchange (docs/46), the way to and from SketchUp Pro:
// SketchUp reads and writes .dae, so a model goes SketchBRep -> .dae ->
// SketchUp (File > Import) and SketchUp (File > Export > 3D Model) ->
// .dae -> SketchBRep.
//
// Export: every part as a triangle mesh (shared corners, so SketchUp joins
// the triangles into faces again when "merge coplanar faces" is on),
// coloured per face, in millimetres, Z up; loose lines and curves as lines.
// Import: meshes (triangles, polylists, polygons) and lines of every node,
// placed by the node transforms (components flattened), in any unit and up
// axis; coplanar triangles are joined back into faces and closed meshes
// become solids. Colours come from the materials' diffuse colour (a
// texture gives its material's colour or grey, counted in the report).

#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <TopoDS_Shape.hxx>

#include "sbr/Types.h"

namespace sbr::geom {

struct ColladaColor {
  double r = 0.8, g = 0.8, b = 0.8, a = 1.0;
  bool operator==(const ColladaColor& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
  bool operator<(const ColladaColor& o) const {
    return std::array<double, 4>{r, g, b, a} < std::array<double, 4>{o.r, o.g, o.b, o.a};
  }
};

// One object: its world shape (faces and / or loose edges), a name, the
// colour of the whole and of faces painted on their own (1-based index in
// TopExp::MapShapes(shape, FACE) order).
struct ColladaPart {
  TopoDS_Shape shape;
  std::string name;
  ColladaColor color;
  std::vector<std::pair<int, ColladaColor>> faceColors;
};

struct ColladaExportOptions {
  // Curved faces are cut into flat pieces no further than this from the
  // surface (mm) and no more than this angle apart (radians): about
  // SketchUp's 24 segments a circle.
  double linearDeflection = 1.0;
  double angularDeflection = 0.26;
};

Status writeCollada(const std::vector<ColladaPart>& parts, const std::string& path,
                    const ColladaExportOptions& options = {});

// What reading found; nothing is dropped silently.
struct ColladaReport {
  std::string units;       // the file's unit name ("inch", "meter", ...)
  double scaleToMm = 1.0;
  std::string upAxis = "Z_UP";
  int parts = 0;           // shapes returned
  int triangles = 0;       // faces read (before joining)
  int lines = 0;           // line segments read
  int solids = 0;          // parts that closed into solids
  int degenerate = 0;      // faces too small or not flat, left out
  int textured = 0;        // materials with a texture (their colour used)
  std::map<std::string, int> unsupported;  // e.g. "tristrips" -> count
  std::string summary() const;
};

Result<std::vector<ColladaPart>> readCollada(const std::string& path, ColladaReport* report = nullptr);

}  // namespace sbr::geom
