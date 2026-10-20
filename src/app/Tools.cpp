#include "Tools.h"

#include <algorithm>
#include <cmath>

#include <QKeyEvent>
#include <QRegularExpression>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepTools.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <BRepPrimAPI_MakePrism.hxx>

#include "Cursors.h"
#include "Viewport.h"
#include "sbr/Units.h"
#include "sbr/geom/Freeform.h"
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

void SelectTool::mouseMove(const ToolEvent& e) { host_.viewport().pickElement(e.pos); }

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
    if (!additive) host_.setSelection({});
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
  if (addressed != picked->objectId || (o && o->kind() == doc::ObjectKind::Instance)) {
    return false;  // components: focus on them
  }
  sinceDoubleClick_.start();
  doubleClickPos_ = e.pos;
  apply(host_.viewport().withNeighbours(*picked), e.modifiers & Qt::ShiftModifier);
  return true;
}

// --- Drawing base ---------------------------------------------------------------

snap::SnapResult DrawingTool::snapAt(const ToolEvent& e, std::optional<gp_Pnt> anchor,
                                     std::optional<gp_Pln> plane) {
  lastEvent_ = e;
  last_ = host_.viewport().snap(e.pos, anchor, anchor ? locked_ : std::nullopt, plane,
                                trackedPoints_, trackedDirections_);
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
    plane = gp_Pln(*anchor, gp::DZ());
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
  }
  updatePreview();
}

void LineTool::mousePress(const ToolEvent& e) {
  const auto p = pointAt(e);
  if (p) addPoint(*p);
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
    resetLock();
    updatePreview();
    host_.setStatus(QObject::tr("始点をクリック"));
    return;
  }
  points_.push_back(p);
  resetLock();
  updatePreview();
}

void LineTool::finish() {
  if (points_.size() >= 2) {
    auto line = host_.modeler().createPolyline(points_);
    if (!line) host_.reportError(QString::fromStdString(line.error()));
  }
  points_.clear();
  current_.reset();
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

// --- Circle ---------------------------------------------------------------------

namespace {
// Sketch plane for a shape started at `r`: the face under the cursor when
// the click is on a face (or on one of its edges/corners), else horizontal.
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
  if (std::abs(distance_) > tolerance::kMinDimension) {
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
  const auto normal = geom::planarFaceNormal(pick->face);
  if (!normal) {
    host_.reportError(QObject::tr("現在は平面のみプッシュ/プルできます"));
    return;
  }
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
  gp_Trsf delta;
  delta.SetTranslation(gp_Vec(base_, current_));
  host_.viewport().setGhost(targets_, delta);
  host_.setMeasurement(QObject::tr("距離"), QString::number(base_.Distance(current_), 'f', 1));
  host_.setStatus(QObject::tr("移動先をクリック、または距離を入力  [%1]").arg(snapLabel(r.kind)) +
                  lockSuffix() + copySuffix());
}

void MoveTool::mousePress(const ToolEvent& e) {
  if (!moving_) {
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
  gp_Trsf delta;
  delta.SetTranslation(gp_Vec(base_, current_));
  if (base_.Distance(current_) > tolerance::kMinDimension) commit(delta);
  resetState();
  host_.viewport().setPickMode(PickMode::Objects);
  host_.setStatus(QObject::tr("基点をクリック（数値で距離変更、コピー後は x3 で配列・/3 で等分割）"));
}

void MoveTool::applyMeasurement(const QString& text) {
  if (moving_) {
    const auto length = units::parseLength(text.toStdString());
    if (!length) {
      host_.reportError(QObject::tr("距離を解釈できません: %1").arg(text));
      return;
    }
    if (base_.Distance(current_) < 1e-9) {
      host_.reportError(QObject::tr("マウスで方向を示してから距離を入力してください"));
      return;
    }
    gp_Trsf delta;
    delta.SetTranslation(gp_Vec(gp_Dir(gp_Vec(base_, current_))) * *length);
    commit(delta);
    resetState();
    host_.viewport().setPickMode(PickMode::Objects);
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
  host_.setStatus(QObject::tr("オフセットする面（平面プロファイル）をクリック"));
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
  if (!o || o->kind() != doc::ObjectKind::Face || !geom::isPlanarFace(pick->face, &plane_)) {
    host_.reportError(QObject::tr("オフセットできるのは平面プロファイル（押し出し前の面）です"));
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
  auto result = host_.modeler().offset(objectId_, distance);
  if (!result) {
    host_.reportError(QObject::tr("オフセットに失敗しました: %1")
                          .arg(QString::fromStdString(result.error())));
  } else {
    last_ = LastOffset{result->id, result->parameterKey};
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

void MeasureTool::mouseMove(const ToolEvent& e) {
  const snap::SnapResult r = snapAt(e, start_, std::nullopt);
  if (!r.valid()) return;
  if (start_) {
    show(*start_, r.point, false);
  } else {
    host_.setStatus(QObject::tr("測りはじめの点をクリック  [%1]").arg(snapLabel(r.kind)));
  }
}

void MeasureTool::mousePress(const ToolEvent& e) {
  const snap::SnapResult r = snapAt(e, start_, std::nullopt);
  if (!r.valid()) return;
  if (!start_) {
    start_ = r.point;
    return;
  }
  show(*start_, r.point, true);
  lastDistance_ = start_->Distance(r.point);
  start_.reset();
  resetLock();
}

}  // namespace sbr::app
