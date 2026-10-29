#include "MainWindow.h"
#include "DwgConverter.h"
#include "ToolPalette.h"
#include "Branding.h"
#include <QTemporaryDir>

#include <algorithm>
#include <unordered_set>
#include <cstdint>
#include <map>

#include <Standard_Failure.hxx>
#include <QAction>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QImage>

#include <Standard_Version.hxx>
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
#include <QCheckBox>
#include <QElapsedTimer>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QInputDialog>
#include <QProgressDialog>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QMenu>
#include <QStatusBar>
#include <QTabBar>
#include <QToolButton>
#include <QTimer>
#include <QToolBar>
#include <QPainter>
#include <QScrollArea>
#include <QGridLayout>

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
#include "McpHttpServer.h"
#include "Messages.h"
#include "ProgressDialog.h"
#include "WheelZoom.h"
#include "Cursors.h"
#include "MaterialEditor.h"
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

enum class ExchangeType { Step, Stl, Dxf, Dwg, Iges, Brep, Rhino, Image };
struct ExchangeFormat {
  ExchangeType type;
  QString filter;
  QStringList extensions;
};
std::vector<ExchangeFormat> exchangeFormats(bool importing) {
  auto text=[](const char* source){return QCoreApplication::translate("MainWindow",source);};
  std::vector<ExchangeFormat> formats{{ExchangeType::Step,text("STEP ファイル (*.step *.stp)"),{"step","stp"}}};
  if(importing) {
    formats.push_back({ExchangeType::Dxf,text("DXF 2D図面 (*.dxf)"),{"dxf"}});
    formats.push_back({ExchangeType::Dwg,text("DWG 2D図面 (*.dwg)"),{"dwg"}});
  } else formats.push_back({ExchangeType::Stl,text("STL ファイル (*.stl)"),{"stl"}});
  formats.push_back({ExchangeType::Iges,text("IGES ファイル (*.igs *.iges)"),{"igs","iges"}});
  formats.push_back({ExchangeType::Brep,text("BREP ファイル (*.brep *.brp)"),{"brep","brp"}});
  formats.push_back({ExchangeType::Rhino,text("Rhino 3DM ファイル (*.3dm)"),{"3dm"}});
  formats.push_back(importing?ExchangeFormat{ExchangeType::Image,text("画像・下敷き (*.png *.jpg *.jpeg *.bmp *.gif)"),{"png","jpg","jpeg","bmp","gif"}}:
                             ExchangeFormat{ExchangeType::Image,text("PNG 画像 (*.png)"),{"png"}});
  return formats;
}

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
  rotatedRectangleTool_ = std::make_unique<RotatedRectangleTool>(*this);
  scaleTool_ = std::make_unique<ScaleTool>(*this);
  componentAxesTool_ = std::make_unique<ComponentAxesTool>(*this);
  wallTool_ = std::make_unique<WallTool>(*this);
  memberTool_ = std::make_unique<MemberTool>(*this);
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
  sectionTool_->setOnPlaced([this] {
    QTimer::singleShot(0, this, [this] { if (active_ == sectionTool_.get()) activateTool(ToolId::Select); });
  });
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
  if (sectionDock_ && sectionDock_->isFloating())
    QSettings().setValue("section/toolboxGeometry", sectionDock_->saveGeometry());
  if (auto* palette = findChild<QDockWidget*>(QStringLiteral("toolsPalette"))) {
    QSettings settings;
    settings.setValue("tools/floating", palette->isFloating());
    settings.setValue("tools/geometry", palette->saveGeometry());
    settings.setValue("tools/area", static_cast<int>(dockWidgetArea(palette)));
  }
  stopRecipeVerification();
  if (autosaveThread_.joinable()) autosaveThread_.join();
  QFile::remove(recoveryFile_ + QStringLiteral(".part"));
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

  // Drag previews must not animate / repeatedly resize the native 3D view.
  setDockOptions(dockOptions() & ~QMainWindow::AnimatedDocks);

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
  auto* dock = new PanelDock(tr("オブジェクト / 属性"), this);
  dock->setObjectName(QStringLiteral("propertiesDock"));
  dock->setWidget(panel_);
  dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
  addDockWidget(Qt::RightDockWidgetArea, dock);
  resizeDocks({dock}, {360}, Qt::Horizontal);

  // Tags: tick to show / hide everything with that tag (SketchUp's Tags panel).
  tagList_ = new QListWidget;
  auto* tagDock = new PanelDock(tr("タグ"), this);
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
  materialsDock_ = new PanelDock(tr("マテリアル（素材）"), this);
  materialsDock_->setObjectName(QStringLiteral("materialsDock"));
  materialsDock_->setWidget(materialsPanel_);
  materialsDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                              QDockWidget::DockWidgetClosable);
  addDockWidget(Qt::RightDockWidgetArea, materialsDock_);
  tabifyDockWidget(tagDock, materialsDock_);
  materialsDock_->raise();
  connect(materialsPanel_, &MaterialsPanel::materialChosen, this, [this](const std::string& id) { chooseMaterial(id); });
  connect(materialsPanel_, &MaterialsPanel::presetRequested, this, [this](int i) { addPresetMaterial(i); });
  connect(materialsPanel_, &MaterialsPanel::presetPictureRequested, this,
          [this](const std::string& id, int index) { applyPresetPicture(id, index); });
  connect(materialsPanel_, &MaterialsPanel::createRequested, this, [this] { editMaterialDialog({}); });
  connect(materialsPanel_->editor(), &MaterialEditor::edited, this, [this](const doc::Material& m, bool continuing) {
    report(modeler_.editMaterial(m, continuing));
  });
  connect(materialsPanel_->editor(), &MaterialEditor::duplicateRequested, this, [this](const std::string& id) {
    auto copy = modeler_.duplicateMaterial(id);
    if (!copy) {
      reportError(QString::fromStdString(copy.error()));
      return;
    }
    chooseMaterial(*copy);
    materialsPanel_->openEditor();
    setStatus(tr("「%1」として複製しました。名前を変えて編集できます")
                  .arg(QString::fromStdString(modeler_.document().findMaterial(*copy)->name)));
  });
  connect(materialsPanel_->editor(), &MaterialEditor::textureFileChosen, this,
          [this](const std::string& id, const QString& path) { setMaterialTexture(id, path); });
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
  connect(viewport_, &Viewport::contextMenuRequested, this, [this](const QPoint& pos) {
    QMenu* menu = buildContextMenu(pos);
    menu->exec(viewport_->mapToGlobal(pos));
    menu->deleteLater();
  });
  connect(viewport_, &Viewport::toolFailed, this, [this](const QString& message) {
    reportError(tr("形の計算でエラーが起きたので、操作をやり直せる状態に戻しました（モデルは変わっていません）%1")
                    .arg(message.isEmpty() ? QString() : QStringLiteral("： ") + message));
  });
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
  connect(panel_, &PropertiesPanel::attributeEdited, this,
          [this](const doc::ObjectId& id, const std::string& key, const std::string& value) {
            const doc::Object* o = modeler_.document().find(id);
            if (!o) return;
            const auto it = o->attributes.find(key);
            const std::string now = it == o->attributes.end() ? std::string() : it->second;
            if (now == value) return;
            QTimer::singleShot(0, this, [this, id, key, value] { report(modeler_.setAttribute({id}, key, value)); });
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
  connect(panel_, &PropertiesPanel::materialAssigned, this, [this](const doc::ObjectId& id, const std::string& mid) {
    QTimer::singleShot(0, this, [this, id, mid] { report(modeler_.assignMaterial({id}, mid)); });
  });
  connect(panel_, &PropertiesPanel::materialEditRequested, this, [this](const std::string& mid) {
    QTimer::singleShot(0, this, [this, mid] {
      if (mid.empty() || !modeler_.document().findMaterial(mid)) {
        editMaterialDialog({});
        return;
      }
      chooseMaterial(mid);
      showMaterials();
      materialsPanel_->openEditor();
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
  file->addAction(tr("インポート..."),this,&MainWindow::importFile);
  file->addAction(tr("エクスポート..."),this,&MainWindow::exportFile);
  file->addAction(tr("DWG 変換ソフトの設定..."), this, &MainWindow::configureDwgConverter);
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
    report(modeler_.setLocked(selection_, true));  // stays selected, shown red
  });
  edit->addAction(tr("ロック解除"), this, [this] { report(modeler_.setLocked(selection_, false)); });
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
  edit->addAction(tr("選んだ辺の線を隠す"), this, [this] {
    // Selected edges keep their faces split; only the line goes (消しゴム Shift+クリックと同じ).
    std::map<doc::ObjectId, std::vector<TopoDS_Edge>> byObject;
    for (const ElementRef& el : elementSelection()) {
      if (el.type != TopAbs_EDGE) continue;
      const TopoDS_Shape edge = viewport_->elementShape(el);
      if (!edge.IsNull()) byObject[el.objectId].push_back(TopoDS::Edge(edge));
    }
    if (byObject.empty()) {
      reportError(tr("隠す辺を選択ツールで選んでください"));
      return;
    }
    for (const auto& [id, edges] : byObject) {
      if (!report(modeler_.setEdgesHidden(id, edges, true))) return;
    }
    setSelection({});
  });
  edit->addAction(tr("隠した線をすべて表示"), this, [this] {
    std::vector<doc::ObjectId> ids = selection_;
    if (ids.empty()) {
      for (const doc::Object& o : modeler_.document().objects()) {
        if (o.parent.empty()) ids.push_back(o.id);
      }
    }
    report(modeler_.showHiddenEdges(ids));
  });
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
  auto* createSection = sections->addAction(toolIcon(CursorKind::Section), tr("断面平面を配置 (Shift+S)"), this,
                                             [this] { activateTool(ToolId::Section); });
  sectionPlanesAction_ = sections->addAction(toolIcon(CursorKind::SectionPlanes), tr("断面平面を表示"));
  sectionPlanesAction_->setObjectName(QStringLiteral("sectionPlanesAction"));
  sectionPlanesAction_->setCheckable(true);
  sectionPlanesAction_->setToolTip(tr("断面の目印だけを表示／非表示（カットは維持）"));
  sectionVisibleAction_ = sections->addAction(toolIcon(CursorKind::SectionCuts), tr("断面カットを表示"));
  sectionVisibleAction_->setObjectName(QStringLiteral("sectionCutsAction"));
  sectionVisibleAction_->setCheckable(true);
  sectionVisibleAction_->setToolTip(tr("選んだ断面で切って表示。OFFにしても選んだ断面は保持"));
  sectionFillAction_ = sections->addAction(toolIcon(CursorKind::SectionFill), tr("断面塗りつぶしを表示"));
  sectionFillAction_->setObjectName(QStringLiteral("sectionFillAction"));
  sectionFillAction_->setCheckable(true);
  sectionFillAction_->setToolTip(tr("切り口の塗りつぶしを表示／非表示（元の面や断面の目印は消しません）"));
  auto finishPlacement = [this] { if (active_ == sectionTool_.get()) activateTool(ToolId::Select); };
  connect(sectionPlanesAction_, &QAction::toggled, this, [this, finishPlacement](bool on) {
    finishPlacement(); modeler_.setSectionPlanesVisible(on); updateTitle();
  });
  connect(sectionVisibleAction_, &QAction::toggled, this, [this, finishPlacement](bool on) {
    finishPlacement(); modeler_.setSectionVisible(on); updateTitle();
  });
  connect(sectionFillAction_, &QAction::toggled, this, [this, finishPlacement](bool on) {
    finishPlacement(); modeler_.setSectionFillVisible(on); updateTitle();
  });
  auto* sectionPalette = new ToolPaletteDock(tr("断面"), this, 4, true);
  sectionDock_ = sectionPalette;
  sectionDock_->setObjectName(QStringLiteral("sectionToolbox"));
  sectionDock_->setAllowedAreas(Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);
  for (auto* action : {createSection, sectionPlanesAction_, sectionVisibleAction_, sectionFillAction_})
    sectionPalette->tools()->addTool(action);
  addDockWidget(Qt::TopDockWidgetArea, sectionDock_);
  sectionDock_->setFloating(true);
  sectionDock_->hide();
  sectionDock_->move(80, 100);
  const auto sectionGeometry = QSettings().value("section/toolboxGeometry").toByteArray();
  if (!sectionGeometry.isEmpty()) sectionDock_->restoreGeometry(sectionGeometry);
  sectionPalette->resetSize(); // discard old stretched sizes, preserve the position
  sectionDock_->toggleViewAction()->setText(tr("断面パレットを表示"));
  view->addAction(sectionDock_->toggleViewAction());
  auto* savedSections = sections->addMenu(tr("保存済み断面（SBRファイルに保存）"));
  connect(savedSections, &QMenu::aboutToShow, this, [this, savedSections] {
    savedSections->clear();
    const auto planes = modeler_.document().settings().sections;
    const auto revision = modeler_.history().revision();
    if (planes.empty()) savedSections->addAction(tr("断面はありません"))->setEnabled(false);
    for (std::size_t i = 0; i < planes.size(); ++i) {
      const auto plane = planes[i];
      auto* menu = savedSections->addMenu(QString::fromStdString(plane.name));
      auto* active = menu->addAction(tr("この断面でカット"));
      active->setCheckable(true);
      active->setChecked(plane.active && modeler_.document().settings().sectionDisplay.cuts);
      connect(active, &QAction::toggled, this, [this, i, revision](bool on) {
        if (modeler_.history().revision() != revision) return;
        auto p = modeler_.document().settings().sections.at(i);
        if (on) modeler_.activateSection(i);
        else { p.active = false; modeler_.updateSection(i, p); }
        updateTitle();
      });
      auto* visible = menu->addAction(tr("断面平面を表示"));
      visible->setCheckable(true);
      visible->setChecked(plane.visible);
      connect(visible, &QAction::toggled, this, [this, i, revision](bool on) {
        if (modeler_.history().revision() != revision) return;
        auto p = modeler_.document().settings().sections.at(i);
        p.visible = on; modeler_.updateSection(i, p); updateTitle();
      });
      menu->addAction(tr("名前を変更..."), this, [this, i, revision] {
        if (modeler_.history().revision() != revision) return;
        auto p = modeler_.document().settings().sections.at(i);
        bool ok = false;
        const auto name = QInputDialog::getText(this, tr("断面の名前"), tr("名前"), QLineEdit::Normal,
                                               QString::fromStdString(p.name), &ok);
        if (ok && modeler_.history().revision() == revision && !name.trimmed().isEmpty()) { p.name = name.trimmed().toStdString(); modeler_.updateSection(i, p); updateTitle(); }
      });
      menu->addAction(tr("位置を移動..."), this, [this, i, revision] {
        if (modeler_.history().revision() != revision) return;
        auto p = modeler_.document().settings().sections.at(i);
        bool ok = false;
        const double offset = QInputDialog::getDouble(this, tr("断面を移動"), tr("法線方向の移動量 (mm)"),
                                                       0, -1e9, 1e9, 3, &ok);
        if (!ok || modeler_.history().revision() != revision) return;
        const gp_Dir n(p.normal[0], p.normal[1], p.normal[2]);
        p.origin[0] += n.X()*offset; p.origin[1] += n.Y()*offset; p.origin[2] += n.Z()*offset;
        modeler_.updateSection(i, p); updateTitle();
      });
      menu->addAction(tr("カット方向を反転"), this, [this, i, revision] {
        if (modeler_.history().revision() != revision) return;
        auto p = modeler_.document().settings().sections.at(i);
        for (auto& n : p.normal) n = -n;
        modeler_.updateSection(i, p); updateTitle();
      });
      menu->addAction(tr("削除"), this, [this, i, revision] {
        if (modeler_.history().revision() == revision) { modeler_.removeSection(i); updateTitle(); }
      });
    }
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
  focusOnDoubleClick_ = view->addAction(tr("中ボタンのダブルクリックでフォーカス"));
  focusOnDoubleClick_->setCheckable(true);
  focusOnDoubleClick_->setChecked(true);
  view->addAction(tr("ホイールズームの設定..."), this, &MainWindow::showWheelZoomDialog);
  {
    const QSettings settings;
    viewport_->setWheelZoom(settings.value(QStringLiteral("view/wheelZoomRatio"), wheel::kDefaultRatioPerNotch).toDouble(),
                            settings.value(QStringLiteral("view/wheelZoomInverted"), false).toBool());
  }

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
  auto* palette = new ToolPaletteDock(tr("ツール"), this, 3);
  palette->setObjectName(QStringLiteral("toolsPalette"));
  palette->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
  const int savedArea = QSettings().value("tools/area", int(Qt::LeftDockWidgetArea)).toInt();
  const auto area = savedArea == Qt::RightDockWidgetArea ? Qt::RightDockWidgetArea : Qt::LeftDockWidgetArea;
  addDockWidget(area, palette);
  auto* expanded = view->addAction(tr("ツール名を表示"));
  expanded->setObjectName(QStringLiteral("expandedToolsAction"));
  expanded->setCheckable(true);
  expanded->setChecked(QSettings().value("tools/showLabels", false).toBool());
  palette->tools()->setShowLabels(expanded->isChecked());
  palette->toggleViewAction()->setText(tr("ツールパレットを表示"));
  view->addAction(palette->toggleViewAction());
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
      {tr("回転矩形"), QStringLiteral("Shift+R"), ToolId::RotatedRectangle, CursorKind::RotatedRectangle},
      {tr("壁"), QStringLiteral("W"), ToolId::Wall, CursorKind::Wall},
      {tr("スラブ"), QStringLiteral("Shift+W"), ToolId::Slab, CursorKind::Slab},
      {tr("開口"), QStringLiteral("O"), ToolId::Opening, CursorKind::Opening},
      {tr("部材"), QStringLiteral("I"), ToolId::Member, CursorKind::Member},
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
    palette->tools()->addTool(a);
    toolActions_[static_cast<int>(id)] = a;
  }

  connect(expanded, &QAction::toggled, this, [palette](bool on) {
    palette->tools()->setShowLabels(on);
    QSettings().setValue("tools/showLabels", on);
  });
  if (QSettings().value("tools/floating", false).toBool()) {
    palette->setFloating(true);
    palette->restoreGeometry(QSettings().value("tools/geometry").toByteArray());
  }
  // Migrate the old clipped/stretched palette once; keep the user's position.
  if (QSettings().value("tools/layoutVersion", 0).toInt() < 2) palette->resetSize();
  QSettings().setValue("tools/layoutVersion", 2);
  auto* resetPalettes = view->addAction(tr("パレットの配置を初期化"));
  resetPalettes->setObjectName(QStringLiteral("resetPalettesAction"));
  connect(resetPalettes, &QAction::triggered, this, [this, palette, sectionPalette, expanded] {
    expanded->setChecked(false);
    palette->setFloating(false);
    addDockWidget(Qt::LeftDockWidgetArea, palette);
    palette->show(); palette->resetSize();
    const bool sectionShown = sectionPalette->isVisible();
    sectionPalette->setFloating(true); sectionPalette->resetSize();
    sectionPalette->move(viewport_->mapToGlobal(QPoint(12, 12)));
    sectionPalette->setVisible(sectionShown);
  });
  tools->addSeparator();
  tools->addAction(tr("通り芯を作成..."), this, &MainWindow::showGridDialog);
  tools->addAction(tr("壁の設定..."), this, &MainWindow::showWallSettingsDialog);
  tools->addAction(tr("部材の設定..."), this, &MainWindow::showMemberSettingsDialog);
  // Direct entries (spec §4.2): the member tool with each one's own last
  // settings; one palette button stays.
  QMenu* members = tools->addMenu(tr("部材を引く"));
  members->setObjectName(QStringLiteral("membersMenu"));
  const std::pair<QString, std::string> memberEntries[] = {
      {tr("鋼材（断面を選んで2点）"), "steel"}, {tr("柱（脚部＋高さ）"), "column"}, {tr("梁（上端基準で2点）"), "beam"}};
  for (const auto& [label, entry] : memberEntries) {
    QAction* a = members->addAction(label);
    a->setObjectName(QStringLiteral("memberEntry_") + QString::fromStdString(entry));
    connect(a, &QAction::triggered, this, [this, entry = entry] { startMember(entry); });
  }
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
  aiAction_->setToolTip(tr("ローカルMCPと、有効にしたリモートMCP接続からモデルを操作できるようにします"));
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
  ai->addAction(tr("リモートMCP接続..."),this,&MainWindow::showChatGptConnection);
  ai->addAction(tr("AI 連携について"), this, [this] {
    QMessageBox::information(
        this, tr("AI 連携"),
        tr("「外部 AI からの操作を許可」を ON にすると、このパソコン上の AI クライアントが "
           "sbr_mcp（MCP サーバー）経由でモデルを操作できます。<br><br>"
           "・AI の操作も GUI と同じコマンドで実行され、形状チェック・失敗時の取り消し・Ctrl+Z が効きます<br>"
           "・AI はファイルの保存・開く・書き出しはできません（保存はあなたが行います）<br>"
           "・通常のMCP接続は同じユーザーのローカルプロセスのみです。リモートMCP接続を開始した場合は専用URLからも操作できます<br><br>"
           "AIクライアントは利用者が選べます。特定のAIモデルや有料AI API、OpenAI APIキーは必須ではありません。<br>設定: ローカルMCPは sbr_mcp を各クライアントの手順で登録し、外部クライアントは「リモートMCP接続...」を使います。操作の可否はサービス・プラン・環境に依存します。"));
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
  help->addSeparator();
  help->addAction(tr("SketchBRep について"), this, &MainWindow::showAbout);
}

void MainWindow::showAbout() {
  // A regular dialog avoids QMessageBox's platform-specific icon/footer
  // layout. Text stays native and crisp at every DPI and on either theme.
  QDialog dialog(this);
  dialog.setObjectName(QStringLiteral("aboutDialog"));
  dialog.setWindowTitle(tr("SketchBRep について"));
  dialog.setWindowIcon(applicationIcon());
  dialog.setAutoFillBackground(true);
  auto* layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(24, 24, 24, 20);
  layout->setSpacing(18);
  auto* brand = new QHBoxLayout;
  brand->setSpacing(16);
  auto* symbol = new QLabel(&dialog);
  symbol->setObjectName(QStringLiteral("aboutSymbol"));
  symbol->setFixedSize(80, 80);
  symbol->setPixmap(applicationIcon().pixmap(QSize(80, 80), dialog.devicePixelRatioF()));
  brand->addWidget(symbol);
  auto* identity = new QVBoxLayout;
  identity->setSpacing(3);
  const QColor ink = dialog.palette().color(QPalette::WindowText);
  const QColor accent(dialog.palette().color(QPalette::Window).lightness() < 128 ? "#65add3" : "#28718e");
  auto* wordmark = new QLabel(QStringLiteral("<span style='color:%1'>Sketch</span><span style='color:%2'>BRep</span>")
                                .arg(ink.name(), accent.name()), &dialog);
  wordmark->setObjectName(QStringLiteral("aboutWordmark"));
  QFont font = wordmark->font(); font.setPointSizeF(26); font.setWeight(QFont::DemiBold);
  wordmark->setFont(font);
  identity->addWidget(wordmark);
  auto* version = new QLabel(tr("バージョン %1").arg(QApplication::applicationVersion()), &dialog);
  version->setTextInteractionFlags(Qt::TextSelectableByMouse);
  identity->addWidget(version);
  brand->addLayout(identity);
  brand->addStretch();
  layout->addLayout(brand);
  auto* description = new QLabel(tr("建築設計向けハイブリッド 3D モデラー。\n"
                                    "SketchUpのように気軽に操作し、B-repで正確な形状を扱います。"), &dialog);
  description->setWordWrap(true);
  layout->addWidget(description);
  auto* libraries = new QLabel(tr("Open CASCADE %1  /  Qt %2")
                                  .arg(QStringLiteral(OCC_VERSION_COMPLETE), QString::fromLatin1(qVersion())), &dialog);
  libraries->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(libraries);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  layout->addWidget(buttons);
  dialog.setMinimumWidth(480);
  dialog.exec();
}

void MainWindow::showChatGptConnection() {
  QDialog dialog(this);dialog.setWindowTitle(tr("リモートMCP接続"));dialog.resize(760,680);
  auto* layout=new QVBoxLayout(&dialog);
  auto* intro=new QLabel(tr("MCP対応クライアントから、HTTPで起動中のこのモデルへ接続するための設定です。<br>"
      "1. 接続を開始<br>2. 必要に応じ、認証・アクセス制御を確認したHTTPS到達経路を用意<br>"
      "3. 公開HTTPS URLを入力し、生成した接続URLをMCPクライアントに設定"),&dialog);
  intro->setWordWrap(true);layout->addWidget(intro);
  auto* state=new QLabel(&dialog);layout->addWidget(state);
  auto* buttons=new QHBoxLayout;auto* start=new QPushButton(tr("接続を開始"),&dialog);start->setObjectName("chatGptStart");
  auto* stop=new QPushButton(tr("接続を停止"),&dialog);stop->setObjectName("chatGptStop");
  buttons->addWidget(start);buttons->addWidget(stop);buttons->addStretch();layout->addLayout(buttons);
  auto* form=new QFormLayout;
  auto* tunnel=new QLineEdit(&dialog);tunnel->setObjectName("chatGptTunnelCommand");tunnel->setReadOnly(true);
  auto* copyTunnel=new QPushButton(tr("コマンドをコピー"),&dialog);auto* tunnelRow=new QHBoxLayout;
  tunnelRow->addWidget(tunnel);tunnelRow->addWidget(copyTunnel);form->addRow(tr("HTTPSトンネルの例（ngrok）"),tunnelRow);
  auto* base=new QLineEdit(&dialog);base->setObjectName("chatGptPublicBase");base->setPlaceholderText("https://example.ngrok-free.app");
  if(auto* server=aiBridge_->chatGptServer();server&&!server->publicUrl().isEmpty())base->setText(server->publicUrl().adjusted(QUrl::RemovePath).toString());
  form->addRow(tr("公開HTTPS URL"),base);
  auto* endpoint=new QLineEdit(&dialog);endpoint->setObjectName("chatGptEndpoint");endpoint->setReadOnly(true);
  auto* copyEndpoint=new QPushButton(tr("接続URLをコピー"),&dialog);copyEndpoint->setObjectName("chatGptCopyEndpoint");
  auto* endpointRow=new QHBoxLayout;endpointRow->addWidget(endpoint);endpointRow->addWidget(copyEndpoint);form->addRow(tr("MCPクライアント用接続URL"),endpointRow);
  layout->addLayout(form);
  auto* availability=new QLabel(tr("<b>AIサービスの対応状況について</b><br>"
      "SketchBRepは標準MCPによる外部AI接続に対応しています。"
      "ただし、利用可能な操作はAIサービス、契約プラン、クライアント環境によって異なります。"
      "一部のAIサービスでは、MCPサーバーを登録できても、通常チャットからのモデル作成・編集が利用できない場合があります。"
      "詳細は利用するAIサービスの公式ドキュメントをご確認ください。"),&dialog);
  availability->setWordWrap(true);layout->addWidget(availability);
  auto* help=new QLabel(tr("HTTPSの暗号化とアクセス認証は別です。CAD編集命令を受け付けるため、認証なしのインターネット公開には危険があります。"
      "接続URLを秘密にするだけでは、十分なアクセス制御とは限りません。URLを共有せず、公開経路の認証・アクセス制御を確認してください。<br><br>"
      "「接続を停止」または「外部 AI からの操作を許可」をOFFにすると切断します。再開後はURLを設定し直してください。"
      "AIの操作はCtrl+Zで取り消せます。ファイル操作の制限は従来どおりです。"
      "トンネルの起動やクライアントの登録は、この画面では自動実行しません。"),&dialog);
  help->setWordWrap(true);layout->addWidget(help);
  auto update=[&]{
    auto* server=aiBridge_->chatGptServer();const bool running=server&&server->isListening();
    start->setEnabled(!running);stop->setEnabled(running);copyTunnel->setEnabled(running);base->setEnabled(running);
    tunnel->setText(running?QString("ngrok http http://127.0.0.1:%1").arg(server->port()):QString());
    QString connectionUrl;
    if(!running)state->setText(tr("停止中。開始するとAI連携もONになります。"));
    else if(base->text().trimmed().isEmpty())state->setText(tr("接続口を開始しました。必要なHTTPS到達経路を確認し、公開HTTPS URLを入力してください。"));
    else if(!server->setPublicBase(QUrl(base->text().trimmed())))state->setText(tr("公開URLは https://ホスト名 の形で入力してください（パス・クエリなし）。"));
    else{connectionUrl=server->publicUrl().toString();state->setText(tr("MCPクライアント用接続URLを生成しました（受信リクエスト %1 件）。画面を閉じても接続口は動作します。").arg(server->requestsHandled()));}
    if(endpoint->text()!=connectionUrl)endpoint->setText(connectionUrl);
    copyEndpoint->setEnabled(!connectionUrl.isEmpty());
  };
  connect(start,&QPushButton::clicked,&dialog,[&]{setAiAccess(true);if(!aiBridge_->isEnabled()){state->setText(tr("AI連携を開始できませんでした：%1").arg(aiBridge_->lastError()));return;}
    if(!aiBridge_->startChatGpt())state->setText(tr("接続を開始できませんでした：%1").arg(aiBridge_->lastError()));else update();});
  connect(stop,&QPushButton::clicked,&dialog,[&]{aiBridge_->stopChatGpt();update();});
  connect(base,&QLineEdit::textChanged,&dialog,[&]{update();});
  connect(copyTunnel,&QPushButton::clicked,&dialog,[&]{QApplication::clipboard()->setText(tunnel->text());});
  connect(copyEndpoint,&QPushButton::clicked,&dialog,[&]{QApplication::clipboard()->setText(endpoint->text());});
  auto* close=new QDialogButtonBox(QDialogButtonBox::Close,&dialog);connect(close,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);layout->addWidget(close);
  QTimer refresh;connect(&refresh,&QTimer::timeout,&dialog,[&]{update();});refresh.start(500);update();dialog.exec();
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
  viewport_->hideSnap();  // no marker or label left from the previous tool
  viewport_->clearGrips();
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
    case ToolId::Section:
      active_ = sectionTool_.get();
      if (sectionDock_) { sectionDock_->show(); sectionDock_->raise(); }
      break;
    case ToolId::FollowMe: active_ = followMeTool_.get(); break;
    case ToolId::Paint: active_ = paintTool_.get(); break;
    case ToolId::Eraser: active_ = eraserTool_.get(); break;
    case ToolId::Paste: active_ = pasteTool_.get(); break;
    case ToolId::ComponentAxes: active_ = componentAxesTool_.get(); break;
    case ToolId::Polygon: active_ = polygonTool_.get(); break;
    case ToolId::RotatedRectangle: active_ = rotatedRectangleTool_.get(); break;
    case ToolId::Scale: active_ = scaleTool_.get(); break;
    case ToolId::Wall: active_ = wallTool_.get(); break;
    case ToolId::Member: active_ = memberTool_.get(); break;
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

// What a selected face / edge index refers to: the object's shape and where
// it is. When either changes the index means something else (or nothing).
std::string MainWindow::elementBasis(const doc::ObjectId& id) const {
  const doc::Object* o = modeler_.document().find(id);
  if (!o) return {};
  const gp_Trsf t = modeler_.document().worldTransform(*o);
  std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(o->shape.TShape().get()));
  for (int r = 1; r <= 3; ++r) {
    for (int c = 1; c <= 4; ++c) key += "," + std::to_string(t.Value(r, c));
  }
  return key;
}

void MainWindow::setElementSelection(const std::vector<ElementRef>& elements) {
  takeElements(elements);
  refreshPanels();
}

void MainWindow::takeElements(const std::vector<ElementRef>& elements) {
  elements_ = elements;
  elementBases_.clear();
  // Owners become the object selection (panel, Move/Rotate targets) but are
  // not highlighted as a whole: only the picked faces/edges are.
  selection_.clear();
  for (const ElementRef& e : elements_) {
    if (elementBases_.count(e.objectId)) continue;  // owner seen already
    elementBases_[e.objectId] = elementBasis(e.objectId);
    selection_.push_back(e.objectId);
  }
  viewport_->setSelection({});
  viewport_->setHighlightedElements(elements);
}

void MainWindow::setMixedSelection(const std::vector<ElementRef>& elements, const std::vector<doc::ObjectId>& whole) {
  if (elements.empty()) {
    setSelection(whole);
    return;
  }
  takeElements(elements);
  std::unordered_set<doc::ObjectId> chosen(selection_.begin(), selection_.end());
  for (const doc::ObjectId& id : whole) {
    if (chosen.insert(id).second) selection_.push_back(id);
  }
  viewport_->setSelection(wholeSelection());
  refreshPanels();
}

// Selected objects taken whole (none of their faces / edges picked).
std::vector<doc::ObjectId> MainWindow::wholeSelection() const {
  std::unordered_set<doc::ObjectId> parts;
  for (const ElementRef& e : elements_) parts.insert(e.objectId);
  std::vector<doc::ObjectId> out;
  for (const doc::ObjectId& id : selection_) {
    if (!parts.count(id)) out.push_back(id);
  }
  return out;
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
    QString text = tr("コンポーネント編集中: %1（終了すると %2 個すべてに反映 / Esc・空白ダブルクリックで終了）")
                       .arg(QString::fromStdString(g->name))
                       .arg(count);
    // The definition is edited as stored; this placement's own settings come back afterwards.
    if (!node->editingOverrides.empty() || node->editingMirrored) {
      text += tr("　※この配置だけの寸法・鏡像は元の形で表示中、終了後も保たれます");
    }
    contextLabel_->setText(text);
    contextLabel_->setStyleSheet(QStringLiteral("QLabel { background: #ffd9b3; padding: 0 6px; }"));
  } else {
    contextLabel_->setText(tr("グループ編集中: %1（Esc・空白ダブルクリックで終了）").arg(QString::fromStdString(g->name)));
    contextLabel_->setStyleSheet(QStringLiteral("QLabel { background: #fff3c4; padding: 0 6px; }"));
  }
  contextLabel_->setToolTip(tr("選択ツールで空白をダブルクリック、または Esc で編集終了。作図中のダブルクリックは作図の確定です。"));
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
    // Selected faces / edges of objects that are gone or changed shape or
    // place (push/pull, move, reshape...) are dropped: their highlight
    // would stay drawn where the geometry was (a stale line).
    const std::size_t before = elements_.size();
    elements_.erase(std::remove_if(elements_.begin(), elements_.end(),
                                   [this](const ElementRef& e) {
                                     const auto basis = elementBases_.find(e.objectId);
                                     return modeler_.document().find(e.objectId) == nullptr ||
                                            basis == elementBases_.end() ||
                                            basis->second != elementBasis(e.objectId);
                                   }),
                    elements_.end());
    if (elements_.size() != before) {
      viewport_->syncNow();
      viewport_->setHighlightedElements(elements_);
    }
    viewport_->setSelection(wholeSelection());
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
    const auto& settings = modeler_.document().settings();
    sectionVisibleAction_->setChecked(settings.sectionDisplay.cuts);
    const QSignalBlocker planesBlock(sectionPlanesAction_), fillBlock(sectionFillAction_);
    sectionPlanesAction_->setChecked(settings.sectionDisplay.planes);
    sectionFillAction_->setChecked(settings.sectionDisplay.fill);
    if (!hadSections_ && !settings.sections.empty() && sectionDock_) sectionDock_->show();
    hadSections_ = !settings.sections.empty();
  }
}

void MainWindow::refreshPanels() {
  refreshTags();
  if (materialsPanel_) {
    materialsPanel_->refresh(modeler_.document(), paintTool_->hasMaterial()
                                                      ? std::optional<std::string>(paintTool_->material())
                                                      : std::nullopt);
  }
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
  // Faces / edges picked on their own copy just those (CLIP-01), whole
  // objects copy whole: the same targets as Delete.
  const std::vector<doc::DeleteTarget> targets = selectionTargets();
  auto clip = targets.empty() ? Result<doc::Modeler::Clipboard>::failure("Nothing selected")
                              : modeler_.copyToClipboard(targets);
  if (!clip) {
    reportError(tr("コピーする物を選択してください"));
    return false;
  }
  clipboard_ = std::move(clip.value());
  const bool partial = std::any_of(targets.begin(), targets.end(), [](const doc::DeleteTarget& t) { return !t.whole; });
  setStatus(partial ? tr("選んだ面・辺をコピーしました（Ctrl+V で貼り付け）")
                    : tr("%1 個をコピーしました（Ctrl+V で貼り付け）").arg(targets.size()));
  return true;
}

bool MainWindow::cutSelection() {
  // Copy, then the same Delete as the Delete key (CLIP-02): a face cut
  // from a box takes that face only; refused, nothing changes and the
  // clipboard keeps what it had.
  const std::vector<doc::DeleteTarget> targets = selectionTargets();
  doc::Modeler::Clipboard previous = clipboard_;
  if (!copySelection()) return false;
  const auto outcome = modeler_.deleteSelection(targets);
  if (!outcome) {
    clipboard_ = std::move(previous);
    reportError(tr("切り取れませんでした: %1").arg(userMessage(QString::fromStdString(outcome.error()))));
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

void MainWindow::startMember(const std::string& entry) {
  memberTool_->useEntry(entry);
  activateTool(ToolId::Member);
  setStatus(memberTool_->settingsText());
}

void MainWindow::showMemberSettingsDialog() {
  QDialog dialog(this);
  dialog.setObjectName(QStringLiteral("memberSettingsDialog"));
  dialog.setWindowTitle(tr("部材の設定"));
  auto* form = new QFormLayout(&dialog);
  const MemberTool::Settings current = memberTool_->settings();
  // The section: a family, then a size found by typing part of its name.
  auto* family = new QComboBox(&dialog);
  family->setObjectName(QStringLiteral("memberFamily"));
  family->addItem(tr("長方形（木・RC）"), QStringLiteral("rectangle"));
  family->addItem(tr("円形（RC丸柱など）"), QStringLiteral("circle"));
  QStringList families;
  for (const geom::CatalogSection& c : geom::sectionCatalog()) {
    if (!families.contains(QString::fromStdString(c.family))) families << QString::fromStdString(c.family);
  }
  const std::pair<QString, QString> familyNames[] = {
      {QStringLiteral("H"), tr("H形鋼")},       {QStringLiteral("I"), tr("I形鋼")},
      {QStringLiteral("BOX"), tr("角形鋼管")},  {QStringLiteral("PIPE"), tr("鋼管")},
      {QStringLiteral("ANGLE"), tr("山形鋼")},  {QStringLiteral("CHANNEL"), tr("溝形鋼")},
      {QStringLiteral("C"), tr("リップ溝形鋼")}, {QStringLiteral("CT"), tr("CT形鋼")},
      {QStringLiteral("FB"), tr("平鋼")},       {QStringLiteral("SB"), tr("角鋼")},
      {QStringLiteral("RB"), tr("丸鋼")},       {QStringLiteral("HB"), tr("六角鋼")}};
  for (const auto& [key, label] : familyNames) {
    if (families.removeAll(key)) family->addItem(label + QStringLiteral(" (") + key + QStringLiteral(")"), key);
  }
  for (const QString& rest : families) family->addItem(rest, rest);
  auto* search = new QLineEdit(&dialog);
  search->setObjectName(QStringLiteral("memberSearch"));
  search->setPlaceholderText(tr("サイズを検索（例 400x200、BCP-350）"));
  auto* sizes = new QListWidget(&dialog);
  sizes->setObjectName(QStringLiteral("memberSizes"));
  sizes->setMinimumHeight(160);
  auto length = [&](double v) { return new QLineEdit(QString::fromStdString(units::formatLength(v, 0)), &dialog); };
  const bool own = current.section.kind == geom::Section::Kind::Rectangle || current.section.kind == geom::Section::Kind::Circle;
  auto* width = length(own ? current.section.width : 105);
  auto* depth = length(current.section.kind == geom::Section::Kind::Rectangle ? current.section.depth : 105);
  auto* diameter = length(current.section.kind == geom::Section::Kind::Circle ? current.section.width : 600);
  width->setObjectName(QStringLiteral("memberWidth"));
  depth->setObjectName(QStringLiteral("memberDepth"));
  diameter->setObjectName(QStringLiteral("memberDiameter"));
  auto fill = [=] {
    const QString key = family->currentData().toString();
    const bool catalog = key != QStringLiteral("rectangle") && key != QStringLiteral("circle");
    width->setEnabled(key == QStringLiteral("rectangle"));
    depth->setEnabled(key == QStringLiteral("rectangle"));
    diameter->setEnabled(key == QStringLiteral("circle"));
    search->setEnabled(catalog);
    sizes->setEnabled(catalog);
    const QString previous = sizes->currentItem() ? sizes->currentItem()->data(Qt::UserRole).toString() : QString();
    sizes->clear();
    if (!catalog) return;
    const std::string text = search->text().trimmed().toStdString();
    for (const geom::CatalogSection& c : geom::sectionCatalog()) {
      if (QString::fromStdString(c.family) != key) continue;
      if (!text.empty() && c.name.find(text) == std::string::npos && c.id.find(text) == std::string::npos) continue;
      auto* item = new QListWidgetItem(QString::fromStdString(c.name), sizes);
      item->setData(Qt::UserRole, QString::fromStdString(c.id));
      item->setToolTip(QString::fromStdString(c.id + "  " + c.standard + " " + c.subtype));
      if (item->data(Qt::UserRole).toString() == previous) sizes->setCurrentItem(item);
    }
    if (!sizes->currentItem() && sizes->count() > 0) sizes->setCurrentRow(0);
  };
  connect(family, &QComboBox::currentIndexChanged, &dialog, fill);
  connect(search, &QLineEdit::textChanged, &dialog, fill);
  // Start where the tool is: its family and size.
  if (current.section.kind == geom::Section::Kind::Circle) {
    family->setCurrentIndex(1);
  } else if (const geom::CatalogSection* entry = geom::findSection(current.section.catalog)) {
    family->setCurrentIndex(std::max(0, family->findData(QString::fromStdString(entry->family))));
  } else {
    family->setCurrentIndex(0);
  }
  fill();
  for (int i = 0; i < sizes->count(); ++i) {
    if (sizes->item(i)->data(Qt::UserRole).toString().toStdString() == current.section.catalog) sizes->setCurrentRow(i);
  }
  auto* up = new QComboBox(&dialog);
  up->setObjectName(QStringLiteral("memberAnchorUp"));
  up->addItems({tr("下端"), tr("芯（中心）"), tr("上端")});
  up->setCurrentIndex(current.anchor.up + 1);
  auto* across = new QComboBox(&dialog);
  across->setObjectName(QStringLiteral("memberAnchorAcross"));
  across->addItems({tr("右"), tr("中心"), tr("左")});
  across->setCurrentIndex(current.anchor.across + 1);
  auto* roll = new QLineEdit(QString::number(current.roll), &dialog);
  roll->setObjectName(QStringLiteral("memberRoll"));
  auto* role = new QComboBox(&dialog);
  role->setObjectName(QStringLiteral("memberRole"));
  const std::pair<QString, QString> roles[] = {{tr("なし"), QString()}, {tr("柱"), QStringLiteral("column")},
                                               {tr("梁"), QStringLiteral("beam")}, {tr("筋かい"), QStringLiteral("brace")}};
  for (const auto& [label, key] : roles) role->addItem(label, key);
  role->setCurrentIndex(std::max(0, role->findData(QString::fromStdString(current.role))));
  auto* material = new QComboBox(&dialog);
  material->setObjectName(QStringLiteral("memberMaterialClass"));
  const std::pair<QString, QString> materials[] = {{tr("未設定"), QString()}, {tr("木"), QStringLiteral("timber")},
                                                   {tr("鋼"), QStringLiteral("steel")}, {tr("RC"), QStringLiteral("rc")}};
  for (const auto& [label, key] : materials) material->addItem(label, key);
  material->setCurrentIndex(std::max(0, material->findData(QString::fromStdString(current.materialClass))));
  auto* height = length(current.columnHeight);
  height->setObjectName(QStringLiteral("memberColumnHeight"));
  // Picking a role suggests its usual anchor (beams: top edge, others:
  // centre), shown in the boxes, unless the anchor was set in this dialog.
  auto anchorTouched = std::make_shared<bool>(false);
  connect(up, &QComboBox::activated, &dialog, [=] { *anchorTouched = true; });
  connect(across, &QComboBox::activated, &dialog, [=] { *anchorTouched = true; });
  connect(role, &QComboBox::currentIndexChanged, &dialog, [=] {
    if (*anchorTouched) return;
    up->setCurrentIndex(role->currentData().toString() == QStringLiteral("beam") ? 2 : 1);
    across->setCurrentIndex(1);
  });
  form->addRow(tr("断面の種類"), family);
  form->addRow(tr("サイズ"), search);
  form->addRow(QString(), sizes);
  form->addRow(tr("幅"), width);
  form->addRow(tr("せい"), depth);
  form->addRow(tr("直径"), diameter);
  form->addRow(tr("基準（上下）"), up);
  form->addRow(tr("基準（左右）"), across);
  form->addRow(tr("回転（度）"), roll);
  form->addRow(tr("用途"), role);
  form->addRow(tr("材料種別"), material);
  form->addRow(tr("柱の高さ"), height);
  form->addRow(new QLabel(tr("基準はクリックした線が断面のどこを通るか。左右は始点から終点を見て。\n"
                             "柱は1点目のあと Enter で「柱の高さ」の鉛直材、2点目のクリックで傾けて配置。\n"
                             "鋼材のサイズ: %1\n作った後も通常の形状として自由に編集できます")
                              .arg(QString::fromStdString(geom::sectionCatalogSource())),
                          &dialog));
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted) return;
  MemberTool::Settings next = current;
  const QString key = family->currentData().toString();
  if (key == QStringLiteral("rectangle")) {
    const auto w = units::parseLength(width->text().toStdString());
    const auto d = units::parseLength(depth->text().toStdString());
    if (!w || !d || *w <= 0 || *d <= 0) {
      reportError(tr("幅とせいは正の長さで入力してください"));
      return;
    }
    next.section = geom::Section{};
    next.section.width = *w;
    next.section.depth = *d;
  } else if (key == QStringLiteral("circle")) {
    const auto d = units::parseLength(diameter->text().toStdString());
    if (!d || *d <= 0) {
      reportError(tr("直径は正の長さで入力してください"));
      return;
    }
    next.section = geom::Section{};
    next.section.kind = geom::Section::Kind::Circle;
    next.section.width = next.section.depth = *d;
  } else {
    if (!sizes->currentItem()) {
      reportError(tr("サイズを選んでください"));
      return;
    }
    next.section = geom::findSection(sizes->currentItem()->data(Qt::UserRole).toString().toStdString())->section;
  }
  bool ok = false;
  next.roll = roll->text().toDouble(&ok);
  if (!ok) {
    reportError(tr("回転は角度（度）で入力してください"));
    return;
  }
  const auto h = units::parseLength(height->text().toStdString());
  if (!h || *h <= 0) {
    reportError(tr("柱の高さは正の長さで入力してください"));
    return;
  }
  next.columnHeight = *h;
  next.anchor = {across->currentIndex() - 1, up->currentIndex() - 1};
  next.role = role->currentData().toString().toStdString();
  next.materialClass = material->currentData().toString().toStdString();
  // A catalog (steel) section without a material class given: steel.
  if (next.materialClass.empty() && !next.section.catalog.empty()) next.materialClass = "steel";
  memberTool_->setSettings(next);
  setStatus(memberTool_->settingsText());
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

void MainWindow::showMaterials() {
  if (!materialsDock_) return;
  materialsDock_->show();
  materialsDock_->raise();
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
  // Made and set up as one undo step; nothing is left of it on a failure.
  modeler_.history().beginGroup("Add Material");
  auto id = modeler_.createMaterial(p.name, p.color);
  Status set = id ? Status::success() : Status::failure(id.error());
  if (id) {
    doc::Material m = *modeler_.document().findMaterial(*id);
    m.opacity = p.opacity;
    m.metallic = p.metallic;
    m.roughness = p.roughness;
    m.reflectance = p.reflectance;
    if (p.usePicture) {
      auto texture = presetTexture(index);
      if (texture) {
        m.texture = *texture;
      } else {
        set = Status::failure(texture.error());
      }
    }
    if (set) set = modeler_.editMaterial(m);
  }
  if (!set) {
    modeler_.history().abortGroup();
    reportError(QString::fromStdString(set.message()));
    return std::nullopt;
  }
  modeler_.history().endGroup();
  chooseMaterial(id.value());
  return id.value();
}

Result<doc::MaterialTexture> MainWindow::presetTexture(int index) {
  const auto& p = MaterialsPanel::presets()[static_cast<std::size_t>(index)];
  // A Qt resource: read through QFile (not a path the C++ library opens).
  QFile file(QString::fromUtf8(p.picture));
  if (!file.open(QIODevice::ReadOnly)) {
    return Result<doc::MaterialTexture>::failure(std::string("The picture of '") + p.name + "' is missing");
  }
  const QByteArray bytes = file.readAll();
  if (QImage::fromData(bytes).isNull()) {
    return Result<doc::MaterialTexture>::failure(std::string("The picture of '") + p.name + "' cannot be read");
  }
  // Embedded in the document (by its hash): the file opens anywhere.
  const std::string image =
      modeler_.addTextureImage(bytes.toStdString(), QFileInfo(file.fileName()).fileName().toStdString());
  return doc::MaterialTexture{image, p.width, p.height, false};
}

bool MainWindow::applyPresetPicture(const std::string& id, int index, bool replaceOwn) {
  const auto& presets = MaterialsPanel::presets();
  const doc::Material* current = modeler_.document().findMaterial(id);
  if (!current || index < 0 || index >= static_cast<int>(presets.size())) return false;
  const auto& p = presets[static_cast<std::size_t>(index)];
  if (current->texture && !replaceOwn) {
    // A picture of its own is kept unless replacing it is confirmed.
    if (QMessageBox::question(this, tr("定番の画像を適用"),
                              tr("「%1」にはすでに画像があります。定番「%2」の画像に置き換えますか？")
                                  .arg(QString::fromStdString(current->name), QString::fromUtf8(p.name)),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
      return false;
    }
  }
  auto texture = presetTexture(index);
  if (!texture) {
    reportError(QString::fromStdString(texture.error()));
    return false;
  }
  // The same material (id, name, colour, opacity, where it is used): only
  // the picture and its size are added.
  doc::Material changed = *current;
  changed.texture = *texture;
  if (!report(modeler_.editMaterial(changed))) return false;
  setStatus(tr("「%1」に定番の画像を適用しました").arg(QString::fromStdString(changed.name)));
  return true;
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

bool MainWindow::setMaterialTexture(const std::string& id, const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    reportError(tr("画像を読み込めません: %1").arg(path));
    return false;
  }
  const QByteArray bytes = file.readAll();
  const QImage picture = QImage::fromData(bytes);
  if (picture.isNull()) {
    reportError(tr("画像として読めません: %1").arg(QFileInfo(path).fileName()));
    return false;
  }
  const doc::Material* current = modeler_.document().findMaterial(id);
  if (!current) return false;
  doc::Material changed = *current;
  // Keep a size already set; a new texture starts 1 m wide, in proportion.
  const double width = current->texture ? current->texture->width : 1000.0;
  changed.texture = doc::MaterialTexture{
      modeler_.addTextureImage(bytes.toStdString(), QFileInfo(path).fileName().toStdString()), width,
      width * picture.height() / std::max(1, picture.width()), current->texture && current->texture->colorize};
  return report(modeler_.editMaterial(changed));
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

void MainWindow::showWheelZoomDialog() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("ホイールズームの設定"));
  auto* form = new QFormLayout(&dialog);
  auto* ratio = new QDoubleSpinBox(&dialog);
  ratio->setRange(1.05, 2.0);
  ratio->setSingleStep(0.05);
  ratio->setDecimals(2);
  ratio->setValue(viewport_->wheelRatioPerNotch());
  ratio->setToolTip(tr("1 ノッチで、カーソルの下の点までの距離を何分の 1 にするか（既定 %1）")
                        .arg(wheel::kDefaultRatioPerNotch));
  form->addRow(tr("1 ノッチの寄り（距離 ÷）"), ratio);
  auto* inverted = new QCheckBox(tr("ホイールの向きを逆にする"), &dialog);
  inverted->setChecked(viewport_->wheelInverted());
  form->addRow(inverted);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults,
                                       &dialog);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog, [&] {
    ratio->setValue(wheel::kDefaultRatioPerNotch);
    inverted->setChecked(false);
  });
  form->addRow(buttons);
  if (dialog.exec() != QDialog::Accepted) return;
  viewport_->setWheelZoom(ratio->value(), inverted->isChecked());
  QSettings settings;
  settings.setValue(QStringLiteral("view/wheelZoomRatio"), viewport_->wheelRatioPerNotch());
  settings.setValue(QStringLiteral("view/wheelZoomInverted"), viewport_->wheelInverted());
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
  connect(autosaveTimer_, &QTimer::timeout, this, [this] { autosaveInBackground(); });
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
  if (GuiProgress::busy()) {
    // In the middle of a long operation: save once it is over (P0-04).
    QTimer::singleShot(2000, this, [this] { autosaveNow(); });
    return false;
  }
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

void MainWindow::autosaveInBackground() {
  if (recoveryFile_.isEmpty() || autosaving_) return;
  if (GuiProgress::busy()) {
    QTimer::singleShot(2000, this, [this] { autosaveInBackground(); });
    return;
  }
  const doc::Document& document = modeler_.document();
  if (modeler_.history().isClean()) {
    clearRecovery();
    return;
  }
  if (document.sequence() == autosavedSequence_ && QFileInfo::exists(recoveryFile_)) return;
  viewport_->storeCamera(modeler_.document().settings().camera);
  if (autosaveThread_.joinable()) autosaveThread_.join();
  // A copy (objects share their shapes, which are not changed in place).
  auto snapshot = std::make_shared<const doc::Document>(document);
  const std::uint64_t sequence = document.sequence();
  const int generation = recoveryGeneration_;
  const QString target = recoveryFile_;
  const QString part = recoveryFile_ + QStringLiteral(".part");
  const QString original = currentPath_;
  autosaving_ = true;
  autosaveThread_ = std::thread([this, snapshot, sequence, generation, target, part, original] {
    // Nothing may leave the thread (that ends the program): a failure is
    // reported like any other failed autosave, the recovery file kept.
    Status s = Status::failure("unknown error");
    try {
      s = doc::saveDocument(*snapshot, utf8(part));
    } catch (const Standard_Failure& e) {
      s = Status::failure(e.GetMessageString());
    } catch (const std::exception& e) {
      s = Status::failure(e.what());
    } catch (...) {
    }
    QMetaObject::invokeMethod(
        this,
        [this, s, sequence, generation, target, part, original] {
          autosaving_ = false;
          if (generation != recoveryGeneration_ || target != recoveryFile_) {
            QFile::remove(part);  // dropped meanwhile: not to come back
            return;
          }
          if (!s) {
            QFile::remove(part);
            statusBar()->showMessage(tr("自動保存できませんでした: %1").arg(QString::fromStdString(s.message())), 8000);
            return;
          }
          QFile::remove(target);
          QFile::rename(part, target);
          QFile meta(target.chopped(4) + QStringLiteral(".json"));
          if (meta.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const nlohmann::json j = {{"original", utf8(original)},
                                      {"saved", QDateTime::currentDateTime().toString(Qt::ISODate).toStdString()}};
            meta.write(QByteArray::fromStdString(j.dump()));
          }
          autosavedSequence_ = sequence;
          statusBar()->showMessage(tr("自動保存しました（復旧用）"), 2500);
        },
        Qt::QueuedConnection);
  });
}

void MainWindow::clearRecovery() {
  ++recoveryGeneration_;  // an autosave under way is not put in place
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
  QString text = measurement_->text().trimmed();
  // Typed with a Japanese IME: full-width digits, signs and unit letters
  // and the ideographic comma read as the plain ones ("３０００，" =
  // "3000,"); only for values (a note's text stays as typed).
  QString plain = text;
  for (QChar& c : plain) {
    const char16_t u = c.unicode();
    if (u >= 0xFF01 && u <= 0xFF5E) c = QChar(u - 0xFEE0);
    if (u == 0x3000) c = QLatin1Char(' ');
    if (u == 0x3001) c = QLatin1Char(',');  // 、
    if (u == 0x2212) c = QLatin1Char('-');  // − minus sign
  }
  static const QRegularExpression value(QStringLiteral("^[0-9\\s.,;+\\-*/xXmMcCsS]*$"));
  if (plain != text && value.match(plain).hasMatch()) text = plain;
  userTyping_ = false;
  // Enter without typing finishes a wall run / slab outline (the box only
  // shows the current length then).
  if (!typed && (active_ == wallTool_.get() || active_ == slabTool_.get() || active_ == memberTool_.get())) {
    viewport_->toolCall([&] { active_->applyMeasurement(QString()); });
    viewport_->setFocus();
    return;
  }
  if (text.isEmpty() || !active_) return;
  viewport_->toolCall([&] { active_->applyMeasurement(text); });
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

std::vector<doc::DeleteTarget> MainWindow::selectionTargets() const {
  // Per object: the faces and edges picked on it, or the whole object
  // (picked as an object: group, component, triple-click, box selection).
  std::map<doc::ObjectId, doc::DeleteTarget> byObject;
  for (const ElementRef& e : elements_) {
    const TopoDS_Shape shape = viewport_->elementShape(e);
    if (shape.IsNull()) continue;
    doc::DeleteTarget& t = byObject[e.objectId];
    t.id = e.objectId;
    if (e.type == TopAbs_FACE) {
      t.faces.push_back(TopoDS::Face(shape));
    } else {
      t.edges.push_back(TopoDS::Edge(shape));
    }
  }
  std::vector<doc::DeleteTarget> targets;
  for (const doc::ObjectId& id : selection_) {
    auto it = byObject.find(id);
    if (it == byObject.end()) {
      targets.push_back(doc::DeleteTarget{id, true, {}, {}});
      continue;
    }
    // Every face and edge picked: the object as a whole.
    const auto all = viewport_->allElements(id);
    const bool complete = !all.empty() && std::all_of(all.begin(), all.end(), [this](const ElementRef& el) {
      return std::find(elements_.begin(), elements_.end(), el) != elements_.end();
    });
    if (complete) it->second = doc::DeleteTarget{id, true, {}, {}};
    targets.push_back(it->second);
  }
  return targets;
}

bool MainWindow::partOfObjectsSelected() const {
  const auto targets = selectionTargets();
  return std::any_of(targets.begin(), targets.end(), [](const doc::DeleteTarget& t) { return !t.whole; });
}

void MainWindow::deleteSelection() {
  if (selection_.empty()) return;
  // What is selected, resolved now (selectionTargets). One shared Modeler
  // call (also the API's "delete"): one undo step, all or nothing, the
  // reason when refused.
  const std::vector<doc::DeleteTarget> targets = selectionTargets();
  const auto outcome = modeler_.deleteSelection(targets);
  if (!outcome) {
    reportError(tr("削除できませんでした: %1").arg(userMessage(QString::fromStdString(outcome.error()))));
    return;
  }
  setSelection({});
  bool opened = false;
  for (const doc::DeleteTarget& t : targets) {
    const doc::Object* o = modeler_.document().find(t.id);
    opened |= o && o->kind() == doc::ObjectKind::Shell;
  }
  QString status = tr("削除しました");
  if (!outcome->lines.empty()) status += tr("（面の輪郭は線として残っています）");
  if (outcome->merged > 0) status += tr("（%1 本の線を消して面をつなげました）").arg(outcome->merged);
  if (opened) status += tr("。開いた形状はそのまま編集を続けられます（編集 > 穴をふさぐ でソリッドに戻すこともできます）");
  setStatus(status);
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
  if (partOfObjectsSelected()) {
    reportError(tr("選んだ面・辺だけをグループにすることはまだできません。オブジェクト全体を選んでください（トリプルクリック）"));
    return;
  }
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
  if (partOfObjectsSelected()) {
    reportError(tr("選んだ面・辺だけをコンポーネントにすることはまだできません。オブジェクト全体を選んでください（トリプルクリック）"));
    return;
  }
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

QMenu* MainWindow::buildContextMenu(const QPoint& pos) {
  auto* menu = new QMenu(this);
  const doc::Document& document = modeler_.document();
  // What was right-clicked, at the level being edited (a group / component
  // as a whole from outside it); selected first unless already selected.
  if (const auto element = viewport_->elementAt(pos)) {
    doc::ObjectId target = element->objectId;
    const doc::ObjectId level = modeler_.activeGroup();
    for (const doc::Object* o = document.find(target); o && !o->parent.empty() && o->parent != level;
         o = document.find(o->parent)) {
      target = o->parent;
    }
    const bool selected = std::find(selection_.begin(), selection_.end(), target) != selection_.end();
    const bool elementSelected = std::find(elements_.begin(), elements_.end(), *element) != elements_.end();
    if (!selected && !elementSelected) setSelection({target});
  }
  const std::vector<doc::ObjectId> ids = selection_;
  const doc::Object* single = ids.size() == 1 ? document.find(ids.front()) : nullptr;

  if (ids.empty()) {
    menu->addAction(tr("貼り付け"), this, &MainWindow::paste);
    menu->addAction(tr("すべて選択"), this, &MainWindow::selectAll);
  } else if (std::all_of(ids.begin(), ids.end(), [&](const doc::ObjectId& id) {
               const doc::Object* o = document.find(id);
               return o && o->locked;
             })) {
    // Locked (SketchUp): selected and shown red, only unlocked from here.
    menu->addAction(tr("ロック解除"), this, [this] { report(modeler_.setLocked(selection_, false)); });
  } else {
    menu->addAction(tr("削除"), this, &MainWindow::deleteSelection);
    menu->addAction(tr("切り取り"), this, [this] { cutSelection(); });
    menu->addAction(tr("コピー"), this, [this] { copySelection(); });
    menu->addSeparator();
    // Groups and components.
    const bool curveBoundary = std::all_of(ids.begin(), ids.end(), [&](const doc::ObjectId& id) {
      const auto* o = document.find(id); return o && o->kind() == doc::ObjectKind::Curve;
    }) && std::none_of(elements_.begin(), elements_.end(), [](const ElementRef& e) { return e.type != TopAbs_EDGE; });
    if (curveBoundary) {
      menu->addAction(tr("選択した線から面を生成"), this, [this] {
        auto face = modeler_.faceFromSelection(selectionTargets());
        if (!face) { reportError(QString::fromStdString(face.error())); return; }
        setSelection({*face});
        viewport_->syncNow();
        setStatus(tr("選択した輪郭から面を作成しました（元の線図は保持しています）"));
      });
      menu->addSeparator();
    }
    menu->addAction(tr("グループ化"), this, &MainWindow::groupSelection);
    menu->addAction(tr("コンポーネントを作成"), this, &MainWindow::makeComponent);
    // Following (docs/42): only when asked, the first one picked the host.
    if (ids.size() >= 2) {
      const doc::Object* host = document.find(ids.front());
      QAction* follow = menu->addAction(
          tr("最初に選んだ「%1」に追従させる").arg(host ? QString::fromStdString(host->name) : QString()), this, [this] {
            const std::vector<doc::ObjectId> followers(selection_.begin() + 1, selection_.end());
            if (report(modeler_.setFollow(followers, selection_.front()))) {
              setStatus(tr("%1 個が「%2」に追従します（手で動かしても追従は続きます）")
                            .arg(followers.size())
                            .arg(QString::fromStdString(modeler_.document().find(selection_.front())->name)));
            }
          });
      follow->setObjectName(QStringLiteral("followFirstAction"));
    }
    if (std::any_of(ids.begin(), ids.end(), [&](const doc::ObjectId& id) {
          const doc::Object* o = document.find(id);
          return o && o->follow;
        })) {
      QAction* stop = menu->addAction(tr("追従を止める"), this, [this] { report(modeler_.setFollow(selection_, std::nullopt)); });
      stop->setObjectName(QStringLiteral("stopFollowingAction"));
    }
    if (single && single->kind() == doc::ObjectKind::Group) {
      const doc::ObjectId id = single->id;
      menu->addAction(tr("グループを編集（中に入る）"), this, [this, id] { enterGroup(id); });
      menu->addAction(tr("グループ解除"), this, &MainWindow::ungroupSelection);
    }
    if (single && single->kind() == doc::ObjectKind::Instance) {
      const doc::ObjectId id = single->id;
      const auto& ref = std::get<doc::ComponentInstance>(single->recipe.base);
      menu->addAction(tr("コンポーネントを編集（%1 個すべてに反映）").arg(modeler_.instanceCount(ref.definitionId)),
                      this, [this, id] { enterGroup(id); });
      menu->addAction(tr("分解"), this, &MainWindow::explodeSelection);
      QMenu* placement = menu->addMenu(tr("この配置"));
      placement->addAction(tr("この配置だけ独立"), this, [this, id] { placementAction({id}, QStringLiteral("unique")); });
      placement->addAction(tr("この配置だけの寸法を戻す"), this,
                           [this, id] { placementAction({id}, QStringLiteral("reset")); });
      placement->addAction(tr("鏡像: 赤軸方向"), this, [this, id] { placementAction({id}, QStringLiteral("mirror0")); });
      placement->addAction(tr("鏡像: 緑軸方向"), this, [this, id] { placementAction({id}, QStringLiteral("mirror1")); });
      placement->addAction(tr("鏡像: 青軸方向"), this, [this, id] { placementAction({id}, QStringLiteral("mirror2")); });
      placement->addAction(tr("軸を変更（原点・赤軸）"), this, [this, id] { placementAction({id}, QStringLiteral("axes")); });
    }
    menu->addSeparator();
    // Materials of the selection as a whole (faces: the paint tool).
    QMenu* materials = menu->addMenu(tr("マテリアル"));
    for (const doc::Material& mat : document.materials()) {
      QPixmap swatch(16, 12);
      swatch.fill(QColor::fromRgbF(mat.color.r, mat.color.g, mat.color.b));
      const std::string mid = mat.id;
      materials->addAction(QIcon(swatch), QString::fromStdString(mat.name), this,
                           [this, mid] { applyMaterialToSelection(mid); });
    }
    materials->addAction(tr("（なし：継承に戻す）"), this, [this] { applyMaterialToSelection({}); });
    materials->addSeparator();
    materials->addAction(tr("マテリアルパネルを開く..."), this, [this] { showMaterials(); });
    materials->addAction(tr("新しいマテリアル..."), this, [this] { editMaterialDialog({}); });
    const bool anyEdge =
        std::any_of(elements_.begin(), elements_.end(), [](const ElementRef& el) { return el.type == TopAbs_EDGE; });
    if (anyEdge) {
      menu->addAction(tr("選んだ辺の線を隠す"), this, [this] {
        std::map<doc::ObjectId, std::vector<TopoDS_Edge>> byObject;
        for (const ElementRef& el : elementSelection()) {
          if (el.type != TopAbs_EDGE) continue;
          const TopoDS_Shape edge = viewport_->elementShape(el);
          if (!edge.IsNull()) byObject[el.objectId].push_back(TopoDS::Edge(edge));
        }
        for (const auto& [id, edges] : byObject) report(modeler_.setEdgesHidden(id, edges, true));
      });
    }
    if (single && single->kind() == doc::ObjectKind::Shell) {
      menu->addAction(tr("穴をふさぐ（ソリッドに戻す）"), this, [this] { report(modeler_.fillHoles(selection_)); });
    }
    menu->addAction(tr("非表示"), this, [this] {
      const std::vector<doc::ObjectId> hide = selection_;
      setSelection({});
      report(modeler_.hide(hide));
    });
    menu->addAction(tr("ロック"), this, [this] { report(modeler_.setLocked(selection_, true)); });
    if (std::any_of(ids.begin(), ids.end(), [&](const doc::ObjectId& id) {
          const doc::Object* o = document.find(id);
          return o && o->locked;
        })) {
      menu->addAction(tr("ロック解除"), this, [this] { report(modeler_.setLocked(selection_, false)); });
    }
  }
  menu->addSeparator();
  if (!modeler_.activeGroup().empty()) menu->addAction(tr("グループ・コンポーネントの編集を終了"), this, [this] { exitGroup(); });
  menu->addAction(tr("全体表示"), this, [this] { viewport_->fitAll(); });
  return menu;
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

bool MainWindow::confirm(const QString& title, const QString& question) {
  return QMessageBox::question(this, title, question, QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) ==
         QMessageBox::Yes;
}

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
  stopRecipeVerification();
  recipeWarnings_.clear();
  // A copy of what to check (objects as loaded, component definitions):
  // the thread never touches the document, which the user goes on editing.
  struct Job {
    std::string prefix;
    doc::Object object;
  };
  const doc::Document& document = modeler_.document();
  auto definitions = std::make_shared<const std::vector<doc::ComponentDefinition>>(document.definitions());
  std::vector<Job> jobs;
  for (const doc::ComponentDefinition& d : *definitions) {
    for (const doc::Object& o : d.objects) {
      if (!o.shape.IsNull() && !o.isContainer()) jobs.push_back({d.name + ": ", o});
    }
  }
  for (const doc::Object& o : document.objects()) {
    if (o.shape.IsNull() || o.isContainer() || std::holds_alternative<doc::ComponentInstance>(o.recipe.base)) continue;
    jobs.push_back({{}, o});
  }
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  verifyCancel_ = cancel;
  verifying_ = true;
  verifyThread_ = std::thread([this, cancel, definitions, jobs = std::move(jobs)] {
    doc::useKeptShapes(false);
    auto find = [definitions](const std::string& id) -> const doc::ComponentDefinition* {
      for (const doc::ComponentDefinition& d : *definitions) {
        if (d.id == id) return &d;
      }
      return nullptr;
    };
    doc::RegenContext plain;
    plain.definitionShape = [find](const std::string& id) -> std::optional<TopoDS_Shape> {
      const doc::ComponentDefinition* d = find(id);
      if (!d || d->shape.IsNull()) return std::nullopt;
      return d->shape;
    };
    doc::RegenContext context = plain;
    context.instanceShape = [find, plain](const doc::ComponentInstance& instance) -> Result<TopoDS_Shape> {
      const doc::ComponentDefinition* d = find(instance.definitionId);
      if (!d) return Result<TopoDS_Shape>::failure("Unknown component definition " + instance.definitionId);
      return doc::buildInstanceShape(*d, instance, plain);
    };
    QStringList warnings;
    for (const Job& job : jobs) {
      if (*cancel) return;
      // Nothing may leave the thread: a failing check is one more note.
      try {
        if (auto w = doc::checkRecipe(job.object, context)) warnings << QString::fromStdString(job.prefix + *w);
      } catch (const Standard_Failure& e) {
        warnings << QString::fromStdString(job.prefix + job.object.name + ": " + e.GetMessageString());
      } catch (const std::exception& e) {
        warnings << QString::fromStdString(job.prefix + job.object.name + ": " + e.what());
      } catch (...) {
        warnings << QString::fromStdString(job.prefix + job.object.name + ": unknown error");
      }
    }
    QMetaObject::invokeMethod(
        this,
        [this, cancel, warnings] {
          if (*cancel) return;
          verifying_ = false;
          recipeWarnings_ = warnings;
          if (!recipeWarnings_.isEmpty()) {
            statusBar()->showMessage(tr("レシピの確認で %1 件の注意（保存された形状を使っています）")
                                         .arg(recipeWarnings_.size()),
                                     15000);
            statusBar()->setToolTip(recipeWarnings_.join(QStringLiteral("\n")));
          }
        },
        Qt::QueuedConnection);
  });
}

void MainWindow::stopRecipeVerification() {
  if (verifyCancel_) *verifyCancel_ = true;
  // Stops between objects: waits for the one being checked at most.
  if (verifyThread_.joinable()) verifyThread_.join();
  verifyCancel_.reset();
  verifying_ = false;
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

void MainWindow::configureDwgConverter() {
  const auto path = QFileDialog::getOpenFileName(this, tr("ODA File Converter の実行ファイルを選択"),
                                                QSettings().value("import/odaConverter").toString());
  if (path.isEmpty()) return;
  if (!QFileInfo(path).isExecutable()) { reportError(tr("実行ファイルを選択してください。")); return; }
  QSettings().setValue("import/odaConverter", path);
  setStatus(tr("DWG 変換ソフトを設定しました"));
}

void MainWindow::importFile() {
  const auto formats=exchangeFormats(true);
  QStringList filters,patterns;
  for(const auto& format:formats) {
    filters<<format.filter;
    for(const auto& extension:format.extensions)patterns<<"*."+extension;
  }
  filters.prepend(tr("対応するすべてのファイル (%1)").arg(patterns.join(' ')));
  QFileDialog file(this,tr("インポート"),QSettings().value("files/importDirectory").toString());
  file.setObjectName("importFileDialog");file.setFileMode(QFileDialog::ExistingFile);file.setNameFilters(filters);
  if(file.exec()!=QDialog::Accepted || file.selectedFiles().isEmpty())return;
  const auto path=file.selectedFiles().front();
  QSettings().setValue("files/importDirectory",QFileInfo(path).absolutePath());
  const auto suffix=QFileInfo(path).suffix().toLower();
  const auto format=std::find_if(formats.begin(),formats.end(),[&](const auto& f){return f.extensions.contains(suffix);});
  if(format==formats.end()){reportError(tr("この形式のインポートには対応していません：%1").arg(suffix));return;}
  switch(format->type) {
    case ExchangeType::Step:importStepFile(path);break;
    case ExchangeType::Dxf:case ExchangeType::Dwg:importDrawingFile(path);break;
    case ExchangeType::Iges:importGeometryFile(path,GeometryFormat::Iges);break;
    case ExchangeType::Brep:importGeometryFile(path,GeometryFormat::Brep);break;
    case ExchangeType::Rhino:import3dmFile(path);break;
    case ExchangeType::Image:importUnderlay(path);break;
    case ExchangeType::Stl:break; // not offered by the import dialog
  }
}

void MainWindow::exportFile() {
  const auto formats=exchangeFormats(false);
  QStringList filters;for(const auto& format:formats)filters<<format.filter;
  QFileDialog file(this,tr("エクスポート"),QSettings().value("files/exportDirectory").toString());
  file.setObjectName("exportFileDialog");file.setAcceptMode(QFileDialog::AcceptSave);
  file.setFileMode(QFileDialog::AnyFile);file.setNameFilters(filters);
  // Confirm the final, normalized filename ourselves, including when the
  // chosen format changes an extension typed in the filename field.
  file.setOption(QFileDialog::DontConfirmOverwrite,true);
  auto updateSuffix=[&](const QString& filter){for(const auto& format:formats)if(format.filter==filter)file.setDefaultSuffix(format.extensions.front());};
  const auto last=QSettings().value("files/exportFormat",QStringLiteral("step")).toString();
  for(const auto& format:formats)if(format.extensions.front()==last)file.selectNameFilter(format.filter);
  updateSuffix(file.selectedNameFilter());
  connect(&file,&QFileDialog::filterSelected,&file,updateSuffix);
  if(file.exec()!=QDialog::Accepted || file.selectedFiles().isEmpty())return;
  const auto format=std::find_if(formats.begin(),formats.end(),[&](const auto& f){return f.filter==file.selectedNameFilter();});
  if(format==formats.end())return;
  QString path=file.selectedFiles().front();const auto suffix=QFileInfo(path).suffix().toLower();
  if(!format->extensions.contains(suffix)) {
    const bool known=std::any_of(formats.begin(),formats.end(),[&](const auto& f){return f.extensions.contains(suffix);});
    if(known)path.chop(suffix.size()+1);
    path+='.'+format->extensions.front();
  }
  if(QFileInfo::exists(path) && QMessageBox::question(this,tr("上書きの確認"),
      tr("このファイルは既に存在します。上書きしますか？\n%1").arg(path),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;
  QSettings().setValue("files/exportDirectory",QFileInfo(path).absolutePath());
  QSettings().setValue("files/exportFormat",format->extensions.front());
  switch(format->type) {
    case ExchangeType::Step:exportStepFile(path);break;
    case ExchangeType::Stl:exportStl(path);break;
    case ExchangeType::Iges:exportGeometry(path,GeometryFormat::Iges);break;
    case ExchangeType::Brep:exportGeometry(path,GeometryFormat::Brep);break;
    case ExchangeType::Rhino:export3dmFile(path);break;
    case ExchangeType::Image:exportImage(path);break;
    case ExchangeType::Dxf:case ExchangeType::Dwg:break; // not offered for export
  }
}

void MainWindow::importDrawingFile(const QString& path) {
  if (path.endsWith(".dwg", Qt::CaseInsensitive)) {
    QString converter = QSettings().value("import/odaConverter").toString();
    if (converter.isEmpty()) {
      QMessageBox::information(this, tr("DWG の取り込み"),
          tr("DWG の取り込みには ODA File Converter の別途インストールが必要です。\n"
             "インストール済みの場合は、次の画面で実行ファイルを選択してください。\n"
             "公式配布元: https://www.opendesign.com/guestfiles/oda_file_converter"));
      configureDwgConverter();
      converter = QSettings().value("import/odaConverter").toString();
      if (converter.isEmpty()) return;
    }
    QTemporaryDir output;
    if (!output.isValid()) { reportError(tr("一時フォルダーを作成できません。")); return; }
    auto converted = convertDwgToDxf(path, converter, output.path(), this);
    if (!converted) { reportError(QString::fromStdString(converted.error())); return; }
    importDxfFile(*converted);
  } else importDxfFile(path);
}

bool MainWindow::importDxfFile(const QString& path) {
  geom::DxfImportOptions options;
  geom::DxfImportReport report;
  QProgressDialog progress(tr("DXF の線図を読み取り中…"), QString(), 0, 0, this);
  progress.setCancelButton(nullptr);
  progress.setWindowModality(Qt::ApplicationModal);
  progress.setMinimumDuration(0);
  progress.show();
  QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  // Inspect before committing, so neither cancelling units nor partial import adds history.
  {
    auto inspected = geom::readDxf(utf8(path), options, &report);
    if (!inspected) { reportError(QString::fromStdString(inspected.error())); return false; }
  }
  progress.hide();
  if (!report.unitsKnown) {
    bool ok = false;
    options.unitlessScaleToMm = QInputDialog::getDouble(this, tr("図面の単位"),
        tr("図面に単位の指定がありません。1 図面単位は何 mm ですか？\n"
           "mm: 1 / cm: 10 / m: 1000 / inch: 25.4"), 1, 0.000001, 1000000, 6, &ok);
    if (!ok) return false;
  }
  if (report.incomplete() || report.imported > 1000) {
    QStringList omitted;
    for (const auto& [type, count] : report.omitted)
      omitted << tr("%1: %2 個").arg(QString::fromStdString(type)).arg(count);
    QMessageBox confirm(QMessageBox::Question, tr("2D図面の取り込み"),
        tr("読み込める線図: %1 本\n%2\n取り込みますか？")
            .arg(report.imported).arg(omitted.empty() ? QString() :
                tr("次の要素は読み込めません。\n%1\n").arg(omitted.join('\n'))),
        QMessageBox::Yes | QMessageBox::No, this);
    confirm.setDefaultButton(QMessageBox::No);
    auto* grouped = new QCheckBox(tr("レイヤごとにまとめる（大きな図面向け・線の形状は保持）"), &confirm);
    grouped->setChecked(report.imported > 1000);
    grouped->setToolTip(tr("まとめるとレイヤ単位で選択・移動します。個別の線図として扱う場合はチェックを外してください。"));
    confirm.setCheckBox(grouped);
    if (confirm.exec() != QMessageBox::Yes)
      return false;
    options.groupByLayer = grouped->isChecked();
    options.allowPartial = true;
  }
  progress.setLabelText(tr("%1 本の線図を登録して表示を準備中…").arg(report.imported));
  progress.show();
  QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  auto ids = modeler_.importDxf(utf8(path), options, &report);
  if (!ids) { reportError(QString::fromStdString(ids.error())); return false; }
  setSelection(*ids);
  viewport_->syncNow();
  viewport_->setStandardView(Viewport::StandardView::Top);
  viewport_->fitAll();
  progress.hide();
  setStatus(tr("取り込み完了: %1 本の線図 → %2 オブジェクト（単位 mm、レイヤはタグ）。%3")
      .arg(report.imported).arg(ids->size()).arg(report.incomplete() ? tr("未対応の要素は除外しました。") : QString()) +
      (report.paperSpace ? tr(" ペーパー空間の %1 個は対象外です。").arg(report.paperSpace) : QString()));
  return true;
}

void MainWindow::importStepFile(const QString& path) {
  auto ids = modeler_.importStep(utf8(path));
  if (!ids) {
    reportError(QString::fromStdString(ids.error()));
    return;
  }
  setSelection(ids.value());
  viewport_->syncNow();
  viewport_->fitAll();
}

void MainWindow::exportStepFile(const QString& path) {
  const Status s = modeler_.exportStep(utf8(path), selection_);
  if (!s) {
    reportError(QString::fromStdString(s.message()));
  } else {
    statusBar()->showMessage(tr("STEP を書き出しました: %1").arg(path), 4000);
  }
}

void MainWindow::exportStl(const QString& path) {
  QDialog dialog(this); dialog.setWindowTitle(tr("STL エクスポート"));
  auto* form = new QFormLayout(&dialog);
  form->addRow(new QLabel(selection_.empty() ? tr("出力範囲：表示中のモデル全体") :
      tr("出力範囲：選択したオブジェクト %1 個").arg(selection_.size()), &dialog));
  auto* tolerance = new QDoubleSpinBox(&dialog);
  tolerance->setDecimals(3); tolerance->setRange(0.001,1000); tolerance->setValue(0.1);
  tolerance->setSuffix(tr(" mm")); tolerance->setSingleStep(0.05);
  form->addRow(tr("曲面の近似精度（小さいほど細かい）"), tolerance);
  auto* info = new QLabel(tr("バイナリSTLで書き出します。単位はmmとして読み込んでください。\n"
      "線・色・テクスチャは含みません。面だけの形状には厚みがありません。"), &dialog);
  info->setWordWrap(true); form->addRow(info);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  form->addRow(buttons);
  if (dialog.exec()!=QDialog::Accepted) return;
  exportStlFile(path,tolerance->value());
}

bool MainWindow::exportStlFile(const QString& path, double linearDeflection) {
  geom::StlExportOptions options; options.linearDeflection=linearDeflection;
  const auto result=modeler_.exportStl(utf8(path),selection_,options);
  if (!result) { reportError(QString::fromStdString(result.message())); return false; }
  setStatus(tr("STLを書き出しました（mm）：%1").arg(path));
  return true;
}

bool MainWindow::importGeometryFile(const QString& path,GeometryFormat format) {
  const bool iges=format==GeometryFormat::Iges;
  auto ids=iges?modeler_.importIges(utf8(path)):modeler_.importBrep(utf8(path));
  if(!ids){reportError(QString::fromStdString(ids.error()));return false;}
  setSelection(*ids);viewport_->syncNow();viewport_->fitAll();
  setStatus(tr("%1から%2個のオブジェクトを読み込みました（形状のみ、mm）。")
      .arg(iges?QStringLiteral("IGES"):QStringLiteral("BREP")).arg(ids->size()));
  return true;
}

void MainWindow::exportGeometry(const QString& path, GeometryFormat format) {
  const bool iges=format==GeometryFormat::Iges;
  QDialog settings(this);settings.setWindowTitle(iges?tr("IGES エクスポート"):tr("BREP エクスポート"));
  auto* form=new QFormLayout(&settings);
  form->addRow(new QLabel(selection_.empty()?tr("出力範囲：表示中のモデル全体"):
      tr("出力範囲：選択したオブジェクト %1 個").arg(selection_.size()),&settings));
  form->addRow(new QLabel(tr("正確な曲線・面・立体をmm基準で書き出します。\n"
      "形状のみの出力です。編集履歴・名前・タグ・素材はSBR形式で保存してください。"),&settings));
  auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&settings);
  connect(buttons,&QDialogButtonBox::accepted,&settings,&QDialog::accept);
  connect(buttons,&QDialogButtonBox::rejected,&settings,&QDialog::reject);form->addRow(buttons);
  if(settings.exec()!=QDialog::Accepted)return;
  exportGeometryFile(path,format);
}

bool MainWindow::exportGeometryFile(const QString& path,GeometryFormat format) {
  const bool iges=format==GeometryFormat::Iges;
  const auto status=iges?modeler_.exportIges(utf8(path),selection_):modeler_.exportBrep(utf8(path),selection_);
  if(!status){reportError(QString::fromStdString(status.message()));return false;}
  setStatus(tr("%1を書き出しました（形状のみ、mm）：%2").arg(iges?QStringLiteral("IGES"):QStringLiteral("BREP")).arg(path));
  return true;
}

QString MainWindow::describe3dmReport(const geom::Import3dmReport& r) {
  static const std::map<std::string, QString> kinds = {
      {"mesh", tr("メッシュ")}, {"subd", tr("SubD")}, {"point", tr("点")}, {"curve", tr("曲線")},
      {"annotation", tr("注釈")}, {"block", tr("ブロック")}, {"surface", tr("サーフェス")}, {"other", tr("その他")}};
  QStringList lines;
  lines << tr("オブジェクト %1 個のうち、読み込めるのは %2 個").arg(r.objects).arg(r.imported);
  if (r.partial > 0) {
    lines << tr("・%1 個は一部が欠けます（面 %2 枚・トリム %3 本を変換できず）")
                 .arg(r.partial).arg(r.skippedFaces).arg(r.skippedTrims);
  }
  if (r.failed > 0) lines << tr("・%1 個は変換に失敗しました").arg(r.failed);
  if (r.unsupportedCount() > 0) {
    QStringList parts;
    for (const auto& [kind, count] : r.unsupported) {
      const auto k = kinds.find(kind);
      parts << tr("%1 %2 個").arg(k != kinds.end() ? k->second : QString::fromStdString(kind)).arg(count);
    }
    lines << tr("・未対応の種類（読み込みません）：%1").arg(parts.join(tr("、")));
  }
  return lines.join(QLatin1Char('\n'));
}

void MainWindow::import3dmFile(const QString& path) {
  geom::Import3dmReport report;
  auto ids = modeler_.import3dm(utf8(path), false, &report);
  if (!ids && report.incomplete() && report.imported > 0) {
    // Not everything came through: say what, and let the user decide.
    const auto answer = QMessageBox::question(
        this, tr("Rhino 3DM インポート"),
        tr("このファイルは一部を読み込めませんでした。\n\n%1\n\n読めた分だけ読み込みますか？")
            .arg(describe3dmReport(report)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
      setStatus(tr("3DM の読み込みを取りやめました"));
      return;
    }
    ids = modeler_.import3dm(utf8(path), true, &report);
  }
  if (!ids) {
    QString message = QString::fromStdString(ids.error());
    if (report.objects > 0) message = describe3dmReport(report) + QLatin1Char('\n') + message;
    reportError(message);
    return;
  }
  setSelection(ids.value());
  viewport_->syncNow();
  viewport_->fitAll();
  if (report.incomplete()) {
    setStatus(tr("3DM から %1 個を読み込みました（一部欠落あり：%2 個は読み込めず）")
                  .arg(ids->size()).arg(report.objects - report.imported));
  } else {
    setStatus(tr("3DM から %1 個を読み込みました（単位は mm に換算、レイヤはタグに）").arg(ids->size()));
  }
}

void MainWindow::export3dmFile(const QString& path) {
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
