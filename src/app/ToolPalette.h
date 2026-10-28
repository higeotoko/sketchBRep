#pragma once

#include <QAbstractScrollArea>
#include <QDockWidget>
#include <QToolButton>
#include <vector>

class QAction;

namespace sbr::app {

// Fixed-size buttons in reading order. Resize changes columns, never the
// button size. Only vertical scrolling is allowed, including keyboard focus.
class ToolPalette : public QAbstractScrollArea {
  Q_OBJECT
 public:
  explicit ToolPalette(QWidget* parent = nullptr, int preferredColumns = 3);
  QToolButton* addTool(QAction* action);
  void setShowLabels(bool on);
  bool showLabels() const { return labels_; }
  QSize cellSize() const { return cell_; }
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;
  void ensureButtonVisible(QToolButton* button);
  void setCompactRow(bool fixedWidth);
 signals:
  void metricsChanged();
 protected:
  void resizeEvent(QResizeEvent* event) override;
  void scrollContentsBy(int dx, int dy) override;
  void changeEvent(QEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
 private:
  void updateMetrics();
  void arrange();
  std::vector<QToolButton*> buttons_;
  QSize cell_{36, 36};
  QSize previousHint_;
  int preferredColumns_;
  bool labels_ = false;
  bool compactRow_ = false;
  bool arranging_ = false;
};

// Constraints belong to the content, so Qt can account for docked vs
// floating title bars. A four-command toolbox fits its contents when floating.
class ToolPaletteDock : public QDockWidget {
  Q_OBJECT
 public:
  ToolPaletteDock(const QString& title, QWidget* parent, int columns, bool compactRow = false);
  ToolPalette* tools() const { return tools_; }
  void resetSize();
 private:
  void updateConstraints();
  ToolPalette* tools_;
  bool compactRow_;
};

}  // namespace sbr::app
