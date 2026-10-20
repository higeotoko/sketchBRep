#include "Viewport.h"

#include <algorithm>
#include <cmath>

#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QWheelEvent>

#include <Aspect_DisplayConnection.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <Graphic3d_Camera.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_PointAspect.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <SelectMgr_ViewerSelector.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <StdSelect_ViewerSelector3d.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>

#ifdef _WIN32
#include <WNT_Window.hxx>
#else
#include <Xw_Window.hxx>
#endif

#include "Tools.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"

namespace sbr::app {
namespace {

Quantity_Color rgb(double r, double g, double b) { return Quantity_Color(r, g, b, Quantity_TOC_RGB); }

const Quantity_Color kAxisColors[3] = {rgb(0.85, 0.1, 0.1), rgb(0.1, 0.65, 0.1), rgb(0.1, 0.2, 0.9)};
const Quantity_Color kSelectedColor = rgb(0.45, 0.62, 0.95);

Quantity_Color snapColor(snap::SnapKind kind) {
  switch (kind) {
    case snap::SnapKind::Endpoint: return rgb(0.0, 0.7, 0.0);
    case snap::SnapKind::Midpoint: return rgb(0.0, 0.75, 0.85);
    case snap::SnapKind::OnEdge: return rgb(0.9, 0.0, 0.0);
    case snap::SnapKind::OnFace: return rgb(0.2, 0.3, 1.0);
    case snap::SnapKind::Origin: return rgb(0.95, 0.65, 0.0);
    case snap::SnapKind::AxisX: return kAxisColors[0];
    case snap::SnapKind::AxisY: return kAxisColors[1];
    case snap::SnapKind::AxisZ: return kAxisColors[2];
    case snap::SnapKind::Locked: return rgb(0.6, 0.1, 0.7);
    default: return rgb(0.3, 0.3, 0.3);
  }
}

bool sameLocatedShape(const TopoDS_Shape& a, const TopoDS_Shape& b) {
  if (a.TShape() != b.TShape() || a.Orientation() != b.Orientation()) return false;
  const gp_Trsf ta = a.Location().Transformation();
  const gp_Trsf tb = b.Location().Transformation();
  for (int r = 1; r <= 3; ++r) {
    for (int c = 1; c <= 4; ++c) {
      if (std::abs(ta.Value(r, c) - tb.Value(r, c)) > 1e-12) return false;
    }
  }
  return true;
}

}  // namespace

Viewport::Viewport(doc::Modeler& modeler, QWidget* parent) : QWidget(parent), modeler_(modeler) {
  setAttribute(Qt::WA_PaintOnScreen);
  setAttribute(Qt::WA_NoSystemBackground);
  setAttribute(Qt::WA_NativeWindow);
  setAttribute(Qt::WA_OpaquePaintEvent);
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setMinimumSize(320, 240);

  syncTimer_.setSingleShot(true);
  syncTimer_.setInterval(0);
  connect(&syncTimer_, &QTimer::timeout, this, &Viewport::syncNow);
}

Viewport::~Viewport() = default;

void Viewport::initViewer() {
  try {
    Handle(Aspect_DisplayConnection) display = new Aspect_DisplayConnection();
    Handle(OpenGl_GraphicDriver) driver = new OpenGl_GraphicDriver(display, Standard_False);
    viewer_ = new V3d_Viewer(driver);
    viewer_->SetDefaultLights();
    viewer_->SetLightOn();
    context_ = new AIS_InteractiveContext(viewer_);

    view_ = viewer_->CreateView();
#ifdef _WIN32
    Handle(WNT_Window) window = new WNT_Window(reinterpret_cast<Aspect_Handle>(winId()));
#else
    Handle(Xw_Window) window = new Xw_Window(display, static_cast<Aspect_Drawable>(winId()));
#endif
    view_->SetWindow(window);
    if (!window->IsMapped()) window->Map();
  } catch (const Standard_Failure& e) {
    view_.Nullify();
    QMessageBox::critical(this, tr("OpenGL エラー"),
                          tr("3Dビューを初期化できませんでした: %1").arg(e.GetMessageString()));
    return;
  }

  view_->SetBgGradientColors(rgb(0.96, 0.97, 0.99), rgb(0.80, 0.84, 0.89), Aspect_GFM_VER);
  view_->TriedronDisplay(Aspect_TOTP_LEFT_LOWER, Quantity_NOC_GRAY20, 0.08, V3d_ZBUFFER);
  view_->ChangeRenderingParams().IsAntialiasingEnabled = Standard_True;
  view_->ChangeRenderingParams().NbMsaaSamples = 4;

  const Handle(Prs3d_Drawer)& drawer = context_->DefaultDrawer();
  drawer->SetFaceBoundaryDraw(Standard_True);
  drawer->SetFaceBoundaryAspect(new Prs3d_LineAspect(Quantity_NOC_BLACK, Aspect_TOL_SOLID, 1.0));
  drawer->SetTypeOfDeflection(Aspect_TOD_ABSOLUTE);
  drawer->SetMaximalChordialDeviation(quality_.linearDeflection);
  drawer->SetDeviationAngle(quality_.angularDeflection);
  context_->SetDisplayMode(AIS_Shaded, Standard_False);
  context_->SetPixelTolerance(static_cast<int>(6 * devicePixelRatioF()));

  const Handle(Prs3d_Drawer)& hover = context_->HighlightStyle(Prs3d_TypeOfHighlight_LocalDynamic);
  hover->SetColor(rgb(1.0, 0.75, 0.1));
  hover->SetTransparency(0.3f);
  context_->HighlightStyle(Prs3d_TypeOfHighlight_Dynamic)->SetColor(rgb(0.25, 0.55, 1.0));

  displayAxes();
  restoreCamera(modeler_.document().settings().camera);
}

void Viewport::displayAxes() {
  const double extent = 1.0e6;
  const gp_Dir dirs[3] = {gp::DX(), gp::DY(), gp::DZ()};
  for (int i = 0; i < 3; ++i) {
    const gp_Pnt a(0, 0, 0);
    const gp_Pnt b = a.Translated(gp_Vec(dirs[i]) * extent);
    const gp_Pnt c = a.Translated(gp_Vec(dirs[i]) * -extent);
    for (int half = 0; half < 2; ++half) {
      Handle(AIS_Shape) axis = new AIS_Shape(BRepBuilderAPI_MakeEdge(a, half == 0 ? b : c).Edge());
      axis->SetColor(kAxisColors[i]);
      axis->Attributes()->SetWireAspect(new Prs3d_LineAspect(
          kAxisColors[i], half == 0 ? Aspect_TOL_SOLID : Aspect_TOL_DOT, 1.5));
      axis->SetInfiniteState(Standard_True);
      context_->Display(axis, AIS_WireFrame, -1, Standard_False);
    }
  }
}

// --- Qt events ----------------------------------------------------------------

void Viewport::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (view_.IsNull()) {
    initViewer();
    if (!view_.IsNull()) view_->MustBeResized();
    syncNow();
  }
}

void Viewport::paintEvent(QPaintEvent*) { redraw(); }

void Viewport::resizeEvent(QResizeEvent*) {
  if (!view_.IsNull()) view_->MustBeResized();
}

QPoint Viewport::devicePos(const QPoint& logical) const {
  const double dpr = devicePixelRatioF();
  return QPoint(static_cast<int>(std::lround(logical.x() * dpr)),
                static_cast<int>(std::lround(logical.y() * dpr)));
}

void Viewport::mousePressEvent(QMouseEvent* event) {
  setFocus();
  if (view_.IsNull()) return;
  if (event->button() == Qt::MiddleButton) {
    drag_ = (event->modifiers() & Qt::ShiftModifier) ? Drag::Pan : Drag::Orbit;
    lastDragPos_ = event->position().toPoint();
    setCursor(drag_ == Drag::Pan ? Qt::SizeAllCursor : Qt::ClosedHandCursor);
    if (drag_ == Drag::Orbit) {
      // Orbit around what is under the cursor (geometry or ground), like
      // SketchUp; fall back to the view centre.
      const snap::SnapResult r = snap(lastDragPos_, std::nullopt, std::nullopt, std::nullopt);
      orbitPivot_ = r.valid() ? r.point : view_->Camera()->Center();
    }
    return;
  }
  if (event->button() == Qt::LeftButton && tool_) {
    tool_->mousePress(ToolEvent{event->position().toPoint(), event->modifiers()});
  }
}

void Viewport::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::MiddleButton) {
    drag_ = Drag::None;
    setCursor(toolCursor_);
    return;
  }
  if (event->button() == Qt::LeftButton && tool_) {
    tool_->mouseRelease(ToolEvent{event->position().toPoint(), event->modifiers()});
  }
}

void Viewport::mouseMoveEvent(QMouseEvent* event) {
  if (view_.IsNull()) return;
  const QPoint pos = event->position().toPoint();
  if (drag_ != Drag::None) {
    const QPoint delta = pos - lastDragPos_;
    lastDragPos_ = pos;
    if (drag_ == Drag::Orbit && !(event->modifiers() & Qt::ShiftModifier)) {
      orbit(delta);
    } else {
      const QPoint d = devicePos(delta);
      view_->Pan(d.x(), -d.y(), 1.0, Standard_False);
    }
    redraw();
    return;
  }
  if (tool_) tool_->mouseMove(ToolEvent{pos, event->modifiers()});
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* event) {
  const QPoint pos = event->position().toPoint();
  if (event->button() == Qt::LeftButton && tool_ &&
      tool_->mouseDoubleClick(ToolEvent{pos, event->modifiers()})) {
    return;
  }
  if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
    emit focusRequested(pos);
  }
}

void Viewport::wheelEvent(QWheelEvent* event) {
  if (view_.IsNull()) return;
  const QPoint p = devicePos(event->position().toPoint());
  const int delta = event->angleDelta().y() / 4;
  if (delta == 0) return;
  view_->StartZoomAtPoint(p.x(), p.y());
  view_->ZoomAtPoint(p.x(), p.y(), p.x() + delta, p.y() + delta);
  redraw();
}

void Viewport::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape) {
    emit escapePressed();
    return;
  }
  if (tool_ && tool_->keyPress(event)) return;
  switch (event->key()) {
    case Qt::Key_Backspace: emit measurementBackspace(); return;
    case Qt::Key_Return:
    case Qt::Key_Enter: emit measurementCommit(); return;
    default: break;
  }
  const QString text = event->text();
  if (text.size() == 1) {
    const QChar c = text.at(0);
    if (c.isDigit() || QStringLiteral(".,;-+ mcMCxX*/").contains(c)) {
      emit measurementChar(c);
      return;
    }
  }
  QWidget::keyPressEvent(event);
}

void Viewport::keyReleaseEvent(QKeyEvent* event) {
  if (tool_ && tool_->keyRelease(event)) return;
  QWidget::keyReleaseEvent(event);
}

// --- document display ---------------------------------------------------------

void Viewport::scheduleSync() { syncTimer_.start(); }

Handle(AIS_Shape) Viewport::makeDisplay(const doc::Object& object, Entry& entry) {
  entry.canonicalWorld = modeler_.document().worldShape(object);
  entry.displayShape = display::makeDisplayCopy(entry.canonicalWorld);
  entry.displayFaces.Clear();
  entry.canonicalFaces.Clear();
  TopExp::MapShapes(entry.displayShape, TopAbs_FACE, entry.displayFaces);
  TopExp::MapShapes(entry.canonicalWorld, TopAbs_FACE, entry.canonicalFaces);
  entry.displayEdges.Clear();
  entry.canonicalEdges.Clear();
  TopExp::MapShapes(entry.displayShape, TopAbs_EDGE, entry.displayEdges);
  TopExp::MapShapes(entry.canonicalWorld, TopAbs_EDGE, entry.canonicalEdges);

  Handle(AIS_Shape) ais = new AIS_Shape(entry.displayShape);
  if (object.kind() == doc::ObjectKind::Curve) {
    ais->SetColor(Quantity_NOC_BLACK);
    ais->SetWidth(2.0);
  } else {
    ais->SetColor(rgb(object.color.r, object.color.g, object.color.b));
  }
  ais->Attributes()->SetFaceBoundaryDraw(Standard_True);
  ais->Attributes()->SetFaceBoundaryAspect(
      new Prs3d_LineAspect(Quantity_NOC_BLACK, Aspect_TOL_SOLID, 1.5));
  // Profiles often lie on faces of solids (outline drawn on a wall): draw
  // them in a later layer that shares the depth buffer so they show on top
  // of coplanar faces without z-fighting, yet stay hidden behind geometry.
  if (object.kind() == doc::ObjectKind::Face || object.kind() == doc::ObjectKind::Curve) {
    ais->SetZLayer(Graphic3d_ZLayerId_Top);
  }
  return ais;
}

void Viewport::applyPickMode(const Handle(AIS_Shape)& ais) {
  context_->Deactivate(ais);
  switch (pickMode_) {
    case PickMode::Objects: context_->Activate(ais, 0); break;
    case PickMode::Faces: context_->Activate(ais, AIS_Shape::SelectionMode(TopAbs_FACE)); break;
    case PickMode::Elements:
      // Edges first so that thin edges win over the faces they bound.
      context_->Activate(ais, AIS_Shape::SelectionMode(TopAbs_EDGE));
      context_->Activate(ais, AIS_Shape::SelectionMode(TopAbs_FACE));
      break;
    case PickMode::None: break;
  }
}

void Viewport::syncNow() {
  syncTimer_.stop();
  if (view_.IsNull()) return;

  const auto& objects = modeler_.document().objects();
  // Remove entries for deleted / hidden / changed objects.
  for (auto it = entries_.begin(); it != entries_.end();) {
    const doc::Object* o = modeler_.document().find(it->first);
    // Shapes are compared by TShape + location: unchanged objects keep their
    // tessellated display copy.
    const bool keep = o && modeler_.document().isVisible(o->id) && !o->shape.IsNull() &&
                      sameLocatedShape(it->second.canonicalWorld,
                                       modeler_.document().worldShape(*o));
    if (!keep) {
      context_->Remove(it->second.ais, Standard_False);
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }

  std::vector<TopoDS_Shape> snapShapes;
  for (const doc::Object& o : objects) {
    if (o.shape.IsNull() || !modeler_.document().isVisible(o.id)) continue;
    auto found = entries_.find(o.id);
    if (found == entries_.end()) {
      Entry entry;
      entry.ais = makeDisplay(o, entry);
      // Selection mode -1 would make the object permanently unselectable in
      // OCCT 7.6, so display with mode 0 and then apply the tool's pick mode.
      context_->Display(entry.ais, shaded_ ? AIS_Shaded : AIS_WireFrame, 0, Standard_False);
      applyPickMode(entry.ais);
      found = entries_.emplace(o.id, std::move(entry)).first;
    }
    bool selected = false;
    for (const doc::ObjectId& s : selection_) {
      selected |= s == o.id || modeler_.document().isAncestor(s, o.id);
    }
    Quantity_Color color = o.kind() == doc::ObjectKind::Curve
                               ? Quantity_Color(Quantity_NOC_BLACK)
                               : rgb(o.color.r, o.color.g, o.color.b);
    if (selected) color = kSelectedColor;
    Quantity_Color current;
    found->second.ais->Color(current);
    if (!current.IsEqual(color)) {
      found->second.ais->SetColor(color);
      // SetColor() also recolours the face boundaries; keep them black.
      found->second.ais->Attributes()->SetFaceBoundaryAspect(
          new Prs3d_LineAspect(Quantity_NOC_BLACK, Aspect_TOL_SOLID, 1.5));
    }
    context_->Redisplay(found->second.ais, Standard_False);
    snapShapes.push_back(found->second.canonicalWorld);
  }
  inference_.setShapes(snapShapes);
  // Display copies may have been rebuilt: refresh the element overlay.
  std::vector<ElementRef> still;
  for (const ElementRef& e : highlighted_) {
    const auto it = entries_.find(e.objectId);
    if (it == entries_.end()) continue;
    const int count = e.type == TopAbs_FACE ? it->second.displayFaces.Extent()
                                            : it->second.displayEdges.Extent();
    if (e.index <= count) still.push_back(e);
  }
  setHighlightedElements(still);
}

void Viewport::setSelection(const std::vector<doc::ObjectId>& ids) {
  selection_ = ids;
  syncNow();
}

void Viewport::setDisplayQuality(const display::DisplayQuality& quality) {
  quality_ = quality;
  if (view_.IsNull()) return;
  const Handle(Prs3d_Drawer)& drawer = context_->DefaultDrawer();
  drawer->SetMaximalChordialDeviation(quality.linearDeflection);
  drawer->SetDeviationAngle(quality.angularDeflection);
  // Re-tessellate fresh display copies; canonical shapes are untouched.
  for (auto& [id, entry] : entries_) context_->Remove(entry.ais, Standard_False);
  entries_.clear();
  syncNow();
}

void Viewport::setShaded(bool shaded) {
  shaded_ = shaded;
  if (view_.IsNull()) return;
  for (auto& [id, entry] : entries_) {
    context_->SetDisplayMode(entry.ais, shaded ? AIS_Shaded : AIS_WireFrame, Standard_False);
  }
  redraw();
}

// --- picking ------------------------------------------------------------------

snap::Ray Viewport::rayAt(const QPoint& pos) const {
  const Handle(Graphic3d_Camera)& camera = view_->Camera();
  Standard_Integer w = 1, h = 1;
  view_->Window()->Size(w, h);
  const double dpr = devicePixelRatioF();
  const double ndcX = 2.0 * (pos.x() * dpr) / w - 1.0;
  const double ndcY = 1.0 - 2.0 * (pos.y() * dpr) / h;
  const gp_Pnt nearPnt = camera->UnProject(gp_Pnt(ndcX, ndcY, -1.0));
  const gp_Pnt farPnt = camera->UnProject(gp_Pnt(ndcX, ndcY, 1.0));
  gp_Vec dir(nearPnt, farPnt);
  if (dir.Magnitude() < 1e-12) dir = gp_Vec(camera->Direction());
  const gp_Dir d(dir);
  if (camera->IsOrthographic()) {
    // Start well behind the near plane so that geometry outside the
    // depth range (e.g. the infinite ground plane) is still hit.
    return snap::Ray{nearPnt.Translated(gp_Vec(d) * -1.0e7), d};
  }
  return snap::Ray{camera->Eye(), d};
}

snap::ScreenPoint Viewport::project(const gp_Pnt& p) const {
  const Handle(Graphic3d_Camera)& camera = view_->Camera();
  Standard_Integer w = 1, h = 1;
  view_->Window()->Size(w, h);
  const gp_Pnt ndc = camera->Project(p);
  const double dpr = devicePixelRatioF();
  return snap::ScreenPoint{(ndc.X() + 1.0) * 0.5 * w / dpr, (1.0 - ndc.Y()) * 0.5 * h / dpr};
}

QPoint Viewport::toWidget(const gp_Pnt& p) const {
  const snap::ScreenPoint s = project(p);
  return QPoint(static_cast<int>(std::lround(s.x)), static_cast<int>(std::lround(s.y)));
}

snap::SnapResult Viewport::snap(const QPoint& pos, std::optional<gp_Pnt> anchor,
                                std::optional<gp_Dir> lockedDirection,
                                std::optional<gp_Pln> workPlane,
                                const std::vector<gp_Pnt>& trackedPoints,
                                const std::vector<gp_Dir>& trackedDirections) const {
  snap::SnapQuery query;
  query.ray = rayAt(pos);
  query.cursor = snap::ScreenPoint{static_cast<double>(pos.x()), static_cast<double>(pos.y())};
  query.project = [this](const gp_Pnt& p) { return project(p); };
  query.pixelTolerance = 10.0;
  query.anchor = anchor;
  query.lockedDirection = lockedDirection;
  if (workPlane) query.workPlane = *workPlane;
  query.trackedPoints = trackedPoints;
  query.trackedDirections = trackedDirections;
  return inference_.snap(query);
}

std::optional<std::pair<gp_Pnt, gp_Dir>> Viewport::faceAt(const QPoint& pos) const {
  snap::SnapQuery query;
  query.ray = rayAt(pos);
  query.cursor = snap::ScreenPoint{static_cast<double>(pos.x()), static_cast<double>(pos.y())};
  query.project = [this](const gp_Pnt& p) { return project(p); };
  query.pixelTolerance = 0.0;  // no point/edge snapping: just the face under the cursor
  const snap::SnapResult r = inference_.snap(query);
  if (r.kind != snap::SnapKind::OnFace || !r.faceNormal) return std::nullopt;
  return std::make_pair(r.point, *r.faceNormal);
}

void Viewport::setPickMode(PickMode mode) {
  if (mode == pickMode_) return;
  pickMode_ = mode;
  if (view_.IsNull()) return;
  context_->ClearDetected(Standard_False);
  for (auto& [id, entry] : entries_) applyPickMode(entry.ais);
  redraw();
}

std::optional<doc::ObjectId> Viewport::idOf(const Handle(AIS_InteractiveObject)& object) const {
  for (const auto& [id, entry] : entries_) {
    if (entry.ais == object) return id;
  }
  return std::nullopt;
}

std::optional<FacePick> Viewport::pickFace(const QPoint& pos) {
  if (view_.IsNull() || pickMode_ != PickMode::Faces) return std::nullopt;
  const QPoint p = devicePos(pos);
  context_->MoveTo(p.x(), p.y(), view_, Standard_False);
  redraw();
  if (!context_->HasDetected()) return std::nullopt;

  // Among faces at (nearly) the same depth, prefer bare profiles: a
  // rectangle drawn on a wall must win over the wall face it lies on.
  const Handle(StdSelect_ViewerSelector3d)& selector = context_->MainSelector();
  int chosen = 0;
  for (int i = 1; i <= selector->NbPicked(); ++i) {
    Handle(StdSelect_BRepOwner) owner = Handle(StdSelect_BRepOwner)::DownCast(selector->Picked(i));
    if (owner.IsNull() || owner->Shape().ShapeType() != TopAbs_FACE) continue;
    if (chosen == 0) {
      chosen = i;
      continue;
    }
    const auto id = idOf(Handle(AIS_InteractiveObject)::DownCast(owner->Selectable()));
    const doc::Object* o = id ? modeler_.document().find(*id) : nullptr;
    gp_Pln plane;
    // Coplanar with the front-most hit at the cursor: the profile lies on it.
    if (o && o->kind() == doc::ObjectKind::Face &&
        geom::isPlanarFace(TopoDS::Face(owner->Shape()), &plane) &&
        plane.Distance(selector->PickedPoint(chosen)) < 1.0) {
      chosen = i;
      break;
    }
  }
  if (chosen == 0) return std::nullopt;
  Handle(StdSelect_BRepOwner) owner = Handle(StdSelect_BRepOwner)::DownCast(selector->Picked(chosen));
  const auto id = idOf(Handle(AIS_InteractiveObject)::DownCast(owner->Selectable()));
  if (!id) return std::nullopt;
  const Entry& entry = entries_.at(*id);
  const int index = entry.displayFaces.FindIndex(owner->Shape());
  if (index < 1 || index > entry.canonicalFaces.Extent()) return std::nullopt;

  FacePick pick;
  pick.objectId = *id;
  // Keep the orientation of the canonical face (outward normal).
  pick.face = TopoDS::Face(entry.canonicalFaces(index));
  pick.point = selector->PickedPoint(chosen);
  return pick;
}

std::optional<doc::ObjectId> Viewport::pickObject(const QPoint& pos) {
  if (view_.IsNull() || pickMode_ != PickMode::Objects) return std::nullopt;
  const QPoint p = devicePos(pos);
  context_->MoveTo(p.x(), p.y(), view_, Standard_False);
  redraw();
  if (!context_->HasDetected()) return std::nullopt;
  return idOf(context_->DetectedInteractive());
}

std::optional<ElementRef> Viewport::pickElement(const QPoint& pos) {
  if (view_.IsNull() || pickMode_ != PickMode::Elements) return std::nullopt;
  const QPoint p = devicePos(pos);
  context_->MoveTo(p.x(), p.y(), view_, Standard_False);
  redraw();
  if (!context_->HasDetected()) return std::nullopt;
  Handle(StdSelect_BRepOwner) owner = Handle(StdSelect_BRepOwner)::DownCast(context_->DetectedOwner());
  if (owner.IsNull()) return std::nullopt;
  const auto id = idOf(context_->DetectedInteractive());
  if (!id) return std::nullopt;
  const Entry& entry = entries_.at(*id);
  const TopAbs_ShapeEnum type = owner->Shape().ShapeType();
  int index = 0;
  if (type == TopAbs_FACE) {
    index = entry.displayFaces.FindIndex(owner->Shape());
  } else if (type == TopAbs_EDGE) {
    index = entry.displayEdges.FindIndex(owner->Shape());
  }
  if (index < 1) return std::nullopt;
  return ElementRef{*id, type, index};
}

std::vector<ElementRef> Viewport::withNeighbours(const ElementRef& element) const {
  std::vector<ElementRef> out{element};
  const auto it = entries_.find(element.objectId);
  if (it == entries_.end()) return out;
  const Entry& entry = it->second;
  if (element.type == TopAbs_FACE && element.index <= entry.canonicalFaces.Extent()) {
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(entry.canonicalFaces(element.index), TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent(); ++i) {
      const int index = entry.canonicalEdges.FindIndex(edges(i));
      if (index > 0) out.push_back(ElementRef{element.objectId, TopAbs_EDGE, index});
    }
  } else if (element.type == TopAbs_EDGE && element.index <= entry.canonicalEdges.Extent()) {
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndUniqueAncestors(entry.canonicalWorld, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    const TopoDS_Shape& edge = entry.canonicalEdges(element.index);
    if (edgeFaces.Contains(edge)) {
      for (const TopoDS_Shape& face : edgeFaces.FindFromKey(edge)) {
        const int index = entry.canonicalFaces.FindIndex(face);
        if (index > 0) out.push_back(ElementRef{element.objectId, TopAbs_FACE, index});
      }
    }
  }
  return out;
}

std::vector<ElementRef> Viewport::allElements(const doc::ObjectId& id) const {
  std::vector<ElementRef> out;
  const auto it = entries_.find(id);
  if (it == entries_.end()) return out;
  for (int i = 1; i <= it->second.canonicalFaces.Extent(); ++i) out.push_back({id, TopAbs_FACE, i});
  for (int i = 1; i <= it->second.canonicalEdges.Extent(); ++i) out.push_back({id, TopAbs_EDGE, i});
  return out;
}

void Viewport::setHighlightedElements(const std::vector<ElementRef>& elements) {
  highlighted_ = elements;
  if (view_.IsNull()) return;
  for (Handle(AIS_Shape)* overlay : {&highlightFaces_, &highlightEdges_}) {
    if (!overlay->IsNull()) context_->Remove(*overlay, Standard_False);
    overlay->Nullify();
  }
  // Built from the display copies (already tessellated), never from the
  // canonical shapes.
  TopoDS_Compound faces, edges;
  BRep_Builder builder;
  builder.MakeCompound(faces);
  builder.MakeCompound(edges);
  bool anyFace = false, anyEdge = false;
  for (const ElementRef& e : elements) {
    const auto it = entries_.find(e.objectId);
    if (it == entries_.end()) continue;
    const Entry& entry = it->second;
    if (e.type == TopAbs_FACE && e.index <= entry.displayFaces.Extent()) {
      builder.Add(faces, entry.displayFaces(e.index));
      anyFace = true;
    } else if (e.type == TopAbs_EDGE && e.index <= entry.displayEdges.Extent()) {
      builder.Add(edges, entry.displayEdges(e.index));
      anyEdge = true;
    }
  }
  if (anyFace) {
    highlightFaces_ = new AIS_Shape(faces);
    highlightFaces_->SetColor(kSelectedColor);
    highlightFaces_->SetTransparency(0.35);
    highlightFaces_->Attributes()->SetFaceBoundaryDraw(Standard_False);
    highlightFaces_->SetZLayer(Graphic3d_ZLayerId_Top);
    context_->Display(highlightFaces_, AIS_Shaded, -1, Standard_False);
  }
  if (anyEdge) {
    highlightEdges_ = new AIS_Shape(edges);
    highlightEdges_->SetColor(rgb(0.1, 0.35, 0.95));
    highlightEdges_->SetWidth(3.0);
    highlightEdges_->SetZLayer(Graphic3d_ZLayerId_Top);
    context_->Display(highlightEdges_, AIS_WireFrame, -1, Standard_False);
  }
  redraw();
}

void Viewport::setToolCursor(const QCursor& cursor) {
  toolCursor_ = cursor;
  if (drag_ == Drag::None) setCursor(cursor);
}

void Viewport::clearHover() {
  if (view_.IsNull()) return;
  context_->ClearDetected(Standard_False);
  redraw();
}

// --- transient feedback -----------------------------------------------------------

void Viewport::setPreview(const TopoDS_Shape& shape, bool solid) {
  if (view_.IsNull()) return;
  if (!preview_.IsNull()) context_->Remove(preview_, Standard_False);
  preview_.Nullify();
  if (!shape.IsNull()) {
    preview_ = new AIS_Shape(shape);
    if (solid) {
      preview_->SetColor(rgb(0.55, 0.7, 0.95));
      preview_->SetTransparency(0.45);
      context_->Display(preview_, AIS_Shaded, -1, Standard_False);
    } else {
      preview_->SetColor(Quantity_NOC_BLACK);
      preview_->SetWidth(2.0);
      context_->Display(preview_, AIS_WireFrame, -1, Standard_False);
    }
  }
  redraw();
}

void Viewport::clearPreview() { setPreview(TopoDS_Shape()); }

void Viewport::setGhost(const std::vector<doc::ObjectId>& ids, const gp_Trsf& delta) {
  TopoDS_Compound compound;
  BRep_Builder builder;
  builder.MakeCompound(compound);
  bool any = false;
  for (const auto& [id, entry] : entries_) {
    bool wanted = false;
    for (const doc::ObjectId& root : ids) {
      wanted |= root == id || modeler_.document().isAncestor(root, id);
    }
    if (!wanted) continue;
    builder.Add(compound, entry.displayShape.Moved(TopLoc_Location(delta)));
    any = true;
  }
  setPreview(any ? TopoDS_Shape(compound) : TopoDS_Shape(), true);
}

void Viewport::showSnap(const snap::SnapResult& result, std::optional<gp_Pnt> anchor) {
  if (view_.IsNull()) return;
  if (!snapMarker_.IsNull()) context_->Remove(snapMarker_, Standard_False);
  if (!inferenceLine_.IsNull()) context_->Remove(inferenceLine_, Standard_False);
  if (!referenceLine_.IsNull()) context_->Remove(referenceLine_, Standard_False);
  snapMarker_.Nullify();
  inferenceLine_.Nullify();
  referenceLine_.Nullify();
  if (result.valid()) {
    const Quantity_Color color = snapColor(result.kind);
    snapMarker_ = new AIS_Shape(BRepBuilderAPI_MakeVertex(result.point).Vertex());
    const bool strong = result.kind != snap::SnapKind::WorkPlane &&
                        result.kind != snap::SnapKind::OnFace;
    snapMarker_->Attributes()->SetPointAspect(
        new Prs3d_PointAspect(strong ? Aspect_TOM_O_POINT : Aspect_TOM_PLUS, color, 2.5));
    snapMarker_->SetZLayer(Graphic3d_ZLayerId_Topmost);
    context_->Display(snapMarker_, AIS_WireFrame, -1, Standard_False);

    if (anchor && result.axis && anchor->Distance(result.point) > 1e-6) {
      inferenceLine_ = new AIS_Shape(BRepBuilderAPI_MakeEdge(*anchor, result.point).Edge());
      inferenceLine_->Attributes()->SetWireAspect(
          new Prs3d_LineAspect(color, Aspect_TOL_DASH, 2.0));
      inferenceLine_->SetZLayer(Graphic3d_ZLayerId_Topmost);
      context_->Display(inferenceLine_, AIS_WireFrame, -1, Standard_False);
    }
    // "From point" inference: dotted line back to the tracked point.
    if (result.referencePoint && result.referencePoint->Distance(result.point) > 1e-6) {
      referenceLine_ =
          new AIS_Shape(BRepBuilderAPI_MakeEdge(*result.referencePoint, result.point).Edge());
      referenceLine_->Attributes()->SetWireAspect(
          new Prs3d_LineAspect(color, Aspect_TOL_DOT, 1.5));
      referenceLine_->SetZLayer(Graphic3d_ZLayerId_Topmost);
      context_->Display(referenceLine_, AIS_WireFrame, -1, Standard_False);
    }
  }
  redraw();
}

void Viewport::hideSnap() { showSnap(snap::SnapResult{}, std::nullopt); }

bool Viewport::dumpImage(const QString& path) {
  if (view_.IsNull()) return false;
  view_->Redraw();
  return view_->Dump(path.toUtf8().constData()) == Standard_True;
}

void Viewport::redraw() {
  if (view_.IsNull()) return;
  // OCCT talks to the window system through its own connection, which can
  // see Qt's geometry changes late (e.g. right after show) without another
  // resize event following. Compare with the real native size on each redraw.
  view_->Window()->DoResize();
  Standard_Integer w = 0, h = 0;
  view_->Window()->Size(w, h);
  if (QSize(w, h) != glSize_) {
    glSize_ = QSize(w, h);
    view_->MustBeResized();
  }
  view_->Redraw();
}

// --- camera --------------------------------------------------------------------

void Viewport::orbit(const QPoint& delta) {
  // Turntable orbit around orbitPivot_ keeping world Z up. Eye and centre
  // are rotated together so the pivot stays fixed on screen.
  const Handle(Graphic3d_Camera)& camera = view_->Camera();
  gp_Pnt eye = camera->Eye();
  gp_Pnt center = camera->Center();
  gp_Dir up = camera->Up();
  const double k = 0.006;

  gp_Trsf yaw;
  yaw.SetRotation(gp_Ax1(orbitPivot_, gp::DZ()), -delta.x() * k);
  eye.Transform(yaw);
  center.Transform(yaw);
  up.Transform(yaw);

  const gp_Vec forward(eye, center);
  const gp_Vec right = forward.Crossed(gp_Vec(up));
  if (right.Magnitude() > 1e-9) {
    gp_Trsf pitch;
    pitch.SetRotation(gp_Ax1(orbitPivot_, gp_Dir(right)), -delta.y() * k);
    const gp_Pnt pitchedEye = eye.Transformed(pitch);
    const gp_Pnt pitchedCenter = center.Transformed(pitch);
    const double elevation = gp_Vec(pitchedCenter, pitchedEye).Angle(gp_Vec(gp::DZ()));
    if (elevation > 0.01 && elevation < M_PI - 0.01) {
      eye = pitchedEye;
      center = pitchedCenter;
      up.Transform(pitch);
    }
  }
  camera->SetEyeAndCenter(eye, center);
  camera->SetUp(up);
  view_->Invalidate();
}

void Viewport::setStandardView(StandardView v) {
  if (view_.IsNull()) return;
  switch (v) {
    case StandardView::Iso: view_->SetProj(V3d_XposYnegZpos); break;
    case StandardView::Top: view_->SetProj(V3d_Zpos); break;
    case StandardView::Front: view_->SetProj(V3d_Yneg); break;
    case StandardView::Right: view_->SetProj(V3d_Xpos); break;
    case StandardView::Back: view_->SetProj(V3d_Ypos); break;
    case StandardView::Left: view_->SetProj(V3d_Xneg); break;
  }
  fitAll();
}

void Viewport::setPerspective(bool perspective) {
  if (view_.IsNull()) return;
  view_->Camera()->SetProjectionType(perspective ? Graphic3d_Camera::Projection_Perspective
                                                 : Graphic3d_Camera::Projection_Orthographic);
  view_->Invalidate();
  redraw();
}

bool Viewport::isPerspective() const {
  return !view_.IsNull() && !view_->Camera()->IsOrthographic();
}

void Viewport::fitAll() {
  if (view_.IsNull()) return;
  // Fit to the model only: V3d's own FitAll() would include the long axis
  // lines and transient previews.
  Bnd_Box box;
  for (const auto& [id, entry] : entries_) box.Add(geom::exactBounds(entry.canonicalWorld));
  if (box.IsVoid()) {
    box.Add(gp_Pnt(-2000, -2000, 0));
    box.Add(gp_Pnt(8000, 8000, 3000));
  }
  view_->FitAll(box, 0.08, Standard_False);
  view_->ZFitAll();
  redraw();
}

void Viewport::fitObjects(const std::vector<doc::ObjectId>& ids) {
  if (view_.IsNull()) return;
  Bnd_Box box;
  for (const auto& id : ids) {
    for (const doc::ObjectId& leaf : modeler_.document().subtree(id)) {
      box.Add(geom::exactBounds(modeler_.document().worldShape(leaf)));
    }
  }
  if (box.IsVoid()) {
    fitAll();
    return;
  }
  view_->FitAll(box, 0.15, Standard_False);
  view_->ZFitAll();
  redraw();
}

void Viewport::storeCamera(doc::CameraState& state) const {
  if (view_.IsNull()) return;
  const Handle(Graphic3d_Camera)& c = view_->Camera();
  state.eye = {c->Eye().X(), c->Eye().Y(), c->Eye().Z()};
  state.target = {c->Center().X(), c->Center().Y(), c->Center().Z()};
  state.up = {c->Up().X(), c->Up().Y(), c->Up().Z()};
  state.perspective = !c->IsOrthographic();
  state.scale = c->Scale();
}

void Viewport::restoreCamera(const doc::CameraState& state) {
  if (view_.IsNull()) return;
  const Handle(Graphic3d_Camera)& c = view_->Camera();
  c->SetProjectionType(state.perspective ? Graphic3d_Camera::Projection_Perspective
                                         : Graphic3d_Camera::Projection_Orthographic);
  c->SetEyeAndCenter(gp_Pnt(state.eye[0], state.eye[1], state.eye[2]),
                     gp_Pnt(state.target[0], state.target[1], state.target[2]));
  c->SetUp(gp_Dir(state.up[0], state.up[1], state.up[2]));
  if (state.scale > 0) {
    c->SetScale(state.scale);
    view_->ZFitAll();
    view_->Invalidate();
    redraw();
  } else {
    fitAll();
  }
}

}  // namespace sbr::app
