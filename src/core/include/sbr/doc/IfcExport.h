// IFC export (docs/90 G-13, docs/101): the building parts of a model as an IFC4 file other
// BIM software can open - storeys from the levels, walls with their openings, slabs, roofs,
// columns / beams / members, rooms as spaces, grids, and other solids as proxies.
#pragma once

#include <string>
#include <vector>

#include "sbr/Types.h"

namespace sbr::doc {

class Modeler;

struct IfcSummary {
  int stairs = 0;
  int storeys = 0, walls = 0, slabs = 0, roofs = 0, columns = 0, beams = 0, members = 0;
  int openings = 0, windows = 0, doors = 0, spaces = 0, grids = 0, proxies = 0;
  int skipped = 0;                 // visible objects without volume (lines, faces, notes, guides)
  std::vector<std::string> notes;  // what was assumed or left out, for the user
};

// The file's text (ISO 10303-21, schema IFC4). `fileName` goes in its header.
Result<std::string> ifcText(const Modeler& modeler, const std::string& fileName, IfcSummary* summary = nullptr);
// Writes it to `path`.
Result<IfcSummary> exportIfc(const Modeler& modeler, const std::string& path);

}  // namespace sbr::doc
