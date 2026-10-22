#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <QMainWindow>
#include <QStringList>
#include <QTimer>

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
  enum class ToolId { Select, Line, Rectangle, PushPull, Move, Rotate, Offset, Curve, Revolve, Measure, Circle, Paint, Eraser, Arc, Dimension, Text, Section, FollowMe, Protractor, Polygon, Scale, Wall, Slab, Opening, Paste, Count };
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
  // Structural grid from typed spacings ("3600*3, 2700"); false + error
  // message when the text cannot be read or the grid fails.
  bool createGrid(const QString& xSpacings, const QString& ySpacings);
  // After opening, recipes are verified in the background (a few objects
  // per idle moment); true while that is still running.
  bool verifyingRecipes() const { return verifyTimer_ && verifyTimer_->isActive(); }
  const QStringList& recipeWarnings() const { return recipeWarnings_; }
  // Clipboard (inside this window): copy / cut the selection, paste with the
  // paste tool (follows the cursor) or in place.
  // The current view as shown (without the snap marker) as a PNG.
  bool exportImage(const QString& path);
  bool copySelection();
  bool cutSelection();
  void paste();
  bool pasteInPlace();
  // Architectural helpers behind the Tools menu (also used by tests).
  WallTool& wallTool() { return *wallTool_; }
  SlabTool& slabTool() { return *slabTool_; }
  OpeningTool& openingTool() { return *openingTool_; }
  // Openings into the selected wall: the face picked (face selection) or the
  // object's largest vertical face.
  bool placeOpeningsOnSelection(int count, double width, double height, double sill,
                                std::optional<double> spacing = std::nullopt);
  // "1F=0, 2F=3000" (units allowed) -> attribute level on the selection
  // (everything when nothing is selected).
  bool assignLevelsFromText(const QString& text);
  // Aligns the selection on a world axis (0 x / 1 y / 2 z) to the first
  // selected object's min (-1) / centre (0) / max (1).
  bool alignSelection(int axis, int edge);
  // Opening `index` (1, 2, ... as in Modeler::openings) of a host.
  bool resizeOpening(const doc::ObjectId& host, int index, double width, double height);

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
  void showGridDialog();
  void showWallSettingsDialog();
  void showSlabSettingsDialog();
  void showOpeningSettingsDialog();
  void showOpeningsDialog();
  void showLevelsDialog();
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
  std::unique_ptr<ProtractorTool> protractorTool_;
  std::unique_ptr<PasteTool> pasteTool_;
  std::unique_ptr<PolygonTool> polygonTool_;
  std::unique_ptr<ScaleTool> scaleTool_;
  std::unique_ptr<WallTool> wallTool_;
  std::unique_ptr<SlabTool> slabTool_;
  std::unique_ptr<OpeningTool> openingTool_;
  doc::Modeler::Clipboard clipboard_;
  QTimer* verifyTimer_ = nullptr;
  std::size_t verifyCursor_ = 0;
  QStringList recipeWarnings_;
  void startRecipeVerification();
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
  QAction* toolActions_[static_cast<int>(ToolId::Count)] = {};
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
    std::string changesJson;  // the change set shown to the user
  };
  std::optional<Proposal> proposal_;
  QWidget* proposalBar_ = nullptr;
  Handle(geom::CancellableProgress) progress_;
  QLabel* proposalLabel_ = nullptr;
  QListWidget* tagList_ = nullptr;
  QAction* sectionVisibleAction_ = nullptr;
};

}  // namespace sbr::app
