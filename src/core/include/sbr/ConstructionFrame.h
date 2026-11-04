#pragma once
#include "sbr/doc/Document.h"

namespace sbr::construction {
// Fingerprints editable state, never a cached B-rep or the frame's manifest.
std::string frameFingerprint(const doc::Object& object);
std::string frameDefinitionFingerprint(const doc::ComponentDefinition& definition);
// Level / grid following for a generated shared part. The caller supplies
// world endpoint displacements and this placement's current world frame.
Status moveFrameInstance(doc::Object& object, const doc::Document& document, const gp_Trsf& world,
                         const gp_Vec& start, const gp_Vec& end);
}  // namespace sbr::construction
