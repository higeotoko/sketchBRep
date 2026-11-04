#pragma once
#include <QDialog>
#include <memory>
#include "sbr/doc/Document.h"

class QLabel;
class QLineEdit;
class QComboBox;
class QTableWidget;
class QTextBrowser;
class QTimer;
namespace sbr::app {
class MainWindow;
class WallQuantityDialog : public QDialog {
  Q_OBJECT
 public:
  explicit WallQuantityDialog(MainWindow* window);
  void refreshNow();
  void reload();
  Status applyInputs();
  Status exportCsv(const QString& path);
  const std::string& reportJson() const { return report_; }

 private:
  std::string inputs() const;
  void addWall(const std::string& wall);
  void addFloor(const std::string& floor);
  void changed();
  void useTable(const std::string& table);
  MainWindow* window_;
  std::shared_ptr<const doc::Document> snapshot_;
  long long revision_ = 0;
  bool filling_ = false;
  QLineEdit *height_, *wind_, *basis_;
  QComboBox* co_;
  QTableWidget *floors_, *walls_, *results_;
  QLabel* status_;
  QTextBrowser* notice_;
  QTimer *debounce_, *poll_;
  std::string table_, frame_, report_;
};
}  // namespace sbr::app
