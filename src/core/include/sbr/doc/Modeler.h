#pragma once
// High level modelling API.
//
// Every user operation (GUI tools, scripts, future AI agents) goes through this
// class, so all of them share validation, rollback and undo/redo.

#include <set>
#include <string>
#include <vector>

#include <gp_Ax1.hxx>
#include <gp_Vec.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>

#include "sbr/doc/CommandStack.h"
#include "sbr/doc/Document.h"
#include "sbr/geom/Roof.h"
#include "sbr/geom/Stair.h"
#include "sbr/geom/Rhino3dm.h"
#include "sbr/geom/Collada.h"
#include "sbr/geom/Skp.h"
#include "sbr/geom/Dxf.h"
#include "sbr/geom/Gltf.h"
#include "sbr/geom/Jww.h"
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

// What Delete takes away from one object: the whole object, or the faces /
// edges picked on it (world shapes, as shown when they were picked).
struct DeleteTarget {
  ObjectId id;
  bool whole = false;
  std::vector<TopoDS_Face> faces;
  std::vector<TopoDS_Edge> edges;
};

struct DeleteOutcome {
  std::vector<ObjectId> removed;  // objects that are gone
  std::vector<ObjectId> lines;    // lines left by deleted faces / pieces of lines
  int merged = 0;                 // dividing lines taken away (faces joined)
  int openings = 0;               // openings removed (their walls made whole there)
};

// A room (docs/90 G-07, docs/98): the region its marker stands in, read from the walls as they
// are now. Nothing of it is stored but the marker (a point and a name): areas are never stale.
struct RoomInfo {
  ObjectId id;        // the marker; empty for a region asked about without one
  std::string name;
  std::string level;  // id of the level the marker's height is at, if any
  gp_Pnt at;          // the marker (world)
  bool closed = false;
  std::string problem;  // why not closed, or what else is wrong (e.g. shares its region); empty when fine
  double centreArea = 0.0;  // mm2 within the wall lines (壁芯)
  bool innerKnown = false;  // false: the wall faces give no simple outline here (not evaluated)
  double innerArea = 0.0;   // mm2 within the wall faces (内法)
  double innerPerimeter = 0.0;
  std::vector<gp_Pnt> centre, inner;  // outlines at the marker's height (outer ones)
  std::vector<ObjectId> walls;        // the walls around it
  std::vector<gp_Pnt> looseEnds;      // not closed: the wall ends nearest the marker that meet nothing
  int unreadableWalls = 0;            // walls of this storey no longer the shape their tool made (not counted as walls)
};

// What a room is used for and its finishes (docs/97 N-02, docs/115): words as people write them
// on a finish schedule; the ceiling height from the floor (0: not given).
struct RoomFinish {
  std::string use, floor, wall, ceiling;
  double ceilingHeight = 0.0;
  // docs/162: the part of the building the room is, for the floor area the floor area ratio is
  // worked out from (a code of floorRatioParts(); empty: counted like any room).
  std::string ratioPart;
  bool empty() const {
    return use.empty() && floor.empty() && wall.empty() && ceiling.empty() && ceilingHeight <= 0.0 && ratioPart.empty();
  }
};
// The parts of a building whose floor area is left out of the total the floor area ratio (容積率)
// is worked out from (docs/162). `share`: at most this share of the total of every storey's floor
// area is left out (0: all of it). As read on e-Gov on 2026-11-17: Building Standards Act
// Enforcement Order art. 2 (1) iv proviso and (3) (in force 2026-11-01), the Act art. 52 (6) i, ii
// (in force 2026-05-27) and the Order art. 135-16 (the lift of art. 52 (6) i is an elevator).
// docs/166: basement_dwelling (Act art. 52 (3)) counts only in a basement whose ceiling is 1 m or
// less above the ground (the level named GL), up to 1/3 (of the whole: the building taken as a dwelling).
struct FloorRatioPart {
  const char* code;
  const char* label;  // Japanese, as the Order names it
  double share;
  const char* basis;
};
const std::vector<FloorRatioPart>& floorRatioParts();
// Read from a room marker (attribute room_finish); empty when none is given.
RoomFinish roomFinishOf(const Object& marker);

// A roof made by createRoof (docs/90 G-08, docs/99) as it stands in the world.
struct RoofInfo {
  geom::RoofSpec spec;  // outline, height and fall direction in world coordinates
  double planArea = 0.0, slopeArea = 0.0;  // mm2
  double ridgeHeight = 0.0, eaveHeight = 0.0;  // world z of its highest point / lowest underside edge
  int parts = 0;
  std::string level;  // id of the level its bearing height follows, if any
};

// A stair made by createStair (docs/90 G-09, docs/103) as it stands in the world.
struct StairInfo {
  geom::StairSpec spec;  // its line and base in world coordinates
  int risers = 0;        // as built (spec.risers may be 0 = automatic)
  double riser = 0.0;
  int treads = 0;
  double goingMin = 0.0, goingMax = 0.0, winderGoing = 0.0, walkLength = 0.0;
  int flights = 0, landings = 0, winders = 0;
  std::vector<geom::StairCheck> checks;
  std::string baseLevel, topLevel;      // ids of the levels its foot and top follow, if any
  std::vector<ObjectId> openedSlabs;    // floors opened for it
};

// A stair made by createStair, as it stands in the world (its line, base), read from the
// document alone; none for an object that is not such a stair any more.
std::optional<geom::StairSpec> stairInWorld(const Document& document, const Object& object);

// What a move of walls did to the walls joined to them (docs/90 G-05).
struct JoinFollow {
  int followed = 0;               // walls made longer / shorter to stay joined
  int carried = 0;                // walls at a corner carried along as they are (a slide, a change of length)
  int slabs = 0;                  // slabs whose outline went with a wall
  int members = 0;                // columns / beams standing in or lying along a wall that went with it (docs/169)
  int roofs = 0;                  // roofs whose outline went with a wall (docs/169)
  std::vector<std::string> left;  // names of joined walls that could not follow (they stay as they were)
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
  // Structural grid (通り芯, docs/43): a group "Grid" placed at 'origin'
  // holding the grid's own description (GridSpec): lines X1.. along Y at
  // the cumulative x spacings, Y1.. along X, each labelled 'labelOffset'
  // outside the grid, with chained spacing dimensions, all on the "Grid"
  // tag. Edited afterwards with setGrid, or by moving or erasing a line
  // inside the group. One undo step.
  struct GridLines {
    ObjectId grid;                 // the group
    std::vector<ObjectId> lines;   // X1..Xn, then Y1..Ym
    std::vector<ObjectId> labels;  // same order
    std::vector<ObjectId> dimensions;
  };
  Result<GridLines> createGridLines(const gp_Pnt& origin, const std::vector<double>& xSpacings,
                                    const std::vector<double>& ySpacings, double labelOffset = 1500.0);
  // Levels (GL, 1FL..., docs/45), kept by elevation in the document's
  // settings (saved, undone). setLevels replaces them all: a level keeps
  // its id (new ones get one), members whose ends stand on a level move up
  // or down with it, in the same undo step. Refused: an empty or repeated
  // name, a repeated id, a height that is not a number.
  const std::vector<FloorLevel>& levels() const { return document_.settings().levels; }
  Status setLevels(std::vector<FloorLevel> levels);
  Status setConstructionOverrides(const std::string& system, const std::string& patch);
  Status setWallCalculation(const std::string& inputs);
  // Date, time, place and north for the sun (docs/82); one undo step.
  Status setSun(const geom::SunSettings& sun);
  Status setSiteNorth(const SiteNorth& north);
  Status setSiteNorthFromPoints(const gp_Pnt& tail, const gp_Pnt& tip, const std::string& source,
                                const std::string& checkedOn);
  Status setSiteBoundary(const std::optional<SiteBoundary>& boundary);
  Status setSiteRoad(const std::array<gp_Pnt, 2>& edge, SiteRoad road);
  // docs/205: the group of use zones the site is in, for the daylight factor (令 20 条 2 項 1-3 号):
  // residential / industrial / commercial ("" takes it away), with the document it was read from,
  // as attributes "use_zone" / "use_zone_source" on the site outline. One undo step.
  Status setSiteZone(const std::string& zone, const std::string& source);
  // docs/212: a person's mark that a permit workflow stage (1-12) was looked at, with the
  // fingerprint of its rows now (doc::permitConfirmation tells when the model has changed them
  // since); who, a note, and the date (YYYY-MM-DD, empty: today). confirmed false takes the mark
  // away. One undo step. A record, never a pass.
  Status setPermitConfirmation(int stage, bool confirmed, const std::string& by = {}, const std::string& note = {},
                               const std::string& on = {});
  // docs/210: energy inputs. The route the energy documents follow (spec: 仕様基準 / performance:
  // 性能基準 / other / "" takes it away), the region (1-8; 0: not given) and the source, as
  // attributes on the site outline ("energy_route", "energy_region", "energy_source").
  Status setEnergyRoute(const std::string& route, int region, const std::string& source);
  // The insulation of walls, slabs and roofs (`ids`): material ("" takes it away), thickness (mm),
  // thermal conductivity λ (W/(m·K), 0: not given) or thermal resistance R (m²·K/W, 0: not given),
  // placement (fill: 充填 / outside: 外張り / inside / ""), source; as attribute "insulation".
  // One undo step for all.
  Status setInsulation(const std::vector<ObjectId>& ids, const std::string& material, double thicknessMm,
                       double conductivity, double resistance, const std::string& placement, const std::string& source);
  // An opening's performance: heat transmittance U (W/(m²·K), below 0 takes the whole away), glass
  // and source, by opening id as attribute "opening_performance" on the host. `indices` 1-based,
  // all when empty. One undo step.
  Status setOpeningPerformance(const ObjectId& host, const std::vector<std::size_t>& indices, double uValue,
                               const std::string& glass, const std::string& source);
  void clearSiteRoad();
  Status setSiteGround(SiteGround ground, ObjectId building = {}, bool confirmedPlacement = false);
  void clearSiteGround();
  // The description of a grid group (null: not a grid).
  const GridSpec* gridSpec(const ObjectId& grid) const;
  // Gives the grid new lines (names, offsets, added or taken away; ids
  // kept for lines that stay): its lines, labels and dimensions are made
  // again and members pinned to its crossings follow. One undo step.
  // Refused: no lines in a direction, an empty or repeated name, a
  // repeated id, two lines at the same place.
  Status setGrid(const ObjectId& grid, const GridSpec& spec);
  // A line (world points) drawn from edge to edge across a planar face of a
  // visible solid splits that face (parametric SplitOp). Returns the solid's
  // id, or a failure if the line does not divide any face.
  // Profiles (single faces) are divided into separate face objects that keep
  // the original recipe. Only objects of the current editing context are
  // considered.
  Result<ObjectId> splitWithLine(const std::vector<gp_Pnt>& worldPoints);
  // Imprint an exact single-edge curve on planar faces in its editing group.
  // True when divided; consume the used part and retain exact outside pieces.
  // Part of the caller's undo group (as settleOverlaps).
  Result<bool> splitWithCurve(const ObjectId& curveId);
  // A straight line (polyline, world) lying on flat faces of the current
  // group: every part of it that runs across a face from edge to edge
  // divides that face, so one line can divide several faces side by side
  // and may start and end outside them. Parts that stop inside a face are
  // not extended, lines only touching a face (crossing its plane) or a
  // little off it do not divide it. `rest`: the parts not used (to keep as
  // lines). Part of the caller's undo step.
  struct Divided {
    int faces = 0;
    std::vector<std::vector<gp_Pnt>> rest;
  };
  Result<Divided> splitAcross(const std::vector<gp_Pnt>& worldPoints);
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
  // Delete (SketchUp): the faces go, their edges stay as lines unless they
  // are in `droppedEdges` or still bound a face that is left. A loose face
  // leaves its outline (and holes); a solid / shell keeps its other faces
  // (open shell) and the edges only the deleted faces used become lines;
  // all faces of an object leave all its edges as lines. One undo step;
  // returns the lines made. Faces bounded by `droppedEdges` go as well
  // (an edge taken away cannot leave its faces).
  Result<std::vector<ObjectId>> deleteFaces(const ObjectId& id, const std::vector<TopoDS_Face>& worldFaces,
                                            const std::vector<TopoDS_Edge>& droppedEdges = {});
  // Delete of a selection (GUI Delete key, API delete): everything is
  // located before anything changes, then per object:
  //  - whole: the object goes (with its children);
  //  - a line's edges: just those edges go, the rest stays as lines;
  //  - a line dividing a flat face (same plane both sides): the faces join;
  //  - other edges of faces / solids: the faces they bound go (deleteFaces),
  //    edges no face uses any more stay as lines;
  //  - faces: they go, their edges stay as lines.
  // Elements are found again by their geometry at their turn, so an earlier
  // step changing ids or indices cannot make a later one hit the wrong
  // thing. One undo step; on any failure nothing changes and the reason is
  // returned.
  Result<DeleteOutcome> deleteSelection(const std::vector<DeleteTarget>& targets);
  // Fills the flat holes of open shells; closed again they are solids.
  Status fillHoles(const std::vector<ObjectId>& ids);
  // Turns the faces of open shells / profiles over (front <-> back).
  Status reverseFaces(const std::vector<ObjectId>& ids);
  // Moves corners of a solid, face or open shape (world points, as snapped
  // on its vertices) by a world vector, keeping every face flat: an edge
  // is its two corners, a face all of its corners (SketchUp's move of an
  // edge / corner). Faces, shells and solids beside it in the same group
  // that share an edge with it (e.g. the other half of a split face) move
  // their corners at those points too, in the same undo step. A face that
  // cannot stay flat is folded (Autofold: divided into flat pieces along
  // new edges) when `autofold`, else the move is refused; refused anyway
  // when a face would fold over or collapse, a face with holes or a
  // concave one would need folding, or the solid would cross itself.
  // Painted faces keep their materials (both pieces of a folded one).
  Status moveVertices(const ObjectId& id, const std::vector<gp_Pnt>& worldPoints, const gp_Vec& worldDelta,
                      bool autofold = true);
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
  // One closed planar boundary from selected curve objects / their edges.
  // Source curves remain intact; the face retains their recipe snapshots.
  Result<ObjectId> faceFromSelection(const std::vector<DeleteTarget>& targets);
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
  // Along edges picked one by one, possibly of several objects (lines,
  // arcs, edges of faces or solids): joined into one path whatever their
  // order (geom::pathFromEdges; the profile's own edges are left out).
  Result<ObjectId> followMe(const ObjectId& profileId, const std::vector<TopoDS_Edge>& worldEdges);

  // Replaces this model with a copy of 'other' (for previews: a scratch
  // model where proposed commands are tried out). History is not copied.
  void copyFrom(const Modeler& other);

  // Persistent display settings: each change is one undo step (and part of
  // an enclosing batch, so a failed atomic batch restores it).
  void setTagVisible(const std::string& tag, bool visible);
  std::vector<std::string> tags() const;  // tags in use, sorted
  // Adds an active section plane (others are deactivated); empty = none.
  void setSection(const std::optional<SectionPlane>& plane);
  // Toggles clipping while retaining the chosen active plane.
  void setSectionVisible(bool visible);
  void updateSection(std::size_t index, const SectionPlane& plane);
  void activateSection(std::size_t index);
  void removeSection(std::size_t index);
  void setSectionPlanesVisible(bool visible);
  void setSectionFillVisible(bool visible);
  bool sectionVisible() const;
  void clearSections();
  Result<ObjectId> createPolygon(const std::vector<gp_Pnt>& points, const std::string& name = {});
  Result<ObjectId> createPolyline(const std::vector<gp_Pnt>& points, const std::string& name = {});
  Result<ObjectId> importShape(const TopoDS_Shape& shape, const std::string& name,
                               const std::string& source);

  // --- editing ------------------------------------------------------------
  // `face` is a face of the object's world shape (or an equivalent copy).
  // `keepStartFace` (Ctrl, SketchUp's "create new starting face"): on an
  // open shape the face stays where it was and gives a volume of its own
  // (e.g. a wall given thickness) instead of moving out as on a surface.
  Result<PushPullResult> pushPull(const ObjectId& id, const TopoDS_Face& face, double distance,
                                  bool keepStartFace = false);
  // Extrudes a face/profile object directly (convenience for scripting).
  Result<PushPullResult> extrude(const ObjectId& id, double distance);
  // Creates a new face object whose outline is offset from a profile face
  // (positive = outwards). The original is kept, as in SketchUp, and the two
  // outlines divide the plane: inwards the original becomes the frame (a
  // hole where the new face is), outwards the new face is the frame. One
  // undo step.
  Result<PushPullResult> offset(const ObjectId& id, double distance);
  // Offset on a planar face of a solid. Inwards (distance < 0) the outline
  // is drawn onto the face, dividing it into a frame and an inner face of
  // the solid (parametric; returns the solid). Outwards it leaves the face:
  // a new profile on its plane (returns that).
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
  // Walls joined to a wall that is moved sideways (corners, T joints) are made longer or
  // shorter so the joint stays (docs/90 G-05), in the same undo step. A joined wall that
  // cannot follow (locked, an opening would be left outside it, nothing left of it) stays as
  // it was: the move itself is never refused for it. Rotations leave joints alone.
  Status transform(const std::vector<ObjectId>& ids, const gp_Trsf& worldDelta,
                   const std::string& commandName = "Move");
  // --- roofs (docs/90 G-08) -------------------------------------------------
  // A roof as one solid over a right-angled plan outline (geom::makeRoof): attribute kind =
  // roof, its sizes kept (attribute roof_spec) so they can be changed later. Made at a level's
  // height its bearing height follows that level. One undo step.
  Result<ObjectId> createRoof(const geom::RoofSpec& spec);
  // Refused for an object that is not such a roof any more (its shape edited, or tilted).
  Result<RoofInfo> roofInfo(const ObjectId& id) const;
  // The same roof with other sizes (the same object). One undo step.
  Status changeRoof(const ObjectId& id, const geom::RoofSpec& spec);

  // Walls made by the wall tool stand up to a roof's underside (docs/97 N-05): they are made
  // tall enough and lose what is above the underside. They stay walls (length, build-up,
  // openings as before; attribute top_roof = the roof). When the roof or the wall changes or
  // moves they are fitted again. Walls that cannot be fitted (reshaped by hand, wholly above
  // the roof) are refused by name and nothing changes. One undo step.
  Status fitWallsToRoof(const std::vector<ObjectId>& walls, const ObjectId& roof);
  // Mitred corners (docs/159): the walls (all when empty) are marked; where exactly two marked
  // walls meet at a corner, both end on the line through the inner and the outer corner point
  // instead of one butting against the other (no seam on the faces, no gap or overlap at corners
  // that are not right angles). The walls stay walls as drawn (their lengths, joins, openings,
  // rooms as before) and are mitred again when they or their neighbours change. `on` false: square
  // again. One undo step. Returns the corners mitred now among all marked walls.
  Result<int> mitreWallCorners(const std::vector<ObjectId>& walls, bool on = true);
  // Flat tops again, at the height the walls had before.
  Status releaseWallsFromRoof(const std::vector<ObjectId>& walls);
  // Walls made by the wall tool whose line lies within the roof's outline (or on it) and that
  // start below its bearing height: what "fit the walls under this roof" means.
  std::vector<ObjectId> wallsUnderRoof(const ObjectId& roof) const;
  // docs/161: the sides of this roof outline (world coordinates; side i from corner i to i + 1)
  // that lie along a wall standing well above the roof's bearing height: where a lower roof
  // ends against the upper storey. What RoofSpec::wallSides is meant to hold.
  std::vector<int> roofSidesAtWalls(const geom::RoofSpec& roof) const;
  // The walls fitted to this roof now.
  std::vector<ObjectId> wallsFittedToRoof(const ObjectId& roof) const;

  // --- stairs (docs/90 G-09, docs/103) --------------------------------------
  // A stair along a walking line in plan (geom::makeStair): attribute kind = stair, its sizes
  // kept (attribute stair_spec) so they can be changed later. Its foot and top at levels'
  // heights follow those levels: the risers are worked out again when the storey height changes.
  Result<ObjectId> createStair(const geom::StairSpec& spec);
  // Refused for an object that is not such a stair any more (its shape edited, or tilted).
  Result<StairInfo> stairInfo(const ObjectId& id) const;
  // The same stair with other sizes (the same object); floors opened for it are opened again.
  Status changeStair(const ObjectId& id, const geom::StairSpec& spec);
  // Opens floors made by the slab tool over the stair, where a tread has less than `headroom`
  // to the floor's underside. `slabs` empty: every slab whose thickness holds the stair's top.
  // The slab stays a slab (outline, thickness); the opening follows the stair and the slab and
  // closes when the stair is deleted. The slabs opened; refused when there is none.
  Result<std::vector<ObjectId>> openFloorForStair(const ObjectId& stair, const std::vector<ObjectId>& slabs = {},
                                                  double headroom = 2000.0);
  Status closeFloorForStair(const ObjectId& stair);

  // --- rooms (docs/90 G-07) -------------------------------------------------
  // The region `at` is in, bounded by the walls made by the wall tool that stand at its
  // height (those cut by a plan 1100 above it). closed = false with `problem` when no walls
  // enclose it: nothing is closed for the user.
  RoomInfo roomAt(const gp_Pnt& at) const;
  // Puts a room marker (a text note, attribute kind = room) there. Refused when the place is
  // not enclosed, or already has a room. One undo step.
  Result<ObjectId> createRoom(const gp_Pnt& at, const std::string& name);
  // Every room marker with its region as the walls are now.
  std::vector<RoomInfo> rooms() const;
  // A room's use, finishes and ceiling height (docs/115), kept on its marker. All empty: taken
  // away. Each word at most 60 characters on one line; the ceiling height 0 (not given) or
  // 1000 to 10000. One undo step.
  Status setRoomFinish(const ObjectId& room, const RoomFinish& finish);
  // Ceilings as boards (docs/97 N-02, docs/123): for each room (all when `rooms` is empty) with a
  // ceiling height and a known inner outline, a board 10 thick whose underside is at that height
  // above the room's floor, in the outline within the wall faces (attribute kind = ceiling,
  // room = the room). A room's ceiling made before is made again. Rooms without a ceiling height
  // or inner outline are named. One undo step. The ceilings made.
  struct Ceilings {
    std::vector<ObjectId> made;
    std::vector<std::string> left;  // room names
  };
  // Boards made from docs/142 on remember what they were made as (attribute ceiling_made); while
  // they are still that (not moved, cut or resized by hand), they follow their room when its
  // walls or its ceiling height change, in the same undo step. Changed by hand, they stay.
  Result<Ceilings> makeCeilings(const std::vector<ObjectId>& rooms = {});
  // The same, from a document alone (tables and drawings run without a Modeler).
  static std::vector<RoomInfo> roomsIn(const Document& document);
  // Everything the walls standing on a floor at height `floor` enclose (壁芯), with or
  // without rooms placed in it.
  struct EnclosedArea {
    double area = 0.0;  // mm2
    int regions = 0;
    int unreadableWalls = 0;
  };
  static EnclosedArea enclosedAt(const Document& document, double floor);
  // The outlines of those regions on the wall lines (壁芯), at z 0 (docs/141).
  static std::vector<std::vector<gp_Pnt>> enclosedOutlines(const Document& document, double floor);

  // Whether moves keep wall joints (on by default; a setting of the session, not saved).
  void setKeepWallJoins(bool on) { keepWallJoins_ = on; }
  bool keepWallJoins() const { return keepWallJoins_; }
  // Walls made from now on (createWalls, createTypedWalls) get mitred corners (docs/159) in the
  // same undo step as they are made (docs/177): the wall tool's default. Off unless set.
  void setMitreNewWalls(bool on) { mitreNewWalls_ = on; }
  bool mitreNewWalls() const { return mitreNewWalls_; }
  // What the last transform did to joined walls (empty when none were joined).
  const JoinFollow& lastJoinFollow() const { return lastJoins_; }
  Status move(const std::vector<ObjectId>& ids, const gp_Vec& offset);
  Status rotate(const std::vector<ObjectId>& ids, const gp_Ax1& axis, double angleRadians);
  // Uniform scale about a world point (one undo step). Solids, faces and
  // curves get a parametric ScaleOp (merged with a directly preceding one);
  // guides and annotations have their points scaled. Groups scale all their
  // contents; components are refused (edit the definition instead).
  Status scale(const std::vector<ObjectId>& ids, const gp_Pnt& center, double factor);
  // Non-uniform scale (SketchUp's scale grips): along the world directions
  // xDir, yDir and xDir x yDir by fx, fy, fz about worldCenter. The B-rep
  // geometry is transformed exactly (a recipe step, StretchOp), painted
  // faces and hidden lines follow. Equal factors are the uniform scale.
  // Factors must be positive (no mirroring here). Groups stretch their
  // contents; component placements are refused (they would change the
  // shared definition; make it unique first); images too (uniform only).
  // One undo step.
  Status stretch(const std::vector<ObjectId>& ids, const gp_Pnt& worldCenter, const gp_Dir& xDir, const gp_Dir& yDir,
                 double fx, double fy, double fz);
  // --- architectural intents (shared by GUI and AI) -----------------------
  // Openings: rectangular profiles subtracted from a host (a wall with a
  // door / window pushed through), numbered 1, 2, ... in the order they
  // were cut (also through nested Subtracts). The number of an opening changes when
  // an earlier one is removed; its `id` does not (docs/90 B-01).
  struct OpeningInfo {
    std::size_t index = 0;
    // Stays with the opening. Openings cut before ids existed report their number as text
    // until the wall's openings are next edited, which gives them ids.
    std::string id;
    double offset = 0.0;  // from the host's left end (seen from the side it was cut from) to its left edge
    double sill = 0.0;    // from the host's bottom to its lowest edge
    std::string name;
    double width = 0.0, height = 0.0;  // absolute
    gp_Pnt center;                     // world
    double bottom = 0.0;               // world Z of the sill / lowest edge
    gp_Dir normal;                     // of the face it was cut from (world)
    double along = 0.0;                // horizontal extent along the wall (Z x normal)
    double depth = 0.0;                // through the host, from the face along -normal (wall thickness)
  };
  Result<std::vector<OpeningInfo>> openings(const ObjectId& host) const;
  // The same, from a document alone (drawings run on a copy without a Modeler).
  static Result<std::vector<OpeningInfo>> openingsIn(const Document& document, const ObjectId& host);
  // Door / window kind of openings (plan symbols): `fitting` is a JSON object, e.g.
  // {"type":"swing_single","hinge":"left","swing":"in"}; applies to `indices` (1-based, all
  // when empty). Stored on the host as attribute "opening_fittings". One undo step.
  Status setOpeningFittings(const ObjectId& host, const std::vector<std::size_t>& indices, const std::string& fitting);
  // Fitting types (docs/171): a fitting's kind and sizes (frame, leaf, glass) kept under a name in
  // the register of types (part "fitting"). A fitting given as {"type_name": name} (with its own
  // hinge / swing / slide if wanted) takes the type's kind and sizes and keeps the name; defining
  // the type again changes every fitting of that name in every wall, the hands kept (one undo
  // step; how many). Removing it leaves the fittings as they are, without the name.
  struct FittingOpeningSize {
    double width, height, sill;
  };
  // The opening size is a placement default. It never resizes existing openings.
  // A host/index registers that opening's dimensions and gives its fitting the type's name.
  // Without a source or an explicit size, any saved placement default is kept.
  Result<int> defineFittingType(const std::string& name, const std::string& fitting, const ObjectId& host = {},
                                std::size_t index = 0, std::optional<FittingOpeningSize> openingSize = {});
  std::optional<FittingOpeningSize> fittingOpeningSize(const std::string& name) const;
  std::map<std::string, std::string> fittingTypes() const;  // name -> its fitting (JSON)
  Result<int> removeFittingType(const std::string& name);
  // A mark of one's own and a product number for openings (docs/109): kept under the openings'
  // ids as attribute "opening_marks" on the host, and used by the schedule and the plan tags
  // instead of the mark worked out from kind and size. Both empty: back to the worked-out
  // mark. Applies to `indices` (1-based, all when empty). One undo step.
  Status setOpeningMarks(const ObjectId& host, const std::vector<std::size_t>& indices, const std::string& mark,
                         const std::string& product);
  // docs/203: what a person worked out for daylight and ventilation of openings (法 28 条, 令 20 条):
  // the daylight factor (採光補正係数, 0 to 3) with its basis, and the area open for ventilation
  // (m²). Kept by opening id as attribute "opening_daylight" on the host; NaN leaves what is
  // stored, a value below 0 takes it away (both away: none). Applies to `indices` (1-based, all when empty). One undo step.
  Status setOpeningDaylight(const ObjectId& host, const std::vector<std::size_t>& indices, double factor,
                            const std::string& basis, double ventilationM2);
  // docs/203: a room's kind for daylight (dwelling: 住宅の居住のための居室 / other: another kind of
  // 居室 / none: not a 居室 / empty: not given) and how it is ventilated (window / equipment /
  // empty), as attribute "room_daylight" on its marker. One undo step.
  Status setRoomDaylight(const ObjectId& room, const std::string& kind, const std::string& ventilation);
  // docs/211: the room joined to another by fusuma / shoji and the like (法 28 条 4 項: one room for
  // daylight and ventilation; "" takes it away; kept on both rooms) and the effective airflow of
  // the mechanical ventilation serving it (m³/h, 令 20 条の 8; 0 or below takes it away; NaN leaves
  // it). In "room_daylight". One undo step.
  // docs/213: fanUnit names the ventilation unit serving it (rooms of one name share one unit, and
  // an airflow given goes to all of them; "" takes it away; nullopt leaves it).
  Status setRoomVentilation(const ObjectId& room, const std::string& joinedWith, double fanM3h,
                            const std::optional<std::string>& fanUnit = std::nullopt);
  // docs/211: openings with a veranda 90 cm wide or more outside them (令 20 条 2 項: the daylight
  // factor worked out x 0.7), by opening id in "opening_daylight". One undo step.
  Status setOpeningVeranda(const ObjectId& host, const std::vector<std::size_t>& indices, bool veranda);
  // Kinds of floors and roofs (docs/97 N-08, docs/134): slabs and roofs given one type name
  // (attribute type_name, e.g. "F1", "R1") are one type. setTypeName names them (empty: no type;
  // other objects refused); one undo step. changeElementType changes every one of that name:
  // slabs their thickness (the top kept), roofs their pitch, thickness, eave and gable overhangs
  // (outline and eave height kept); all or nothing, one undo step; how many. matchElementType:
  // the others of this one's name take its values; how many (0: it is the only one).
  Status setTypeName(const std::vector<ObjectId>& ids, const std::string& name);
  struct ElementTypeChange {
    std::optional<double> thickness, pitch, eave, gableEave;
  };
  Result<int> changeElementType(const std::string& name, const ElementTypeChange& change);
  Result<int> matchElementType(const ObjectId& id);
  // Registered types (docs/149): a name, its part ("floor" or "roof") and its values, kept with
  // the model (saved, undone). Naming a slab / roof with a registered name gives it those values
  // (setTypeName, same undo step); changing or matching a registered type changes the register too.
  // defineElementType registers or changes one: every one of that name takes the values (one undo
  // step; refused if objects of the other part have the name); registerElementType: from an
  // object's own values under its name. Returns how many objects changed.
  // A floor type may have its layers, top to bottom (docs/151): its thickness is their sum.
  struct TypeLayer {
    std::string name;
    double thickness = 0.0;
  };
  struct ElementTypeDefinition {
    std::string name, part;
    ElementTypeChange values;
    std::vector<TypeLayer> layers;
  };
  std::vector<ElementTypeDefinition> elementTypes() const;
  // `layers`: none given, those kept; an empty list takes them off.
  Result<int> defineElementType(const std::string& name, const std::string& part, const ElementTypeChange& values,
                                const std::optional<std::vector<TypeLayer>>& layers = std::nullopt);
  // The depths below its top where the layers of a slab / roof of a registered type meet
  // (docs/151, roofs across the slope docs/152); empty when it has none, or its thickness no
  // longer adds up to them.
  static std::vector<double> typeLayerDepths(const Document& document, const Object& element);
  Result<int> registerElementType(const ObjectId& id);
  Status removeElementType(const std::string& name);
  // A canopy over an opening (庇, docs/97 N-03, docs/147): a board out from the face the opening
  // was cut from, `depth` out, `thickness` thick, `extra` longer at each side than the opening, its
  // underside `above` over the opening's head. Attribute kind = canopy, host / opening, and
  // `canopy` (where it stands out from the wall's line: for the building area). One undo step.
  // While it is as made (docs/148) it goes with its opening and wall (moved, resized, the wall
  // moved), in the same undo step; moved or reshaped by hand, or its opening gone, it stays.
  struct CanopySpec {
    double depth = 600.0, thickness = 60.0, extra = 150.0, above = 100.0;
  };
  Result<ObjectId> makeCanopy(const ObjectId& host, std::size_t index, const CanopySpec& spec);
  // A canopy's sizes as made (docs/154), and made again with other sizes over the same opening
  // (its name kept; a new id, returned). One undo step. Refused when its opening is gone.
  // A balcony on a wall (バルコニー, docs/97 N-03, docs/163): a floor out from one face of a wall
  // made by the wall tool, with a rail on its three other sides, as one solid. Attribute kind =
  // balcony, host, and `balcony` (its sizes, and where it stands out from the wall's line: for the
  // building area). While it is as made it goes with its wall when the wall moves or changes, in
  // the same undo step; moved or reshaped by hand, or its wall gone, it stays.
  struct BalconySpec {
    ObjectId host;
    double along = 0.0;        // its middle, from the wall's start along its line
    int side = 1;              // 1: on the wall's left face (looking along its line), -1: its right
    double width = 2730.0, depth = 910.0;
    double top = 0.0;          // z of its floor's top (world)
    double thickness = 150.0;  // of its floor
    double railHeight = 1100.0;  // above its floor's top
    std::string rail = "wall";   // wall (a parapet), bars (a top rail on posts) or none
    // How it counts as floor area (令 2 条 1 項 3 号): "count", "exclude", or empty: by the notice
    // on floor areas (昭和 61 年 住指発第 115 号 1 (4)(5), docs/175): open enough (1.1 m or more
    // and half its ceiling height or more), only beyond 2 m from the wall's centre line counts;
    // else all of it.
    std::string floorArea;
    // docs/167: posts under its front edge (0: none, else 2 to 6 spread along it, the two at the
    // outer corners), `postSize` square, from the next level below its floor up to its floor.
    int posts = 0;
    double postSize = 105.0;
    // docs/168: round the outer corner where its wall meets another at its "start" or "end" (an
    // L): along its wall from (along - width / 2) towards that end up to the corner (the other way
    // round for "start", from along + width / 2), then `wrapLength` along the other wall's outer
    // face from the corner. Empty: straight on its wall.
    std::string wrap;
    double wrapLength = 1820.0;
  };
  // The parts of a balcony as made (docs/167): its floor, its rail and its posts (world; a part
  // it has not is null). Refused when it was moved or reshaped by hand (no longer as made).
  struct BalconyParts {
    TopoDS_Shape floor, rail, posts;
  };
  Result<BalconyParts> balconyParts(const ObjectId& id) const;
  // In the middle of the wall, on its left face, its floor at the wall's foot.
  Result<BalconySpec> defaultBalcony(const ObjectId& wall) const;
  // For a point in plan: how far along the wall's line it is (kept within the wall) and the side
  // of the line it is on (1 left, -1 right): where a balcony pointed at there goes.
  Result<std::pair<double, int>> balconyNear(const ObjectId& wall, const gp_Pnt& at) const;
  Result<ObjectId> createBalcony(const BalconySpec& spec);
  Result<BalconySpec> balconySpec(const ObjectId& id) const;
  // Made again with other sizes (its name kept; a new id, returned). One undo step.
  Result<ObjectId> changeBalcony(const ObjectId& id, const BalconySpec& spec);
  Result<CanopySpec> canopySpec(const ObjectId& id) const;
  Result<ObjectId> changeCanopy(const ObjectId& id, const CanopySpec& spec);
  // A kind of opening (docs/97 N-08, docs/133): the openings that share a mark in the door and
  // window schedule (one given by hand, or worked out from kind, fitting and size) are one type.
  // Changed as a type, every one of them takes the new width / height (centre and sill kept),
  // fitting (a JSON object as setOpeningFittings, {"type":"none"} for none) and product number,
  // in every wall. All or nothing (refused where one would not fit), one undo step. How many.
  struct OpeningTypeChange {
    std::optional<double> width, height;
    std::optional<std::string> fitting, product;
  };
  Result<int> changeOpeningType(const std::string& mark, const OpeningTypeChange& change);
  // The other openings of this one's type take its width, height, fitting and product number
  // (changeOpeningType with this one's). How many changed (0: it is the only one).
  Result<int> matchOpeningType(const ObjectId& host, std::size_t index);
  // Resizes openings (all when `indices` is empty) keeping the horizontal
  // centre and the sill (keepCenter = false) or the centre (true) in place.
  // One undo step; the parameters stay editable.
  Status setOpeningSize(const ObjectId& host, const std::vector<std::size_t>& indices,
                        std::optional<double> width, std::optional<double> height, bool keepCenter = false);
  // The opening a face of `host` belongs to: one of the faces inside the hole (its jambs, head
  // or sill), given in world coordinates. Those faces stand for the opening when picked.
  std::optional<std::size_t> openingOfFace(const ObjectId& host, const TopoDS_Face& worldFace) const;
  // The numbers of the openings with these ids (OpeningInfo::id), in the order given.
  Result<std::vector<std::size_t>> openingIndices(const ObjectId& host, const std::vector<std::string>& ids) const;
  // Moves one opening within its wall by the part of `worldDelta` that lies in the wall's face
  // (along the wall and up / down). It must stay inside the wall and clear of the other
  // openings. Its size and fitting stay. One undo step.
  Status moveOpening(const ObjectId& host, std::size_t index, const gp_Vec& worldDelta);
  // Whether that move would be accepted (the same checks), changing nothing: what a tool
  // shows while the opening is dragged.
  Status canMoveOpening(const ObjectId& host, std::size_t index, const gp_Vec& worldDelta) const;
  // The same, to a position: `offset` and / or `sill` as in OpeningInfo.
  Status placeOpening(const ObjectId& host, std::size_t index, std::optional<double> offset,
                      std::optional<double> sill);
  // A copy of one opening (docs/110): the same width, height and fitting, and the same mark and
  // product number when given by hand; a new id. Within its wall it goes by the part of
  // `worldDelta` in the wall's face (along and up / down, as moveOpening); into `target` (another
  // wall) it is centred along that wall at `worldDelta` taken as a point, at the same sill
  // height, cut from the side facing the way the original does (or that wall's left side).
  // Refused, with nothing changed, where it would not fit or would overlap an opening. One undo
  // step. The new opening's id.
  Result<std::string> copyOpening(const ObjectId& host, std::size_t index, const gp_Vec& worldDelta,
                                  const std::optional<ObjectId>& target = std::nullopt);
  // A copied opening pasted where clicked (Ctrl+C / Ctrl+V, docs/126): `point` on (or over, in a
  // plan) a wall made by the wall tool. In the opening's own wall it goes along to there, at the
  // same sill; in another wall it is centred there, at the same sill (copyOpening). `openingId`
  // is the opening's id, so other openings changing in between do not matter. Refused, nothing
  // changed, when the opening is gone or no wall is there. One undo step. The new opening's id.
  Result<std::string> pasteOpening(const ObjectId& host, const std::string& openingId, const gp_Pnt& point);
  // Walls made by the wall tool whose end meets `host` where one of its openings is (docs/118):
  // the wall would end in the hole. Nothing stops it; a tool or an agent says so. Per opening
  // (its number) the names of those walls.
  std::vector<std::pair<std::size_t, std::string>> wallsMeetingOpenings(const ObjectId& host) const;
  // A storey copied to another level (docs/97 N-07, docs/121): every top-level solid starting in
  // the storey of `fromLevel` (300 under its floor to 300 under the next floor, as the 3D storey
  // view) and its room markers, moved up by the difference of the levels; what was on a level
  // goes onto the level as far above (or loses it when there is none). Slabs are not copied when
  // the storey above has a slab already. Refused when walls already stand in the target storey
  // (they would be doubled). One undo step. The copies made.
  struct StoreyCopy {
    std::vector<ObjectId> made;
    int walls = 0, slabs = 0, rooms = 0, others = 0;
    bool slabsLeft = false;  // the target had a slab: the storey's slabs were not copied
  };
  Result<StoreyCopy> copyStorey(const std::string& fromLevel, const std::string& toLevel);
  // The wall made by the wall tool whose band holds `p` in plan, at its height (docs/119); the
  // nearest line when several do.
  std::optional<ObjectId> wallAt(const gp_Pnt& p) const;
  // Where the layers of walls of a build-up (set_wall_type, createTypedWalls) meet, cut by a plan
  // at height `z` (docs/97 G-12, docs/124): lines along each such wall between its layers, broken
  // where an opening is cut by the plan. Walls whose thickness no longer adds up to their layers
  // are left out. `only`: those walls (all when empty).
  static std::vector<std::pair<gp_Pnt, gp_Pnt>> wallLayerLines(const Document& document, double z,
                                                               const std::set<ObjectId>& only = {});
  // The upright planes where a wall's layers meet (docs/156): a point on each and its normal
  // (across the wall). Empty as for wallLayerLines. Sections draw where they cross the cut.
  static std::vector<std::pair<gp_Pnt, gp_Dir>> wallLayerPlanes(const Document& document, const Object& wall);
  // The layers of a wall, floor or roof as they are now (docs/170): names and thicknesses in
  // order (a wall from its outer face in, a floor / roof from its top down), the set's name, and
  // for a wall where the first layer starts from its drawn line (`offset`, mm to its left) and
  // which way the layers go (`towardsLeft`). Empty when it has none or they no longer add up.
  struct LayerUsage {
    std::string setName;
    std::vector<TypeLayer> layers;
    bool wall = false;
    double offset = 0.0;
    bool towardsLeft = false;
  };
  static std::optional<LayerUsage> layerUsage(const Document& document, const Object& element);
  // Removes openings: the wall is whole again there. Their fittings go with them; the other
  // openings keep their ids, sizes and fittings. One undo step.
  Status removeOpenings(const ObjectId& host, const std::vector<std::size_t>& indices);
  // Walls and slabs on levels (docs/90 G-02). A wall made by its tool stands on a level
  // (attribute "base_level") and may reach up to one ("top_level"); a slab's top lies on one
  // ("top_level"). The values are level ids. When a level's height changes, what stands on it
  // moves by as much and a wall reaching up to it grows or shrinks; openings keep their sill
  // heights. Walls and slabs get these when they are made at a level's height; drawn lines
  // and pushed shapes never do.
  // attachToLevels sets them by hand: `base` / `top` name a level (id or name) to put that
  // end on it now and keep it there, "" lets it go, nullopt leaves it as it is. A wall whose
  // top is on a level keeps its top when its foot is moved to another one. One undo step;
  // refused as a whole when a wall would end up with no height or cannot be rebuilt.
  Status attachToLevels(const std::vector<ObjectId>& ids, std::optional<std::string> base,
                        std::optional<std::string> top);
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
  // Timber walls of a build-up (WallTypes.h) on the column centre line: the core's centre on the
  // line, the outside `type.outside()` to the left of it (looking along it) and the inside
  // `type.inside()` to the right; `outsideRight` swaps them. A closed outline is turned so the
  // outside faces out whichever way it was drawn. Attributes: kind = wall, wall_type,
  // wall_type_name, wall_layers (JSON, outside -> inside), wall_line = offset and
  // wall_line_offset (the line's distance from the wall's right face). One undo step.
  Result<std::vector<ObjectId>> createTypedWalls(std::vector<gp_Pnt> points, double height, const std::string& type,
                                                 double core = 105.0, bool outsideRight = false);
  // Gives walls already drawn (kind = wall, a rectangle pushed up, with or without openings) a
  // build-up: each keeps the line it was drawn on (its wall_line) and gets the layers around it.
  // outside: "left" / "right" of the line, or "auto" (a typed wall keeps its outside; otherwise
  // the side away from the middle of the walls changed, or of all walls; "flip": the other side
  // from that). Corners and T joints
  // with neighbouring walls are fitted again (a neighbour's end may move); openings are moved
  // to the new faces and cut through the new thickness. Returns every wall changed. One undo step.
  Result<std::vector<ObjectId>> setWallType(const std::vector<ObjectId>& ids, const std::string& type,
                                            double core = 105.0, const std::string& outside = "auto");
  // A floor slab: plan polygon (any winding) with its top at `top`,
  // `thickness` below. Attribute kind = slab. One undo step.
  Result<ObjectId> createSlab(const std::vector<gp_Pnt>& outline, double top, double thickness);
  // --- following (docs/42) ---------------------------------------------------
  // `ids` follow `host` from where they are now (nullopt: stop following).
  // Only asked for, never made by snapping or placing. Refused (nothing
  // changes) for itself, an object inside the other, or a circle of
  // followers. One undo step.
  Status setFollow(const std::vector<ObjectId>& ids, const std::optional<ObjectId>& host);
  enum class FollowState { None, Active, Suspended };  // suspended: the host is gone
  FollowState followState(const ObjectId& id) const;

  // --- linear members (docs/40) ---------------------------------------------
  // A beam, column or brace: `section` swept from `start` to `end` (world),
  // the axis line through `anchor` of it. `up`: the section's up hint
  // (default world up; columns: world +Y), fixed when made. role / material
  // class go into the attributes kind / material_class (optional). A plain
  // solid otherwise: every edit works on it. One undo step.
  struct MemberSpec {
    gp_Pnt start;
    gp_Pnt end;
    geom::Section section;
    geom::SectionAnchor anchor;
    std::optional<gp_Dir> up;
    double roll = 0.0;  // radians
    std::string role;
    std::string materialClass;
    std::string name;
  };
  Result<ObjectId> createMember(const MemberSpec& spec);
  // Timber recipe (docs/52 stage 2): shared parts, levels and layer groups,
  // committed as one command. Inputs / result use the CommandApi JSON form.
  Result<std::string> generateFrame(const std::string& argsJson);
  // Concrete strip-foundation planning geometry from existing sills; one undo.
  Result<std::string> generateFoundation(const std::string& argsJson);
  // Members on a grid (docs/43), pinned to its crossings: columns standing
  // on the crossings, beams from crossing to neighbouring crossing along
  // the x lines (running along the grid's Y) and the y lines. Only the
  // lines listed (axis ids; empty: all). Columns from `base` above the
  // grid's plane up `height`; beams at base + height. Sections, anchors and
  // roles from the specs (their points ignored). A crossing or span that
  // already has a member pinned the same way is skipped. One undo step.
  struct GridMemberOptions {
    std::vector<std::string> x, y;
    double base = 0.0;
    double height = 3000.0;
    std::optional<MemberSpec> columns;
    std::optional<MemberSpec> xBeams;  // on the x lines
    std::optional<MemberSpec> yBeams;  // on the y lines
    // Levels (ids, docs/45) instead of base and height: columns from the
    // base level up to the top level, beams on the top level; the members'
    // ends then follow those levels.
    std::string baseLevel, topLevel;
  };
  struct GridMembers {
    std::vector<ObjectId> columns, beams;
    std::size_t skipped = 0;
  };
  Result<GridMembers> placeMembersOnGrid(const ObjectId& grid, const GridMemberOptions& options);
  // Columns, beams and other members drawn before (docs/97 G-06, docs/122) pinned to a grid now:
  // each end on a crossing (within `tolerance` in plan) is pinned to it, and to the level at its
  // height; from then on they follow the grid and the levels as placed members do. Walls made by
  // the wall tool (docs/129): each end of its line to the nearest crossing within its thickness
  // (+ `tolerance`); when the grid's spacings change each end goes as far as its crossing (the
  // wall goes sideways, gets longer or shorter; walls joined to it follow). `ids` empty: every
  // member and wall not pinned yet. `grid` empty: the only grid. Members whose ends are not both
  // on crossings, walls not between two crossings, and other things are left and named. One undo
  // step.
  struct GridPinning {
    int pinned = 0;
    int walls = 0;  // of `pinned`
    std::vector<std::string> left;  // names
  };
  Result<GridPinning> pinToGrid(const std::vector<ObjectId>& ids, const std::string& grid, double tolerance = 10.0);
  // Members and walls let go of their grid (docs/144): they stay where they are and no longer
  // follow it (nor the levels pinned with it). `ids` empty: every one pinned. One undo step. How many.
  Result<int> unpinFromGrid(const std::vector<ObjectId>& ids);
  // What can be edited of a member as it is now: each value with what it
  // means and where it comes from (the member's parameter, or measured on
  // its shape after other edits). Computed when asked, never kept.
  struct MemberField {
    std::string key;      // length, width, depth (diameter), web, flange, radius, roll
    std::string meaning;
    std::string unit;     // mm or deg
    double value = 0.0;
    std::string basis;    // parameter / shape
    bool editable = true;
    std::string note;
  };
  struct MemberInfo {
    ObjectId id;
    std::string element;  // member, wall or slab
    std::string section;  // current section's name
    std::string catalog;  // the catalog entry it is now ("" own dimensions)
    std::string origin;   // the catalog entry it was made from
    geom::Section::Kind kind = geom::Section::Kind::Rectangle;
    geom::SectionAnchor anchor;
    bool edited = false;  // edited after it was made (push/pull, holes, ...)
    std::string role;
    std::string materialClass;
    std::vector<MemberField> fields;
  };
  Result<MemberInfo> memberInfo(const ObjectId& id) const;
  // Basic edits, all together in one step (checked as a whole, never one
  // by one): another catalog section (anchor, rotation and length kept),
  // own dimensions, length (absolute: the start stays, a column's foot),
  // rotation, or the anchor (the member stays where it is; only the axis
  // line moves). Other edits made since are kept: when they cannot be
  // redone on the changed member, nothing changes and the reason is given.
  struct MemberChange {
    // A whole new section (any kind: own rectangle / circle, or a catalog
    // one), the member's axis, anchor and roll kept.
    std::optional<geom::Section> section;
    std::optional<std::string> catalog;
    std::optional<double> width, depth, web, flange, radius, length, roll;  // mm, mm..., mm, radians
    std::optional<double> diameter;  // a round section
    // Walls and slabs (docs/41): thickness (wall: keep its left / right /
    // center line, default the one it was drawn on; slab: keep top
    // (default) / bottom / center), height (wall, from its foot), length.
    std::optional<double> thickness, height;
    std::optional<std::string> keep;
    std::optional<geom::SectionAnchor> anchor;
  };
  Result<MemberInfo> changeMember(const ObjectId& id, const MemberChange& change);

 private:
  // Runs `cut` (making openings in `host`, returning the new host) and gives the new openings
  // `fitting`, as one undo step.
  Result<ObjectId> withFitting(const ObjectId& host, const std::string& fitting,
                               const std::function<Result<ObjectId>()>& cut);
  // Walls along `points`, each the band [o, o + thickness] to the left of its segment, tagged
  // kind = wall plus `attributes`; one undo step.
  Result<std::vector<ObjectId>> createWallBands(std::vector<gp_Pnt> points, double thickness, double height, double o,
                                                const std::vector<std::pair<std::string, std::string>>& attributes);
  std::optional<MemberInfo> wallOrSlabInfo(const Object& o) const;
  Result<MemberInfo> changeWallOrSlab(const ObjectId& id, const MemberChange& change);

 public:

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
  // The same with a door / window fitting (OpeningSymbols.h) for the new openings; one undo step.
  Result<ObjectId> placeOpeningAt(const ObjectId& host, const gp_Dir& faceNormal, const gp_Pnt& at, double width,
                                  double height, double sill, const std::string& fitting);
  Result<ObjectId> placeOpenings(const ObjectId& host, const gp_Dir& faceNormal, int count, double width,
                                 double height, double sill, std::optional<double> spacing, const std::string& fitting);
  // Copies the objects `count` times, copy k placed at worldDelta^k (linear
  // or polar arrays). Returns the ids of the new top-level copies.
  // What was just drawn, moved, copied, pasted or offset (`ids`) against
  // what is already there in the same edit context (same parent; never
  // other groups, components or solids' own faces), from the 3D shapes and
  // the geometric tolerance, so nothing is stacked twice:
  //  - lines: pieces lying on an edge already there (a line, a face's or a
  //    solid's edge) are dropped, the rest kept with their connection
  //    points; a loose line another one ends on gets a vertex there;
  //  - flat faces on the same plane (either side): the overlap is kept
  //    once, by the face already there (its material, attributes and
  //    side); each face is divided along the other's outline (holes kept),
  //    and every region is a face of its own (select, paint, push/pull).
  // Only those objects and their neighbours change. Part of the caller's
  // undo step. `became`: what each id is now (empty = it went: everything
  // already there; an id of an existing object = it took the place).
  struct Settled {
    std::map<ObjectId, std::vector<ObjectId>> became;
    int linesTrimmed = 0;
    int linesSplit = 0;
    int facesDivided = 0;
  };
  Result<Settled> settleOverlaps(const std::vector<ObjectId>& ids);
  // The ids after settling (in order, without repeats).
  static std::vector<ObjectId> settledIds(const std::vector<ObjectId>& ids, const Settled& settled);

  // Copies only these edges (world; edges of lines, faces or solids picked
  // one by one) `count` times, copy k at worldDelta^k, as lines of their own
  // (exact: straight runs as polylines, arcs as arcs, other curves as they
  // are). A straight copy lying across a face of the current group from
  // edge to edge divides that face instead (SketchUp); a copy on top of an
  // existing edge is not made twice. One undo step; returns the new lines.
  Result<std::vector<ObjectId>> copyEdges(const std::vector<TopoDS_Edge>& worldEdges, const gp_Trsf& worldDelta,
                                          int count = 1);
  // Most objects one step may make by copying (all copies of all contents
  // together, and across the commands of an enclosing batch).
  static constexpr std::size_t kMaxObjectsPerStep = 200000;
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
  // What is selected (the same targets as deleteSelection): whole objects
  // as above; faces and edges picked on an object copy just those, never
  // the object they belong to: each face as a face of its own (holes kept,
  // its material), edges not bounding a copied face as loose lines (exact
  // arcs and curves).
  Result<Clipboard> copyToClipboard(const std::vector<DeleteTarget>& targets) const;
  // Move + Ctrl on such a selection: `count` copies, copy k at worldDelta^k
  // (an array), one undo step; returns the new ids.
  Result<std::vector<ObjectId>> copyElements(const std::vector<DeleteTarget>& targets, const gp_Trsf& worldDelta,
                                             int count = 1);
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

  // --- Rhino-style surfaces and edge operations (docs/47) ---------------------
  // A curve to build from: a whole curve object (kept parametric: editing
  // it later edits the input), or one edge of any object (world, a copy).
  struct CurvePick {
    ObjectId id;
    std::optional<TopoDS_Edge> edge;
  };
  // Profiles along rail1, keeping touch with rail2 (Sweep2). A new object;
  // the curves stay.
  Result<ObjectId> sweep2(const CurvePick& rail1, const CurvePick& rail2, const std::vector<CurvePick>& profiles);
  // EdgeSrf (2-4 curves) or, with `patch`, a smooth patch over one closed
  // outline. A new object; the curves stay.
  Result<ObjectId> surfaceFromCurves(const std::vector<CurvePick>& curves, bool patch);
  // Edges of a solid (world edges) rounded / bevelled; the size stays
  // editable: returns its parameter key (setParameter). One undo step.
  Result<std::string> roundEdges(const ObjectId& id, const std::vector<TopoDS_Edge>& worldEdges, double size,
                                 bool chamfer);
  // Corners of a flat face object (world points at the corners) rounded /
  // cut; the size stays editable (returns its parameter key). One undo step.
  Result<std::string> roundCorners(const ObjectId& id, const std::vector<gp_Pnt>& worldCorners, double size,
                                   bool chamfer);
  // AutoCAD's fillet / chamfer of two straight lines: each is extended or
  // cut back to where they meet, the part on the side of `near` kept, and
  // joined there by an arc of `size` (chamfer: a straight cut `size` back
  // along each; 0: they just meet). A line is a segment of a line object;
  // only its free ends move (two segments meeting inside one line object
  // are split there). The new arc / cut is returned (empty for 0). If the
  // lines then close an outline, its face is made. One undo step.
  struct LinePick {
    ObjectId id;
    std::size_t segment = 0;  // points[segment] -> points[segment + 1]
    // Where it was clicked (world): the side kept. (Not "near": a macro
    // in the Windows headers.)
    gp_Pnt clicked;
  };
  Result<std::optional<ObjectId>> filletLines(const LinePick& a, const LinePick& b, double size, bool chamfer);
  // The solid hollowed, these faces (world) left open; returns the
  // thickness's parameter key. One undo step.
  Result<std::string> shellSolid(const ObjectId& id, const std::vector<TopoDS_Face>& worldFaces, double thickness);
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
  // A copy of the model in a new .sbr file (docs/111): never over a file that is there, and the
  // document stays as it is (not marked saved; the user's own file is untouched).
  Status saveCopy(const std::string& path) const;
  // verifyRecipes = false opens with the stored geometry only (fast for
  // large models); verify later with verifyRecipes(cursor, budget).
  Status load(const std::string& path, std::vector<std::string>* warnings = nullptr, bool verifyRecipes = true);
  // Checks up to `budget` objects from `cursor` on (advanced); returns the
  // warnings found. Done when cursor reaches the object count.
  std::vector<std::string> verifyRecipes(std::size_t& cursor, std::size_t budget) const;
  // Exports the given objects (all visible objects when empty) as STEP,
  // keeping object names and colours.
  Status exportStep(const std::string& path, const std::vector<ObjectId>& ids = {}) const;
  // glTF for renderers (docs/85_gltf_export): visible objects (or `ids`) with
  // their materials' colour, opacity, metallic and roughness.
  Status exportGltf(const std::string& path, const std::vector<ObjectId>& ids = {},
                    const geom::GltfExportOptions& options = {}) const;
  Status exportBrep(const std::string& path, const std::vector<ObjectId>& ids = {}) const;
  Status exportIges(const std::string& path, const std::vector<ObjectId>& ids = {}) const;
  Result<std::vector<ObjectId>> importBrep(const std::string& path);
  Result<std::vector<ObjectId>> importIges(const std::string& path);
  Status exportStl(const std::string& path, const std::vector<ObjectId>& ids = {},
                   const geom::StlExportOptions& options = {}) const;
  Result<std::vector<ObjectId>> importDxf(const std::string& path, const geom::DxfImportOptions& options = {},
                                         geom::DxfImportReport* report = nullptr);
  // Jw_cad .jww (docs/50): lines and arcs at real size, layers as tags.
  Result<std::vector<ObjectId>> importJww(const std::string& path, const geom::JwwImportOptions& options = {},
                                         geom::JwwImportReport* report = nullptr);
  Result<std::vector<ObjectId>> importStep(const std::string& path);
  // Rhino .3dm: exact NURBS Breps with names, colours and tags as layers.
  Status export3dm(const std::string& path, const std::vector<ObjectId>& ids = {}) const;
  // Refuses a file that does not come through whole (unsupported objects,
  // failed or partial conversions; the error says what) unless
  // `allowPartial`; `report` (optional) says what was found either way.
  Result<std::vector<ObjectId>> import3dm(const std::string& path, bool allowPartial = false,
                                          geom::Import3dmReport* report = nullptr);

  // COLLADA (.dae, docs/46), for SketchUp Pro: the given objects (all
  // visible ones when empty) as meshes in mm, each object's colours and
  // faces painted on their own kept; loose lines as lines.
  Status exportCollada(const std::string& path, const std::vector<ObjectId>& ids = {},
                       const geom::ColladaExportOptions& options = {}) const;
  // Each mesh of the file an object (faces joined, closed ones solids),
  // coloured from its materials; one undo step. `report` says what came.
  Result<std::vector<ObjectId>> importCollada(const std::string& path, geom::ColladaReport* report = nullptr);
  // SketchUp (.skp, docs/181) through the SketchUp SDK on this PC: as COLLADA, each object a
  // group (flat faces whole, with their holes); the file's groups and components each an object.
  Status exportSkp(const std::string& path, const std::vector<ObjectId>& ids = {},
                   const geom::SkpExportOptions& options = {}) const;
  Result<std::vector<ObjectId>> importSkp(const std::string& path, geom::SkpReport* report = nullptr);
  // A SketchUp file written by exportSkp and changed in SketchUp, back into the model (docs/186):
  // each group / component carrying an object's id is compared with that object. Moved or turned
  // in SketchUp (its shape as it was): the object is moved the same way (walls stay walls, their
  // joins follow). Its shape changed: listed as edited, or with `replaceEdited` the object takes
  // the shape from SketchUp (it is then a shape, no longer what its tool made). Drawn anew in
  // SketchUp: added. Written to the file but gone from it: listed, or removed with
  // `removeMissing`. One undo step.
  struct SkpSync {
    std::vector<ObjectId> moved, unchanged, edited, replaced, added, missing, removed, locked;
    geom::SkpReport report;
  };
  Result<SkpSync> syncSkp(const std::string& path, bool replaceEdited = false, bool removeMissing = false);

  // --- queries ------------------------------------------------------------
  std::vector<ObjectSummary> listObjects() const;
  std::vector<Parameter> parameters(const ObjectId& id) const;
  // Removes ids whose ancestor is also in the list (operating on a group
  // already covers its children).
  std::vector<ObjectId> topmost(const std::vector<ObjectId>& ids) const;

 private:
  // Those of `ids` that are slabs take `change`'s thickness, roofs its values; no group of its own.
  Status applyElementValues(const std::vector<ObjectId>& ids, const ElementTypeChange& change);
  // Push/pull with the starting face kept, on an open shape: a new solid
  // from that face (its own object, distance editable), the face taken out
  // of the open shape (no duplicate faces), nothing else changed or filled.
  Result<PushPullResult> volumeFromOpenFace(const ObjectId& id, const TopoDS_Face& worldFace, double distance);
  // The object with a MoveVerticesOp for these world points (not committed).
  Result<Object> movedVertices(const Object& current, const std::vector<gp_Pnt>& worldPoints,
                               const gp_Vec& worldDelta, bool autofold) const;
  Result<ObjectId> cutOpenings(const ObjectId& host, const gp_Dir& faceNormal, int count, double width,
                               double height, double sill, std::optional<double> spacing,
                               std::optional<gp_Pnt> at);
  Result<ObjectId> addObject(Object object, const std::string& commandName);
  // The member object for `spec` (named, not built or added).
  Result<Object> memberObject(const MemberSpec& spec);
  Status commitModified(Object object, const std::string& commandName);
  Status commitWallChange(Object object, const std::string& commandName, double lengthened);
  // docs/142: after a step that changed walls or rooms, the ceiling boards still as they were made
  // are made again over their rooms as they are now, in the same undo step.
  void followCeilings(const Command& step);
  bool followingCeilings_ = false;
  bool keepWallJoins_ = true;
  bool mitreNewWalls_ = false;
  JoinFollow lastJoins_;
  // settleOverlaps' two halves (one object each).
  Status settleLine(const ObjectId& id, Settled& out);
  // A loose line dividing the faces it runs across (splitAcross); the line
  // is replaced by its unused parts. `now`: what the line became.
  Status divideWithLine(const ObjectId& lineId, std::vector<ObjectId>& now);
  Status divideWithCurve(const ObjectId& curveId, std::vector<ObjectId>& now);
  // Loose lines of `parent` lying on this face (world) and its plane
  // divide it (lines drawn first, the face after).
  Status divideWithLinesOn(const ObjectId& parent, const TopoDS_Face& face, const gp_Pln& plane);
  Status settleFace(const ObjectId& id, Settled& out);
  // Faces and open shells of `id`'s group joined to it edge by edge: when
  // together they close a volume (no gap, no edge with three faces, valid),
  // they are replaced by one solid (SewnFeature), materials kept.
  Status closeIntoSolid(const ObjectId& id, Settled& out);
  // `base` replaced by these world faces (the first keeps its id), with its
  // material, tag and attributes; returns the ids.
  Result<std::vector<ObjectId>> replaceByFaces(const Object& base, const std::vector<TopoDS_Face>& worldFaces);
  Result<ObjectId> addProfile(Object object, const std::string& commandName);
  Result<ObjectId> followMeAlong(const ObjectId& profileId, const BooleanOperand& path);
  // True if a profile lying on a solid's face points into that solid.
  bool facesIntoSolid(const TopoDS_Face& worldFace, const ObjectId& parent) const;
  std::vector<geom::NamedShape> exportParts(const std::vector<ObjectId>& ids) const;
  Result<TopoDS_Shape> exportGeometry(const std::vector<ObjectId>& ids, bool facesOnly = false) const;
  // The visible objects (or `ids` and what is under them) as coloured parts, for COLLADA and SketchUp.
  Result<std::vector<geom::ColladaPart>> colouredParts(const std::vector<ObjectId>& ids) const;
  // Coloured parts read from a file as objects, one undo step.
  Result<std::vector<ObjectId>> addColouredParts(const std::vector<geom::ColladaPart>& parts, const std::string& path,
                                                 const std::string& commandName);
  Result<std::vector<ObjectId>> addImported(const std::vector<geom::NamedShape>& parts, const std::string& path,
                                            const std::string& commandName);
  Result<ObjectId> faceFromCurveJunctions(const ObjectId& curveId, bool* loopFound);
  std::string uniqueName(const std::string& base) const;
  // Deep copy of a subtree with fresh ids. Returns objects parents-first.
  std::vector<Object> cloneSubtree(const ObjectId& id, const ObjectId& newParent,
                                   std::map<ObjectId, ObjectId>* copies = nullptr) const;
  void refollowCopies(const std::map<ObjectId, ObjectId>& copyOf, const std::vector<ObjectId>& created);
  // Push/pull of a profile lying on a face of another solid: returns the
  // committed cut/join, or nullopt to fall back to a plain extrusion.
  std::optional<Result<PushPullResult>> mergeIntoHost(const Object& profileBefore, Object extruded,
                                                      const std::string& key);
  // Push/pull of a profile whose volume overlaps or shares a face with
  // solids beside it (same context): one solid, as loose geometry in
  // SketchUp; the seams between them go. Nullopt: none (a plain extrusion).
  std::optional<Result<PushPullResult>> joinTouching(const Object& profileBefore, Object extruded,
                                                     const std::string& key);
  Status checkSameParent(const std::vector<ObjectId>& ids, ObjectId& parent) const;

  Document document_;
  CommandStack history_;
  ObjectId activeGroup_;
  bool settling_ = false;
};

}  // namespace sbr::doc
