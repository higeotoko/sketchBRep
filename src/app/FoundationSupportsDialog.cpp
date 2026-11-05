#include "FoundationSupportsDialog.h"
#include "MainWindow.h"
#include "sbr/ConstructionFoundation.h"
#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <TopoDS_Compound.hxx>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <set>
#include <nlohmann/json.hpp>

namespace sbr::app {
using json=nlohmann::json;
namespace {
QString state(const std::string& value) {
  if(value=="detected") return QObject::tr("形状対応あり");
  if(value=="missing") return QObject::tr("支持先未検出");
  if(value=="partial") return QObject::tr("一部だけ重なる");
  if(value=="ambiguous") return QObject::tr("複数候補");
  if(value=="gap") return QObject::tr("高さ・隙間を確認");
  if(value=="interference") return QObject::tr("食い込みを確認");
  return QObject::tr("未評価");
}
QString role(const std::string& value) {
  if(value=="sill") return QObject::tr("土台");
  if(value=="floor_post") return QObject::tr("床束");
  if(value=="through_column") return QObject::tr("通し柱");
  return QObject::tr("柱");
}
}
FoundationSupportsDialog::FoundationSupportsDialog(MainWindow* window):QDialog(window),window_(window),viewport_(&window->viewport()) {
  setObjectName(QStringLiteral("foundationSupportsDialog"));setWindowTitle(tr("基礎までの支持先（柱・土台・床束）"));resize(1150,600);
  auto* layout=new QVBoxLayout(this);
  auto* explanation=new QLabel(tr("柱脚・土台下・床束下の実形状を確認します。行を選ぶと対象部材、「支持先を見る」で土台と基礎を選択します。\n"
      "全面の重なりと高さの対応があっても、接合・パッキン実体・アンカー・耐力・地盤の確認は別に必要です。\n"
      "未選定の床束、上階から梁を経由する経路、壁の荷重経路は未評価です。非表示の部材も含めます。"),this);
  explanation->setWordWrap(true);layout->addWidget(explanation);
  table_=new QTableWidget(this);table_->setObjectName(QStringLiteral("foundationSupportsTable"));
  table_->setColumnCount(8);table_->setHorizontalHeaderLabels({tr("対象部材"),tr("役割"),tr("直下の形状"),tr("支持先"),
      tr("隙間 mm"),tr("下面の重なり %"),tr("基礎までの経路"),tr("理由・注意")});
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  layout->addWidget(table_);
  status_=new QLabel(this);status_->setObjectName(QStringLiteral("foundationSupportsStatus"));status_->setWordWrap(true);layout->addWidget(status_);
  auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close,this);buttons->button(QDialogButtonBox::Close)->setText(tr("閉じる"));
  supports_=buttons->addButton(tr("支持先を見る"),QDialogButtonBox::ActionRole);supports_->setObjectName(QStringLiteral("foundationSupportsSelect"));supports_->setEnabled(false);
  auto* reload=buttons->addButton(tr("モデルから再確認"),QDialogButtonBox::ActionRole);reload->setObjectName(QStringLiteral("foundationSupportsRefresh"));
  layout->addWidget(buttons);
  connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
  connect(supports_,&QPushButton::clicked,this,[this]{highlight(true);});
  connect(reload,&QPushButton::clicked,this,&FoundationSupportsDialog::refresh);
  connect(table_,&QTableWidget::itemSelectionChanged,this,[this]{highlight(false);});
  connect(this,&QDialog::finished,this,[this]{if(viewport_) viewport_->setInspectionMarker({});});
  auto* poll=new QTimer(this);poll->setInterval(150);connect(poll,&QTimer::timeout,this,[this]{
    if(!report_.empty() && sequence_!=window_->modeler().document().sequence()) {
      clear();status_->setText(tr("モデルが変わりました。「モデルから再確認」で更新してください。"));
    }
  });poll->start();refresh();
}
FoundationSupportsDialog::~FoundationSupportsDialog() {if(viewport_) viewport_->setInspectionMarker({});}
void FoundationSupportsDialog::clear() {
  report_.clear();supports_->setEnabled(false);table_->setRowCount(0);if(viewport_) viewport_->setInspectionMarker({});
}
void FoundationSupportsDialog::refresh() {
  clear();const auto result=construction::foundationSupports(window_->modeler().document());
  if(!result) {status_->setText(QString::fromStdString(result.error()));return;}
  report_=*result;const auto data=json::parse(report_);sequence_=data["sequence"].get<std::uint64_t>();
  const QSignalBlocker block(table_);
  int complete=0;
  for(const auto& own:data["rows"]) {
    const int row=table_->rowCount();table_->insertRow(row);QStringList names,gaps,coverage,notes;
    if(!own["reason"].get<std::string>().empty()) notes<<QString::fromStdString(own["reason"]);
    const auto append=[&](const json& candidate,bool via) {
      names<<(via?tr("経由：%1"):QStringLiteral("%1")).arg(QString::fromStdString(candidate["name"]));
      gaps<<QString::number(candidate["gap_mm"].get<double>(),'f',2);
      coverage<<QString::number(candidate["coverage_ratio"].get<double>()*100,'f',1);
      if(!candidate["plan_status"].is_null() && candidate["plan_status"]!="current") notes<<tr("参照した基礎計画に変更あり・再確認が必要");
    };
    for(const auto& c:own["candidates"]) append(c,false);
    for(const auto& via:own["via"]) {
      notes<<tr("経由土台：%1").arg(state(via["status"]));
      for(const auto& c:via["candidates"]) append(c,true);
    }
    const bool found=own["path_status"]=="geometry_detected";if(found) ++complete;
    const QString path=found?tr("形状の経路あり（耐力未照査）"):own["path_status"]=="not_evaluated"?tr("未評価"):tr("要確認");
    const QStringList values{QString::fromStdString(own["name"]),role(own["role"]),state(own["status"]),
      names.join("\n"),gaps.join("\n"),coverage.join("\n"),path,notes.join("\n")};
    for(int column=0;column<values.size();++column) {auto* item=new QTableWidgetItem(values[column]);item->setToolTip(values[column]);table_->setItem(row,column,item);}
  }
  table_->resizeRowsToContents();status_->setText(tr("対象 %1／基礎まで形状の経路あり %2。検出許容差 %3 mm、上下探索 %4 mm。%5 未評価の基礎 %6。")
    .arg(data["rows"].size()).arg(complete).arg(data["tolerance_mm"].get<double>()).arg(data["search_depth_mm"].get<double>())
    .arg(data["incomplete"]==true?tr("計算上限に達したため未確認が残ります。"):tr("構造の合否ではありません。"))
    .arg(data["unassessed_supports"].size()));
  QStringList unassessed;
  for(const auto& item:data["unassessed_supports"]) unassessed<<QString::fromStdString(item["name"])+QStringLiteral("：")+QString::fromStdString(item["reason"]);
  status_->setToolTip(unassessed.join(QStringLiteral("\n")));
}
void FoundationSupportsDialog::highlight(bool support) {
  supports_->setEnabled(false);
  if(report_.empty() || sequence_!=window_->modeler().document().sequence()) {if(viewport_) viewport_->setInspectionMarker({});return;}
  const auto data=json::parse(report_);const int row=table_->currentRow();
  if(row<0 || row>=static_cast<int>(data["rows"].size())) return;
  const auto& own=data["rows"][row];std::set<doc::ObjectId> targets;
  for(const auto& c:own["candidates"]) targets.insert(c["id"].get<std::string>());
  for(const auto& via:own["via"]) for(const auto& c:via["candidates"]) targets.insert(c["id"].get<std::string>());
  supports_->setEnabled(!targets.empty());if(!support) targets={own["id"].get<std::string>()};
  if(targets.empty()) return;
  TopoDS_Compound shapes;BRep_Builder builder;builder.MakeCompound(shapes);std::vector<doc::ObjectId> ids;
  for(const auto& id:targets) {
    const auto* o=window_->modeler().document().find(id);if(!o) return;ids.push_back(id);
    const auto shape=window_->modeler().document().worldShape(*o);
    // Adding a solid to a compound changes its shared TShape Free flag.
    // Markers must own copies, so even native B-rep serialization stays unchanged.
    if(!shape.IsNull()) builder.Add(shapes,BRepBuilderAPI_Copy(shape,true,false).Shape());
  }
  window_->setSelection(ids);if(viewport_) viewport_->setInspectionMarker(shapes);
}
}
