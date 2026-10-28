#include "ToolPalette.h"

#include <algorithm>
#include <QAction>
#include <QEvent>
#include <QIconEngine>
#include <QMainWindow>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollBar>
#include <QStyle>

namespace sbr::app {
namespace {
constexpr int margin = 6;
constexpr int gap = 4;

// Palette buttons use the selected foreground on their accent background.
// The shared QAction icon keeps its normal colors in menus.
class ButtonIcon : public QIconEngine {
 public:
  explicit ButtonIcon(QIcon source) : source_(std::move(source)) {}
  QIconEngine* clone() const override { return new ButtonIcon(source_); }
  void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
    source_.paint(p, rect, Qt::AlignCenter, mode == QIcon::Disabled ? mode : state == QIcon::On ? QIcon::Selected : mode, QIcon::Off);
  }
  QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
    return source_.pixmap(size, mode == QIcon::Disabled ? mode : state == QIcon::On ? QIcon::Selected : mode, QIcon::Off);
  }
  QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
    const QSize logical(qRound(size.width() / scale), qRound(size.height() / scale));
#else
    const QSize logical = size;
#endif
    return source_.pixmap(logical, scale, mode == QIcon::Disabled ? mode : state == QIcon::On ? QIcon::Selected : mode, QIcon::Off);
  }
 private:
  QIcon source_;
};
}

ToolPalette::ToolPalette(QWidget* parent, int preferredColumns)
    : QAbstractScrollArea(parent), preferredColumns_(std::max(1, preferredColumns)) {
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
  setStyleSheet(QStringLiteral(
      "QToolButton { border: 1px solid transparent; border-radius: 4px; padding: 0px; }"
      "QToolButton:hover { background-color: palette(alternate-base); border-color: palette(mid); }"
      "QToolButton:focus { border-color: palette(highlight); }"
      "QToolButton:checked { background-color: palette(highlight); color: palette(highlighted-text); }"
      "QToolButton:disabled { background-color: transparent; color: palette(mid); }"));
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { arrange(); });
  updateMetrics();
}

QToolButton* ToolPalette::addTool(QAction* action) {
  auto* button = new QToolButton(viewport());
  button->setDefaultAction(action);
  button->setAutoRaise(true);
  button->setFocusPolicy(Qt::TabFocus);
  button->setIconSize(QSize(24, 24));
  button->setToolButtonStyle(labels_ ? Qt::ToolButtonTextUnderIcon : Qt::ToolButtonIconOnly);
  button->setAccessibleName(action->text());
  auto refresh = [this, button, action] {
    button->setIcon(QIcon(new ButtonIcon(action->icon())));
    button->setAccessibleName(action->text());
    updateMetrics();
  };
  connect(action, &QAction::changed, this, refresh);
  button->installEventFilter(this);
  buttons_.push_back(button);
  button->show();
  refresh();
  return button;
}

void ToolPalette::setShowLabels(bool on) {
  if (labels_ == on) return;
  labels_ = on;
  for (auto* button : buttons_)
    button->setToolButtonStyle(on ? Qt::ToolButtonTextUnderIcon : Qt::ToolButtonIconOnly);
  updateMetrics();
}

QSize ToolPalette::minimumSizeHint() const {
  const int bar = verticalScrollBar()->sizeHint().width();
  return QSize(cell_.width() + 2 * margin + bar, cell_.height() + 2 * margin);
}

QSize ToolPalette::sizeHint() const {
  const int count = std::max(1, static_cast<int>(buttons_.size()));
  const int columns = std::min(count, preferredColumns_);
  const int rows = (count + columns - 1) / columns;
  return QSize(2 * margin + columns * cell_.width() + (columns - 1) * gap,
               2 * margin + rows * cell_.height() + (rows - 1) * gap);
}

void ToolPalette::setCompactRow(bool fixedWidth) {
  compactRow_ = fixedWidth;
  updateMetrics();
}

void ToolPalette::updateMetrics() {
  QSize cell(labels_ ? 68 : 36, labels_ ? 48 : 36);
  for (auto* button : buttons_) cell = cell.expandedTo(button->sizeHint());
  const bool changed = cell != cell_;
  cell_ = cell;
  for (auto* button : buttons_) if (button->size() != cell_) button->setFixedSize(cell_);
  // Do not unset and reapply fixed constraints when an action is merely
  // checked. That would send native window resize requests on every click.
  const QSize hint = sizeHint();
  const QSize minimum = compactRow_ ? hint : minimumSizeHint();
  const QSize maximum = compactRow_ ? hint : QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
  const bool geometryChanged = changed || hint != previousHint_ || minimumSize() != minimum || maximumSize() != maximum;
  if (maximumSize() != maximum) setMaximumSize(maximum);
  if (minimumSize() != minimum) setMinimumSize(minimum);
  previousHint_ = hint;
  if (geometryChanged) updateGeometry();
  arrange();
  if (geometryChanged) emit metricsChanged();
}

void ToolPalette::arrange() {
  if (arranging_) return;
  arranging_ = true;
  const int count = static_cast<int>(buttons_.size());
  const QSize available = maximumViewportSize(); // independent of an old scrollbar state
  const auto columnsFor = [this, count](int width) {
    return std::max(1, std::min(std::max(1, count),
        (width - 2 * margin + gap) / (cell_.width() + gap)));
  };
  int columns = columnsFor(available.width());
  const auto heightFor = [this, count](int cols) {
    const int rows = (count + cols - 1) / cols;
    return 2 * margin + rows * (cell_.height() + gap) - (rows ? gap : 0);
  };
  int height = heightFor(columns);
  if (height > available.height()) {
    columns = columnsFor(available.width() - verticalScrollBar()->sizeHint().width());
    height = heightFor(columns);
  }
  verticalScrollBar()->setPageStep(std::max(1, available.height()));
  verticalScrollBar()->setSingleStep(cell_.height() + gap);
  verticalScrollBar()->setRange(0, std::max(0, height - available.height()));
  for (int i = 0; i < count; ++i) {
    buttons_[i]->move(margin + (i % columns) * (cell_.width() + gap),
                      margin + (i / columns) * (cell_.height() + gap) - verticalScrollBar()->value());
  }
  arranging_ = false;
}

void ToolPalette::ensureButtonVisible(QToolButton* button) {
  const QRect r = button->geometry();
  if (r.top() < margin) verticalScrollBar()->setValue(verticalScrollBar()->value() + r.top() - margin);
  else if (r.bottom() >= viewport()->height() - margin)
    verticalScrollBar()->setValue(verticalScrollBar()->value() + r.bottom() - viewport()->height() + margin + 1);
}

void ToolPalette::resizeEvent(QResizeEvent* event) {
  QAbstractScrollArea::resizeEvent(event);
  arrange();
}
void ToolPalette::scrollContentsBy(int, int) { arrange(); }
void ToolPalette::changeEvent(QEvent* event) {
  QAbstractScrollArea::changeEvent(event);
  if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) updateMetrics();
}
bool ToolPalette::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::FocusIn)
    if (auto* button = qobject_cast<QToolButton*>(watched)) ensureButtonVisible(button);
  return QAbstractScrollArea::eventFilter(watched, event);
}

ToolPaletteDock::ToolPaletteDock(const QString& title, QWidget* parent, int columns, bool compactRow)
    : QDockWidget(title, parent), tools_(new ToolPalette(this, columns)), compactRow_(compactRow) {
  setFeatures(DockWidgetMovable | DockWidgetFloatable | DockWidgetClosable);
  setWidget(tools_);
  connect(this, &QDockWidget::topLevelChanged, this, [this] { updateConstraints(); });
  connect(tools_, &ToolPalette::metricsChanged, this, [this] { updateConstraints(); });
}

void ToolPaletteDock::updateConstraints() {
  // Keep constraints on the child as prescribed by QDockWidget. In
  // particular, never carry a floating-frame minimum onto a docked panel.
  const bool fixed = compactRow_ && isFloating();
  if (tools_->property("fixedFloatingRow").toBool() != fixed) {
    tools_->setProperty("fixedFloatingRow", fixed);
    tools_->setCompactRow(fixed);
  }
}

void ToolPaletteDock::resetSize() {
  updateConstraints();
  if (isFloating()) {
    QSize wanted = sizeHint();
    if (screen()) wanted.setHeight(std::min(wanted.height(), screen()->availableGeometry().height() - 48));
    resize(wanted.expandedTo(minimumSizeHint()));
  } else if (auto* main = qobject_cast<QMainWindow*>(parentWidget())) {
    const auto area = main->dockWidgetArea(this);
    const bool side = area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea;
    main->resizeDocks({this}, {side ? sizeHint().width() : sizeHint().height()}, side ? Qt::Horizontal : Qt::Vertical);
  }
}

}  // namespace sbr::app
