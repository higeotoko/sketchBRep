#pragma once
// Parametric recipe of an object: a base feature followed by modifiers.
//
// The recipe is the editable modelling history stored in the Document layer.
// Regenerating it yields the canonical B-rep shape (Geometry layer).

#include <string>
#include <variant>
#include <vector>

#include <gp_Pnt.hxx>
#include <TopoDS_Shape.hxx>

#include "sbr/Types.h"
#include "sbr/geom/FaceRef.h"
#include "sbr/geom/Profile.h"

namespace sbr::doc {

// --- Base features --------------------------------------------------------
struct RectangleProfile {
  geom::SketchPlane plane;
  double width = 0.0;   // along plane.xDir, signed
  double height = 0.0;  // along plane.yDir, signed
};

struct PolygonProfile {
  std::vector<gp_Pnt> points;  // closed implicitly
};

struct PolylineCurve {
  std::vector<gp_Pnt> points;  // open
};

// Non-parametric geometry (STEP import, results of future direct edits).
struct ImportedShape {
  TopoDS_Shape shape;
  std::string source;
};

using BaseFeature = std::variant<RectangleProfile, PolygonProfile, PolylineCurve, ImportedShape>;

// --- Modifiers ------------------------------------------------------------
// Extrudes the profile face along its normal. Must be the first modifier.
struct ExtrudeOp {
  double distance = 0.0;  // signed, relative to the profile normal
};

// Push/pull of a planar face of the current solid.
struct PushPullOp {
  geom::FaceRef face;
  double distance = 0.0;  // signed, along the outward face normal
};

using Modifier = std::variant<ExtrudeOp, PushPullOp>;

struct Recipe {
  BaseFeature base;
  std::vector<Modifier> modifiers;
};

// A numeric parameter exposed for re-editing (UI panel, scripting, AI API).
struct Parameter {
  std::string key;    // stable key, e.g. "width", "extrude.distance", "op1.distance"
  std::string label;  // human readable
  double value = 0.0;
  bool isLength = true;
};

// Rebuilds the shape from scratch. Every intermediate result is validated.
Result<TopoDS_Shape> regenerate(const Recipe& recipe);

std::vector<Parameter> parameters(const Recipe& recipe);
// Changes a parameter in place. Does not regenerate.
Status setParameter(Recipe& recipe, const std::string& key, double value);

std::string describe(const Recipe& recipe);

}  // namespace sbr::doc
