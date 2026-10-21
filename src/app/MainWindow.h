#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <QMainWindow>

#include "Tools.h"
#include "sbr/geom/Progress.h"
#include "Viewport.h"
#include "sbr/doc/Modeler.h"

class QAction;
class QActionGroup;
class QLabel;
class QLineEdit;
class QTabBar;
class QListWidget;

namespace sbr::app {

class AiBridge;
class PropertiesPanel;
class Viewport;

class MainWindow : public QMainWindow, public ToolHost {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  // ToolHost
  doc::Modeler& modeler() override { return modeler_; }
  Viewport& viewport() override { return *viewport_; }
  void setStatus(const QString& text) override;
  void setMeasurement(const QString& label, const QString& value) override;
  void reportError(const QString& text) override;
  void setSelection(const std::vector<doc::ObjectId>& ids) override;
  void setElementSelection(const std::vector<ElementRef>& elements) override;
  std::vector<ElementRef> elementSelection() const override { return elements_; }
  std::vector<doc::ObjectId> selection() const override { return selection_; }
  doc::ObjectId editingGroup() const override { return modeler_.activeGroup(); }
  void enterGroup(const doc::ObjectId& groupId) override;
  void exitGroup() override;
  void beginTextEntry(const QString& label, const QString& text) override;

  // Used by tests and the command line.
  bool openFile(const QString& path);
  bool saveFile(const QString& path);
  enum class ToolId { Select, Line, Rectangle, PushPull, Move, Rotate, Offset, Curve, Revolve, Measure, Circle, Paint, Eraser, Arc, Dimension, Text, Section, FollowMe };
  void activateTool(ToolId id);
  Tool* activeTool() const { return active_; }
  QLineEdit* measurementBox() const { return measurement_; }
  QString lastError() const { return lastError_; }
  AiBridge* aiBridge() const { return aiBridge_.get(); }
  void setAiAccess(bool enabled);
  // AI proposal awaiting the user's decision (spec 07 §9.2).
  bool hasPendingProposal() const { return proposal_.has_value() && proposal_->state == "pending"; }
  void applyProposal();
  void rejectProposal();
  // Saved views ("scenes"), shown as tabs above the viewport.
  void addScene(const QString& name = {});
  bool goToScene(int index);
  QTabBar* sceneBar() const { return sceneBar_; }
  QListWidget* tagList() const { return tagList_; }

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  void buildUi();
  void buildMenus();
  void onDocumentChanged();
  void refreshPanels();
  void updateTitle();
  bool maybeSave();
  void commitMeasurement();
  void newDocument();
  void open();
  bool save();
  bool saveAs();
  void importStep();
  void exportStep();
  void import3dm();
  void export3dm();
  void deleteSelection();
  void selectAll();
  void groupSelection();
  void ungroupSelection();
  void makeComponent();
  void explodeSelection();
  void booleanSelection(doc::BooleanKind kind);
  bool report(const Status& status);
  QString elementSummary() const;
  void undo();
  void redo();
  void buildAiBridge();
  void syncEditContext();
  void refreshScenes();
  void refreshTags();
  void sceneMenu(int index, const QPoint& globalPos);

  doc::Modeler modeler_;
  Viewport* viewport_ = nullptr;
  PropertiesPanel* panel_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* measurementLabel_ = nullptr;
  QLabel* contextLabel_ = nullptr;
  QLineEdit* measurement_ = nullptr;
  bool userTyping_ = false;

  std::unique_ptr<SelectTool> selectTool_;
  std::unique_ptr<LineTool> lineTool_;
  std::unique_ptr<RectangleTool> rectangleTool_;
  std::unique_ptr<PushPullTool> pushPullTool_;
  std::unique_ptr<MoveTool> moveTool_;
  std::unique_ptr<RotateTool> rotateTool_;
  std::unique_ptr<OffsetTool> offsetTool_;
  std::unique_ptr<CurveTool> curveTool_;
  std::unique_ptr<RevolveTool> revolveTool_;
  std::unique_ptr<MeasureTool> measureTool_;
  std::unique_ptr<CircleTool> circleTool_;
  std::unique_ptr<PaintTool> paintTool_;
  std::unique_ptr<EraserTool> eraserTool_;
  std::unique_ptr<ArcTool> arcTool_;
  std::unique_ptr<DimensionTool> dimensionTool_;
  std::unique_ptr<TextTool> textTool_;
  std::unique_ptr<SectionTool> sectionTool_;
  std::unique_ptr<FollowMeTool> followMeTool_;
  Tool* active_ = nullptr;
  QActionGroup* toolGroup_ = nullptr;
  QAction* toolActions_[18] = {};
  QAction* undoAction_ = nullptr;
  QAction* redoAction_ = nullptr;
  QAction* focusOnDoubleClick_ = nullptr;
  QAction* perspectiveAction_ = nullptr;

  std::vector<doc::ObjectId> selection_;
  std::vector<ElementRef> elements_;
  QString currentPath_;
  QString lastError_;
  bool refreshPending_ = false;

  std::unique_ptr<AiBridge> aiBridge_;
  QAction* aiAction_ = nullptr;
  QLabel* aiLabel_ = nullptr;
  QTabBar* sceneBar_ = nullptr;
  struct Proposal {
    std::string commandsJson;
    long long revision = 0;
    std::string state;  // pending / applied / rejected / failed
    std::string error;
  };
  std::optional<Proposal> proposal_;
  QWidget* proposalBar_ = nullptr;
  Handle(geom::CancellableProgress) progress_;
  QLabel* proposalLabel_ = nullptr;
  QListWidget* tagList_ = nullptr;
  QAction* sectionVisibleAction_ = nullptr;
};

}  // namespace sbr::app
