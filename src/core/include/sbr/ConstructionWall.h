#pragma once
#include "sbr/doc/Document.h"

namespace sbr::construction {
// Data-only wall designations. Missing inputs stay unknown; reports never
// establish the adequacy of the building or gate a geometry edit.
std::string wallTables();
// Validates and pins the complete coefficient-table snapshot, including custom
// provenance, so a future embedded-table update cannot change saved projects.
std::string normalizeWallInputs(const std::string& inputs);
Result<std::string> checkWallQuantity(const doc::Document& document, const std::string& options = "{}");
std::string wallQuantityCsv(const std::string& report);
}  // namespace sbr::construction
