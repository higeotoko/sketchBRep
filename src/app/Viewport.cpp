#include "Viewport.h"

#include "WheelZoom.h"

#include <AIS_ColoredDrawer.hxx>
#include <AIS_ColoredShape.hxx>
#include <AIS_ConnectedInteractive.hxx>
#include <cstring>
#include <unordered_set>
#include <QElapsedTimer>
#include <QImage>
#include <Image_PixMap.hxx>
#include <TopLoc_Location.hxx>
#include <gp_Pnt2d.hxx>
#include <Graphic3d_MaterialAspect.hxx>
#include <Poly_Triangulation.hxx>
#include <Graphic3d_ShaderObject.hxx>
#include <Graphic3d_ShaderProgram.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <SelectMgr_Selection.hxx>
#include <Prs3d_Presentation.hxx>
#include <AIS_InteractiveObject.hxx>
#include <Graphic3d_Group.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_TextureParams.hxx>
#include <Graphic3d_Texture2D.hxx>
#include <Standard_Version.hxx>
#include <TopoDS_Iterator.hxx>

#include <algorithm>
#include <cmath>

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QWheelEvent>

#include <Aspect_DisplayConnection.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <Graphic3d_SequenceOfHClipPlane.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
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
// Sky above the horizon, ground below (SketchUp's default style), drawn in
// screen pixels behind everything. `horizon` counts rows from the top.
class Backdrop : public AIS_InteractiveObject {
 public:
  Backdrop(int width, int height, int horizon, bool under)
      : width_(width), height_(height), horizon_(horizon), under_(under) {}
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs,
               const Standard_Integer) override {
    const Quantity_Color skyTop(0.82, 0.85, 0.87, Quantity_TOC_sRGB), skyLow(0.95, 0.95, 0.96, Quantity_TOC_sRGB),
        ground(0.80, 0.80, 0.78, Quantity_TOC_sRGB);
    Handle(Graphic3d_ArrayOfTriangles) tris = new Graphic3d_ArrayOfTriangles(12, 0, Graphic3d_ArrayFlags_VertexColor);
    auto band = [&](double y0, double y1, const Quantity_Color& c0, const Quantity_Color& c1) {
      if (y1 - y0 < 0.5) return;
      const double w = width_;
      tris->AddVertex(gp_Pnt(0, y0, 0), c0);
      tris->AddVertex(gp_Pnt(w, y0, 0), c0);
      tris->AddVertex(gp_Pnt(w, y1, 0), c1);
      tris->AddVertex(gp_Pnt(0, y0, 0), c0);
      tris->AddVertex(gp_Pnt(w, y1, 0), c1);
      tris->AddVertex(gp_Pnt(0, y1, 0), c1);
    };
    // Screen y from the bottom: the horizon line and the bands either side.
    const double h = height_, line = std::clamp(h - horizon_, 0.0, h);
    if (!under_) {
      band(0, line, ground, ground);
      band(line, h, skyLow, skyTop);
    } else {
      band(0, line, skyTop, skyLow);
      band(line, h, ground, ground);
    }
    if (tris->VertexNumber() == 0) return;
    Handle(Graphic3d_AspectFillArea3d) aspect = new Graphic3d_AspectFillArea3d();
    aspect->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);
    aspect->SetInteriorStyle(Aspect_IS_SOLID);
    Handle(Graphic3d_Group) group = prs->NewGroup();
    group->SetGroupPrimitivesAspect(aspect);
    group->AddPrimitiveArray(tris);
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}

 private:
  int width_, height_, horizon_;
  bool under_;
};

// Selected faces as SketchUp shows them: blue dots over the face's own
// colour (a material stays recognisable), a fixed pixel pattern whatever
// the zoom, drawn just in front of the face.
Handle(Graphic3d_ShaderProgram) selectionDotsProgram() {
  static Handle(Graphic3d_ShaderProgram) program;
  if (!program.IsNull()) return program;
  program = new Graphic3d_ShaderProgram();
  program->AttachShader(Graphic3d_ShaderObject::CreateFromSource(
      Graphic3d_TOS_VERTEX,
      "void main() { gl_Position = occProjectionMatrix * occWorldViewMatrix * occModelWorldMatrix * occVertex; }"));
  program->AttachShader(Graphic3d_ShaderObject::CreateFromSource(
      Graphic3d_TOS_FRAGMENT,
      "void main() {\n"
      "  vec2 cell = mod(floor(gl_FragCoord.xy), 4.0);\n"
      "  if (cell.x != 0.0 || cell.y != 0.0) discard;\n"
      "  occSetFragColor(vec4(0.05, 0.15, 0.80, 1.0));\n"
      "}"));
  return program;
}

Handle(AIS_Shape) makeSelectionDots(const TopoDS_Shape& faces) {
  Handle(AIS_Shape) dots = new AIS_Shape(faces);
  const Handle(Prs3d_Drawer)& drawer = dots->Attributes();
  drawer->SetFaceBoundaryDraw(Standard_False);
  drawer->SetupOwnShadingAspect();
  const Handle(Graphic3d_AspectFillArea3d)& aspect = drawer->ShadingAspect()->Aspect();
  aspect->SetShaderProgram(selectionDotsProgram());
  aspect->SetPolygonOffsets(Aspect_POM_Fill, -1.0f, -2.0f);  // in front of the face it marks
  return dots;
}

// Faces with a picture material: the picture tiled over them (texel
// coordinates in tiles), lit like the faces, drawn just in front of them.
class TexturedFaces : public AIS_InteractiveObject {
 public:
  struct Layer {
    Handle(Graphic3d_Texture2D) texture;
    Handle(Graphic3d_ArrayOfTriangles) triangles;
    double opacity = 1.0;
  };
  std::vector<Layer> layers;
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs,
               const Standard_Integer) override {
    for (const Layer& layer : layers) {
      Graphic3d_MaterialAspect material(Graphic3d_NameOfMaterial_Plastified);
      material.SetColor(Quantity_NOC_WHITE);
      if (layer.opacity < 1.0) material.SetTransparency(static_cast<float>(1.0 - layer.opacity));
      Handle(Graphic3d_AspectFillArea3d) aspect = new Graphic3d_AspectFillArea3d();
      aspect->SetInteriorStyle(Aspect_IS_SOLID);
      aspect->SetInteriorColor(Quantity_NOC_WHITE);
      aspect->SetFrontMaterial(material);
      aspect->SetBackMaterial(material);
      aspect->SetDistinguishOff();
      aspect->SetTextureMap(layer.texture);
      aspect->SetTextureMapOn();
      aspect->SetPolygonOffsets(Aspect_POM_Fill, -0.5f, -1.0f);  // over the plain face
      Handle(Graphic3d_Group) group = prs->NewGroup();
      group->SetGroupPrimitivesAspect(aspect);
      group->AddPrimitiveArray(layer.triangles);
    }
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}
};

// A texture from pixels in memory (pictures, the sky / ground background).
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
namespace {

Quantity_Color rgb(double r, double g, double b) { return Quantity_Color(r, g, b, Quantity_TOC_RGB); }

const Quantity_Color kAxisColors[3] = {rgb(0.85, 0.1, 0.1), rgb(0.1, 0.65, 0.1), rgb(0.1, 0.2, 0.9)};
const Quantity_Color kSelectedColor = rgb(0.45, 0.62, 0.95);
const Quantity_Color kSelectedEdgeColor = rgb(0.05, 0.15, 0.80);
const Quantity_Color kLockedColor = rgb(0.9, 0.05, 0.05);

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

Viewport::Viewport(doc::Modeler& modeler, QWidget* parent)
    : QWidget(parent), modeler_(modeler), wheelRatio_(wheel::kDefaultRatioPerNotch) {
  setAttribute(Qt::WA_PaintOnScreen);
  setAttribute(Qt::WA_NoSystemBackground);
  setAttribute(Qt::WA_NativeWindow);
  setAttribute(Qt::WA_OpaquePaintEvent);
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setMinimumSize(320, 240);

  syncTimer_.setSingleShot(true);
  syncTimer_.setInterval(0);
  connect(&syncTimer_, &QTimer::timeout, this, &Viewport::syncSlice);
  interactionQualityTimer_.setSingleShot(true);
  interactionQualityTimer_.setInterval(180);
  connect(&interactionQualityTimer_, &QTimer::timeout, this, [this] {
    if (drag_ != Drag::None) { interactionQualityTimer_.start(); return; }
    restoreRenderingQuality();
  });
  wheelClock_.start();
  if (const QString log = qEnvironmentVariable("SBR_WHEEL_LOG"); !log.isEmpty()) {
    wheelLog_ = std::make_unique<QFile>(log);
    if (wheelLog_->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
      wheelLog_->write("ms\tdevice\tangle_x\tangle_y\tpixel_x\tpixel_y\tinverted\tphase\tx\ty\tdpr\tnotches"
                       "\tprojection\tfov\teye\tcenter\tscale\tanchor\tsource\n");
    } else {
      wheelLog_.reset();
    }
  }
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
    // No pre-highlight under the cursor (SketchUp): what the tools point at
    // is detected only; selections are drawn by setHighlightedElements /
    // setSelection. A pre-highlight could also stay drawn on geometry that
    // an edit had already replaced.
    context_->SetAutomaticHilight(Standard_False);

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

  view_->SetBackgroundColor(rgb(0.95, 0.95, 0.96));  // behind the sky / ground backdrop
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
  hideSnapTip();  // placed for the old size; back on the next move
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
  // Middle held, then left (CAM-01): a pan, never a click of the tool (no
  // stray point); its release and a double-click are the camera's too.
  if (event->button() == Qt::LeftButton && drag_ != Drag::None) {
    drag_ = Drag::Pan;
    leftInCameraDrag_ = true;
    setCursor(Qt::SizeAllCursor);
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
    // A step group the tool had opened is taken back whole, so later
    // edits do not pile into it.
    if (modeler_.history().inGroup()) modeler_.history().abortGroup();
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
  if (event->button() == Qt::LeftButton && leftInCameraDrag_) {
    leftInCameraDrag_ = false;
    if (drag_ == Drag::Pan && (event->buttons() & Qt::MiddleButton)) {
      drag_ = (event->modifiers() & Qt::ShiftModifier) ? Drag::Pan : Drag::Orbit;  // back to the middle's own
      setCursor(drag_ == Drag::Pan ? Qt::SizeAllCursor : Qt::ClosedHandCursor);
    }
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
  beginInteractiveRendering();
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
  if (event->button() == Qt::LeftButton && (drag_ != Drag::None || leftInCameraDrag_)) return;
  bool consumed = false;
  if (event->button() == Qt::LeftButton && tool_) {
    toolCall([&] { consumed = tool_->mouseDoubleClick(ToolEvent{pos, event->modifiers()}); });
  }
  if (consumed) {
    emit toolInput();
    return;
  }
  // Only the middle button: a left double-click that hits nothing (a miss,
  // empty space) must not jump the view.
  if (event->button() == Qt::MiddleButton) emit focusRequested(pos);
}

void Viewport::wheelEvent(QWheelEvent* event) {
  event->accept();
  if (view_.IsNull()) return;
  const double q = wheel::notches(event->angleDelta(), event->pixelDelta()) * (wheelInverted_ ? -1.0 : 1.0);
  if (q != 0.0) zoomWheel(event->position(), q);
  logWheel(event, q);
  if (q == 0.0) return;
  // The drawing in progress stays; its preview follows the new view (the
  // cursor now points at another place of the model).
  if (tool_ && drag_ == Drag::None) {
    toolCall([&] { tool_->mouseMove(ToolEvent{event->position().toPoint(), event->modifiers()}); });
  }
}

void Viewport::setWheelZoom(double ratioPerNotch, bool inverted) {
  wheelRatio_ = std::clamp(ratioPerNotch, 1.01, 4.0);
  wheelInverted_ = inverted;
}

std::optional<gp_Pnt> Viewport::lastWheelAnchor() const {
  if (!wheel_.active) return std::nullopt;
  return wheel_.anchor;
}

void Viewport::acquireWheelAnchor(const QPointF& pos) {
  // What is under the cursor: a face of the model (as shown: hidden objects,
  // guides and previews are not in the snap index) or else the ground.
  // Nothing there: a point on the cursor's ray at the depth of the last
  // anchor on something, or of the camera's focus, so the zoom never jumps
  // to the world origin or the model's centre.
  const Handle(Graphic3d_Camera)& camera = view_->Camera();
  const QPoint p = pos.toPoint();
  const snap::Ray ray = rayAt(p);
  const gp_Dir forward = camera->Direction();
  const gp_Pnt eye = camera->Eye();
  wheel_.virtualAnchor = false;
  if (const auto face = faceAt(p)) {
    wheel_.anchor = face->first;
    wheel_.source = "face";
    wheel_.lastSolid = wheel_.anchor;
    return;
  }
  double depth = eye.Distance(camera->Center());
  wheel_.source = "focus";
  if (wheel_.lastSolid) {
    const double last = gp_Vec(eye, *wheel_.lastSolid).Dot(gp_Vec(forward));
    if (last > wheel::kMinDistance) {
      depth = last;
      wheel_.source = "depth";
    }
  }
  if (std::abs(ray.direction.Z()) > 1e-9) {
    const double t = -ray.origin.Z() / ray.direction.Z();
    const gp_Pnt ground = ray.origin.Translated(gp_Vec(ray.direction) * t);
    const double groundDepth = gp_Vec(eye, ground).Dot(gp_Vec(forward));
    // Near the horizon the ground is far away; past a few times the usual
    // depth it would make one notch a huge step, so it is not used there.
    if (t > 0 && groundDepth > 0 && (camera->IsOrthographic() || groundDepth <= 3.0 * depth)) {
      wheel_.anchor = ground;
      wheel_.source = "ground";
      wheel_.lastSolid = ground;
      return;
    }
  }
  const double along = ray.direction.Dot(forward);
  if (camera->IsOrthographic() || along < 1e-6) {
    // Across the view the depth does not matter; take the focus plane.
    const double t = gp_Vec(ray.origin, camera->Center()).Dot(gp_Vec(ray.direction));
    wheel_.anchor = ray.origin.Translated(gp_Vec(ray.direction) * t);
  } else {
    wheel_.anchor = ray.origin.Translated(gp_Vec(ray.direction) * (std::max(depth, wheel::kMinVirtualDepth) / along));
  }
  wheel_.virtualAnchor = true;
}

void Viewport::zoomWheel(const QPointF& pos, double notches) {
  if (view_.IsNull() || notches == 0.0) return;
  beginInteractiveRendering();
  const Handle(Graphic3d_Camera)& camera = view_->Camera();
  const qint64 now = wheelClock_.elapsed();
  // Same gesture: close in time, the cursor (nearly) still, and the camera
  // as the last step left it (no orbit or view change in between).
  const QPointF moved = pos - wheel_.pos;
  const bool sameGesture = wheel_.active && now - wheel_.lastMs <= wheel::kGestureGapMs &&
                           std::abs(moved.x()) + std::abs(moved.y()) <= wheel::kGestureMovePx &&
                           camera->Eye().Distance(wheel_.eyeAfter) < 1e-6 &&
                           std::abs(camera->Scale() - wheel_.scaleAfter) <= 1e-9 * wheel_.scaleAfter;
  if (!sameGesture) acquireWheelAnchor(pos);
  wheel_.active = true;
  wheel_.lastMs = now;
  wheel_.pos = pos;
  const double ratio = std::pow(wheelRatio_, notches);
  if (camera->IsOrthographic()) {
    const wheel::Pose pose = wheel::zoomOrthographic(camera->Eye(), camera->Center(), camera->Direction(),
                                                     camera->Scale(), wheel_.anchor, ratio);
    camera->SetEyeAndCenter(pose.eye, pose.center);
    camera->SetScale(pose.scale);
  } else {
    if (wheel_.virtualAnchor && notches > 0) {
      // An anchor in empty space keeps ahead (along the cursor's ray, so it
      // stays under the cursor): flying through empty space never stalls.
      const gp_Vec ahead(camera->Eye(), wheel_.anchor);
      const double distance = ahead.Magnitude();
      if (distance < wheel::kMinVirtualDepth && distance > 1e-9) {
        wheel_.anchor = camera->Eye().Translated(ahead * (wheel::kMinVirtualDepth / distance));
      }
    }
    const wheel::Pose pose = wheel::zoomPerspective(camera->Eye(), camera->Center(), wheel_.anchor, ratio);
    camera->SetEyeAndCenter(pose.eye, pose.center);
  }
  wheel_.eyeAfter = camera->Eye();
  wheel_.scaleAfter = camera->Scale();
  view_->Invalidate();
  redraw();
}

void Viewport::logWheel(const QWheelEvent* event, double notches) {
  if (!wheelLog_) return;
  const Handle(Graphic3d_Camera)& c = view_->Camera();
  auto point = [](const gp_Pnt& p) { return QStringLiteral("%1,%2,%3").arg(p.X()).arg(p.Y()).arg(p.Z()); };
  const QStringList fields = {
      QString::number(wheelClock_.elapsed()),
      event->pointingDevice() ? event->pointingDevice()->name() : QString(),
      QString::number(event->angleDelta().x()), QString::number(event->angleDelta().y()),
      QString::number(event->pixelDelta().x()), QString::number(event->pixelDelta().y()),
      QString::number(event->inverted() ? 1 : 0), QString::number(static_cast<int>(event->phase())),
      QString::number(event->position().x()), QString::number(event->position().y()),
      QString::number(devicePixelRatioF()), QString::number(notches, 'g', 9),
      c->IsOrthographic() ? QStringLiteral("ortho") : QStringLiteral("persp"), QString::number(c->FOVy()),
      point(c->Eye()), point(c->Center()), QString::number(c->Scale(), 'g', 9),
      wheel_.active ? point(wheel_.anchor) : QString(), QString::fromLatin1(wheel_.source)};
  wheelLog_->write((fields.join(QLatin1Char('\t')) + QLatin1Char('\n')).toUtf8());
  wheelLog_->flush();
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

void Viewport::displayWithPickMode(Entry& entry) {
  // Displayed straight with the selection the tool picks with (computing
  // another first, then replacing it, doubled the cost of every new
  // object). Selection mode -1 would make the object permanently
  // unselectable in OCCT 7.6: no pick mode displays with 0, deactivated.
  const bool lockedFaces = entry.locked && pickMode_ == PickMode::Faces;
  const int mode = pickMode_ == PickMode::Faces && !lockedFaces ? AIS_Shape::SelectionMode(TopAbs_FACE)
                   : pickMode_ == PickMode::Elements           ? AIS_Shape::SelectionMode(TopAbs_EDGE)
                                                               : 0;
  context_->Display(entry.ais, shaded_ ? AIS_Shaded : AIS_WireFrame, mode, Standard_False);
  if (!entry.edgeOverlay.IsNull()) context_->Display(entry.edgeOverlay, AIS_WireFrame, -1, Standard_False);
  if (pickMode_ == PickMode::Elements) {
    context_->Activate(entry.ais, AIS_Shape::SelectionMode(TopAbs_FACE));
  } else if (pickMode_ == PickMode::None || lockedFaces) {
    context_->Deactivate(entry.ais);
  }
}

bool Viewport::relocate(const doc::ObjectId& id, Entry& entry, const TopoDS_Shape& world) {
  if (world.TShape() != entry.canonicalWorld.TShape() || world.Orientation() != entry.canonicalWorld.Orientation()) {
    return false;
  }
  const gp_Trsf shown = world.Location().Transformation() * entry.builtLocation.Transformation().Inverted();
  if (std::abs(shown.ScaleFactor() - 1.0) > 1e-9 || shown.IsNegative()) return false;  // rigid only
  entry.shown = shown;
  const TopLoc_Location location(shown);
  context_->SetLocation(entry.ais, location);
  if (!entry.edgeOverlay.IsNull()) context_->SetLocation(entry.edgeOverlay, location);
  if (!entry.textureOverlay.IsNull()) context_->SetLocation(entry.textureOverlay, location);
  if (!entry.selectionDots.IsNull()) {
    // Shared dots are placed at the object's world transformation.
    const bool shared = !Handle(AIS_ConnectedInteractive)::DownCast(entry.selectionDots).IsNull();
    context_->SetLocation(entry.selectionDots, shared ? world.Location() : location);
  }
  entry.canonicalWorld = world;
  entry.canonicalFaces.Clear();
  entry.canonicalEdges.Clear();
  TopExp::MapShapes(world, TopAbs_FACE, entry.canonicalFaces);
  TopExp::MapShapes(world, TopAbs_EDGE, entry.canonicalEdges);
  inference_.setShape(id, world);
  return true;
}

void Viewport::releaseProto(const Entry& entry) {
  if (!entry.proto) return;
  const auto it = protos_.find(*entry.proto);
  if (it != protos_.end() && --it->second.users <= 0) protos_.erase(it);
}

Handle(AIS_Shape) Viewport::makeDisplay(const doc::Object& object, Entry& entry) {
  entry.canonicalWorld = modeler_.document().worldShape(object);
  entry.builtLocation = entry.canonicalWorld.Location();
  entry.shown = gp_Trsf();
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
  if (!entry.textureOverlay.IsNull()) context_->Remove(entry.textureOverlay, Standard_False);
  if (!entry.selectionDots.IsNull()) context_->Remove(entry.selectionDots, Standard_False);
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
  const auto& document = modeler_.document();
  const auto& planes = document.settings().sections;
  const auto& display = document.settings().sectionDisplay;
  if (sectionMarkerSequence_ != document.sequence()) {
    const auto changed = document.changedSince(sectionMarkerSequence_);
    sectionBoundsDirty_ |= !changed || !changed->empty();
    sectionMarkerSequence_ = document.sequence();
  }
  if (!display.planes) {
    if (!sectionMarkers_.IsNull()) context_->Erase(sectionMarkers_, Standard_False);
  } else if (sectionMarkerKey_ != planes || sectionBoundsDirty_) {
    const bool any = std::any_of(planes.begin(), planes.end(), [](const auto& p) { return p.visible; });
    if (any && sectionBoundsDirty_) {
      sectionBounds_ = Bnd_Box();
      for (const auto& o : document.objects()) {
        if (o.kind() != doc::ObjectKind::Guide && o.kind() != doc::ObjectKind::Annotation && document.isVisible(o.id))
          sectionBounds_.Add(document.worldBounds(o));
      }
      sectionBoundsDirty_ = false;
      ++sectionBoundsScans_;
    }
    if (!sectionMarkers_.IsNull()) context_->Remove(sectionMarkers_, Standard_False);
    sectionMarkers_.Nullify();
    double half = 2000;
    gp_Pnt center(0, 0, 0);
    if (!sectionBounds_.IsVoid() && !sectionBounds_.IsOpen()) {
      double x, y, z, X, Y, Z;
      sectionBounds_.Get(x, y, z, X, Y, Z);
      center = gp_Pnt((x+X)/2, (y+Y)/2, (z+Z)/2);
      half = std::max(100.0, 0.6 * std::sqrt(sectionBounds_.SquareExtent()));
    }
    BRep_Builder builder;
    TopoDS_Compound markers;
    builder.MakeCompound(markers);
    for (const auto& p : planes) {
      if (!p.visible) continue;
      const gp_Pnt origin(p.origin[0], p.origin[1], p.origin[2]);
      const gp_Vec n(p.normal[0], p.normal[1], p.normal[2]);
      if (n.SquareMagnitude() < 1e-20) continue;
      const gp_Dir normal(n);
      const gp_Pnt at = center.Translated(-gp_Vec(normal) * gp_Vec(origin, center).Dot(gp_Vec(normal)));
      builder.Add(markers, BRepBuilderAPI_MakeFace(gp_Pln(at, normal), -half, half, -half, half).Face());
    }
    if (any) {
      sectionMarkers_ = new AIS_Shape(markers);
      sectionMarkers_->SetColor(rgb(0.65, 0.61, 0.52));
      sectionMarkers_->SetTransparency(0.9);
      sectionMarkers_->SetPolygonOffsets(Aspect_POM_Fill, -1.0f, -2.0f);
      sectionMarkers_->Attributes()->SetFaceBoundaryDraw(true);
      sectionMarkers_->Attributes()->SetFaceBoundaryAspect(
          new Prs3d_LineAspect(rgb(0.45, 0.42, 0.35), Aspect_TOL_DASH, 1.5));
      Handle(Graphic3d_SequenceOfHClipPlane) unclipped = new Graphic3d_SequenceOfHClipPlane;
      unclipped->SetOverrideGlobal(true);
      sectionMarkers_->SetClipPlanes(unclipped);
      context_->Display(sectionMarkers_, AIS_Shaded, -1, Standard_False);
      ++sectionMarkerBuilds_;
    }
    sectionMarkerKey_ = planes;
  } else if (!sectionMarkers_.IsNull() && !context_->IsDisplayed(sectionMarkers_)) {
    context_->Display(sectionMarkers_, AIS_Shaded, -1, Standard_False);
  }
  const doc::SectionPlane* active = nullptr;
  for (const doc::SectionPlane& p : modeler_.document().settings().sections) {
    if (p.active && display.cuts) active = &p;
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
  if (!section_.IsNull() && active && key == sectionKey_) {
    section_->SetCapping(display.fill);
    return;
  }
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
  section_->SetCapping(display.fill);
  Graphic3d_MaterialAspect material(Graphic3d_NameOfMaterial_Plastified);
  material.SetColor(Quantity_Color(0.25, 0.25, 0.28, Quantity_TOC_RGB));
  section_->SetCappingMaterial(material);
  view_->AddClipPlane(section_);
  sectionKey_ = key;
}

void Viewport::syncLabel(const doc::ObjectId& id) {
  // One annotation's text, as its object is synchronised (all of them were
  // made again on every update of the view).
  if (const auto it = labels_.find(id); it != labels_.end()) {
    context_->Remove(it->second, Standard_False);
    labels_.erase(it);
  }
  const doc::Object* o = modeler_.document().find(id);
  if (!o || o->kind() != doc::ObjectKind::Annotation || !modeler_.document().isVisible(o->id)) return;
  const auto label = doc::annotationLabel(o->recipe);
  if (!label || label->text.empty()) return;
  const bool selected = isSelected(*o);
  Handle(AIS_TextLabel) text = new AIS_TextLabel();
  text->SetText(TCollection_ExtendedString(label->text.c_str(), Standard_True));  // UTF-8
  text->SetPosition(label->position.Transformed(modeler_.document().worldTransform(*o)));
  text->SetHeight(15);
  text->SetColor(selected ? kSelectedColor : Quantity_Color(Quantity_NOC_BLACK));
  text->SetHJustification(Graphic3d_HTA_CENTER);
  text->SetVJustification(Graphic3d_VTA_BOTTOM);
  text->SetZLayer(Graphic3d_ZLayerId_TopOSD);
  // Selection mode -1: labels are not pickable; their lines are.
  context_->Display(text, 0, -1, Standard_False);
  labels_.emplace(o->id, text);
}

QString Viewport::annotationText(const doc::ObjectId& id) const {
  const auto it = labels_.find(id);
  if (it == labels_.end()) return {};
  return QString::fromUtf16(reinterpret_cast<const char16_t*>(it->second->Text().ToExtString()),
                            it->second->Text().Length());
}

void Viewport::applyPickMode(const Handle(AIS_Shape)& ais, bool locked) {
  context_->Deactivate(ais);
  // Locked: still selectable (as a whole, shown red) but no face to push.
  if (locked && pickMode_ == PickMode::Faces) return;
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

std::vector<std::pair<int, std::string>> Viewport::faceMaterialIdsOf(const doc::Object& object) {
  const doc::Document& document = modeler_.document();
  std::vector<std::pair<int, std::string>> out;
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
    return cached.faces;
  }
  return doc::resolveFaceMaterials(object).faces;
}

std::vector<std::pair<int, std::array<double, 4>>> Viewport::faceColorsOf(const doc::Object& object) {
  const doc::Document& document = modeler_.document();
  std::vector<std::pair<int, std::array<double, 4>>> out;
  for (const auto& [index, material] : faceMaterialIdsOf(object)) {
    const doc::Color c = document.materialColor(material, object.color);
    out.emplace_back(index, std::array<double, 4>{c.r, c.g, c.b, document.materialOpacity(material)});
  }
  return out;
}

Handle(Graphic3d_Texture2D) Viewport::materialTexture(const doc::Material& material) {
  // One decoded picture per (picture, tint): shared by every face using it.
  const std::string key = material.texture->image +
                          (material.texture->colorize ? "/" + std::to_string(material.color.r) + "," +
                                                            std::to_string(material.color.g) + "," +
                                                            std::to_string(material.color.b)
                                                      : std::string());
  if (const auto it = textures_.find(key); it != textures_.end()) return it->second;
  const doc::Document::Image* stored = modeler_.document().findImage(material.texture->image);
  QImage picture;
  if (!stored || !picture.loadFromData(reinterpret_cast<const uchar*>(stored->bytes.data()),
                                       static_cast<int>(stored->bytes.size()))) {
    return {};
  }
  picture = picture.convertToFormat(QImage::Format_RGBA8888);
  if (material.texture->colorize) {
    // The picture's light and dark, in the material's colour.
    for (int y = 0; y < picture.height(); ++y) {
      uchar* row = picture.scanLine(y);
      for (int x = 0; x < picture.width(); ++x) {
        const double v = (0.299 * row[4 * x] + 0.587 * row[4 * x + 1] + 0.114 * row[4 * x + 2]) / 255.0;
        row[4 * x] = static_cast<uchar>(std::lround(255 * material.color.r * v));
        row[4 * x + 1] = static_cast<uchar>(std::lround(255 * material.color.g * v));
        row[4 * x + 2] = static_cast<uchar>(std::lround(255 * material.color.b * v));
      }
    }
  }
  Handle(Image_PixMap) pixels = new Image_PixMap();
  if (!pixels->InitTrash(Image_Format_RGBA, static_cast<Standard_Size>(picture.width()),
                         static_cast<Standard_Size>(picture.height()))) {
    return {};
  }
  pixels->SetTopDown(false);
  for (int row = 0; row < picture.height(); ++row) {
    std::memcpy(pixels->ChangeRow(static_cast<Standard_Size>(row)), picture.constScanLine(row),
                static_cast<std::size_t>(picture.width()) * 4);
  }
  Handle(Graphic3d_Texture2D) texture = new PixmapTexture(pixels);
  texture->GetParams()->SetModulate(Standard_True);  // lit like the faces
  texture->GetParams()->SetRepeat(Standard_True);    // tiles
  texture->GetParams()->SetFilter(Graphic3d_TOTF_TRILINEAR);
  // Pictures no face shows any more (a tint dragged through many colours
  // makes one each) are let go: only this cache still holds them.
  for (auto it = textures_.begin(); it != textures_.end();) {
    it = it->second->GetRefCount() == 1 ? textures_.erase(it) : std::next(it);
  }
  textures_[key] = texture;
  return texture;
}

void Viewport::updateTextures(const doc::Object& object, Entry& entry) {
  // Faces whose material has a picture get it laid over them at its real
  // size: a tile per width x height mm, on the face's plane (horizontal
  // rows along the face).
  const doc::Document& document = modeler_.document();
  std::map<int, std::string> own;
  for (const auto& [i, m] : faceMaterialIdsOf(object)) own[i] = m;
  const std::string base = document.inheritedMaterial(object);
  std::map<std::string, std::vector<int>> byMaterial;
  for (int i = 1; i <= entry.displayFaces.Extent(); ++i) {
    const auto it = own.find(i);
    const std::string id = it != own.end() ? it->second : base;
    const doc::Material* m = document.findMaterial(id);
    if (m && m->texture) byMaterial[id].push_back(i);
  }
  // What it shows, to rebuild only on a change.
  std::string key;
  for (const auto& [id, faces] : byMaterial) {
    const doc::Material& m = *document.findMaterial(id);
    key += id + ":" + m.texture->image + std::to_string(m.texture->width) + "x" + std::to_string(m.texture->height) +
           (m.texture->colorize ? "c" + std::to_string(m.color.r + 2 * m.color.g + 4 * m.color.b) : "") + "o" +
           std::to_string(m.opacity) + "[" + std::to_string(faces.size()) + "]";
  }
  if (key == entry.textureKey) return;
  entry.textureKey = key;
  if (!entry.textureOverlay.IsNull()) context_->Remove(entry.textureOverlay, Standard_False);
  entry.textureOverlay.Nullify();
  if (byMaterial.empty()) return;
  Handle(TexturedFaces) overlay = new TexturedFaces();
  for (const auto& [id, faces] : byMaterial) {
    const doc::Material& m = *document.findMaterial(id);
    const Handle(Graphic3d_Texture2D) texture = materialTexture(m);
    if (texture.IsNull()) continue;
    std::vector<gp_Pnt> nodes;
    std::vector<gp_Dir> normals;
    std::vector<gp_Pnt2d> uvs;
    std::vector<std::array<int, 3>> triangles;
    for (int index : faces) {
      const TopoDS_Face face = TopoDS::Face(entry.displayFaces(index));
      TopLoc_Location loc;
      const Handle(Poly_Triangulation) mesh = BRep_Tool::Triangulation(face, loc);
      if (mesh.IsNull()) continue;
      const gp_Trsf t = loc.Transformation();
      // The face's plane: rows run horizontally on walls, along x on floors.
      gp_Dir n = gp::DZ();
      if (const auto pn = geom::planarFaceNormal(face)) n = *pn;
      const gp_Dir u = std::abs(n.Z()) > 0.9 ? gp::DX() : gp_Dir(gp::DZ().Crossed(n));
      const gp_Dir v = n.Crossed(u);
      const int first = static_cast<int>(nodes.size());
      for (int k = 1; k <= mesh->NbNodes(); ++k) {
        const gp_Pnt p = mesh->Node(k).Transformed(t);
        nodes.push_back(p);
        normals.push_back(n);
        uvs.emplace_back(gp_Vec(p.XYZ()).Dot(gp_Vec(u)) / m.texture->width,
                         gp_Vec(p.XYZ()).Dot(gp_Vec(v)) / m.texture->height);
      }
      const bool reversed = face.Orientation() == TopAbs_REVERSED;
      for (int k = 1; k <= mesh->NbTriangles(); ++k) {
        int a, b, c;
        mesh->Triangle(k).Get(a, b, c);
        if (reversed) std::swap(b, c);
        triangles.push_back({first + a, first + b, first + c});
      }
    }
    if (triangles.empty()) continue;
    Handle(Graphic3d_ArrayOfTriangles) array = new Graphic3d_ArrayOfTriangles(
        static_cast<int>(nodes.size()), static_cast<int>(3 * triangles.size()),
        Graphic3d_ArrayFlags_VertexNormal | Graphic3d_ArrayFlags_VertexTexel);
    for (std::size_t k = 0; k < nodes.size(); ++k) array->AddVertex(nodes[k], normals[k], uvs[k]);
    for (const auto& tri : triangles) array->AddEdges(tri[0], tri[1], tri[2]);
    overlay->layers.push_back({texture, array, m.opacity});
  }
  if (overlay->layers.empty()) return;
  overlay->SetZLayer(entry.ais->ZLayer());
  if (entry.shown.Form() != gp_Identity) overlay->SetLocalTransformation(entry.shown);
  entry.textureOverlay = overlay;
  context_->Display(entry.textureOverlay, 0, -1, Standard_False);
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
    if (i == index) return std::array<double, 3>{c[0], c[1], c[2]};
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
  // Hidden (itself, its tag or a parent): the display is only taken off
  // the screen, to come back as it was (showing a tag of thousands of
  // objects rebuilt every display).
  if (found != entries_.end() && o && !o->shape.IsNull() && !wanted) {
    Entry& entry = found->second;
    if (!entry.erased) {
      entry.erased = true;
      for (const Handle(AIS_InteractiveObject)& shown :
           {Handle(AIS_InteractiveObject)(entry.ais), Handle(AIS_InteractiveObject)(entry.edgeOverlay),
            entry.textureOverlay, entry.selectionDots}) {
        if (!shown.IsNull()) context_->Erase(shown, Standard_False);
      }
      inference_.removeShape(id);
      guidesChanged = guideLines_.erase(id) > 0;
    }
    return guidesChanged;
  }
  const bool needsFaceColors = wanted && !faceColorsOf(*o).empty();
  const TopoDS_Shape world = wanted ? document.worldShape(*o) : TopoDS_Shape();
  if (found != entries_.end() && wanted && found->second.erased &&
      sameLocatedShape(found->second.canonicalWorld, world) && o->kind() != doc::ObjectKind::Guide) {
    // Shown again: the kept display back on screen.
    Entry& entry = found->second;
    entry.erased = false;
    displayWithPickMode(entry);
    if (!entry.textureOverlay.IsNull()) context_->Display(entry.textureOverlay, 0, -1, Standard_False);
    if (!entry.selectionDots.IsNull()) context_->Display(entry.selectionDots, AIS_Shaded, -1, Standard_False);
    inference_.setShape(id, entry.canonicalWorld);
  } else if (found != entries_.end() && found->second.erased) {
    // Changed while hidden: built again below.
    found->second.erased = false;
    removeDisplay(found->second);
    releaseProto(found->second);
    entries_.erase(found);
    found = entries_.end();
  }
  if (found != entries_.end() && wanted && o->kind() != doc::ObjectKind::Guide &&
      !sameLocatedShape(found->second.canonicalWorld, world) &&
      needsFaceColors == (dynamic_cast<AIS_ColoredShape*>(found->second.ais.get()) != nullptr) &&
      (o->hiddenEdges.empty() ? std::vector<int>() : doc::hiddenEdgeIndices(*o)) == found->second.hiddenEdges) {
    relocate(id, found->second, world);  // moved only: no new display
  }
  if (found != entries_.end() &&
      (!wanted || !sameLocatedShape(found->second.canonicalWorld, world) ||
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
    entry.locked = o->locked;
    // Displayed below, once styled: its presentation is computed once.
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
  const bool selected = isSelected(*o);
  // Locked itself or inside a locked group: shown red when selected.
  bool locked = false;
  for (const doc::Object* a = o; a && !locked; a = a->parent.empty() ? nullptr : document.find(a->parent)) {
    locked = a->locked;
  }
  const doc::Color shown = document.displayColor(*o);
  Quantity_Color color = o->kind() == doc::ObjectKind::Curve        ? Quantity_Color(Quantity_NOC_BLACK)
                         : o->kind() == doc::ObjectKind::Annotation ? Quantity_Color(Quantity_NOC_GRAY20)
                         : o->kind() == doc::ObjectKind::Guide      ? Quantity_Color(Quantity_NOC_GRAY50)
                                                                    : rgb(shown.r, shown.g, shown.b);
  // Selected: lines (curves, notes) turn blue; objects with faces keep
  // their colours under blue dots, with blue edges (SketchUp).
  const bool hasFaces = o->kind() != doc::ObjectKind::Curve && o->kind() != doc::ObjectKind::Annotation &&
                        o->kind() != doc::ObjectKind::Guide;
  if (selected && !hasFaces) color = locked ? kLockedColor : kSelectedColor;
  // Restyling changes the aspects of the computed presentation in place
  // (SetColor / SetTransparency / SynchronizeAspects); only per-face
  // colours need the presentation computed again.
  bool recompute = false;
  Quantity_Color current;
  found->second.ais->Color(current);
  Entry& entry = found->second;
  auto faceColors = faceColorsOf(*o);
  const bool colorChanged = !current.IsEqual(color);
  const bool faceColorsChanged = faceColors != entry.faceColors;
  const bool stateChanged = selected != entry.shownSelected || locked != entry.shownLocked;
  if (created || colorChanged || faceColorsChanged || stateChanged) {
    entry.ais->SetColor(color);
    auto sameFaces = [&] {
      // The same faces painted, see-through the same way: only colours differ.
      if (entry.faceColors.size() != faceColors.size()) return false;
      for (std::size_t k = 0; k < faceColors.size(); ++k) {
        if (faceColors[k].first != entry.faceColors[k].first ||
            (faceColors[k].second[3] < 1.0) != (entry.faceColors[k].second[3] < 1.0)) {
          return false;
        }
      }
      return true;
    };
    auto* colored = dynamic_cast<AIS_ColoredShape*>(entry.ais.get());
    if (colored && !created && !colorChanged && faceColorsChanged && sameFaces()) {
      // A material's colour being edited: the faces' aspects change in
      // place (SynchronizeAspects below), the presentation is kept.
      for (const auto& [index, rgbValue] : faceColors) {
        if (index < 1 || index > entry.displayFaces.Extent()) continue;
        const Handle(AIS_ColoredDrawer)& drawer = colored->CustomAspects(entry.displayFaces(index));
        const Quantity_Color c = rgb(rgbValue[0], rgbValue[1], rgbValue[2]);
        drawer->SetOwnColor(c);
        drawer->ShadingAspect()->SetColor(c);
        if (rgbValue[3] < 1.0) {
          drawer->SetOwnTransparency(1.0 - rgbValue[3]);
          drawer->ShadingAspect()->SetTransparency(1.0 - rgbValue[3]);
        }
      }
    } else if (colored && (created || colorChanged || faceColorsChanged)) {
      recompute = true;
      colored->ClearCustomAspects();
      for (const auto& [index, rgbValue] : faceColors) {
        if (index >= 1 && index <= entry.displayFaces.Extent()) {
          colored->SetCustomColor(entry.displayFaces(index), rgb(rgbValue[0], rgbValue[1], rgbValue[2]));
          if (rgbValue[3] < 1.0) colored->SetCustomTransparency(entry.displayFaces(index), 1.0 - rgbValue[3]);
        }
      }
    }
    entry.faceColors = std::move(faceColors);
    entry.shownSelected = selected;
    entry.shownLocked = locked;
    showBackSide(entry.ais, o->kind());
    // SetColor() also recolours the face boundaries; black, or blue when
    // selected (red when locked: selected but not to be edited).
    const Quantity_Color line = !selected  ? Quantity_Color(Quantity_NOC_BLACK)
                                : locked    ? kLockedColor
                                            : kSelectedEdgeColor;
    entry.ais->Attributes()->FaceBoundaryAspect()->SetColor(line);
    entry.ais->SynchronizeAspects();
    if (!entry.edgeOverlay.IsNull()) entry.edgeOverlay->SetColor(line);
    // The dots over the faces of a selected object.
    if (!entry.selectionDots.IsNull()) context_->Remove(entry.selectionDots, Standard_False);
    entry.selectionDots.Nullify();
    if (selected && hasFaces && !locked) {
      const auto proto = entry.proto ? protos_.find(*entry.proto) : protos_.end();
      if (proto != protos_.end()) {
        // Placements of one shape share the dots' presentation.
        if (proto->second.dots.IsNull()) proto->second.dots = makeSelectionDots(proto->second.display);
        Handle(AIS_ConnectedInteractive) placed = new AIS_ConnectedInteractive();
        placed->Connect(proto->second.dots, document.worldTransform(*o));
        entry.selectionDots = placed;
      } else {
        entry.selectionDots = makeSelectionDots(entry.displayShape);
        if (entry.shown.Form() != gp_Identity) entry.selectionDots->SetLocalTransformation(entry.shown);
      }
      entry.selectionDots->SetZLayer(entry.ais->ZLayer());
      context_->Display(entry.selectionDots, AIS_Shaded, -1, Standard_False);
    }
  }
  if (!created && found->second.locked != o->locked) {
    found->second.locked = o->locked;
    applyPickMode(found->second.ais, o->locked);
  }
  const bool faded = !editContext_.empty() && !document.isAncestor(editContext_, id);
  // A see-through material shows through (glass), whatever else applies.
  const double own = hasFaces ? 1.0 - document.displayOpacity(*o) : 0.0;
  const double transparency = std::max(own, faded ? 0.8 : (xRay_ ? 0.5 : 0.0));
  // Fading in / out (group edit, X-ray) only changes the aspects of the
  // computed presentation (SetTransparency synchronises them): nothing is
  // tessellated again, so entering a group among thousands stays quick.
  if (std::abs(found->second.ais->Transparency() - transparency) > 1e-6) {
    found->second.ais->SetTransparency(transparency);
    if (!found->second.edgeOverlay.IsNull()) found->second.edgeOverlay->SetTransparency(transparency);
  }
  if (created) {
    displayWithPickMode(entry);
  } else if (recompute) {
    context_->Redisplay(entry.ais, Standard_False);  // per-face colours changed
  }
  // Display/Redisplay tessellates the display copy. A rebuilt shape has no
  // triangles yet before that call; caching its texture then loses the picture.
  if (hasFaces) updateTextures(*o, entry);
  return guidesChanged;
}

void Viewport::syncNow() { syncSome(std::nullopt); }

void Viewport::syncSlice() {
  constexpr double kSliceMs = 50.0;
  syncSome(kSliceMs);
}

void Viewport::syncSome(std::optional<double> budgetMs) {
  syncTimer_.stop();
  if (view_.IsNull()) return;
  const doc::Document& document = modeler_.document();

  // The budget counts from here: gathering the queue is part of the slice.
  QElapsedTimer clock;
  clock.start();
  // Objects to look at: what an earlier slice left over first (changes
  // coming in all the time must not keep it waiting for ever), then those
  // whose appearance changed (selection, context) and those changed since
  // the last sync (plus everything below a changed group, whose placement
  // they inherit), or everything.
  std::optional<std::vector<doc::ObjectId>> changed;
  if (!fullSync_) changed = document.changedSince(syncedSequence_);
  syncedSequence_ = document.sequence();
  std::vector<doc::ObjectId> queue = std::move(pendingSync_);
  pendingSync_.clear();
  pendingSet_.clear();
  queue.insert(queue.end(), appearanceDirty_.begin(), appearanceDirty_.end());
  appearanceDirty_.clear();
  if (!changed) {
    for (const auto& [id, entry] : entries_) {
      if (!document.find(id)) queue.push_back(id);
    }
    for (const doc::Object& o : document.objects()) queue.push_back(o.id);
  } else {
    for (const doc::ObjectId& id : *changed) {
      queue.push_back(id);
      const doc::Object* o = document.find(id);
      if (o && o->isContainer()) {
        for (const doc::ObjectId& c : document.subtree(id)) queue.push_back(c);
      }
    }
  }
  fullSync_ = false;
  std::unordered_set<doc::ObjectId> done;
  bool guidesChanged = false;
  for (std::size_t i = 0; i < queue.size(); ++i) {
    if (budgetMs && clock.nsecsElapsed() / 1e6 > *budgetMs) {
      for (std::size_t j = i; j < queue.size(); ++j) {
        if (done.insert(queue[j]).second) {
          pendingSync_.push_back(queue[j]);
          pendingSet_.insert(queue[j]);
        }
      }
      break;
    }
    if (done.insert(queue[i]).second) {
      guidesChanged |= syncObject(queue[i]);
      syncLabel(queue[i]);
    }
  }
  if (guidesChanged) {
    std::vector<gp_Lin> lines;
    for (const auto& [id, line] : guideLines_) lines.push_back(line);
    inference_.setGuides(lines);
  }
  syncSection();
  syncOpenEnds();
  syncEditBox();
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
  if (!pendingSync_.empty()) {
    redraw();  // show what is done so far
    syncTimer_.start();
  }
}

void Viewport::setSelection(const std::vector<doc::ObjectId>& ids) {
  // Only objects entering or leaving the selection change colour.
  for (const std::vector<doc::ObjectId>* list : {static_cast<const std::vector<doc::ObjectId>*>(&selection_), &ids}) {
    for (const doc::ObjectId& id : *list) {
      for (const doc::ObjectId& c : modeler_.document().subtree(id)) appearanceDirty_.insert(c);
    }
  }
  selection_ = ids;
  selectedSet_ = std::unordered_set<doc::ObjectId>(ids.begin(), ids.end());
  syncSlice();
}

bool Viewport::isSelected(const doc::Object& object) const {
  if (selectedSet_.empty()) return false;
  const doc::Document& document = modeler_.document();
  for (const doc::Object* a = &object; a; a = a->parent.empty() ? nullptr : document.find(a->parent)) {
    if (selectedSet_.count(a->id)) return true;
  }
  return false;
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
  const QSize size = viewSize();
  const double w = size.width(), h = size.height();
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

QSize Viewport::viewSize() const {
  // As of the last redraw (the camera's aspect is too); asking the window
  // costs a round trip to the window system (X11), once per projected
  // point: thousands per mouse move when snapping in a large model.
  if (!glSize_.isEmpty()) return glSize_;
  Standard_Integer w = 1, h = 1;
  view_->Window()->Size(w, h);
  return QSize(w, h);
}

snap::ScreenPoint Viewport::project(const gp_Pnt& p) const {
  const Handle(Graphic3d_Camera)& camera = view_->Camera();
  const QSize size = viewSize();
  const double w = size.width(), h = size.height();
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
  snapCursor_ = pos;
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

void Viewport::takeInChanges() {
  // Changes not looked at yet (the slice timer has not run): a slice takes
  // them in before picking, the rest waits as usual.
  if (fullSync_ || modeler_.document().sequence() != syncedSequence_) syncSome(50.0);
}

bool Viewport::freshen(const doc::ObjectId& id) {
  bool behind = false;
  if (pendingSet_.erase(id) > 0) {
    syncObject(id);
    syncLabel(id);
    behind = true;
  }
  return behind;
}

std::optional<FacePick> Viewport::pickFace(const QPoint& pos) {
  if (view_.IsNull() || pickMode_ != PickMode::Faces) return std::nullopt;
  takeInChanges();
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
    // Coplanar with the front-most hit at the cursor: the profile lies on it
    // (the face where it is now, the display may have been moved).
    if (o && o->kind() == doc::ObjectKind::Face &&
        geom::isPlanarFace(TopoDS::Face(owner->Shape().Moved(TopLoc_Location(entries_.at(*id).shown))), &plane) &&
        plane.Distance(selector->PickedPoint(chosen)) < 1.0) {
      chosen = i;
      break;
    }
  }
  if (chosen == 0) return std::nullopt;
  Handle(StdSelect_BRepOwner) owner = Handle(StdSelect_BRepOwner)::DownCast(selector->Picked(chosen));
  const auto id = idOf(Handle(AIS_InteractiveObject)::DownCast(owner->Selectable()));
  if (!id) return std::nullopt;
  if (freshen(*id)) return pickFace(pos);
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
  takeInChanges();
  const QPoint p = devicePos(pos);
  context_->MoveTo(p.x(), p.y(), view_, Standard_False);
  redraw();
  if (!context_->HasDetected()) return std::nullopt;
  const auto id = idOf(context_->DetectedInteractive());
  if (id && freshen(*id)) return pickObject(pos);
  return id;
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
  takeInChanges();
  const QPoint p = devicePos(pos);
  // Detection only: pointing at a face or an edge shows nothing (SketchUp);
  // selected elements are shown by setHighlightedElements.
  if (context_->HasDetected()) {
    context_->ClearDetected(Standard_False);
    redraw();
  }
  const Handle(StdSelect_ViewerSelector3d)& selector = context_->MainSelector();
  selector->Pick(p.x(), p.y(), view_);
  if (selector->NbPicked() < 1) return std::nullopt;
  Handle(StdSelect_BRepOwner) owner = Handle(StdSelect_BRepOwner)::DownCast(selector->Picked(1));
  if (owner.IsNull()) return std::nullopt;
  const auto id = idOf(Handle(AIS_InteractiveObject)::DownCast(owner->Selectable()));
  if (!id) return std::nullopt;
  if (freshen(*id)) return pickElement(pos);
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
    const TopLoc_Location shown(entry.shown);
    if (e.type == TopAbs_FACE && e.index <= entry.displayFaces.Extent()) {
      builder.Add(faces, entry.displayFaces(e.index).Moved(shown));
      anyFace = true;
    } else if (e.type == TopAbs_EDGE && e.index <= entry.displayEdges.Extent()) {
      builder.Add(edges, entry.displayEdges(e.index).Moved(shown));
      anyEdge = true;
    }
  }
  if (anyFace) {
    highlightFaces_ = makeSelectionDots(faces);
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
  syncSlice();
}

void Viewport::syncEditBox() {
  const doc::Document& document = modeler_.document();
  const std::pair<doc::ObjectId, std::uint64_t> key{editContext_, document.sequence()};
  if (key == editBoxKey_) return;
  editBoxKey_ = key;
  if (!editBox_.IsNull()) context_->Remove(editBox_, Standard_False);
  editBox_.Nullify();
  const doc::Object* group = editContext_.empty() ? nullptr : document.find(editContext_);
  if (!group) return;
  // Along the group's own axes, around everything in it, a little apart.
  const gp_Trsf t = document.worldTransform(*group);
  const gp_Ax3 frame(gp_Pnt(0, 0, 0).Transformed(t), gp::DZ().Transformed(t), gp::DX().Transformed(t));
  gp_Trsf toFrame;
  toFrame.SetTransformation(frame);
  Bnd_Box bounds;
  for (const doc::ObjectId& id : document.subtree(editContext_)) {
    const doc::Object* o = document.find(id);
    if (!o || o->shape.IsNull()) continue;
    BRepBndLib::Add(document.worldShape(*o).Moved(TopLoc_Location(toFrame)), bounds);
  }
  if (bounds.IsVoid()) return;
  const double margin = std::max(0.03 * std::sqrt(bounds.SquareExtent()), 10.0);
  bounds.Enlarge(margin);
  double x[2], y[2], z[2];
  bounds.Get(x[0], y[0], z[0], x[1], y[1], z[1]);
  const gp_Trsf fromFrame = toFrame.Inverted();
  auto corner = [&](int i, int j, int k) { return gp_Pnt(x[i], y[j], z[k]).Transformed(fromFrame); };
  TopoDS_Compound edges;
  BRep_Builder b;
  b.MakeCompound(edges);
  for (int u = 0; u < 2; ++u) {
    for (int v = 0; v < 2; ++v) {
      b.Add(edges, BRepBuilderAPI_MakeEdge(corner(0, u, v), corner(1, u, v)).Edge());
      b.Add(edges, BRepBuilderAPI_MakeEdge(corner(u, 0, v), corner(u, 1, v)).Edge());
      b.Add(edges, BRepBuilderAPI_MakeEdge(corner(u, v, 0), corner(u, v, 1)).Edge());
    }
  }
  editBox_ = new AIS_Shape(edges);
  editBox_->SetColor(rgb(0.25, 0.25, 0.25));
  editBox_->Attributes()->SetWireAspect(new Prs3d_LineAspect(rgb(0.25, 0.25, 0.25), Aspect_TOL_DOT, 1.5));
  context_->Display(editBox_, AIS_WireFrame, -1, Standard_False);  // -1: never picked
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

void Viewport::setPreview(const TopoDS_Shape& shape, bool solid, bool unclipped) {
  if (view_.IsNull()) return;
  if (!preview_.IsNull()) context_->Remove(preview_, Standard_False);
  preview_.Nullify();
  if (!shape.IsNull()) {
    preview_ = new AIS_Shape(shape);
    if (unclipped) {
      Handle(Graphic3d_SequenceOfHClipPlane) planes = new Graphic3d_SequenceOfHClipPlane;
      planes->SetOverrideGlobal(true);
      preview_->SetClipPlanes(planes);
      preview_->SetPolygonOffsets(Aspect_POM_Fill, -1.0f, -2.0f);
      preview_->Attributes()->SetFaceBoundaryDraw(true);
    }
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
    builder.Add(compound, scaled ? entry.displayShape.Moved(TopLoc_Location(entry.shown))
                                 : entry.displayShape.Moved(TopLoc_Location(delta * entry.shown)));
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
  // The label: the name of this very result, next to the cursor.
  const QString text = result.valid() ? snapLabel(result.kind) : QString();
  if (text.isEmpty()) {
    hideSnapTip();
  } else {
    if (!snapTip_) {
      // A tooltip window: above the OpenGL view on every platform, drawn by
      // Qt (fonts, DPI), never takes the mouse, the focus or the picking.
      snapTip_ = new QLabel(this, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
                                      Qt::WindowDoesNotAcceptFocus);
      snapTip_->setAttribute(Qt::WA_TransparentForMouseEvents);
      snapTip_->setAttribute(Qt::WA_ShowWithoutActivating);
      snapTip_->setStyleSheet(QStringLiteral(
          "QLabel { background: #fffbe6; color: #202020; border: 1px solid #a89f7a; padding: 1px 5px; }"));
      // Not left floating over other applications.
      connect(qApp, &QGuiApplication::applicationStateChanged, snapTip_, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) hideSnapTip();
      });
    }
    if (snapTip_->text() != text) {
      snapTip_->setText(text);
      snapTip_->adjustSize();
    }
    const QPoint marker = toWidget(result.point);
    std::optional<QPoint> from;
    if (anchor) from = toWidget(*anchor);
    snapTipRect_ = placeSnapTip(snapTip_->size(), marker, from);
    snapTipText_ = text;
    snapTipPoint_ = result.point;
    snapTip_->move(mapToGlobal(snapTipRect_.topLeft()));
    if (!snapTip_->isVisible() && isVisible()) snapTip_->show();
  }
  redraw();
}

void Viewport::hideSnap() { showSnap(snap::SnapResult{}, std::nullopt); }

void Viewport::showGrips(const std::vector<gp_Pnt>& grips, int hot, const gp_Ax3& frame,
                         const TopoDS_Shape& outline) {
  if (view_.IsNull()) return;
  for (const Handle(AIS_Shape)& g : grips_) context_->Remove(g, Standard_False);
  grips_.clear();
  if (!gripOutline_.IsNull()) context_->Remove(gripOutline_, Standard_False);
  gripOutline_.Nullify();
  if (!outline.IsNull()) {
    gripOutline_ = new AIS_Shape(outline);
    gripOutline_->SetColor(rgb(0.95, 0.85, 0.05));
    gripOutline_->SetWidth(2.0);
    gripOutline_->SetZLayer(Graphic3d_ZLayerId_Topmost);
    context_->Display(gripOutline_, AIS_WireFrame, -1, Standard_False);
  }
  if (!grips.empty()) {
    // One cube around the origin, in pixels: zoom persistence keeps its
    // size on screen and places it at each grip.
    const double side = 9.0 * devicePixelRatioF();
    const gp_XYZ half = (frame.XDirection().XYZ() + frame.YDirection().XYZ() + frame.Direction().XYZ()) * (side / 2);
    const TopoDS_Shape cube =
        BRepPrimAPI_MakeBox(gp_Ax2(gp_Pnt(half.Reversed()), frame.Direction(), frame.XDirection()), side, side, side).Shape();
    for (std::size_t i = 0; i < grips.size(); ++i) {
      Handle(AIS_Shape) g = new AIS_Shape(cube);
      g->SetColor(static_cast<int>(i) == hot ? rgb(0.9, 0.15, 0.1) : rgb(0.2, 0.85, 0.2));
      g->Attributes()->SetFaceBoundaryDraw(Standard_True);
      g->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(Quantity_NOC_BLACK, Aspect_TOL_SOLID, 1.0));
      g->SetTransformPersistence(new Graphic3d_TransformPers(Graphic3d_TMF_ZoomPers, grips[i]));
      g->SetZLayer(Graphic3d_ZLayerId_Topmost);
      context_->Display(g, AIS_Shaded, -1, Standard_False);  // -1: never picked
      grips_.push_back(g);
    }
  }
  redraw();
}

void Viewport::hideSnapTip() {
  if (snapTip_) snapTip_->hide();
  snapTipText_.clear();
  snapTipRect_ = QRect();
}

// Beside the cursor, away from the line being drawn, covering neither the
// cursor nor the snapped point nor that line, inside the view.
QRect Viewport::placeSnapTip(const QSize& size, const QPoint& marker, std::optional<QPoint> anchor) const {
  const QPoint c = snapCursor_;
  const int gx = 16, gy = 18;  // clear of the cursor's tip and a crosshair
  std::vector<QRect> candidates = {
      QRect(QPoint(c.x() + gx, c.y() + gy), size),                                  // below right
      QRect(QPoint(c.x() + gx, c.y() - gy - size.height()), size),                  // above right
      QRect(QPoint(c.x() - gx - size.width(), c.y() + gy), size),                   // below left
      QRect(QPoint(c.x() - gx - size.width(), c.y() - gy - size.height()), size)};  // above left
  // Facing away from where the line comes from first.
  if (anchor) {
    const QPointF back(anchor->x() - marker.x(), anchor->y() - marker.y());
    std::stable_sort(candidates.begin(), candidates.end(), [&](const QRect& a, const QRect& b) {
      const QPointF da = QPointF(a.center()) - QPointF(c), db = QPointF(b.center()) - QPointF(c);
      return da.x() * back.x() + da.y() * back.y() < db.x() * back.x() + db.y() * back.y();
    });
  }
  auto blocks = [&](const QRect& r) {
    const QRect around = r.adjusted(-4, -4, 4, 4);
    if (around.contains(c) || around.contains(marker)) return true;
    if (anchor) {
      for (int i = 0; i <= 32; ++i) {
        const double t = i / 32.0;
        const QPoint p(static_cast<int>(std::lround(anchor->x() + t * (marker.x() - anchor->x()))),
                       static_cast<int>(std::lround(anchor->y() + t * (marker.y() - anchor->y()))));
        if (r.contains(p)) return true;
      }
    }
    return false;
  };
  for (const QRect& r : candidates) {
    if (rect().contains(r) && !blocks(r)) return r;
  }
  for (const QRect& r : candidates) {
    if (rect().contains(r)) return r;
  }
  // A view smaller than usual: kept inside it.
  QRect r = candidates.front();
  r.moveLeft(std::clamp(r.left(), 0, std::max(0, width() - size.width())));
  r.moveTop(std::clamp(r.top(), 0, std::max(0, height() - size.height())));
  return r;
}

void Viewport::leaveEvent(QEvent* event) {
  hideSnapTip();
  redraw();
  QWidget::leaveEvent(event);
}

bool Viewport::dumpImage(const QString& path) {
  if (view_.IsNull()) return false;
  if (interactiveRendering()) restoreRenderingQuality();
  renderNow();
  return view_->Dump(path.toUtf8().constData()) == Standard_True;
}

void Viewport::redraw() {
  // Several changes in one event (sync, selection, highlight, snap marker)
  // become one frame: the view is rendered when Qt paints.
  update();
}

void Viewport::beginInteractiveRendering() {
  if (view_.IsNull()) return;
  // Keep exact geometry; only multisampling changes during mouse interaction.
  view_->ChangeRenderingParams().NbMsaaSamples = 0;
  view_->ChangeRenderingParams().IsAntialiasingEnabled = Standard_False;
  interactionQualityTimer_.start();
}

void Viewport::restoreRenderingQuality() {
  interactionQualityTimer_.stop();
  if (view_.IsNull()) return;
  view_->ChangeRenderingParams().NbMsaaSamples = 4;
  view_->ChangeRenderingParams().IsAntialiasingEnabled = Standard_True;
  view_->Invalidate();
  redraw();
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
  updateSkyAndGround();
  view_->Redraw();
}

void Viewport::updateSkyAndGround() {
  const int rows = std::max(glSize_.height(), 2);
  const Handle(Graphic3d_Camera)& camera = view_->Camera();
  // The horizon is where horizontal planes vanish: a point far away in the
  // looking direction, levelled. Above the screen when looking down in a
  // parallel view (all ground), below it when looking up (all sky).
  const gp_Dir d = camera->Direction();
  const gp_Vec level(d.X(), d.Y(), 0.0);
  double fromTop;  // horizon position, 0 = top edge, 1 = bottom edge
  if (level.Magnitude() < 1e-9) {
    fromTop = d.Z() < 0 ? -1.0 : 2.0;
  } else {
    const gp_Pnt distant = camera->Eye().Translated(level.Normalized() * (1e6 * std::max(camera->Distance(), 1.0)));
    const double y = camera->Project(distant).Y();  // normalised device coordinates, +1 = top
    fromTop = std::isfinite(y) ? (1.0 - y) / 2.0 : (d.Z() < 0 ? -1.0 : 2.0);
  }
  const int horizon = static_cast<int>(std::lround(std::clamp(fromTop, -1.0, 2.0) * rows));
  const bool under = camera->Eye().Z() < 0;
  if (horizon == horizonRow_ && rows == backgroundRows_ && under == underGround_ && !backdrop_.IsNull()) return;
  horizonRow_ = horizon;
  backdropWidth_ = std::max(glSize_.width(), 2);
  backgroundRows_ = rows;
  underGround_ = under;
  if (!backdrop_.IsNull()) context_->Remove(backdrop_, Standard_False);
  backdrop_ = new Backdrop(backdropWidth_, rows, horizon, under);
  backdrop_->SetZLayer(Graphic3d_ZLayerId_BotOSD);
  backdrop_->SetInfiniteState(true);  // not part of "fit all"
  backdrop_->SetTransformPersistence(new Graphic3d_TransformPers(Graphic3d_TMF_2d, Aspect_TOTP_LEFT_LOWER));
  context_->Display(backdrop_, 0, -1, Standard_False);
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
