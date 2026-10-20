#pragma once

#include <memory>
#include <vector>

#include <QMainWindow>

#include "Tools.h"
#include "Viewport.h"
#include "sbr/doc/Modeler.h"

class QAction;
class QActionGroup;
class QLabel;
class QLineEdit;

namespace sbr::app {

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

  // Used by tests and the command line.
  bool openFile(const QString& path);
  bool saveFile(const QString& path);
  enum class ToolId { Select, Line, Rectangle, PushPull, Move, Rotate, Offset, Curve };
  void activateTool(ToolId id);
  Tool* activeTool() const { return active_; }
  QLineEdit* measurementBox() const { return measurement_; }
  QString lastError() const { return lastError_; }

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

  doc::Modeler modeler_;
  Viewport* viewport_ = nullptr;
  PropertiesPanel* panel_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* measurementLabel_ = nullptr;
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
  Tool* active_ = nullptr;
  QActionGroup* toolGroup_ = nullptr;
  QAction* toolActions_[8] = {};
  QAction* undoAction_ = nullptr;
  QAction* redoAction_ = nullptr;
  QAction* focusOnDoubleClick_ = nullptr;
  QAction* perspectiveAction_ = nullptr;

  std::vector<doc::ObjectId> selection_;
  std::vector<ElementRef> elements_;
  QString currentPath_;
  QString lastError_;
  bool refreshPending_ = false;
};

}  // namespace sbr::app
