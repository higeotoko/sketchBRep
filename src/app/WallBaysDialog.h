#pragma once
#include <QDialog>
#include <QPointer>
#include <cstdint>
#include <string>
#include <vector>

class QLabel;
class QComboBox;
class QTableWidget;
class QPushButton;
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
  void showSupport(const std::string& role);
  void clearSupportButtons();
  MainWindow* window_;
  QPointer<Viewport> markerViewport_;
  QComboBox* scope_;
  QTableWidget* table_;
  QLabel* status_;
  std::vector<QPushButton*> supportButtons_;
  std::uint64_t sequence_ = 0;
  std::string report_;
};
}
