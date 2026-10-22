#pragma once
// High level modelling API.
//
// Every user operation (GUI tools, scripts, future AI agents) goes through this
// class, so all of them share validation, rollback and undo/redo.

#include <string>
#include <vector>

#include <gp_Ax1.hxx>
#include <gp_Vec.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>

#include "sbr/doc/CommandStack.h"
#include "sbr/doc/Document.h"
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
  // The object a line would split (cheap check for the "split here" hint).
  std::optional<ObjectId> splitTarget(const std::vector<gp_Pnt>& worldPoints) const;
  // If the curve closes a loop with other loose open curves (lines, arcs,
  // NURBS) in the same group, replaces them by one face bounded by them
  // (parametric: the curves' parameters stay editable). One undo step.
  // loopFound (optional) tells "no closed loop" apart from "a loop that cannot
  // become a face" (not planar, crossing itself).
  Result<ObjectId> faceFromLoop(const ObjectId& curveId, bool* loopFound = nullptr);
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
  Status setColor(const ObjectId& id, const Color& color);
  // Paints objects; groups paint all their descendants (one undo step).
  Status paint(const std::vector<ObjectId>& ids, const Color& color);
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
  Status load(const std::string& path, std::vector<std::string>* warnings = nullptr);
  // Exports the given objects (all visible objects when empty) as STEP,
  // keeping object names and colours.
  Status exportStep(const std::string& path, const std::vector<ObjectId>& ids = {}) const;
  Result<std::vector<ObjectId>> importStep(const std::string& path);
  // Rhino .3dm: exact NURBS Breps with names, colours and tags as layers.
  Status export3dm(const std::string& path, const std::vector<ObjectId>& ids = {}) const;
  Result<std::vector<ObjectId>> import3dm(const std::string& path);

  // --- queries ------------------------------------------------------------
  std::vector<ObjectSummary> listObjects() const;
  std::vector<Parameter> parameters(const ObjectId& id) const;
  // Removes ids whose ancestor is also in the list (operating on a group
  // already covers its children).
  std::vector<ObjectId> topmost(const std::vector<ObjectId>& ids) const;

 private:
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
