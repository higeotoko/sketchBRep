#pragma once
// Parametric recipe of an object: a base feature followed by modifiers.
//
// The recipe is the editable modelling history stored in the Document layer.
// Regenerating it yields the canonical B-rep shape (Geometry layer).

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gp.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
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

struct CircleProfile {
  geom::SketchPlane plane;  // origin = centre
  double radius = 0.0;
};

// Regular polygon: 'sides' corners on a circle of 'radius' (the first one
// along plane.xDir). Radius and sides stay editable.
struct RegularPolygonProfile {
  geom::SketchPlane plane;  // origin = centre
  double radius = 0.0;
  int sides = 6;
};

struct PolygonProfile {
  std::vector<gp_Pnt> points;  // closed implicitly
};

struct PolylineCurve {
  std::vector<gp_Pnt> points;  // open
};

// Exact circular arc (2-point arc + bulge). closed = segment face.
struct ArcCurve {
  gp_Pnt start;
  gp_Pnt end;
  gp_Dir normal = gp::DZ();  // plane of the arc
  double bulge = 0.0;        // signed sagitta, see geom::makeArc
  bool closed = false;
};

// Annotations. Text empty = automatic (measured length).
struct LinearDimension {
  gp_Pnt start;
  gp_Pnt end;
  gp_Vec offset;  // moves the dimension line away from the measured points
  std::string text;
};

struct TextNote {
  gp_Pnt position;              // where the text sits
  std::optional<gp_Pnt> target; // leader start (a point on the model)
  std::string text;
};

// Construction line (SketchUp "guide"): an infinite line to snap to while
// drawing. Not model geometry: excluded from exports and from "fit all".
struct GuideLine {
  gp_Pnt point;
  gp_Dir direction = gp::DX();
};

// A picture laid on a plane to draw over (a plan, an elevation): the
// image is held by the document (content hash), the object is the
// rectangle it covers. Kept apart from profiles: not extruded or painted.
struct ImageUnderlay {
  std::string image;        // sha-256 of the image file's bytes
  geom::SketchPlane plane;  // lower-left corner, normal, image x direction
  double width = 1000.0;    // mm
  double height = 1000.0;   // mm
  double opacity = 1.0;
};

// Cubic NURBS curve through points (periodic when closed).
struct InterpolatedCurve {
  std::vector<gp_Pnt> points;
  bool closed = false;
};

// Non-parametric geometry (STEP import, results of future direct edits).
struct ImportedShape {
  TopoDS_Shape shape;
  std::string source;
};

struct Recipe;

enum class BooleanKind { Union, Subtract, Intersect };
const char* toString(BooleanKind kind);

// One input of a Boolean: its own (editable) recipe and its world placement
// at the time the Boolean was created.
struct BooleanOperand {
  std::shared_ptr<const Recipe> recipe;
  gp_Trsf placement;
  std::string name;
};

// Parametric Boolean: operands are regenerated before combining, so editing
// an operand parameter (e.g. "in1.op0.distance") recomputes the result.
// Subtract removes operands 1..n from operand 0.
struct BooleanFeature {
  BooleanKind kind = BooleanKind::Union;
  std::vector<BooleanOperand> operands;
};

// Parametric loft through section profiles (faces or curves), in order.
// Sections are snapshots like Boolean operands, editable as "in<i>.<key>".
struct LoftFeature {
  std::vector<BooleanOperand> sections;
  bool solid = true;
};

// Parametric sweep: inputs[0] = profile (face or curve), inputs[1] = path.
struct SweepFeature {
  std::vector<BooleanOperand> inputs;
};

// Face bounded by curves (lines, arcs, NURBS) chained into a closed loop.
// The curves are snapshots, so e.g. an arc's bulge stays editable
// ("in<i>.bulge").
struct LoopProfile {
  std::vector<BooleanOperand> curves;
  // Faces away from the default (+Z / +Y / +X) side, e.g. to face outwards
  // when the outline was drawn on a solid's face.
  bool reversed = false;
};

// One part of a profile face split by a line drawn across it: the original
// face's recipe (source[0], editable as "in0.<key>"), the split line and a
// point inside this part (parent coordinates).
struct FacePart {
  std::vector<BooleanOperand> source;
  std::vector<gp_Pnt> line;
  gp_Pnt inside;
};

// Faces drawn one by one that close a volume between them (SketchUp: six
// rectangles round a box are a box): the faces' own recipes (each still
// editable as "in<i>.<key>"), joined along their shared edges into one
// closed solid. Regenerating fails rather than give a shape with gaps.
struct SewnFeature {
  std::vector<BooleanOperand> faces;
};

// A parameter of one object of a component definition, changed for one
// placement only (a door twice as wide, the same component otherwise).
struct ParameterOverride {
  std::string object;  // object id inside the definition
  std::string key;     // parameter key of that object
  double value = 0.0;
  bool operator==(const ParameterOverride& o) const {
    return object == o.object && key == o.key && value == o.value;
  }
};

// Container node of the object hierarchy. Has no geometry of its own.
struct GroupNode {
  // Set while this group is a component being edited in place (spec 07
  // §6.2): the definition's objects are its children until editing ends,
  // then they become the definition again and the instance comes back.
  std::string editingDefinition;
  std::string editingInstance;      // id of the instance replaced meanwhile
  std::string editingInstanceName;
  // The placement's own settings, given back when editing ends.
  std::vector<ParameterOverride> editingOverrides;
  bool editingMirrored = false;
};

// Scale along three directions of a frame (object coordinates), each by its
// own factor, about `center` (SketchUp's scale grips: width, depth or
// height alone, or several). Exact: the B-rep geometry is transformed
// (planes stay planes, circles become ellipses as exact NURBS). Factors
// are positive (mirroring is a separate operation).
struct StretchOp {
  gp_Pnt center;
  gp_Dir xDir{1, 0, 0};
  gp_Dir yDir{0, 1, 0};  // zDir = xDir x yDir
  double fx = 1.0, fy = 1.0, fz = 1.0;
};

// Placement of a shared component definition.
struct ComponentInstance {
  std::string definitionId;
  std::vector<ParameterOverride> overrides;
  // Mirrored across the definition's YZ plane (its red axis flips). The
  // mirror lives in the geometry, never in the placement transform: OCCT
  // locations stay rigid.
  bool mirrored = false;
  // This placement alone stretched (width only, height only, ...), in the
  // placement's own coordinates, applied in order after the mirror. The
  // definition, and every other placement, stay as they are (XFORM-05).
  std::vector<StretchOp> stretches;
  // Uses the definition's shape as it is (shared display and meshes).
  bool plain() const { return overrides.empty() && !mirrored && stretches.empty(); }
};

using BaseFeature = std::variant<RectangleProfile, CircleProfile, PolygonProfile, PolylineCurve, InterpolatedCurve,
                                 ArcCurve, LinearDimension, TextNote, GuideLine, ImageUnderlay, ImportedShape,
                                 BooleanFeature, LoftFeature, SweepFeature, LoopProfile, FacePart, GroupNode,
                                 ComponentInstance, RegularPolygonProfile, SewnFeature>;

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

// Offsets the outline of a planar profile face (positive = outwards).
struct OffsetOp {
  double distance = 0.0;
};

// Revolves the profile face around an axis (local coordinates) into a solid.
struct RevolveOp {
  gp_Ax1 axis;
  double angle = 0.0;  // radians, signed
};

// A line drawn across a planar face of the solid splits that face
// (points in object coordinates, ends on the face boundary).
struct SplitOp {
  geom::FaceRef face;
  std::vector<gp_Pnt> points;
};

// Push/pull of a curved face: offset along its normal, neighbours adapt.
struct OffsetFaceOp {
  geom::SurfaceRef face;
  double distance = 0.0;
};

// A hole in a profile face: an outline drawn inside it (F-05). The outline's
// recipe is kept (placement in this object's coordinates), so the face and
// the hole regenerate together.
struct HoleOp {
  BooleanOperand outline;
};

// Uniform scale about a point (object coordinates). Exact: planes stay
// planes, so later push/pulls keep working. The factor stays editable.
struct ScaleOp {
  gp_Pnt center;
  double factor = 1.0;
};

// A face taken out (SketchUp: select a face, Delete): the solid becomes an
// open shell that can still be drawn on, painted and filled again.
struct RemoveFaceOp {
  std::optional<geom::FaceRef> plane;
  std::optional<geom::SurfaceRef> surface;
};

// Moves the corners of the shape at `points` (local coordinates, before the
// move) by direction * distance, keeping every face flat (geom::moveVertices).
// An edge move is its two corners, a face move all of its corners.
struct MoveVerticesOp {
  std::vector<gp_Pnt> points;
  gp_Dir direction;
  double distance = 0.0;
  bool autofold = false;  // faces that cannot stay flat are divided into flat pieces
};

// Every flat hole of an open shell gets a face; closed again, it is a solid.
struct CapHolesOp {};

// All faces turned over (front <-> back) - for open shells and profiles.
struct ReverseOp {};

// Offset on a face of a solid (SketchUp): the face's outline offset by
// `distance` (negative: inwards) is drawn onto the face, which is divided
// into the frame between the two outlines and the inner face; each can then
// be pushed / pulled or painted on its own. The distance stays editable.
struct InsetOp {
  geom::FaceRef face;
  double distance = 0.0;
};

using Modifier = std::variant<ExtrudeOp, PushPullOp, OffsetOp, RevolveOp, SplitOp, OffsetFaceOp, HoleOp, ScaleOp,
                              RemoveFaceOp, CapHolesOp, ReverseOp, MoveVerticesOp, InsetOp, StretchOp>;

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

// Resolves data that lives outside a single recipe (component definitions).
struct RegenContext {
  std::function<std::optional<TopoDS_Shape>(const std::string& definitionId)> definitionShape;
  // Shape of a placement with its own parameters or mirror (cached by the
  // document); a plain placement only needs definitionShape.
  std::function<Result<TopoDS_Shape>(const ComponentInstance& instance)> instanceShape;
};

// Rebuilds the shape from scratch. Every intermediate result is validated.
// Group nodes regenerate to a null shape.
Result<TopoDS_Shape> regenerate(const Recipe& recipe, const RegenContext& context = {});

std::vector<Parameter> parameters(const Recipe& recipe);
// Changes a parameter in place. Does not regenerate.
Status setParameter(Recipe& recipe, const std::string& key, double value);

std::string describe(const Recipe& recipe);

// Text of an annotation recipe and where it goes (recipe coordinates).
struct AnnotationLabel {
  gp_Pnt position;
  std::string text;
};
std::optional<AnnotationLabel> annotationLabel(const Recipe& recipe);
bool isAnnotation(const Recipe& recipe);

}  // namespace sbr::doc
