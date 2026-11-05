#pragma once
#include <QPointer>
#include <string>

#include "PanelDock.h"

class QCheckBox;
class QLabel;
class QTableWidget;
namespace sbr::app {
class MainWindow;
class Viewport;
// Read-only structural overview: wall quantity bars per storey and direction,
// markers from the frame check and the support paths, the checks not computed yet,
// and the same items drawn over the model. Nothing here edits the model; any model
// change clears the drawing at once until the user updates it again.
class StructureDiagnosticsPanel : public PanelDock {
  Q_OBJECT
 public:
  explicit StructureDiagnosticsPanel(MainWindow* window);
  ~StructureDiagnosticsPanel() override;
  void refresh();
  void invalidate();
  const std::string& resultJson() const { return result_; }

 private:
  void populate();
  void drawOverlay();
  MainWindow* window_;
  // The viewport can be destroyed before this panel when the window closes:
  // every drawing call goes through this guarded pointer.
  QPointer<Viewport> viewport_;
  void clearDrawing();
  QLabel* status_;
  QLabel* wallNotice_;
  QLabel* unassessed_;
  QCheckBox* overlay_;
  QCheckBox* frameMarks_;
  QCheckBox* supportMarks_;
  QTableWidget* walls_;
  QTableWidget* markers_;
  std::string result_;
  long long sequence_ = -1;
};
}  // namespace sbr::app
