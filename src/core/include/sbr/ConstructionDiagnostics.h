#pragma once
#include "sbr/doc/Document.h"

namespace sbr::construction {
// Read-only summary for an on-screen structural overlay: the wall quantity rows
// (existing / required per storey and direction, with the table caveat), the
// designated walls with world coordinates, and markers from the frame check and
// the support-path check. Unimplemented checks are listed as unassessed, never
// as passing. Nothing here changes the model or gates an edit.
Result<std::string> structuralDiagnostics(const doc::Document& document, const std::string& options = "{}");
}  // namespace sbr::construction
