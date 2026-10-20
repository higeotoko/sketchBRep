#pragma once
// 3D viewport: OCCT V3d view embedded in a native Qt widget.
//
// The viewport only ever displays *copies* of canonical shapes (display
// layer). Picking results are mapped back to canonical faces by index.

#include <map>
#include <optional>
#include <vector>

#include <QPoint>
#include <QTimer>
#include <QWidget>

#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

#include "sbr/display/DisplayMesh.h"
#include "sbr/doc/Modeler.h"
#include "sbr/snap/Inference.h"

namespace sbr::app {

class Tool;

struct FacePick {
  doc::ObjectId objectId;
  TopoDS_Face face;  // canonical face in world coordinates
  gp_Pnt point;      // hit point
};

enum class PickMode { None, Objects, Faces };

class Viewport : public QWidget {
  Q_OBJECT
 public:
  explicit Viewport(doc::Modeler& modeler, QWidget* parent = nullptr);
  ~Viewport() override;

  QPaintEngine* paintEngine() const override { return nullptr; }

  void setTool(Tool* tool) { tool_ = tool; }

  // --- document display ---------------------------------------------------
  void scheduleSync();
  void syncNow();
  void setSelection(const std::vector<doc::ObjectId>& ids);
  void setDisplayQuality(const display::DisplayQuality& quality);
  const display::DisplayQuality& displayQuality() const { return quality_; }
  void setShaded(bool shaded);

  // --- picking / inference (positions in widget logical pixels) -----------
  snap::Ray rayAt(const QPoint& pos) const;
  snap::ScreenPoint project(const gp_Pnt& p) const;  // logical pixels
  QPoint toWidget(const gp_Pnt& p) const;
  snap::SnapResult snap(const QPoint& pos, std::optional<gp_Pnt> anchor,
                        std::optional<gp_Dir> lockedDirection,
                        std::optional<gp_Pln> workPlane) const;
  void setPickMode(PickMode mode);
  std::optional<FacePick> pickFace(const QPoint& pos);
  std::optional<doc::ObjectId> pickObject(const QPoint& pos);
  void clearHover();

  // --- transient feedback ---------------------------------------------------
  void setPreview(const TopoDS_Shape& shape, bool solid = false);
  void clearPreview();
  void showSnap(const snap::SnapResult& result, std::optional<gp_Pnt> anchor);
  void hideSnap();
  void redraw();

  // --- camera -------------------------------------------------------------
  enum class StandardView { Iso, Top, Front, Right, Back, Left };
  void setStandardView(StandardView view);
  void setPerspective(bool perspective);
  bool isPerspective() const;
  void fitAll();
  void fitObjects(const std::vector<doc::ObjectId>& ids);
  void storeCamera(doc::CameraState& state) const;
  void restoreCamera(const doc::CameraState& state);

  bool isInitialized() const { return !view_.IsNull(); }
  // Writes the current view to an image file (PNG etc.).
  bool dumpImage(const QString& path);

 signals:
  void measurementChar(QChar c);
  void measurementBackspace();
  void measurementCommit();
  void escapePressed();
  void focusRequested(const QPoint& pos);

 protected:
  void showEvent(QShowEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void keyReleaseEvent(QKeyEvent* event) override;

 private:
  struct Entry {
    Handle(AIS_Shape) ais;
    TopoDS_Shape canonicalWorld;
    TopoDS_Shape displayShape;
    TopTools_IndexedMapOfShape displayFaces;
    TopTools_IndexedMapOfShape canonicalFaces;
  };

  void initViewer();
  void displayAxes();
  void applyPickMode(const Handle(AIS_Shape)& ais);
  Handle(AIS_Shape) makeDisplay(const doc::Object& object, Entry& entry);
  QPoint devicePos(const QPoint& logical) const;
  void orbit(const QPoint& delta);
  std::optional<doc::ObjectId> idOf(const Handle(AIS_InteractiveObject)& object) const;

  doc::Modeler& modeler_;
  Tool* tool_ = nullptr;

  Handle(V3d_Viewer) viewer_;
  Handle(V3d_View) view_;
  Handle(AIS_InteractiveContext) context_;

  std::map<doc::ObjectId, Entry> entries_;
  std::vector<doc::ObjectId> selection_;
  snap::InferenceEngine inference_;
  display::DisplayQuality quality_ = display::DisplayQuality::normal();
  bool shaded_ = true;
  PickMode pickMode_ = PickMode::None;

  Handle(AIS_Shape) preview_;
  Handle(AIS_Shape) snapMarker_;
  Handle(AIS_Shape) inferenceLine_;

  QTimer syncTimer_;
  enum class Drag { None, Orbit, Pan } drag_ = Drag::None;
  QPoint lastDragPos_;
};

}  // namespace sbr::app
