#include "Tools.h"

#include <algorithm>
#include <cmath>

#include <QKeyEvent>
#include <QRegularExpression>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <gp_Ax3.hxx>
#include <IntAna_IntConicQuad.hxx>
#include <Precision.hxx>
#include <gp_Lin.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <TopoDS.hxx>

#include "Cursors.h"
#include "Viewport.h"
#include "sbr/Units.h"
#include "sbr/geom/Annotation.h"
#include "sbr/geom/Freeform.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"

namespace sbr::app {
namespace {

QString mm(double value) { return QString::number(value, 'f', 1); }

std::optional<double> parseOne(const QString& text) {
  return units::parseLength(text.toStdString());
}

}  // namespace

QString snapLabel(snap::SnapKind kind) {
  switch (kind) {
    case snap::SnapKind::Endpoint: return QObject::tr("端点");
    case snap::SnapKind::Midpoint: return QObject::tr("中点");
    case snap::SnapKind::OnEdge: return QObject::tr("エッジ上");
    case snap::SnapKind::OnFace: return QObject::tr("面上");
    case snap::SnapKind::Origin: return QObject::tr("原点");
    case snap::SnapKind::AxisX: return QObject::tr("赤軸 (X) 方向");
    case snap::SnapKind::AxisY: return QObject::tr("緑軸 (Y) 方向");
    case snap::SnapKind::AxisZ: return QObject::tr("青軸 (Z) 方向");
    case snap::SnapKind::Locked: return QObject::tr("方向ロック中");
    case snap::SnapKind::Parallel: return QObject::tr("エッジに平行");
    case snap::SnapKind::Perpendicular: return QObject::tr("エッジに垂直");
    case snap::SnapKind::FromPoint: return QObject::tr("点から");
    case snap::SnapKind::WorkPlane: return QObject::tr("作業平面上");
    case snap::SnapKind::Intersection: return QObject::tr("交点");
    case snap::SnapKind::None: break;
  }
  return {};
}

// --- Select -------------------------------------------------------------------

QString SelectTool::name() const { return QObject::tr("選択"); }
QCursor SelectTool::cursor() const { return toolCursor(CursorKind::Select); }

void SelectTool::activate() {
  host_.viewport().setPickMode(PickMode::Elements);
  host_.setMeasurement(QString(), QString());
  host_.setStatus(QObject::tr("クリック: 面・エッジ / ダブルクリック: 隣接も選択 / トリプルクリック: オブジェクト全体 / "
                              "Shift で追加・解除"));
}

void SelectTool::mouseMove(const ToolEvent& e) {
  if (boxStart_) {
    host_.viewport().setRubberBand(QRect(*boxStart_, e.pos).normalized());
    return;
  } host_.viewport().pickElement(e.pos); }

void SelectTool::apply(const std::vector<ElementRef>& picked, bool additive) {
  std::vector<ElementRef> selection = additive ? host_.elementSelection() : std::vector<ElementRef>{};
  if (additive) {
    // Shift toggles: remove when everything picked is already selected.
    const bool allSelected = std::all_of(picked.begin(), picked.end(), [&](const ElementRef& p) {
      return std::find(selection.begin(), selection.end(), p) != selection.end();
    });
    for (const ElementRef& p : picked) {
      const auto it = std::find(selection.begin(), selection.end(), p);
      if (allSelected && it != selection.end()) {
        selection.erase(it);
      } else if (!allSelected && it == selection.end()) {
        selection.push_back(p);
      }
    }
  } else {
    selection = picked;
  }
  host_.setElementSelection(selection);
}

doc::ObjectId SelectTool::target(const doc::ObjectId& leaf) const {
  const doc::Document& document = host_.modeler().document();
  const doc::ObjectId context = host_.editingGroup();
  if (context.empty()) return document.rootOf(leaf);
  // Inside a group: the child of the edited group that contains the leaf.
  doc::ObjectId current = leaf;
  int guard = 0;
  while (guard++ < 1000) {
    const doc::Object* o = document.find(current);
    if (!o) break;
    if (o->parent == context) return current;
    if (o->parent.empty()) break;
    current = o->parent;
  }
  return {};  // outside the edited group
}

void SelectTool::mousePress(const ToolEvent& e) {
  const bool additive = e.modifiers & Qt::ShiftModifier;
  const auto picked = host_.viewport().pickElement(e.pos);
  if (!picked) {
    // Empty space: a click clears the selection, a drag selects a box.
    boxStart_ = e.pos;
    boxAdditive_ = additive;
    return;
  }
  doc::ObjectId addressed = target(picked->objectId);
  if (addressed.empty()) {
    // Clicked outside the edited group: leave it, as in SketchUp.
    host_.exitGroup();
    addressed = target(picked->objectId);
  }
  const doc::Document& document = host_.modeler().document();
  const doc::Object* owner = document.find(picked->objectId);
  // Groups and components are picked as a whole, like SketchUp.
  if (addressed != picked->objectId || (owner && owner->kind() == doc::ObjectKind::Instance)) {
    std::vector<doc::ObjectId> selection = additive ? host_.selection() : std::vector<doc::ObjectId>{};
    const auto it = std::find(selection.begin(), selection.end(), addressed);
    if (it == selection.end()) {
      selection.push_back(addressed);
    } else {
      selection.erase(it);
    }
    host_.setSelection(selection);
    return;
  }
  // Third click shortly after a double-click: the whole object.
  const bool triple = sinceDoubleClick_.isValid() && sinceDoubleClick_.elapsed() < 450 &&
                      (e.pos - doubleClickPos_).manhattanLength() < 6;
  sinceDoubleClick_.invalidate();
  if (triple) {
    apply(host_.viewport().allElements(picked->objectId), additive);
    return;
  }
  apply({*picked}, additive);
}

void SelectTool::cancel() {
  boxStart_.reset();
  host_.viewport().setRubberBand(std::nullopt);
}

void SelectTool::mouseRelease(const ToolEvent& e) {
  if (!boxStart_) return;
  const QPoint start = *boxStart_;
  boxStart_.reset();
  host_.viewport().setRubberBand(std::nullopt);
  if ((e.pos - start).manhattanLength() < 5) {
    if (!boxAdditive_) host_.setSelection({});
    return;
  }
  boxSelect(QRect(start, e.pos).normalized(), e.pos.x() < start.x(), boxAdditive_);
}

void SelectTool::boxSelect(const QRect& rect, bool crossing, bool additive) {
  const doc::Document& document = host_.modeler().document();
  const doc::ObjectId context = host_.editingGroup();
  std::vector<doc::ObjectId> selection = additive ? host_.selection() : std::vector<doc::ObjectId>{};
  for (const doc::Object& o : document.objects()) {
    if (o.parent != context || !document.isVisible(o.id)) continue;
    // Screen footprint from the exact bounds of the object (and its children).
    Bnd_Box box;
    for (const doc::ObjectId& id : document.subtree(o.id)) {
      const doc::Object* leaf = document.find(id);
      if (!leaf || leaf->shape.IsNull() || !document.isVisible(id)) continue;
      box.Add(geom::exactBounds(document.worldShape(*leaf)));
    }
    if (box.IsVoid()) continue;
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    QRect footprint;
    bool allInside = true;
    for (int i = 0; i < 8; ++i) {
      const QPoint p = host_.viewport().toWidget(gp_Pnt(i & 1 ? x1 : x0, i & 2 ? y1 : y0, i & 4 ? z1 : z0));
      allInside = allInside && rect.contains(p);
      footprint = footprint.isNull() ? QRect(p, QSize(1, 1)) : footprint.united(QRect(p, QSize(1, 1)));
    }
    const bool hit = crossing ? rect.intersects(footprint) : allInside;
    if (hit && std::find(selection.begin(), selection.end(), o.id) == selection.end()) selection.push_back(o.id);
  }
  host_.setSelection(selection);
  host_.setStatus(QObject::tr("%1 個を選択（左→右: 完全に囲んだ物 / 右→左: 触れた物）").arg(selection.size()));
}

bool SelectTool::mouseDoubleClick(const ToolEvent& e) {
  const auto picked = host_.viewport().pickElement(e.pos);
  if (!picked) return false;  // empty space: let the viewport focus/fit
  const doc::Document& document = host_.modeler().document();
  const doc::ObjectId addressed = target(picked->objectId);
  if (addressed.empty()) return false;
  const doc::Object* o = document.find(addressed);
  if (o && o->kind() == doc::ObjectKind::Group) {
    host_.enterGroup(addressed);  // edit inside the group
    return true;
  }
  if (o && o->kind() == doc::ObjectKind::Instance) {
    host_.enterGroup(addressed);  // edit the component's definition in place
    return true;
  }
  if (addressed != picked->objectId) return false;
  sinceDoubleClick_.start();
  doubleClickPos_ = e.pos;
  apply(host_.viewport().withNeighbours(*picked), e.modifiers & Qt::ShiftModifier);
  return true;
}

// --- Drawing base ---------------------------------------------------------------

snap::SnapResult DrawingTool::snapAt(const ToolEvent& e, std::optional<gp_Pnt> anchor,
                                     std::optional<gp_Pln> plane) {
  lastEvent_ = e;
  std::optional<gp_Pnt> sticky;
  if (last_.kind == snap::SnapKind::Endpoint || last_.kind == snap::SnapKind::Midpoint ||
      last_.kind == snap::SnapKind::Origin) {
    sticky = last_.point;
  }
  last_ = host_.viewport().snap(e.pos, anchor, anchor ? locked_ : std::nullopt, plane,
                                trackedPoints_, trackedDirections_, sticky);
  host_.viewport().showSnap(last_, anchor);

  // Remember hovered points / edges for later inference (SketchUp tracking).
  const bool isPoint = last_.kind == snap::SnapKind::Endpoint ||
                       last_.kind == snap::SnapKind::Midpoint ||
                       last_.kind == snap::SnapKind::Origin;
  if (isPoint && (!anchor || anchor->Distance(last_.point) > 1e-6)) {
    const bool known = std::any_of(trackedPoints_.begin(), trackedPoints_.end(),
                                   [&](const gp_Pnt& p) { return p.Distance(last_.point) < 1e-6; });
    if (!known) {
      trackedPoints_.push_back(last_.point);
      if (trackedPoints_.size() > 2) trackedPoints_.erase(trackedPoints_.begin());
    }
  }
  if (last_.kind == snap::SnapKind::OnEdge && last_.edgeDirection) {
    trackedDirections_ = {*last_.edgeDirection};
  }
  return last_;
}

void DrawingTool::resetTracking() {
  trackedPoints_.clear();
  trackedDirections_.clear();
}

bool DrawingTool::keyPress(QKeyEvent* event) {
  if (event->isAutoRepeat()) return false;
  auto toggle = [&](const gp_Dir& d) {
    if (locked_ && locked_->IsParallel(d, 1e-9) && !shiftLock_) {
      locked_.reset();
    } else {
      locked_ = d;
      shiftLock_ = false;
    }
    refresh();
    return true;
  };
  switch (event->key()) {
    case Qt::Key_Shift:
      if (last_.axis) {
        locked_ = *last_.axis;
        shiftLock_ = true;
        refresh();
      }
      return true;
    case Qt::Key_Right: return toggle(gp::DX());
    case Qt::Key_Left: return toggle(gp::DY());
    case Qt::Key_Up: return toggle(gp::DZ());
    case Qt::Key_Down:
      resetLock();
      refresh();
      return true;
    default: return false;
  }
}

bool DrawingTool::keyRelease(QKeyEvent* event) {
  if (event->isAutoRepeat() || event->key() != Qt::Key_Shift) return false;
  if (shiftLock_) {
    resetLock();
    refresh();
  }
  return true;
}

void DrawingTool::resetLock() {
  locked_.reset();
  shiftLock_ = false;
}

void DrawingTool::refresh() {
  if (lastEvent_) mouseMove(*lastEvent_);
}

QString DrawingTool::lockSuffix() const {
  return locked_ ? QObject::tr("（方向ロック中・↓で解除）") : QString();
}

namespace {
// Sketch plane for a shape started at `r`: the face under the cursor when
// the click is on a face (or on one of its edges/corners), else horizontal.
// Why a closed loop could not become a face, for the user (F-04/F-06).
QString loopFailureText(const std::string& error) {
  if (error.find("not planar") != std::string::npos) {
    return QObject::tr("輪郭が同じ平面上にないため面にできません。線はそのまま残しました"
                       "（曲面を張るには曲面メニューのロフトを使います）");
  }
  if (error.find("crosses itself") != std::string::npos) {
    return QObject::tr("輪郭が自分自身と交差しているため面にできません。線はそのまま残しました");
  }
  return QObject::tr("輪郭を面にできませんでした（%1）。線はそのまま残しました").arg(QString::fromStdString(error));
}

// Creates a curve and, if it closes a loop, the face — one undo step (F-08).
template <class Create>
void createCurveAndFace(ToolHost& host, Create create, const char* undoName) {
  doc::Modeler& m = host.modeler();
  m.history().beginGroup(undoName);
  auto curve = create();
  if (!curve) {
    m.history().abortGroup();
    host.reportError(QString::fromStdString(curve.error()));
    return;
  }
  bool loopFound = false;
  auto face = m.faceFromLoop(*curve, &loopFound);
  m.history().endGroup();
  if (face) {
    host.setStatus(QObject::tr("輪郭が閉じたので面を作成しました。そのままプッシュ/プルできます"));
  } else if (loopFound) {
    host.reportError(loopFailureText(face.error()));
  }
}

geom::SketchPlane planeForStart(Viewport& viewport, const QPoint& pos, const snap::SnapResult& r) {
  gp_Dir normal = gp::DZ();
  if (r.kind == snap::SnapKind::OnFace && r.faceNormal) {
    normal = *r.faceNormal;
  } else if (const auto face = viewport.faceAt(pos)) {
    if (std::abs(gp_Vec(face->first, r.point).Dot(gp_Vec(face->second))) < 1e-6) normal = face->second;
  }
  return geom::SketchPlane::fromNormal(r.point, normal);
}
}  // namespace

// --- Line -----------------------------------------------------------------------

QString LineTool::name() const { return QObject::tr("線"); }
QCursor LineTool::cursor() const { return toolCursor(CursorKind::Pencil); }

void LineTool::activate() {
  resetTracking();
  host_.viewport().setPickMode(PickMode::None);
  host_.setMeasurement(QObject::tr("長さ"), QString());
  host_.setStatus(QObject::tr("始点をクリック"));
}

void LineTool::cancel() {
  finish();
  host_.viewport().hideSnap();
}

std::optional<gp_Pnt> LineTool::pointAt(const ToolEvent& e) {
  std::optional<gp_Pnt> anchor;
  std::optional<gp_Pln> plane;
  if (!points_.empty()) {
    anchor = points_.back();
    // Stay on the plane the chain started on (a wall face, the ground, ...).
    plane = drawPlane_ ? *drawPlane_ : gp_Pln(*anchor, gp::DZ());
  }
  snap::SnapResult r = snapAt(e, anchor, plane);
  // Closing the loop: snap to the first point of the chain being drawn.
  if (points_.size() >= 2 && !locked_) {
    const snap::ScreenPoint s = host_.viewport().project(points_.front());
    if (std::hypot(s.x - e.pos.x(), s.y - e.pos.y()) < 10.0) {
      r.kind = snap::SnapKind::Endpoint;
      r.point = points_.front();
      r.axis.reset();
      last_ = r;
      host_.viewport().showSnap(r, anchor);
    }
  }
  if (!r.valid()) return std::nullopt;
  host_.setStatus((points_.empty() ? QObject::tr("始点をクリック")
                                   : QObject::tr("次の点をクリック / 始点で閉じると面を作成 / "
                                                 "ダブルクリック・Escで終了")) +
                  QStringLiteral("  [") + snapLabel(r.kind) + QStringLiteral("]") + lockSuffix());
  return r.point;
}

void LineTool::mouseMove(const ToolEvent& e) {
  current_ = pointAt(e);
  if (!points_.empty() && current_) {
    host_.setMeasurement(QObject::tr("長さ"), mm(points_.back().Distance(*current_)));
    // Say what clicking here will do (P-05: predictable before commit).
    std::vector<gp_Pnt> line = points_;
    line.push_back(*current_);
    if (points_.size() >= 3 && current_->Distance(points_.front()) < 1e-6) {
      host_.setStatus(QObject::tr("クリックで閉じて面にします"));
    } else if (host_.modeler().isLooseCurveEnd(*current_)) {
      host_.setStatus(QObject::tr("クリックで線・円弧とつなぎます（輪郭が閉じれば面になります）"));
    } else if (host_.modeler().splitTarget(line)) {
      host_.setStatus(QObject::tr("クリックで面をこの線で分割します"));
    }
  }
  updatePreview();
}

void LineTool::mousePress(const ToolEvent& e) {
  const auto p = pointAt(e);
  if (!p) return;
  if (points_.empty()) drawPlane_ = planeForStart(host_.viewport(), e.pos, last_).plane();
  addPoint(*p);
}

bool LineTool::mouseDoubleClick(const ToolEvent&) {
  finish();
  return true;
}

void LineTool::addPoint(const gp_Pnt& p) {
  if (!points_.empty() && p.Distance(points_.back()) < tolerance::kMinDimension) return;
  if (points_.size() >= 3 && p.Distance(points_.front()) < 1e-6) {
    auto face = host_.modeler().createPolygon(points_);
    if (!face) {
      std::vector<gp_Pnt> closed = points_;
      closed.push_back(points_.front());
      auto line = host_.modeler().createPolyline(closed);
      host_.reportError(QObject::tr("面を作成できませんでした（%1）。線として保存しました。")
                            .arg(QString::fromStdString(face.error())));
      (void)line;
    }
    points_.clear();
    drawPlane_.reset();
    resetLock();
    updatePreview();
    host_.setStatus(QObject::tr("始点をクリック"));
    return;
  }
  points_.push_back(p);
  // Edge to edge across a face of a solid: the face is split right away
  // (SketchUp-style) and the line ends there.
  if (points_.size() >= 2 && host_.modeler().splitWithLine(points_)) {
    points_.clear();
    current_.reset();
    drawPlane_.reset();
    resetLock();
    host_.viewport().clearPreview();
    host_.setStatus(QObject::tr("面を分割しました。分かれた面をプッシュ/プルできます"));
    return;
  }
  // Reaching the end of another loose line or arc: finish here so the loop
  // can close into a face.
  if (points_.size() >= 2 && host_.modeler().isLooseCurveEnd(p)) {
    finish();
    return;
  }
  resetLock();
  updatePreview();
}

void LineTool::finish() {
  if (points_.size() >= 2) {
    const std::vector<gp_Pnt> pts = points_;
    createCurveAndFace(host_, [&] { return host_.modeler().createPolyline(pts); }, "Line");
  }
  points_.clear();
  current_.reset();
  drawPlane_.reset();
  resetLock();
  host_.viewport().clearPreview();
  host_.setMeasurement(QObject::tr("長さ"), QString());
}

void LineTool::updatePreview() {
  std::vector<gp_Pnt> pts = points_;
  if (current_ && !pts.empty()) pts.push_back(*current_);
  if (pts.size() < 2) {
    host_.viewport().clearPreview();
    return;
  }
  BRepBuilderAPI_MakePolygon polygon;
  for (const gp_Pnt& p : pts) polygon.Add(p);
  host_.viewport().setPreview(polygon.IsDone() ? polygon.Shape() : TopoDS_Shape());
}

void LineTool::applyMeasurement(const QString& text) {
  const auto length = parseOne(text);
  if (!length || *length <= 0) {
    host_.reportError(QObject::tr("長さを解釈できません: %1").arg(text));
    return;
  }
  if (points_.empty() || !current_ || current_->Distance(points_.back()) < 1e-9) {
    host_.reportError(QObject::tr("始点をクリックし、方向を示してから長さを入力してください"));
    return;
  }
  const gp_Dir dir(gp_Vec(points_.back(), *current_));
  addPoint(points_.back().Translated(gp_Vec(dir) * *length));
  current_ = points_.back();
  updatePreview();
}

// --- Curve ----------------------------------------------------------------------

QString CurveTool::name() const { return QObject::tr("曲線"); }
QCursor CurveTool::cursor() const { return toolCursor(CursorKind::Curve); }

void CurveTool::activate() {
  resetTracking();
  host_.viewport().setPickMode(PickMode::None);
  host_.setMeasurement(QString(), QString());
  host_.setStatus(QObject::tr("曲線が通る点を順にクリック / 始点クリックで閉曲線 / ダブルクリック・Enter で確定"));
}

void CurveTool::cancel() {
  points_.clear();
  current_.reset();
  host_.viewport().clearPreview();
  host_.viewport().hideSnap();
}

void CurveTool::mouseMove(const ToolEvent& e) {
  std::optional<gp_Pnt> anchor;
  if (!points_.empty()) anchor = points_.back();
  const snap::SnapResult r =
      snapAt(e, anchor, anchor ? std::optional<gp_Pln>(gp_Pln(*anchor, gp::DZ())) : std::nullopt);
  if (r.valid()) current_ = r.point;
  updatePreview();
}

void CurveTool::mousePress(const ToolEvent& e) {
  mouseMove(e);
  if (!current_) return;
  if (points_.size() >= 3) {
    const snap::ScreenPoint s = host_.viewport().project(points_.front());
    if (std::hypot(s.x - e.pos.x(), s.y - e.pos.y()) < 10.0) {
      finish(true);
      return;
    }
  }
  if (!points_.empty() && current_->Distance(points_.back()) < tolerance::kMinDimension) return;
  points_.push_back(*current_);
  updatePreview();
}

bool CurveTool::mouseDoubleClick(const ToolEvent&) {
  finish(false);
  return true;
}

bool CurveTool::keyPress(QKeyEvent* event) {
  // Enter finishes the curve even when the measurement box is empty.
  if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && !points_.empty()) {
    finish(false);
    return true;
  }
  return DrawingTool::keyPress(event);
}

void CurveTool::applyMeasurement(const QString&) { finish(false); }

void CurveTool::finish(bool closed) {
  if (points_.size() >= 2) {
    auto id = host_.modeler().createCurve(points_, closed);
    if (!id) host_.reportError(QString::fromStdString(id.error()));
  }
  cancel();
  activate();
}

void CurveTool::updatePreview() {
  std::vector<gp_Pnt> pts = points_;
  if (current_ && (pts.empty() || current_->Distance(pts.back()) > tolerance::kMinDimension)) {
    pts.push_back(*current_);
  }
  if (pts.size() < 2) {
    host_.viewport().clearPreview();
    return;
  }
  auto curve = geom::makeInterpolatedCurve(pts, false);
  host_.viewport().setPreview(curve ? TopoDS_Shape(curve.value()) : TopoDS_Shape());
}

// --- Dimension --------------------------------------------------------------------

QString DimensionTool::name() const { return QObject::tr("寸法"); }
QCursor DimensionTool::cursor() const { return toolCursor(CursorKind::Dimension); }

void DimensionTool::activate() {
  resetTracking();
  reset();
  host_.viewport().setPickMode(PickMode::None);
  host_.setMeasurement(QObject::tr("寸法"), QString());
  host_.setStatus(QObject::tr("測る始点をクリック（直後に文字を入力すると寸法の文字を置き換え）"));
}

void DimensionTool::reset() {
  stage_ = Stage::Start;
  offset_ = gp_Vec();
  resetLock();
  host_.viewport().clearPreview();
}

void DimensionTool::cancel() {
  reset();
  host_.viewport().hideSnap();
}

// Horizontal segments are dimensioned in their horizontal plane (plans);
// others in the plane through the segment that faces the camera most.
gp_Pln DimensionTool::offsetPlane() const {
  const gp_Vec chord(start_, end_);
  if (std::abs(chord.Normalized().Z()) < 1e-6) return gp_Pln(start_, gp::DZ());
  gp_Vec normal = chord.Crossed(gp_Vec(host_.viewport().viewDirection()).Crossed(chord));
  if (normal.Magnitude() < 1e-9) normal = chord.Crossed(gp_Vec(1, 0, 0));
  return gp_Pln(start_, gp_Dir(normal));
}

void DimensionTool::mouseMove(const ToolEvent& e) {
  lastEvent_ = e;
  switch (stage_) {
    case Stage::Start: {
      const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
      host_.setStatus(QObject::tr("測る始点をクリック  [%1]").arg(snapLabel(r.kind)));
      return;
    }
    case Stage::End: {
      const snap::SnapResult r = snapAt(e, start_, std::nullopt);
      if (!r.valid()) return;
      end_ = r.point;
      host_.setMeasurement(QObject::tr("長さ"), mm(start_.Distance(end_)));
      host_.viewport().setPreview(start_.Distance(end_) > tolerance::kMinDimension
                                      ? TopoDS_Shape(BRepBuilderAPI_MakeEdge(start_, end_).Edge())
                                      : TopoDS_Shape());
      host_.setStatus(QObject::tr("終点をクリック  [%1]").arg(snapLabel(r.kind)));
      return;
    }
    case Stage::Offset: {
      const snap::SnapResult r = snapAt(e, std::nullopt, offsetPlane());
      if (!r.valid()) return;
      const gp_Dir dir(gp_Vec(start_, end_));
      gp_Vec v(start_, r.point);
      offset_ = v - gp_Vec(dir) * v.Dot(gp_Vec(dir));
      host_.setMeasurement(QObject::tr("離れ"), mm(offset_.Magnitude()));
      auto shape = geom::makeDimensionShape(start_, end_, offset_);
      host_.viewport().setPreview(shape ? shape.value() : TopoDS_Shape());
      host_.setStatus(QObject::tr("寸法線の位置をクリック、または離れを入力"));
      return;
    }
  }
}

void DimensionTool::mousePress(const ToolEvent& e) {
  switch (stage_) {
    case Stage::Start: {
      const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
      if (!r.valid()) return;
      start_ = r.point;
      stage_ = Stage::End;
      lastCreated_.reset();
      return;
    }
    case Stage::End:
      mouseMove(e);
      if (start_.Distance(end_) < tolerance::kMinDimension) return;
      stage_ = Stage::Offset;
      resetLock();
      return;
    case Stage::Offset:
      mouseMove(e);
      commit();
      return;
  }
}

bool DimensionTool::stepBack() {
  if (stage_ != Stage::Offset) return false;
  stage_ = Stage::End;
  offset_ = gp_Vec();
  if (lastEvent_) mouseMove(*lastEvent_);
  return true;
}

void DimensionTool::commit() {
  auto id = host_.modeler().createDimension(start_, end_, offset_);
  if (!id) {
    host_.reportError(QString::fromStdString(id.error()));
  } else {
    lastCreated_ = id.value();
  }
  reset();
  host_.setStatus(QObject::tr("測る始点をクリック（直後に文字を入力すると寸法の文字を置き換え）"));
}

void DimensionTool::applyMeasurement(const QString& text) {
  if (stage_ == Stage::Offset) {
    const auto value = units::parseLength(text.toStdString());
    if (!value || *value < 0) {
      host_.reportError(QObject::tr("長さを解釈できません: %1").arg(text));
      return;
    }
    if (offset_.Magnitude() < tolerance::kMinDimension) {
      host_.reportError(QObject::tr("マウスで寸法線を出す側を示してから入力してください"));
      return;
    }
    offset_ = offset_.Normalized() * *value;
    commit();
    return;
  }
  if (stage_ == Stage::Start && lastCreated_ && host_.modeler().document().find(*lastCreated_)) {
    // "-" puts the measured value back.
    const std::string replacement = text == QStringLiteral("-") ? std::string() : text.toStdString();
    const Status s = host_.modeler().setAnnotationText(*lastCreated_, replacement);
    if (!s) host_.reportError(QString::fromStdString(s.message()));
    return;
  }
  host_.reportError(QObject::tr("寸法を描いてから入力してください"));
}

// --- Text -----------------------------------------------------------------------------

QString TextTool::name() const { return QObject::tr("テキスト"); }
QCursor TextTool::cursor() const { return toolCursor(CursorKind::Text); }

void TextTool::activate() {
  resetTracking();
  reset();
  host_.viewport().setPickMode(PickMode::None);
  host_.setMeasurement(QObject::tr("テキスト"), QString());
  host_.setStatus(QObject::tr("指し示す点か面をクリック（面なら面積が入ります）"));
}

void TextTool::reset() {
  hasTarget_ = false;
  plane_.reset();
  resetLock();
  host_.viewport().clearPreview();
}

void TextTool::cancel() {
  reset();
  host_.viewport().hideSnap();
}

void TextTool::mouseMove(const ToolEvent& e) {
  lastEvent_ = e;
  if (!hasTarget_) {
    const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
    host_.setStatus(QObject::tr("指し示す点か面をクリック  [%1]").arg(snapLabel(r.kind)));
    return;
  }
  const snap::SnapResult r = snapAt(e, std::nullopt, plane_);
  if (!r.valid()) return;
  const bool leader = (e.pos - targetPos_).manhattanLength() > 4;
  host_.viewport().setPreview(leader && r.point.Distance(target_) > tolerance::kMinDimension
                                  ? TopoDS_Shape(BRepBuilderAPI_MakeEdge(target_, r.point).Edge())
                                  : TopoDS_Shape());
  host_.setStatus(QObject::tr("文字の位置をクリック（同じ場所なら引き出し線なし）"));
}

void TextTool::mousePress(const ToolEvent& e) {
  if (!hasTarget_) {
    const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
    if (!r.valid()) return;
    target_ = r.point;
    targetPos_ = e.pos;
    defaultText_ = QObject::tr("テキスト");
    // A face: its area is the useful default (room labels).
    if (r.kind == snap::SnapKind::OnFace && !r.face.IsNull()) {
      defaultText_ = QStringLiteral("%1 m²").arg(geom::area(r.face) / 1e6, 0, 'f', 2);
    }
    // The text stays in the plane through the target facing the camera.
    plane_ = gp_Pln(target_, -host_.viewport().viewDirection());
    hasTarget_ = true;
    lastCreated_.reset();
    return;
  }
  const snap::SnapResult r = snapAt(e, std::nullopt, plane_);
  const bool leader = r.valid() && (e.pos - targetPos_).manhattanLength() > 4;
  const gp_Pnt position = leader ? r.point : target_;
  auto id = host_.modeler().createText(position, leader ? std::optional<gp_Pnt>(target_) : std::nullopt,
                                       defaultText_.toStdString());
  reset();
  if (!id) {
    host_.reportError(QString::fromStdString(id.error()));
    return;
  }
  lastCreated_ = id.value();
  host_.setStatus(QObject::tr("文字を入力して Enter で置き換え（次の注記はクリック）"));
  host_.beginTextEntry(QObject::tr("テキスト"), defaultText_);
}

void TextTool::applyMeasurement(const QString& text) {
  if (!lastCreated_ || !host_.modeler().document().find(*lastCreated_)) {
    host_.reportError(QObject::tr("先に注記を置いてください"));
    return;
  }
  const Status s = host_.modeler().setAnnotationText(*lastCreated_, text.toStdString());
  if (!s) host_.reportError(QString::fromStdString(s.message()));
}

// --- Section ----------------------------------------------------------------------

QString SectionTool::name() const { return QObject::tr("断面"); }
QCursor SectionTool::cursor() const { return toolCursor(CursorKind::Section); }

void SectionTool::activate() {
  cancel();
  host_.viewport().setPickMode(PickMode::None);
  host_.setMeasurement(QObject::tr("奥行き"), QString());
  host_.setStatus(QObject::tr("切りたい向きの面をクリック（例: 外壁→立面の断面、屋根→平面図）"));
}

void SectionTool::cancel() {
  placing_ = false;
  depth_ = 0.0;
  host_.viewport().clearPreview();
}

void SectionTool::preview(double depth) {
  const gp_Pnt at = origin_.Translated(-gp_Vec(normal_) * depth);
  // A large square showing where the cut goes.
  const double half = 20000.0;
  const gp_Ax3 frame(at, normal_);
  const TopoDS_Face square = BRepBuilderAPI_MakeFace(gp_Pln(frame), -half, half, -half, half).Face();
  host_.viewport().setPreview(BRepTools::OuterWire(square));
}

void SectionTool::mouseMove(const ToolEvent& e) {
  if (!placing_) {
    const auto hit = host_.viewport().faceAt(e.pos);
    host_.setStatus(hit ? QObject::tr("この面と平行に切ります。クリックして奥行きを決める")
                        : QObject::tr("切りたい向きの面をクリック（例: 外壁→立面の断面、屋根→平面図）"));
    return;
  }
  const gp_Pnt p = snap::closestPointOnLineToRay(origin_, normal_, host_.viewport().rayAt(e.pos));
  depth_ = gp_Vec(p, origin_).Dot(gp_Vec(normal_));
  host_.setMeasurement(QObject::tr("奥行き"), mm(depth_));
  preview(depth_);
}

void SectionTool::mousePress(const ToolEvent& e) {
  if (!placing_) {
    const auto hit = host_.viewport().faceAt(e.pos);
    if (!hit) return;
    origin_ = hit->first;
    normal_ = hit->second;
    placing_ = true;
    depth_ = 0.0;
    preview(0.0);
    host_.setStatus(QObject::tr("奥へ動かしてクリック、または奥行きを入力（例: 1200）"));
    return;
  }
  mouseMove(e);
  commit(depth_);
}

void SectionTool::commit(double depth) {
  doc::SectionPlane plane;
  const gp_Pnt at = origin_.Translated(-gp_Vec(normal_) * depth);
  plane.origin = {at.X(), at.Y(), at.Z()};
  plane.normal = {normal_.X(), normal_.Y(), normal_.Z()};
  host_.modeler().setSection(plane);
  cancel();
  host_.setStatus(QObject::tr("断面を作成しました（表示メニュー > 断面 で表示切替・解除）"));
}

void SectionTool::applyMeasurement(const QString& text) {
  const auto value = units::parseLength(text.toStdString());
  if (!value) {
    host_.reportError(QObject::tr("奥行きを解釈できません: %1").arg(text));
    return;
  }
  if (!placing_) {
    host_.reportError(QObject::tr("先に面をクリックしてください"));
    return;
  }
  commit(*value);
}

// --- Arc --------------------------------------------------------------------------

QString ArcTool::name() const { return QObject::tr("円弧"); }
QCursor ArcTool::cursor() const { return toolCursor(CursorKind::Arc); }

void ArcTool::activate() {
  resetTracking();
  reset();
  host_.viewport().setPickMode(PickMode::None);
  updateStatus();
}

void ArcTool::reset() {
  stage_ = Stage::Start;
  bulge_ = 0.0;
  semicircle_ = false;
  resetLock();
  host_.viewport().clearPreview();
}

void ArcTool::cancel() {
  reset();
  host_.viewport().hideSnap();
}

void ArcTool::updateStatus(const QString& snap) {
  const QString mode = face_ ? QObject::tr("弓形の面") : QObject::tr("円弧の線");
  const QString suffix = snap.isEmpty() ? QString() : QStringLiteral("  [%1]").arg(snap);
  switch (stage_) {
    case Stage::Start:
      host_.setMeasurement(QObject::tr("膨らみ"), QString());
      host_.setStatus(QObject::tr("円弧の始点をクリック（%1 / Ctrl で切替。数値で直前の円弧の膨らみを変更）%2")
                          .arg(mode, suffix));
      break;
    case Stage::End:
      host_.setStatus(QObject::tr("終点をクリック、または弦の長さを入力%1").arg(suffix));
      break;
    case Stage::Bulge:
      host_.setStatus(QObject::tr("膨らみをクリック、または数値を入力（%1 / Ctrl で切替）%2")
                          .arg(mode, semicircle_ ? QObject::tr("  [半円]") : suffix));
      break;
  }
}

bool ArcTool::stepBack() {
  if (stage_ != Stage::Bulge) return false;
  stage_ = Stage::End;  // keep the start, pick the end again
  bulge_ = 0.0;
  if (lastEvent_) mouseMove(*lastEvent_);
  updateStatus();
  return true;
}

bool ArcTool::keyPress(QKeyEvent* event) {
  if (event->key() == Qt::Key_Control && !event->isAutoRepeat()) {
    face_ = !face_;
    if (lastEvent_) mouseMove(*lastEvent_);
    updateStatus();
    return true;
  }
  return DrawingTool::keyPress(event);
}

void ArcTool::mouseMove(const ToolEvent& e) {
  lastEvent_ = e;
  switch (stage_) {
    case Stage::Start: {
      const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
      updateStatus(snapLabel(r.kind));
      return;
    }
    case Stage::End: {
      const snap::SnapResult r = snapAt(e, start_, plane_.plane());
      if (!r.valid()) return;
      end_ = r.point;
      host_.setMeasurement(QObject::tr("弦の長さ"), mm(start_.Distance(end_)));
      host_.viewport().setPreview(start_.Distance(end_) > tolerance::kMinDimension
                                      ? TopoDS_Shape(BRepBuilderAPI_MakeEdge(start_, end_).Edge())
                                      : TopoDS_Shape());
      updateStatus(snapLabel(r.kind));
      return;
    }
    case Stage::Bulge: {
      snap::SnapResult r = snapAt(e, std::nullopt, plane_.plane());
      if (!r.valid()) return;
      // Only points on the sketch plane can define the bulge: an inference
      // off the plane (e.g. a vertical axis through a tracked point) would
      // project to a meaningless value, so use the cursor on the plane.
      if (plane_.plane().Distance(r.point) > 1e-3) {
        const snap::Ray ray = host_.viewport().rayAt(e.pos);
        IntAna_IntConicQuad hit(gp_Lin(ray.origin, ray.direction), plane_.plane(), Precision::Angular());
        if (!hit.IsDone() || hit.NbPoints() < 1) return;
        r.point = hit.Point(1);
        r.kind = snap::SnapKind::WorkPlane;
        host_.viewport().hideSnap();
      }
      const gp_Vec chord(start_, end_);
      const gp_Vec side = gp_Vec(plane_.normal).Crossed(chord).Normalized();
      const gp_Pnt mid((start_.XYZ() + end_.XYZ()) / 2.0);
      // Project onto the sketch plane before measuring the bulge.
      gp_Vec offset(mid, r.point);
      offset -= gp_Vec(plane_.normal) * offset.Dot(gp_Vec(plane_.normal));
      bulge_ = offset.Dot(side);
      const double half = chord.Magnitude() / 2.0;
      semicircle_ = std::abs(std::abs(bulge_) - half) < 0.04 * half;
      if (semicircle_) bulge_ = std::copysign(half, bulge_);
      host_.setMeasurement(QObject::tr("膨らみ"), mm(std::abs(bulge_)));
      auto arc = geom::makeArc(start_, end_, plane_.normal, bulge_, face_);
      host_.viewport().setPreview(arc ? arc.value() : TopoDS_Shape(), face_);
      updateStatus(snapLabel(r.kind));
      return;
    }
  }
}

void ArcTool::mousePress(const ToolEvent& e) {
  switch (stage_) {
    case Stage::Start: {
      const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
      if (!r.valid()) return;
      plane_ = planeForStart(host_.viewport(), e.pos, r);
      start_ = r.point;
      stage_ = Stage::End;
      lastCreated_.reset();
      updateStatus();
      return;
    }
    case Stage::End:
      mouseMove(e);
      if (start_.Distance(end_) < tolerance::kMinDimension) return;
      stage_ = Stage::Bulge;
      resetLock();
      updateStatus();
      return;
    case Stage::Bulge:
      mouseMove(e);
      commit(bulge_);
      return;
  }
}

void ArcTool::commit(double bulge) {
  if (std::abs(bulge) < tolerance::kMinDimension) return;
  if (!face_) {
    // An open arc closing a loop with lines becomes part of a face.
    std::optional<doc::ObjectId> arc;
    createCurveAndFace(host_, [&] {
      auto id = host_.modeler().createArc(start_, end_, plane_.normal, bulge, false);
      if (id) arc = id.value();
      return id;
    }, "Arc");
    lastCreated_ = arc && host_.modeler().document().find(*arc) ? arc : std::nullopt;
  } else {
    auto id = host_.modeler().createArc(start_, end_, plane_.normal, bulge, true);
    if (!id) {
      host_.reportError(QString::fromStdString(id.error()));
    } else {
      lastCreated_ = id.value();
    }
  }
  reset();
  updateStatus();
}

void ArcTool::applyMeasurement(const QString& text) {
  const auto value = units::parseLength(text.toStdString());
  if (!value || *value <= 0) {
    host_.reportError(QObject::tr("長さを解釈できません: %1").arg(text));
    return;
  }
  switch (stage_) {
    case Stage::End: {
      // Chord length along the current direction.
      const gp_Vec dir(start_, end_);
      if (dir.Magnitude() < tolerance::kMinDimension) {
        host_.reportError(QObject::tr("マウスで方向を示してから長さを入力してください"));
        return;
      }
      end_ = start_.Translated(dir.Normalized() * *value);
      stage_ = Stage::Bulge;
      resetLock();
      updateStatus();
      return;
    }
    case Stage::Bulge:
      // The typed value keeps the side the mouse is on.
      commit(std::copysign(*value, bulge_ == 0.0 ? 1.0 : bulge_));
      return;
    case Stage::Start:
      if (lastCreated_ && host_.modeler().document().find(*lastCreated_)) {
        const auto* o = host_.modeler().document().find(*lastCreated_);
        const auto* arc = std::get_if<doc::ArcCurve>(&o->recipe.base);
        const double sign = arc && arc->bulge < 0 ? -1.0 : 1.0;
        const Status s = host_.modeler().setParameter(*lastCreated_, "bulge", sign * *value);
        if (!s) host_.reportError(QString::fromStdString(s.message()));
        return;
      }
      host_.reportError(QObject::tr("円弧の始点をクリックしてから入力してください"));
      return;
  }
}

// --- Circle ---------------------------------------------------------------------


QString CircleTool::name() const { return QObject::tr("円"); }
QCursor CircleTool::cursor() const { return toolCursor(CursorKind::Circle); }

void CircleTool::activate() {
  resetTracking();
  reset();
  host_.viewport().setPickMode(PickMode::None);
  host_.setMeasurement(QObject::tr("半径"), QString());
  host_.setStatus(QObject::tr("円の中心をクリック（数値を入力すると直前の円の半径を変更）"));
}

void CircleTool::reset() {
  hasCentre_ = false;
  radius_ = 0.0;
  resetLock();
  host_.viewport().clearPreview();
}

void CircleTool::cancel() {
  reset();
  host_.viewport().hideSnap();
}

void CircleTool::mouseMove(const ToolEvent& e) {
  if (!hasCentre_) {
    const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
    host_.setStatus(QObject::tr("円の中心をクリック  [%1]").arg(snapLabel(r.kind)));
    return;
  }
  const snap::SnapResult r = snapAt(e, plane_.origin, plane_.plane());
  if (!r.valid()) return;
  double u = 0, v = 0;
  plane_.toLocal(r.point, u, v);
  radius_ = std::hypot(u, v);
  host_.setMeasurement(QObject::tr("半径"), mm(radius_));
  host_.setStatus(QObject::tr("半径の位置をクリック、または半径を入力  [%1]").arg(snapLabel(r.kind)));
  auto face = geom::makeCircleFace(plane_, radius_);
  host_.viewport().setPreview(face ? TopoDS_Shape(BRepTools::OuterWire(face.value())) : TopoDS_Shape());
}

void CircleTool::mousePress(const ToolEvent& e) {
  if (!hasCentre_) {
    const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
    if (!r.valid()) return;
    plane_ = planeForStart(host_.viewport(), e.pos, r);
    hasCentre_ = true;
    lastCreated_.reset();
    return;
  }
  mouseMove(e);
  commit(radius_);
}

void CircleTool::commit(double radius) {
  if (radius < tolerance::kMinDimension) return;
  auto id = host_.modeler().createCircle(plane_, radius);
  if (!id) {
    host_.reportError(QString::fromStdString(id.error()));
  } else {
    lastCreated_ = id.value();
  }
  reset();
  host_.setStatus(QObject::tr("円の中心をクリック（数値を入力すると直前の円の半径を変更）"));
}

void CircleTool::applyMeasurement(const QString& text) {
  const auto value = units::parseLength(text.toStdString());
  if (!value || *value <= 0) {
    host_.reportError(QObject::tr("半径を解釈できません: %1").arg(text));
    return;
  }
  if (hasCentre_) {
    commit(*value);
    return;
  }
  if (lastCreated_ && host_.modeler().document().find(*lastCreated_)) {
    const Status s = host_.modeler().setParameter(*lastCreated_, "radius", *value);
    if (!s) host_.reportError(QString::fromStdString(s.message()));
    return;
  }
  host_.reportError(QObject::tr("円の中心をクリックしてから半径を入力してください"));
}

// --- Polygon --------------------------------------------------------------------------

QString PolygonTool::name() const { return QObject::tr("多角形"); }
QCursor PolygonTool::cursor() const { return toolCursor(CursorKind::Polygon); }

QString PolygonTool::prompt() const {
  return QObject::tr("%1 角形の中心をクリック（「8s」で辺の数、数値で直前の多角形の半径を変更）").arg(sides_);
}

void PolygonTool::activate() {
  resetTracking();
  reset();
  host_.viewport().setPickMode(PickMode::None);
  host_.setMeasurement(QObject::tr("半径"), QString());
  host_.setStatus(prompt());
}

void PolygonTool::reset() {
  hasCentre_ = false;
  radius_ = 0.0;
  resetLock();
  host_.viewport().clearPreview();
}

void PolygonTool::cancel() {
  reset();
  host_.viewport().hideSnap();
}

void PolygonTool::mouseMove(const ToolEvent& e) {
  if (!hasCentre_) {
    const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
    host_.setStatus(prompt() + QStringLiteral("  [") + snapLabel(r.kind) + QStringLiteral("]"));
    return;
  }
  const snap::SnapResult r = snapAt(e, plane_.origin, plane_.plane());
  if (!r.valid()) return;
  gp_Vec v(plane_.origin, r.point);
  v -= gp_Vec(plane_.normal) * v.Dot(gp_Vec(plane_.normal));
  radius_ = v.Magnitude();
  if (radius_ > tolerance::kMinDimension) plane_.xDir = gp_Dir(v);  // first corner under the mouse
  host_.setMeasurement(QObject::tr("半径"), mm(radius_));
  host_.setStatus(QObject::tr("角の位置をクリック、または半径を入力  [%1]").arg(snapLabel(r.kind)));
  std::vector<gp_Pnt> corners;
  const gp_Vec x(plane_.xDir), y(plane_.yDir());
  for (int k = 0; k < sides_; ++k) {
    const double a = 2.0 * M_PI * k / sides_;
    corners.push_back(plane_.origin.Translated(radius_ * (std::cos(a) * x + std::sin(a) * y)));
  }
  auto face = radius_ > tolerance::kMinDimension ? geom::makePolygonFace(corners) : Result<TopoDS_Face>::failure("");
  host_.viewport().setPreview(face ? TopoDS_Shape(BRepTools::OuterWire(face.value())) : TopoDS_Shape());
}

void PolygonTool::mousePress(const ToolEvent& e) {
  if (!hasCentre_) {
    const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
    if (!r.valid()) return;
    plane_ = planeForStart(host_.viewport(), e.pos, r);
    hasCentre_ = true;
    lastCreated_.reset();
    return;
  }
  mouseMove(e);
  commit(radius_);
}

void PolygonTool::commit(double radius) {
  if (radius < tolerance::kMinDimension) return;
  auto id = host_.modeler().createRegularPolygon(plane_, radius, sides_);
  if (!id) {
    host_.reportError(QString::fromStdString(id.error()));
  } else {
    lastCreated_ = id.value();
  }
  reset();
  host_.setStatus(prompt());
}

void PolygonTool::applyMeasurement(const QString& text) {
  const QString t = text.trimmed();
  if (t.endsWith(QLatin1Char('s'), Qt::CaseInsensitive)) {
    bool ok = false;
    const int n = t.chopped(1).trimmed().toInt(&ok);
    if (!ok || n < 3 || n > 256) {
      host_.reportError(QObject::tr("辺の数は 3〜256 で入力してください（例: 8s）"));
      return;
    }
    sides_ = n;
    if (!hasCentre_ && lastCreated_ && host_.modeler().document().find(*lastCreated_)) {
      const Status s = host_.modeler().setParameter(*lastCreated_, "sides", n);
      if (!s) host_.reportError(QString::fromStdString(s.message()));
    }
    host_.setStatus(prompt());
    return;
  }
  const auto value = units::parseLength(t.toStdString());
  if (!value || *value <= 0) {
    host_.reportError(QObject::tr("半径を解釈できません: %1").arg(text));
    return;
  }
  if (hasCentre_) {
    commit(*value);
    return;
  }
  if (lastCreated_ && host_.modeler().document().find(*lastCreated_)) {
    const Status s = host_.modeler().setParameter(*lastCreated_, "radius", *value);
    if (!s) host_.reportError(QString::fromStdString(s.message()));
    return;
  }
  host_.reportError(QObject::tr("多角形の中心をクリックしてから半径を入力してください"));
}

// --- Rectangle --------------------------------------------------------------------

QString RectangleTool::name() const { return QObject::tr("矩形"); }
QCursor RectangleTool::cursor() const { return toolCursor(CursorKind::Rectangle); }

void RectangleTool::activate() {
  resetTracking();
  host_.viewport().setPickMode(PickMode::None);
  host_.setMeasurement(QObject::tr("寸法 (幅,奥行)"), QString());
  host_.setStatus(QObject::tr("最初の角をクリック"));
}

void RectangleTool::cancel() {
  reset();
  host_.viewport().hideSnap();
}

void RectangleTool::reset() {
  hasCorner_ = false;
  u_ = v_ = 0.0;
  resetLock();
  host_.viewport().clearPreview();
}

void RectangleTool::mouseMove(const ToolEvent& e) {
  if (!hasCorner_) {
    const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
    host_.setStatus(QObject::tr("最初の角をクリック  [%1]").arg(snapLabel(r.kind)));
    return;
  }
  const snap::SnapResult r = snapAt(e, plane_.origin, plane_.plane());
  if (!r.valid()) return;
  plane_.toLocal(r.point, u_, v_);
  host_.setMeasurement(QObject::tr("寸法 (幅,奥行)"), mm(std::abs(u_)) + QStringLiteral(", ") +
                                                        mm(std::abs(v_)));
  host_.setStatus(QObject::tr("対角の点をクリック、または寸法を入力して Enter  [%1]")
                      .arg(snapLabel(r.kind)) + lockSuffix());
  auto face = geom::makeRectangleFace(plane_, u_, v_);
  host_.viewport().setPreview(face ? TopoDS_Shape(face.value()) : TopoDS_Shape());
}

void RectangleTool::mousePress(const ToolEvent& e) {
  if (!hasCorner_) {
    const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
    if (!r.valid()) return;
    plane_ = planeForStart(host_.viewport(), e.pos, r);
    hasCorner_ = true;
    lastCreated_.reset();
    return;
  }
  mouseMove(e);
  commit(u_, v_);
}

void RectangleTool::commit(double u, double v) {
  if (std::abs(u) < tolerance::kMinDimension || std::abs(v) < tolerance::kMinDimension) return;
  auto id = host_.modeler().createRectangle(plane_, u, v);
  if (!id) {
    host_.reportError(QString::fromStdString(id.error()));
  } else {
    lastCreated_ = id.value();
  }
  reset();
  host_.setStatus(QObject::tr("最初の角をクリック（寸法を入力すると直前の矩形を変更）"));
}

void RectangleTool::applyMeasurement(const QString& text) {
  const auto values = units::parseLengthList(text.toStdString());
  if (!values || values->size() != 2) {
    host_.reportError(QObject::tr("「幅,奥行」の形式で入力してください: %1").arg(text));
    return;
  }
  const double w = std::abs((*values)[0]);
  const double h = std::abs((*values)[1]);
  if (hasCorner_) {
    commit(u_ < 0 ? -w : w, v_ < 0 ? -h : h);
    return;
  }
  // Re-edit the rectangle that was just drawn (parametric, keeps direction).
  if (lastCreated_) {
    const doc::Object* o = host_.modeler().document().find(*lastCreated_);
    const auto* r = o ? std::get_if<doc::RectangleProfile>(&o->recipe.base) : nullptr;
    if (r) {
      const double newW = r->width < 0 ? -w : w;
      const double newH = r->height < 0 ? -h : h;
      Status s = host_.modeler().setParameter(*lastCreated_, "width", newW);
      if (s) s = host_.modeler().setParameter(*lastCreated_, "height", newH);
      if (!s) host_.reportError(QString::fromStdString(s.message()));
      return;
    }
  }
  host_.reportError(QObject::tr("最初の角をクリックしてから寸法を入力してください"));
}

// --- Push/Pull --------------------------------------------------------------------

QString PushPullTool::name() const { return QObject::tr("プッシュ/プル"); }
QCursor PushPullTool::cursor() const { return toolCursor(CursorKind::PushPull); }

void PushPullTool::activate() {
  host_.viewport().setPickMode(PickMode::Faces);
  host_.setMeasurement(QObject::tr("距離"), QString());
  host_.setStatus(QObject::tr("押し出す面をクリック（数値入力で直前の操作を変更）"));
}

void PushPullTool::cancel() {
  reset();
}

void PushPullTool::reset() {
  dragging_ = false;
  distance_ = 0.0;
  face_.Nullify();
  host_.viewport().clearPreview();
  host_.viewport().hideSnap();
  host_.viewport().setPickMode(PickMode::Faces);
}

void PushPullTool::mouseMove(const ToolEvent& e) {
  Viewport& vp = host_.viewport();
  if (!dragging_) {
    const auto pick = vp.pickFace(e.pos);
    host_.setStatus(pick ? QObject::tr("クリックして押し出し開始")
                         : QObject::tr("押し出す面をクリック（数値入力で直前の操作を変更）"));
    return;
  }
  // Distance along the face normal, optionally inferred from geometry.
  const snap::SnapResult s = vp.snap(e.pos, std::nullopt, std::nullopt, std::nullopt);
  gp_Pnt target;
  // Geometry points only count when they actually give a height (points on
  // the face's own plane would snap the distance to zero).
  const bool pointSnap =
      (s.kind == snap::SnapKind::Endpoint || s.kind == snap::SnapKind::Midpoint ||
       s.kind == snap::SnapKind::OnEdge) &&
      std::abs(gp_Vec(origin_, s.point).Dot(gp_Vec(normal_))) > tolerance::kMinDimension;
  if (pointSnap) {
    target = s.point;
    vp.showSnap(s, std::nullopt);
  } else {
    target = snap::closestPointOnLineToRay(origin_, normal_, vp.rayAt(e.pos));
    vp.hideSnap();
  }
  distance_ = gp_Vec(origin_, target).Dot(gp_Vec(normal_));
  host_.setMeasurement(QObject::tr("距離"), mm(distance_));
  host_.setStatus(QObject::tr("クリックで確定、または距離を入力して Enter") +
                  (pointSnap ? QStringLiteral("  [") + snapLabel(s.kind) + QStringLiteral("]")
                             : QString()));
  if (std::abs(distance_) > tolerance::kMinDimension && curved_) {
    // Preview the whole solid with the face moved.
    const doc::Object* o = host_.modeler().document().find(objectId_);
    auto moved = o ? geom::offsetSolidFace(host_.modeler().document().worldShape(*o), face_, distance_)
                   : Result<TopoDS_Shape>::failure("");
    vp.setPreview(moved ? moved.value() : TopoDS_Shape(), true);
  } else if (std::abs(distance_) > tolerance::kMinDimension) {
    BRepPrimAPI_MakePrism prism(face_, gp_Vec(normal_) * distance_, Standard_True);
    vp.setPreview(prism.IsDone() ? prism.Shape() : TopoDS_Shape(), true);
  } else {
    vp.clearPreview();
  }
}

void PushPullTool::mousePress(const ToolEvent& e) {
  if (dragging_) {
    mouseMove(e);
    commit(distance_);
    return;
  }
  const auto pick = host_.viewport().pickFace(e.pos);
  if (!pick) return;
  auto normal = geom::planarFaceNormal(pick->face);
  curved_ = !normal;
  // Curved faces (cylinders, swept/lofted surfaces) move along their
  // normal at the picked point.
  if (!normal) normal = geom::faceNormalAt(pick->face, pick->point);
  if (!normal) return;
  objectId_ = pick->objectId;
  face_ = pick->face;
  origin_ = pick->point;
  normal_ = *normal;
  distance_ = 0.0;
  dragging_ = true;
  host_.viewport().clearHover();
  host_.viewport().setPickMode(PickMode::None);
}

void PushPullTool::commit(double distance) {
  if (std::abs(distance) < tolerance::kMinDimension) {
    reset();
    return;
  }
  doc::Modeler& m = host_.modeler();
  auto before = m.parameters(objectId_);
  auto result = m.pushPull(objectId_, face_, distance);
  if (!result) {
    host_.reportError(QObject::tr("プッシュ/プルに失敗しました: %1")
                          .arg(QString::fromStdString(result.error())));
    reset();
    return;
  }
  double base = 0.0;
  for (const auto& p : before) {
    if (p.key == result->parameterKey) base = p.value;
  }
  double after = base;
  // The result may be a different object (profile merged into a host solid).
  for (const auto& p : m.parameters(result->id)) {
    if (p.key == result->parameterKey) after = p.value;
  }
  lastDistance_ = distance;
  // Typed values are measured along the drag direction.
  last_ = LastPush{result->id, result->parameterKey, base, (after - base) / std::abs(distance)};
  reset();
  host_.setMeasurement(QObject::tr("距離"), mm(distance));
}

bool PushPullTool::mouseDoubleClick(const ToolEvent&) {
  // The first click of the double-click already started a drag on the face.
  if (!dragging_ || !lastDistance_ || std::abs(distance_) > tolerance::kMinDimension) return false;
  commit(*lastDistance_);
  return true;
}

void PushPullTool::applyMeasurement(const QString& text) {
  const auto value = parseOne(text);
  if (!value) {
    host_.reportError(QObject::tr("距離を解釈できません: %1").arg(text));
    return;
  }
  if (dragging_) {
    commit(distance_ < -tolerance::kMinDimension ? -*value : *value);
    return;
  }
  if (last_) {
    const Status s =
        host_.modeler().setParameter(last_->id, last_->key, last_->base + last_->sign * *value);
    if (!s) host_.reportError(QString::fromStdString(s.message()));
    return;
  }
  host_.reportError(QObject::tr("面をクリックしてから距離を入力してください"));
}

// --- Arrays -------------------------------------------------------------------------

std::optional<ArrayRequest> parseArrayRequest(const QString& text) {
  static const QRegularExpression multiply(QStringLiteral("^\\s*(?:[xX*]\\s*(\\d+)|(\\d+)\\s*[xX*])\\s*$"));
  static const QRegularExpression divide(QStringLiteral("^\\s*(?:/\\s*(\\d+)|(\\d+)\\s*/)\\s*$"));
  for (const auto& [re, isDivide] : {std::pair{&multiply, false}, std::pair{&divide, true}}) {
    const QRegularExpressionMatch m = re->match(text);
    if (!m.hasMatch()) continue;
    const QString digits = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
    const int n = digits.toInt();
    if (n < 1 || n > 10000) return std::nullopt;
    return ArrayRequest{isDivide, n};
  }
  return std::nullopt;
}

// --- Move / Rotate shared ------------------------------------------------------------

bool TransformTool::keyPress(QKeyEvent* event) {
  if (event->key() == Qt::Key_Control && !event->isAutoRepeat()) {
    copy_ = !copy_;
    refresh();
    return true;
  }
  return DrawingTool::keyPress(event);
}

QString TransformTool::copySuffix() const {
  return copy_ ? QObject::tr("［コピー］") : QObject::tr("（Ctrl でコピー）");
}

void TransformTool::commit(const gp_Trsf& delta, int count) {
  doc::Modeler& m = host_.modeler();
  if (copy_) {
    auto created = m.copy(targets_, delta, count);
    if (!created) {
      host_.reportError(QString::fromStdString(created.error()));
      last_.reset();
      return;
    }
    host_.setSelection(created.value());
  } else {
    const Status s = m.transform(targets_, delta, name().toStdString());
    if (!s) {
      host_.reportError(QString::fromStdString(s.message()));
      last_.reset();
      return;
    }
  }
  last_ = Last{targets_, delta, copy_, count, m.history().undoCount()};
}

bool TransformTool::canReeditLast() const {
  return last_ && host_.modeler().history().undoCount() == last_->undoCount;
}

bool TransformTool::redoLast(const gp_Trsf& delta, int count) {
  if (!canReeditLast()) return false;
  doc::Modeler& m = host_.modeler();
  m.undo();
  targets_ = last_->targets;
  copy_ = last_->copy;
  commit(delta, count);
  copy_ = false;
  return true;
}

// --- Move ------------------------------------------------------------------------------

QString MoveTool::name() const { return QObject::tr("移動"); }
QCursor MoveTool::cursor() const { return toolCursor(CursorKind::Move); }

void MoveTool::activate() {
  resetTracking();
  resetState();
  host_.viewport().setPickMode(PickMode::Objects);
  host_.setMeasurement(QObject::tr("距離"), QString());
  host_.setStatus(QObject::tr("基点をクリック（未選択ならクリックした対象を移動）") + copySuffix());
}

void MoveTool::resetState() {
  moving_ = false;
  stretch_.reset();
  copy_ = false;
  resetLock();
  host_.viewport().clearPreview();
  host_.viewport().hideSnap();
}

void MoveTool::cancel() { resetState(); }

void MoveTool::mouseMove(const ToolEvent& e) {
  if (!moving_) {
    host_.viewport().pickObject(e.pos);
    const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
    host_.setStatus(QObject::tr("基点をクリック  [%1]").arg(snapLabel(r.kind)) + copySuffix());
    return;
  }
  const snap::SnapResult r = snapAt(e, base_, gp_Pln(base_, gp::DZ()));
  if (!r.valid()) return;
  current_ = r.point;
  if (stretch_) {
    // Preview the slab the face sweeps through.
    const double d = gp_Vec(base_, current_).Dot(gp_Vec(stretchNormal_));
    host_.setMeasurement(QObject::tr("距離"), mm(d));
    host_.setStatus(QObject::tr("面を移動（ソリッドを伸縮）: クリックで確定、または距離を入力"));
    if (std::abs(d) > tolerance::kMinDimension) {
      BRepPrimAPI_MakePrism prism(stretch_->second, gp_Vec(stretchNormal_) * d, Standard_True);
      host_.viewport().setPreview(prism.IsDone() ? prism.Shape() : TopoDS_Shape(), true);
    } else {
      host_.viewport().clearPreview();
    }
    return;
  }
  gp_Trsf delta;
  delta.SetTranslation(gp_Vec(base_, current_));
  host_.viewport().setGhost(targets_, delta);
  host_.setMeasurement(QObject::tr("距離"), QString::number(base_.Distance(current_), 'f', 1));
  host_.setStatus(QObject::tr("移動先をクリック、または距離を入力  [%1]").arg(snapLabel(r.kind)) +
                  lockSuffix() + copySuffix());
}

void MoveTool::finish() {
  if (stretch_) {
    const double d = gp_Vec(base_, current_).Dot(gp_Vec(stretchNormal_));
    if (std::abs(d) > tolerance::kMinDimension) {
      auto result = host_.modeler().pushPull(stretch_->first, stretch_->second, d);
      if (!result) {
        host_.reportError(QObject::tr("面を移動できませんでした: %1").arg(QString::fromStdString(result.error())));
      }
      // Face indices change with the topology: drop the stale face selection.
      host_.setSelection({});
    }
    last_.reset();
  } else {
    gp_Trsf delta;
    delta.SetTranslation(gp_Vec(base_, current_));
    if (base_.Distance(current_) > tolerance::kMinDimension) commit(delta);
  }
  resetState();
  host_.viewport().setPickMode(PickMode::Objects);
}

void MoveTool::mousePress(const ToolEvent& e) {
  if (!moving_) {
    // A single selected face of a solid or profile: stretch along its normal.
    const auto elements = host_.elementSelection();
    if (elements.size() == 1 && elements.front().type == TopAbs_FACE) {
      const TopoDS_Shape face = host_.viewport().elementShape(elements.front());
      const doc::Object* owner = host_.modeler().document().find(elements.front().objectId);
      const auto normal = face.IsNull() ? std::nullopt : geom::planarFaceNormal(TopoDS::Face(face));
      if (owner && normal &&
          (owner->kind() == doc::ObjectKind::Solid || owner->kind() == doc::ObjectKind::Face)) {
        const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
        if (!r.valid()) return;
        stretch_ = std::make_pair(owner->id, TopoDS::Face(face));
        stretchNormal_ = *normal;
        locked_ = *normal;  // movement constrained to the face normal
        base_ = current_ = r.point;
        moving_ = true;
        host_.viewport().clearHover();
        host_.viewport().setPickMode(PickMode::None);
        return;
      }
    }
    targets_ = host_.selection();
    if (targets_.empty()) {
      if (const auto picked = host_.viewport().pickObject(e.pos)) {
        targets_ = {host_.modeler().document().rootOf(*picked)};
        host_.setSelection(targets_);
      }
    }
    if (targets_.empty()) {
      host_.reportError(QObject::tr("移動する対象を選択してください"));
      return;
    }
    const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
    if (!r.valid()) return;
    base_ = current_ = r.point;
    moving_ = true;
    host_.viewport().clearHover();
    host_.viewport().setPickMode(PickMode::None);
    return;
  }
  mouseMove(e);
  finish();
  host_.setStatus(QObject::tr("基点をクリック（数値で距離変更、コピー後は x3 で配列・/3 で等分割）"));
}

void MoveTool::applyMeasurement(const QString& text) {
  if (moving_) {
    const auto length = units::parseLength(text.toStdString());
    if (!length) {
      host_.reportError(QObject::tr("距離を解釈できません: %1").arg(text));
      return;
    }
    if (stretch_) {
      const double d = gp_Vec(base_, current_).Dot(gp_Vec(stretchNormal_));
      current_ = base_.Translated(gp_Vec(stretchNormal_) * (d < 0 ? -*length : *length));
      finish();
      return;
    }
    if (base_.Distance(current_) < 1e-9) {
      host_.reportError(QObject::tr("マウスで方向を示してから距離を入力してください"));
      return;
    }
    current_ = base_.Translated(gp_Vec(gp_Dir(gp_Vec(base_, current_))) * *length);
    finish();
    return;
  }
  if (!canReeditLast()) {
    host_.reportError(QObject::tr("基点をクリックしてから距離を入力してください"));
    return;
  }
  const gp_Vec step = last_->delta.TranslationPart();
  const gp_Vec total = step * last_->count;
  if (const auto array = parseArrayRequest(text)) {
    if (!last_->copy) {
      host_.reportError(QObject::tr("配列は Ctrl でコピーしたときに使えます"));
      return;
    }
    gp_Trsf delta;
    // "x3": three copies at the same spacing; "/3": three copies dividing the distance.
    delta.SetTranslation(array->divide ? total / array->count : step);
    redoLast(delta, array->count);
    return;
  }
  const auto length = units::parseLength(text.toStdString());
  if (!length || step.Magnitude() < 1e-9) {
    host_.reportError(QObject::tr("距離を解釈できません: %1").arg(text));
    return;
  }
  gp_Trsf delta;
  delta.SetTranslation(gp_Vec(gp_Dir(step)) * *length);
  redoLast(delta, last_->count);
}

// --- Rotate ------------------------------------------------------------------------------

QString RotateTool::name() const { return QObject::tr("回転"); }
QCursor RotateTool::cursor() const { return toolCursor(CursorKind::Rotate); }

void RotateTool::activate() {
  resetTracking();
  resetState();
  host_.viewport().setPickMode(PickMode::None);
  host_.setMeasurement(QObject::tr("角度"), QString());
  host_.setStatus(QObject::tr("回転の中心をクリック（面上なら面の法線まわり）") + copySuffix());
}

void RotateTool::resetState() {
  stage_ = Stage::Center;
  angle_ = 0.0;
  copy_ = false;
  resetLock();
  host_.viewport().clearPreview();
  host_.viewport().hideSnap();
}

void RotateTool::cancel() { resetState(); }

gp_Trsf RotateTool::rotation(double angle) const {
  gp_Trsf t;
  t.SetRotation(gp_Ax1(center_, normal_), angle);
  return t;
}

std::optional<double> RotateTool::angleAt(const gp_Pnt& p) const {
  gp_Vec v(center_, p);
  v -= gp_Vec(normal_) * v.Dot(gp_Vec(normal_));
  if (v.Magnitude() < 1e-6) return std::nullopt;
  const gp_Vec ref(reference_);
  double angle = std::atan2(ref.Crossed(v).Dot(gp_Vec(normal_)), ref.Dot(v));
  // Protractor snapping to 15 degree steps.
  const double step = M_PI / 12;
  const double snapped = std::round(angle / step) * step;
  if (std::abs(angle - snapped) < M_PI / 90) angle = snapped;
  return angle;
}

void RotateTool::mouseMove(const ToolEvent& e) {
  Viewport& vp = host_.viewport();
  switch (stage_) {
    case Stage::Center: {
      const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
      host_.setStatus(QObject::tr("回転の中心をクリック  [%1]").arg(snapLabel(r.kind)) + copySuffix());
      return;
    }
    case Stage::Reference: {
      const snap::SnapResult r = snapAt(e, center_, gp_Pln(center_, normal_));
      if (!r.valid()) return;
      if (r.point.Distance(center_) > 1e-6) {
        vp.setPreview(BRepBuilderAPI_MakeEdge(center_, r.point).Edge());
      }
      host_.setStatus(QObject::tr("基準方向をクリック  [%1]").arg(snapLabel(r.kind)));
      return;
    }
    case Stage::Rotating: {
      const snap::SnapResult r = snapAt(e, center_, gp_Pln(center_, normal_));
      if (!r.valid()) return;
      const auto angle = angleAt(r.point);
      if (!angle) return;
      angle_ = *angle;
      vp.setGhost(targets_, rotation(angle_));
      host_.setMeasurement(QObject::tr("角度"), QString::number(angle_ * 180.0 / M_PI, 'f', 1));
      host_.setStatus(QObject::tr("クリックで確定、または角度を入力") + copySuffix());
      return;
    }
  }
}

void RotateTool::mousePress(const ToolEvent& e) {
  switch (stage_) {
    case Stage::Center: {
      targets_ = host_.selection();
      if (targets_.empty()) {
        host_.reportError(QObject::tr("先に回転する対象を選択してください"));
        return;
      }
      const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
      if (!r.valid()) return;
      center_ = r.point;
      normal_ = (r.kind == snap::SnapKind::OnFace && r.faceNormal) ? *r.faceNormal : gp::DZ();
      stage_ = Stage::Reference;
      return;
    }
    case Stage::Reference: {
      const snap::SnapResult r = snapAt(e, center_, gp_Pln(center_, normal_));
      gp_Vec v(center_, r.point);
      v -= gp_Vec(normal_) * v.Dot(gp_Vec(normal_));
      if (v.Magnitude() < 1e-6) return;
      reference_ = gp_Dir(v);
      stage_ = Stage::Rotating;
      return;
    }
    case Stage::Rotating:
      mouseMove(e);
      if (std::abs(angle_) > 1e-9) {
        lastAxis_ = gp_Ax1(center_, normal_);
        lastAngle_ = angle_;
        commit(rotation(angle_));
      }
      resetState();
      host_.setStatus(QObject::tr("回転の中心をクリック（数値で角度変更、コピー後は x4 で配列）"));
      return;
  }
}

void RotateTool::applyMeasurement(const QString& text) {
  if (stage_ == Stage::Rotating) {
    bool ok = false;
    const double degrees = text.trimmed().toDouble(&ok);
    if (!ok) {
      host_.reportError(QObject::tr("角度を解釈できません: %1").arg(text));
      return;
    }
    const double sign = angle_ < 0 ? -1.0 : 1.0;
    lastAxis_ = gp_Ax1(center_, normal_);
    lastAngle_ = sign * degrees * M_PI / 180.0;
    commit(rotation(lastAngle_));
    resetState();
    return;
  }
  if (!canReeditLast()) {
    host_.reportError(QObject::tr("回転してから数値を入力してください"));
    return;
  }
  const double angle = lastAngle_;
  if (const auto array = parseArrayRequest(text)) {
    if (!last_->copy) {
      host_.reportError(QObject::tr("配列は Ctrl でコピーしたときに使えます"));
      return;
    }
    // "x4": four copies at the same angle; "/4": divide the total angle.
    const double step = array->divide ? angle * last_->count / array->count : angle;
    gp_Trsf delta;
    delta.SetRotation(lastAxis_, step);
    lastAngle_ = step;
    redoLast(delta, array->count);
    return;
  }
  bool ok = false;
  const double degrees = text.trimmed().toDouble(&ok);
  if (!ok) {
    host_.reportError(QObject::tr("角度を解釈できません: %1").arg(text));
    return;
  }
  lastAngle_ = (angle < 0 ? -1.0 : 1.0) * degrees * M_PI / 180.0;
  gp_Trsf delta;
  delta.SetRotation(lastAxis_, lastAngle_);
  redoLast(delta, last_->count);
}

// --- Offset ------------------------------------------------------------------------------

namespace {

// Signed distance from a point on the face plane to the outer boundary:
// negative inside the face, positive outside.
double signedBoundaryDistance(const TopoDS_Face& face, const gp_Pnt& p) {
  const TopoDS_Wire outer = BRepTools::OuterWire(face);
  BRepExtrema_DistShapeShape dist(BRepBuilderAPI_MakeVertex(p).Vertex(), outer);
  if (!dist.IsDone()) return 0.0;
  BRepClass_FaceClassifier classifier(face, p, tolerance::kLinear);
  return classifier.State() == TopAbs_IN ? -dist.Value() : dist.Value();
}

}  // namespace

QString OffsetTool::name() const { return QObject::tr("オフセット"); }
QCursor OffsetTool::cursor() const { return toolCursor(CursorKind::Offset); }

void OffsetTool::activate() {
  host_.viewport().setPickMode(PickMode::Faces);
  host_.setMeasurement(QObject::tr("オフセット距離"), QString());
  host_.setStatus(QObject::tr("オフセットする面をクリック（ソリッドの面なら面上に内側の輪郭を作成）"));
}

void OffsetTool::cancel() { reset(); }

void OffsetTool::reset() {
  dragging_ = false;
  distance_ = 0.0;
  face_.Nullify();
  host_.viewport().clearPreview();
  host_.viewport().setPickMode(PickMode::Faces);
}

void OffsetTool::mouseMove(const ToolEvent& e) {
  Viewport& vp = host_.viewport();
  if (!dragging_) {
    vp.pickFace(e.pos);
    return;
  }
  const snap::Ray ray = vp.rayAt(e.pos);
  const gp_Dir n = plane_.Axis().Direction();
  const double denom = gp_Vec(ray.direction).Dot(gp_Vec(n));
  if (std::abs(denom) < 1e-9) return;
  const double t = gp_Vec(ray.origin, plane_.Location()).Dot(gp_Vec(n)) / denom;
  const gp_Pnt p = ray.origin.Translated(gp_Vec(ray.direction) * t);
  distance_ = signedBoundaryDistance(face_, p);
  host_.setMeasurement(QObject::tr("オフセット距離"), QString::number(distance_, 'f', 1));
  auto preview = geom::offsetFace(face_, distance_);
  vp.setPreview(preview ? TopoDS_Shape(BRepTools::OuterWire(preview.value())) : TopoDS_Shape());
  host_.setStatus(QObject::tr("クリックで確定、または距離を入力（外側が正）"));
}

void OffsetTool::mousePress(const ToolEvent& e) {
  if (dragging_) {
    mouseMove(e);
    commit(distance_);
    return;
  }
  const auto pick = host_.viewport().pickFace(e.pos);
  if (!pick) return;
  const doc::Object* o = host_.modeler().document().find(pick->objectId);
  const bool usable = o && (o->kind() == doc::ObjectKind::Face || o->kind() == doc::ObjectKind::Solid);
  if (!usable || !geom::isPlanarFace(pick->face, &plane_)) {
    host_.reportError(QObject::tr("オフセットできるのは平らな面です"));
    return;
  }
  objectId_ = pick->objectId;
  face_ = pick->face;
  distance_ = 0.0;
  dragging_ = true;
  host_.viewport().clearHover();
  host_.viewport().setPickMode(PickMode::None);
}

void OffsetTool::commit(double distance) {
  if (std::abs(distance) < tolerance::kMinDimension) {
    reset();
    return;
  }
  const doc::Object* owner = host_.modeler().document().find(objectId_);
  if (owner && owner->kind() == doc::ObjectKind::Solid) {
    // Face of a solid: a new outline on that face (push it in for a recess).
    auto created = host_.modeler().offsetSolidFace(objectId_, face_, distance);
    if (!created) {
      host_.reportError(QObject::tr("オフセットに失敗しました: %1")
                            .arg(QString::fromStdString(created.error())));
    }
    last_.reset();
  } else {
    auto result = host_.modeler().offset(objectId_, distance);
    if (!result) {
      host_.reportError(QObject::tr("オフセットに失敗しました: %1")
                            .arg(QString::fromStdString(result.error())));
    } else {
      last_ = LastOffset{result->id, result->parameterKey};
    }
  }
  reset();
  host_.setMeasurement(QObject::tr("オフセット距離"), QString::number(distance, 'f', 1));
}

void OffsetTool::applyMeasurement(const QString& text) {
  const auto value = units::parseLength(text.toStdString());
  if (!value) {
    host_.reportError(QObject::tr("距離を解釈できません: %1").arg(text));
    return;
  }
  if (dragging_) {
    // Typed values follow the drag side: inside = inwards.
    commit(distance_ < 0 ? -std::abs(*value) : std::abs(*value));
    return;
  }
  if (last_) {
    const Status s = host_.modeler().setParameter(last_->id, last_->key, *value);
    if (!s) host_.reportError(QString::fromStdString(s.message()));
    return;
  }
  host_.reportError(QObject::tr("面をクリックしてから距離を入力してください"));
}

// --- Revolve -------------------------------------------------------------------------

QString RevolveTool::name() const { return QObject::tr("回転体"); }
QCursor RevolveTool::cursor() const { return toolCursor(CursorKind::Revolve); }

void RevolveTool::activate() {
  resetTracking();
  reset();
  host_.setMeasurement(QObject::tr("角度"), QString());
}

void RevolveTool::reset() {
  stage_ = Stage::Profile;
  resetLock();
  host_.viewport().clearPreview();
  host_.viewport().hideSnap();
  host_.viewport().setPickMode(PickMode::Faces);
  host_.setStatus(QObject::tr("回転させる面（押し出し前のプロファイル）をクリック"));
}

void RevolveTool::cancel() { reset(); }

void RevolveTool::mouseMove(const ToolEvent& e) {
  switch (stage_) {
    case Stage::Profile:
      host_.viewport().pickFace(e.pos);
      return;
    case Stage::AxisStart: {
      const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
      host_.setStatus(QObject::tr("回転軸の 1 点目をクリック  [%1]").arg(snapLabel(r.kind)));
      return;
    }
    case Stage::AxisEnd: {
      const snap::SnapResult r = snapAt(e, axisStart_, std::nullopt);
      if (r.valid() && r.point.Distance(axisStart_) > tolerance::kMinDimension) {
        host_.viewport().setPreview(BRepBuilderAPI_MakeEdge(axisStart_, r.point).Edge());
      }
      host_.setStatus(QObject::tr("回転軸の 2 点目をクリック  [%1]").arg(snapLabel(r.kind)) + lockSuffix());
      return;
    }
  }
}

void RevolveTool::mousePress(const ToolEvent& e) {
  switch (stage_) {
    case Stage::Profile: {
      const auto pick = host_.viewport().pickFace(e.pos);
      if (!pick) return;
      const doc::Object* o = host_.modeler().document().find(pick->objectId);
      if (!o || o->kind() != doc::ObjectKind::Face) {
        host_.reportError(QObject::tr("回転できるのは押し出し前の面（プロファイル）です"));
        return;
      }
      profileId_ = pick->objectId;
      host_.viewport().clearHover();
      host_.viewport().setPickMode(PickMode::None);
      stage_ = Stage::AxisStart;
      host_.setStatus(QObject::tr("回転軸の 1 点目をクリック"));
      return;
    }
    case Stage::AxisStart: {
      const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
      if (!r.valid()) return;
      axisStart_ = r.point;
      stage_ = Stage::AxisEnd;
      return;
    }
    case Stage::AxisEnd: {
      const snap::SnapResult r = snapAt(e, axisStart_, std::nullopt);
      if (!r.valid() || r.point.Distance(axisStart_) < tolerance::kMinDimension) return;
      const gp_Ax1 axis(axisStart_, gp_Dir(gp_Vec(axisStart_, r.point)));
      auto id = host_.modeler().revolveObject(profileId_, axis, 2 * M_PI);
      if (!id) {
        host_.reportError(QObject::tr("回転体を作れませんでした: %1").arg(QString::fromStdString(id.error())));
        reset();
        return;
      }
      last_.reset();
      for (const doc::Parameter& p : host_.modeler().parameters(*id)) {
        if (p.key.size() > 6 && p.key.compare(p.key.size() - 6, 6, ".angle") == 0) last_ = {{*id, p.key}};
      }
      reset();
      host_.setMeasurement(QObject::tr("角度"), QStringLiteral("360"));
      host_.setStatus(QObject::tr("回転体を作成しました（角度を入力すると変更できます）"));
      return;
    }
  }
}

void RevolveTool::applyMeasurement(const QString& text) {
  bool ok = false;
  const double degrees = text.trimmed().toDouble(&ok);
  if (!ok || !last_) {
    host_.reportError(QObject::tr("回転体を作ってから角度を入力してください"));
    return;
  }
  const Status s = host_.modeler().setParameter(last_->first, last_->second, degrees);
  if (!s) host_.reportError(QString::fromStdString(s.message()));
}

// --- Measure ---------------------------------------------------------------------------

QString MeasureTool::name() const { return QObject::tr("メジャー"); }
QCursor MeasureTool::cursor() const { return toolCursor(CursorKind::Measure); }

void MeasureTool::activate() {
  resetTracking();
  host_.viewport().setPickMode(PickMode::None);
  host_.setMeasurement(QObject::tr("距離"), QString());
  host_.setStatus(QObject::tr("測りはじめの点をクリック"));
}

void MeasureTool::cancel() {
  start_.reset();
  guideDir_.reset();
  guideOffset_ = gp_Vec();
  resetLock();
  host_.viewport().clearPreview();
  host_.viewport().hideSnap();
}

void MeasureTool::show(const gp_Pnt& a, const gp_Pnt& b, bool final) {
  const gp_Vec d(a, b);
  host_.setMeasurement(QObject::tr("距離"), mm(d.Magnitude()));
  host_.setStatus((final ? QObject::tr("距離 %1 mm") : QObject::tr("距離 %1 mm（クリックで確定）"))
                      .arg(mm(d.Magnitude())) +
                  QObject::tr("   ΔX %1 / ΔY %2 / ΔZ %3").arg(mm(d.X()), mm(d.Y()), mm(d.Z())));
  if (a.Distance(b) > tolerance::kMinDimension) {
    host_.viewport().setPreview(BRepBuilderAPI_MakeEdge(a, b).Edge());
  }
}

gp_Vec MeasureTool::guideOffset(const gp_Pnt& p) const {
  const gp_Vec v(*start_, p);
  const gp_Vec along = gp_Vec(*guideDir_) * v.Dot(gp_Vec(*guideDir_));
  return v - along;
}

void MeasureTool::showGuide(const gp_Pnt& p) {
  guideOffset_ = guideOffset(p);
  const double d = guideOffset_.Magnitude();
  host_.setMeasurement(QObject::tr("オフセット"), mm(d));
  host_.setStatus(QObject::tr("ガイド: エッジから %1 mm（クリックで配置、数値入力で距離指定）").arg(mm(d)));
  if (d <= tolerance::kMinDimension) {
    host_.viewport().clearPreview();
    return;
  }
  // Offset tick from the edge plus a stretch of the guide around the cursor.
  const gp_Pnt foot = p.Translated(-guideOffset_);
  const gp_Vec reach = gp_Vec(*guideDir_) * std::max(2000.0, 4.0 * d);
  BRep_Builder builder;
  TopoDS_Compound preview;
  builder.MakeCompound(preview);
  builder.Add(preview, BRepBuilderAPI_MakeEdge(foot, p).Edge());
  builder.Add(preview, BRepBuilderAPI_MakeEdge(p.Translated(-reach), p.Translated(reach)).Edge());
  host_.viewport().setPreview(preview);
}

void MeasureTool::placeGuide(const gp_Vec& offset) {
  auto id = host_.modeler().createGuide(start_->Translated(offset), *guideDir_);
  if (!id) host_.reportError(QString::fromStdString(id.error()));
  lastDistance_ = offset.Magnitude();
  cancel();
  host_.setStatus(QObject::tr("ガイドを配置しました（%1 mm）").arg(mm(offset.Magnitude())));
}

void MeasureTool::mouseMove(const ToolEvent& e) {
  const snap::SnapResult r = snapAt(e, start_, std::nullopt);
  if (!r.valid()) return;
  if (start_ && guideDir_) {
    showGuide(r.point);
  } else if (start_) {
    show(*start_, r.point, false);
  } else {
    host_.setStatus(QObject::tr("測りはじめの点をクリック（直線エッジ上から始めるとガイド）  [%1]")
                        .arg(snapLabel(r.kind)));
  }
}

void MeasureTool::mousePress(const ToolEvent& e) {
  const snap::SnapResult r = snapAt(e, start_, std::nullopt);
  if (!r.valid()) return;
  if (!start_) {
    start_ = r.point;
    // Starting on the middle of a straight edge pulls a parallel guide off
    // it (SketchUp); starting on a point measures.
    if (r.kind == snap::SnapKind::OnEdge && r.edgeDirection) guideDir_ = *r.edgeDirection;
    return;
  }
  if (guideDir_) {
    const gp_Vec offset = guideOffset(r.point);
    if (offset.Magnitude() <= tolerance::kMinDimension) return;  // still on the edge
    placeGuide(offset);
    return;
  }
  show(*start_, r.point, true);
  lastDistance_ = start_->Distance(r.point);
  start_.reset();
  resetLock();
}

void MeasureTool::applyMeasurement(const QString& text) {
  if (!start_ || !guideDir_) return;  // measuring: nothing to apply
  const auto value = units::parseLength(text.toStdString());
  if (!value || *value <= 0) {
    host_.reportError(QObject::tr("長さを解釈できません: %1").arg(text));
    return;
  }
  if (guideOffset_.Magnitude() <= tolerance::kMinDimension) {
    host_.reportError(QObject::tr("マウスでガイドを出す側を示してから入力してください"));
    return;
  }
  placeGuide(guideOffset_.Normalized() * *value);
}

// --- Paste ------------------------------------------------------------------------------

QString PasteTool::name() const { return QObject::tr("貼り付け"); }
QCursor PasteTool::cursor() const { return toolCursor(CursorKind::Move); }

void PasteTool::activate() {
  resetTracking();
  host_.viewport().setPickMode(PickMode::None);
  host_.setStatus(QObject::tr("貼り付ける位置をクリック（コピーした物の左下手前の角が吸着点に来ます、Esc で中止）"));
}

void PasteTool::cancel() {
  resetLock();
  host_.viewport().clearPreview();
  host_.viewport().hideSnap();
}

void PasteTool::mouseMove(const ToolEvent& e) {
  if (!clipboard_ || clipboard_->empty()) return;
  const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
  if (!r.valid()) return;
  gp_Trsf delta;
  delta.SetTranslation(clipboard_->anchor, r.point);
  host_.viewport().setPreview(clipboard_->preview.Moved(TopLoc_Location(delta)), true);
  host_.setStatus(QObject::tr("貼り付ける位置をクリック  [%1]").arg(snapLabel(r.kind)));
}

void PasteTool::mousePress(const ToolEvent& e) {
  if (!clipboard_ || clipboard_->empty()) return;
  const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
  if (!r.valid()) return;
  gp_Trsf delta;
  delta.SetTranslation(clipboard_->anchor, r.point);
  auto ids = host_.modeler().paste(*clipboard_, delta);
  cancel();
  if (!ids) {
    host_.reportError(QString::fromStdString(ids.error()));
    return;
  }
  host_.setSelection(*ids);
  if (onPlaced_) onPlaced_(*ids);
}

// --- Scale ------------------------------------------------------------------------------

QString ScaleTool::name() const { return QObject::tr("尺度"); }
QCursor ScaleTool::cursor() const { return toolCursor(CursorKind::Scale); }

void ScaleTool::activate() {
  resetTracking();
  cancel();
  host_.viewport().setPickMode(PickMode::None);
  host_.setMeasurement(QObject::tr("倍率"), QString());
  host_.setStatus(QObject::tr("拡大縮小の基準（動かない点）をクリック。先に対象を選択しておきます"));
}

void ScaleTool::cancel() {
  stage_ = Stage::Fixed;
  factor_ = 1.0;
  resetLock();
  host_.viewport().clearPreview();
  host_.viewport().hideSnap();
}

bool ScaleTool::stepBack() {
  if (stage_ == Stage::Fixed) return false;
  stage_ = stage_ == Stage::Scaling ? Stage::Reference : Stage::Fixed;
  host_.viewport().clearPreview();
  return true;
}

std::optional<double> ScaleTool::factorAt(const gp_Pnt& p) const {
  const gp_Vec ref(fixed_, reference_);
  const double d0 = ref.Magnitude();
  if (d0 < tolerance::kMinDimension) return std::nullopt;
  // Distance along the reference direction: moving past the fixed point
  // does not mirror.
  const double d = gp_Vec(fixed_, p).Dot(ref / d0);
  if (d < tolerance::kMinDimension) return std::nullopt;
  double f = d / d0;
  const double snapped = std::round(f * 20.0) / 20.0;  // 0.05 steps when close
  if (std::abs(f - snapped) < 0.005) f = snapped;
  return f;
}

void ScaleTool::commit(double factor) {
  const Status s = host_.modeler().scale(targets_, fixed_, factor);
  if (!s) {
    host_.reportError(QString::fromStdString(s.message()));
  } else {
    last_ = Last{targets_, fixed_, fixed_.Distance(reference_), host_.modeler().history().undoCount()};
    host_.setStatus(QObject::tr("%1 倍にしました（続けて数値を入力すると倍率を変更）").arg(factor));
  }
  stage_ = Stage::Fixed;
  resetLock();
  host_.viewport().clearPreview();
}

void ScaleTool::mouseMove(const ToolEvent& e) {
  Viewport& vp = host_.viewport();
  switch (stage_) {
    case Stage::Fixed: {
      const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
      host_.setStatus(QObject::tr("基準（動かない点）をクリック  [%1]").arg(snapLabel(r.kind)));
      return;
    }
    case Stage::Reference: {
      const snap::SnapResult r = snapAt(e, fixed_, std::nullopt);
      if (!r.valid()) return;
      if (r.point.Distance(fixed_) > 1e-6) vp.setPreview(BRepBuilderAPI_MakeEdge(fixed_, r.point).Edge());
      host_.setStatus(QObject::tr("基準の長さを測る点をクリック  [%1]").arg(snapLabel(r.kind)));
      return;
    }
    case Stage::Scaling: {
      const snap::SnapResult r = snapAt(e, fixed_, std::nullopt);
      if (!r.valid()) return;
      const auto f = factorAt(r.point);
      if (!f) return;
      factor_ = *f;
      gp_Trsf t;
      t.SetScale(fixed_, factor_);
      vp.setGhost(targets_, t);
      host_.setMeasurement(QObject::tr("倍率"), QString::number(factor_, 'g', 4));
      host_.setStatus(QObject::tr("クリックで確定、または倍率（1.5）か長さ（3000mm）を入力"));
      return;
    }
  }
}

void ScaleTool::mousePress(const ToolEvent& e) {
  switch (stage_) {
    case Stage::Fixed: {
      targets_ = host_.selection();
      if (targets_.empty()) {
        host_.reportError(QObject::tr("先に拡大縮小する対象を選択してください"));
        return;
      }
      const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
      if (!r.valid()) return;
      fixed_ = r.point;
      stage_ = Stage::Reference;
      return;
    }
    case Stage::Reference: {
      const snap::SnapResult r = snapAt(e, fixed_, std::nullopt);
      if (!r.valid() || r.point.Distance(fixed_) < tolerance::kMinDimension) return;
      reference_ = r.point;
      stage_ = Stage::Scaling;
      return;
    }
    case Stage::Scaling:
      mouseMove(e);
      if (std::abs(factor_ - 1.0) > 1e-12) commit(factor_);
      return;
  }
}

void ScaleTool::applyMeasurement(const QString& text) {
  const QString t = text.trimmed();
  // A unit makes it a length (new reference distance), otherwise a factor.
  const bool isLength = t.contains(QRegularExpression(QStringLiteral("[a-zA-Z]")));
  double referenceDistance = 0.0;
  if (stage_ == Stage::Scaling) {
    referenceDistance = fixed_.Distance(reference_);
  } else if (last_ && host_.modeler().history().undoCount() == last_->undoCount) {
    referenceDistance = last_->referenceDistance;
  } else {
    host_.reportError(QObject::tr("基準と参照点をクリックしてから倍率を入力してください"));
    return;
  }
  double factor = 0.0;
  if (isLength) {
    const auto length = units::parseLength(t.toStdString());
    if (!length || *length <= 0 || referenceDistance < tolerance::kMinDimension) {
      host_.reportError(QObject::tr("長さを解釈できません: %1").arg(text));
      return;
    }
    factor = *length / referenceDistance;
  } else {
    bool ok = false;
    factor = t.toDouble(&ok);
    if (!ok || factor <= 0) {
      host_.reportError(QObject::tr("倍率を解釈できません: %1").arg(text));
      return;
    }
  }
  if (stage_ != Stage::Scaling) {  // re-edit the last scale
    host_.modeler().undo();
    targets_ = last_->targets;
    fixed_ = last_->fixed;
    reference_ = fixed_.Translated(gp_Vec(1, 0, 0) * last_->referenceDistance);
  }
  commit(factor);
}

// --- Protractor -----------------------------------------------------------------------

QString ProtractorTool::name() const { return QObject::tr("分度器"); }
QCursor ProtractorTool::cursor() const { return toolCursor(CursorKind::Protractor); }

void ProtractorTool::activate() {
  resetTracking();
  cancel();
  host_.viewport().setPickMode(PickMode::None);
  host_.setMeasurement(QObject::tr("角度"), QString());
  host_.setStatus(QObject::tr("分度器の中心をクリック（面上なら面の平面で、それ以外は水平）"));
}

void ProtractorTool::cancel() {
  stage_ = Stage::Center;
  angle_ = 0.0;
  resetLock();
  host_.viewport().clearPreview();
  host_.viewport().hideSnap();
}

bool ProtractorTool::stepBack() {
  if (stage_ == Stage::Center) return false;
  stage_ = stage_ == Stage::Angle ? Stage::Base : Stage::Center;
  host_.viewport().clearPreview();
  return true;
}

std::optional<double> ProtractorTool::angleAt(const gp_Pnt& p) const {
  gp_Vec v(center_, p);
  v -= gp_Vec(normal_) * v.Dot(gp_Vec(normal_));
  if (v.Magnitude() < 1e-6) return std::nullopt;
  const gp_Vec ref(base_);
  double angle = std::atan2(ref.Crossed(v).Dot(gp_Vec(normal_)), ref.Dot(v));
  const double step = M_PI / 12;  // 15 degree steps, as on the rotate tool
  const double snapped = std::round(angle / step) * step;
  if (std::abs(angle - snapped) < M_PI / 90) angle = snapped;
  return angle;
}

gp_Dir ProtractorTool::guideDirection(double angle) const {
  return base_.Rotated(gp_Ax1(gp::Origin(), normal_), angle);
}

void ProtractorTool::place(double angle) {
  auto id = host_.modeler().createGuide(center_, guideDirection(angle));
  if (!id) host_.reportError(QString::fromStdString(id.error()));
  const double degrees = angle * 180.0 / M_PI;
  cancel();
  host_.setStatus(QObject::tr("ガイドを配置しました（%1°）").arg(QString::number(degrees, 'f', 1)));
}

void ProtractorTool::mouseMove(const ToolEvent& e) {
  Viewport& vp = host_.viewport();
  switch (stage_) {
    case Stage::Center: {
      const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
      host_.setStatus(QObject::tr("分度器の中心をクリック  [%1]").arg(snapLabel(r.kind)));
      return;
    }
    case Stage::Base: {
      const snap::SnapResult r = snapAt(e, center_, gp_Pln(center_, normal_));
      if (!r.valid()) return;
      if (r.point.Distance(center_) > 1e-6) vp.setPreview(BRepBuilderAPI_MakeEdge(center_, r.point).Edge());
      host_.setStatus(QObject::tr("基準の方向をクリック  [%1]").arg(snapLabel(r.kind)));
      return;
    }
    case Stage::Angle: {
      const snap::SnapResult r = snapAt(e, center_, gp_Pln(center_, normal_));
      if (!r.valid()) return;
      const auto angle = angleAt(r.point);
      if (!angle) return;
      angle_ = *angle;
      const double reach = std::max(1000.0, 2.0 * center_.Distance(r.point));
      BRep_Builder builder;
      TopoDS_Compound preview;
      builder.MakeCompound(preview);
      builder.Add(preview, BRepBuilderAPI_MakeEdge(center_, center_.Translated(gp_Vec(base_) * reach)).Edge());
      const gp_Vec along = gp_Vec(guideDirection(angle_)) * reach;
      builder.Add(preview, BRepBuilderAPI_MakeEdge(center_.Translated(-along), center_.Translated(along)).Edge());
      vp.setPreview(preview);
      host_.setMeasurement(QObject::tr("角度"), QString::number(angle_ * 180.0 / M_PI, 'f', 1));
      host_.setStatus(QObject::tr("クリックでガイドを配置、または角度を入力"));
      return;
    }
  }
}

void ProtractorTool::mousePress(const ToolEvent& e) {
  switch (stage_) {
    case Stage::Center: {
      const snap::SnapResult r = snapAt(e, std::nullopt, std::nullopt);
      if (!r.valid()) return;
      center_ = r.point;
      normal_ = (r.kind == snap::SnapKind::OnFace && r.faceNormal) ? *r.faceNormal : gp::DZ();
      stage_ = Stage::Base;
      return;
    }
    case Stage::Base: {
      const snap::SnapResult r = snapAt(e, center_, gp_Pln(center_, normal_));
      gp_Vec v(center_, r.point);
      v -= gp_Vec(normal_) * v.Dot(gp_Vec(normal_));
      if (v.Magnitude() < 1e-6) return;
      base_ = gp_Dir(v);
      stage_ = Stage::Angle;
      return;
    }
    case Stage::Angle:
      mouseMove(e);
      place(angle_);
      return;
  }
}

void ProtractorTool::applyMeasurement(const QString& text) {
  if (stage_ != Stage::Angle) {
    host_.reportError(QObject::tr("中心と基準の方向をクリックしてから角度を入力してください"));
    return;
  }
  bool ok = false;
  const double degrees = text.trimmed().toDouble(&ok);
  if (!ok) {
    host_.reportError(QObject::tr("角度を解釈できません: %1").arg(text));
    return;
  }
  // The side the mouse shows decides the sign, as on the rotate tool.
  const double sign = angle_ < 0 && degrees > 0 ? -1.0 : 1.0;
  place(sign * degrees * M_PI / 180.0);
}

// --- Eraser --------------------------------------------------------------------------

namespace {
// The object a click on `leaf` addresses at the current editing level.
doc::ObjectId addressedAtLevel(const doc::Document& document, doc::ObjectId leaf,
                               const doc::ObjectId& context) {
  int guard = 0;
  while (guard++ < 1000) {
    const doc::Object* o = document.find(leaf);
    if (!o || o->parent.empty() || o->parent == context) break;
    leaf = o->parent;
  }
  return leaf;
}
}  // namespace

QString EraserTool::name() const { return QObject::tr("消しゴム"); }
QCursor EraserTool::cursor() const { return toolCursor(CursorKind::Eraser); }

void EraserTool::activate() {
  host_.viewport().setPickMode(PickMode::Objects);
  host_.setMeasurement(QString(), QString());
  host_.setStatus(QObject::tr("クリックで削除（グループ・コンポーネントは丸ごと）"));
}

void EraserTool::mouseMove(const ToolEvent& e) { host_.viewport().pickObject(e.pos); }

void EraserTool::mousePress(const ToolEvent& e) {
  const auto picked = host_.viewport().pickObject(e.pos);
  if (!picked) return;
  const doc::ObjectId target =
      addressedAtLevel(host_.modeler().document(), *picked, host_.editingGroup());
  const Status s = host_.modeler().remove({target});
  if (!s) host_.reportError(QString::fromStdString(s.message()));
  host_.viewport().clearHover();
}

// --- Follow Me ---------------------------------------------------------------------

QString FollowMeTool::name() const { return QObject::tr("フォローミー"); }
QCursor FollowMeTool::cursor() const { return toolCursor(CursorKind::FollowMe); }

void FollowMeTool::activate() {
  profile_.reset();
  host_.viewport().setPickMode(PickMode::Elements);
  host_.setMeasurement(QString(), QString());
  host_.setStatus(QObject::tr("沿わせる形（断面の面）をクリック"));
}

void FollowMeTool::cancel() {
  profile_.reset();
  host_.viewport().setHighlightedElements({});
}

void FollowMeTool::mouseMove(const ToolEvent& e) {
  const auto picked = host_.viewport().pickElement(e.pos);
  const doc::Object* o = picked ? host_.modeler().document().find(picked->objectId) : nullptr;
  if (!profile_) {
    host_.setStatus(o && o->kind() == doc::ObjectKind::Face ? QObject::tr("この面を断面にします")
                                                            : QObject::tr("沿わせる形（断面の面）をクリック"));
    return;
  }
  if (o && o->kind() == doc::ObjectKind::Curve) {
    host_.setStatus(QObject::tr("この線に沿わせます"));
  } else if (o && o->kind() == doc::ObjectKind::Solid && picked->type == TopAbs_FACE) {
    host_.setStatus(QObject::tr("この面の輪郭を一周させます（巾木・笠木など）"));
  } else {
    host_.setStatus(QObject::tr("パスにする線・円弧、またはソリッドの面をクリック（Esc で中止）"));
  }
}

void FollowMeTool::mousePress(const ToolEvent& e) {
  const auto picked = host_.viewport().pickElement(e.pos);
  if (!picked) return;
  doc::Modeler& m = host_.modeler();
  const doc::Object* o = m.document().find(picked->objectId);
  if (!o) return;
  if (!profile_) {
    if (o->kind() != doc::ObjectKind::Face) {
      host_.reportError(QObject::tr("断面には面（押し出す前の形）を選んでください"));
      return;
    }
    profile_ = o->id;
    host_.viewport().setHighlightedElements({*picked});
    host_.setStatus(QObject::tr("パスにする線・円弧、またはソリッドの面をクリック（Esc で中止）"));
    return;
  }
  Result<doc::ObjectId> swept = Result<doc::ObjectId>::failure("");
  if (o->kind() == doc::ObjectKind::Curve) {
    swept = m.followMe(*profile_, o->id);
  } else if (picked->type == TopAbs_FACE) {
    const TopoDS_Shape face = host_.viewport().elementShape(*picked);
    swept = m.followMe(*profile_, BRepTools::OuterWire(TopoDS::Face(face)));
  } else {
    host_.reportError(QObject::tr("パスには線・円弧・曲線か、ソリッドの面を選んでください"));
    return;
  }
  cancel();
  if (!swept) {
    host_.reportError(QObject::tr("フォローミーに失敗しました: %1").arg(QString::fromStdString(swept.error())));
    return;
  }
  host_.setStatus(QObject::tr("断面を沿わせました（断面の寸法はパネルで変更できます）"));
}

// --- Paint ---------------------------------------------------------------------------

QString PaintTool::name() const { return QObject::tr("ペイント"); }
QCursor PaintTool::cursor() const { return toolCursor(CursorKind::Paint); }

void PaintTool::activate() {
  host_.viewport().setPickMode(PickMode::Objects);
  host_.setMeasurement(QString(), QString());
  host_.setStatus(QObject::tr("クリックで塗る / Alt+クリックで色を拾う（色はツールをもう一度選ぶと変更）"));
}

void PaintTool::mouseMove(const ToolEvent& e) { host_.viewport().pickObject(e.pos); }

void PaintTool::mousePress(const ToolEvent& e) {
  const auto picked = host_.viewport().pickObject(e.pos);
  if (!picked) return;
  const doc::Document& document = host_.modeler().document();
  if (e.modifiers & Qt::AltModifier) {
    if (const doc::Object* o = document.find(*picked)) {
      color_ = o->color;
      host_.setStatus(QObject::tr("色を拾いました（クリックで塗る）"));
    }
    return;
  }
  // Paint at the current editing level: a group/component as a whole, or a
  // single object inside the group being edited.
  const doc::ObjectId target = addressedAtLevel(document, *picked, host_.editingGroup());
  const Status s = host_.modeler().paint({target}, color_);
  if (!s) host_.reportError(QString::fromStdString(s.message()));
}

}  // namespace sbr::app
