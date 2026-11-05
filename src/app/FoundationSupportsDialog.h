#pragma once
#include <QDialog>
#include <QPointer>
#include <string>
#include <cstdint>
class QTableWidget;
class QLabel;
class QPushButton;
namespace sbr::app {
class MainWindow;
class Viewport;
class FoundationSupportsDialog : public QDialog {
  Q_OBJECT
 public:
  explicit FoundationSupportsDialog(MainWindow* window);
  ~FoundationSupportsDialog() override;
  void refresh();
 private:
  void highlight(bool support);
  void clear();
  MainWindow* window_;
  QPointer<Viewport> viewport_;
  QTableWidget* table_;
  QLabel* status_;
  QPushButton* supports_;
  std::uint64_t sequence_=0;
  std::string report_;
};
}
