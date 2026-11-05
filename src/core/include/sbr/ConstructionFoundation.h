#pragma once
#include "sbr/doc/Document.h"
namespace sbr::construction {
// Read-only plan/geometry diagnostics; no soil, reinforcement or capacity verdict.
Result<std::string> checkFoundation(const doc::Document& document, const std::string& options = "{}");
// Read-only base-face overlap and gaps; does not establish load transfer/capacity.
Result<std::string> foundationSupports(const doc::Document& document, const std::string& options = "{}");
}
