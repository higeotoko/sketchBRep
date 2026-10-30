#pragma once

#include <atomic>
#include <thread>
#include <map>
#include <memory>
#include <string>
#include <optional>
#include <vector>

#include <QDateTime>
#include <QLockFile>
#include <QMainWindow>
#include <QStringList>
#include <QTimer>

#include "Tools.h"
#include "sbr/geom/Progress.h"
#include "Viewport.h"
#include "sbr/doc/Modeler.h"

class QAction;
class QDockWidget;
class QActionGroup;
class QLabel;
class QLineEdit;
class QTabBar;
class QListWidget;

namespace sbr::app {

class AiBridge;
class MaterialsPanel;
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
  void setMixedSelection(const std::vector<ElementRef>& elements, const std::vector<doc::ObjectId>& whole) override;
  std::vector<doc::ObjectId> wholeSelection() const override;
  std::vector<ElementRef> elementSelection() const override { return elements_; }
  std::vector<doc::ObjectId> selection() const override { return selection_; }
  std::vector<doc::DeleteTarget> selectionTargets() const override;
  doc::ObjectId editingGroup() const override { return modeler_.activeGroup(); }
  bool confirm(const QString& title, const QString& question) override;
  void enterGroup(const doc::ObjectId& groupId) override;
  void exitGroup() override;
  void beginTextEntry(const QString& label, const QString& text) override;
  void materialPicked(const std::string& id) override;
  void showMaterials() override;

  // Used by tests and the command line.
  bool openFile(const QString& path);
  bool saveFile(const QString& path);
  enum class ToolId { Select, Line, Rectangle, PushPull, Move, Rotate, Offset, Curve, Revolve, Measure, Circle, Paint, Eraser, Arc, Dimension, Text, Section, FollowMe, Protractor, Polygon, Scale, Wall, Slab, Opening, Paste, ComponentAxes, RotatedRectangle, Member, Count };
  void activateTool(ToolId id);
  // Component placement commands on the instances among `ids`: "unique",
  // "reset", "mirror0".."mirror2" (red / green / blue) or "axes" (tool).
  void placementAction(const std::vector<doc::ObjectId>& ids, const QString& action);
  // The right-click menu for what is at `pos` (selecting it first if it is
  // not selected), as SketchUp. Built separately so it can be inspected.
  QMenu* buildContextMenu(const QPoint& pos);
  // Help > About: logo, version, what it is built on.
  void showAbout();
  Tool* activeTool() const { return active_; }
  QLineEdit* measurementBox() const { return measurement_; }
  QString lastError() const { return lastError_; }
  void clearLastError() { lastError_.clear(); }
  QString statusText() const;
  // Constraints shown in the status bar (lock / working plane / target).
  QString constraintText() const;
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
  bool verifyingRecipes() const { return verifying_; }
  const QStringList& recipeWarnings() const { return recipeWarnings_; }
  // Clipboard (inside this window): copy / cut the selection, paste with the
  // paste tool (follows the cursor) or in place.
  // The current view as shown (without the snap marker) as a PNG.
  bool exportImage(const QString& path);
  bool exportStlFile(const QString& path, double linearDeflection = 0.1);
  enum class GeometryFormat { Brep, Iges };
  bool importGeometryFile(const QString& path, GeometryFormat format);
  bool exportGeometryFile(const QString& path, GeometryFormat format);
  bool copySelection();
  bool cutSelection();
  void paste();
  bool pasteInPlace();
  // --- autosave and recovery (docs/12_recovery.md) ------------------------
  // Unsaved work is written every few minutes to a recovery file of this
  // session; a session that did not end normally leaves it behind and the
  // next start offers it back. SBR_RECOVERY_DIR overrides the folder.
  static QString recoveryDir();
  struct Recoverable {
    QString file;          // the recovery model
    QString originalPath;  // the file it was being edited as ("" = untitled)
    QDateTime saved;
  };
  // Recovery files of sessions that are no longer running (newest first).
  static std::vector<Recoverable> findRecoverable();
  static void discardRecovery(const Recoverable& r);
  // Opens it as unsaved changes to its original file (or untitled).
  bool recover(const Recoverable& r);
  // Startup: asks whether to recover what an earlier session left.
  void offerRecovery();
  bool autosaveNow();
  // What the autosave timer does: the same, written on a thread of its own.
  void autosaveInBackground();
  bool autosaving() const { return autosaving_; }
  QString recoveryFile() const { return recoveryFile_; }
  // Minutes between autosaves (0 = off) and backup generations kept on save.
  void setAutosaveMinutes(int minutes);
  int autosaveMinutes() const { return autosaveMinutes_; }
  void setBackupGenerations(int n) { backupGenerations_ = n; }
  // Ends the session the way a crash would (files stay, the lock goes): tests.
  void abandonSessionForTest();

  // Image underlay: lays the picture on the ground (10 m wide to start),
  // selects it and starts the scale tool for the two-point calibration.
  bool importUnderlay(const QString& path);

  // Materials (panel actions; also used by tests).
  MaterialsPanel* materialsPanel() const { return materialsPanel_; }
  PaintTool& paintTool() { return *paintTool_; }
  // Makes `id` the paint material and switches to the paint tool.
  void chooseMaterial(const std::string& id);
  std::optional<std::string> addPresetMaterial(int index);
  // The preset's picture to an existing material (id and assignments
  // kept); a picture of its own is replaced only when confirmed (or asked).
  bool applyPresetPicture(const std::string& id, int presetIndex, bool replaceOwn = false);
  // The selected faces (or else the selected objects) take `id`.
  bool applyMaterialToSelection(const std::string& id);
  // Gives the material the picture in `path` as its texture (embedded).
  bool setMaterialTexture(const std::string& id, const QString& path);
  // Architectural helpers behind the Tools menu (also used by tests).
  WallTool& wallTool() { return *wallTool_; }
  MemberTool& memberTool() { return *memberTool_; }
  // Tools > Member settings (section family, size search, anchor, role).
  void showMemberSettingsDialog();
  // Tools > Members > steel / column / beam: the member tool with that
  // entry's own settings.
  void startMember(const std::string& entry);
  // A grid (docs/43): its lines' names and spacings edited in a table;
  // columns and beams placed on its crossings with the member tool's
  // column / beam settings. `grid`: the grid group.
  void showGridEditDialog(const doc::ObjectId& grid);
  void showGridMembersDialog(const doc::ObjectId& grid);
  // The grid a selection means: the grid itself, or a line inside it.
  std::optional<doc::ObjectId> selectedGrid() const;
  // The panel shown while the member tool is in use: the entries, what will
  // be drawn and a way to the settings (created on first use).
  QDockWidget* memberPanel() const { return memberPanel_; }
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

  // Imports a .3dm file; when it does not come through whole, asks before
  // taking what could be read.
  void import3dmFile(const QString& path);
  bool importDxfFile(const QString& path);
  static QString describe3dmReport(const geom::Import3dmReport& report);

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  std::string elementBasis(const doc::ObjectId& id) const;
  std::map<doc::ObjectId, std::string> elementBases_;  // per object of elements_, when selected
  void buildUi();
  void buildMenus();
  void onDocumentChanged();
  void refreshPanels();
  void updateTitle();
  bool maybeSave();
  void commitMeasurement();
  void showGridDialog();
  void editMaterialDialog(const std::string& id);  // "" = new material
  void startSession();
  void clearRecovery();
  void showAutosaveDialog();
  void showWheelZoomDialog();
  void refreshConstraints();
  void showWallSettingsDialog();
  void showSlabSettingsDialog();
  void showOpeningSettingsDialog();
  void showOpeningsDialog();
  void showLevelsDialog();
  void newDocument();
  void open();
  bool save();
  bool saveAs();
  void importFile();
  void exportFile();
  void importStepFile(const QString& path);
  void importDrawingFile(const QString& path);
  void configureDwgConverter();
  void exportStepFile(const QString& path);
  void exportStl(const QString& path);
  void exportGeometry(const QString& path, GeometryFormat format);
  void export3dmFile(const QString& path);
  void deleteSelection();
  // Faces / edges of some object picked without the whole object.
  bool partOfObjectsSelected() const;
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
  void showChatGptConnection();
  void syncEditContext();
  void refreshScenes();
  void refreshTags();
  void sceneMenu(int index, const QPoint& globalPos);

  doc::Modeler modeler_;
  Viewport* viewport_ = nullptr;
  PropertiesPanel* panel_ = nullptr;
  MaterialsPanel* materialsPanel_ = nullptr;
  QDockWidget* materialsDock_ = nullptr;
  QTimer* autosaveTimer_ = nullptr;
  std::unique_ptr<QLockFile> sessionLock_;
  QString recoveryFile_;
  std::uint64_t autosavedSequence_ = 0;
  // Timed autosaves write a copy of the document on a thread of their own
  // (seconds for a large model froze the window every few minutes), to a
  // part file put in place afterwards unless the recovery file was dropped
  // meanwhile (saved, closed: the generation changed).
  std::thread autosaveThread_;
  bool autosaving_ = false;
  int recoveryGeneration_ = 0;
  int autosaveMinutes_ = 5;
  int backupGenerations_ = 3;
  QLabel* status_ = nullptr;
  QLabel* measurementLabel_ = nullptr;
  QLabel* constraintLabel_ = nullptr;
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
  std::unique_ptr<RotatedRectangleTool> rotatedRectangleTool_;
  std::unique_ptr<ScaleTool> scaleTool_;
  std::unique_ptr<ComponentAxesTool> componentAxesTool_;
  std::unique_ptr<WallTool> wallTool_;
  std::unique_ptr<MemberTool> memberTool_;
  std::unique_ptr<SlabTool> slabTool_;
  std::unique_ptr<OpeningTool> openingTool_;
  doc::Modeler::Clipboard clipboard_;
  // Recipes are checked on a thread of their own after opening a file (a
  // wall with many openings took seconds, which froze the window).
  std::thread verifyThread_;
  std::shared_ptr<std::atomic<bool>> verifyCancel_;
  bool verifying_ = false;
  QStringList recipeWarnings_;
  void startRecipeVerification();
  void stopRecipeVerification();
  // Elements picked (their owners become the object selection), without
  // refreshing the panels.
  void takeElements(const std::vector<ElementRef>& elements);
  // A preset's picture embedded in the document, as a material texture.
  Result<doc::MaterialTexture> presetTexture(int index);
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
  QAction* sectionPlanesAction_ = nullptr;
  QAction* sectionFillAction_ = nullptr;
  QDockWidget* sectionDock_ = nullptr;
  QDockWidget* memberPanel_ = nullptr;
  QLabel* memberSummary_ = nullptr;
  void showMemberPanel();
  void refreshMemberPanel();
  bool hadSections_ = false;
};

}  // namespace sbr::app
