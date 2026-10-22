#include "MainWindow.h"

#include <algorithm>
#include <map>

#include <QAction>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QImage>
#include <QFile>
#include <QLocale>
#include <QStandardPaths>
#include <QUuid>
#include <QRegularExpression>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDockWidget>
#include <QFileDialog>
#include <QDialog>
#include <QElapsedTimer>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QInputDialog>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QMenu>
#include <QStatusBar>
#include <QTabBar>
#include <QToolButton>
#include <QTimer>
#include <QToolBar>

#include <BRep_Builder.hxx>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QPlainTextEdit>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <nlohmann/json.hpp>

#include "AiBridge.h"
#include "Messages.h"
#include "ProgressDialog.h"
#include "Cursors.h"
#include "MaterialsPanel.h"
#include "PropertiesPanel.h"
#include "Viewport.h"
#include "sbr/Units.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/FaceRef.h"
#include "sbr/geom/Measure.h"

namespace sbr::app {
namespace {
std::string utf8(const QString& s) { return s.toUtf8().toStdString(); }
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  selectTool_ = std::make_unique<SelectTool>(*this);
  lineTool_ = std::make_unique<LineTool>(*this);
  rectangleTool_ = std::make_unique<RectangleTool>(*this);
  pushPullTool_ = std::make_unique<PushPullTool>(*this);
  moveTool_ = std::make_unique<MoveTool>(*this);
  rotateTool_ = std::make_unique<RotateTool>(*this);
  offsetTool_ = std::make_unique<OffsetTool>(*this);
  curveTool_ = std::make_unique<CurveTool>(*this);
  revolveTool_ = std::make_unique<RevolveTool>(*this);
  measureTool_ = std::make_unique<MeasureTool>(*this);
  protractorTool_ = std::make_unique<ProtractorTool>(*this);
  pasteTool_ = std::make_unique<PasteTool>(*this);
  polygonTool_ = std::make_unique<PolygonTool>(*this);
  scaleTool_ = std::make_unique<ScaleTool>(*this);
  componentAxesTool_ = std::make_unique<ComponentAxesTool>(*this);
  wallTool_ = std::make_unique<WallTool>(*this);
  slabTool_ = std::make_unique<SlabTool>(*this);
  openingTool_ = std::make_unique<OpeningTool>(*this);
  pasteTool_->setClipboard(&clipboard_);
  pasteTool_->setOnPlaced([this](const std::vector<doc::ObjectId>&) {
    // Leave the paste tool after the current event has been handled.
    QTimer::singleShot(0, this, [this] {
      if (active_ == pasteTool_.get()) activateTool(ToolId::Select);
    });
  });
  circleTool_ = std::make_unique<CircleTool>(*this);
  paintTool_ = std::make_unique<PaintTool>(*this);
  eraserTool_ = std::make_unique<EraserTool>(*this);
  arcTool_ = std::make_unique<ArcTool>(*this);
  dimensionTool_ = std::make_unique<DimensionTool>(*this);
  textTool_ = std::make_unique<TextTool>(*this);
  sectionTool_ = std::make_unique<SectionTool>(*this);
  followMeTool_ = std::make_unique<FollowMeTool>(*this);

  buildUi();
  // Long Booleans / push-pulls show a cancellable progress dialog.
  progress_ = new GuiProgress(this);
  geom::setProgressIndicator(progress_);
  buildAiBridge();
  buildMenus();
  modeler_.document().addListener([this] { onDocumentChanged(); });
  activateTool(ToolId::Rectangle);
  if (QSettings().value(QStringLiteral("ai/bridgeEnabled"), false).toBool()) aiAction_->setChecked(true);
  refreshPanels();
  updateTitle();
  startSession();
}

MainWindow::~MainWindow() {
  geom::setProgressIndicator(nullptr);
  // A normal end leaves no recovery file behind (a crash does).
  if (sessionLock_) clearRecovery();
  // Tools reference the viewport; drop them before child widgets go away.
  if (active_) active_->deactivate();
  viewport_->setTool(nullptr);
}

void MainWindow::buildUi() {
  viewport_ = new Viewport(modeler_, this);
  setCentralWidget(viewport_);

  // Scene tabs (saved views) above the viewport.
  QToolBar* scenes = new QToolBar(tr("シーン"), this);
  scenes->setObjectName(QStringLiteral("scenesBar"));
  scenes->setMovable(false);
  auto* addSceneButton = new QToolButton;
  addSceneButton->setText(tr("＋ シーン"));
  addSceneButton->setToolTip(tr("今のビューをシーンとして保存"));
  connect(addSceneButton, &QToolButton::clicked, this, [this] { addScene(); });
  sceneBar_ = new QTabBar;
  sceneBar_->setDrawBase(false);
  sceneBar_->setExpanding(false);
  sceneBar_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(sceneBar_, &QTabBar::tabBarClicked, this, [this](int index) { goToScene(index); });
  connect(sceneBar_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    const int index = sceneBar_->tabAt(pos);
    if (index >= 0) sceneMenu(index, sceneBar_->mapToGlobal(pos));
  });
  scenes->addWidget(sceneBar_);
  scenes->addWidget(addSceneButton);
  addToolBar(Qt::TopToolBarArea, scenes);

  panel_ = new PropertiesPanel;
  auto* dock = new QDockWidget(tr("オブジェクト / 属性"), this);
  dock->setObjectName(QStringLiteral("propertiesDock"));
  dock->setWidget(panel_);
  dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
  addDockWidget(Qt::RightDockWidgetArea, dock);
  resizeDocks({dock}, {360}, Qt::Horizontal);

  // Tags: tick to show / hide everything with that tag (SketchUp's Tags panel).
  tagList_ = new QListWidget;
  auto* tagDock = new QDockWidget(tr("タグ"), this);
  tagDock->setObjectName(QStringLiteral("tagsDock"));
  tagDock->setWidget(tagList_);
  tagDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                       QDockWidget::DockWidgetClosable);
  addDockWidget(Qt::RightDockWidgetArea, tagDock);
  connect(tagList_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
    modeler_.setTagVisible(utf8(item->data(Qt::UserRole).toString()), item->checkState() == Qt::Checked);
    updateTitle();
  });

  // Materials: pick one to paint with (SketchUp's Materials panel).
  materialsPanel_ = new MaterialsPanel;
  materialsDock_ = new QDockWidget(tr("素材"), this);
  materialsDock_->setObjectName(QStringLiteral("materialsDock"));
  materialsDock_->setWidget(materialsPanel_);
  materialsDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                              QDockWidget::DockWidgetClosable);
  addDockWidget(Qt::RightDockWidgetArea, materialsDock_);
  tabifyDockWidget(tagDock, materialsDock_);
  materialsDock_->raise();
  connect(materialsPanel_, &MaterialsPanel::materialChosen, this, [this](const std::string& id) { chooseMaterial(id); });
  connect(materialsPanel_, &MaterialsPanel::presetRequested, this, [this](int i) { addPresetMaterial(i); });
  connect(materialsPanel_, &MaterialsPanel::createRequested, this, [this] { editMaterialDialog({}); });
  connect(materialsPanel_, &MaterialsPanel::editRequested, this, [this](const std::string& id) { editMaterialDialog(id); });
  connect(materialsPanel_, &MaterialsPanel::deleteRequested, this, [this](const std::string& id) {
    const doc::Material* m = modeler_.document().findMaterial(id);
    if (!m) return;
    if (QMessageBox::question(this, tr("素材を削除"),
                              tr("「%1」を削除しますか？使っている面・部材は継承に戻ります（元に戻すで復元できます）")
                                  .arg(QString::fromStdString(m->name))) != QMessageBox::Yes) {
      return;
    }
    if (report(modeler_.deleteMaterial(id)) && paintTool_->material() == id) chooseMaterial({});
  });
  connect(materialsPanel_, &MaterialsPanel::applyRequested, this,
          [this](const std::string& id) { applyMaterialToSelection(id); });

  status_ = new QLabel;
  status_->setMinimumWidth(200);
  measurementLabel_ = new QLabel;
  measurement_ = new QLineEdit;
  measurement_->setFixedWidth(200);
  measurement_->setPlaceholderText(tr("数値入力"));
  contextLabel_ = new QLabel;
  contextLabel_->setStyleSheet(QStringLiteral("QLabel { background: #fff3c4; padding: 0 6px; }"));
  contextLabel_->hide();
  statusBar()->addWidget(contextLabel_);
  statusBar()->addWidget(status_, 1);
  // AI proposal banner: what the AI suggests, with Apply / Reject.
  proposalBar_ = new QWidget;
  auto* proposalLayout = new QHBoxLayout(proposalBar_);
  proposalLayout->setContentsMargins(6, 0, 6, 0);
  proposalLabel_ = new QLabel;
  auto* applyButton = new QPushButton(tr("適用"));
  auto* rejectButton = new QPushButton(tr("却下"));
  applyButton->setObjectName(QStringLiteral("proposalApply"));
  rejectButton->setObjectName(QStringLiteral("proposalReject"));
  proposalLayout->addWidget(proposalLabel_);
  proposalLayout->addWidget(applyButton);
  proposalLayout->addWidget(rejectButton);
  proposalBar_->setStyleSheet(QStringLiteral("QWidget { background: #e8f0ff; }"));
  proposalBar_->hide();
  connect(applyButton, &QPushButton::clicked, this, &MainWindow::applyProposal);
  connect(rejectButton, &QPushButton::clicked, this, &MainWindow::rejectProposal);
  statusBar()->addPermanentWidget(proposalBar_);

  aiLabel_ = new QLabel;
  aiLabel_->setStyleSheet(QStringLiteral("QLabel { background: #dbeafe; padding: 0 6px; }"));
  aiLabel_->hide();
  statusBar()->addPermanentWidget(aiLabel_);
  constraintLabel_ = new QLabel;
  constraintLabel_->setObjectName(QStringLiteral("constraintLabel"));
  constraintLabel_->setStyleSheet(QStringLiteral("QLabel { color: #1a5fb4; padding: 0 6px; }"));
  constraintLabel_->setToolTip(tr("今かかっている固定（対象・作業面・方向ロック）"));
  constraintLabel_->hide();
  statusBar()->addPermanentWidget(constraintLabel_);
  statusBar()->addPermanentWidget(measurementLabel_);
  statusBar()->addPermanentWidget(measurement_);

  connect(viewport_, &Viewport::measurementChar, this, [this](QChar c) {
    if (!userTyping_) measurement_->clear();
    userTyping_ = true;
    measurement_->insert(QString(c));
  });
  connect(viewport_, &Viewport::measurementBackspace, this, [this] {
    if (userTyping_) measurement_->backspace();
  });
  connect(viewport_, &Viewport::measurementCommit, this, &MainWindow::commitMeasurement);
  connect(viewport_, &Viewport::toolInput, this, &MainWindow::refreshConstraints);
  connect(measurement_, &QLineEdit::returnPressed, this, &MainWindow::commitMeasurement);
  connect(measurement_, &QLineEdit::textEdited, this, [this] { userTyping_ = true; });
  connect(viewport_, &Viewport::escapePressed, this, [this] {
    userTyping_ = false;
    if (active_ == selectTool_.get() && !modeler_.activeGroup().empty()) {
      exitGroup();
      return;
    }
    if (active_ && active_->stepBack()) {  // one stage back first
      refreshConstraints();
      return;
    }
    if (active_ == pasteTool_.get()) {  // nothing pasted: back to selecting
      activateTool(ToolId::Select);
      return;
    }
    if (active_) {
      active_->cancel();
      active_->activate();
    }
    refreshConstraints();
  });
  connect(viewport_, &Viewport::focusRequested, this, [this](const QPoint&) {
    if (!focusOnDoubleClick_->isChecked()) return;
    if (selection_.empty()) {
      viewport_->fitAll();
    } else {
      viewport_->fitObjects(selection_);
    }
  });

  connect(panel_, &PropertiesPanel::selectionChanged, this,
          [this](const std::vector<doc::ObjectId>& ids) { setSelection(ids); });
  connect(panel_, &PropertiesPanel::parameterEdited, this,
          [this](const doc::ObjectId& id, const std::string& key, double value) {
            // Defer so the spin box is not destroyed inside its own signal.
            QTimer::singleShot(0, this, [this, id, key, value] {
              const Status s = modeler_.setParameter(id, key, value);
              if (!s) {
                reportError(tr("パラメータを変更できません: %1").arg(QString::fromStdString(s.message())));
                refreshPanels();
              }
            });
          });
  connect(panel_, &PropertiesPanel::renameRequested, this,
          [this](const doc::ObjectId& id, const QString& name) {
            const doc::Object* o = modeler_.document().find(id);
            if (!o || o->name == utf8(name)) return;
            QTimer::singleShot(0, this, [this, id, name] { modeler_.rename(id, utf8(name)); });
          });
  connect(panel_, &PropertiesPanel::annotationTextEdited, this, [this](const doc::ObjectId& id, const QString& text) {
    const doc::Object* o = modeler_.document().find(id);
    const auto label = o ? doc::annotationLabel(o->recipe) : std::nullopt;
    if (!label) return;
    const auto* d = std::get_if<doc::LinearDimension>(&o->recipe.base);
    if ((d && d->text == utf8(text)) || (!d && label->text == utf8(text))) return;
    QTimer::singleShot(0, this, [this, id, text] { report(modeler_.setAnnotationText(id, utf8(text))); });
  });
  connect(panel_, &PropertiesPanel::tagEdited, this, [this](const doc::ObjectId& id, const QString& tag) {
    const doc::Object* o = modeler_.document().find(id);
    if (!o || o->layer == utf8(tag)) return;
    QTimer::singleShot(0, this, [this, id, tag] { report(modeler_.setLayer(id, utf8(tag))); });
  });
  connect(panel_, &PropertiesPanel::colorEdited, this,
          [this](const doc::ObjectId& id, const doc::Color& color) {
            QTimer::singleShot(0, this, [this, id, color] { report(modeler_.setColor(id, color)); });
          });
  connect(panel_, &PropertiesPanel::positionEdited, this,
          [this](const doc::ObjectId& id, double x, double y, double z) {
            QTimer::singleShot(0, this, [this, id, x, y, z] {
              report(modeler_.setPosition(id, gp_Vec(x, y, z)));
            });
          });
  connect(panel_, &PropertiesPanel::definitionParameterEdited, this,
          [this](const std::string& defId, const doc::ObjectId& objectId, const std::string& key,
                 double value) {
            QTimer::singleShot(0, this, [this, defId, objectId, key, value] {
              if (!report(modeler_.setDefinitionParameter(defId, objectId, key, value))) refreshPanels();
            });
          });
  connect(panel_, &PropertiesPanel::placementParameterEdited, this,
          [this](const doc::ObjectId& instanceId, const doc::ObjectId& objectId, const std::string& key,
                 double value) {
            QTimer::singleShot(0, this, [this, instanceId, objectId, key, value] {
              if (!report(modeler_.setPlacementParameter(instanceId, objectId, key, value))) refreshPanels();
            });
          });
  connect(panel_, &PropertiesPanel::placementAction, this,
          [this](const doc::ObjectId& instanceId, const QString& action) {
            QTimer::singleShot(0, this, [this, instanceId, action] { placementAction({instanceId}, action); });
          });
  connect(panel_, &PropertiesPanel::definitionRenamed, this,
          [this](const std::string& defId, const QString& name) {
            const doc::ComponentDefinition* d = modeler_.document().findDefinition(defId);
            if (!d || d->name == utf8(name)) return;
            QTimer::singleShot(0, this, [this, defId, name] {
              report(modeler_.renameDefinition(defId, utf8(name)));
            });
          });
  connect(panel_, &PropertiesPanel::curvePointEdited, this,
          [this](const doc::ObjectId& id, int index, double x, double y, double z) {
            QTimer::singleShot(0, this, [this, id, index, x, y, z] {
              const doc::Object* o = modeler_.document().find(id);
              const auto* curve = o ? std::get_if<doc::InterpolatedCurve>(&o->recipe.base) : nullptr;
              if (!curve || index >= static_cast<int>(curve->points.size())) return;
              std::vector<gp_Pnt> points = curve->points;
              points[static_cast<std::size_t>(index)] = gp_Pnt(x, y, z);
              if (!report(modeler_.setCurvePoints(id, points))) refreshPanels();
            });
          });
  connect(panel_, &PropertiesPanel::openingSizeEdited, this,
          [this](const doc::ObjectId& id, int index, double width, double height) {
            QTimer::singleShot(0, this, [this, id, index, width, height] {
              if (!resizeOpening(id, index, width, height)) refreshPanels();
            });
          });
  connect(panel_, &PropertiesPanel::lockToggled, this, [this](const doc::ObjectId& id, bool locked) {
    QTimer::singleShot(0, this, [this, id, locked] { report(modeler_.setLocked({id}, locked)); });
  });
  connect(panel_, &PropertiesPanel::visibilityToggled, this,
          [this](const doc::ObjectId& id, bool visible) {
            QTimer::singleShot(0, this, [this, id, visible] { modeler_.setVisible(id, visible); });
          });
}

void MainWindow::buildMenus() {
  // --- File
  QMenu* file = menuBar()->addMenu(tr("ファイル(&F)"));
  file->addAction(tr("新規(&N)"), QKeySequence::New, this, &MainWindow::newDocument);
  file->addAction(tr("開く(&O)..."), QKeySequence::Open, this, &MainWindow::open);
  file->addAction(tr("保存(&S)"), QKeySequence::Save, this, &MainWindow::save);
  file->addAction(tr("名前を付けて保存(&A)..."), QKeySequence::SaveAs, this, &MainWindow::saveAs);
  file->addSeparator();
  file->addAction(tr("画像を下敷きとして読み込む..."), this, [this] {
    const QString path = QFileDialog::getOpenFileName(this, tr("下敷きの画像"), QString(),
                                                      tr("画像 (*.png *.jpg *.jpeg *.bmp *.gif)"));
    if (!path.isEmpty()) importUnderlay(path);
  });
  file->addAction(tr("STEP インポート..."), this, &MainWindow::importStep);
  file->addAction(tr("STEP エクスポート..."), this, &MainWindow::exportStep);
  file->addAction(tr("Rhino 3DM インポート..."), this, &MainWindow::import3dm);
  file->addAction(tr("Rhino 3DM エクスポート..."), this, &MainWindow::export3dm);
  file->addAction(tr("画像エクスポート (PNG)..."), this, [this] {
    QString path = QFileDialog::getSaveFileName(this, tr("画像エクスポート"), QString(), tr("PNG 画像 (*.png)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) path += QStringLiteral(".png");
    exportImage(path);
  });
  file->addSeparator();
  file->addAction(tr("自動保存とバックアップの設定..."), this, &MainWindow::showAutosaveDialog);
  file->addAction(tr("終了(&X)"), QKeySequence::Quit, this, &QWidget::close);

  // --- Edit
  QMenu* edit = menuBar()->addMenu(tr("編集(&E)"));
  undoAction_ = edit->addAction(tr("元に戻す"), QKeySequence::Undo, this, &MainWindow::undo);
  redoAction_ = edit->addAction(tr("やり直し"), this, &MainWindow::redo);
  redoAction_->setShortcuts({QKeySequence(tr("Ctrl+Y")), QKeySequence(tr("Ctrl+Shift+Z"))});
  edit->addSeparator();
  edit->addSeparator();
  edit->addAction(tr("切り取り"), QKeySequence::Cut, this, &MainWindow::cutSelection);
  edit->addAction(tr("コピー"), QKeySequence::Copy, this, &MainWindow::copySelection);
  edit->addAction(tr("貼り付け"), QKeySequence::Paste, this, &MainWindow::paste);
  edit->addAction(tr("同じ位置に貼り付け"), QKeySequence(tr("Ctrl+Shift+V")), this, &MainWindow::pasteInPlace);
  edit->addAction(tr("削除"), QKeySequence::Delete, this, &MainWindow::deleteSelection);
  edit->addAction(tr("ロック"), QKeySequence(tr("Ctrl+L")), this, [this] {
    if (report(modeler_.setLocked(selection_, true))) setSelection({});
  });
  edit->addAction(tr("ロックをすべて解除"), this, [this] {
    std::vector<doc::ObjectId> locked;
    for (const doc::Object& o : modeler_.document().objects()) {
      if (o.locked) locked.push_back(o.id);
    }
    report(modeler_.setLocked(locked, false));
  });
  edit->addAction(tr("穴をふさぐ（開いた形状をソリッドに）"), this, [this] {
    if (report(modeler_.fillHoles(selection_))) setStatus(tr("穴をふさぎました"));
  });
  edit->addAction(tr("面の向きを反転（表裏）"), this, [this] { report(modeler_.reverseFaces(selection_)); });
  edit->addAction(tr("すべて選択"), QKeySequence::SelectAll, this, &MainWindow::selectAll);
  edit->addAction(tr("選択を非表示"), QKeySequence(tr("Ctrl+H")), this, [this] {
    const std::vector<doc::ObjectId> ids = selection();
    if (ids.empty()) {
      reportError(tr("隠す物を選択してください"));
      return;
    }
    const Status s = modeler_.hide(ids);
    if (!s) reportError(QString::fromStdString(s.message()));
    setSelection({});
  });
  edit->addAction(tr("すべて表示"), QKeySequence(tr("Ctrl+Shift+H")), this, [this] {
    const Status s = modeler_.unhideAll();
    if (!s) reportError(QString::fromStdString(s.message()));
  });
  edit->addAction(tr("ガイドをすべて削除"), this, [this] {
    const Status s = modeler_.clearGuides();
    if (!s) reportError(QString::fromStdString(s.message()));
  });
  edit->addAction(tr("選択解除"), QKeySequence(tr("Ctrl+T")), this, [this] { setSelection({}); });
  edit->addSeparator();
  edit->addAction(tr("グループ化"), QKeySequence(tr("Ctrl+G")), this, &MainWindow::groupSelection);
  edit->addAction(tr("グループ解除"), QKeySequence(tr("Ctrl+Shift+G")), this,
                  &MainWindow::ungroupSelection);
  edit->addAction(tr("コンポーネントを作成"), QKeySequence(tr("Ctrl+K")), this,
                  &MainWindow::makeComponent);
  edit->addAction(tr("分解（コンポーネント → 個別オブジェクト）"), this,
                  &MainWindow::explodeSelection);
  QMenu* placement = edit->addMenu(tr("コンポーネントの配置"));
  placement->addAction(tr("この配置だけ独立"), this, [this] { placementAction(selection_, QStringLiteral("unique")); });
  placement->addAction(tr("この配置だけの寸法を戻す"), this,
                       [this] { placementAction(selection_, QStringLiteral("reset")); });
  placement->addSeparator();
  placement->addAction(tr("鏡像: 赤軸方向"), this, [this] { placementAction(selection_, QStringLiteral("mirror0")); });
  placement->addAction(tr("鏡像: 緑軸方向"), this, [this] { placementAction(selection_, QStringLiteral("mirror1")); });
  placement->addAction(tr("鏡像: 青軸方向"), this, [this] { placementAction(selection_, QStringLiteral("mirror2")); });
  placement->addSeparator();
  placement->addAction(tr("軸を変更（原点・赤軸）"), this, [this] { placementAction(selection_, QStringLiteral("axes")); });

  // --- Solid tools
  QMenu* solid = menuBar()->addMenu(tr("ソリッド(&S)"));
  solid->addAction(tr("和（結合）"), this, [this] { booleanSelection(doc::BooleanKind::Union); });
  solid->addAction(tr("差（最初に選択したものから他を引く）"), this,
                   [this] { booleanSelection(doc::BooleanKind::Subtract); });
  solid->addAction(tr("積（共通部分）"), this,
                   [this] { booleanSelection(doc::BooleanKind::Intersect); });

  // --- Freeform (Phase 3)
  QMenu* freeform = menuBar()->addMenu(tr("曲面(&U)"));
  freeform->addAction(tr("ロフト（選択順の断面をつなぐ）"), this, [this] {
    auto id = modeler_.loftObjects(selection_, true);
    if (!id) {
      reportError(tr("ロフトに失敗しました: %1").arg(QString::fromStdString(id.error())));
      return;
    }
    setSelection({id.value()});
  });
  freeform->addAction(tr("回転体（面 → 軸の 2 点をクリック）"), this,
                      [this] { activateTool(ToolId::Revolve); });
  freeform->addAction(tr("スイープ（1 つ目 = 断面、2 つ目 = パス）"), this, [this] {
    if (selection_.size() != 2) {
      reportError(tr("断面とパスの 2 つを選択してください"));
      return;
    }
    auto id = modeler_.sweepObject(selection_[0], selection_[1]);
    if (!id) {
      reportError(tr("スイープに失敗しました: %1").arg(QString::fromStdString(id.error())));
      return;
    }
    setSelection({id.value()});
  });

  // --- View
  QMenu* view = menuBar()->addMenu(tr("表示(&V)"));
  QMenu* sections = view->addMenu(tr("断面"));
  sections->addAction(tr("断面ツール (Shift+S)"), this, [this] { activateTool(ToolId::Section); });
  sectionVisibleAction_ = sections->addAction(tr("断面で切って表示"));
  sectionVisibleAction_->setCheckable(true);
  sectionVisibleAction_->setEnabled(false);
  connect(sectionVisibleAction_, &QAction::toggled, this, [this](bool on) {
    modeler_.setSectionVisible(on);
    updateTitle();
  });
  sections->addAction(tr("断面をすべて削除"), this, [this] {
    modeler_.clearSections();
    updateTitle();
  });
  view->addSeparator();
  struct ViewEntry {
    QString label;
    const char* key;
    Viewport::StandardView view;
  };
  const ViewEntry views[] = {
      {tr("アイソメ"), "F2", Viewport::StandardView::Iso},
      {tr("平面 (上)"), "F3", Viewport::StandardView::Top},
      {tr("正面"), "F4", Viewport::StandardView::Front},
      {tr("右側面"), "F5", Viewport::StandardView::Right},
      {tr("背面"), "F6", Viewport::StandardView::Back},
      {tr("左側面"), "F7", Viewport::StandardView::Left},
  };
  for (const ViewEntry& v : views) {
    const Viewport::StandardView sv = v.view;
    view->addAction(v.label, QKeySequence(QString::fromLatin1(v.key)), this,
                    [this, sv] { viewport_->setStandardView(sv); });
  }
  view->addAction(tr("全体表示"), QKeySequence(tr("Shift+Z")), this, [this] { viewport_->fitAll(); });
  view->addSeparator();
  perspectiveAction_ = view->addAction(tr("透視投影"));
  perspectiveAction_->setCheckable(true);
  perspectiveAction_->setChecked(true);
  perspectiveAction_->setShortcut(QKeySequence(QStringLiteral("F8")));
  connect(perspectiveAction_, &QAction::toggled, this,
          [this](bool on) { viewport_->setPerspective(on); });
  QAction* shaded = view->addAction(tr("シェーディング"));
  shaded->setCheckable(true);
  shaded->setChecked(true);
  connect(shaded, &QAction::toggled, this, [this](bool on) { viewport_->setShaded(on); });
  QAction* xray = view->addAction(tr("X 線（透かして表示・裏の点にも吸着）"));
  xray->setCheckable(true);
  xray->setShortcut(QKeySequence(tr("Alt+X")));
  connect(xray, &QAction::toggled, this, [this](bool on) { viewport_->setXRay(on); });
  focusOnDoubleClick_ = view->addAction(tr("ダブルクリックでフォーカス"));
  focusOnDoubleClick_->setCheckable(true);
  focusOnDoubleClick_->setChecked(true);

  QMenu* quality = view->addMenu(tr("表示品質（描画メッシュのみ）"));
  auto* qualityGroup = new QActionGroup(this);
  struct QualityEntry {
    QString label;
    display::DisplayQuality q;
  };
  const QualityEntry qualities[] = {{tr("粗い"), display::DisplayQuality::coarse()},
                                    {tr("標準"), display::DisplayQuality::normal()},
                                    {tr("精細"), display::DisplayQuality::fine()}};
  for (const QualityEntry& e : qualities) {
    QAction* a = quality->addAction(e.label);
    a->setCheckable(true);
    a->setChecked(e.label == tr("標準"));
    qualityGroup->addAction(a);
    const display::DisplayQuality q = e.q;
    connect(a, &QAction::triggered, this, [this, q] { viewport_->setDisplayQuality(q); });
  }

  // --- Tools (menu + left toolbar)
  QMenu* tools = menuBar()->addMenu(tr("ツール(&T)"));
  QToolBar* bar = new QToolBar(tr("ツール"), this);
  bar->setObjectName(QStringLiteral("toolsBar"));
  bar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  bar->setIconSize(QSize(28, 28));
  bar->setMovable(false);
  addToolBar(Qt::LeftToolBarArea, bar);
  toolGroup_ = new QActionGroup(this);
  struct ToolEntry {
    QString label;
    QString key;
    ToolId id;
    CursorKind icon;
  };
  const ToolEntry entries[] = {
      {tr("選択"), QStringLiteral("Space"), ToolId::Select, CursorKind::Select},
      {tr("線"), QStringLiteral("L"), ToolId::Line, CursorKind::Pencil},
      {tr("矩形"), QStringLiteral("R"), ToolId::Rectangle, CursorKind::Rectangle},
      {tr("壁"), QStringLiteral("W"), ToolId::Wall, CursorKind::Wall},
      {tr("スラブ"), QStringLiteral("Shift+W"), ToolId::Slab, CursorKind::Slab},
      {tr("開口"), QStringLiteral("O"), ToolId::Opening, CursorKind::Opening},
      {tr("円"), QStringLiteral("C"), ToolId::Circle, CursorKind::Circle},
      {tr("円弧"), QStringLiteral("A"), ToolId::Arc, CursorKind::Arc},
      {tr("多角形"), QStringLiteral("Shift+C"), ToolId::Polygon, CursorKind::Polygon},
      {tr("プッシュ/プル"), QStringLiteral("P"), ToolId::PushPull, CursorKind::PushPull},
      {tr("移動"), QStringLiteral("M"), ToolId::Move, CursorKind::Move},
      {tr("回転"), QStringLiteral("Q"), ToolId::Rotate, CursorKind::Rotate},
      {tr("尺度"), QStringLiteral("S"), ToolId::Scale, CursorKind::Scale},
      {tr("オフセット"), QStringLiteral("F"), ToolId::Offset, CursorKind::Offset},
      {tr("曲線"), QStringLiteral("N"), ToolId::Curve, CursorKind::Curve},
      {tr("回転体"), QStringLiteral("V"), ToolId::Revolve, CursorKind::Revolve},
      {tr("フォローミー"), QStringLiteral("Shift+F"), ToolId::FollowMe, CursorKind::FollowMe},
      {tr("メジャー"), QStringLiteral("T"), ToolId::Measure, CursorKind::Measure},
      {tr("分度器"), QStringLiteral("Shift+P"), ToolId::Protractor, CursorKind::Protractor},
      {tr("寸法"), QStringLiteral("D"), ToolId::Dimension, CursorKind::Dimension},
      {tr("テキスト"), QStringLiteral("Shift+T"), ToolId::Text, CursorKind::Text},
      {tr("断面"), QStringLiteral("Shift+S"), ToolId::Section, CursorKind::Section},
      {tr("ペイント"), QStringLiteral("B"), ToolId::Paint, CursorKind::Paint},
      {tr("消しゴム"), QStringLiteral("E"), ToolId::Eraser, CursorKind::Eraser},
  };
  for (const ToolEntry& e : entries) {
    QAction* a = new QAction(toolIcon(e.icon), e.label, this);
    a->setCheckable(true);
    a->setShortcut(QKeySequence(e.key));
    a->setToolTip(QStringLiteral("%1 (%2)").arg(e.label, e.key));
    const ToolId id = e.id;
    connect(a, &QAction::triggered, this, [this, id] {
      // Painting goes with the materials panel: bring it up.
      if (id == ToolId::Paint && materialsDock_) {
        materialsDock_->show();
        materialsDock_->raise();
      }
      activateTool(id);
    });
    toolGroup_->addAction(a);
    tools->addAction(a);
    bar->addAction(a);
    toolActions_[static_cast<int>(id)] = a;
  }

  tools->addSeparator();
  tools->addAction(tr("通り芯を作成..."), this, &MainWindow::showGridDialog);
  tools->addAction(tr("壁の設定..."), this, &MainWindow::showWallSettingsDialog);
  tools->addAction(tr("スラブの設定..."), this, &MainWindow::showSlabSettingsDialog);
  tools->addAction(tr("開口の設定..."), this, &MainWindow::showOpeningSettingsDialog);
  tools->addAction(tr("開口を等間隔に配置..."), this, &MainWindow::showOpeningsDialog);
  tools->addAction(tr("階を割り当て..."), this, &MainWindow::showLevelsDialog);
  QMenu* alignMenu = tools->addMenu(tr("整列（最初に選んだ物に揃える）"));
  const char* axisNames[] = {"X", "Y", "Z"};
  for (int axis = 0; axis < 3; ++axis) {
    const QString a = QString::fromLatin1(axisNames[axis]);
    alignMenu->addAction(tr("%1 最小").arg(a), this, [this, axis] { alignSelection(axis, -1); });
    alignMenu->addAction(tr("%1 中心").arg(a), this, [this, axis] { alignSelection(axis, 0); });
    alignMenu->addAction(tr("%1 最大").arg(a), this, [this, axis] { alignSelection(axis, 1); });
    if (axis < 2) alignMenu->addSeparator();
  }

  // --- AI integration (off by default; see docs/06_ai_integration.md)
  QMenu* ai = menuBar()->addMenu(tr("AI 連携(&A)"));
  aiAction_ = ai->addAction(tr("外部 AI からの操作を許可"));
  aiAction_->setCheckable(true);
  aiAction_->setToolTip(tr("このパソコン上の AI クライアント（MCP: sbr_mcp 経由）がモデルを操作できるようにします"));
  connect(aiAction_, &QAction::toggled, this, [this](bool on) { setAiAccess(on); });
  ai->addAction(tr("MCP 接続設定をコピー"), this, [this] {
    QString exe = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("sbr_mcp"));
#ifdef Q_OS_WIN
    exe += QStringLiteral(".exe");
#endif
    const nlohmann::json config = {
        {"mcpServers", {{"sketchbrep", {{"command", QDir::toNativeSeparators(exe).toStdString()}}}}}};
    QApplication::clipboard()->setText(QString::fromStdString(config.dump(2)));
    setStatus(tr("MCP 設定をコピーしました（Claude Desktop などの設定ファイルに貼り付け）"));
  });
  ai->addAction(tr("AI 連携について"), this, [this] {
    QMessageBox::information(
        this, tr("AI 連携"),
        tr("「外部 AI からの操作を許可」を ON にすると、このパソコン上の AI クライアントが "
           "sbr_mcp（MCP サーバー）経由でモデルを操作できます。<br><br>"
           "・AI の操作も GUI と同じコマンドで実行され、形状チェック・失敗時の取り消し・Ctrl+Z が効きます<br>"
           "・AI はファイルの保存・開く・書き出しはできません（保存はあなたが行います）<br>"
           "・接続できるのは同じユーザーのローカルプロセスだけです<br><br>"
           "設定: 「MCP 接続設定をコピー」で Claude Desktop などの設定に貼り付けてください。"));
  });

  QMenu* help = menuBar()->addMenu(tr("ヘルプ(&H)"));
  help->addAction(tr("操作方法"), this, [this] {
    QMessageBox::information(
        this, tr("操作方法"),
        tr("<b>カメラ</b><br>ホイール: ズーム / ホイールドラッグ: オービット / "
           "Shift+ホイールドラッグ: パン / ダブルクリック: フォーカス<br>"
           "F2〜F7: 標準ビュー / F8: 透視・平行投影 / Shift+Z: 全体表示<br><br>"
           "<b>ツール</b><br>Space: 選択 / L: 線 / R: 矩形 / P: プッシュ/プル<br>"
           "D: 寸法（2 点 → 寸法線の位置）/ Shift+T: テキスト（面をクリックすると面積）<br>"
           "C: 円 / A: 円弧（始点 → 終点 → 膨らみ。Ctrl で面/線）/ N: 曲線 / V: 回転体 / T: メジャー / B: ペイント（Alt+クリックで色を拾う）/ E: 消しゴム<br>"
           "M: 移動 / Q: 回転 / F: オフセット（移動・回転中に Ctrl でコピー、確定後 x3 で配列・/3 で等分割）<br>"
           "作図中に数値を打って Enter で寸法指定（例: 3000, 4.5m, 3000,4500）<br>"
           "Shift: 推論方向をロック / →・←・↑: X・Y・Z 軸ロック / ↓: 解除 / Esc: 中止"));
  });
}

void MainWindow::setAiAccess(bool enabled) {
  if (aiAction_->isChecked() != enabled) {
    aiAction_->setChecked(enabled);  // re-enters through toggled
    return;
  }
  if (!aiBridge_->setEnabled(enabled)) {
    reportError(tr("AI 連携を開始できませんでした: %1").arg(aiBridge_->lastError()));
    const QSignalBlocker block(aiAction_);
    aiAction_->setChecked(false);
    enabled = false;
  }
  QSettings().setValue(QStringLiteral("ai/bridgeEnabled"), enabled);
  aiLabel_->setText(tr("AI 連携 ON"));
  aiLabel_->setVisible(enabled);
  setStatus(enabled ? tr("外部 AI からの操作を受け付けています（Ctrl+Z で AI の操作も元に戻せます）")
                    : tr("AI 連携を停止しました"));
}

void MainWindow::buildAiBridge() {
  using nlohmann::json;
  using Extension = api::CommandApi::Extension;
  std::map<std::string, std::function<std::string(const std::string&)>> handlers;
  handlers["get_selection"] = [this](const std::string&) {
    json elements = json::array();
    for (const ElementRef& e : elements_) {
      elements.push_back({{"id", e.objectId},
                          {"type", e.type == TopAbs_FACE ? "face" : "edge"},
                          {"index", e.index}});
    }
    return json{{"ids", selection_}, {"elements", elements}, {"editing_group", modeler_.activeGroup()}}
        .dump();
  };
  handlers["select"] = [this](const std::string& args) {
    const json a = json::parse(args);
    std::vector<doc::ObjectId> ids;
    for (const json& id : a.at("ids")) {
      if (!modeler_.document().find(id.get<std::string>())) {
        throw std::runtime_error("no object with id '" + id.get<std::string>() + "'");
      }
      ids.push_back(id.get<std::string>());
    }
    setSelection(ids);
    return json{{"ids", ids}}.dump();
  };
  handlers["set_view"] = [this](const std::string& args) {
    const json a = json::parse(args);
    const std::string v = a.at("view");
    using SV = Viewport::StandardView;
    if (v == "fit") {
      if (a.contains("ids")) {
        viewport_->fitObjects(a["ids"].get<std::vector<doc::ObjectId>>());
      } else {
        viewport_->fitAll();
      }
    } else {
      static const std::map<std::string, SV> views = {{"iso", SV::Iso},     {"top", SV::Top},
                                                      {"front", SV::Front}, {"right", SV::Right},
                                                      {"back", SV::Back},   {"left", SV::Left}};
      const auto it = views.find(v);
      if (it == views.end()) throw std::runtime_error("unknown view '" + v + "'");
      viewport_->setStandardView(it->second);
    }
    return json{{"view", v}}.dump();
  };
  handlers["capture_view"] = [this](const std::string&) {
    viewport_->syncNow();
    const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("sketchbrep-view.png"));
    if (!viewport_->dumpImage(path)) throw std::runtime_error("the view could not be captured");
    return json{{"image_path", QDir::toNativeSeparators(path).toStdString()}}.dump();
  };

  handlers["list_scenes"] = [this](const std::string&) {
    json list = json::array();
    for (const doc::Scene& s : modeler_.document().settings().scenes) list.push_back(s.name);
    return json{{"scenes", list}}.dump();
  };
  handlers["go_to_scene"] = [this](const std::string& args) {
    const json a = json::parse(args);
    if (a.value("save", false)) {
      addScene(QString::fromStdString(a.value("name", std::string())));
      return json{{"saved", modeler_.document().settings().scenes.back().name}}.dump();
    }
    const auto& scenes = modeler_.document().settings().scenes;
    int index = a.value("index", -1);
    if (a.contains("name")) {
      for (std::size_t i = 0; i < scenes.size(); ++i) {
        if (scenes[i].name == a["name"].get<std::string>()) index = static_cast<int>(i);
      }
    }
    if (!goToScene(index)) throw std::runtime_error("no such scene");
    return json{{"scene", scenes[index].name}}.dump();
  };

  handlers["propose"] = [this](const std::string& args) {
    const json a = json::parse(args);
    if (!a.contains("commands") || !a["commands"].is_array()) throw std::runtime_error("'commands' must be an array");
    // Try it on a copy first: never show a proposal that cannot be applied.
    doc::Modeler scratch;
    scratch.copyFrom(modeler_);
    api::CommandApi tryApi(scratch, api::CommandApi::Options{false});
    const json tried = json::parse(tryApi.execute(json{{"command", "batch"}, {"args", {{"commands", a["commands"]}}}}.dump()));
    if (!tried.value("ok", false)) {
      proposal_ = Proposal{a["commands"].dump(), modeler_.history().revision(), "failed", tried.value("error", std::string()), {}};
      throw std::runtime_error("the proposal cannot be applied: " + proposal_->error);
    }
    // What the proposal adds or changes (blue ghost) and removes or hides
    // (red ghost), from the change set its trial run recorded: the same one
    // preview reports and the commit records (review B).
    const json& changes = tried["result"]["changes"];
    BRep_Builder builder;
    TopoDS_Compound ghost, removedGhost;
    builder.MakeCompound(ghost);
    builder.MakeCompound(removedGhost);
    QStringList added, changed, removed;
    auto addGhost = [&](TopoDS_Compound& into, const doc::Modeler& from, const doc::ObjectId& id) {
      const doc::Object* o = from.document().find(id);
      if (o && !o->shape.IsNull() && from.document().isVisible(id)) builder.Add(into, from.document().worldShape(*o));
    };
    for (const json& id : changes["added"]) {
      added << QString::fromStdString(scratch.document().find(id.get<std::string>())->name);
      addGhost(ghost, scratch, id.get<std::string>());
    }
    for (const json& m : changes["modified"]) {
      const doc::ObjectId id = m["id"];
      changed << QString::fromStdString(scratch.document().find(id)->name);
      if (scratch.document().isVisible(id)) {
        addGhost(ghost, scratch, id);
      } else {
        addGhost(removedGhost, modeler_, id);  // about to be hidden
      }
    }
    for (const json& id : changes["removed"]) {
      removed << QString::fromStdString(modeler_.document().find(id.get<std::string>())->name);
      addGhost(removedGhost, modeler_, id.get<std::string>());
    }
    viewport_->setProposalGhost(ghost, removedGhost);
    proposal_ = Proposal{a["commands"].dump(), modeler_.history().revision(), "pending", {}, changes.dump()};
    const QString text = QString::fromStdString(a.value("description", std::string()));
    const QString counts = tr("追加 %1 / 変更 %2 / 削除 %3").arg(added.size()).arg(changed.size()).arg(removed.size());
    proposalLabel_->setText(text.isEmpty() ? tr("AI の提案: %1").arg(counts)
                                           : tr("AI の提案: %1（%2）").arg(text, counts));
    QStringList details;
    if (!changes["settings"].empty()) details << tr("表示設定（タグ・断面・シーン）も変わります");
    if (!added.isEmpty()) details << tr("追加: %1").arg(added.join(QStringLiteral(", ")));
    if (!changed.isEmpty()) details << tr("変更: %1").arg(changed.join(QStringLiteral(", ")));
    if (!removed.isEmpty()) details << tr("削除: %1").arg(removed.join(QStringLiteral(", ")));
    proposalLabel_->setToolTip(details.join(QStringLiteral("\n")));
    proposalBar_->show();
    setStatus(tr("AI の提案を表示しています（青: 追加・変更、赤: 削除）。「適用」か「却下」を選んでください"));
    return json{{"state", "pending"},
                {"revision", proposal_->revision},
                {"added", added.size()},
                {"changed", changed.size()},
                {"removed", removed.size()},
                {"changes", changes}}
        .dump();
  };
  handlers["proposal_status"] = [this](const std::string&) {
    if (!proposal_) return json{{"state", "none"}}.dump();
    json r = {{"state", proposal_->state}};
    if (!proposal_->error.empty()) r["error"] = proposal_->error;
    return r.dump();
  };

  std::vector<Extension> ext = api::CommandApi::appCommandSpecs();
  for (Extension& e : ext) e.run = handlers.at(e.name);
  aiBridge_ = std::make_unique<AiBridge>(modeler_, std::move(ext));
  // An AI command must not land in the middle of a half-drawn rectangle.
  aiBridge_->setHooks([this] { if (active_) active_->cancel(); },
                      [this] { if (active_) active_->activate(); });
  connect(aiBridge_.get(), &AiBridge::commandExecuted, this,
          [this](const QString& command, bool ok, const QString& error) {
            if (ok) {
              setStatus(tr("AI: %1").arg(command));
            } else {
              setStatus(tr("AI: %1 は失敗しました（%2）").arg(command, error));
            }
          });
  connect(aiBridge_.get(), &AiBridge::clientCountChanged, this, [this](int count) {
    aiLabel_->setText(count > 0 ? tr("AI 接続中") : tr("AI 連携 ON"));
  });
}

void MainWindow::refreshScenes() {
  const QSignalBlocker block(sceneBar_);
  while (sceneBar_->count() > 0) sceneBar_->removeTab(0);
  for (const doc::Scene& s : modeler_.document().settings().scenes) {
    sceneBar_->addTab(QString::fromStdString(s.name));
  }
}

void MainWindow::addScene(const QString& name) {
  auto& scenes = modeler_.document().settings().scenes;
  doc::Scene scene;
  scene.name = name.isEmpty() ? tr("シーン %1").arg(scenes.size() + 1).toStdString() : name.toStdString();
  viewport_->storeCamera(scene.camera);
  scenes.push_back(scene);
  modeler_.history().markDirty();
  refreshScenes();
  sceneBar_->setCurrentIndex(sceneBar_->count() - 1);
  updateTitle();
  setStatus(tr("シーン「%1」を保存しました（右クリックで更新・名前変更・削除）").arg(QString::fromStdString(scene.name)));
}

bool MainWindow::goToScene(int index) {
  const auto& scenes = modeler_.document().settings().scenes;
  if (index < 0 || index >= static_cast<int>(scenes.size())) return false;
  viewport_->restoreCamera(scenes[index].camera);
  perspectiveAction_->setChecked(scenes[index].camera.perspective);
  sceneBar_->setCurrentIndex(index);
  return true;
}

void MainWindow::sceneMenu(int index, const QPoint& globalPos) {
  auto& scenes = modeler_.document().settings().scenes;
  QMenu menu(this);
  QAction* update = menu.addAction(tr("今のビューで更新"));
  QAction* rename = menu.addAction(tr("名前の変更..."));
  QAction* remove = menu.addAction(tr("削除"));
  QAction* chosen = menu.exec(globalPos);
  if (!chosen || index >= static_cast<int>(scenes.size())) return;
  if (chosen == update) {
    viewport_->storeCamera(scenes[index].camera);
  } else if (chosen == rename) {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("シーン名"), tr("名前"), QLineEdit::Normal,
                                               QString::fromStdString(scenes[index].name), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    scenes[index].name = name.trimmed().toStdString();
  } else if (chosen == remove) {
    scenes.erase(scenes.begin() + index);
  }
  modeler_.history().markDirty();
  refreshScenes();
  updateTitle();
}

void MainWindow::applyProposal() {
  if (!hasPendingProposal()) return;
  proposalBar_->hide();
  viewport_->setProposalGhost(TopoDS_Shape());
  // Same command path as everything else; refused if the model changed.
  api::CommandApi api(modeler_, api::CommandApi::Options{false});
  const nlohmann::json request = {
      {"command", "batch"},
      {"args", {{"name", "AI proposal"}, {"commands", nlohmann::json::parse(proposal_->commandsJson)},
                {"expected_revision", proposal_->revision}}}};
  const auto result = nlohmann::json::parse(api.execute(request.dump()));
  if (result.value("ok", false)) {
    proposal_->state = "applied";
    // What was committed must be what was shown (ids of new objects differ
    // between the trial copy and the model, so those are compared by count).
    auto comparable = [](nlohmann::json c) {
      c["added"] = c["added"].size();
      return c;
    };
    const nlohmann::json committed = result["result"]["changes"];
    if (comparable(committed) != comparable(nlohmann::json::parse(proposal_->changesJson))) {
      setStatus(tr("AI の提案を適用しましたが、表示した内容と結果が異なります。確認してください（Ctrl+Z で元に戻せます）"));
      return;
    }
    setStatus(tr("AI の提案を適用しました（Ctrl+Z で元に戻せます）"));
  } else {
    proposal_->state = "failed";
    proposal_->error = result.value("error", std::string());
    reportError(tr("AI の提案を適用できませんでした: %1").arg(QString::fromStdString(proposal_->error)));
  }
}

void MainWindow::rejectProposal() {
  if (!hasPendingProposal()) return;
  proposal_->state = "rejected";
  proposalBar_->hide();
  viewport_->setProposalGhost(TopoDS_Shape());
  setStatus(tr("AI の提案を却下しました（モデルは変更されていません）"));
}

void MainWindow::activateTool(ToolId id) {
  if (active_) {
    active_->deactivate();
    active_->recentEdit().clear();  // typing after switching tools changes nothing earlier
  }
  // While drawing outlines, show where loose lines/arcs are still open.
  viewport_->setShowOpenEnds(id == ToolId::Line || id == ToolId::Arc || id == ToolId::Curve);
  switch (id) {
    case ToolId::Select: active_ = selectTool_.get(); break;
    case ToolId::Line: active_ = lineTool_.get(); break;
    case ToolId::Rectangle: active_ = rectangleTool_.get(); break;
    case ToolId::PushPull: active_ = pushPullTool_.get(); break;
    case ToolId::Move: active_ = moveTool_.get(); break;
    case ToolId::Rotate: active_ = rotateTool_.get(); break;
    case ToolId::Offset: active_ = offsetTool_.get(); break;
    case ToolId::Curve: active_ = curveTool_.get(); break;
    case ToolId::Revolve: active_ = revolveTool_.get(); break;
    case ToolId::Measure: active_ = measureTool_.get(); break;
    case ToolId::Protractor: active_ = protractorTool_.get(); break;
    case ToolId::Circle: active_ = circleTool_.get(); break;
    case ToolId::Arc: active_ = arcTool_.get(); break;
    case ToolId::Dimension: active_ = dimensionTool_.get(); break;
    case ToolId::Text: active_ = textTool_.get(); break;
    case ToolId::Section: active_ = sectionTool_.get(); break;
    case ToolId::FollowMe: active_ = followMeTool_.get(); break;
    case ToolId::Paint: active_ = paintTool_.get(); break;
    case ToolId::Eraser: active_ = eraserTool_.get(); break;
    case ToolId::Paste: active_ = pasteTool_.get(); break;
    case ToolId::ComponentAxes: active_ = componentAxesTool_.get(); break;
    case ToolId::Polygon: active_ = polygonTool_.get(); break;
    case ToolId::Scale: active_ = scaleTool_.get(); break;
    case ToolId::Wall: active_ = wallTool_.get(); break;
    case ToolId::Slab: active_ = slabTool_.get(); break;
    case ToolId::Opening: active_ = openingTool_.get(); break;
    case ToolId::Count: return;
  }
  // Tools without a toolbar button (paste) leave the toolbar unchecked.
  if (QAction* a = toolActions_[static_cast<int>(id)]) {
    a->setChecked(true);
  } else if (QAction* checked = toolGroup_->checkedAction()) {
    toolGroup_->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    checked->setChecked(false);
    toolGroup_->setExclusionPolicy(QActionGroup::ExclusionPolicy::Exclusive);
  }
  viewport_->setTool(active_);
  viewport_->setToolCursor(active_->cursor());
  userTyping_ = false;
  active_->activate();
  refreshConstraints();
  viewport_->setFocus();
}

// --- ToolHost ---------------------------------------------------------------------

void MainWindow::setStatus(const QString& text) {
  status_->setText(text);
  refreshConstraints();  // tools report on (nearly) every input
}

void MainWindow::refreshConstraints() {
  if (!constraintLabel_) return;
  const QString text = active_ ? active_->constraintText() : QString();
  constraintLabel_->setText(text);
  constraintLabel_->setVisible(!text.isEmpty());
}

QString MainWindow::constraintText() const { return constraintLabel_ ? constraintLabel_->text() : QString(); }

QString MainWindow::statusText() const { return status_->text(); }

void MainWindow::setMeasurement(const QString& label, const QString& value) {
  measurementLabel_->setText(label);
  if (!userTyping_) measurement_->setText(value);
}

void MainWindow::reportError(const QString& raw) {
  static_cast<GuiProgress*>(progress_.get())->finish();
  const QString text = userMessage(raw);
  lastError_ = text;
  statusBar()->showMessage(text, 8000);
  QApplication::beep();
}

void MainWindow::setSelection(const std::vector<doc::ObjectId>& ids) {
  selection_ = ids;
  elements_.clear();
  viewport_->setHighlightedElements({});
  viewport_->setSelection(ids);
  refreshPanels();
}

void MainWindow::setElementSelection(const std::vector<ElementRef>& elements) {
  elements_ = elements;
  // Owners become the object selection (panel, Move/Rotate targets) but are
  // not highlighted as a whole: only the picked faces/edges are.
  selection_.clear();
  for (const ElementRef& e : elements) {
    if (std::find(selection_.begin(), selection_.end(), e.objectId) == selection_.end()) {
      selection_.push_back(e.objectId);
    }
  }
  viewport_->setSelection({});
  viewport_->setHighlightedElements(elements);
  refreshPanels();
}

void MainWindow::enterGroup(const doc::ObjectId& id) {
  doc::ObjectId groupId = id;
  const doc::Object* target = modeler_.document().find(id);
  if (target && target->kind() == doc::ObjectKind::Instance) {
    // Components are edited in place; the change reaches every instance.
    if (active_) active_->cancel();
    auto group = modeler_.beginComponentEdit(id);
    if (!group) {
      reportError(QString::fromStdString(group.error()));
      return;
    }
    groupId = *group;
  } else if (!report(modeler_.setActiveGroup(groupId))) {
    return;
  }
  const doc::Object* g = modeler_.document().find(groupId);
  const auto* node = g ? std::get_if<doc::GroupNode>(&g->recipe.base) : nullptr;
  if (node && !node->editingDefinition.empty()) {
    // Make the scope visible: every instance changes (spec 07 §6.2).
    const int count = modeler_.instanceCount(node->editingDefinition) + 1;
    QString text = tr("コンポーネント編集中: %1（終了すると %2 個すべてに反映 / Esc で終了）")
                       .arg(QString::fromStdString(g->name))
                       .arg(count);
    // The definition is edited as stored; this placement's own settings come back afterwards.
    if (!node->editingOverrides.empty() || node->editingMirrored) {
      text += tr("　※この配置だけの寸法・鏡像は元の形で表示中、終了後も保たれます");
    }
    contextLabel_->setText(text);
    contextLabel_->setStyleSheet(QStringLiteral("QLabel { background: #ffd9b3; padding: 0 6px; }"));
  } else {
    contextLabel_->setText(tr("グループ編集中: %1（Esc で終了）").arg(QString::fromStdString(g->name)));
    contextLabel_->setStyleSheet(QStringLiteral("QLabel { background: #fff3c4; padding: 0 6px; }"));
  }
  contextLabel_->show();
  viewport_->setEditContext(groupId);
  setSelection({});
  if (active_) active_->activate();
}

void MainWindow::exitGroup() {
  const doc::ObjectId group = modeler_.activeGroup();
  if (group.empty()) return;
  doc::ObjectId select = group;
  if (const auto editing = modeler_.componentEditGroup(group)) {
    const auto* node = std::get_if<doc::GroupNode>(&modeler_.document().find(*editing)->recipe.base);
    const doc::ObjectId instance = node->editingInstance;
    if (!report(modeler_.endComponentEdit(*editing))) return;  // stay inside, nothing lost
    select = instance;
  }
  modeler_.setActiveGroup({});
  contextLabel_->hide();
  viewport_->setEditContext({});
  setSelection(modeler_.document().find(select) ? std::vector<doc::ObjectId>{select}
                                                : std::vector<doc::ObjectId>{});
}

QString MainWindow::elementSummary() const {
  if (elements_.empty()) return {};
  int faces = 0, edges = 0;
  for (const ElementRef& e : elements_) (e.type == TopAbs_FACE ? faces : edges)++;
  return tr("面 %1 / エッジ %2").arg(faces).arg(edges);
}

// --- document ---------------------------------------------------------------------

void MainWindow::onDocumentChanged() {
  static_cast<GuiProgress*>(progress_.get())->finish();
  viewport_->scheduleSync();
  if (refreshPending_) return;
  refreshPending_ = true;
  QTimer::singleShot(0, this, [this] {
    refreshPending_ = false;
    // A pending AI proposal is based on the model as it was: once the user
    // edits, it is withdrawn (it would be refused anyway, spec 07 §9.2).
    if (hasPendingProposal() && modeler_.history().revision() != proposal_->revision) {
      proposal_->state = "failed";
      proposal_->error = "the model changed before the proposal was applied";
      proposalBar_->hide();
      viewport_->setProposalGhost(TopoDS_Shape());
      setStatus(tr("モデルが変更されたため AI の提案を取り下げました"));
    }
    // Drop selected ids that no longer exist.
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(),
                                    [this](const doc::ObjectId& id) {
                                      return modeler_.document().find(id) == nullptr;
                                    }),
                     selection_.end());
    elements_.erase(std::remove_if(elements_.begin(), elements_.end(),
                                   [this](const ElementRef& e) {
                                     return modeler_.document().find(e.objectId) == nullptr;
                                   }),
                    elements_.end());
    viewport_->setSelection(elements_.empty() ? selection_ : std::vector<doc::ObjectId>{});
    refreshPanels();
    updateTitle();
  });
}

void MainWindow::refreshTags() {
  const QSignalBlocker block(tagList_);
  tagList_->clear();
  const auto& hidden = modeler_.document().settings().hiddenTags;
  for (const std::string& tag : modeler_.tags()) {
    const QString name = QString::fromStdString(tag);
    auto* item = new QListWidgetItem(name == QStringLiteral("Default")       ? tr("標準 (Default)")
                                     : name == QStringLiteral("Annotations") ? tr("注記 (Annotations)")
                                                                             : name);
    item->setData(Qt::UserRole, name);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(hidden.count(tag) ? Qt::Unchecked : Qt::Checked);
    tagList_->addItem(item);
  }
  if (sectionVisibleAction_) {
    const QSignalBlocker blockAction(sectionVisibleAction_);
    sectionVisibleAction_->setEnabled(!modeler_.document().settings().sections.empty());
    sectionVisibleAction_->setChecked(modeler_.sectionVisible());
  }
}

void MainWindow::refreshPanels() {
  refreshTags();
  if (materialsPanel_) materialsPanel_->refresh(modeler_.document(), paintTool_->material());
  panel_->refresh(modeler_, selection_, elementSummary());
  const auto& h = modeler_.history();
  undoAction_->setEnabled(h.canUndo());
  redoAction_->setEnabled(h.canRedo());
  undoAction_->setText(h.canUndo() ? tr("元に戻す: %1").arg(QString::fromStdString(h.undoName()))
                                   : tr("元に戻す"));
  redoAction_->setText(h.canRedo() ? tr("やり直し: %1").arg(QString::fromStdString(h.redoName()))
                                   : tr("やり直し"));
}

void MainWindow::updateTitle() {
  const QString name = currentPath_.isEmpty() ? tr("無題") : QFileInfo(currentPath_).fileName();
  setWindowTitle(QStringLiteral("%1[*] - SketchBRep").arg(name));
  setWindowModified(!modeler_.history().isClean());
}

void MainWindow::beginTextEntry(const QString& label, const QString& text) {
  userTyping_ = false;
  setMeasurement(label, text);
  userTyping_ = true;  // keep the text while the user edits it
  measurement_->setFocus();
  measurement_->selectAll();
}

bool MainWindow::exportImage(const QString& path) {
  viewport_->hideSnap();
  viewport_->syncNow();
  if (!viewport_->dumpImage(path)) {
    reportError(tr("画像を書き出せませんでした: %1").arg(path));
    return false;
  }
  statusBar()->showMessage(tr("画像を書き出しました: %1").arg(path), 5000);
  return true;
}

bool MainWindow::copySelection() {
  auto clip = modeler_.copyToClipboard(selection());
  if (!clip) {
    reportError(tr("コピーする物を選択してください"));
    return false;
  }
  clipboard_ = std::move(clip.value());
  setStatus(tr("%1 個をコピーしました（Ctrl+V で貼り付け）").arg(selection().size()));
  return true;
}

bool MainWindow::cutSelection() {
  const std::vector<doc::ObjectId> ids = selection();
  if (!copySelection()) return false;
  const Status s = modeler_.remove(ids);
  if (!s) {
    reportError(QString::fromStdString(s.message()));
    return false;
  }
  setSelection({});
  return true;
}

void MainWindow::paste() {
  if (clipboard_.empty()) {
    reportError(tr("貼り付ける物がありません。先にコピーしてください"));
    return;
  }
  activateTool(ToolId::Paste);
}

bool MainWindow::pasteInPlace() {
  if (clipboard_.empty()) {
    reportError(tr("貼り付ける物がありません。先にコピーしてください"));
    return false;
  }
  auto ids = modeler_.paste(clipboard_);
  if (!ids) {
    reportError(QString::fromStdString(ids.error()));
    return false;
  }
  setSelection(*ids);
  return true;
}

bool MainWindow::createGrid(const QString& xSpacings, const QString& ySpacings) {
  const auto xs = units::parseSpacingList(xSpacings.toStdString());
  const auto ys = units::parseSpacingList(ySpacings.toStdString());
  if (!xs || !ys) {
    reportError(tr("間隔を解釈できません（例: 3600*3, 2700）"));
    return false;
  }
  auto grid = modeler_.createGridLines(gp::Origin(), *xs, *ys);
  if (!grid) {
    reportError(QString::fromStdString(grid.error()));
    return false;
  }
  setStatus(tr("通り芯を作成しました（X %1 本・Y %2 本）").arg(xs->size() + 1).arg(ys->size() + 1));
  return true;
}

void MainWindow::showGridDialog() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("通り芯を作成"));
  auto* form = new QFormLayout(&dialog);
  auto* xEdit = new QLineEdit(QStringLiteral("3600*3"), &dialog);
  auto* yEdit = new QLineEdit(QStringLiteral("3600*2"), &dialog);
  xEdit->setPlaceholderText(tr("例: 3600*3, 2700"));
  form->addRow(tr("X 方向の間隔 (mm)"), xEdit);
  form->addRow(tr("Y 方向の間隔 (mm)"), yEdit);
  form->addRow(new QLabel(tr("原点から並べます。「3600*3」で 3 スパン。単位 m も可"), &dialog));
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() == QDialog::Accepted) createGrid(xEdit->text(), yEdit->text());
}

void MainWindow::showWallSettingsDialog() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("壁の設定"));
  auto* form = new QFormLayout(&dialog);
  const WallTool::Settings current = wallTool_->settings();
  auto* thickness = new QLineEdit(QString::fromStdString(units::formatLength(current.thickness, 0)), &dialog);
  auto* height = new QLineEdit(QString::fromStdString(units::formatLength(current.height, 0)), &dialog);
  auto* justification = new QComboBox(&dialog);
  justification->addItems({tr("線の左に寄せる"), tr("線を中心にする"), tr("線の右に寄せる")});
  justification->setCurrentIndex(current.justification + 1);
  form->addRow(tr("厚さ"), thickness);
  form->addRow(tr("高さ"), height);
  form->addRow(tr("基準線"), justification);
  form->addRow(new QLabel(tr("進む向きに見て左右。単位 m も可"), &dialog));
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted) return;
  const auto t = units::parseLength(thickness->text().toStdString());
  const auto h = units::parseLength(height->text().toStdString());
  if (!t || !h || *t <= 0 || *h <= 0) {
    reportError(tr("厚さと高さは正の長さで入力してください"));
    return;
  }
  wallTool_->setSettings({*t, *h, justification->currentIndex() - 1});
  setStatus(wallTool_->settingsText());
}

void MainWindow::showSlabSettingsDialog() {
  bool ok = false;
  const QString text = QInputDialog::getText(this, tr("スラブの設定"), tr("厚さ（天端はクリックした高さ。単位 m も可）"),
                                             QLineEdit::Normal,
                                             QString::fromStdString(units::formatLength(slabTool_->thickness(), 0)), &ok);
  if (!ok) return;
  const auto t = units::parseLength(text.toStdString());
  if (!t || *t <= 0) {
    reportError(tr("厚さは正の長さで入力してください"));
    return;
  }
  slabTool_->setThickness(*t);
  setStatus(slabTool_->settingsText());
}

void MainWindow::showOpeningSettingsDialog() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("開口の設定"));
  auto* form = new QFormLayout(&dialog);
  const OpeningTool::Settings current = openingTool_->settings();
  auto field = [&](double v) {
    return new QLineEdit(QString::fromStdString(units::formatLength(v, 0)), &dialog);
  };
  auto* width = field(current.width);
  auto* height = field(current.height);
  auto* sill = field(current.sill);
  auto* preset = new QComboBox(&dialog);
  preset->addItems({tr("（そのまま）"), tr("ドア 900×2100"), tr("引き戸 1600×2100"), tr("腰窓 1600×1100・腰 900"),
                    tr("掃き出し窓 1600×2000")});
  connect(preset, &QComboBox::currentIndexChanged, &dialog, [=](int i) {
    const double sizes[][3] = {{0, 0, 0}, {900, 2100, 0}, {1600, 2100, 0}, {1600, 1100, 900}, {1600, 2000, 0}};
    if (i < 1 || i > 4) return;
    width->setText(QString::number(sizes[i][0]));
    height->setText(QString::number(sizes[i][1]));
    sill->setText(QString::number(sizes[i][2]));
  });
  form->addRow(tr("よく使う大きさ"), preset);
  form->addRow(tr("幅"), width);
  form->addRow(tr("高さ"), height);
  form->addRow(tr("床からの高さ（腰）"), sill);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted) return;
  const auto w = units::parseLength(width->text().toStdString());
  const auto h = units::parseLength(height->text().toStdString());
  const auto s = units::parseLength(sill->text().toStdString());
  if (!w || !h || !s || *w <= 0 || *h <= 0 || *s < 0) {
    reportError(tr("幅・高さは正の長さ、腰は 0 以上で入力してください"));
    return;
  }
  openingTool_->setSettings({*w, *h, *s});
  setStatus(openingTool_->settingsText());
}

void MainWindow::materialPicked(const std::string& id) {
  if (materialsPanel_) materialsPanel_->refresh(modeler_.document(), id);
}

void MainWindow::chooseMaterial(const std::string& id) {
  paintTool_->setMaterial(id);
  if (materialsPanel_) materialsPanel_->refresh(modeler_.document(), id);
  if (active_ == paintTool_.get()) {
    paintTool_->activate();  // status shows the new material
  } else {
    activateTool(ToolId::Paint);
  }
}

std::optional<std::string> MainWindow::addPresetMaterial(int index) {
  const auto& presets = MaterialsPanel::presets();
  if (index < 0 || index >= static_cast<int>(presets.size())) return std::nullopt;
  const auto& p = presets[static_cast<std::size_t>(index)];
  auto id = modeler_.createMaterial(p.name, p.color);
  if (!id) {
    reportError(QString::fromStdString(id.error()));
    return std::nullopt;
  }
  chooseMaterial(id.value());
  return id.value();
}

void MainWindow::editMaterialDialog(const std::string& id) {
  const doc::Material* current = modeler_.document().findMaterial(id);
  QDialog dialog(this);
  dialog.setWindowTitle(current ? tr("素材を変更") : tr("新しい素材"));
  auto* form = new QFormLayout(&dialog);
  auto* name = new QLineEdit(current ? QString::fromStdString(current->name)
                                     : tr("素材 %1").arg(modeler_.document().materials().size() + 1),
                             &dialog);
  QColor color = current ? QColor::fromRgbF(current->color.r, current->color.g, current->color.b) : QColor(200, 170, 130);
  auto* pick = new QPushButton(&dialog);
  auto showColor = [pick, &color] {
    pick->setText(color.name());
    pick->setStyleSheet(QStringLiteral("QPushButton { background: %1; }").arg(color.name()));
  };
  showColor();
  connect(pick, &QPushButton::clicked, &dialog, [&] {
    const QColor chosen = QColorDialog::getColor(color, &dialog, tr("素材の色"));
    if (chosen.isValid()) color = chosen;
    showColor();
  });
  form->addRow(tr("名前"), name);
  form->addRow(tr("色"), pick);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted) return;
  const doc::Color c{color.redF(), color.greenF(), color.blueF()};
  if (current) {
    report(modeler_.updateMaterial(id, name->text().trimmed().toStdString(), c));
    return;
  }
  auto created = modeler_.createMaterial(name->text().trimmed().toStdString(), c);
  if (!created) {
    reportError(QString::fromStdString(created.error()));
    return;
  }
  chooseMaterial(created.value());
}

bool MainWindow::applyMaterialToSelection(const std::string& id) {
  if (selection_.empty()) {
    reportError(tr("素材を割り当てる部材（または面）を選んでください"));
    return false;
  }
  // Faces selected: those faces; otherwise the selected objects as a whole.
  std::map<doc::ObjectId, std::vector<TopoDS_Face>> faces;
  for (const ElementRef& e : elements_) {
    if (e.type != TopAbs_FACE) continue;
    const TopoDS_Shape f = viewport_->elementShape(e);
    if (!f.IsNull()) faces[e.objectId].push_back(TopoDS::Face(f));
  }
  if (faces.empty()) return report(modeler_.assignMaterial(selection_, id));
  modeler_.history().beginGroup("Paint Faces");
  for (const auto& [object, list] : faces) {
    if (!report(modeler_.paintFaces(object, list, id))) {
      modeler_.history().abortGroup();
      return false;
    }
  }
  modeler_.history().endGroup();
  return true;
}

// --- autosave and recovery ------------------------------------------------------

QString MainWindow::recoveryDir() {
  const QString overridden = qEnvironmentVariable("SBR_RECOVERY_DIR");
  QString dir = !overridden.isEmpty() ? overridden
                                      : QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                            .filePath(QStringLiteral("recovery"));
  QDir().mkpath(dir);
  return dir;
}

void MainWindow::startSession() {
  const QSettings settings;
  autosaveMinutes_ = settings.value(QStringLiteral("autosave/minutes"), 5).toInt();
  backupGenerations_ = settings.value(QStringLiteral("backup/generations"), 3).toInt();
  const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QDir dir(recoveryDir());
  // The lock says "this session is running": its recovery file is not
  // offered to other windows while it is held.
  sessionLock_ = std::make_unique<QLockFile>(dir.filePath(id + QStringLiteral(".lock")));
  sessionLock_->setStaleLockTime(0);
  sessionLock_->tryLock(0);
  recoveryFile_ = dir.filePath(id + QStringLiteral(".sbr"));
  autosavedSequence_ = modeler_.document().sequence();
  autosaveTimer_ = new QTimer(this);
  connect(autosaveTimer_, &QTimer::timeout, this, [this] { autosaveNow(); });
  setAutosaveMinutes(autosaveMinutes_);
}

void MainWindow::setAutosaveMinutes(int minutes) {
  autosaveMinutes_ = std::max(0, minutes);
  if (!autosaveTimer_) return;
  if (autosaveMinutes_ == 0) {
    autosaveTimer_->stop();
  } else {
    autosaveTimer_->start(autosaveMinutes_ * 60 * 1000);
  }
}

bool MainWindow::autosaveNow() {
  if (recoveryFile_.isEmpty()) return false;
  const doc::Document& document = modeler_.document();
  if (modeler_.history().isClean()) {  // nothing that is not in a file
    clearRecovery();
    return true;
  }
  if (document.sequence() == autosavedSequence_ && QFileInfo::exists(recoveryFile_)) return true;
  viewport_->storeCamera(modeler_.document().settings().camera);
  const Status s = doc::saveDocument(document, utf8(recoveryFile_));
  if (!s) {
    statusBar()->showMessage(tr("自動保存できませんでした: %1").arg(QString::fromStdString(s.message())), 8000);
    return false;
  }
  QFile meta(recoveryFile_.chopped(4) + QStringLiteral(".json"));
  if (meta.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    const nlohmann::json j = {{"original", utf8(currentPath_)},
                              {"saved", QDateTime::currentDateTime().toString(Qt::ISODate).toStdString()}};
    meta.write(QByteArray::fromStdString(j.dump()));
  }
  autosavedSequence_ = document.sequence();
  statusBar()->showMessage(tr("自動保存しました（復旧用）"), 2500);
  return true;
}

void MainWindow::clearRecovery() {
  if (recoveryFile_.isEmpty()) return;
  QFile::remove(recoveryFile_);
  QFile::remove(recoveryFile_.chopped(4) + QStringLiteral(".json"));
  autosavedSequence_ = modeler_.document().sequence();
}

void MainWindow::abandonSessionForTest() {
  if (autosaveTimer_) autosaveTimer_->stop();
  sessionLock_.reset();  // unlocks; the recovery files stay as after a crash
  recoveryFile_.clear();
}

std::vector<MainWindow::Recoverable> MainWindow::findRecoverable() {
  std::vector<Recoverable> out;
  const QDir dir(recoveryDir());
  for (const QFileInfo& model : dir.entryInfoList({QStringLiteral("*.sbr")}, QDir::Files)) {
    const QString base = model.completeBaseName();
    // A session still running holds its lock.
    QLockFile lock(dir.filePath(base + QStringLiteral(".lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) continue;
    lock.unlock();
    Recoverable r{model.absoluteFilePath(), QString(), model.lastModified()};
    QFile meta(dir.filePath(base + QStringLiteral(".json")));
    if (meta.open(QIODevice::ReadOnly)) {
      try {
        const nlohmann::json j = nlohmann::json::parse(meta.readAll().toStdString());
        r.originalPath = QString::fromStdString(j.value("original", std::string()));
      } catch (...) {
      }
    }
    out.push_back(r);
  }
  std::sort(out.begin(), out.end(), [](const Recoverable& a, const Recoverable& b) { return a.saved > b.saved; });
  return out;
}

void MainWindow::discardRecovery(const Recoverable& r) {
  QFile::remove(r.file);
  QFile::remove(r.file.chopped(4) + QStringLiteral(".json"));
}

bool MainWindow::recover(const Recoverable& r) {
  if (active_) active_->cancel();
  exitGroup();
  std::vector<std::string> warnings;
  const Status s = modeler_.load(utf8(r.file), &warnings, false);
  if (!s) {
    reportError(tr("復旧できませんでした: %1").arg(QString::fromStdString(s.message())));
    return false;
  }
  startRecipeVerification();
  currentPath_ = r.originalPath;
  modeler_.history().markDirty();  // not saved to its file yet
  setSelection({});
  viewport_->syncNow();
  viewport_->restoreCamera(modeler_.document().settings().camera);
  refreshScenes();
  updateTitle();
  // This session keeps it safe from now on; the old file can go.
  autosavedSequence_ = 0;
  autosaveNow();
  discardRecovery(r);
  setStatus(tr("前回の作業を復旧しました。保存すると %1 に書き込みます")
                .arg(r.originalPath.isEmpty() ? tr("新しいファイル") : r.originalPath));
  return true;
}

void MainWindow::offerRecovery() {
  const std::vector<Recoverable> found = findRecoverable();
  if (found.empty()) return;
  const Recoverable& newest = found.front();
  const QString what = newest.originalPath.isEmpty() ? tr("無題のモデル") : QFileInfo(newest.originalPath).fileName();
  QMessageBox box(QMessageBox::Question, tr("作業の復旧"),
                  tr("前回、正しく終了しなかった作業があります。\n%1（%2 に自動保存）\n\n復旧しますか？")
                      .arg(what, QLocale().toString(newest.saved, QLocale::ShortFormat)),
                  QMessageBox::NoButton, this);
  QPushButton* restore = box.addButton(tr("復旧する"), QMessageBox::AcceptRole);
  QPushButton* discard = box.addButton(tr("破棄する"), QMessageBox::DestructiveRole);
  box.addButton(tr("あとで"), QMessageBox::RejectRole);
  if (found.size() > 1) box.setInformativeText(tr("ほかに %1 件あります（次回の起動でも確認できます）").arg(found.size() - 1));
  box.exec();
  if (box.clickedButton() == restore) {
    recover(newest);
  } else if (box.clickedButton() == discard) {
    discardRecovery(newest);
  }
}

void MainWindow::showAutosaveDialog() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("自動保存とバックアップ"));
  auto* form = new QFormLayout(&dialog);
  auto* minutes = new QSpinBox(&dialog);
  minutes->setRange(0, 60);
  minutes->setSuffix(tr(" 分ごと"));
  minutes->setSpecialValueText(tr("しない"));
  minutes->setValue(autosaveMinutes_);
  auto* generations = new QSpinBox(&dialog);
  generations->setRange(0, 20);
  generations->setSuffix(tr(" 世代"));
  generations->setSpecialValueText(tr("残さない"));
  generations->setValue(backupGenerations_);
  form->addRow(tr("自動保存（復旧用）"), minutes);
  form->addRow(tr("保存時に前の版を残す"), generations);
  form->addRow(new QLabel(tr("自動保存は元のファイルを書き換えません（復旧用の別ファイル）。\n"
                             "前の版は「ファイル名.sbr.bak1」（新しい順）として同じフォルダに残ります。"),
                          &dialog));
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted) return;
  setAutosaveMinutes(minutes->value());
  backupGenerations_ = generations->value();
  QSettings settings;
  settings.setValue(QStringLiteral("autosave/minutes"), autosaveMinutes_);
  settings.setValue(QStringLiteral("backup/generations"), backupGenerations_);
}

bool MainWindow::importUnderlay(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    reportError(tr("画像を開けませんでした: %1").arg(path));
    return false;
  }
  const QByteArray bytes = file.readAll();
  QImage picture;
  if (!picture.loadFromData(bytes)) {
    reportError(tr("画像として読めませんでした: %1").arg(path));
    return false;
  }
  if (active_) active_->cancel();
  const auto id = modeler_.addImageUnderlay(bytes.toStdString(), utf8(QFileInfo(path).fileName()), picture.width(),
                                            picture.height(), geom::SketchPlane::ground(), 10000.0);
  if (!id) {
    reportError(QString::fromStdString(id.error()));
    return false;
  }
  setSelection({id.value()});
  viewport_->fitAll();
  // Straight into the calibration: two points on the picture, then the
  // real distance between them.
  activateTool(ToolId::Scale);
  setStatus(tr("実寸合わせ：画像の上で寸法の分かる 2 点をクリックし、その実際の距離を入力（例: 7280 か 7.28m）。"
               "合わせたら 編集 > ロック（Ctrl+L）で固定して、上からなぞれます"));
  return true;
}

bool MainWindow::placeOpeningsOnSelection(int count, double width, double height, double sill,
                                          std::optional<double> spacing) {
  if (selection_.size() != 1) {
    reportError(tr("開口を入れる壁を 1 つ選んでください（面を選ぶとその面に入ります）"));
    return false;
  }
  const doc::ObjectId host = selection_.front();
  // The face picked, else the largest vertical planar face of the object.
  std::optional<gp_Dir> normal;
  for (const ElementRef& e : elements_) {
    if (e.objectId != host || e.type != TopAbs_FACE) continue;
    const TopoDS_Shape f = viewport_->elementShape(e);
    if (f.IsNull()) continue;
    if (const auto ref = geom::FaceRef::fromFace(TopoDS::Face(f)); ref && std::abs(ref->normal.Z()) < 1e-6) {
      normal = ref->normal;
      break;
    }
  }
  if (!normal) {
    const doc::Object* o = modeler_.document().find(host);
    if (!o || o->shape.IsNull()) return false;
    double best = 0;
    for (TopExp_Explorer x(modeler_.document().worldShape(*o), TopAbs_FACE); x.More(); x.Next()) {
      const TopoDS_Face f = TopoDS::Face(x.Current());
      const auto ref = geom::FaceRef::fromFace(f);
      if (!ref || std::abs(ref->normal.Z()) > 1e-6) continue;
      const double a = geom::area(f);
      if (a > best + 1e-6) {
        best = a;
        normal = ref->normal;
      }
    }
  }
  if (!normal) {
    reportError(tr("この物には縦の平らな面がありません"));
    return false;
  }
  const auto r = modeler_.placeOpenings(host, *normal, count, width, height, sill, spacing);
  if (!r) {
    reportError(QString::fromStdString(r.error()));
    return false;
  }
  elements_.clear();
  setSelection({r.value()});
  setStatus(tr("開口を %1 か所あけました（サイズはプロパティで変更できます）").arg(count));
  return true;
}

void MainWindow::showOpeningsDialog() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("開口を等間隔に配置"));
  auto* form = new QFormLayout(&dialog);
  auto* count = new QSpinBox(&dialog);
  count->setRange(1, 200);
  count->setValue(3);
  auto* width = new QLineEdit(QStringLiteral("1600"), &dialog);
  auto* height = new QLineEdit(QStringLiteral("1100"), &dialog);
  auto* sill = new QLineEdit(QStringLiteral("900"), &dialog);
  auto* spacing = new QLineEdit(&dialog);
  spacing->setPlaceholderText(tr("空欄なら面に均等"));
  form->addRow(tr("個数"), count);
  form->addRow(tr("幅"), width);
  form->addRow(tr("高さ"), height);
  form->addRow(tr("床からの高さ（腰）"), sill);
  form->addRow(tr("中心の間隔"), spacing);
  form->addRow(new QLabel(tr("選んだ壁の面（面を選んでいなければ一番大きい縦の面）に開けます"), &dialog));
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted) return;
  const auto w = units::parseLength(width->text().toStdString());
  const auto h = units::parseLength(height->text().toStdString());
  const auto s = units::parseLength(sill->text().toStdString());
  std::optional<double> gap;
  if (!spacing->text().trimmed().isEmpty()) {
    gap = units::parseLength(spacing->text().toStdString());
    if (!gap) {
      reportError(tr("間隔を長さとして読めません"));
      return;
    }
  }
  if (!w || !h || !s) {
    reportError(tr("幅・高さ・腰の高さを長さで入力してください"));
    return;
  }
  placeOpeningsOnSelection(count->value(), *w, *h, *s, gap);
}

bool MainWindow::resizeOpening(const doc::ObjectId& host, int index, double width, double height) {
  if (index < 1) return false;  // numbered from 1
  const Status s = modeler_.setOpeningSize(host, {static_cast<std::size_t>(index)}, width, height);
  if (!s) {
    reportError(tr("開口のサイズを変更できません: %1").arg(QString::fromStdString(s.message())));
    return false;
  }
  return true;
}

bool MainWindow::assignLevelsFromText(const QString& text) {
  std::vector<doc::Modeler::Level> levels;
  for (const QString& part : text.split(QRegularExpression(QStringLiteral("[,\\n、]")), Qt::SkipEmptyParts)) {
    const int eq = part.indexOf(QLatin1Char('='));
    const QString name = part.left(eq).trimmed();
    const auto z = eq > 0 ? units::parseLength(part.mid(eq + 1).trimmed().toStdString()) : std::nullopt;
    if (name.isEmpty() || !z) {
      reportError(tr("「%1」を読めません（例: 1F=0, 2F=3000）").arg(part.trimmed()));
      return false;
    }
    levels.push_back({name.toStdString(), *z});
  }
  if (levels.empty()) {
    reportError(tr("階を 1 つ以上入力してください（例: 1F=0, 2F=3000）"));
    return false;
  }
  const auto r = modeler_.assignLevels(selection_, levels);
  if (!r) {
    reportError(QString::fromStdString(r.error()));
    return false;
  }
  setStatus(tr("%1 個に階を割り当てました（属性 level）").arg(r.value().size()));
  return true;
}

void MainWindow::showLevelsDialog() {
  bool ok = false;
  const QString text = QInputDialog::getText(
      this, tr("階を割り当て"),
      tr("階の名前と床の高さ（例: 1F=0, 2F=3000, RF=6000）\n選んだ物（なければ全体）の一番低い点で決めます"),
      QLineEdit::Normal, QStringLiteral("1F=0, 2F=3000"), &ok);
  if (ok) assignLevelsFromText(text);
}

bool MainWindow::alignSelection(int axis, int edge) {
  if (selection_.size() < 2) {
    reportError(tr("2 つ以上選んでください（最初に選んだ物に揃えます）"));
    return false;
  }
  if (!report(modeler_.align(selection_, axis, edge))) return false;
  setStatus(tr("%1 個を整列しました").arg(selection_.size() - 1));
  return true;
}

void MainWindow::commitMeasurement() {
  const bool typed = userTyping_;
  const QString text = measurement_->text().trimmed();
  userTyping_ = false;
  // Enter without typing finishes a wall run / slab outline (the box only
  // shows the current length then).
  if (!typed && (active_ == wallTool_.get() || active_ == slabTool_.get())) {
    active_->applyMeasurement(QString());
    viewport_->setFocus();
    return;
  }
  if (text.isEmpty() || !active_) return;
  active_->applyMeasurement(text);
  refreshConstraints();
  viewport_->setFocus();
}

void MainWindow::undo() {
  if (active_) active_->cancel();
  modeler_.undo();
  syncEditContext();
  if (active_) active_->activate();
}

void MainWindow::redo() {
  if (active_) active_->cancel();
  modeler_.redo();
  syncEditContext();
  if (active_) active_->activate();
}

void MainWindow::syncEditContext() {
  // Undo/redo can remove the group being edited (e.g. entering a component).
  const doc::ObjectId group = modeler_.activeGroup();
  if (group.empty() || modeler_.document().find(group)) return;
  modeler_.setActiveGroup({});
  contextLabel_->hide();
  viewport_->setEditContext({});
}

void MainWindow::deleteSelection() {
  if (selection_.empty()) return;
  // Only edges selected (dividing lines): erase them, merging their faces.
  const bool onlyEdges = !elements_.empty() && std::all_of(elements_.begin(), elements_.end(), [](const ElementRef& e) {
    return e.type == TopAbs_EDGE;
  });
  const doc::Object* single = selection_.size() == 1 ? modeler_.document().find(selection_.front()) : nullptr;
  if (onlyEdges && single && (single->kind() == doc::ObjectKind::Solid || single->kind() == doc::ObjectKind::Face)) {
    std::vector<TopoDS_Edge> edges;
    for (const ElementRef& e : elements_) {
      const TopoDS_Shape edge = viewport_->elementShape(e);
      if (!edge.IsNull()) edges.push_back(TopoDS::Edge(edge));
    }
    modeler_.history().beginGroup("Erase Line");
    doc::ObjectId id = selection_.front();
    for (const TopoDS_Edge& edge : edges) {
      const auto r = modeler_.eraseEdge(id, edge);
      if (!r) {
        modeler_.history().abortGroup();
        reportError(QString::fromStdString(r.error()));
        return;
      }
      id = r.value();
    }
    modeler_.history().endGroup();
    setSelection({});
    return;
  }
  // Only faces of one solid / open shape: delete those faces (it stays open).
  const bool onlyFaces = !elements_.empty() && std::all_of(elements_.begin(), elements_.end(), [](const ElementRef& e) {
    return e.type == TopAbs_FACE;
  });
  if (onlyFaces && single && (single->kind() == doc::ObjectKind::Solid || single->kind() == doc::ObjectKind::Shell)) {
    const auto all = viewport_->allElements(single->id);
    const long faceCount = std::count_if(all.begin(), all.end(), [](const ElementRef& e) { return e.type == TopAbs_FACE; });
    if (static_cast<long>(elements_.size()) < faceCount) {
      std::vector<TopoDS_Face> faces;
      for (const ElementRef& e : elements_) {
        const TopoDS_Shape face = viewport_->elementShape(e);
        if (!face.IsNull()) faces.push_back(TopoDS::Face(face));
      }
      if (report(modeler_.removeFaces(single->id, faces))) {
        setSelection({});
        setStatus(tr("面を消しました（開いた形状。編集 > 穴をふさぐ でソリッドに戻せます）"));
      }
      return;
    }
  }
  if (!elements_.empty()) {
    // Faces/edges cannot be removed from a solid on their own yet; only
    // delete objects whose geometry is selected completely (triple-click).
    for (const doc::ObjectId& id : selection_) {
      const auto all = viewport_->allElements(id);
      const bool whole = std::all_of(all.begin(), all.end(), [this](const ElementRef& e) {
        return std::find(elements_.begin(), elements_.end(), e) != elements_.end();
      });
      // A loose profile or curve is a single face/edge: deleting it is natural.
      const doc::Object* o = modeler_.document().find(id);
      const bool loose = o && (o->kind() == doc::ObjectKind::Face || o->kind() == doc::ObjectKind::Curve);
      if (!whole && !loose) {
        reportError(tr("ソリッドの面だけは削除できません（面を分けた線は選んで Delete か消しゴムで消せます）。"
                       "トリプルクリックでオブジェクト全体を選んでから削除してください"));
        return;
      }
    }
  }
  const Status s = modeler_.remove(selection_);
  if (!s) reportError(QString::fromStdString(s.message()));
  setSelection({});
}

bool MainWindow::report(const Status& status) {
  if (!status) reportError(QString::fromStdString(status.message()));
  return status.ok();
}

void MainWindow::selectAll() {
  std::vector<doc::ObjectId> roots;
  for (const doc::Object& o : modeler_.document().objects()) {
    if (o.parent.empty()) roots.push_back(o.id);
  }
  setSelection(roots);
}

void MainWindow::groupSelection() {
  auto id = modeler_.group(selection_);
  if (!id) {
    reportError(tr("グループ化できません: %1").arg(QString::fromStdString(id.error())));
    return;
  }
  setSelection({id.value()});
}

void MainWindow::ungroupSelection() {
  std::vector<doc::ObjectId> children;
  for (const doc::ObjectId& id : selection_) {
    const doc::Object* o = modeler_.document().find(id);
    if (!o || o->kind() != doc::ObjectKind::Group) continue;
    for (const doc::ObjectId& c : modeler_.document().children(id)) children.push_back(c);
    report(modeler_.ungroup(id));
  }
  setSelection(children);
}

void MainWindow::makeComponent() {
  auto id = modeler_.makeComponent(selection_);
  if (!id) {
    reportError(tr("コンポーネントを作成できません: %1").arg(QString::fromStdString(id.error())));
    return;
  }
  setSelection({id.value()});
}

void MainWindow::explodeSelection() {
  std::vector<doc::ObjectId> parts;
  for (const doc::ObjectId& id : selection_) {
    const doc::Object* o = modeler_.document().find(id);
    if (!o) continue;
    if (o->kind() == doc::ObjectKind::Instance) {
      auto exploded = modeler_.explode(id);
      if (!exploded) {
        reportError(QString::fromStdString(exploded.error()));
        continue;
      }
      parts.insert(parts.end(), exploded->begin(), exploded->end());
    } else if (o->kind() == doc::ObjectKind::Group) {
      for (const doc::ObjectId& c : modeler_.document().children(id)) parts.push_back(c);
      report(modeler_.ungroup(id));
    }
  }
  setSelection(parts);
}

void MainWindow::placementAction(const std::vector<doc::ObjectId>& ids, const QString& action) {
  std::vector<doc::ObjectId> instances;
  for (const doc::ObjectId& id : ids) {
    const doc::Object* o = modeler_.document().find(id);
    if (o && o->kind() == doc::ObjectKind::Instance) instances.push_back(id);
  }
  if (instances.empty()) {
    reportError(tr("コンポーネントの配置を選んでください"));
    return;
  }
  if (action == QStringLiteral("axes")) {
    setSelection({instances.front()});
    activateTool(ToolId::ComponentAxes);
    return;
  }
  for (const doc::ObjectId& id : instances) {
    if (action == QStringLiteral("unique")) {
      const auto unique = modeler_.makeUnique(id);
      if (!unique) {
        reportError(QString::fromStdString(unique.error()));
        continue;
      }
      setStatus(tr("この配置を「%1」として独立させました")
                           .arg(QString::fromStdString(modeler_.document().findDefinition(*unique)->name)));
    } else if (action == QStringLiteral("reset")) {
      report(modeler_.clearPlacementParameters(id));
    } else if (action.startsWith(QStringLiteral("mirror"))) {
      report(modeler_.mirrorPlacement(id, action.back().digitValue()));
    }
  }
  setSelection(instances);
}

void MainWindow::booleanSelection(doc::BooleanKind kind) {
  auto id = modeler_.boolean(kind, selection_);
  if (!id) {
    reportError(tr("ソリッド演算に失敗しました: %1").arg(QString::fromStdString(id.error())));
    return;
  }
  setSelection({id.value()});
}

// --- files ------------------------------------------------------------------------

bool MainWindow::maybeSave() {
  if (modeler_.history().isClean()) return true;
  const auto answer = QMessageBox::question(
      this, tr("SketchBRep"), tr("変更を保存しますか？"),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
  if (answer == QMessageBox::Cancel) return false;
  if (answer == QMessageBox::Save) return save();
  return true;
}

void MainWindow::newDocument() {
  if (!maybeSave()) return;
  if (active_) active_->cancel();
  exitGroup();
  modeler_.newDocument();
  currentPath_.clear();
  clearRecovery();
  setSelection({});
  viewport_->setStandardView(Viewport::StandardView::Iso);
  refreshScenes();
  updateTitle();
}

bool MainWindow::openFile(const QString& path) {
  if (active_) active_->cancel();
  exitGroup();
  std::vector<std::string> warnings;
  // Stored geometry first (quick even for large models); the recipes are
  // checked afterwards in the background.
  const Status s = modeler_.load(utf8(path), &warnings, false);
  if (!s) {
    reportError(tr("開けませんでした: %1").arg(QString::fromStdString(s.message())));
    return false;
  }
  startRecipeVerification();
  currentPath_ = path;
  setSelection({});
  viewport_->syncNow();
  viewport_->restoreCamera(modeler_.document().settings().camera);
  perspectiveAction_->setChecked(modeler_.document().settings().camera.perspective);
  refreshScenes();
  if (!warnings.empty()) {
    QStringList lines;
    for (const auto& w : warnings) lines << QString::fromStdString(w);
    QMessageBox::warning(this, tr("読み込み時の警告"), lines.join(QStringLiteral("\n")));
  }
  updateTitle();
  if (active_) active_->activate();
  return true;
}

void MainWindow::startRecipeVerification() {
  if (!verifyTimer_) {
    verifyTimer_ = new QTimer(this);
    verifyTimer_->setInterval(0);
    connect(verifyTimer_, &QTimer::timeout, this, [this] {
      // ~15 ms of checking per idle moment keeps the window responsive.
      QElapsedTimer slice;
      slice.start();
      const std::size_t total = modeler_.document().objects().size();
      while (verifyCursor_ < total && slice.elapsed() < 15) {
        for (const std::string& w : modeler_.verifyRecipes(verifyCursor_, 8)) {
          recipeWarnings_ << QString::fromStdString(w);
        }
      }
      if (verifyCursor_ < total) return;
      verifyTimer_->stop();
      if (!recipeWarnings_.isEmpty()) {
        statusBar()->showMessage(tr("レシピの確認で %1 件の注意（保存された形状を使っています）")
                                     .arg(recipeWarnings_.size()),
                                 15000);
        statusBar()->setToolTip(recipeWarnings_.join(QStringLiteral("\n")));
      }
    });
  }
  verifyCursor_ = 0;
  recipeWarnings_.clear();
  verifyTimer_->start();
}

bool MainWindow::saveFile(const QString& path) {
  viewport_->storeCamera(modeler_.document().settings().camera);
  // Previous versions of the file stay as .bak1 .. .bakN.
  if (const Status kept = doc::rotateBackups(utf8(path), backupGenerations_); !kept) {
    reportError(QString::fromStdString(kept.message()));
  }
  const Status s = modeler_.save(utf8(path));
  if (!s) {
    reportError(tr("保存できませんでした: %1").arg(QString::fromStdString(s.message())));
    QMessageBox::critical(this, tr("保存エラー"), QString::fromStdString(s.message()));
    return false;
  }
  currentPath_ = path;
  updateTitle();
  clearRecovery();  // the work is in its file now
  statusBar()->showMessage(tr("保存しました: %1").arg(path), 4000);
  return true;
}

void MainWindow::open() {
  if (!maybeSave()) return;
  const QString path = QFileDialog::getOpenFileName(this, tr("開く"), QString(),
                                                    tr("SketchBRep (*.sbr)"));
  if (!path.isEmpty()) openFile(path);
}

bool MainWindow::save() {
  if (currentPath_.isEmpty()) return saveAs();
  return saveFile(currentPath_);
}

bool MainWindow::saveAs() {
  QString path = QFileDialog::getSaveFileName(this, tr("名前を付けて保存"), currentPath_,
                                              tr("SketchBRep (*.sbr)"));
  if (path.isEmpty()) return false;
  if (!path.endsWith(QStringLiteral(".sbr"), Qt::CaseInsensitive)) path += QStringLiteral(".sbr");
  return saveFile(path);
}

void MainWindow::importStep() {
  const QString path = QFileDialog::getOpenFileName(this, tr("STEP インポート"), QString(),
                                                    tr("STEP (*.step *.stp)"));
  if (path.isEmpty()) return;
  auto ids = modeler_.importStep(utf8(path));
  if (!ids) {
    reportError(QString::fromStdString(ids.error()));
    return;
  }
  setSelection(ids.value());
  viewport_->syncNow();
  viewport_->fitAll();
}

void MainWindow::exportStep() {
  QString path = QFileDialog::getSaveFileName(this, tr("STEP エクスポート"), QString(),
                                              tr("STEP (*.step *.stp)"));
  if (path.isEmpty()) return;
  if (!path.endsWith(QStringLiteral(".step"), Qt::CaseInsensitive) &&
      !path.endsWith(QStringLiteral(".stp"), Qt::CaseInsensitive)) {
    path += QStringLiteral(".step");
  }
  const Status s = modeler_.exportStep(utf8(path), selection_);
  if (!s) {
    reportError(QString::fromStdString(s.message()));
  } else {
    statusBar()->showMessage(tr("STEP を書き出しました: %1").arg(path), 4000);
  }
}

void MainWindow::import3dm() {
  const QString path = QFileDialog::getOpenFileName(this, tr("Rhino 3DM インポート"), QString(),
                                                    tr("Rhino (*.3dm)"));
  if (path.isEmpty()) return;
  auto ids = modeler_.import3dm(utf8(path));
  if (!ids) {
    reportError(QString::fromStdString(ids.error()));
    return;
  }
  setSelection(ids.value());
  viewport_->syncNow();
  viewport_->fitAll();
  setStatus(tr("3DM から %1 個を読み込みました（単位は mm に換算、レイヤはタグに）").arg(ids->size()));
}

void MainWindow::export3dm() {
  QString path = QFileDialog::getSaveFileName(this, tr("Rhino 3DM エクスポート"), QString(), tr("Rhino (*.3dm)"));
  if (path.isEmpty()) return;
  if (!path.endsWith(QStringLiteral(".3dm"), Qt::CaseInsensitive)) path += QStringLiteral(".3dm");
  const Status s = modeler_.export3dm(utf8(path), selection_);
  if (!s) {
    reportError(QString::fromStdString(s.message()));
  } else {
    statusBar()->showMessage(tr("3DM を書き出しました（正確な NURBS、タグはレイヤ）: %1").arg(path), 5000);
  }
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (maybeSave()) {
    event->accept();
  } else {
    event->ignore();
  }
}

}  // namespace sbr::app
