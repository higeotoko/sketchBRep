#pragma once

#include <functional>
#include <vector>

#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;
class QShowEvent;
class QTimer;
class QTreeWidget;

namespace sbr::doc {
class Document;
}

namespace sbr::app {

class PermitWorkflowPanel : public QWidget {
 public:
  enum class Action { None, Boundary, North, Road, SavedRoad, Ground, SavedGround, Preview };
  PermitWorkflowPanel(doc::Document& document, std::function<void(Action)> open, QWidget* parent = nullptr);

 protected:
  void showEvent(QShowEvent* event) override;

 private:
  struct Check {
    QString item, state, details;
    QString button;
    Action action = Action::None;
    bool attention = false;
  };

  void refresh();
  void showSelectedCheck();
  void readSiteChecks();

  doc::Document& document_;
  QListWidget* phases_;
  QLabel* next_;
  QTreeWidget* checks_;
  QLabel* details_;
  QPushButton* edit_;
  QPushButton* preview_;
  QTimer* refreshTimer_;
  std::vector<Check> rows_;
  int previousPhase_ = -1;
  bool canPreview_ = false;
};

}  // namespace sbr::app
