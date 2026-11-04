#pragma once
// Jw_cad (.jww) drawings in (docs/50, docs/24): lines, arcs, circles and
// ellipses at real size (Jw keeps paper millimetres; each layer group's
// scale makes them real), block instances expanded, dimensions' lines kept.
// Text, points and solid fills are counted, not read. Layers come as
// "group/layer" names (Jw's own names, else their numbers).

#include <map>
#include <string>
#include <vector>

#include "sbr/Types.h"
#include "sbr/geom/ShapeIO.h"

namespace sbr::geom {

struct JwwImportOptions {
  bool allowPartial = false;   // read what can be read when some kinds cannot
  bool groupByLayer = false;   // one object per layer (large drawings)
};

struct JwwImportReport {
  unsigned version = 0;        // Jw's data version (600 = Ver.6.00)
  int imported = 0;            // curves read
  std::map<std::string, int> omitted;  // "text" -> count, ...
  bool incomplete() const { return !omitted.empty(); }
  std::string summary() const;
};

Result<std::vector<NamedShape>> readJww(const std::string& path, const JwwImportOptions& options = {},
                                        JwwImportReport* report = nullptr);

}  // namespace sbr::geom
