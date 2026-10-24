#pragma once
// Rhino .3dm exchange through openNURBS (Phase 3).
//
// Export keeps the exact geometry: every face becomes a NURBS surface with
// its trimming curves, faces of a solid share edges, so Rhino reads closed
// polysurfaces. Names, colours and tags (as layers) are kept.
// Import reads Breps and extrusions (any unit system, converted to mm).

#include <map>
#include <string>
#include <vector>

#include "sbr/geom/ShapeIO.h"

namespace sbr::geom {

// False when the build has no openNURBS (SBR_WITH_OPENNURBS=OFF).
bool rhino3dmAvailable();

// What reading a .3dm file found: nothing is dropped silently. Objects that
// are not surfaces or solids (meshes, points, curves, annotations) are
// counted by kind; a Brep that could not be converted at all counts as
// failed, one that lost faces or trimming curves on the way as partial.
struct Import3dmReport {
  int objects = 0;       // geometry objects in the file
  int imported = 0;      // shapes returned (partial ones included)
  int failed = 0;        // Breps that gave no usable face
  int partial = 0;       // Breps that came through with pieces missing
  int skippedFaces = 0;  // faces dropped from those
  int skippedTrims = 0;  // trimming curves dropped from those
  std::map<std::string, int> unsupported;  // kind ("mesh", "point", ...) -> count
  std::string units;     // the file's unit system, e.g. "millimeters"
  double scaleToMm = 1.0;

  int unsupportedCount() const;
  // Something in the file did not come through, or not whole.
  bool incomplete() const { return failed > 0 || partial > 0 || unsupportedCount() > 0; }
  // One line, e.g. "5 objects: 2 imported (1 partially: 1 face dropped);
  // 1 failed to convert; not supported: mesh 1, point 1".
  std::string summary() const;
};

Status write3dm(const std::vector<NamedShape>& parts, const std::string& path);
// Reads the surfaces and solids. `report` (optional) is filled whether the
// read succeeds or not.
Result<std::vector<NamedShape>> read3dm(const std::string& path, Import3dmReport* report = nullptr);

}  // namespace sbr::geom
