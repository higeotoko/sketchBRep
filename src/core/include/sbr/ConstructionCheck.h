#pragma once
#include <atomic>
#include "sbr/doc/Document.h"

namespace sbr::construction {
// Read-only, repeatable diagnosis of a document snapshot. JSON carries four
// outcomes, reasons, measurements and provenance; never a modelling gate.
Result<std::string> checkFrame(const doc::Document& document, const std::string& options = "{}",
                               const std::atomic<bool>* cancelled = nullptr);
}
