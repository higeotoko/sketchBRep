#pragma once

#include <memory>
#include <vector>

#include <QMainWindow>

#include "Tools.h"
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
  std::vector<doc::ObjectId> selection() const override { return selection_; }

  // Used by tests and the command line.
  bool openFile(const QString& path);
  bool saveFile(const QString& path);
  enum class ToolId { Select, Line, Rectangle, PushPull };
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
  Tool* active_ = nullptr;
  QActionGroup* toolGroup_ = nullptr;
  QAction* toolActions_[4] = {};
  QAction* undoAction_ = nullptr;
  QAction* redoAction_ = nullptr;
  QAction* focusOnDoubleClick_ = nullptr;
  QAction* perspectiveAction_ = nullptr;

  std::vector<doc::ObjectId> selection_;
  QString currentPath_;
  QString lastError_;
  bool refreshPending_ = false;
};

}  // namespace sbr::app
