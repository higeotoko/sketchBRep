#include "WallBaysDialog.h"
#include "MainWindow.h"
#include "sbr/ConstructionWall.h"
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <QHBoxLayout>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>
#include <map>

namespace sbr::app {
using json = nlohmann::json;
WallBaysDialog::WallBaysDialog(MainWindow* window) : QDialog(window), window_(window), markerViewport_(&window->viewport()) {
  setObjectName(QStringLiteral("wallBaysDialog"));
  setWindowTitle(tr("壁の区間候補（開口を除く）"));
  resize(1100, 520);
  auto* layout = new QVBoxLayout(this);
  auto* explanation = new QLabel(tr("柱と開口の位置で区切り、両端柱・上下材を形状から探します。接合耐力・施工仕様は未確認です。\n"
      "行を選ぶと区間、下のボタンで対応する部材を強調します。壁量には自動計上しません。"), this);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);
  scope_ = new QComboBox(this);
  scope_->setObjectName(QStringLiteral("wallBaysScope"));
  scope_->addItems({tr("表示中の全ての壁"), tr("平面の作業面の高さにある壁")});
  if (window_->viewport().lockedView() == Viewport::StandardView::Top) scope_->setCurrentIndex(1);
  layout->addWidget(scope_);
  table_ = new QTableWidget(this);
  table_->setObjectName(QStringLiteral("wallBaysTable"));
  table_->setColumnCount(10);
  table_->setHorizontalHeaderLabels({tr("壁"), tr("階属性"), tr("方向"), tr("区間長 mm"), tr("形状の高さ mm"),
      tr("始端柱"), tr("終端柱"), tr("下材"), tr("上材"), tr("幾何診断")});
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  layout->addWidget(table_);
  auto* members = new QHBoxLayout;
  members->addWidget(new QLabel(tr("選択区間の部材を見る："), this));
  for (const auto& [role, label] : std::vector<std::pair<std::string, QString>>{
      {"start_post", tr("始端柱")}, {"end_post", tr("終端柱")}, {"bottom", tr("下材")}, {"top", tr("上材")}}) {
    auto* button = new QPushButton(label, this);
    button->setObjectName(QStringLiteral("wallBaySupport_") + QString::fromStdString(role));
    button->setEnabled(false);
    supportButtons_.push_back(button);
    connect(button, &QPushButton::clicked, this, [this, role] { showSupport(role); });
    members->addWidget(button);
  }
  members->addStretch();
  layout->addLayout(members);
  status_ = new QLabel(this);
  status_->setObjectName(QStringLiteral("wallBaysStatus"));
  status_->setWordWrap(true);
  layout->addWidget(status_);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  buttons->button(QDialogButtonBox::Close)->setText(tr("閉じる"));
  auto* reload = buttons->addButton(tr("モデルから再抽出"), QDialogButtonBox::ActionRole);
  reload->setObjectName(QStringLiteral("wallBaysRefresh"));
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(this, &QDialog::finished, this, [this] { window_->viewport().setInspectionMarker({}); });
  connect(reload, &QPushButton::clicked, this, &WallBaysDialog::refresh);
  connect(scope_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { refresh(); });
  connect(table_, &QTableWidget::itemSelectionChanged, this, &WallBaysDialog::highlight);
  auto* poll = new QTimer(this);
  poll->setInterval(150);
  connect(poll, &QTimer::timeout, this, [this] {
    if (!report_.empty() && sequence_ != window_->modeler().document().sequence()) {
      report_.clear();
      table_->setRowCount(0);
      clearSupportButtons();
      window_->viewport().setInspectionMarker({});
      status_->setText(tr("モデルが変わりました。「モデルから再抽出」で候補を更新してください。"));
    }
  });
  poll->start();
  connect(&window_->viewport(), &Viewport::workPlaneChanged, this, [this] { if (scope_->currentIndex() == 1) refresh(); });
  connect(&window_->viewport(), &Viewport::viewLockChanged, this, [this] { if (scope_->currentIndex() == 1) refresh(); });
  refresh();
}
WallBaysDialog::~WallBaysDialog() { if (markerViewport_) markerViewport_->setInspectionMarker({}); }

void WallBaysDialog::refresh() {
  report_.clear();
  table_->setRowCount(0);
  clearSupportButtons();
  window_->viewport().setInspectionMarker({});
  json options = json::object();
  if (scope_->currentIndex() == 1) {
    if (window_->viewport().lockedView() != Viewport::StandardView::Top) {
      status_->setText(tr("平面へ切り替えて高さを選ぶか、「全ての壁」を選んでください。"));
      return;
    }
    options["elevation"] = window_->viewport().workOffset();
  }
  const auto result = construction::wallBayCandidates(window_->modeler(), options.dump());
  if (!result) { status_->setText(QString::fromStdString(result.error())); return; }
  report_ = *result;
  const auto data = json::parse(report_);
  sequence_ = data["sequence"].get<std::uint64_t>();
  const QSignalBlocker block(table_);
  for (const auto& bay : data["bays"]) {
    const int row = table_->rowCount();
    table_->insertRow(row);
    const QString direction = bay["direction"] == "diagonal" ? tr("斜め（壁量未対応）") : QString::fromStdString(bay["direction"]);
    QStringList values{QString::fromStdString(bay["host_name"]),
        bay["level"].is_null() ? tr("未指定") : QString::fromStdString(bay["level"]), direction,
        QString::number(bay["length_mm"].get<double>(), 'f', 1),
        QString::number(bay["geometry_height_mm"].get<double>(), 'f', 1)};
    for (const char* role : {"start_post", "end_post", "bottom", "top"}) {
      const auto& support = bay["supports"][role];
      values << (support["status"] == "detected" ? tr("検出") : support["status"] == "ambiguous" ? tr("複数候補") :
          support["status"] == "partial" ? tr("一部のみ") : tr("未検出"));
    }
    values << (bay["frame_geometry"] == "touching" ? tr("四辺接触・仕様未確認") :
        bay["frame_geometry"] == "gaps" ? tr("部材間に隙間") :
        bay["frame_geometry"] == "unassessed" ? tr("形状判定未完了") :
        bay["frame_geometry"] == "ambiguous" ? tr("対応が曖昧") : tr("未確認の辺あり"));
    for (int c = 0; c < values.size(); ++c) table_->setItem(row, c, new QTableWidgetItem(values[c]));
    int column = 5;
    for (const char* role : {"start_post", "end_post", "bottom", "top"}) {
      QStringList names;
      for (const auto& name : bay["supports"][role]["names"]) names << QString::fromStdString(name);
      for (const auto& name : bay["supports"][role]["partial_names"]) names << tr("一部を覆う材：%1").arg(QString::fromStdString(name));
      table_->item(row, column++)->setToolTip(names.join(QStringLiteral("\n")));
    }
    QStringList joints;
    const std::map<std::string, QString> labels{{"start_bottom", tr("始端柱と下材")}, {"start_top", tr("始端柱と上材")},
        {"end_bottom", tr("終端柱と下材")}, {"end_top", tr("終端柱と上材")}};
    for (const auto& [key, gap] : bay["joint_distances_mm"].items())
      joints << tr("%1：%2 mm").arg(labels.at(key), gap.is_number() ? QString::number(gap.get<double>(), 'f', 3) : tr("未評価"));
    table_->item(row, 9)->setToolTip(joints.join(QStringLiteral("\n")));
  }
  status_->setText(tr("候補 %1 区間／未対応の壁 %2／非表示の壁 %3／未評価の部材 %4。検出許容差 %5 mm。形状の高さは筋かい算定高さと別です。")
      .arg(data["bays"].size()).arg(data["excluded"].size()).arg(data["hidden_walls_omitted"].get<int>())
      .arg(data["unassessed_members"].size()).arg(data["tolerance_mm"].get<double>()));
  QStringList excluded;
  for (const auto& wall : data["excluded"])
    excluded << tr("%1：壁形状または開口の加工が未対応").arg(QString::fromStdString(wall["name"]));
  for (const auto& member : data["unassessed_members"])
    excluded << tr("%1：部材形状または軸方向が未対応").arg(QString::fromStdString(member["name"]));
  status_->setToolTip(excluded.join(QStringLiteral("\n")));
}

void WallBaysDialog::highlight() {
  clearSupportButtons();
  if (report_.empty() || sequence_ != window_->modeler().document().sequence()) {
    window_->viewport().setInspectionMarker({});
    return;
  }
  const auto data = json::parse(report_);
  const int row = table_->currentRow();
  if (row < 0 || row >= static_cast<int>(data["bays"].size())) return;
  const auto& bay = data["bays"][row];
  int index = 0;
  for (const char* role : {"start_post", "end_post", "bottom", "top"})
    supportButtons_[index++]->setEnabled(!bay["supports"][role]["ids"].empty() || !bay["supports"][role]["partial_ids"].empty());
  const auto point = [](const json& p) { return gp_Pnt(p[0].get<double>(), p[1].get<double>(), p[2].get<double>()); };
  window_->setSelection({bay["host"].get<std::string>()});
  window_->viewport().setInspectionMarker(BRepBuilderAPI_MakeEdge(point(bay["start"]), point(bay["end"])).Shape());
}
void WallBaysDialog::clearSupportButtons() { for (auto* button : supportButtons_) button->setEnabled(false); }

void WallBaysDialog::showSupport(const std::string& role) {
  if (report_.empty() || sequence_ != window_->modeler().document().sequence()) return;
  const auto data = json::parse(report_);
  const int row = table_->currentRow();
  if (row < 0 || row >= static_cast<int>(data["bays"].size())) return;
  std::vector<doc::ObjectId> ids;
  TopoDS_Compound shapes;
  BRep_Builder builder;
  builder.MakeCompound(shapes);
  auto references = data["bays"][row]["supports"][role]["ids"];
  for (const auto& id : data["bays"][row]["supports"][role]["partial_ids"]) references.push_back(id);
  for (const auto& id : references) {
    const auto* member = window_->modeler().document().find(id.get<std::string>());
    if (!member) return;
    ids.push_back(member->id);
    builder.Add(shapes, window_->modeler().document().worldShape(*member));
  }
  if (ids.empty()) return;
  window_->setSelection(ids);
  window_->viewport().setInspectionMarker(shapes);
}
}
