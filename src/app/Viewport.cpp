#include "Viewport.h"

#include <AIS_ColoredShape.hxx>
#include <cstring>
#include <QImage>
#include <Image_PixMap.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_TextureParams.hxx>
#include <Graphic3d_Texture2D.hxx>
#include <Standard_Version.hxx>
#include <TopoDS_Iterator.hxx>

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
#include <Standard_Failure.hxx>
#include <TopoDS_Compound.hxx>
#include <Graphic3d_Camera.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_PointAspect.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <SelectMgr_ViewerSelector.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <StdSelect_ViewerSelector3d.hxx>
#include <TopExp.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <gp_Lin.hxx>
#include <TopExp_Explorer.hxx>
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
    case snap::SnapKind::Intersection: return rgb(0.0, 0.55, 0.3);
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

void Viewport::paintEvent(QPaintEvent*) { renderNow(); }

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
  if (event->button() == Qt::RightButton) {
    emit contextMenuRequested(event->position().toPoint());
    return;
  }
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
    toolCall([&] { tool_->mousePress(ToolEvent{event->position().toPoint(), event->modifiers()}); });
    emit toolInput();
  }
}

bool Viewport::toolCall(const std::function<void()>& call) {
  // A geometry kernel exception inside a tool (a degenerate preview, an
  // unexpected shape) must not take the program and the model down with
  // it: the tool starts over and the user is told. Model edits are applied
  // whole at the end of an operation, so the document stays as it was.
  QString failure;
  try {
    call();
    return true;
  } catch (const Standard_Failure& e) {
    failure = QString::fromUtf8(e.GetMessageString());
  } catch (const std::exception& e) {
    failure = QString::fromUtf8(e.what());
  }
  try {
    if (tool_) tool_->cancel();
  } catch (...) {
  }
  clearPreview();
  emit toolFailed(failure);
  return false;
}

void Viewport::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::MiddleButton) {
    drag_ = Drag::None;
    setCursor(toolCursor_);
    return;
  }
  if (event->button() == Qt::LeftButton && tool_) {
    toolCall([&] { tool_->mouseRelease(ToolEvent{event->position().toPoint(), event->modifiers()}); });
    emit toolInput();
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
  if (tool_) toolCall([&] { tool_->mouseMove(ToolEvent{pos, event->modifiers()}); });
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* event) {
  const QPoint pos = event->position().toPoint();
  bool consumed = false;
  if (event->button() == Qt::LeftButton && tool_) {
    toolCall([&] { consumed = tool_->mouseDoubleClick(ToolEvent{pos, event->modifiers()}); });
  }
  if (consumed) {
    emit toolInput();
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
    typing_ = false;
    emit escapePressed();
    return;
  }
  bool consumed = false;
  if (tool_) toolCall([&] { consumed = tool_->keyPress(event); });
  if (consumed) {
    emit toolInput();
    return;
  }
  switch (event->key()) {
    case Qt::Key_Backspace: emit measurementBackspace(); return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
      typing_ = false;
      emit measurementCommit();
      return;
    default: break;
  }
  const QString text = event->text();
  if (text.size() == 1) {
    const QChar c = text.at(0);
    if (c.isDigit() || QStringLiteral(".,;-+ mcMCxX*/sS").contains(c)) {
      typing_ = true;
      emit measurementChar(c);
      return;
    }
  }
  QWidget::keyPressEvent(event);
}

bool Viewport::event(QEvent* event) {
  if (event->type() == QEvent::ShortcutOverride) {
    auto* key = static_cast<QKeyEvent*>(event);
    const QString text = key->text();
    const bool plain = !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    if (plain && text.size() == 1) {
      const QChar c = text.at(0);
      // Digits and signs always go to the value; letters only after a digit
      // (a lone "M" still picks the move tool).
      if (c.isDigit() || QStringLiteral(".,;-+*/").contains(c) ||
          (typing_ && QStringLiteral(" mcMCxXsS").contains(c))) {
        event->accept();
        return true;
      }
    }
  }
  return QWidget::event(event);
}

void Viewport::keyReleaseEvent(QKeyEvent* event) {
  bool consumed = false;
  if (tool_) toolCall([&] { consumed = tool_->keyRelease(event); });
  if (consumed) {
    emit toolInput();
    return;
  }
  QWidget::keyReleaseEvent(event);
}

// --- document display ---------------------------------------------------------

namespace {
// Open shapes and single faces show their back in another colour (as in
// SketchUp), so front and back can be told before painting.
const Quantity_Color kBackColor = rgb(0.56, 0.62, 0.78);

void showBackSide(const Handle(AIS_Shape)& ais, doc::ObjectKind kind) {
  if (kind != doc::ObjectKind::Shell && kind != doc::ObjectKind::Face) return;
  Handle(Prs3d_ShadingAspect) shading = ais->Attributes()->ShadingAspect();
  if (shading.IsNull()) return;
  shading->Aspect()->SetDistinguish(Standard_True);
  shading->SetColor(kBackColor, Aspect_TOFM_BACK_SIDE);
}
}  // namespace

namespace {
// A 2D texture from pixels in memory (OCCT keeps that constructor
// protected, and its "manual" convenience class is gone in 8.0).
class PixmapTexture : public Graphic3d_Texture2D {
 public:
  explicit PixmapTexture(const Handle(Image_PixMap)& pixels)
#if OCC_VERSION_HEX >= 0x070700
      : Graphic3d_Texture2D(pixels, Graphic3d_TypeOfTexture_2D) {}
#else
      : Graphic3d_Texture2D(pixels, Graphic3d_TOT_2D) {}
#endif
};
}  // namespace

void Viewport::showImage(const Handle(AIS_Shape)& ais, const doc::ImageUnderlay& underlay) {
  ais->SetColor(Quantity_NOC_WHITE);
  const doc::Document::Image* stored = modeler_.document().findImage(underlay.image);
  QImage picture;
  if (!stored || !picture.loadFromData(reinterpret_cast<const uchar*>(stored->bytes.data()),
                                       static_cast<int>(stored->bytes.size()))) {
    ais->SetColor(Quantity_Color(0.9, 0.85, 0.85, Quantity_TOC_RGB));  // missing picture: a pale stand-in
    return;
  }
  // The texture maps over the face's parameter range: u along the plane's
  // x direction (the picture's width), v along its y direction; the rows go
  // in as stored (top row first), which puts the picture's top at +y.
  picture = picture.convertToFormat(QImage::Format_RGBA8888);
  Handle(Image_PixMap) pixels = new Image_PixMap();
  if (!pixels->InitTrash(Image_Format_RGBA, static_cast<Standard_Size>(picture.width()),
                         static_cast<Standard_Size>(picture.height()))) {
    return;
  }
  pixels->SetTopDown(false);
  for (int row = 0; row < picture.height(); ++row) {
    std::memcpy(pixels->ChangeRow(static_cast<Standard_Size>(row)), picture.constScanLine(row),
                static_cast<std::size_t>(picture.width()) * 4);
  }
  Handle(Graphic3d_Texture2D) texture = new PixmapTexture(pixels);
  texture->GetParams()->SetModulate(Standard_False);
  texture->GetParams()->SetRepeat(Standard_False);
  ais->Attributes()->SetupOwnShadingAspect();
  const Handle(Graphic3d_AspectFillArea3d)& aspect = ais->Attributes()->ShadingAspect()->Aspect();
  aspect->SetTextureMap(texture);
  aspect->SetTextureMapOn();
  aspect->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);
  // Slightly behind whatever is drawn on it (traced outlines, floors).
  aspect->SetPolygonOffsets(Aspect_POM_Fill, 1.0f, 4.0f);
  if (underlay.opacity < 1.0) ais->SetTransparency(1.0 - underlay.opacity);
}

void Viewport::scheduleSync() { syncTimer_.start(); }

void Viewport::releaseProto(const Entry& entry) {
  if (!entry.proto) return;
  const auto it = protos_.find(*entry.proto);
  if (it != protos_.end() && --it->second.users <= 0) protos_.erase(it);
}

Handle(AIS_Shape) Viewport::makeDisplay(const doc::Object& object, Entry& entry) {
  entry.canonicalWorld = modeler_.document().worldShape(object);
  const TopoDS_Shape& local = object.shape;
  if (local.Location().IsIdentity()) {
    // Same local shape (e.g. every placement of a component): one display
    // copy and tessellation, placed per entry.
    const ProtoKey key{local.TShape().get(), static_cast<int>(local.Orientation())};
    Proto& proto = protos_[key];
    if (proto.display.IsNull()) proto.display = display::makeDisplayCopy(local);
    ++proto.users;
    entry.proto = key;
    const gp_Trsf world = modeler_.document().worldTransform(object);
    entry.displayShape =
        world.Form() == gp_Identity ? proto.display : proto.display.Moved(TopLoc_Location(world));
  } else {
    entry.displayShape = display::makeDisplayCopy(entry.canonicalWorld);
  }
  entry.displayFaces.Clear();
  entry.canonicalFaces.Clear();
  TopExp::MapShapes(entry.displayShape, TopAbs_FACE, entry.displayFaces);
  TopExp::MapShapes(entry.canonicalWorld, TopAbs_FACE, entry.canonicalFaces);
  entry.displayEdges.Clear();
  entry.canonicalEdges.Clear();
  TopExp::MapShapes(entry.displayShape, TopAbs_EDGE, entry.displayEdges);
  TopExp::MapShapes(entry.canonicalWorld, TopAbs_EDGE, entry.canonicalEdges);

  // Coloured shapes take per-face colours on the shared display copy: the
  // tessellation stays shared between placements whatever they are painted.
  // Only objects with faces painted on their own need it (plain ones stay
  // the cheaper AIS_Shape).
  Handle(AIS_Shape) ais = !faceColorsOf(object).empty() ? Handle(AIS_Shape)(new AIS_ColoredShape(entry.displayShape))
                                                        : new AIS_Shape(entry.displayShape);
  if (object.kind() == doc::ObjectKind::Curve) {
    ais->SetColor(Quantity_NOC_BLACK);
    ais->SetWidth(2.0);
  } else if (object.kind() == doc::ObjectKind::Annotation) {
    ais->SetColor(Quantity_NOC_GRAY20);
    ais->SetWidth(1.0);
  } else if (object.kind() == doc::ObjectKind::Guide) {
    ais->SetColor(Quantity_NOC_GRAY50);
    ais->Attributes()->SetWireAspect(new Prs3d_LineAspect(Quantity_NOC_GRAY50, Aspect_TOL_DASH, 1.0));
  } else {
    const doc::Color c = modeler_.document().displayColor(object);
    ais->SetColor(rgb(c.r, c.g, c.b));
    showBackSide(ais, object.kind());
  }
  entry.hiddenEdges = doc::hiddenEdgeIndices(object);
  ais->Attributes()->SetFaceBoundaryDraw(entry.hiddenEdges.empty() ? Standard_True : Standard_False);
  entry.edgeOverlay.Nullify();
  if (!entry.hiddenEdges.empty()) {
    BRep_Builder builder;
    TopoDS_Compound shown;
    builder.MakeCompound(shown);
    for (int i = 1; i <= entry.displayEdges.Extent(); ++i) {
      if (std::find(entry.hiddenEdges.begin(), entry.hiddenEdges.end(), i) == entry.hiddenEdges.end()) {
        builder.Add(shown, entry.displayEdges(i));
      }
    }
    entry.edgeOverlay = new AIS_Shape(shown);
    entry.edgeOverlay->SetColor(Quantity_NOC_BLACK);
    entry.edgeOverlay->SetWidth(1.5);
  }
  if (const auto* image = std::get_if<doc::ImageUnderlay>(&object.recipe.base)) showImage(ais, *image);
  ais->Attributes()->SetFaceBoundaryAspect(
      new Prs3d_LineAspect(Quantity_NOC_BLACK, Aspect_TOL_SOLID, 1.5));
  // Profiles often lie on faces of solids (outline drawn on a wall): draw
  // them in a later layer that shares the depth buffer so they show on top
  // of coplanar faces without z-fighting, yet stay hidden behind geometry.
  if (object.kind() == doc::ObjectKind::Face || object.kind() == doc::ObjectKind::Curve ||
      object.kind() == doc::ObjectKind::Annotation) {
    ais->SetZLayer(Graphic3d_ZLayerId_Top);
    if (!entry.edgeOverlay.IsNull()) entry.edgeOverlay->SetZLayer(Graphic3d_ZLayerId_Top);
  }
  return ais;
}

void Viewport::removeDisplay(Entry& entry) {
  context_->Remove(entry.ais, Standard_False);
  if (!entry.edgeOverlay.IsNull()) context_->Remove(entry.edgeOverlay, Standard_False);
}

gp_Dir Viewport::viewDirection() const {
  if (view_.IsNull()) return -gp::DZ();
  return view_->Camera()->Direction();
}

void Viewport::setProposalGhost(const TopoDS_Shape& shape, const TopoDS_Shape& removed) {
  if (view_.IsNull()) return;
  if (!proposalGhost_.IsNull()) context_->Remove(proposalGhost_, Standard_False);
  proposalGhost_.Nullify();
  if (!proposalRemoved_.IsNull()) context_->Remove(proposalRemoved_, Standard_False);
  proposalRemoved_.Nullify();
  // Objects the proposal deletes: red, drawn over the model.
  bool anyRemoved = false;
  for (TopExp_Explorer e(removed, TopAbs_EDGE); e.More() && !anyRemoved; e.Next()) anyRemoved = true;
  if (!removed.IsNull() && anyRemoved) {
    proposalRemoved_ = new AIS_Shape(display::makeDisplayCopy(removed));
    proposalRemoved_->SetColor(Quantity_Color(0.9, 0.2, 0.15, Quantity_TOC_RGB));
    proposalRemoved_->SetTransparency(0.4);
    proposalRemoved_->SetZLayer(Graphic3d_ZLayerId_Top);
    context_->Display(proposalRemoved_, AIS_Shaded, -1, Standard_False);
  }
  if (!shape.IsNull()) {
    proposalGhost_ = new AIS_Shape(display::makeDisplayCopy(shape));
    proposalGhost_->SetColor(Quantity_Color(0.25, 0.5, 1.0, Quantity_TOC_RGB));
    proposalGhost_->SetTransparency(0.55);
    context_->Display(proposalGhost_, AIS_Shaded, -1, Standard_False);
  }
  redraw();
}

void Viewport::setRubberBand(const std::optional<QRect>& rect) {
  if (view_.IsNull()) return;
  if (!rect) {
    if (!rubberBand_.IsNull()) context_->Remove(rubberBand_, Standard_False);
    rubberBand_.Nullify();
    redraw();
    return;
  }
  if (rubberBand_.IsNull()) {
    rubberBand_ = new AIS_RubberBand(Quantity_Color(0.2, 0.4, 0.9, Quantity_TOC_RGB), Aspect_TOL_DASH,
                                     Quantity_Color(0.2, 0.4, 0.9, Quantity_TOC_RGB), 0.85, 1.0);
    rubberBand_->SetFilling(Standard_True);
    rubberBand_->SetDisplayMode(0);
    rubberBand_->SetZLayer(Graphic3d_ZLayerId_TopOSD);
    rubberBand_->SetTransformPersistence(new Graphic3d_TransformPers(Graphic3d_TMF_2d, Aspect_TOTP_LEFT_LOWER));
  }
  // OCCT's 2D overlay counts pixels from the bottom-left corner.
  const QPoint a = devicePos(rect->topLeft());
  const QPoint b = devicePos(rect->bottomRight());
  const int h = glSize_.height() > 0 ? glSize_.height() : static_cast<int>(height() * devicePixelRatioF());
  rubberBand_->SetRectangle(std::min(a.x(), b.x()), h - std::max(a.y(), b.y()), std::max(a.x(), b.x()),
                            h - std::min(a.y(), b.y()));
  if (context_->IsDisplayed(rubberBand_)) {
    context_->Redisplay(rubberBand_, Standard_False);
  } else {
    context_->Display(rubberBand_, 0, -1, Standard_False);
  }
  redraw();
}

void Viewport::setShowOpenEnds(bool show) {
  if (show == showOpenEnds_) return;
  showOpenEnds_ = show;
  scheduleSync();
}

void Viewport::syncOpenEnds() {
  if (!openEnds_.IsNull()) {
    context_->Remove(openEnds_, Standard_False);
    openEnds_.Nullify();
  }
  openEndCount_ = 0;
  if (!showOpenEnds_) return;
  const std::vector<gp_Pnt> ends = modeler_.openCurveEnds();
  if (ends.empty()) return;
  BRep_Builder builder;
  TopoDS_Compound compound;
  builder.MakeCompound(compound);
  for (const gp_Pnt& p : ends) builder.Add(compound, BRepBuilderAPI_MakeVertex(p).Vertex());
  openEnds_ = new AIS_Shape(compound);
  openEnds_->Attributes()->SetPointAspect(
      new Prs3d_PointAspect(Aspect_TOM_O_POINT, Quantity_Color(0.85, 0.2, 0.15, Quantity_TOC_RGB), 2.0));
  openEnds_->SetZLayer(Graphic3d_ZLayerId_Topmost);
  context_->Display(openEnds_, AIS_WireFrame, -1, Standard_False);
  openEndCount_ = static_cast<int>(ends.size());
}

void Viewport::syncSection() {
  const doc::SectionPlane* active = nullptr;
  for (const doc::SectionPlane& p : modeler_.document().settings().sections) {
    if (p.active) active = &p;
  }
  std::array<double, 6> key{};
  if (active) {
    key = {active->origin[0], active->origin[1], active->origin[2],
           active->normal[0], active->normal[1], active->normal[2]};
  }
  if (active) {
    inference_.setClip(gp_Pln(gp_Pnt(key[0], key[1], key[2]), gp_Dir(key[3], key[4], key[5])));
  } else {
    inference_.setClip(std::nullopt);
  }
  if (!section_.IsNull() && active && key == sectionKey_) return;
  if (!section_.IsNull()) {
    view_->RemoveClipPlane(section_);
    section_.Nullify();
  }
  if (!active) return;
  // OCCT keeps the side the plane normal points to; ours names the side
  // that is cut away, hence the flip.
  const gp_Pnt origin(key[0], key[1], key[2]);
  const gp_Dir normal(key[3], key[4], key[5]);
  section_ = new Graphic3d_ClipPlane(gp_Pln(origin, normal.Reversed()));
  section_->SetCapping(Standard_True);
  Graphic3d_MaterialAspect material(Graphic3d_NameOfMaterial_Plastified);
  material.SetColor(Quantity_Color(0.25, 0.25, 0.28, Quantity_TOC_RGB));
  section_->SetCappingMaterial(material);
  view_->AddClipPlane(section_);
  sectionKey_ = key;
}

void Viewport::syncLabels() {
  for (auto& [id, label] : labels_) context_->Remove(label, Standard_False);
  labels_.clear();
  for (const doc::Object& o : modeler_.document().objects()) {
    if (o.kind() != doc::ObjectKind::Annotation || !modeler_.document().isVisible(o.id)) continue;
    const auto label = doc::annotationLabel(o.recipe);
    if (!label || label->text.empty()) continue;
    bool selected = false;
    for (const doc::ObjectId& s : selection_) selected |= s == o.id || modeler_.document().isAncestor(s, o.id);
    Handle(AIS_TextLabel) text = new AIS_TextLabel();
    text->SetText(TCollection_ExtendedString(label->text.c_str(), Standard_True));  // UTF-8
    text->SetPosition(label->position.Transformed(modeler_.document().worldTransform(o)));
    text->SetHeight(15);
    text->SetColor(selected ? kSelectedColor : Quantity_Color(Quantity_NOC_BLACK));
    text->SetHJustification(Graphic3d_HTA_CENTER);
    text->SetVJustification(Graphic3d_VTA_BOTTOM);
    text->SetZLayer(Graphic3d_ZLayerId_TopOSD);
    // Selection mode -1: labels are not pickable; their lines are.
    context_->Display(text, 0, -1, Standard_False);
    labels_.emplace(o.id, text);
  }
}

QString Viewport::annotationText(const doc::ObjectId& id) const {
  const auto it = labels_.find(id);
  if (it == labels_.end()) return {};
  return QString::fromUtf16(reinterpret_cast<const char16_t*>(it->second->Text().ToExtString()),
                            it->second->Text().Length());
}

void Viewport::applyPickMode(const Handle(AIS_Shape)& ais, bool locked) {
  context_->Deactivate(ais);
  if (locked) return;  // shown and snapped to, never picked
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

std::vector<std::pair<int, std::array<double, 3>>> Viewport::faceColorsOf(const doc::Object& object) {
  const doc::Document& document = modeler_.document();
  std::vector<std::pair<int, std::array<double, 3>>> out;
  auto colorOf = [&](const std::string& material, const doc::Color& fallback) {
    const doc::Color c = document.materialColor(material, fallback);
    return std::array<double, 3>{c.r, c.g, c.b};
  };
  if (const auto* ref = std::get_if<doc::ComponentInstance>(&object.recipe.base)) {
    const doc::ComponentDefinition* def = document.findDefinition(ref->definitionId);
    if (!def || def->shape.IsNull() || object.shape.IsNull()) return out;
    // A placement with its own parameters or mirror has a variant of the
    // definition's compound: same leaves in the same order.
    const TopoDS_Shape& shape = ref->plain() ? def->shape : object.shape;
    DefinitionColors& cached = definitionColors_[ref->plain() ? def->id : def->id + "/" + object.id];
    if (cached.shape != shape.TShape().get() || cached.sequence != document.sequence()) {
      cached = DefinitionColors{shape.TShape().get(), document.sequence(), {}};
      // The definition's compound holds its leaves in order (see
      // buildDefinitionShape); a leaf with a material of its own (or inside
      // a group of the definition that has one) keeps it on every placement.
      TopTools_IndexedMapOfShape all;
      TopExp::MapShapes(shape, TopAbs_FACE, all);
      auto ownMaterial = [&](const doc::Object& leaf) {
        for (const doc::Object* o = &leaf; o;) {
          if (!o->material.empty()) return o->material;
          const doc::Object* parent = nullptr;
          for (const doc::Object& c : def->objects) {
            if (c.id == o->parent) parent = &c;
          }
          o = parent;
        }
        return std::string();
      };
      TopoDS_Iterator part(shape);
      for (const doc::Object& leaf : def->objects) {
        if (leaf.shape.IsNull()) continue;
        if (!part.More()) break;
        const TopoDS_Shape placed = part.Value();
        part.Next();
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(placed, TopAbs_FACE, faces);
        std::map<int, std::string> own;
        for (const auto& [i, m] : doc::resolveFaceMaterials(leaf).faces) own[i] = m;
        const std::string base = ownMaterial(leaf);
        for (int i = 1; i <= faces.Extent(); ++i) {
          const auto it = own.find(i);
          const std::string material = it != own.end() ? it->second : base;
          if (material.empty()) continue;
          if (const int k = all.FindIndex(faces(i)); k > 0) cached.faces.emplace_back(k, material);
        }
      }
    }
    for (const auto& [index, material] : cached.faces) out.emplace_back(index, colorOf(material, object.color));
    return out;
  }
  for (const auto& [index, material] : doc::resolveFaceMaterials(object).faces) {
    out.emplace_back(index, colorOf(material, object.color));
  }
  return out;
}

std::optional<std::array<double, 3>> Viewport::shownFaceColor(const doc::ObjectId& id,
                                                              const TopoDS_Face& worldFace) const {
  const auto it = entries_.find(id);
  if (it == entries_.end()) return std::nullopt;
  const Entry& e = it->second;
  int index = e.canonicalFaces.FindIndex(worldFace);
  for (int i = 1; i <= e.canonicalFaces.Extent() && index == 0; ++i) {
    if (e.canonicalFaces(i).TShape() == worldFace.TShape()) index = i;
  }
  if (index == 0) return std::nullopt;
  for (const auto& [i, c] : e.faceColors) {
    if (i == index) return c;
  }
  Quantity_Color base;
  e.ais->Color(base);
  return std::array<double, 3>{base.Red(), base.Green(), base.Blue()};
}

bool Viewport::syncObject(const doc::ObjectId& id) {
  const doc::Document& document = modeler_.document();
  const doc::Object* o = document.find(id);
  const bool wanted = o && !o->shape.IsNull() && document.isVisible(id);
  bool guidesChanged = false;
  auto found = entries_.find(id);
  // Shapes are compared by TShape + location: unchanged objects keep their
  // tessellated display copy and their snap index.
  // An object whose faces got (or lost every) own colour needs the other
  // kind of display object.
  const bool needsFaceColors = wanted && !faceColorsOf(*o).empty();
  if (found != entries_.end() &&
      (!wanted || !sameLocatedShape(found->second.canonicalWorld, document.worldShape(*o)) ||
       needsFaceColors != (dynamic_cast<AIS_ColoredShape*>(found->second.ais.get()) != nullptr) ||
       (o->hiddenEdges.empty() ? std::vector<int>() : doc::hiddenEdgeIndices(*o)) != found->second.hiddenEdges)) {
    removeDisplay(found->second);
    releaseProto(found->second);
    entries_.erase(found);
    inference_.removeShape(id);
    guidesChanged = guideLines_.erase(id) > 0;
    found = entries_.end();
  }
  if (!wanted) return guidesChanged;
  bool created = false;
  if (found == entries_.end()) {
    Entry entry;
    entry.ais = makeDisplay(*o, entry);
    // Selection mode -1 would make the object permanently unselectable in
    // OCCT 7.6, so display with mode 0 and then apply the tool's pick mode.
    context_->Display(entry.ais, shaded_ ? AIS_Shaded : AIS_WireFrame, 0, Standard_False);
    if (!entry.edgeOverlay.IsNull()) context_->Display(entry.edgeOverlay, AIS_WireFrame, -1, Standard_False);
    entry.locked = o->locked;
    applyPickMode(entry.ais, entry.locked);
    found = entries_.emplace(id, std::move(entry)).first;
    created = true;
    if (o->kind() == doc::ObjectKind::Guide) {
      // Guides snap as infinite lines, not as their long stand-in edge.
      TopExp_Explorer edge(found->second.canonicalWorld, TopAbs_EDGE);
      if (edge.More()) {
        BRepAdaptor_Curve curve(TopoDS::Edge(edge.Current()));
        if (curve.GetType() == GeomAbs_Line) {
          guideLines_[id] = curve.Line();
          guidesChanged = true;
        }
      }
    } else {
      inference_.setShape(id, found->second.canonicalWorld);
    }
  }
  bool selected = false;
  for (const doc::ObjectId& s : selection_) selected |= s == id || document.isAncestor(s, id);
  const doc::Color shown = document.displayColor(*o);
  Quantity_Color color = o->kind() == doc::ObjectKind::Curve        ? Quantity_Color(Quantity_NOC_BLACK)
                         : o->kind() == doc::ObjectKind::Annotation ? Quantity_Color(Quantity_NOC_GRAY20)
                         : o->kind() == doc::ObjectKind::Guide      ? Quantity_Color(Quantity_NOC_GRAY50)
                                                                    : rgb(shown.r, shown.g, shown.b);
  if (selected) color = kSelectedColor;
  bool restyled = false;
  Quantity_Color current;
  found->second.ais->Color(current);
  Entry& entry = found->second;
  auto faceColors = selected ? decltype(entry.faceColors){} : faceColorsOf(*o);
  const bool facesChanged = faceColors != entry.faceColors || selected != entry.shownSelected;
  if (!current.IsEqual(color) || facesChanged) {
    entry.ais->SetColor(color);
    if (auto* colored = dynamic_cast<AIS_ColoredShape*>(entry.ais.get()); colored && facesChanged) {
      colored->ClearCustomAspects();
      for (const auto& [index, rgbValue] : faceColors) {
        if (index >= 1 && index <= entry.displayFaces.Extent()) {
          colored->SetCustomColor(entry.displayFaces(index), rgb(rgbValue[0], rgbValue[1], rgbValue[2]));
        }
      }
    }
    entry.faceColors = std::move(faceColors);
    entry.shownSelected = selected;
    showBackSide(entry.ais, o->kind());
    // SetColor() also recolours the face boundaries; keep them black.
    entry.ais->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(Quantity_NOC_BLACK, Aspect_TOL_SOLID, 1.5));
    restyled = true;
  }
  if (found->second.locked != o->locked) {
    found->second.locked = o->locked;
    applyPickMode(found->second.ais, o->locked);
  }
  const bool faded = !editContext_.empty() && !document.isAncestor(editContext_, id);
  const double transparency = faded ? 0.8 : (xRay_ ? 0.5 : 0.0);
  if (std::abs(found->second.ais->Transparency() - transparency) > 1e-6) {
    found->second.ais->SetTransparency(transparency);
    if (!found->second.edgeOverlay.IsNull()) {
      found->second.edgeOverlay->SetTransparency(transparency);
      context_->Redisplay(found->second.edgeOverlay, Standard_False);
    }
    restyled = true;
  }
  // Only what changed is presented again (was: every object on every sync).
  if (restyled || created) context_->Redisplay(found->second.ais, Standard_False);
  return guidesChanged;
}

void Viewport::syncNow() {
  syncTimer_.stop();
  if (view_.IsNull()) return;
  const doc::Document& document = modeler_.document();

  // Objects to look at: those changed since the last sync (plus everything
  // below a changed group, whose placement they inherit), or everything.
  std::optional<std::vector<doc::ObjectId>> changed;
  if (!fullSync_) changed = document.changedSince(syncedSequence_);
  syncedSequence_ = document.sequence();
  bool guidesChanged = false;
  if (!changed) {
    for (auto it = entries_.begin(); it != entries_.end();) {
      const doc::ObjectId id = (it++)->first;  // syncObject may erase it
      if (!document.find(id)) guidesChanged |= syncObject(id);
    }
    for (const doc::Object& o : document.objects()) guidesChanged |= syncObject(o.id);
  } else {
    std::set<doc::ObjectId> ids(appearanceDirty_.begin(), appearanceDirty_.end());
    for (const doc::ObjectId& id : *changed) {
      ids.insert(id);
      const doc::Object* o = document.find(id);
      if (o && o->isContainer()) {
        for (const doc::ObjectId& c : document.subtree(id)) ids.insert(c);
      }
    }
    for (const doc::ObjectId& id : ids) guidesChanged |= syncObject(id);
  }
  fullSync_ = false;
  appearanceDirty_.clear();
  if (guidesChanged) {
    std::vector<gp_Lin> lines;
    for (const auto& [id, line] : guideLines_) lines.push_back(line);
    inference_.setGuides(lines);
  }
  syncLabels();
  syncSection();
  syncOpenEnds();
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
  // Only objects entering or leaving the selection change colour.
  for (const std::vector<doc::ObjectId>* list : {static_cast<const std::vector<doc::ObjectId>*>(&selection_), &ids}) {
    for (const doc::ObjectId& id : *list) {
      for (const doc::ObjectId& c : modeler_.document().subtree(id)) appearanceDirty_.insert(c);
    }
  }
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
  for (auto& [id, entry] : entries_) removeDisplay(entry);
  entries_.clear();
  protos_.clear();  // re-tessellate at the new quality
  inference_.setShapes({});
  guideLines_.clear();
  fullSync_ = true;
  syncNow();
}

void Viewport::setXRay(bool on) {
  xRay_ = on;
  inference_.setSeeThrough(on);
  fullSync_ = true;  // every transparency changes
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
                                const std::vector<gp_Dir>& trackedDirections,
                                std::optional<gp_Pnt> stickyPoint) const {
  snap::SnapQuery query;
  query.stickyPoint = stickyPoint;
  query.ray = rayAt(pos);
  query.cursor = snap::ScreenPoint{static_cast<double>(pos.x()), static_cast<double>(pos.y())};
  query.project = [this](const gp_Pnt& p) { return project(p); };
  query.pixelTolerance = 10.0;
  query.viewDirection = viewDirection();  // perspective-safe culling
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
  for (auto& [id, entry] : entries_) applyPickMode(entry.ais, entry.locked);
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

std::optional<ElementRef> Viewport::elementAt(const QPoint& pos) {
  const PickMode before = pickMode_;
  setPickMode(PickMode::Elements);
  const auto picked = pickElement(pos);
  setPickMode(before);
  return picked;
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

TopoDS_Shape Viewport::elementShape(const ElementRef& element) const {
  const auto it = entries_.find(element.objectId);
  if (it == entries_.end()) return {};
  const auto& map = element.type == TopAbs_FACE ? it->second.canonicalFaces : it->second.canonicalEdges;
  if (element.index < 1 || element.index > map.Extent()) return {};
  return map(element.index);
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

void Viewport::setEditContext(const doc::ObjectId& groupId) {
  editContext_ = groupId;
  fullSync_ = true;  // fading inside / outside the context
  syncNow();
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
  const bool scaled = std::abs(delta.ScaleFactor() - 1.0) > 1e-12;
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
    // Shapes cannot carry a scaled location: scaling ghosts (scale tool)
    // are moved as a whole by the presentation's transformation instead.
    builder.Add(compound, scaled ? entry.displayShape : entry.displayShape.Moved(TopLoc_Location(delta)));
    any = true;
  }
  setPreview(any ? TopoDS_Shape(compound) : TopoDS_Shape(), true);
  if (scaled && !preview_.IsNull()) {
    context_->SetLocation(preview_, TopLoc_Location(delta));
    redraw();
  }
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
  renderNow();
  return view_->Dump(path.toUtf8().constData()) == Standard_True;
}

void Viewport::redraw() {
  // Several changes in one event (sync, selection, highlight, snap marker)
  // become one frame: the view is rendered when Qt paints.
  update();
}

void Viewport::renderNow() {
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
  for (const auto& [id, entry] : entries_) {
    // Guides are 200 m long: they would zoom far out.
    const doc::Object* o = modeler_.document().find(id);
    if (o && o->kind() == doc::ObjectKind::Guide) continue;
    box.Add(geom::exactBounds(entry.canonicalWorld));
  }
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
