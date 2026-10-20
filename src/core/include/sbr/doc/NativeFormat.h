#pragma once
// Native project file (*.sbr): JSON document + embedded geometry-only BRep.
//
// The parametric recipe is the source of truth for editing; the embedded BRep
// is the exact geometry at save time. On load the recipe is regenerated and
// compared against the stored geometry; on mismatch or failure the stored
// geometry wins and a warning is reported.

#include <string>
#include <vector>

#include "sbr/doc/Document.h"

namespace sbr::doc {

constexpr int kNativeFormatVersion = 2;
constexpr const char* kNativeFormatMagic = "SketchBRep";

std::string serializeDocument(const Document& document);
Status deserializeDocument(const std::string& text, Document& document,
                           std::vector<std::string>* warnings = nullptr);

// Atomic save: writes "<path>.tmp", verifies it parses, then renames.
Status saveDocument(const Document& document, const std::string& path);
Status loadDocument(Document& document, const std::string& path,
                    std::vector<std::string>* warnings = nullptr);

}  // namespace sbr::doc
