#pragma once
#include "sbr/doc/Document.h"

namespace sbr::construction {
// Counts placed members, not shared definitions or individual B-rep solids.
// Unknown sizes remain null; the document and its undo history are untouched.
Result<std::string> takeOff(const doc::Document& document, const std::string& options = "{}");
std::string takeOffCsv(const std::string& report);
}
