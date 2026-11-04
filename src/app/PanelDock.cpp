#include "PanelDock.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <QAbstractButton>
#include <QApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QScopedValueRollback>
#include <QVariantAnimation>
#include <QStatusBar>
#include <QToolBar>

namespace sbr::app {
namespace {
int edgeDistance(const QRect& b, const QPoint& p, Qt::DockWidgetArea area) {
  switch (area) {
    case Qt::LeftDockWidgetArea: return std::abs(p.x() - b.left());
    case Qt::RightDockWidgetArea: return std::abs(p.x() - b.right());
    case Qt::TopDockWidgetArea: return std::abs(p.y() - b.top());
    case Qt::BottomDockWidgetArea: return std::abs(p.y() - b.bottom());
    default: return std::numeric_limits<int>::max();
  }
}

class TitleButton : public QAbstractButton {
 public:
  TitleButton(bool close, QWidget* parent) : QAbstractButton(parent), close_(close) {
    setFixedSize(24, 24);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
  }
 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor ink = palette().color(QPalette::Active, QPalette::WindowText);
    if (underMouse() || hasFocus()) {
      const QColor base = palette().color(QPalette::Window);
      p.setPen(Qt::NoPen);
      p.setBrush(close_ ? QColor("#b83b42") : base.lightness() < 128 ? base.lighter(165) : base.darker(112));
      p.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 3, 3);
      if (close_) ink = Qt::white;
    }
    p.setPen(QPen(ink, 1.25, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    if (close_) { p.drawLine(8, 8, 16, 16); p.drawLine(16, 8, 8, 16); }
    else { p.drawRect(QRectF(7, 9, 9, 8)); p.drawLine(10, 6, 19, 6); p.drawLine(19, 6, 19, 14); }
  }
 private:
  bool close_;
};

}  // namespace

// Two small surfaces can cross-fade between distant edges without allocating
// a window/image covering the entire viewport. Only this painter animates.
class DockPreview : public QWidget {
 public:
  explicit DockPreview(QWidget* parent)
      : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::NoDropShadowWindowHint),
        motion_(this) {
    setObjectName(QStringLiteral("panelDockPreview"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);
    motion_.setObjectName(QStringLiteral("dockPreviewMotion"));
    connect(&motion_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
      const qreal t = value.toReal();
      opacity_ = from_.opacity + (to_.opacity - from_.opacity) * t;
      slide_ = from_.slide + (to_.slide - from_.slide) * t;
      contraction_ = from_.contraction + (to_.contraction - from_.contraction) * t;
      update();
    });
    connect(&motion_, &QVariantAnimation::finished, this, [this] {
      if (phase_ == Entering) phase_ = Holding;
      else hideImmediately();
    });
  }

  bool matches(const QRect& geometry, Qt::DockWidgetArea edge) const {
    return this->geometry() == geometry && edge_ == edge;
  }

  void present(const QRect& geometry, Qt::DockWidgetArea edge) {
    if (this->geometry() != geometry || edge_ != edge) hideImmediately();
    edge_ = edge;
    setGeometry(geometry); // once per candidate, never once per animation frame
    if (!isVisible()) { opacity_ = 0; slide_ = 3; contraction_ = 0; }
    show();
    animate(Entering, {1, 0, 0}, 140);
  }

  void dismiss() {
    if (!isVisible() || phase_ == Exiting) return;
    if (opacity_ <= .001) { hideImmediately(); return; }
    animate(Exiting, {0, 3, 0}, 90);
  }

  void settle() {
    if (!isVisible()) return;
    if (opacity_ <= .001) { hideImmediately(); return; }
    // Preserve the current opacity if released early: no full-opacity flash.
    animate(Settling, {0, 0, 1}, 160);
  }

  void holdForPlacement() {
    motion_.stop();
    if (isVisible()) phase_ = Holding;
  }

  void hideImmediately() {
    motion_.stop();
    phase_ = Hidden;
    opacity_ = 0; slide_ = 3; contraction_ = 0;
    hide();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    if (opacity_ <= 0) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(opacity_);
    const auto mix = [](const QColor& a, const QColor& b, qreal t) {
      return QColor::fromRgbF(a.redF()*(1-t) + b.redF()*t,
                             a.greenF()*(1-t) + b.greenF()*t,
                             a.blueF()*(1-t) + b.blueF()*t);
    };
    const QColor base = palette().color(QPalette::Window);
    const QColor neutral = mix(base, palette().color(QPalette::WindowText), .5);
    QColor border = mix(neutral, palette().color(QPalette::Highlight), .18);
    QColor fill = border;
    border.setAlpha(base.lightness() < 128 ? 142 : 126);
    fill.setAlpha(base.lightness() < 128 ? 30 : 24);
    p.setPen(QPen(border, 1));
    p.setBrush(fill);
    QRectF shape = QRectF(rect()).adjusted(1, 1, -1, -1);
    const qreal inset = 3 * contraction_;
    const bool side = edge_ == Qt::LeftDockWidgetArea || edge_ == Qt::RightDockWidgetArea;
    if (side) {
      shape.adjust(0, inset, 0, -inset);
      const qreal thickness = shape.width() * (1 - .3 * contraction_);
      if (edge_ == Qt::LeftDockWidgetArea) { shape.setWidth(thickness); shape.translate(-slide_, 0); }
      else { shape.setLeft(shape.right() - thickness); shape.translate(slide_, 0); }
    } else {
      shape.adjust(inset, 0, -inset, 0);
      const qreal thickness = shape.height() * (1 - .3 * contraction_);
      if (edge_ == Qt::TopDockWidgetArea) { shape.setHeight(thickness); shape.translate(0, -slide_); }
      else { shape.setTop(shape.bottom() - thickness); shape.translate(0, slide_); }
    }
    p.drawRoundedRect(shape, 3, 3);
  }

  void hideEvent(QHideEvent* event) override {
    motion_.stop(); phase_ = Hidden; opacity_ = 0;
    QWidget::hideEvent(event);
  }

 private:
  enum Phase { Hidden, Entering, Holding, Exiting, Settling } phase_ = Hidden;
  struct Pose { qreal opacity, slide, contraction; } from_{0, 3, 0}, to_{1, 0, 0};
  void animate(Phase phase, Pose target, int duration) {
    motion_.stop();
    from_ = {opacity_, slide_, contraction_}; to_ = target; phase_ = phase;
    motion_.setDuration(duration);
    motion_.setEasingCurve(QEasingCurve::OutCubic);
    motion_.setStartValue(0.0); motion_.setEndValue(1.0);
    motion_.start();
  }
  QVariantAnimation motion_;
  Qt::DockWidgetArea edge_ = Qt::NoDockWidgetArea;
  qreal opacity_ = 0, slide_ = 3, contraction_ = 0;
};

Qt::DockWidgetArea panelDockTarget(const QRect& bounds, const QPoint& p,
                                  Qt::DockWidgetAreas allowed, Qt::DockWidgetArea previous) {
  if (!bounds.adjusted(-8, -8, 8, 8).contains(p)) return Qt::NoDockWidgetArea;
  if (previous != Qt::NoDockWidgetArea && allowed.testFlag(previous) && edgeDistance(bounds, p, previous) <= 28)
    return previous;
  Qt::DockWidgetArea chosen = Qt::NoDockWidgetArea;
  int nearest = 17;
  for (auto edge : {Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea, Qt::TopDockWidgetArea, Qt::BottomDockWidgetArea}) {
    const int distance = edgeDistance(bounds, p, edge);
    if (allowed.testFlag(edge) && distance < nearest) { chosen = edge; nearest = distance; }
  }
  return chosen;
}

class PanelTitleBar : public QWidget {
 public:
  explicit PanelTitleBar(PanelDock* dock) : QWidget(dock), dock_(dock) {
    setObjectName(QStringLiteral("panelTitleBar"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(6, 2, 3, 2); row->setSpacing(0);
    row->addStretch();
    detach_ = new TitleButton(false, this);
    detach_->setObjectName(QStringLiteral("panelFloatButton"));
    detach_->setToolTip(tr("独立表示／ドックに戻す（タイトルのダブルクリックでも切り替え）"));
    detach_->setAccessibleName(tr("独立表示を切り替え"));
    row->addWidget(detach_);
    close_ = new TitleButton(true, this);
    close_->setObjectName(QStringLiteral("panelCloseButton"));
    close_->setToolTip(tr("パネルを閉じる")); close_->setAccessibleName(tr("パネルを閉じる"));
    row->addWidget(close_);
    connect(detach_, &QAbstractButton::clicked, dock_, [dock] { dock->toggleFloating(); });
    connect(close_, &QAbstractButton::clicked, dock_, &QWidget::hide);
    connect(dock_, &QDockWidget::featuresChanged, this, [this] { refresh(); });
    connect(dock_, &QWidget::windowTitleChanged, this, [this] { update(); });
    setToolTip(tr("ドラッグして移動。端の近くで離すとドック。Ctrlを押すと吸い付きを一時無効。Escで移動を中止。"));
    refresh();
  }
  QSize sizeHint() const override { return QSize(140, 28); }
  QSize minimumSizeHint() const override { return QSize(32 + (detach_->isHidden() ? 0 : 24) + (close_->isHidden() ? 0 : 24), 28); }
 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    const QColor base = palette().color(QPalette::Window);
    p.fillRect(rect(), base.lightness() < 128 ? base.lighter(112) : base.darker(104));
    p.setPen(base.lightness() < 128 ? base.lighter(145) : base.darker(115));
    p.drawLine(rect().bottomLeft(), rect().bottomRight());
    p.setPen(palette().color(QPalette::Active, QPalette::WindowText));
    const int end = detach_->isVisible() ? detach_->x() : close_->isVisible() ? close_->x() : width() - 4;
    const QRect text(7, 0, std::max(0, end - 12), height() - 1);
    p.drawText(text, Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(dock_->windowTitle(), Qt::ElideRight, text.width()));
  }
  void mousePressEvent(QMouseEvent* e) override {
    if (e->button() == Qt::LeftButton && dock_->features().testFlag(QDockWidget::DockWidgetMovable)) {
      dock_->startDrag(e->globalPosition().toPoint(), mapTo(dock_, e->position().toPoint()));
      e->accept();
    } else e->ignore();
  }
  void mouseMoveEvent(QMouseEvent* e) override {
    dock_->moveDrag(e->globalPosition().toPoint(), e->modifiers()); e->accept();
  }
  void mouseReleaseEvent(QMouseEvent* e) override {
    if (e->button() == Qt::LeftButton) {
      // The last move can be coalesced by the window system; commit the
      // position and modifiers actually reported by the release event.
      if (dock_->dragging_) dock_->moveDrag(e->globalPosition().toPoint(), e->modifiers());
      if (e->modifiers().testFlag(Qt::ControlModifier)) dock_->clearPreview();
      dock_->endDrag(false); e->accept();
    }
    else e->ignore();
  }
  void mouseDoubleClickEvent(QMouseEvent* e) override {
    if (e->button() == Qt::LeftButton) { dock_->endDrag(true); dock_->toggleFloating(); e->accept(); }
    else e->ignore();
  }
 private:
  void refresh() {
    detach_->setVisible(dock_->features().testFlag(QDockWidget::DockWidgetFloatable));
    close_->setVisible(dock_->features().testFlag(QDockWidget::DockWidgetClosable));
    updateGeometry(); update();
  }
  PanelDock* dock_;
  TitleButton* detach_;
  TitleButton* close_;
};

PanelDock::PanelDock(const QString& title, QWidget* parent)
    : QDockWidget(title, parent), host_(qobject_cast<QMainWindow*>(parent)) {
  setTitleBarWidget(new PanelTitleBar(this));
  connect(this, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea area) {
    if (area != Qt::NoDockWidgetArea) lastArea_ = area;
  });
}

PanelDock::~PanelDock() {
  if (qApp) qApp->removeEventFilter(this);
  delete preview_.data(); delete outgoingPreview_.data();
}

QRect PanelDock::dockBounds() const {
  if (!host_) return {};
  QRect bounds = host_->contentsRect();
  if (host_->menuWidget() && host_->menuWidget()->isVisible()) bounds.setTop(host_->menuWidget()->geometry().bottom() + 1);
  if (auto* status = host_->findChild<QStatusBar*>(QString(), Qt::FindDirectChildrenOnly); status && status->isVisible())
    bounds.setBottom(status->geometry().top() - 1);
  for (auto* bar : host_->findChildren<QToolBar*>(QString(), Qt::FindDirectChildrenOnly)) {
    if (!bar->isVisible() || bar->isFloating()) continue;
    if (host_->toolBarArea(bar) == Qt::TopToolBarArea) bounds.setTop(std::max(bounds.top(), bar->geometry().bottom() + 1));
    if (host_->toolBarArea(bar) == Qt::BottomToolBarArea) bounds.setBottom(std::min(bounds.bottom(), bar->geometry().top() - 1));
  }
  return QRect(host_->mapToGlobal(bounds.topLeft()), bounds.size());
}

void PanelDock::startDrag(const QPoint& global, const QPoint& offset) {
  // A new interaction supersedes any previous panel's short settle tail.
  if (host_) for (auto* dock : host_->findChildren<PanelDock*>(QString(), Qt::FindDirectChildrenOnly)) dock->clearPreview();
  pressed_ = true; dragging_ = false;
  press_ = global; offset_ = offset; wasFloating_ = isFloating(); before_ = geometry();
  qApp->installEventFilter(this);
}

void PanelDock::moveDrag(const QPoint& global, Qt::KeyboardModifiers modifiers) {
  if (!pressed_ || !host_) return;
  if (!dragging_) {
    if ((global - press_).manhattanLength() < QApplication::startDragDistance()) return;
    if (!features().testFlag(DockWidgetFloatable) && !isFloating()) return;
    if (!isFloating()) {
      const auto tabs = host_->tabifiedDockWidgets(this);
      tabPeer_ = tabs.isEmpty() ? nullptr : tabs.front();
      setFloating(true);
      offset_.setX(std::clamp(offset_.x(), 8, std::max(8, width() - 8)));
      titleBarWidget()->grabMouse();
    }
    dragging_ = true;
  }
  move(global - offset_);
  const auto target = modifiers.testFlag(Qt::ControlModifier) ? Qt::NoDockWidgetArea
      : panelDockTarget(dockBounds(), global, allowedAreas(), target_);
  if (target == target_) return;
  target_ = target;
  if (target == Qt::NoDockWidgetArea) { clearPreview(!modifiers.testFlag(Qt::ControlModifier)); return; }
  QRect mark = dockBounds().adjusted(3, 3, -3, -3);
  if (target == Qt::LeftDockWidgetArea) mark.setWidth(36);
  if (target == Qt::RightDockWidgetArea) mark.setLeft(mark.right() - 35);
  if (target == Qt::TopDockWidgetArea) mark.setHeight(36);
  if (target == Qt::BottomDockWidgetArea) mark.setTop(mark.bottom() - 35);
  if (preview_ && preview_->isVisible() && !preview_->matches(mark, target)) {
    if (!outgoingPreview_) outgoingPreview_ = new DockPreview(host_);
    outgoingPreview_->hideImmediately();
    std::swap(preview_, outgoingPreview_);
    outgoingPreview_->setObjectName(QStringLiteral("panelDockPreviewOutgoing"));
    outgoingPreview_->dismiss();
  }
  if (!preview_) preview_ = new DockPreview(host_);
  preview_->setObjectName(QStringLiteral("panelDockPreview"));
  preview_->present(mark, target);
}

void PanelDock::clearPreview(bool animated) {
  target_ = Qt::NoDockWidgetArea;
  for (auto* preview : {preview_.data(), outgoingPreview_.data()}) {
    if (!preview) continue;
    if (animated) preview->dismiss();
    else preview->hideImmediately();
  }
}

void PanelDock::endDrag(bool cancel) {
  if (!pressed_) return;
  pressed_ = false;
  qApp->removeEventFilter(this);
  if (QWidget::mouseGrabber() == titleBarWidget()) titleBarWidget()->releaseMouse();
  const auto target = target_;
  const bool commit = dragging_ && !cancel && target != Qt::NoDockWidgetArea;
  if (commit) {
    target_ = Qt::NoDockWidgetArea;
    if (outgoingPreview_) outgoingPreview_->hideImmediately();
    if (preview_) preview_->holdForPlacement();
  } else clearPreview(!cancel);
  if (!dragging_) return;
  dragging_ = false;
  if (cancel) {
    if (!wasFloating_) {
      dockAt(lastArea_, true);
      const bool side = lastArea_ == Qt::LeftDockWidgetArea || lastArea_ == Qt::RightDockWidgetArea;
      host_->resizeDocks({this}, {side ? before_.width() : before_.height()}, side ? Qt::Horizontal : Qt::Vertical);
    }
    else setGeometry(before_);
  } else if (commit) {
    // Reparent exactly once, immediately. Keep the lightweight visual tail
    // alive across the hide/show events caused by Qt's real docking.
    QScopedValueRollback<bool> committing(committingDock_, true);
    dockAt(target);
    if (preview_) preview_->settle();
  }
}

void PanelDock::dockAt(Qt::DockWidgetArea area, bool restoreTabs) {
  if (!host_ || !allowedAreas().testFlag(area)) return;
  host_->addDockWidget(area, this);
  setFloating(false);
  if (restoreTabs && tabPeer_ && !tabPeer_->isFloating() && host_->dockWidgetArea(tabPeer_) == area)
    host_->tabifyDockWidget(tabPeer_, this);
  show(); raise();
}

void PanelDock::toggleFloating() {
  clearPreview();
  if (!host_ || !features().testFlag(DockWidgetFloatable)) return;
  if (isFloating()) {
    auto area = lastArea_;
    if (!allowedAreas().testFlag(area)) {
      for (auto candidate : {Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea, Qt::TopDockWidgetArea, Qt::BottomDockWidgetArea})
        if (allowedAreas().testFlag(candidate)) { area = candidate; break; }
    }
    dockAt(area, true);
  } else {
    const auto tabs = host_->tabifiedDockWidgets(this);
    tabPeer_ = tabs.isEmpty() ? nullptr : tabs.front();
    setFloating(true); show();
  }
}

bool PanelDock::eventFilter(QObject*, QEvent* event) {
  if (pressed_ && event->type() == QEvent::KeyPress) {
    const int key = static_cast<QKeyEvent*>(event)->key();
    if (key == Qt::Key_Escape) { endDrag(true); return true; }
    if (key == Qt::Key_Control) clearPreview();
  }
  if (pressed_ && event->type() == QEvent::ApplicationDeactivate) endDrag(true);
  return false;
}

void PanelDock::hideEvent(QHideEvent* event) {
  if (!committingDock_) clearPreview();
  if (dragging_) {
    pressed_ = dragging_ = false;
    qApp->removeEventFilter(this);
    if (QWidget::mouseGrabber() == titleBarWidget()) titleBarWidget()->releaseMouse();
  }
  QDockWidget::hideEvent(event);
}

void PanelDock::paintEvent(QPaintEvent* event) {
  QDockWidget::paintEvent(event);
  if (isFloating()) {
    QPainter p(this);
    const QColor base = palette().color(QPalette::Window);
    p.setPen(base.lightness() < 128 ? base.lighter(145) : base.darker(115));
    p.setBrush(Qt::NoBrush);
    p.drawRect(rect().adjusted(0, 0, -1, -1));
  }
}

}  // namespace sbr::app
