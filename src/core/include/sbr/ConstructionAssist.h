#pragma once
#include "sbr/doc/Document.h"

namespace sbr::construction {
// Read-only structural figures taken from the model's shape (docs/87), for the
// designer to check and then enter into the wall calculation:
//  - wind projected areas per storey and direction (the part up to FL + 1.35 m left out),
//  - floor-slab areas and plan extents per storey,
//  - column alignment (the share of upper posts with a post directly below) and the
//    posts standing on beams, with what they stand on,
//  - beam spans between the supports found under / at the ends of each beam, with
//    span / depth (judged only against a limit the caller gives with its basis).
// Geometry only: no loads, joints, sections or code compliance are assessed.
Result<std::string> structureAssist(const doc::Document& document, const std::string& options = "{}");
}  // namespace sbr::construction
