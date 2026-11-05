#include "StructureDiagnosticsPanel.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <QCheckBox>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>

#include "MainWindow.h"
#include "Viewport.h"
#include "sbr/ConstructionDiagnostics.h"

namespace sbr::app {
namespace {
using json = nlohmann::json;
QString text(const std::string& s) { return QString::fromStdString(s); }
QString verdict(const std::string& s) {
  if (s == "pass") return QStringLiteral("足りている（壁量の比較のみ）");
  if (s == "fail") return QStringLiteral("不足");
  if (s == "missing") return QStringLiteral("支えなし");
  if (s == "gap") return QStringLiteral("隙間");
  if (s == "interference") return QStringLiteral("食い込み");
  if (s == "partial") return QStringLiteral("一部だけ");
  if (s == "ambiguous") return QStringLiteral("候補が複数");
  return QStringLiteral("未評価");
}
// The edges of a box only: a marker outline, not a shaded block over the model.
TopoDS_Shape outline(const gp_Pnt& lo, const gp_Pnt& hi) {
  BRep_Builder builder; TopoDS_Compound edges; builder.MakeCompound(edges);
  for (TopExp_Explorer e(BRepPrimAPI_MakeBox(lo, hi).Shape(), TopAbs_EDGE); e.More(); e.Next()) builder.Add(edges, e.Current());
  return edges;
}
Quantity_Color rgb(double r, double g, double b) { return Quantity_Color(r, g, b, Quantity_TOC_RGB); }
Quantity_Color tone(const std::string& status) {
  if (status == "pass") return rgb(.16, .66, .32);
  if (status == "fail" || status == "missing" || status == "interference") return rgb(.90, .20, .16);
  return rgb(.96, .64, .10);
}
}  // namespace

StructureDiagnosticsPanel::StructureDiagnosticsPanel(MainWindow* window)
    : PanelDock(tr("構造の診断"), window), window_(window) {
  setObjectName(QStringLiteral("structureDiagnosticsPanel"));
  setAllowedAreas(Qt::NoDockWidgetArea);
  auto* content = new QWidget(this);
  auto* layout = new QVBoxLayout(content);
  auto* notice = new QLabel(tr("今ある計算（壁量・架構チェック・基礎までの支持先）の結果を、モデルを変えずに画面へ重ねます。\n"
                               "「足りている」は壁量の比較だけの結果で、建物全体の安全・法令適合の判定ではありません。"),
                            content);
  notice->setWordWrap(true);
  layout->addWidget(notice);
  status_ = new QLabel(content);
  status_->setObjectName(QStringLiteral("structureStatus"));
  status_->setWordWrap(true);
  layout->addWidget(status_);
  overlay_ = new QCheckBox(tr("画面に重ねて表示（緑：足りている／赤：不足・支えなし／橙：未評価・要確認）"), content);
  overlay_->setObjectName(QStringLiteral("structureOverlay"));
  overlay_->setChecked(true);
  layout->addWidget(overlay_);
  layout->addWidget(new QLabel(tr("壁量（階・方向ごと、存在壁量／必要壁量）"), content));
  walls_ = new QTableWidget(content);
  walls_->setObjectName(QStringLiteral("structureWallTable"));
  walls_->setColumnCount(4);
  walls_->setHorizontalHeaderLabels({tr("階"), tr("方向"), tr("存在／必要"), tr("判定")});
  walls_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  walls_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
  walls_->setMaximumHeight(170);
  layout->addWidget(walls_);
  wallNotice_ = new QLabel(content);
  wallNotice_->setObjectName(QStringLiteral("structureWallNotice"));
  wallNotice_->setWordWrap(true);
  layout->addWidget(wallNotice_);
  layout->addWidget(new QLabel(tr("印（ダブルクリックで対象を選択）"), content));
  markers_ = new QTableWidget(content);
  markers_->setObjectName(QStringLiteral("structureMarkers"));
  markers_->setColumnCount(4);
  markers_->setHorizontalHeaderLabels({tr("判定"), tr("元の確認"), tr("ルール・役割"), tr("内容")});
  markers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  markers_->setSelectionBehavior(QAbstractItemView::SelectRows);
  markers_->horizontalHeader()->setStretchLastSection(true);
  layout->addWidget(markers_, 1);
  unassessed_ = new QLabel(content);
  unassessed_->setObjectName(QStringLiteral("structureUnassessed"));
  unassessed_->setWordWrap(true);
  layout->addWidget(unassessed_);
  auto* update = new QPushButton(tr("モデルから更新"), content);
  update->setObjectName(QStringLiteral("structureRefresh"));
  layout->addWidget(update);
  connect(update, &QPushButton::clicked, this, &StructureDiagnosticsPanel::refresh);
  connect(overlay_, &QCheckBox::toggled, this, [this] { drawOverlay(); });
  connect(markers_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
    std::vector<doc::ObjectId> targets;
    for (const auto& id : markers_->item(row, 0)->data(Qt::UserRole).toStringList())
      if (window_->modeler().document().find(id.toStdString())) targets.push_back(id.toStdString());
    if (targets.empty()) return;
    window_->setSelection(targets);
    window_->viewport().fitObjects(targets);
  });
  connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
    if (!visible) window_->viewport().setDiagnosticShapes({});
    else drawOverlay();
  });
  setWidget(content);
  setFloating(true);
  resize(760, 640);
}

StructureDiagnosticsPanel::~StructureDiagnosticsPanel() { window_->viewport().setDiagnosticShapes({}); }

void StructureDiagnosticsPanel::refresh() {
  const auto r = construction::structuralDiagnostics(window_->modeler().document());
  if (!r) {
    result_.clear();
    status_->setText(tr("診断できません: %1").arg(text(r.error())));
    populate();
    return;
  }
  result_ = *r;
  sequence_ = window_->modeler().document().sequence();
  populate();
}

void StructureDiagnosticsPanel::invalidate() {
  if (result_.empty() || sequence_ == window_->modeler().document().sequence()) return;
  // An edit makes the drawing stale: never leave old marks over a changed model.
  result_.clear();
  status_->setText(tr("モデルが変わりました。「モデルから更新」で診断し直してください。"));
  populate();
}

void StructureDiagnosticsPanel::populate() {
  walls_->setRowCount(0);
  markers_->setRowCount(0);
  wallNotice_->clear();
  unassessed_->clear();
  if (result_.empty()) { drawOverlay(); return; }
  const auto r = json::parse(result_);
  const auto& wall = r.at("wall_quantity");
  if (!wall.value("entered", false)) {
    wallNotice_->setText(tr("壁量の入力がありません（「木造の壁量...」で耐力壁・荷重を入力すると表示します）。"));
  } else {
    for (const auto& row : wall.value("rows", json::array())) {
      const int i = walls_->rowCount();
      walls_->insertRow(i);
      walls_->setItem(i, 0, new QTableWidgetItem(text(row.value("floor", std::string()))));
      walls_->setItem(i, 1, new QTableWidgetItem(text(row.value("direction", std::string()))));
      auto* bar = new QProgressBar(walls_);
      const std::string status = row.value("status", std::string("not_evaluated"));
      if (row["existing_m"].is_number() && row["required_m"].is_number() && row["required_m"].get<double>() > 0) {
        const double existing = row["existing_m"].get<double>(), required = row["required_m"].get<double>();
        bar->setRange(0, 1000);
        bar->setValue(int(std::min(1.0, existing / required) * 1000));
        bar->setFormat(tr("%1 m ／ %2 m（%3%）").arg(existing, 0, 'f', 2).arg(required, 0, 'f', 2).arg(existing / required * 100, 0, 'f', 0));
      } else {
        bar->setRange(0, 1);
        bar->setValue(0);
        bar->setFormat(tr("未評価（入力不足）"));
      }
      const auto c = tone(status);
      bar->setStyleSheet(QStringLiteral("QProgressBar::chunk{background:rgb(%1,%2,%3);}")
                             .arg(int(c.Red() * 255)).arg(int(c.Green() * 255)).arg(int(c.Blue() * 255)));
      walls_->setCellWidget(i, 2, bar);
      auto* judged = new QTableWidgetItem(verdict(status));
      QStringList reasons;
      for (const auto& reason : row.value("reasons", json::array())) reasons << text(reason.get<std::string>());
      judged->setToolTip(reasons.join(QStringLiteral("\n")));
      walls_->setItem(i, 3, judged);
    }
    QString note = text(wall.value("notice", std::string()));
    if (wall.value("stale_table", false)) note += tr("\n係数表の確認から1年以上たっています。最新の基準を確認してください。");
    if (wall.contains("display_note")) note += QStringLiteral("\n") + text(wall["display_note"].get<std::string>());
    wallNotice_->setText(note);
  }
  for (const auto& m : r.at("markers")) {
    const int i = markers_->rowCount();
    markers_->insertRow(i);
    const bool frameRow = m["source"] == "frame_check";
    const std::string status = m.value("status", std::string());
    auto* first = new QTableWidgetItem(frameRow && status == "fail" ? tr("不適合") : verdict(status));
    QStringList ids;
    for (const auto& id : m.value("targets", json::array())) ids << text(id.get<std::string>());
    first->setData(Qt::UserRole, ids);
    markers_->setItem(i, 0, first);
    markers_->setItem(i, 1, new QTableWidgetItem(frameRow ? tr("架構チェック") : tr("基礎までの支持先")));
    markers_->setItem(i, 2, new QTableWidgetItem(text(m.value("rule", std::string()))));
    markers_->setItem(i, 3, new QTableWidgetItem(text(m.value("message", std::string()))));
  }
  QStringList open;
  for (const auto& item : r.at("unassessed")) open << text(item.get<std::string>());
  unassessed_->setText(tr("まだ計算しない項目（未評価・合格扱いにしません）：%1").arg(open.join(QStringLiteral("、"))));
  const auto& counts = r.at("counts");
  status_->setText(tr("壁 %1 本／架構チェック 不適合 %2・未評価 %3／支持先の要確認 %4")
                       .arg(counts.value("walls", 0)).arg(counts.value("frame_fail", 0))
                       .arg(counts.value("frame_not_evaluated", 0)).arg(counts.value("support_issues", 0)));
  drawOverlay();
}

void StructureDiagnosticsPanel::drawOverlay() {
  std::vector<std::pair<TopoDS_Shape, Quantity_Color>> shapes;
  if (!result_.empty() && overlay_->isChecked() && isVisible()) {
    const auto r = json::parse(result_);
    for (const auto& w : r.at("walls")) {
      const gp_Pnt a(w["start"][0].get<double>(), w["start"][1].get<double>(), w["start"][2].get<double>() + 5);
      const gp_Pnt b(w["end"][0].get<double>(), w["end"][1].get<double>(), w["end"][2].get<double>() + 5);
      if (a.Distance(b) < 1) continue;
      const std::string status = w.value("wall_status", std::string()) == "evaluated" ? w.value("row_status", std::string()) : "not_evaluated";
      shapes.push_back({BRepBuilderAPI_MakeEdge(a, b).Edge(), tone(status)});
    }
    const auto& d = window_->modeler().document();
    for (const auto& m : r.at("markers")) {
      for (const auto& id : m.value("targets", json::array())) {
        const auto* o = d.find(id.get<std::string>());
        if (!o) continue;
        Bnd_Box box = d.worldBounds(*o);
        if (box.IsVoid()) continue;
        box.Enlarge(25);
        const gp_Pnt lo = box.CornerMin(), hi = box.CornerMax();
        if (hi.X() - lo.X() < 1 || hi.Y() - lo.Y() < 1 || hi.Z() - lo.Z() < 1) continue;
        shapes.push_back({outline(lo, hi), tone(m.value("status", std::string()))});
      }
    }
  }
  window_->viewport().setDiagnosticShapes(shapes);
}
}  // namespace sbr::app
