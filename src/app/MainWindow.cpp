#include "MainWindow.h"

#include <algorithm>
#include <map>

#include <QAction>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDockWidget>
#include <QFileDialog>
#include <QDialog>
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
#include <TopoDS_Compound.hxx>
#include <nlohmann/json.hpp>

#include "AiBridge.h"
#include "Messages.h"
#include "ProgressDialog.h"
#include "Cursors.h"
#include "PropertiesPanel.h"
#include "Viewport.h"
#include "sbr/Units.h"

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
}

MainWindow::~MainWindow() {
  geom::setProgressIndicator(nullptr);
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
  connect(measurement_, &QLineEdit::returnPressed, this, &MainWindow::commitMeasurement);
  connect(measurement_, &QLineEdit::textEdited, this, [this] { userTyping_ = true; });
  connect(viewport_, &Viewport::escapePressed, this, [this] {
    userTyping_ = false;
    if (active_ == selectTool_.get() && !modeler_.activeGroup().empty()) {
      exitGroup();
      return;
    }
    if (active_ && active_->stepBack()) return;  // one stage back first
    if (active_ == pasteTool_.get()) {  // nothing pasted: back to selecting
      activateTool(ToolId::Select);
      return;
    }
    if (active_) {
      active_->cancel();
      active_->activate();
    }
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
      // Choosing the paint tool (again) opens the colour picker first.
      if (id == ToolId::Paint) {
        const doc::Color c = paintTool_->color();
        const QColor chosen = QColorDialog::getColor(QColor::fromRgbF(c.r, c.g, c.b), this, tr("ペイント色"));
        if (chosen.isValid()) paintTool_->setColor({chosen.redF(), chosen.greenF(), chosen.blueF()});
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
  if (active_) active_->deactivate();
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
    case ToolId::Polygon: active_ = polygonTool_.get(); break;
    case ToolId::Scale: active_ = scaleTool_.get(); break;
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
  viewport_->setFocus();
}

// --- ToolHost ---------------------------------------------------------------------

void MainWindow::setStatus(const QString& text) { status_->setText(text); }

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
    contextLabel_->setText(tr("コンポーネント編集中: %1（終了すると %2 個すべてに反映 / Esc で終了）")
                               .arg(QString::fromStdString(g->name))
                               .arg(count));
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

void MainWindow::commitMeasurement() {
  const QString text = measurement_->text().trimmed();
  userTyping_ = false;
  if (text.isEmpty() || !active_) return;
  active_->applyMeasurement(text);
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
        reportError(tr("ソリッドの面・エッジ単体は削除できません。トリプルクリックでオブジェクト全体を選んでから削除してください"));
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
  setSelection({});
  viewport_->setStandardView(Viewport::StandardView::Iso);
  refreshScenes();
  updateTitle();
}

bool MainWindow::openFile(const QString& path) {
  if (active_) active_->cancel();
  exitGroup();
  std::vector<std::string> warnings;
  const Status s = modeler_.load(utf8(path), &warnings);
  if (!s) {
    reportError(tr("開けませんでした: %1").arg(QString::fromStdString(s.message())));
    return false;
  }
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

bool MainWindow::saveFile(const QString& path) {
  viewport_->storeCamera(modeler_.document().settings().camera);
  const Status s = modeler_.save(utf8(path));
  if (!s) {
    reportError(tr("保存できませんでした: %1").arg(QString::fromStdString(s.message())));
    QMessageBox::critical(this, tr("保存エラー"), QString::fromStdString(s.message()));
    return false;
  }
  currentPath_ = path;
  updateTitle();
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
