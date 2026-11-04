#pragma once
#include <atomic>
#include <future>
#include <memory>
#include <string>

#include "PanelDock.h"
#include "sbr/Types.h"

class QLabel;
class QTableWidget;
class QCheckBox;
class QTimer;
namespace sbr::app {
class MainWindow;
class FrameCheckPanel : public PanelDock {
  Q_OBJECT
 public:
  explicit FrameCheckPanel(MainWindow* window);
  ~FrameCheckPanel() override;
  void invalidate();
  bool computing() const { return dirty_ || job_.valid(); }
  const std::string& resultJson() const { return result_; }
  bool exportCsv(const QString& path) const;

 private:
  void launch();
  void collect();
  void populate();
  MainWindow* window_;
  QLabel* status_;
  QTableWidget* table_;
  QCheckBox* issues_;
  QTimer* debounce_;
  QTimer* poll_;
  std::future<Result<std::string>> job_;
  std::shared_ptr<std::atomic<bool>> cancel_;
  long long epoch_ = 0, jobEpoch_ = 0, jobRevision_ = 0;
  bool dirty_ = true;
  std::string result_;
};
}  // namespace sbr::app
