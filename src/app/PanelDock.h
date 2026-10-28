#pragma once

#include <QDockWidget>
#include <QPointer>

class QMainWindow;

namespace sbr::app {

// Logical pixels: deliberate edge docking, with a little hysteresis so a
// hand tremor does not flash the preview on and off.
Qt::DockWidgetArea panelDockTarget(const QRect& bounds, const QPoint& position,
                                  Qt::DockWidgetAreas allowed, Qt::DockWidgetArea previous = Qt::NoDockWidgetArea);

class PanelTitleBar;

// Standard Qt docking/resizing, with a themed title bar and a lightweight
// edge preview. Dragging never rearranges the model view until release.
class PanelDock : public QDockWidget {
  Q_OBJECT
 public:
  explicit PanelDock(const QString& title, QWidget* parent);
  ~PanelDock() override;
 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
 private:
  friend class PanelTitleBar;
  void startDrag(const QPoint& global, const QPoint& offset);
  void moveDrag(const QPoint& global, Qt::KeyboardModifiers modifiers);
  void endDrag(bool cancel);
  void toggleFloating();
  void dockAt(Qt::DockWidgetArea area, bool restoreTabs = false);
  QRect dockBounds() const;
  void clearPreview();
  QMainWindow* host_;
  QPointer<QWidget> preview_;
  QPointer<QDockWidget> tabPeer_;
  Qt::DockWidgetArea lastArea_ = Qt::RightDockWidgetArea;
  Qt::DockWidgetArea target_ = Qt::NoDockWidgetArea;
  QPoint press_, offset_;
  QRect before_;
  bool pressed_ = false;
  bool dragging_ = false;
  bool wasFloating_ = false;
};

}  // namespace sbr::app
