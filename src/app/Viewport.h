#pragma once
// 3D viewport: OCCT V3d view embedded in a native Qt widget.
//
// The viewport only ever displays *copies* of canonical shapes (display
// layer). Picking results are mapped back to canonical faces by index.

#include <cstdint>
#include <functional>
#include <map>
#include <set>

#include <gp_Lin.hxx>
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

  void setTool(Tool* tool) {
    tool_ = tool;
    typing_ = false;
  }

  // --- document display ---------------------------------------------------
  void scheduleSync();
  void syncNow();
  void setSelection(const std::vector<doc::ObjectId>& ids);
  void setDisplayQuality(const display::DisplayQuality& quality);
  const display::DisplayQuality& displayQuality() const { return quality_; }
  void setShaded(bool shaded);
  // X-ray: solids drawn see-through, and hidden edges and points can be
  // snapped to.
  void setXRay(bool on);
  bool xRay() const { return xRay_; }
  // Shared display copies in use (one per distinct local shape; tests).
  std::size_t sharedDisplayCount() const { return protos_.size(); }
  // Edges drawn without their line for an object (tests / status).
  std::size_t hiddenEdgeCount(const doc::ObjectId& id) const {
    const auto it = entries_.find(id);
    return it == entries_.end() || it->second.edgeOverlay.IsNull() ? 0 : it->second.hiddenEdges.size();
  }
  // Colour the viewport shows on one face (RGB 0..1; tests, eyedropper).
  std::optional<std::array<double, 3>> shownFaceColor(const doc::ObjectId& id, const TopoDS_Face& worldFace) const;

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
  // Schedules a repaint (coalesced); renderNow() draws immediately.
  void redraw();
  void renderNow();

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
  void setProposalGhost(const TopoDS_Shape& shape, const TopoDS_Shape& removed = TopoDS_Shape());
  bool hasProposalGhost() const { return !proposalGhost_.IsNull(); }
  int openEndCount() const { return openEndCount_; }
  // Writes the current view to an image file (PNG etc.).
  bool dumpImage(const QString& path);
  // Runs a tool callback; an exception resets the tool (toolFailed) instead
  // of ending the program. Returns false then.
  bool toolCall(const std::function<void()>& call);

 signals:
  void measurementChar(QChar c);
  void measurementBackspace();
  void measurementCommit();
  void escapePressed();
  void focusRequested(const QPoint& pos);
  // A click / key went to the tool (its state may have changed).
  void toolInput();
  // The tool hit a geometry error and was reset (message from the kernel).
  void toolFailed(const QString& message);

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
  // Keeps letters typed as units ("3m", "8s") from triggering tool shortcuts.
  bool event(QEvent* event) override;
  void keyReleaseEvent(QKeyEvent* event) override;

 private:
  // Display copies shared by every object with the same local canonical
  // shape (component placements): copied and tessellated once, each entry
  // only adds its placement (performance step 2, GPU instancing later).
  struct ProtoKey {
    const void* tshape = nullptr;
    int orientation = 0;
    bool operator<(const ProtoKey& o) const {
      return tshape != o.tshape ? tshape < o.tshape : orientation < o.orientation;
    }
  };
  struct Proto {
    TopoDS_Shape display;  // local coordinates
    int users = 0;
  };
  struct Entry {
    Handle(AIS_Shape) ais;
    std::optional<ProtoKey> proto;
    doc::Color color;
    // Faces painted on their own as shown (index in displayFaces -> RGB),
    // and whether the object is shown selected (no face colours then).
    std::vector<std::pair<int, std::array<double, 3>>> faceColors;
    bool shownSelected = false;
    bool locked = false;  // not pickable
    TopoDS_Shape canonicalWorld;
    TopoDS_Shape displayShape;
    TopTools_IndexedMapOfShape displayFaces;
    TopTools_IndexedMapOfShape canonicalFaces;
    TopTools_IndexedMapOfShape displayEdges;
    TopTools_IndexedMapOfShape canonicalEdges;
    // Objects with hidden edges draw no face boundaries; their other edges
    // are this line overlay instead (not pickable).
    std::vector<int> hiddenEdges;
    Handle(AIS_Shape) edgeOverlay;
  };

  void initViewer();
  void displayAxes();
  void applyPickMode(const Handle(AIS_Shape)& ais, bool locked = false);
  // A picture laid on its rectangle (texture over the face, unlit).
  void showImage(const Handle(AIS_Shape)& ais, const doc::ImageUnderlay& underlay);
  // Brings one object's display entry and snap index up to date (created,
  // replaced, recoloured or removed). Returns true if a guide changed.
  bool syncObject(const doc::ObjectId& id);
  void syncLabels();
  void syncSection();
  void syncOpenEnds();
  Handle(AIS_Shape) makeDisplay(const doc::Object& object, Entry& entry);
  void removeDisplay(Entry& entry);
  QPoint devicePos(const QPoint& logical) const;
  void orbit(const QPoint& delta);
  std::optional<doc::ObjectId> idOf(const Handle(AIS_InteractiveObject)& object) const;

  doc::Modeler& modeler_;
  Tool* tool_ = nullptr;

  Handle(V3d_Viewer) viewer_;
  Handle(V3d_View) view_;
  Handle(AIS_InteractiveContext) context_;

  std::map<doc::ObjectId, Entry> entries_;
  std::map<ProtoKey, Proto> protos_;
  // Colours faces show on their own: own face materials, or for a component
  // placement those coming from inside its definition (computed once per
  // definition and document state).
  std::vector<std::pair<int, std::array<double, 3>>> faceColorsOf(const doc::Object& object);
  struct DefinitionColors {
    const void* shape = nullptr;
    std::uint64_t sequence = 0;
    std::vector<std::pair<int, std::string>> faces;  // compound face index -> material ("" = placement's)
  };
  std::map<std::string, DefinitionColors> definitionColors_;
  void releaseProto(const Entry& entry);
  std::map<doc::ObjectId, Handle(AIS_TextLabel)> labels_;  // annotation texts
  Handle(Graphic3d_ClipPlane) section_;
  bool showOpenEnds_ = false;
  int openEndCount_ = 0;
  Handle(AIS_Shape) openEnds_;
  Handle(AIS_RubberBand) rubberBand_;
  Handle(AIS_Shape) proposalGhost_;
  Handle(AIS_Shape) proposalRemoved_;
  std::array<double, 6> sectionKey_{};  // origin + normal of section_
  std::vector<doc::ObjectId> selection_;
  snap::InferenceEngine inference_;
  std::map<doc::ObjectId, gp_Lin> guideLines_;
  // Incremental sync: the document sequence last synchronised, and objects
  // whose appearance (selection, context) must be refreshed regardless.
  std::uint64_t syncedSequence_ = 0;
  bool fullSync_ = true;
  std::set<doc::ObjectId> appearanceDirty_;
  display::DisplayQuality quality_ = display::DisplayQuality::normal();
  bool shaded_ = true;
  bool xRay_ = false;
  bool typing_ = false;  // a value is being typed into the measurement box
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
