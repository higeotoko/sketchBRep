#pragma once
#include "sbr/doc/Document.h"

namespace sbr::doc { class Modeler; }

namespace sbr::construction {
// Read-only geometric intervals remaining after rectangular openings.
// These candidates do not establish supports, specifications or wall quantity.
Result<std::string> wallBayCandidates(const doc::Modeler& modeler, const std::string& options = "{}");
// Data-only wall designations. Missing inputs stay unknown; reports never
// establish the adequacy of the building or gate a geometry edit.
std::string wallTables();
// Validates and pins the complete coefficient-table snapshot, including custom
// provenance, so a future embedded-table update cannot change saved projects.
std::string normalizeWallInputs(const std::string& inputs);
Result<std::string> checkWallQuantity(const doc::Document& document, const std::string& options = "{}");
std::string wallQuantityCsv(const std::string& report);
}  // namespace sbr::construction
