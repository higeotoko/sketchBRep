#pragma once
// Native project file (*.sbr): JSON document + embedded geometry-only BRep.
//
// The parametric recipe is the source of truth for editing; the embedded BRep
// is the exact geometry at save time. On load the recipe is regenerated and
// compared against the stored geometry; on mismatch or failure the stored
// geometry wins and a warning is reported.

#include <optional>
#include <string>
#include <vector>

#include "sbr/doc/Document.h"

namespace sbr::doc {

// 3: geometry shared by several objects (component placements) is stored
// once in a "shapes" table and referenced; versions 1 and 2 are still read.
constexpr int kNativeFormatVersion = 3;
constexpr const char* kNativeFormatMagic = "SketchBRep";

std::string serializeDocument(const Document& document);
// regenerate = false skips re-running the recipes (stored geometry is
// used as is): a structural check of a file just written.
Status deserializeDocument(const std::string& text, Document& document,
                           std::vector<std::string>* warnings = nullptr, bool regenerate = true);

// Whether a recipe still regenerates the stored geometry; a warning text
// (like the ones loading reports) if not. Used to verify recipes after a
// fast load, a few objects at a time.
std::optional<std::string> checkRecipe(const Object& object, const RegenContext& context);

// Atomic save: writes "<path>.tmp", verifies it parses, then renames.
Status saveDocument(const Document& document, const std::string& path);
// Before overwriting `path`: keeps the previous `generations` versions as
// "<path>.bak1" (newest) .. "<path>.bakN". Nothing to do when `path` does
// not exist yet. The current file is copied, not moved, so a failing save
// still leaves it in place.
Status rotateBackups(const std::string& path, int generations);
std::string backupPath(const std::string& path, int generation);
Status loadDocument(Document& document, const std::string& path,
                    std::vector<std::string>* warnings = nullptr, bool regenerate = true);

}  // namespace sbr::doc
