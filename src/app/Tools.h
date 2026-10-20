#pragma once
// Interactive modelling tools (SketchUp-style direct manipulation).

#include <optional>
#include <vector>

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
  virtual void setSelection(const std::vector<doc::ObjectId>& ids) = 0;
  virtual std::vector<doc::ObjectId> selection() const = 0;
};

class Tool {
 public:
  explicit Tool(ToolHost& host) : host_(host) {}
  virtual ~Tool() = default;

  virtual QString name() const = 0;
  virtual void activate() = 0;
  virtual void deactivate() { cancel(); }
  virtual void cancel() {}
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

class SelectTool : public Tool {
 public:
  using Tool::Tool;
  QString name() const override;
  void activate() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
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
};

// Clicks control-through points of a NURBS curve; Enter/double-click
// finishes, clicking the first point closes it (periodic curve).
class CurveTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  QString name() const override;
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

class RectangleTool : public DrawingTool {
 public:
  using DrawingTool::DrawingTool;
  QString name() const override;
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

class PushPullTool : public Tool {
 public:
  using Tool::Tool;
  QString name() const override;
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;

  bool isDragging() const { return dragging_; }

 private:
  void commit(double distance);
  void reset();

  bool dragging_ = false;
  doc::ObjectId objectId_;
  TopoDS_Face face_;
  gp_Pnt origin_;
  gp_Dir normal_;
  double distance_ = 0.0;

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
  void activate() override;
  void cancel() override;
  void mousePress(const ToolEvent& e) override;
  void mouseMove(const ToolEvent& e) override;
  void applyMeasurement(const QString& text) override;

 private:
  void resetState() override;
  bool moving_ = false;
  gp_Pnt base_;
  gp_Pnt current_;
};

class RotateTool : public TransformTool {
 public:
  using TransformTool::TransformTool;
  QString name() const override;
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
