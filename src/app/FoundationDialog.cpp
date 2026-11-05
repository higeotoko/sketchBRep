#include "MainWindow.h"
#include "AiBridge.h"
#include "sbr/ConstructionFoundation.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>

namespace sbr::app {
using json=nlohmann::json;
bool MainWindow::previewFoundation(const std::string& inputs,const std::string& replace,bool replaceEdited) {
  json args={{"inputs",json::parse(inputs)},{"preview",true},{"expected_revision",modeler_.history().revision()}};
  if(!replace.empty()) args["replace"]=replace;
  if(replaceEdited) args["on_modified"]="replace";
  const auto trial=json::parse(aiBridge_->execute(json{{"command","generate_foundation"},{"args",args}}.dump()));
  if(!trial.value("ok",false)) { lastError_=QString::fromStdString(trial.value("error",std::string("基礎を生成できません")));return false; }
  const double volume=trial["result"]["generated_parts_volume_m3"].get<double>();
  args["preview"]=false;args.erase("expected_revision");
  const json request={{"command","propose"},{"args",{{"description","布基礎計画のプレビュー"},
      {"commands",json::array({{{"command","generate_foundation"},{"args",args}}})}}}};
  const auto response=json::parse(aiBridge_->execute(request.dump()));
  if(!response.value("ok",false)) { lastError_=QString::fromStdString(response.value("error",std::string("プレビューできません")));return false; }
  lastError_.clear();
  proposalLabel_->setText(tr("布基礎計画 — コンクリート %1 m³（計画形状）。「適用」／「却下」").arg(volume,0,'f',3));
  setStatus(tr("布基礎をプレビューしています。地盤・構造・配筋等は未照査です。確認して「適用」を押してください"));
  return true;
}

void MainWindow::showFoundationDialog() {
  const auto openedRevision=modeler_.history().revision();
  QDialog dialog(this);dialog.setObjectName(QStringLiteral("foundationDialog"));
  dialog.setWindowTitle(tr("布基礎計画（土台から作成・作り直し）"));dialog.resize(780,680);
  auto* layout=new QVBoxLayout(&dialog);
  auto* notice=new QLabel(tr("実在する土台から、立上りと底盤のコンクリート形状を作ります。交点は層ごとに結合します。\n"
      "初期寸法は計画用です。地盤・支持力・構造・配筋・凍結深度・人通口・換気は未照査です。\n"
      "対象は指定した土台の下のみ。床束下の束石、土台のない耐力壁下、べた基礎は別途計画が必要です。"),&dialog);
  notice->setWordWrap(true);layout->addWidget(notice);
  auto* form=new QFormLayout;
  auto* plans=new QComboBox(&dialog);plans->setObjectName(QStringLiteral("foundationPlan"));plans->addItem(tr("新規"),QString());
  auto* frames=new QComboBox(&dialog);frames->setObjectName(QStringLiteral("foundationSourceFrame"));
  const json initialSources=selection_;
  json selectedSources=initialSources;
  frames->addItem(tr("現在選択した土台（グループを除く）"),QString());
  for(const auto& o:modeler_.document().objects()) {
    if(o.attributes.count("foundation_plan")) plans->addItem(QString::fromStdString(o.name),QString::fromStdString(o.id));
    const auto* group=std::get_if<doc::GroupNode>(&o.recipe.base);
    if(group && (!group->constructionFrame.empty() || (selection_.size()==1 && selection_[0]==o.id)))
      frames->addItem(QString::fromStdString(o.name),QString::fromStdString(o.id));
  }
  if(frames->count()>1 && selection_.empty()) frames->setCurrentIndex(1);
  for(int i=1;i<frames->count();++i) if(selection_.size()==1 && frames->itemData(i).toString().toStdString()==selection_[0]) frames->setCurrentIndex(i);
  form->addRow(tr("基礎計画"),plans);form->addRow(tr("対象の土台"),frames);
  auto number=[&](const QString& name,double value,double minimum,double maximum) {
    auto* spin=new QDoubleSpinBox(&dialog);spin->setObjectName(name);spin->setRange(minimum,maximum);spin->setDecimals(1);spin->setValue(value);return spin;
  };
  auto* stemWidth=number("foundationStemWidth",150,1,10000);
  auto* stemHeight=number("foundationStemHeight",600,1,10000);
  auto* footWidth=number("foundationFootWidth",450,1,10000);
  auto* footHeight=number("foundationFootHeight",150,1,10000);
  auto* packing=number("foundationPacking",20,0,1000);
  auto* top=number("foundationTop",400,-1000000,1000000);
  form->addRow(tr("立上り幅 mm"),stemWidth);form->addRow(tr("立上り高さ mm"),stemHeight);
  form->addRow(tr("底盤幅 mm"),footWidth);form->addRow(tr("底盤厚 mm"),footHeight);form->addRow(tr("土台下のパッキン高さ mm"),packing);
  auto* automatic=new QCheckBox(tr("土台下面−パッキンで基礎天端を算出"),&dialog);automatic->setChecked(true);
  automatic->setObjectName(QStringLiteral("foundationAutomaticTop"));form->addRow(automatic);
  auto* topLabel=new QLabel(tr("基礎天端（世界Z）mm"),&dialog);form->addRow(topLabel,top);
  auto visibility=[=]{ top->setVisible(!automatic->isChecked());topLabel->setVisible(!automatic->isChecked()); };
  connect(automatic,&QCheckBox::toggled,&dialog,visibility);visibility();
  auto* overwrite=new QCheckBox(tr("手編集した基礎も置換する（プレビューで確認）"),&dialog);
  overwrite->setObjectName(QStringLiteral("foundationReplaceEdited"));form->addRow(overwrite);
  layout->addLayout(form);
  auto* report=new QTextBrowser(&dialog);report->setObjectName(QStringLiteral("foundationReport"));layout->addWidget(report,1);
  auto refreshReport=[&] {
    const auto id=plans->currentData().toString().toStdString();
    if(id.empty()) { report->setPlainText(tr("新規計画はプレビューで形状とコンクリート量を確認できます。基礎は文書直下の独立グループに作成します。"));return; }
    const auto result=construction::checkFoundation(modeler_.document(),json{{"foundation",id}}.dump());
    if(!result) { report->setPlainText(QString::fromStdString(result.error()));return; }
    const auto p=json::parse(*result)["plans"][0];
    QStringList text;
    const auto state=p["status"].get<std::string>();
    text<<tr("状態：%1").arg(state=="current"?tr("参照土台・生成形状に変更なし（構造の合否ではありません）"):
        state=="needs_regeneration"?tr("土台変更あり・再生成が必要"):state=="modified"?tr("基礎に手編集あり"):tr("参照土台がない・未対応"));
    text<<tr("基礎天端 Z=%1 mm").arg(p["top_z"].get<double>());
    if(p["generated_parts_volume_m3"].is_number()) text<<tr("コンクリート %1 m³").arg(p["generated_parts_volume_m3"].get<double>(),0,'f',3);
    for(const auto& [role,q]:p["quantities"].items()) text<<tr("%1：%2 m³／連続体 %3 個").arg(
        role=="foundation_stem"?tr("立上り"):tr("底盤")).arg(q["volume_m3"].get<double>(),0,'f',3).arg(q["solids"].get<int>());
    for(const auto& s:p["sources"]) {
      text<<tr("%1：土台下面から基礎天端 %2 mm").arg(QString::fromStdString(s["name"])).arg(s["packing_gap_mm"].get<double>(),0,'f',1);
      if(s["packing_gap_matches"]==false) text<<tr("  要確認：設定したパッキン高さと一致しません。");
    }
    for(const auto& reason:p["reasons"]) text<<QString::fromStdString(reason);
    text<<tr("地盤・構造・配筋・開口等の設計確認は含みません。");report->setPlainText(text.join(QStringLiteral("\n")));
  };
  auto loadPlan=[&] {
    const auto id=plans->currentData().toString().toStdString();overwrite->setChecked(false);
    if(id.empty()) { selectedSources=initialSources;refreshReport();return; }
    const auto* root=modeler_.document().find(id);if(!root) return;
    const auto manifest=json::parse(root->attributes.at("foundation_plan"));const auto& in=manifest["inputs"];
    if(in.contains("frame")) {
      const auto sourceId=QString::fromStdString(in["frame"]);
      int index=frames->findData(sourceId);
      if(index<0) {
        const auto* source=modeler_.document().find(in["frame"].get<std::string>());
        frames->addItem(source?QString::fromStdString(source->name):tr("参照グループなし（対象を選び直してください）"),sourceId);
        index=frames->count()-1;
      }
      frames->setCurrentIndex(index);
    }
    else { selectedSources=in["sources"];frames->setCurrentIndex(0); }
    stemWidth->setValue(in["stem_width"]);stemHeight->setValue(in["stem_height"]);footWidth->setValue(in["footing_width"]);
    footHeight->setValue(in["footing_thickness"]);packing->setValue(in["packing_mm"]);
    automatic->setChecked(!in.contains("top_z"));top->setValue(manifest["top_z"]);refreshReport();
  };
  connect(plans,qOverload<int>(&QComboBox::currentIndexChanged),&dialog,[&]{loadPlan();});
  for(int i=1;i<plans->count();++i) if(selection_.size()==1 && plans->itemData(i).toString().toStdString()==selection_[0]) plans->setCurrentIndex(i);
  loadPlan();
  auto* buttons=new QDialogButtonBox(QDialogButtonBox::Cancel,&dialog);
  auto* preview=buttons->addButton(tr("プレビュー"),QDialogButtonBox::AcceptRole);preview->setObjectName(QStringLiteral("foundationPreview"));
  auto* inspect=buttons->addButton(tr("検査・数量を更新"),QDialogButtonBox::ActionRole);inspect->setObjectName(QStringLiteral("foundationInspect"));
  connect(inspect,&QPushButton::clicked,&dialog,refreshReport);
  connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
  connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);layout->addWidget(buttons);
  if(dialog.exec()!=QDialog::Accepted) return;
  if(modeler_.history().revision()!=openedRevision) { reportError(tr("モデルが変更されました。基礎の設定を開き直してください"));return; }
  json inputs={{"stem_width",stemWidth->value()},{"stem_height",stemHeight->value()},
      {"footing_width",footWidth->value()},{"footing_thickness",footHeight->value()},{"packing_mm",packing->value()}};
  const auto source=frames->currentData().toString().toStdString();
  if(source.empty()) { inputs["sources"]=selectedSources;inputs["frame"]=nullptr; }
  else { inputs["frame"]=source;inputs["sources"]=nullptr; }
  inputs["top_z"]=automatic->isChecked()?json():json(top->value());
  // merge-patch null removes a previous manual height when switching to auto.
  const auto replace=plans->currentData().toString().toStdString();
  if(!previewFoundation(inputs.dump(),replace,overwrite->isChecked())) reportError(lastError_);
}
}
