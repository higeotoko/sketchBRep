#pragma once
// Interactive modelling tools (SketchUp-style direct manipulation).

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <QCursor>
#include <QElapsedTimer>
#include <QPoint>
#include <QString>

#include <gp_Ax1.hxx>
#include <gp_Pln.hxx>

#include "sbr/doc/Modeler.h"
#include "sbr/geom/Profile.h"
#include "sbr/snap/Inference.h"

class QKeyEvent;

namespace sbr::app {

class Viewport;
struct ElementRef;

struct ToolEvent {
  QPoint pos;  // widget logical pixels
  Qt::KeyboardModifiers modifiers;
};

// Services the main window provides to tools.
class ToolHost {
 public:
  virtual ~ToolHost() = default;
  virtual doc::Modeler& modeler() = 0;
  virtual Viewport& viewport() = 0;
  virtual void setStatus(const QString& text) = 0;
  virtual void setMeasurement(const QString& label, const QString& value) = 0;
  virtual void reportError(const QString& text) = 0;
  // Whole-object selection (groups, components, results of commands).
  virtual void setSelection(const std::vector<doc::ObjectId>& ids) = 0;
  // Face/edge selection; the owning objects become the object selection
  // (for the panel and for Move/Rotate) without highlighting them whole.
  virtual void setElementSelection(const std::vector<ElementRef>& elements) = 0;
  // Faces / edges of raw geometry together with objects taken whole
  // (groups, components, locked ones) - e.g. from a selection box.
  virtual void setMixedSelection(const std::vector<ElementRef>& elements, const std::vector<doc::ObjectId>& whole) = 0;
  virtual std::vector<ElementRef> elementSelection() const = 0;
  virtual std::vector<doc::ObjectId> selection() const = 0;
  // What is selected, resolved now, the one meaning shared by Delete,
  // copy / cut and Move / Rotate copies: per object, the faces and edges
  // picked on it, or the object as a whole (picked whole, or every face and
  // edge of it picked).
  virtual std::vector<doc::DeleteTarget> selectionTargets() const = 0;
  // Group edit mode (double-click a group): picks resolve inside it and new
  // geometry is created in it.
  virtual doc::ObjectId editingGroup() const = 0;
  virtual void enterGroup(const doc::ObjectId& groupId) = 0;
  virtual void exitGroup() = 0;
  // Puts 'text' into the measurement box with keyboard focus (IME capable)
  // so the user can type a replacement and press Enter.
  virtual void beginTextEntry(const QString& label, const QString& text) {
    setMeasurement(label, text);
  }
  // The paint tool picked up a material (Alt+click): the panel follows.
  virtual void materialPicked(const std::string&) {}
  // Brings the material library into view (paint tool without a material).
  virtual void showMaterials() {}
};

// The edit a tool has just committed, which a value typed right after may
// revise ("type to change it", spec 07 / review §7). Common contract:
// - it is "recent" only while nothing else has touched the model since (no
//   other edit, undo or redo, by the user or an AI) and the tool stayed
//   active; camera moves, selection and AI reads keep it;
// - a revision stays in the same undo step as the edit, however often it
//   is retyped: one Ctrl+Z takes the edit and all its revisions back.
class RecentEdit {
 public:
  // Before the tool commits an edit (optional): if the edit then records
  // nothing (e.g. a copy landing on what is there), mark() remembers that
  // there is no step of its own to take back.
  void begin(doc::Modeler& m) { begun_ = m.history().revision(); }
  // Right after the tool committed an edit.
  void mark(doc::Modeler& m);
  void clear() { token_.reset(); }
  bool fresh(doc::Modeler& m) const;
  // Changes the recent edit in place (e.g. a parameter): what `edit` does
  // is merged into the edit's undo step. Still recent afterwards.
  Status revise(doc::Modeler& m, const std::function<Status()>& edit);
  // Takes the recent edit back and does `edit` instead (e.g. placing it
  // again with other values), as the same single undo step; when `edit`
  // fails the original comes back.
  Status replace(doc::Modeler& m, const std::function<Status()>& edit);

 private:
  struct Token {
    long long revision;
    std::uint64_t sequence;
    bool empty = false;  // the edit recorded no undo step
  };
  std::optional<Token> token_;
  std::optional<long long> begun_;
};

class Tool {
 public:
  explicit Tool(ToolHost& host) : host_(host) {}
  virtual ~Tool() = default;

  virtual QString name() const = 0;
  virtual QCursor cursor() const = 0;
  virtual void activate() = 0;
  virtual void deactivate() { cancel(); }
  virtual void cancel() {}
  // Esc: go back one stage of a multi-step input (spec 07 §2.2). Returns
  // false when there is no earlier stage, and Esc then cancels it all.
  virtual bool stepBack() { return false; }
  virtual void mousePress(const ToolEvent&) {}
  virtual void mouseMove(const ToolEvent&) {}
  virtual void mouseRelease(const ToolEvent&) {}
  // Returns true if the double click was consumed.
  virtual bool mouseDoubleClick(const ToolEvent&) { return false; }
  virtual bool keyPress(QKeyEvent*) { return false; }
  virtual bool keyRelease(QKeyEvent*) { return false; }
  // Value typed into the measurement box (VCB) and confirmed with Enter.
  virtual void applyMeasurement(const QString&) {}
  // What a value typed right after may still change (see RecentEdit).
  RecentEdit& recentEdit() { return recent_; }
  // The constraints in force right now, for the status bar (review §7:
  // show the current lock / working plane / target): e.g. "方向: X 軸に
  // 固定 ｜ 作業面: 地面". Empty when nothing is fixed.
  virtual QString constraintText() const { return {}; }

 protected:
  ToolHost& host_;
  RecentEdit recent_;
};

QString snapLabel(snap::SnapKind kind);

// Click: face or edge. Double-click: plus its neighbours (face -> bounding
// edges, edge -> adjacent faces). Triple-click: the whole object. Geometry
// inside a group or component selects the whole group/component.
class SelectTool : public Tool {
 public:
  using Tool::Tool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  bool mouseDoubleClick(const ToolEvent& e) override;
  void mouseRelease(const ToolEvent& e) override;
  void cancel() override;

 private:
  void apply(const std::vector<ElementRef>& picked, bool additive);
  // Rectangle selection from empty space (spec 07 §6.1): dragged left to
  // right selects what is fully inside, right to left what it touches.
  void boxSelect(const QRect& rect, bool crossing, bool additive);
  std::optional<QPoint> boxStart_;
  bool boxAdditive_ = false;
  // Pressed on a face / edge: a click picks it; dragging on draws a box
  // instead (SketchUp), from the selection as it was before the press.
  bool boxOverElement_ = false;
  std::vector<ElementRef> beforeElements_;
  std::vector<doc::ObjectId> beforeWhole_;
  // Object that a click on `leaf` addresses at the current editing level.
  doc::ObjectId target(const doc::ObjectId& leaf) const;
  QElapsedTimer sinceDoubleClick_;
  QPoint doubleClickPos_;
};

// Shared inference handling (Shift lock, arrow-key axis locks).
class DrawingTool : public Tool {
 public:
  using Tool::Tool;
  bool keyPress(QKeyEvent* event) override;
  bool keyRelease(QKeyEvent* event) override;
  // Target, working plane and direction lock.
  QString constraintText() const override;

 protected:
  // The plane the input is held to while drawing, if any.
  virtual std::optional<gp_Pln> workingPlane() const { return std::nullopt; }
  // What the operation acts on (e.g. "3 個"), if anything yet.
  virtual QString targetText() const { return {}; }
  snap::SnapResult snapAt(const ToolEvent& e, std::optional<gp_Pnt> anchor,
                          std::optional<gp_Pln> plane);
  void resetLock();
  void resetTracking();
  void refresh();
  QString lockSuffix() const;

  std::optional<gp_Dir> locked_;
  std::vector<gp_Pnt> trackedPoints_;      // recently hovered points
  std::vector<gp_Dir> trackedDirections_;  // recently hovered straight edge
  bool shiftLock_ = false;
  snap::SnapResult last_;
  std::optional<ToolEvent> lastEvent_;
};

// Like SketchUp, Esc ends the chain and keeps what was clicked (the one
// exception to "Esc takes back one point": line users press Esc to stop).
class LineTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  bool mouseDoubleClick(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;

 protected:
  std::optional<gp_Pln> workingPlane() const override;

 private:
  std::optional<gp_Pnt> pointAt(const ToolEvent& e);
  void addPoint(const gp_Pnt& p);
  void finish();
  void updatePreview();

  std::vector<gp_Pnt> points_;
  std::optional<gp_Pnt> current_;
  std::optional<gp_Pln> drawPlane_;  // plane of the first point (face or horizontal)
};

// Points clicked on one level like the line tool (inference, typed
// lengths); clicking the first point closes the outline, double-click /
// Enter ends, Esc takes the last point back. Walls and slabs build on it.
class PlanPolylineTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  void activate() override;
  void deactivate() override;
  void cancel() override;
  bool stepBack() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  bool mouseDoubleClick(const ToolEvent& e) override;
  // Empty text (Enter without typing) finishes; a length places the next
  // point in the direction shown.
  void applyMeasurement(const QString& text) override;
  virtual QString settingsText() const = 0;

 protected:
  virtual QString startText() const = 0;
  virtual QString finishedText(std::size_t created) const = 0;
  // Creates the elements; `closed` when the outline came back to its start.
  virtual Result<std::size_t> commit(const std::vector<gp_Pnt>& points, bool closed) = 0;
  // Preview shape for the points so far (+ the cursor point).
  virtual TopoDS_Shape previewShape(const std::vector<gp_Pnt>& points) const = 0;
  virtual bool closesOnFinish() const { return false; }
  std::optional<gp_Pln> workingPlane() const override;

  std::vector<gp_Pnt> points_;


 private:
  std::optional<gp_Pnt> pointAt(const ToolEvent& e);
  void addPoint(const gp_Pnt& p);
  void finish(bool closed);
  void updatePreview();

  std::optional<gp_Pnt> current_;
};

// Walls (W): one parametric wall per segment, corners butt-joined.
// Thickness, height and justification come from the wall settings. Same
// as create_wall.
class WallTool : public PlanPolylineTool {
 public:
  struct Settings {
    double thickness = 200.0;
    double height = 3000.0;
    int justification = 0;  // -1 left of the line, 0 centred, 1 right
  };
  using PlanPolylineTool::PlanPolylineTool;
  QString name() const override;
  QCursor cursor() const override;
  void setSettings(const Settings& s) { settings_ = s; }
  const Settings& settings() const { return settings_; }
  QString settingsText() const override;

 protected:
  QString startText() const override;
  QString finishedText(std::size_t created) const override;
  Result<std::size_t> commit(const std::vector<gp_Pnt>& points, bool closed) override;
  TopoDS_Shape previewShape(const std::vector<gp_Pnt>& points) const override;

 private:
  Settings settings_;
};

// Floor slabs (Shift+W): click the outline's corners on the level of the
// slab's top (the ground, or the top of walls); the slab hangs `thickness`
// below. Enter closes the outline. Same as create_slab.
class SlabTool : public PlanPolylineTool {
 public:
  using PlanPolylineTool::PlanPolylineTool;
  QString name() const override;
  QCursor cursor() const override;
  void setThickness(double t) { thickness_ = t; }
  double thickness() const { return thickness_; }
  QString settingsText() const override;

 protected:
  QString startText() const override;
  QString finishedText(std::size_t created) const override;
  Result<std::size_t> commit(const std::vector<gp_Pnt>& points, bool closed) override;
  TopoDS_Shape previewShape(const std::vector<gp_Pnt>& points) const override;
  bool closesOnFinish() const override { return true; }

 private:
  double thickness_ = 200.0;
};

// Openings (O): hover a vertical face of a wall to see the window / door
// there (width x height, sill above the face's bottom; snaps to the
// face's middle and ends, and next to / in line with / between openings
// already there), click to cut it through. Typing "w,h" or "w,h,sill"
// sets the size, and right after a click redoes that opening at the new
// size. Same as place_openings with 'at'.
class OpeningTool : public Tool {
 public:
  struct Settings {
    double width = 900.0;
    double height = 2100.0;
    double sill = 0.0;
  };
  using Tool::Tool;
  QString constraintText() const override;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;
  void setSettings(const Settings& s) { settings_ = s; }
  const Settings& settings() const { return settings_; }
  QString settingsText() const;

 private:
  struct Target {
    doc::ObjectId host;
    gp_Dir normal;
    gp_Pnt at;
    QString snap;  // what it snapped to, if anything
    double fromStart = 0.0, toEnd = 0.0;  // clear distance to the face's ends
    bool overlaps = false;                // with an opening already there
  };
  std::optional<Target> targetAt(const ToolEvent& e);
  bool place(const Target& t);

  Settings settings_;
  std::optional<Target> hover_;
  std::optional<Target> last_;  // as clicked (the host before the cut)
};

// Clicks control-through points of a NURBS curve; Enter/double-click
// finishes, clicking the first point closes it (periodic curve).
class CurveTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  bool stepBack() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  bool mouseDoubleClick(const ToolEvent& e) override;
  bool keyPress(QKeyEvent* event) override;
  void applyMeasurement(const QString& text) override;

 private:
  void finish(bool closed);
  void updatePreview();
  std::vector<gp_Pnt> points_;
  std::optional<gp_Pnt> current_;
};

// 2-point arc: start, end, then the bulge (snaps to a semicircle). Makes a
// segment face that can be pushed/pulled; Ctrl toggles "arc only" (curve).
class ArcTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  bool keyPress(QKeyEvent* event) override;
  void applyMeasurement(const QString& text) override;
  bool makesFace() const { return face_; }
  bool stepBack() override;

 protected:
  std::optional<gp_Pln> workingPlane() const override;

 private:
  void commit(double bulge);
  void reset();
  void updateStatus(const QString& snap = {});
  enum class Stage { Start, End, Bulge } stage_ = Stage::Start;
  geom::SketchPlane plane_;
  gp_Pnt start_, end_;
  double bulge_ = 0.0;
  bool semicircle_ = false;
  bool face_ = true;
  std::optional<doc::ObjectId> lastCreated_;
};

// Linear dimension: two points, then drag the dimension line out (or type
// the offset). Typing right after replaces the text ("" = measured value).
class DimensionTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;
  bool stepBack() override;

 private:
  void commit();
  void reset();
  gp_Pln offsetPlane() const;
  enum class Stage { Start, End, Offset } stage_ = Stage::Start;
  gp_Pnt start_, end_;
  gp_Vec offset_;
  std::optional<doc::ObjectId> lastCreated_;
};

// Text: click a point or face (faces get their area as default text), then
// where the text goes (same spot = no leader). Type to replace the text.
class TextTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;

 private:
  void reset();
  bool hasTarget_ = false;
  QPoint targetPos_;
  gp_Pnt target_;
  QString defaultText_;
  std::optional<gp_Pln> plane_;
  std::optional<doc::ObjectId> lastCreated_;
};

// Section plane: click a face, then move inwards (or type the depth). The
// model is shown cut there, the side the face looks at removed.
class SectionTool : public Tool {
 public:
  using Tool::Tool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;

 private:
  void commit(double depth);
  void preview(double depth);
  bool placing_ = false;
  gp_Pnt origin_;
  gp_Dir normal_;
  double depth_ = 0.0;
};

// Circle: centre (on a face or the ground), then radius by mouse or value.
class CircleTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;

 protected:
  std::optional<gp_Pln> workingPlane() const override;

 private:
  void commit(double radius);
  void reset();
  bool hasCentre_ = false;
  geom::SketchPlane plane_;
  double radius_ = 0.0;
  std::optional<doc::ObjectId> lastCreated_;
};

// Regular polygon: click the centre, then a corner (the polygon turns with
// the mouse). Typing "8s" sets the number of sides, a length the radius;
// both re-edit the last polygon when nothing is being drawn.
class PolygonTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;
  int sides() const { return sides_; }

 protected:
  std::optional<gp_Pln> workingPlane() const override;

 private:
  void commit(double radius);
  void reset();
  QString prompt() const;
  bool hasCentre_ = false;
  geom::SketchPlane plane_;  // xDir points at the first corner
  double radius_ = 0.0;
  int sides_ = 6;
  std::optional<doc::ObjectId> lastCreated_;
};

class RectangleTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  bool keyPress(QKeyEvent* event) override;
  void applyMeasurement(const QString& text) override;

 protected:
  std::optional<gp_Pln> workingPlane() const override;

 private:
  void commit(double u, double v);
  void reset();
  // The plane the rectangle is drawn on for this cursor point: the locked
  // one, else the upright one through the corner and a point of the model
  // off the start plane (SketchUp-style vertical rectangles), else the
  // start plane.
  geom::SketchPlane planeFor(const ToolEvent& e);

  bool hasCorner_ = false;
  geom::SketchPlane start_;  // from the first click (face or ground)
  geom::SketchPlane plane_;  // the one drawn on now
  std::optional<gp_Dir> planeLock_;  // arrow keys: normal of the plane
  double u_ = 0.0;
  double v_ = 0.0;
  std::optional<doc::ObjectId> lastCreated_;
};

// Eraser: click deletes what is clicked (a group/component as a whole, or
// a single object inside the group being edited).
// SketchUp's eraser works on lines: a loose line loses the piece between
// its neighbouring crossings, a flat face's outline edge takes the face
// (its other edges stay as lines), a line dividing a flat face merges the
// two, and a corner edge of a solid takes the two faces meeting there (the
// shape stays open). Shift+click hides the line; clicking a face erases
// the object.
class EraserTool : public Tool {
 public:
  using Tool::Tool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void mouseRelease(const ToolEvent& e) override;

 private:
  enum class EdgeAction { None, Piece, FaceOutline, Merge, Corner };
  // What clicking this edge would do.
  EdgeAction actionFor(const ElementRef& picked, const TopoDS_Edge& edge) const;
  // A click: the line (or object) under the cursor.
  void clickAt(const ToolEvent& e);
  // Erases (or hides) the line through `point` on `object`; "" or the reason.
  std::string eraseLine(const doc::ObjectId& object, const TopoDS_Edge& edge, const gp_Pnt& point, bool hide);
  // Dragging (SketchUp): lines crossed are marked, erased together on release.
  void markAt(const QPoint& pos);
  struct Mark {
    doc::ObjectId object;
    int edgeIndex = 0;  // as picked (for the highlight)
    gp_Pnt point;       // on the line, to find it again after earlier erasures
  };
  std::vector<Mark> marked_;
  std::optional<ToolEvent> press_;
  QPoint last_;
  bool dragging_ = false;
};

// Follow Me: click the face to sweep (a profile), then the path: a line /
// arc / curve, or a face of a solid to run around its outline.
class FollowMeTool : public Tool {
 public:
  using Tool::Tool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;

 private:
  std::optional<doc::ObjectId> profile_;
  // SketchUp order: the path's edges selected before the tool (lines,
  // arcs, edges of faces / solids, possibly of several objects); then one
  // click on the profile runs it.
  std::vector<TopoDS_Edge> path_;
};

// Paint bucket: click to apply the current colour (groups and components
// as a whole), Alt+click to pick up the colour of what is clicked.
// Paint (B), with the material chosen in the materials panel: click a face
// to paint that face; a group or component at the current editing level is
// painted as a whole; Ctrl+click paints the whole object. Alt+click picks
// up the material shown there. With "inherit" (no material) the click takes
// the face's / object's own material away.
class PaintTool : public Tool {
 public:
  using Tool::Tool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void setMaterial(const std::string& id) {
    material_ = id;
    chosen_ = true;
  }
  const std::string& material() const { return material_; }
  // False until a material (or "none") was picked: clicking then asks for one
  // instead of silently painting "none".
  bool hasMaterial() const { return chosen_; }

 private:
  QString materialName() const;
  std::string material_;
  bool chosen_ = false;
};

// Tape measure: click two points (with inference) to read the distance.
// Does not change the model.
class MeasureTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;
  std::optional<double> lastDistance() const { return lastDistance_; }

 private:
  void show(const gp_Pnt& a, const gp_Pnt& b, bool final);
  // Guide mode (started on a straight edge): perpendicular offset from the
  // edge's line to `p`, and its preview.
  gp_Vec guideOffset(const gp_Pnt& p) const;
  void showGuide(const gp_Pnt& p);
  void placeGuide(const gp_Vec& offset);
  std::optional<gp_Pnt> start_;
  std::optional<gp_Dir> guideDir_;  // set while pulling a guide off an edge
  gp_Vec guideOffset_;
  std::optional<double> lastDistance_;
};

// The selected component's origin and red axis (SketchUp's Change Axes):
// click the new origin, then a point the red axis should point toward
// (turning about the blue axis). Enter after the origin keeps the
// directions. Nothing moves; every placement of the component follows.
class ComponentAxesTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  bool stepBack() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  bool keyPress(QKeyEvent* event) override;

 protected:
  std::optional<gp_Pln> workingPlane() const override;
  QString targetText() const override;

 private:
  void finish(const std::optional<gp_Dir>& red);
  doc::ObjectId instance_;
  std::optional<gp_Pnt> origin_;
};

// Uniform scale of the selection: click the fixed point, a reference point,
// then where the reference should go (or type a factor like "1.5", or a
// length like "3000mm" for the new reference distance). Typing again right
// after re-edits the last scale.
class ScaleTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void deactivate() override;
  bool stepBack() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  bool keyPress(QKeyEvent* event) override;
  void applyMeasurement(const QString& text) override;

  // --- grips (SketchUp): a box around the selection, grips at its corners
  // (uniform), edge middles (two directions) and face centres (one) --------
  // The box frame: the object's own axes for one object / group, else the
  // world axes. Exposed for tests.
  struct Grip {
    int s[3] = {0, 0, 0};  // -1 / 0 / +1 along the frame's x, y, z
  };
  bool hasGrips() const { return box_.has_value(); }
  std::optional<gp_Pnt> gripPoint(int sx, int sy, int sz) const;
  gp_Ax3 gripFrame() const { return box_ ? box_->frame : gp_Ax3(); }

 private:
  enum class Stage { Fixed, Reference, Scaling, Grip };
  struct Box {
    gp_Ax3 frame;
    gp_XYZ lo, hi;  // in the frame
  };
  void buildBox();
  void showBox(const std::optional<Grip>& hot);
  std::optional<Grip> gripAt(const QPoint& pos) const;
  gp_Pnt local(const gp_XYZ& p) const;  // frame coordinates -> world
  gp_Pnt gripWorld(const Grip& g) const;
  bool gripUniform() const;
  void updateGripDrag(const QPoint& pos);
  bool commitGrip(const std::array<double, 3>& factors);
  QString axisName(int axis) const;
  std::optional<Box> box_;
  std::optional<Grip> hot_;
  Grip active_;
  bool flipUniform_ = false;  // Shift
  bool aboutCentre_ = false;  // Ctrl
  gp_Pnt anchor_;
  std::array<double, 3> factors_{1, 1, 1};
  struct LastGrip {
    std::vector<doc::ObjectId> targets;
    Box box;
    Grip grip;
    gp_Pnt anchor;
    bool uniform = false;
  };
  std::optional<LastGrip> lastGrip_;
  std::optional<double> factorAt(const gp_Pnt& p) const;
  bool commit(double factor);

  Stage stage_ = Stage::Fixed;
  std::vector<doc::ObjectId> targets_;
  gp_Pnt fixed_;
  gp_Pnt reference_;
  double factor_ = 1.0;
  struct Last {
    std::vector<doc::ObjectId> targets;
    gp_Pnt fixed;
    double referenceDistance = 0.0;
  };
  std::optional<Last> last_;
};

// Protractor: click the centre (on a face: in its plane, else horizontal),
// a base direction, then the angle (15 degree steps, or typed) to place a
// guide line through the centre.
class ProtractorTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  bool stepBack() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;

 private:
  enum class Stage { Center, Base, Angle };
  std::optional<double> angleAt(const gp_Pnt& p) const;
  gp_Dir guideDirection(double angle) const;
  void place(double angle);

  Stage stage_ = Stage::Center;
  gp_Pnt center_;
  gp_Dir normal_{0, 0, 1};
  gp_Dir base_{1, 0, 0};
  double angle_ = 0.0;
};

// Paste: the copied objects follow the cursor (their lowest corner snaps to
// the inferred point); a click places them and hands back to selection.
class PasteTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  QString name() const override;
  QCursor cursor() const override;
  void setClipboard(const doc::Modeler::Clipboard* clipboard) { clipboard_ = clipboard; }
  // Called with the new ids after placing (the host switches tools).
  void setOnPlaced(std::function<void(const std::vector<doc::ObjectId>&)> f) { onPlaced_ = std::move(f); }
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;

 private:
  const doc::Modeler::Clipboard* clipboard_ = nullptr;
  std::function<void(const std::vector<doc::ObjectId>&)> onPlaced_;
};

// Revolve: click a profile face, then two points of the axis. Creates a
// full revolution; typing an angle re-edits it (parametric).
class RevolveTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  bool stepBack() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;

 private:
  enum class Stage { Profile, AxisStart, AxisEnd };
  void reset();
  Stage stage_ = Stage::Profile;
  doc::ObjectId profileId_;
  gp_Pnt axisStart_;
  std::optional<std::pair<doc::ObjectId, std::string>> last_;
};

class PushPullTool : public Tool {
 public:
  using Tool::Tool;
  QString constraintText() const override;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;
  // Double-clicking a face repeats the previous push/pull distance.
  bool mouseDoubleClick(const ToolEvent& e) override;

  bool isDragging() const { return dragging_; }

 private:
  void commit(double distance);
  void reset();

  bool dragging_ = false;
  bool curved_ = false;
  doc::ObjectId objectId_;
  TopoDS_Face face_;
  gp_Pnt origin_;
  gp_Dir normal_;
  double distance_ = 0.0;
  std::optional<double> lastDistance_;

  struct LastPush {
    doc::ObjectId id;
    std::string key;
    double base = 0.0;
    double sign = 1.0;
  };
  std::optional<LastPush> last_;
};

// "x3" / "3x" / "*3" (array) or "/3" / "3/" (divide) typed after a copy.
struct ArrayRequest {
  bool divide = false;
  int count = 1;
};
std::optional<ArrayRequest> parseArrayRequest(const QString& text);

// Shared state of Move and Rotate: targets, Ctrl copy mode and re-editing of
// the last operation through the measurement box.
class TransformTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  bool keyPress(QKeyEvent* event) override;

 protected:
  // Applies `delta` (count copies when copying) and remembers it.
  bool commit(const gp_Trsf& delta, int count = 1);
  // Re-runs the last operation with a different transform / count.
  bool redoLast(const gp_Trsf& delta, int count);
  bool canReeditLast();
  virtual void resetState() = 0;
  QString copySuffix() const;

  // Called when Ctrl switches copying on or off.
  virtual void copyToggled() {}
  // The selection at the start: whole objects (targets_), and faces /
  // edges picked on their own (elementTargets_; edgeTargets_ when only
  // edges). A copy copies just those elements, never their whole owner.
  void captureTargets();
  bool partialSelection() const { return !elementTargets_.empty(); }

  std::vector<doc::ObjectId> targets_;
  // Edges selected on their own (one edge of a line, a face or a solid):
  // a copy copies just them, as lines (Modeler::copyEdges).
  std::vector<TopoDS_Edge> edgeTargets_;
  std::vector<doc::DeleteTarget> elementTargets_;
  bool copy_ = false;
  struct Last {
    std::vector<doc::ObjectId> targets;
    std::vector<TopoDS_Edge> edges;
    std::vector<doc::DeleteTarget> elements;
    gp_Trsf delta;
    bool copy = false;
    int count = 1;
  };
  std::optional<Last> last_;
};

class MoveTool : public TransformTool {
 public:
  using TransformTool::TransformTool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;

 protected:
  QString targetText() const override;

 private:
  void resetState() override;
  void copyToggled() override;
  void finish();
  bool moving_ = false;
  gp_Pnt base_;
  gp_Pnt current_;
  // Moving a single selected face stretches the solid along its normal.
  std::optional<std::pair<doc::ObjectId, TopoDS_Face>> stretch_;
  gp_Dir stretchNormal_;
  // Moving corners (selected edges, or Alt+click on a corner / edge /
  // face): the shape is reshaped with its faces kept flat.
  bool grabCorners(const ToolEvent& e);
  struct Corners {
    doc::ObjectId object;
    std::vector<gp_Pnt> points;  // world
    QString what;                // "角" / "辺" / "面" / "辺 3 本"
  };
  std::optional<Corners> corners_;
  // The copy's first placement as the user gave it: arrays ("*5") repeat
  // it, divisions ("/3") divide it, a typed distance replaces it; never
  // the result of an earlier array (ARR-02).
  gp_Vec reference_;
  // A typed negative distance went against the direction shown with the
  // mouse: a value typed again is read against that same direction.
  double referenceSign_ = 1.0;
  double typedSign_ = 1.0;
  bool divided_ = false;
  void previewEdgeCopy(const gp_Vec& d);
};

class RotateTool : public TransformTool {
 public:
  using TransformTool::TransformTool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  bool stepBack() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;

 protected:
  std::optional<gp_Pln> workingPlane() const override;
  QString targetText() const override;

 private:
  enum class Stage { Center, Reference, Rotating };
  void resetState() override;
  gp_Trsf rotation(double angle) const;
  std::optional<double> angleAt(const gp_Pnt& p) const;

  Stage stage_ = Stage::Center;
  gp_Pnt center_;
  gp_Dir normal_{0, 0, 1};
  gp_Dir reference_{1, 0, 0};
  double angle_ = 0.0;
  gp_Ax1 lastAxis_;
  double lastAngle_ = 0.0;
  // The angle the user gave (the rotation, or typed after it): arrays
  // count from it ("*5" five times it, "/5" it divided in five), never
  // from an earlier array's result (as Move's distance, ARR-02).
  double referenceAngle_ = 0.0;
  bool divided_ = false;
};

class OffsetTool : public Tool {
 public:
  using Tool::Tool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;

 private:
  void commit(double distance);
  void reset();

  bool dragging_ = false;
  doc::ObjectId objectId_;
  TopoDS_Face face_;
  gp_Pln plane_;
  double distance_ = 0.0;
  struct LastOffset {
    doc::ObjectId id;
    std::string key;
    doc::ObjectId source;  // a loose face: offset again from it (the hole follows)
    bool inwards = false;
  };
  std::optional<LastOffset> last_;
};

}  // namespace sbr::app
