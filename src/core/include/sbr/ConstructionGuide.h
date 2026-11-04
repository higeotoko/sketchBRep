#pragma once
// Embedded construction tables and project overrides (docs/52, stage 1).
// JSON crosses this boundary as text, like CommandApi. Invalid data throws
// std::runtime_error; callers validate before changing the document.
#include <string>
#include "sbr/geom/Section.h"

namespace sbr::construction {
std::string guideData(const std::string& system, const std::string& overrides = "{}");
std::string guideMarkdown(const std::string& system, const std::string& overrides = "{}");
// Overrides are a merge patch of the sections / roles / presets tables;
// each table's entries are keyed by stable ids. Source metadata is required.
void validateOverrides(const std::string& system, const std::string& overrides);
geom::Section section(const std::string& id, const std::string& overrides = "{}");
const geom::CatalogSection* findSection(const std::string& id);
}  // namespace sbr::construction
