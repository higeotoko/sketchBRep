#pragma once
// 3D viewport: OCCT V3d view embedded in a native Qt widget.
//
// The viewport only ever displays *copies* of canonical shapes (display
// layer). Picking results are mapped back to canonical faces by index.

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <unordered_set>

#include <gp_Lin.hxx>
#include <optional>
#include <vector>

#include <QCursor>
#include <QElapsedTimer>
#include <QFile>
#include <memory>
#include <QPoint>
#include <QTimer>
#include <QWidget>
#include <QLabel>
#include <gp_Ax3.hxx>

#include <AIS_InteractiveContext.hxx>
#include <AIS_InteractiveObject.hxx>
#include <Graphic3d_Texture2D.hxx>
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
  // Brings the display up to date a slice at a time (the event loop runs
  // between slices, so thousands of placements rebuilt after a component
  // edit do not freeze the window); syncNow() does it all at once.
  void scheduleSync();
  void syncNow();
  bool syncPending() const { return !pendingSync_.empty(); }
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
  // Whether an object's faces show a material picture (tests).
  bool showsTexture(const doc::ObjectId& id) const {
    const auto it = entries_.find(id);
    return it != entries_.end() && !it->second.textureOverlay.IsNull();
  }
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
  QSize viewSize() const;  // device pixels
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
  // Face or edge under the cursor whatever the tool's pick mode (the
  // right-click menu); the mode is left as it was.
  std::optional<ElementRef> elementAt(const QPoint& pos);
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
  // What the tool preview shows now (null: nothing) - for checks.
  TopoDS_Shape previewShape() const { return preview_.IsNull() ? TopoDS_Shape() : preview_->Shape(); }
  // The marker, guide lines and a small label next to the cursor naming
  // what the point snapped to (SketchUp's ScreenTip), all from `result`.
  void showSnap(const snap::SnapResult& result, std::optional<gp_Pnt> anchor);
  void hideSnap();
  // Scale grips: small cubes along `frame`, the same size on screen at any
  // zoom, the `hot` one (index, -1 = none) marked; `outline` the box.
  void showGrips(const std::vector<gp_Pnt>& grips, int hot, const gp_Ax3& frame, const TopoDS_Shape& outline);
  void clearGrips() { showGrips({}, -1, gp_Ax3(), TopoDS_Shape()); }
  std::size_t gripCount() const { return grips_.size(); }
  bool hasEditBox() const { return !editBox_.IsNull(); }
  // The label shown (empty when none), where (logical pixels) and the point
  // it names.
  QString snapTip() const { return snapTipText_; }
  QRect snapTipRect() const { return snapTipRect_; }
  gp_Pnt snapTipPoint() const { return snapTipPoint_; }
  // Schedules a repaint (coalesced); renderNow() draws immediately.
  void redraw();
  void renderNow();

  // --- camera -------------------------------------------------------------
  enum class StandardView { Iso, Top, Front, Right, Back, Left };
  void setStandardView(StandardView view);
  void setPerspective(bool perspective);
  bool isPerspective() const;
  void fitAll();

  // --- wheel zoom (WheelZoom.h, docs/17_wheel_zoom.md) ----------------------
  // Zooms by `notches` (positive: in) at the cursor position `pos`
  // (logical pixels), as one wheel event does.
  void zoomWheel(const QPointF& pos, double notches);
  // Distance ratio per notch (> 1) and an explicit direction reversal.
  void setWheelZoom(double ratioPerNotch, bool inverted);
  double wheelRatioPerNotch() const { return wheelRatio_; }
  bool wheelInverted() const { return wheelInverted_; }
  // The point the last wheel step zoomed at, and where it came from
  // ("face", "ground", "depth": the last depth in empty space, "focus").
  std::optional<gp_Pnt> lastWheelAnchor() const;
  QString lastWheelAnchorSource() const { return QString::fromLatin1(wheel_.source); }
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
  // Right button released without dragging: the context menu.
  void contextMenuRequested(const QPoint& pos);

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
  void leaveEvent(QEvent* event) override;

 private:
  QRect placeSnapTip(const QSize& size, const QPoint& marker, std::optional<QPoint> anchor) const;
  void hideSnapTip();
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
    Handle(AIS_Shape) dots;  // selection dots, shared by the placements (made when first needed)
  };
  struct Entry {
    Handle(AIS_Shape) ais;
    std::optional<ProtoKey> proto;
    doc::Color color;
    // Faces painted on their own as shown (index in displayFaces -> RGB),
    // and whether the object is shown selected (no face colours then).
    std::vector<std::pair<int, std::array<double, 4>>> faceColors;  // RGB + opacity
    bool shownSelected = false;
    bool shownLocked = false;
    bool locked = false;  // not pickable
    TopoDS_Shape canonicalWorld;
    // The display (and its face / edge maps, which picks are looked up in)
    // stays where it was built; a moved object only gets this rigid
    // transformation on top: world = shown * display.
    TopLoc_Location builtLocation;  // canonicalWorld's location when the display was built
    gp_Trsf shown;
    TopoDS_Shape displayShape;
    TopTools_IndexedMapOfShape displayFaces;
    TopTools_IndexedMapOfShape canonicalFaces;
    TopTools_IndexedMapOfShape displayEdges;
    TopTools_IndexedMapOfShape canonicalEdges;
    // Objects with hidden edges draw no face boundaries; their other edges
    // are this line overlay instead (not pickable).
    std::vector<int> hiddenEdges;
    Handle(AIS_Shape) edgeOverlay;
    Handle(AIS_InteractiveObject) selectionDots;  // shown while the object is selected
    bool erased = false;  // hidden (object, tag or parent): display kept to show again
    Handle(AIS_InteractiveObject) textureOverlay;  // faces with a picture material
    std::string textureKey;                        // what the overlay shows
  };

  void initViewer();
  void displayAxes();
  void applyPickMode(const Handle(AIS_Shape)& ais, bool locked = false);
  // A picture laid on its rectangle (texture over the face, unlit).
  void showImage(const Handle(AIS_Shape)& ais, const doc::ImageUnderlay& underlay);
  // Brings one object's display entry and snap index up to date (created,
  // replaced, recoloured or removed). Returns true if a guide changed.
  bool syncObject(const doc::ObjectId& id);
  void syncLabel(const doc::ObjectId& id);
  void syncSection();
  void syncOpenEnds();
  // Editing inside a group or component: a dotted box around it (SketchUp).
  void syncEditBox();
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
  std::vector<std::pair<int, std::array<double, 4>>> faceColorsOf(const doc::Object& object);  // RGB + opacity
  // Faces with a material of their own: index -> material id.
  std::vector<std::pair<int, std::string>> faceMaterialIdsOf(const doc::Object& object);
  // Pictures of materials on their faces (rebuilt when what they show changes).
  void updateTextures(const doc::Object& object, Entry& entry);
  Handle(Graphic3d_Texture2D) materialTexture(const doc::Material& material);
  std::map<std::string, Handle(Graphic3d_Texture2D)> textures_;
  struct DefinitionColors {
    const void* shape = nullptr;
    std::uint64_t sequence = 0;
    std::vector<std::pair<int, std::string>> faces;  // compound face index -> material ("" = placement's)
  };
  std::map<std::string, DefinitionColors> definitionColors_;
  void releaseProto(const Entry& entry);
  // Moves an object's display to its new place without rebuilding it (same
  // shape, new rigid location). False when it has to be rebuilt instead.
  bool relocate(const doc::ObjectId& id, Entry& entry, const TopoDS_Shape& world);
  // Puts the display on screen with the selection the tool picks with.
  void displayWithPickMode(Entry& entry);
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
  std::unordered_set<doc::ObjectId> selectedSet_;  // selection_, for lookups
  // Selected itself or inside a selected group.
  bool isSelected(const doc::Object& object) const;
  snap::InferenceEngine inference_;
  std::map<doc::ObjectId, gp_Lin> guideLines_;
  // Incremental sync: the document sequence last synchronised, and objects
  // whose appearance (selection, context) must be refreshed regardless.
  std::uint64_t syncedSequence_ = 0;
  bool fullSync_ = true;
  std::set<doc::ObjectId> appearanceDirty_;
  std::vector<doc::ObjectId> pendingSync_;  // left over by a time-limited slice
  std::unordered_set<doc::ObjectId> pendingSet_;  // the same, to look up
  // Before a pick is used: brings the picked object's display up to date
  // when it is behind the document (a slice not done yet), so the face /
  // edge numbers picked are those of its current shape. True when it was
  // behind (the pick is made again).
  bool freshen(const doc::ObjectId& id);
  void takeInChanges();
  void syncSlice();
  void syncSome(std::optional<double> budgetMs);
  display::DisplayQuality quality_ = display::DisplayQuality::normal();
  bool shaded_ = true;
  bool xRay_ = false;
  bool typing_ = false;  // a value is being typed into the measurement box
  PickMode pickMode_ = PickMode::None;

  Handle(AIS_Shape) preview_;
  Handle(AIS_Shape) snapMarker_;
  Handle(AIS_Shape) inferenceLine_;
  Handle(AIS_Shape) referenceLine_;
  std::vector<Handle(AIS_Shape)> grips_;
  Handle(AIS_Shape) gripOutline_;
  QLabel* snapTip_ = nullptr;  // a tooltip window letting the mouse through
  QString snapTipText_;
  QRect snapTipRect_;
  gp_Pnt snapTipPoint_;
  mutable QPoint snapCursor_;  // the cursor of the last snap() query
  Handle(AIS_Shape) highlightFaces_;
  Handle(AIS_Shape) highlightEdges_;
  std::vector<ElementRef> highlighted_;
  QCursor toolCursor_;
  doc::ObjectId editContext_;
  Handle(AIS_Shape) editBox_;
  std::pair<doc::ObjectId, std::uint64_t> editBoxKey_;  // context and document state shown

  QTimer syncTimer_;
  QSize glSize_;  // native window size the GL view was last resized to
  // Sky above the horizon, ground below (SketchUp): screen-space bands
  // behind everything, rebuilt only when the horizon moves on screen.
  void updateSkyAndGround();
  Handle(AIS_InteractiveObject) backdrop_;
  int backdropWidth_ = 0;
  int horizonRow_ = -1000000;
  int backgroundRows_ = 0;
  bool underGround_ = false;
  enum class Drag { None, Orbit, Pan } drag_ = Drag::None;
  bool leftInCameraDrag_ = false;  // left pressed during a middle-button drag
  QPoint lastDragPos_;
  gp_Pnt orbitPivot_;

  // Wheel zoom: one anchor per gesture (events close in time while the
  // cursor stays), so a run of notches zooms at one point.
  struct WheelGesture {
    bool active = false;
    qint64 lastMs = 0;
    QPointF pos;
    gp_Pnt anchor;
    const char* source = "";
    bool virtualAnchor = false;  // in empty space (no face or ground there)
    gp_Pnt eyeAfter;             // the camera as this zoom left it
    double scaleAfter = 0.0;
    std::optional<gp_Pnt> lastSolid;  // the last anchor on something: depth for empty space
  };
  void acquireWheelAnchor(const QPointF& pos);
  void logWheel(const class QWheelEvent* event, double notches);
  WheelGesture wheel_;
  QElapsedTimer wheelClock_;
  double wheelRatio_;
  bool wheelInverted_ = false;
  std::unique_ptr<QFile> wheelLog_;  // SBR_WHEEL_LOG=<file>: diagnostics, off by default
};

}  // namespace sbr::app
