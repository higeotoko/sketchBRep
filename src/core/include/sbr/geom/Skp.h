#pragma once
// SketchUp (.skp) export and import (docs/181) through Trimble's SketchUp C API.
//
// The SDK is not in Git: a build made with SBR_SKETCHUP_SDK_DIR compiles against its headers,
// and at run time SketchUpAPI.dll is loaded (never linked): first the one next to the program
// (the ZIP carries the SDK's DLLs since docs/182, the owner's decision of 2026-11-18), then the
// SDK folder (the environment variable SBR_SKETCHUP_SDK, else the folder the build was made
// with). Without it, export and import say so and do nothing.
//
// Export: each part a group named as the object; flat faces as SketchUp faces with their holes,
// curved ones cut into triangles whose inner edges are soft and smooth; colours as materials
// (the part's on the group, faces painted on their own on those faces); loose lines as edges.
// Inches inside the file (SketchUp's), millimetres here.
// Import: every face of the model, its groups and its components (placed by their
// transformations, nested), each group / component / the loose model an object; closed ones
// solids; colours from the faces' materials (else the group's); loose edges as lines.

#include <string>
#include <vector>

#include "sbr/Types.h"
#include "sbr/geom/Collada.h"

namespace sbr::geom {

// Whether SketchUp files can be read and written here; `why` says what is missing.
bool skpAvailable(std::string* why = nullptr);
// The SketchUpAPI.dll in use (empty when none).
std::string skpLibrary();

struct SkpExportOptions {
  // Curved faces are cut into flat pieces no further than this from the surface (mm) and no
  // more than this angle apart (radians).
  double linearDeflection = 1.0;
  double angularDeflection = 0.26;
};

Status writeSkp(const std::vector<ColladaPart>& parts, const std::string& path, const SkpExportOptions& options = {});

// What reading found; nothing is dropped silently.
struct SkpReport {
  int groups = 0;      // groups read
  int instances = 0;   // component instances read
  int faces = 0;       // SketchUp faces read
  int lines = 0;       // loose edges read
  int parts = 0;       // objects returned
  int solids = 0;      // of them, closed into solids
  int leftOut = 0;     // faces that could not be made (too small, not flat)
  int hidden = 0;      // hidden groups / instances / faces left out
  int textured = 0;    // materials with a texture (their colour used)
  // The objects the file was written from (docs/186): their ids, as SketchBRep wrote them.
  std::vector<std::string> exportedIds;
  std::string summary() const;
};

Result<std::vector<ColladaPart>> readSkp(const std::string& path, SkpReport* report = nullptr);

// For tests and tools (docs/186): in a SketchUp file written by writeSkp, the top-level group
// carrying the object `id` moved by `move` (world, mm) on top of where it is, or erased; saved
// in place. As if done in SketchUp.
Status skpEditGroup(const std::string& path, const std::string& id, const gp_Trsf* move, bool erase);

}  // namespace sbr::geom
