#pragma once
// Rhino .3dm exchange through openNURBS (Phase 3).
//
// Export keeps the exact geometry: every face becomes a NURBS surface with
// its trimming curves, faces of a solid share edges, so Rhino reads closed
// polysurfaces. Names, colours and tags (as layers) are kept.
// Import reads Breps and extrusions (any unit system, converted to mm).

#include <string>
#include <vector>

#include "sbr/geom/ShapeIO.h"

namespace sbr::geom {

// False when the build has no openNURBS (SBR_WITH_OPENNURBS=OFF).
bool rhino3dmAvailable();

Status write3dm(const std::vector<NamedShape>& parts, const std::string& path);
Result<std::vector<NamedShape>> read3dm(const std::string& path);

}  // namespace sbr::geom
