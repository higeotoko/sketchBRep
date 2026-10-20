#pragma once
// Interactive modelling tools (SketchUp-style direct manipulation).

#include <optional>
#include <vector>

#include <QPoint>
#include <QString>

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
  void refresh();
  QString lockSuffix() const;

  std::optional<gp_Dir> locked_;
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

}  // namespace sbr::app
