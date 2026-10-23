#pragma once
// High level modelling API.
//
// Every user operation (GUI tools, scripts, future AI agents) goes through this
// class, so all of them share validation, rollback and undo/redo.

#include <string>
#include <vector>

#include <gp_Ax1.hxx>
#include <gp_Vec.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>

#include "sbr/doc/CommandStack.h"
#include "sbr/doc/Document.h"
#include "sbr/geom/Rhino3dm.h"
#include "sbr/geom/ShapeIO.h"

namespace sbr::doc {

struct ObjectSummary {
  ObjectId id;
  ObjectId parent;
  std::string name;
  ObjectKind kind;
  std::string recipe;
  double volume = 0.0;
};

// Result of a push/pull: which object and parameter received the distance,
// so the caller can re-edit it (e.g. typing a value right after the drag).
struct PushPullResult {
  ObjectId id;
  std::string parameterKey;
};

class Modeler {
 public:
  Modeler();
  Modeler(const Modeler&) = delete;
  Modeler& operator=(const Modeler&) = delete;

  Document& document() { return document_; }
  const Document& document() const { return document_; }
  CommandStack& history() { return history_; }

  // --- creation -----------------------------------------------------------
  Result<ObjectId> createRectangle(const geom::SketchPlane& plane, double width, double height,
                                   const std::string& name = {});
  Result<ObjectId> createCircle(const geom::SketchPlane& plane, double radius,
                                const std::string& name = {});
  // Regular polygon centred on plane.origin, first corner along plane.xDir.
  Result<ObjectId> createRegularPolygon(const geom::SketchPlane& plane, double radius, int sides,
                                        const std::string& name = {});
  // 2-point arc with a signed bulge (sagitta) in the plane with 'normal';
  // closed = segment face (can be pushed/pulled).
  Result<ObjectId> createArc(const gp_Pnt& start, const gp_Pnt& end, const gp_Dir& normal, double bulge,
                             bool closed = false, const std::string& name = {});
  // Annotations. Dimension text empty = measured length.
  Result<ObjectId> createDimension(const gp_Pnt& start, const gp_Pnt& end, const gp_Vec& offset,
                                   const std::string& text = {});
  Result<ObjectId> createText(const gp_Pnt& position, const std::optional<gp_Pnt>& target, const std::string& text);
  Status setAnnotationText(const ObjectId& id, const std::string& text);
  // Construction lines (tag "Guides"), and removing them all at once
  // (grid lines on the "Grid" tag are kept).
  Result<ObjectId> createGuide(const gp_Pnt& point, const gp_Dir& direction, const std::string& name = {},
                               const std::string& layer = "Guides");
  Status clearGuides();
  // An image to draw over (a plan, an elevation), laid on `plane` with its
  // lower-left corner at the plane's origin, `width` mm wide and as high as
  // its pixel proportions say. The file's bytes are kept in the document
  // (by content hash) and saved with it. Calibrate it with scale (two
  // points and the real distance), then lock it.
  Result<ObjectId> addImageUnderlay(const std::string& bytes, const std::string& name, int pixelWidth,
                                    int pixelHeight, const geom::SketchPlane& plane, double width);
  // Locked objects cannot be picked, moved or erased (still shown and
  // snapped to). One undo step.
  Status setLocked(const std::vector<ObjectId>& ids, bool locked);
  // Structural grid (通り芯) on the ground plane: lines X1.. along Y at the
  // cumulative x spacings from 'origin', Y1.. along X, each labelled at its
  // start 'labelOffset' outside the grid, with chained spacing dimensions.
  // Everything goes on the "Grid" tag; one undo step.
  struct GridLines {
    std::vector<ObjectId> lines;   // X1..Xn, then Y1..Ym
    std::vector<ObjectId> labels;  // same order
    std::vector<ObjectId> dimensions;
  };
  Result<GridLines> createGridLines(const gp_Pnt& origin, const std::vector<double>& xSpacings,
                                    const std::vector<double>& ySpacings, double labelOffset = 1500.0);
  // A line (world points) drawn from edge to edge across a planar face of a
  // visible solid splits that face (parametric SplitOp). Returns the solid's
  // id, or a failure if the line does not divide any face.
  // Profiles (single faces) are divided into separate face objects that keep
  // the original recipe. Only objects of the current editing context are
  // considered.
  Result<ObjectId> splitWithLine(const std::vector<gp_Pnt>& worldPoints);
  // The eraser on an edge (SketchUp): a line that divides a flat face
  // (drawn with splitWithLine) is taken away and the parts become one face
  // again - on a solid by dropping that split from its history, on a loose
  // profile by putting its parts back together. The shape itself never
  // changes; an edge that is a real corner is refused. Returns the object
  // that holds the merged face.
  Result<ObjectId> eraseEdge(const ObjectId& id, const TopoDS_Edge& worldEdge);
  // Faces taken out of a solid (or an open shell): it becomes / stays an
  // open shell (one undo step). Taking out every face is refused.
  Status removeFaces(const ObjectId& id, const std::vector<TopoDS_Face>& worldFaces);
  // Fills the flat holes of open shells; closed again they are solids.
  Status fillHoles(const std::vector<ObjectId>& ids);
  // Turns the faces of open shells / profiles over (front <-> back).
  Status reverseFaces(const std::vector<ObjectId>& ids);
  // Moves corners of a solid, face or open shape (world points, as snapped
  // on its vertices) by a world vector, keeping every face flat: an edge
  // is its two corners, a face all of its corners (SketchUp's move of an
  // edge / corner). Refused when a face would bend or fold, or the solid
  // would cross itself. Painted faces keep their materials.
  Status moveVertices(const ObjectId& id, const std::vector<gp_Pnt>& worldPoints, const gp_Vec& worldDelta);
  // Hides (or shows again) the lines of edges of a solid, face or open
  // shape: display only, the faces stay split (SketchUp's hidden edge,
  // e.g. the line that divides a wall for two materials). One undo step.
  Status setEdgesHidden(const ObjectId& id, const std::vector<TopoDS_Edge>& worldEdges, bool hidden);
  // Shows every hidden edge of the objects (and of what they contain).
  Status showHiddenEdges(const std::vector<ObjectId>& ids);
  // The object a line would split (cheap check for the "split here" hint).
  std::optional<ObjectId> splitTarget(const std::vector<gp_Pnt>& worldPoints) const;
  // If the curve closes a loop with other loose open curves (lines, arcs,
  // NURBS) in the same group, replaces them by one face bounded by them
  // (parametric: the curves' parameters stay editable). One undo step.
  // loopFound (optional) tells "no closed loop" apart from "a loop that cannot
  // become a face" (not planar, crossing itself).
  Result<ObjectId> faceFromLoop(const ObjectId& curveId, bool* loopFound = nullptr);
  // SketchUp-style: where the line crosses other loose straight lines (or
  // itself) in the same plane and group and so closes areas, those areas
  // become faces. The lines are cut at the crossings; pieces bounding a new
  // face go into it, the rest (tails) stay as lines. Returns the new faces
  // (none: nothing changed). Use after faceFromLoop found no loop.
  Result<std::vector<ObjectId>> facesFromCrossings(const ObjectId& curveId);
  // Whether a chain being drawn (world points) would close an area that way
  // (the line tool ends the chain there).
  bool closesByCrossing(const std::vector<gp_Pnt>& chain) const;
  // The eraser on a line (SketchUp): only the piece of the edge between
  // its neighbouring break points (its own corners, where other lines or
  // face outlines cross or end on it) goes. On a loose line / arc / curve
  // the rest stays as lines; on the outline of a flat face (not a solid)
  // the face goes and its other edges stay as lines (a rectangle leaves
  // three sides). `worldPoint` says which piece. One undo step; returns
  // the lines left.
  Result<std::vector<ObjectId>> eraseLinePiece(const ObjectId& id, const TopoDS_Edge& worldEdge,
                                               const gp_Pnt& worldPoint);
  // The eraser on a corner edge of a solid / open shape (SketchUp): the
  // faces meeting at that edge go with it and the shape stays open (a box
  // loses two faces). A line dividing a flat face is eraseEdge instead.
  Status eraseCornerEdge(const ObjectId& id, const TopoDS_Edge& worldEdge);
  // Faces of the object that meet at the edge (world).
  std::vector<TopoDS_Face> facesAtEdge(const ObjectId& id, const TopoDS_Edge& worldEdge) const;
  // True if p is an end point of a loose open curve (where a loop can close).
  bool isLooseCurveEnd(const gp_Pnt& p) const;
  // Ends of loose open curves in the current editing context that no other
  // curve continues from: where an outline is still open (AC-05).
  std::vector<gp_Pnt> openCurveEnds() const;
  // F-05: a profile drawn strictly inside another coplanar profile (same
  // group) cuts a hole into it; the new profile stays as the island.
  // Called by every profile-creating command, in the same undo step.
  void punchHole(const ObjectId& profileId);

  // Component editing in place (double-click an instance): the instance is
  // replaced by a group holding the definition's objects, edited like any
  // group; ending puts them back into the definition, so every instance
  // updates. Both ends are single undo steps.
  Result<ObjectId> beginComponentEdit(const ObjectId& instanceId);
  Status endComponentEdit(const ObjectId& editGroupId);
  // The component-editing group containing 'id' (or 'id' itself), if any.
  std::optional<ObjectId> componentEditGroup(const ObjectId& id) const;
  int instanceCount(const std::string& definitionId) const;

  // SketchUp's Follow Me: the profile face is swept along a path (a curve
  // object, or the outline of a face picked on a solid) and consumed. The
  // result stays parametric in the profile (in0.<key>). One undo step.
  Result<ObjectId> followMe(const ObjectId& profileId, const ObjectId& pathId);
  Result<ObjectId> followMe(const ObjectId& profileId, const TopoDS_Wire& worldPath);

  // Replaces this model with a copy of 'other' (for previews: a scratch
  // model where proposed commands are tried out). History is not copied.
  void copyFrom(const Modeler& other);

  // Persistent display settings: each change is one undo step (and part of
  // an enclosing batch, so a failed atomic batch restores it).
  void setTagVisible(const std::string& tag, bool visible);
  std::vector<std::string> tags() const;  // tags in use, sorted
  // Adds an active section plane (others are deactivated); empty = none.
  void setSection(const std::optional<SectionPlane>& plane);
  // Shows the most recent section again (true) or none (false).
  void setSectionVisible(bool visible);
  bool sectionVisible() const;
  void clearSections();
  Result<ObjectId> createPolygon(const std::vector<gp_Pnt>& points, const std::string& name = {});
  Result<ObjectId> createPolyline(const std::vector<gp_Pnt>& points, const std::string& name = {});
  Result<ObjectId> importShape(const TopoDS_Shape& shape, const std::string& name,
                               const std::string& source);

  // --- editing ------------------------------------------------------------
  // `face` is a face of the object's world shape (or an equivalent copy).
  Result<PushPullResult> pushPull(const ObjectId& id, const TopoDS_Face& face, double distance);
  // Extrudes a face/profile object directly (convenience for scripting).
  Result<PushPullResult> extrude(const ObjectId& id, double distance);
  // Creates a new face object whose outline is offset from a profile face
  // (positive = outwards). The original is kept, as in SketchUp.
  Result<PushPullResult> offset(const ObjectId& id, double distance);
  // Offsets the outline of a planar face of a solid into a new profile lying
  // on that face (e.g. an inset to push in for a frame or a recess).
  Result<ObjectId> offsetSolidFace(const ObjectId& id, const TopoDS_Face& worldFace, double distance);
  Status setParameter(const ObjectId& id, const std::string& key, double value);
  Status rename(const ObjectId& id, const std::string& name);
  Status setVisible(const ObjectId& id, bool visible);
  // Hides the objects (one undo step) / shows every hidden object again.
  Status hide(const std::vector<ObjectId>& ids);
  Status unhideAll();
  Status setLayer(const ObjectId& id, const std::string& layer);
  // Meaning attributes on several objects in one undo step; an empty value
  // removes the key.
  Status setAttribute(const std::vector<ObjectId>& ids, const std::string& key, const std::string& value);
  // Plain colours go through materials: the material made from that colour
  // (colorMaterial) is created when needed and assigned to the objects.
  Status setColor(const ObjectId& id, const Color& color);
  Status paint(const std::vector<ObjectId>& ids, const Color& color);

  // --- materials (docs/11_materials.md) -----------------------------------
  // A new named material with a fresh stable id (one undo step).
  Result<std::string> createMaterial(const std::string& name, const Color& color);
  Status updateMaterial(const std::string& id, std::optional<std::string> name, std::optional<Color> color);
  // Replaces a material's settings (name, colour, opacity, texture,
  // rendering values); objects using it change look, the id stays. With
  // `continuing`, an edit right after an edit of the same material merges
  // into its undo step (a colour slider being dragged).
  Status editMaterial(const Material& changed, bool continuing = false);
  // A copy with a new id, to edit separately ("" name: "<name> 2", ...).
  Result<std::string> duplicateMaterial(const std::string& id, const std::string& name = {});
  // Stores a picture for a texture (by content); returns its hash.
  std::string addTextureImage(const std::string& bytes, const std::string& name);
  // Objects and faces using it fall back to inheriting (same undo step).
  Status deleteMaterial(const std::string& id);
  // Whole objects take `material` ("" = inherit from the container again).
  // A group or component placement colours what inside has no material of
  // its own. Unknown ids are refused. One undo step.
  Status assignMaterial(const std::vector<ObjectId>& ids, const std::string& material);
  // Faces of one object (world faces of its shape, e.g. picked) take
  // `material`; "" removes their own material (back to the object's).
  // One undo step.
  Status paintFaces(const ObjectId& id, const std::vector<TopoDS_Face>& worldFaces, const std::string& material);
  // Faces' own materials removed on these objects (all faces).
  Status clearFaceMaterials(const std::vector<ObjectId>& ids);
  // What a face of an object shows and why ("face", "object", "container"
  // or "none" with the legacy colour).
  struct MaterialAt {
    std::string material;
    std::string source;
    Color color;
  };
  Result<MaterialAt> materialAt(const ObjectId& id, const TopoDS_Face& worldFace) const;
  // Deletes objects together with their descendants.
  Status remove(const std::vector<ObjectId>& ids);

  // --- placement ------------------------------------------------------------
  // Applies a world-space transformation to the objects (and their children).
  Status transform(const std::vector<ObjectId>& ids, const gp_Trsf& worldDelta,
                   const std::string& commandName = "Move");
  Status move(const std::vector<ObjectId>& ids, const gp_Vec& offset);
  Status rotate(const std::vector<ObjectId>& ids, const gp_Ax1& axis, double angleRadians);
  // Uniform scale about a world point (one undo step). Solids, faces and
  // curves get a parametric ScaleOp (merged with a directly preceding one);
  // guides and annotations have their points scaled. Groups scale all their
  // contents; components are refused (edit the definition instead).
  Status scale(const std::vector<ObjectId>& ids, const gp_Pnt& center, double factor);
  // --- architectural intents (shared by GUI and AI) -----------------------
  // Openings: rectangular profiles subtracted from a host (a wall with a
  // door / window pushed through), numbered 1, 2, ... in the order they
  // were cut (also through nested Subtracts).
  struct OpeningInfo {
    std::size_t index = 0;
    std::string name;
    double width = 0.0, height = 0.0;  // absolute
    gp_Pnt center;                     // world
    double bottom = 0.0;               // world Z of the sill / lowest edge
    gp_Dir normal;                     // of the face it was cut from (world)
    double along = 0.0;                // horizontal extent along the wall (Z x normal)
  };
  Result<std::vector<OpeningInfo>> openings(const ObjectId& host) const;
  // Resizes openings (all when `indices` is empty) keeping the horizontal
  // centre and the sill (keepCenter = false) or the centre (true) in place.
  // One undo step; the parameters stay editable.
  Status setOpeningSize(const ObjectId& host, const std::vector<std::size_t>& indices,
                        std::optional<double> width, std::optional<double> height, bool keepCenter = false);
  // Levels (floors): each object (topmost of `ids`, all top-level objects
  // when empty) gets attribute `key` = the name of the highest level whose
  // elevation is at or below its lowest point. Objects below every level
  // are left as they are and returned. One undo step.
  struct Level {
    std::string name;
    double elevation = 0.0;
  };
  Result<std::vector<ObjectId>> assignLevels(const std::vector<ObjectId>& ids, std::vector<Level> levels,
                                             const std::string& key = "level");
  // Moves each object along a world axis (0 x, 1 y, 2 z) so that its
  // bounds' min / max / centre (edge -1 / 1 / 0) lie at `value`, or at
  // those of the first object when no value is given. One undo step.
  Status align(const std::vector<ObjectId>& ids, int axis, int edge, std::optional<double> value = std::nullopt);
  // Walls along a plan polyline (points at the base height; closed when the
  // last equals the first): one parametric wall per segment (rectangle of
  // length x thickness extruded by height), corners butt-joined (exact for
  // right angles). justification: -1 = wall left of the line (looking
  // along it), 0 = centred, 1 = right. Attribute kind = wall. One undo step.
  Result<std::vector<ObjectId>> createWalls(std::vector<gp_Pnt> points, double thickness, double height,
                                            int justification = 0);
  // A floor slab: plan polygon (any winding) with its top at `top`,
  // `thickness` below. Attribute kind = slab. One undo step.
  Result<ObjectId> createSlab(const std::vector<gp_Pnt>& outline, double top, double thickness);
  // Cuts `count` equal rectangular openings through a wall's face (the
  // outermost planar face facing `faceNormal`), evenly spread along it, or
  // `spacing` apart (centre to centre) around the face's middle. sill =
  // height of the lower edge above the face's bottom. Goes through the
  // same push/pull as drawing them by hand; returns the host's new id.
  Result<ObjectId> placeOpenings(const ObjectId& host, const gp_Dir& faceNormal, int count, double width,
                                 double height, double sill, std::optional<double> spacing = std::nullopt);
  // One such opening centred (along the wall) on the world point `at`
  // (only its position along the face counts), e.g. where the user clicked.
  Result<ObjectId> placeOpeningAt(const ObjectId& host, const gp_Dir& faceNormal, const gp_Pnt& at, double width,
                                  double height, double sill);
  // Copies the objects `count` times, copy k placed at worldDelta^k (linear
  // or polar arrays). Returns the ids of the new top-level copies.
  Result<std::vector<ObjectId>> copy(const std::vector<ObjectId>& ids, const gp_Trsf& worldDelta,
                                     int count = 1);
  // Clipboard: a snapshot of the objects (with their subtrees) that stays
  // valid while the model changes. Roots carry their world placement.
  struct Clipboard {
    std::vector<Object> objects;  // parents first; roots have no parent
    TopoDS_Shape preview;         // world geometry at copy time
    gp_Pnt anchor;                // lowest corner of the bounding box
    bool empty() const { return objects.empty(); }
  };
  Result<Clipboard> copyToClipboard(const std::vector<ObjectId>& ids) const;
  // Adds a fresh copy (new ids) into the current editing context, moved by
  // worldDelta from where it was copied. Returns the new top-level ids.
  Result<std::vector<ObjectId>> paste(const Clipboard& clipboard, const gp_Trsf& worldDelta = {});
  // Sets the local translation of an object (position field in the UI).
  Status setPosition(const ObjectId& id, const gp_Vec& translation);

  // --- structure ------------------------------------------------------------
  Result<ObjectId> group(const std::vector<ObjectId>& ids, const std::string& name = {});
  Status ungroup(const ObjectId& groupId);
  // Turns the objects into a component definition and replaces them by one
  // instance. Returns the instance id.
  Result<ObjectId> makeComponent(const std::vector<ObjectId>& ids, const std::string& name = {});
  // Replaces an instance by independent copies of the definition objects.
  Result<std::vector<ObjectId>> explode(const ObjectId& instanceId);
  std::vector<Parameter> definitionParameters(const std::string& definitionId,
                                              const ObjectId& objectId) const;
  // Edits a parameter of an object inside a definition: all instances update.
  Status setDefinitionParameter(const std::string& definitionId, const ObjectId& objectId,
                                const std::string& key, double value);
  Status renameDefinition(const std::string& definitionId, const std::string& name);

  // --- one placement of a component ----------------------------------------
  // Parameters of the definition's objects as this placement shows them.
  struct PlacementParameter {
    ObjectId object;  // inside the definition
    std::string objectName;
    Parameter parameter;  // this placement's value
    double shared = 0.0;  // the definition's value
    bool own = false;     // set for this placement only
  };
  std::vector<PlacementParameter> placementParameters(const ObjectId& instanceId) const;
  // Changes a parameter for this placement only (the others keep the
  // definition's value). Setting the definition's value again removes it.
  Status setPlacementParameter(const ObjectId& instanceId, const ObjectId& objectId, const std::string& key,
                               double value);
  Status clearPlacementParameters(const ObjectId& instanceId);
  // Gives this placement a definition of its own: a copy with the
  // placement's parameters written in. Returns the new definition id.
  Result<std::string> makeUnique(const ObjectId& instanceId);
  // Flips the placement across its own axis (0 red, 1 green, 2 blue), in
  // place: its bounds stay where they are.
  Status mirrorPlacement(const ObjectId& instanceId, int axis);
  // Moves the definition's origin to a world point and, with xDirection,
  // turns its red axis toward it (about its blue axis). Every placement
  // stays where it is.
  Status setComponentOrigin(const ObjectId& instanceId, const gp_Pnt& worldOrigin,
                            const std::optional<gp_Dir>& xDirection = std::nullopt);

  // Parametric Boolean of solids. For Subtract, ids[1..] are removed from
  // ids[0]. The operands are consumed and kept inside the result's recipe.
  Result<ObjectId> boolean(BooleanKind kind, const std::vector<ObjectId>& ids);

  // --- freeform (Phase 3) ----------------------------------------------------
  // Curves store their through-points; revolve, loft and sweep are parametric.
  Result<ObjectId> createCurve(const std::vector<gp_Pnt>& points, bool closed,
                               const std::string& name = {});
  // Lofts through curve/face objects (their outer wires), in the given order.
  Result<ObjectId> loftObjects(const std::vector<ObjectId>& sections, bool solid);
  // Face profiles are revolved parametrically in place (returns the same id);
  // other shapes produce a new non-parametric object.
  Result<ObjectId> revolveObject(const ObjectId& profile, const gp_Ax1& axis, double angleRadians);
  Result<ObjectId> sweepObject(const ObjectId& profile, const ObjectId& spine);
  // Replaces the through-points of a NURBS curve object (local coordinates).
  Status setCurvePoints(const ObjectId& id, const std::vector<gp_Pnt>& points);

  // Group being edited ("group edit mode"): objects created from now on are
  // placed inside it. Empty = top level.
  Status setActiveGroup(const ObjectId& groupId);
  const ObjectId& activeGroup() const { return activeGroup_; }

  bool undo() { return history_.undo(); }
  bool redo() { return history_.redo(); }

  // --- files --------------------------------------------------------------
  void newDocument();
  Status save(const std::string& path);
  // verifyRecipes = false opens with the stored geometry only (fast for
  // large models); verify later with verifyRecipes(cursor, budget).
  Status load(const std::string& path, std::vector<std::string>* warnings = nullptr, bool verifyRecipes = true);
  // Checks up to `budget` objects from `cursor` on (advanced); returns the
  // warnings found. Done when cursor reaches the object count.
  std::vector<std::string> verifyRecipes(std::size_t& cursor, std::size_t budget) const;
  // Exports the given objects (all visible objects when empty) as STEP,
  // keeping object names and colours.
  Status exportStep(const std::string& path, const std::vector<ObjectId>& ids = {}) const;
  Result<std::vector<ObjectId>> importStep(const std::string& path);
  // Rhino .3dm: exact NURBS Breps with names, colours and tags as layers.
  Status export3dm(const std::string& path, const std::vector<ObjectId>& ids = {}) const;
  // Refuses a file that does not come through whole (unsupported objects,
  // failed or partial conversions; the error says what) unless
  // `allowPartial`; `report` (optional) says what was found either way.
  Result<std::vector<ObjectId>> import3dm(const std::string& path, bool allowPartial = false,
                                          geom::Import3dmReport* report = nullptr);

  // --- queries ------------------------------------------------------------
  std::vector<ObjectSummary> listObjects() const;
  std::vector<Parameter> parameters(const ObjectId& id) const;
  // Removes ids whose ancestor is also in the list (operating on a group
  // already covers its children).
  std::vector<ObjectId> topmost(const std::vector<ObjectId>& ids) const;

 private:
  Result<ObjectId> cutOpenings(const ObjectId& host, const gp_Dir& faceNormal, int count, double width,
                               double height, double sill, std::optional<double> spacing,
                               std::optional<gp_Pnt> at);
  Result<ObjectId> addObject(Object object, const std::string& commandName);
  Status commitModified(Object object, const std::string& commandName);
  Result<ObjectId> addProfile(Object object, const std::string& commandName);
  Result<ObjectId> followMeAlong(const ObjectId& profileId, const BooleanOperand& path);
  // True if a profile lying on a solid's face points into that solid.
  bool facesIntoSolid(const TopoDS_Face& worldFace, const ObjectId& parent) const;
  std::vector<geom::NamedShape> exportParts(const std::vector<ObjectId>& ids) const;
  Result<std::vector<ObjectId>> addImported(const std::vector<geom::NamedShape>& parts, const std::string& path,
                                            const std::string& commandName);
  std::string uniqueName(const std::string& base) const;
  // Deep copy of a subtree with fresh ids. Returns objects parents-first.
  std::vector<Object> cloneSubtree(const ObjectId& id, const ObjectId& newParent) const;
  // Push/pull of a profile lying on a face of another solid: returns the
  // committed cut/join, or nullopt to fall back to a plain extrusion.
  std::optional<Result<PushPullResult>> mergeIntoHost(const Object& profileBefore, Object extruded,
                                                      const std::string& key);
  Status checkSameParent(const std::vector<ObjectId>& ids, ObjectId& parent) const;

  Document document_;
  CommandStack history_;
  ObjectId activeGroup_;
};

}  // namespace sbr::doc
