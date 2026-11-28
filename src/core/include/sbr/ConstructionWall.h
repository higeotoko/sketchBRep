#pragma once
#include "sbr/doc/Document.h"
#include <map>
#include <string>

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
// docs/332: the wall symbols (壁記号 A, B, ...) of the specifications the report's walls use, as the MLIT manual's
// 耐力壁図 marks the walls and lists them in its 耐力壁仕様一覧: by wall ratio, lowest first (then the spec id).
std::map<std::string, std::string> wallSymbols(const std::string& report);
// docs/358: 柱頭・柱脚の接合方法の N 値計算法 (令47条1項, 平12建告1460号) for the frame's columns at the bearing walls of
// the wall calculation, the braces' correction (表3-14) from the frame's braces. JSON: status, columns [{floor, at,
// column, corner, second_floor_above, directions {X, Y: wall_left, wall_right, brace_left, brace_right, correction,
// A1, B1, H1/2.7, A2, B2, H2/2.7, L, N}, N, joint, joint_name, head, foot, status, reasons}], joints, notes, basis.
// Missing inputs are not evaluated with the reason; no product is chosen.
Result<std::string> columnNValues(const doc::Document& document);
}  // namespace sbr::construction
