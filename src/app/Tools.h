#pragma once
// Interactive modelling tools (SketchUp-style direct manipulation).

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
  virtual std::vector<ElementRef> elementSelection() const = 0;
  virtual std::vector<doc::ObjectId> selection() const = 0;
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

 protected:
  ToolHost& host_;
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

 protected:
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

 private:
  std::optional<gp_Pnt> pointAt(const ToolEvent& e);
  void addPoint(const gp_Pnt& p);
  void finish();
  void updatePreview();

  std::vector<gp_Pnt> points_;
  std::optional<gp_Pnt> current_;
  std::optional<gp_Pln> drawPlane_;  // plane of the first point (face or horizontal)
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

 private:
  void commit(double radius);
  void reset();
  bool hasCentre_ = false;
  geom::SketchPlane plane_;
  double radius_ = 0.0;
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
  void applyMeasurement(const QString& text) override;

 private:
  void commit(double u, double v);
  void reset();

  bool hasCorner_ = false;
  geom::SketchPlane plane_;
  double u_ = 0.0;
  double v_ = 0.0;
  std::optional<doc::ObjectId> lastCreated_;
};

// Eraser: click deletes what is clicked (a group/component as a whole, or
// a single object inside the group being edited).
class EraserTool : public Tool {
 public:
  using Tool::Tool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
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
};

// Paint bucket: click to apply the current colour (groups and components
// as a whole), Alt+click to pick up the colour of what is clicked.
class PaintTool : public Tool {
 public:
  using Tool::Tool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void setColor(const doc::Color& color) { color_ = color; }
  const doc::Color& color() const { return color_; }

 private:
  doc::Color color_{0.75, 0.55, 0.35};  // warm wood tone by default
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

// Revolve: click a profile face, then two points of the axis. Creates a
// full revolution; typing an angle re-edits it (parametric).
class RevolveTool : public DrawingTool {
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
  void commit(const gp_Trsf& delta, int count = 1);
  // Re-runs the last operation with a different transform / count.
  bool redoLast(const gp_Trsf& delta, int count);
  bool canReeditLast() const;
  virtual void resetState() = 0;
  QString copySuffix() const;

  std::vector<doc::ObjectId> targets_;
  bool copy_ = false;
  struct Last {
    std::vector<doc::ObjectId> targets;
    gp_Trsf delta;
    bool copy = false;
    int count = 1;
    std::size_t undoCount = 0;
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

 private:
  void resetState() override;
  void finish();
  bool moving_ = false;
  gp_Pnt base_;
  gp_Pnt current_;
  // Moving a single selected face stretches the solid along its normal.
  std::optional<std::pair<doc::ObjectId, TopoDS_Face>> stretch_;
  gp_Dir stretchNormal_;
};

class RotateTool : public TransformTool {
 public:
  using TransformTool::TransformTool;
  QString name() const override;
  QCursor cursor() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;

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
  };
  std::optional<LastOffset> last_;
};

}  // namespace sbr::app
