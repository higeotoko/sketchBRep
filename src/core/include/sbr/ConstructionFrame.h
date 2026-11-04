#pragma once
#include <gp_Ax1.hxx>
#include "sbr/doc/Document.h"

namespace sbr::construction {
// World reference line of a known wall-tool recipe (including subtracts).
// Unknown edited recipes are not inferred from their bounding box.
std::optional<gp_Ax1> wallReferenceLine(const doc::Object& wall, const doc::Document& document);
// Expands a single-member placement to its design recipe (including its own
// dimensions and anchor). No hierarchy or document state is changed.
std::optional<doc::Object> frameMemberObject(const doc::Document& document, const doc::Object& instance);
// Fingerprints editable state, never a cached B-rep or the frame's manifest.
std::string frameFingerprint(const doc::Object& object);
std::string frameDefinitionFingerprint(const doc::ComponentDefinition& definition);
// Level / grid following for a generated shared part. The caller supplies
// world endpoint displacements and this placement's current world frame.
Status moveFrameInstance(doc::Object& object, const doc::Document& document, const gp_Trsf& world,
                         const gp_Vec& start, const gp_Vec& end);
}  // namespace sbr::construction
