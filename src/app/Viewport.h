#pragma once
// 3D viewport: OCCT V3d view embedded in a native Qt widget.
//
// The viewport only ever displays *copies* of canonical shapes (display
// layer). Picking results are mapped back to canonical faces by index.

#include <map>
#include <optional>
#include <vector>

#include <QCursor>
#include <QPoint>
#include <QTimer>
#include <QWidget>

#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_RubberBand.hxx>
#include <AIS_TextLabel.hxx>
#include <Graphic3d_ClipPlane.hxx>
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

enum class PickMode { None, Objects, Faces, Elements };

// A face or edge of an object, identified by its index in the object's
// canonical world shape (TopExp::MapShapes order).
struct ElementRef {
  doc::ObjectId objectId;
  TopAbs_ShapeEnum type = TopAbs_FACE;
  int index = 0;
  bool operator==(const ElementRef& o) const {
    return objectId == o.objectId && type == o.type && index == o.index;
  }
};

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
                        std::optional<gp_Pln> workPlane,
                        const std::vector<gp_Pnt>& trackedPoints = {},
                        const std::vector<gp_Dir>& trackedDirections = {},
                        std::optional<gp_Pnt> stickyPoint = std::nullopt) const;
  // Face under the cursor (hit point and outward normal), ignoring point and
  // edge snaps.
  std::optional<std::pair<gp_Pnt, gp_Dir>> faceAt(const QPoint& pos) const;
  void setPickMode(PickMode mode);
  std::optional<FacePick> pickFace(const QPoint& pos);
  // Direction the camera looks in.
  gp_Dir viewDirection() const;
  std::optional<doc::ObjectId> pickObject(const QPoint& pos);
  // Face or edge under the cursor (PickMode::Elements).
  std::optional<ElementRef> pickElement(const QPoint& pos);
  // Face -> face + bounding edges; edge -> edge + adjacent faces.
  std::vector<ElementRef> withNeighbours(const ElementRef& element) const;
  // Every face and edge of the object.
  std::vector<ElementRef> allElements(const doc::ObjectId& id) const;
  void setHighlightedElements(const std::vector<ElementRef>& elements);
  // Canonical world shape of a face/edge (null if no longer valid).
  TopoDS_Shape elementShape(const ElementRef& element) const;
  void setToolCursor(const QCursor& cursor);
  // Group edit mode: everything outside the group is faded.
  void setEditContext(const doc::ObjectId& groupId);
  void clearHover();

  // --- transient feedback ---------------------------------------------------
  void setPreview(const TopoDS_Shape& shape, bool solid = false);
  // Transparent copy of the objects (and their children) moved by `delta`
  // in world space. Built from display copies, never from canonical shapes.
  void setGhost(const std::vector<doc::ObjectId>& ids, const gp_Trsf& delta);
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
  // Text drawn for an annotation (empty if none is shown).
  QString annotationText(const doc::ObjectId& id) const;
  // Marks the open ends of loose lines/arcs (while drawing outlines).
  void setShowOpenEnds(bool show);
  // Selection rectangle overlay (widget coordinates); empty hides it.
  void setRubberBand(const std::optional<QRect>& rect);
  // Translucent ghost of an AI proposal (independent of tool previews).
  void setProposalGhost(const TopoDS_Shape& shape);
  bool hasProposalGhost() const { return !proposalGhost_.IsNull(); }
  int openEndCount() const { return openEndCount_; }
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
    doc::Color color;
    TopoDS_Shape canonicalWorld;
    TopoDS_Shape displayShape;
    TopTools_IndexedMapOfShape displayFaces;
    TopTools_IndexedMapOfShape canonicalFaces;
    TopTools_IndexedMapOfShape displayEdges;
    TopTools_IndexedMapOfShape canonicalEdges;
  };

  void initViewer();
  void displayAxes();
  void applyPickMode(const Handle(AIS_Shape)& ais);
  void syncLabels();
  void syncSection();
  void syncOpenEnds();
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
  std::map<doc::ObjectId, Handle(AIS_TextLabel)> labels_;  // annotation texts
  Handle(Graphic3d_ClipPlane) section_;
  bool showOpenEnds_ = false;
  int openEndCount_ = 0;
  Handle(AIS_Shape) openEnds_;
  Handle(AIS_RubberBand) rubberBand_;
  Handle(AIS_Shape) proposalGhost_;
  std::array<double, 6> sectionKey_{};  // origin + normal of section_
  std::vector<doc::ObjectId> selection_;
  snap::InferenceEngine inference_;
  display::DisplayQuality quality_ = display::DisplayQuality::normal();
  bool shaded_ = true;
  PickMode pickMode_ = PickMode::None;

  Handle(AIS_Shape) preview_;
  Handle(AIS_Shape) snapMarker_;
  Handle(AIS_Shape) inferenceLine_;
  Handle(AIS_Shape) referenceLine_;
  Handle(AIS_Shape) highlightFaces_;
  Handle(AIS_Shape) highlightEdges_;
  std::vector<ElementRef> highlighted_;
  QCursor toolCursor_;
  doc::ObjectId editContext_;

  QTimer syncTimer_;
  QSize glSize_;  // native window size the GL view was last resized to
  enum class Drag { None, Orbit, Pan } drag_ = Drag::None;
  QPoint lastDragPos_;
  gp_Pnt orbitPivot_;
};

}  // namespace sbr::app
