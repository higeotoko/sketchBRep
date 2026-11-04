#pragma once
#include <QDialog>
#include <QPointer>
#include <cstdint>
#include <string>

class QLabel;
class QComboBox;
class QTableWidget;
namespace sbr::app {
class MainWindow;
class Viewport;
class WallBaysDialog : public QDialog {
  Q_OBJECT
 public:
  explicit WallBaysDialog(MainWindow* window);
  ~WallBaysDialog() override;
  void refresh();
 private:
  void highlight();
  MainWindow* window_;
  QPointer<Viewport> markerViewport_;
  QComboBox* scope_;
  QTableWidget* table_;
  QLabel* status_;
  std::uint64_t sequence_ = 0;
  std::string report_;
};
}
