#include "WallBaysDialog.h"
#include "MainWindow.h"
#include "sbr/ConstructionWall.h"
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>

namespace sbr::app {
using json = nlohmann::json;
WallBaysDialog::WallBaysDialog(MainWindow* window) : QDialog(window), window_(window), markerViewport_(&window->viewport()) {
  setObjectName(QStringLiteral("wallBaysDialog"));
  setWindowTitle(tr("壁の区間候補（開口を除く）"));
  resize(820, 470);
  auto* layout = new QVBoxLayout(this);
  auto* explanation = new QLabel(tr("壁と開口から取り出した形状上の候補です。柱・横架材・仕様は未確認です。\n"
      "行を選ぶと、対象の壁と区間をモデルで強調します。壁量には自動計上しません。"), this);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);
  scope_ = new QComboBox(this);
  scope_->setObjectName(QStringLiteral("wallBaysScope"));
  scope_->addItems({tr("表示中の全ての壁"), tr("平面の作業面の高さにある壁")});
  if (window_->viewport().lockedView() == Viewport::StandardView::Top) scope_->setCurrentIndex(1);
  layout->addWidget(scope_);
  table_ = new QTableWidget(this);
  table_->setObjectName(QStringLiteral("wallBaysTable"));
  table_->setColumnCount(6);
  table_->setHorizontalHeaderLabels({tr("壁"), tr("階属性"), tr("方向"), tr("区間長 mm"), tr("形状の高さ mm"), tr("状態")});
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  layout->addWidget(table_);
  status_ = new QLabel(this);
  status_->setObjectName(QStringLiteral("wallBaysStatus"));
  status_->setWordWrap(true);
  layout->addWidget(status_);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
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
    const QStringList values{QString::fromStdString(bay["host_name"]),
        bay["level"].is_null() ? tr("未指定") : QString::fromStdString(bay["level"]), direction,
        QString::number(bay["length_mm"].get<double>(), 'f', 1),
        QString::number(bay["geometry_height_mm"].get<double>(), 'f', 1), tr("候補・仕様未確認")};
    for (int c = 0; c < values.size(); ++c) table_->setItem(row, c, new QTableWidgetItem(values[c]));
  }
  status_->setText(tr("候補 %1 区間／未対応の壁 %2／非表示の壁 %3。形状の高さは筋かいの算定高さではありません。")
      .arg(data["bays"].size()).arg(data["excluded"].size()).arg(data["hidden_walls_omitted"].get<int>()));
  QStringList excluded;
  for (const auto& wall : data["excluded"])
    excluded << tr("%1：壁形状または開口の加工が未対応").arg(QString::fromStdString(wall["name"]));
  status_->setToolTip(excluded.join(QStringLiteral("\n")));
}

void WallBaysDialog::highlight() {
  if (report_.empty() || sequence_ != window_->modeler().document().sequence()) {
    window_->viewport().setInspectionMarker({});
    return;
  }
  const auto data = json::parse(report_);
  const int row = table_->currentRow();
  if (row < 0 || row >= static_cast<int>(data["bays"].size())) return;
  const auto& bay = data["bays"][row];
  const auto point = [](const json& p) { return gp_Pnt(p[0].get<double>(), p[1].get<double>(), p[2].get<double>()); };
  window_->setSelection({bay["host"].get<std::string>()});
  window_->viewport().setInspectionMarker(BRepBuilderAPI_MakeEdge(point(bay["start"]), point(bay["end"])).Shape());
}
}
