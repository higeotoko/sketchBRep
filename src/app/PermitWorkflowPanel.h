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
  // SelectObjects: `target` holds the ids separated by '\n' (docs/194).
  enum class Action { None, Boundary, North, Road, SavedRoad, Ground, SavedGround, Preview, Rooms, Openings, SelectRoom,
                      SelectObjects, ApplyDaylightFactors, Zone, ConfirmStage, ClearConfirmation, Outline,
                      EnergyRoute, Equipment, HeightConditions, RoadHeightPlane, NeighborHeight };
  // ConfirmStage / ClearConfirmation (docs/212): `target` holds the stage number 1-12.
  PermitWorkflowPanel(doc::Document& document, std::function<void(Action, const QString&)> open,
                      QWidget* parent = nullptr);

 protected:
  void showEvent(QShowEvent* event) override;

 private:
  struct Check {
    QString item, state, details;
    QString button;
    Action action = Action::None;
    bool attention = false;
    QString target;
  };

  void refresh();
  void showSelectedCheck();
  void readSiteChecks();
  void readRoomOpeningChecks();
  void readModelChecks(int stage);
  void showConfirmations(int phase);

  doc::Document& document_;
  QListWidget* phases_;
  QLabel* next_;
  QTreeWidget* checks_;
  QLabel* details_;
  QPushButton* edit_;
  QPushButton* preview_;
  QLabel* confirmation_;
  QPushButton* confirm_;
  bool confirmed_ = false;
  QTimer* refreshTimer_;
  std::vector<Check> rows_;
  int previousPhase_ = -1;
  bool canPreview_ = false;
};

}  // namespace sbr::app
