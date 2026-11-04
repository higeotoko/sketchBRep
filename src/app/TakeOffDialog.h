#pragma once
#include <QDialog>
#include <future>
#include <memory>

#include "sbr/doc/Document.h"
class QComboBox;
class QListWidget;
class QCheckBox;
class QLabel;
class QTableWidget;
class QTimer;
namespace sbr::app {
class TakeOffDialog : public QDialog {
  Q_OBJECT
 public:
  explicit TakeOffDialog(const doc::Document& document, QWidget* parent = nullptr);
  ~TakeOffDialog() override;
  void refreshNow();
  const std::string& reportJson() const { return report_; }
  Status exportCsv(const QString& path);

 private:
  std::string options() const;
  void changed();
  void launch();
  void collect();
  void populate();
  std::shared_ptr<const doc::Document> snapshot_;
  QComboBox* within_;
  QListWidget* tags_;
  QCheckBox* visible_;
  QLabel* summary_;
  QTableWidget* table_;
  QTimer* timer_;
  QTimer* poll_;
  std::future<Result<std::string>> job_;
  std::string report_;
  int generation_ = 0, jobGeneration_ = 0;
};
}  // namespace sbr::app
