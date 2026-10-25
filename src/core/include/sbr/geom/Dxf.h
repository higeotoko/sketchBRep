#pragma once

#include <map>
#include "sbr/geom/ShapeIO.h"

namespace sbr::geom {

struct DxfImportOptions {
  // Used only when $INSUNITS is absent or zero. Model coordinates are mm.
  double unitlessScaleToMm = 1.0;
  bool allowPartial = false;
};

struct DxfImportReport {
  int imported = 0;
  int paperSpace = 0;
  bool unitsKnown = false;
  double scaleToMm = 1.0;
  std::map<std::string, int> omitted;
  bool incomplete() const { return !omitted.empty(); }
  std::string summary() const;
};

// ASCII DXF model space; curves remain exact edges/wires, never tessellated.
// Unsupported entities are counted. The Modeler decides whether to commit.
Result<std::vector<NamedShape>> readDxf(const std::string& path, const DxfImportOptions& options = {},
                                      DxfImportReport* report = nullptr);

}  // namespace sbr::geom
