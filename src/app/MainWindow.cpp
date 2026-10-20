#include "MainWindow.h"

#include <algorithm>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>

#include "Cursors.h"
#include "PropertiesPanel.h"
#include "Viewport.h"

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

  buildUi();
  buildMenus();
  modeler_.document().addListener([this] { onDocumentChanged(); });
  activateTool(ToolId::Rectangle);
  refreshPanels();
  updateTitle();
}

MainWindow::~MainWindow() {
  // Tools reference the viewport; drop them before child widgets go away.
  if (active_) active_->deactivate();
  viewport_->setTool(nullptr);
}

void MainWindow::buildUi() {
  viewport_ = new Viewport(modeler_, this);
  setCentralWidget(viewport_);

  panel_ = new PropertiesPanel;
  auto* dock = new QDockWidget(tr("オブジェクト / 属性"), this);
  dock->setObjectName(QStringLiteral("propertiesDock"));
  dock->setWidget(panel_);
  dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
  addDockWidget(Qt::RightDockWidgetArea, dock);
  resizeDocks({dock}, {360}, Qt::Horizontal);

  status_ = new QLabel;
  status_->setMinimumWidth(200);
  measurementLabel_ = new QLabel;
  measurement_ = new QLineEdit;
  measurement_->setFixedWidth(200);
  measurement_->setPlaceholderText(tr("数値入力"));
  statusBar()->addWidget(status_, 1);
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
  file->addSeparator();
  file->addAction(tr("終了(&X)"), QKeySequence::Quit, this, &QWidget::close);

  // --- Edit
  QMenu* edit = menuBar()->addMenu(tr("編集(&E)"));
  undoAction_ = edit->addAction(tr("元に戻す"), QKeySequence::Undo, this, &MainWindow::undo);
  redoAction_ = edit->addAction(tr("やり直し"), this, &MainWindow::redo);
  redoAction_->setShortcuts({QKeySequence(tr("Ctrl+Y")), QKeySequence(tr("Ctrl+Shift+Z"))});
  edit->addSeparator();
  edit->addAction(tr("削除"), QKeySequence::Delete, this, &MainWindow::deleteSelection);
  edit->addAction(tr("すべて選択"), QKeySequence::SelectAll, this, &MainWindow::selectAll);
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
      {tr("プッシュ/プル"), QStringLiteral("P"), ToolId::PushPull, CursorKind::PushPull},
      {tr("移動"), QStringLiteral("M"), ToolId::Move, CursorKind::Move},
      {tr("回転"), QStringLiteral("Q"), ToolId::Rotate, CursorKind::Rotate},
      {tr("オフセット"), QStringLiteral("F"), ToolId::Offset, CursorKind::Offset},
      {tr("曲線"), QStringLiteral("C"), ToolId::Curve, CursorKind::Curve},
      {tr("回転体"), QStringLiteral("V"), ToolId::Revolve, CursorKind::Revolve},
      {tr("メジャー"), QStringLiteral("T"), ToolId::Measure, CursorKind::Measure},
  };
  for (const ToolEntry& e : entries) {
    QAction* a = new QAction(toolIcon(e.icon), e.label, this);
    a->setCheckable(true);
    a->setShortcut(QKeySequence(e.key));
    a->setToolTip(QStringLiteral("%1 (%2)").arg(e.label, e.key));
    const ToolId id = e.id;
    connect(a, &QAction::triggered, this, [this, id] { activateTool(id); });
    toolGroup_->addAction(a);
    tools->addAction(a);
    bar->addAction(a);
    toolActions_[static_cast<int>(id)] = a;
  }

  QMenu* help = menuBar()->addMenu(tr("ヘルプ(&H)"));
  help->addAction(tr("操作方法"), this, [this] {
    QMessageBox::information(
        this, tr("操作方法"),
        tr("<b>カメラ</b><br>ホイール: ズーム / ホイールドラッグ: オービット / "
           "Shift+ホイールドラッグ: パン / ダブルクリック: フォーカス<br>"
           "F2〜F7: 標準ビュー / F8: 透視・平行投影 / Shift+Z: 全体表示<br><br>"
           "<b>ツール</b><br>Space: 選択 / L: 線 / R: 矩形 / P: プッシュ/プル<br>"
           "M: 移動 / Q: 回転 / F: オフセット（移動・回転中に Ctrl でコピー、確定後 x3 で配列・/3 で等分割）<br>"
           "作図中に数値を打って Enter で寸法指定（例: 3000, 4.5m, 3000,4500）<br>"
           "Shift: 推論方向をロック / →・←・↑: X・Y・Z 軸ロック / ↓: 解除 / Esc: 中止"));
  });
}

void MainWindow::activateTool(ToolId id) {
  if (active_) active_->deactivate();
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
  }
  toolActions_[static_cast<int>(id)]->setChecked(true);
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

void MainWindow::reportError(const QString& text) {
  lastError_ = text;
  statusBar()->showMessage(text, 6000);
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

QString MainWindow::elementSummary() const {
  if (elements_.empty()) return {};
  int faces = 0, edges = 0;
  for (const ElementRef& e : elements_) (e.type == TopAbs_FACE ? faces : edges)++;
  return tr("面 %1 / エッジ %2").arg(faces).arg(edges);
}

// --- document ---------------------------------------------------------------------

void MainWindow::onDocumentChanged() {
  viewport_->scheduleSync();
  if (refreshPending_) return;
  refreshPending_ = true;
  QTimer::singleShot(0, this, [this] {
    refreshPending_ = false;
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

void MainWindow::refreshPanels() {
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
  if (active_) active_->activate();
}

void MainWindow::redo() {
  if (active_) active_->cancel();
  modeler_.redo();
  if (active_) active_->activate();
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
      if (!whole) {
        reportError(tr("面・エッジ単体の削除はまだできません。トリプルクリックでオブジェクト全体を選んでから削除してください"));
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
  modeler_.newDocument();
  currentPath_.clear();
  setSelection({});
  viewport_->setStandardView(Viewport::StandardView::Iso);
  updateTitle();
}

bool MainWindow::openFile(const QString& path) {
  if (active_) active_->cancel();
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

void MainWindow::closeEvent(QCloseEvent* event) {
  if (maybeSave()) {
    event->accept();
  } else {
    event->ignore();
  }
}

}  // namespace sbr::app
