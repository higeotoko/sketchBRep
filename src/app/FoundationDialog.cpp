#include "MainWindow.h"
#include "AiBridge.h"
#include "sbr/ConstructionFoundation.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>

namespace sbr::app {
using json=nlohmann::json;
bool MainWindow::previewFoundation(const std::string& inputs,const std::string& replace,bool replaceEdited) {
  json args={{"inputs",json::parse(inputs)},{"preview",true},{"expected_revision",modeler_.history().revision()}};
  const auto type=args["inputs"].value("type",std::string("strip"));
  const auto title=type=="pads"?tr("束石・独立基礎計画"):type=="mat"?tr("べた基礎計画"):tr("布基礎計画");
  if(!replace.empty()) args["replace"]=replace;
  if(replaceEdited) args["on_modified"]="replace";
  const auto trial=json::parse(aiBridge_->execute(json{{"command","generate_foundation"},{"args",args}}.dump()));
  if(!trial.value("ok",false)) { lastError_=QString::fromStdString(trial.value("error",std::string("基礎を生成できません")));return false; }
  const double volume=trial["result"]["generated_parts_volume_m3"].get<double>();
  args["preview"]=false;args.erase("expected_revision");
  const json request={{"command","propose"},{"args",{{"description",title.toStdString()},
      {"commands",json::array({{{"command","generate_foundation"},{"args",args}}})}}}};
  const auto response=json::parse(aiBridge_->execute(request.dump()));
  if(!response.value("ok",false)) { lastError_=QString::fromStdString(response.value("error",std::string("プレビューできません")));return false; }
  lastError_.clear();
  proposalLabel_->setText(tr("%1 — コンクリート %2 m³（計画形状）。「適用」／「却下」").arg(title).arg(volume,0,'f',3));
  setStatus(tr("%1をプレビューしています。地盤・構造・配筋等は未照査です。確認して「適用」を押してください").arg(title));
  return true;
}

void MainWindow::showFoundationDialog() {
  showFoundationDialogFor(false);
}
void MainWindow::showFoundationPadsDialog() {
  showFoundationDialogFor(true);
}
void MainWindow::showFoundationMatDialog() {
  showFoundationDialogFor(false,true);
}
// Strip and mat share the sill-based dialog (a type choice); pads have their own.
void MainWindow::showFoundationDialogFor(bool pads,bool mat) {
  const auto openedRevision=modeler_.history().revision();
  QDialog dialog(this);dialog.setObjectName(QStringLiteral("foundationDialog"));
  dialog.setWindowTitle(pads?tr("束石・独立基礎計画（床束・柱から）"):tr("布基礎・べた基礎計画（土台から作成・作り直し）"));dialog.resize(780,720);
  auto* layout=new QVBoxLayout(&dialog);
  auto* notice=new QLabel(pads?tr("選択した実在する床束・柱の下面から、束石・独立基礎の計画形状を作ります。\n"
      "寸法は仮の計画値です。地盤・支持力・凍結・構造・配筋・アンカー等は未照査です。\n"
      "未選定の床束の目印は対象外です。既存基礎と重なる配置は避け、支持先一覧で未検出の床束を選んでください。"):
      tr("実在する土台から基礎のコンクリート形状を作ります。布基礎は立上りと底盤、べた基礎は立上りと外周の内側の耐圧盤です。\n"
      "初期寸法は計画用です。地盤・支持力・構造・配筋・凍結深度・防湿・換気は未照査です。\n"
      "対象は指定した土台の下のみ。べた基礎は外周の土台が一周つながっている必要があります。"),&dialog);
  notice->setWordWrap(true);layout->addWidget(notice);
  auto* form=new QFormLayout;
  auto* plans=new QComboBox(&dialog);plans->setObjectName(QStringLiteral("foundationPlan"));plans->addItem(tr("新規"),QString());
  auto* kind=new QComboBox(&dialog);kind->setObjectName(QStringLiteral("foundationType"));
  kind->addItem(tr("布基礎（立上り＋底盤）"),QStringLiteral("strip"));kind->addItem(tr("べた基礎（立上り＋耐圧盤）"),QStringLiteral("mat"));
  kind->setCurrentIndex(mat?1:0);kind->setVisible(!pads);
  auto* frames=new QComboBox(&dialog);frames->setObjectName(QStringLiteral("foundationSourceFrame"));
  const json initialSources=selection_;
  json selectedSources=initialSources;
  frames->addItem(pads?tr("選択した実部材の床束・柱"):tr("現在選択した土台（グループを除く）"),QString());
  if(pads) frames->addItem(tr("この計画に保存した参照部材"),QString());
  for(const auto& o:modeler_.document().objects()) {
    if(o.attributes.count("foundation_plan") && (json::parse(o.attributes.at("foundation_plan"))["inputs"].value("type",std::string("strip"))=="pads")==pads)
      plans->addItem(QString::fromStdString(o.name),QString::fromStdString(o.id));
    const auto* group=std::get_if<doc::GroupNode>(&o.recipe.base);
    if(!pads && group && (!group->constructionFrame.empty() || (selection_.size()==1 && selection_[0]==o.id)))
      frames->addItem(QString::fromStdString(o.name),QString::fromStdString(o.id));
  }
  if(!pads && frames->count()>1 && selection_.empty()) frames->setCurrentIndex(1);
  for(int i=1;i<frames->count();++i) if(selection_.size()==1 && frames->itemData(i).toString().toStdString()==selection_[0]) frames->setCurrentIndex(i);
  form->addRow(tr("基礎計画"),plans);if(!pads) form->addRow(tr("種別"),kind);form->addRow(pads?tr("対象の床束・柱"):tr("対象の土台"),frames);
  auto number=[&](const QString& name,double value,double minimum,double maximum) {
    auto* spin=new QDoubleSpinBox(&dialog);spin->setObjectName(name);spin->setRange(minimum,maximum);spin->setDecimals(1);spin->setValue(value);return spin;
  };
  auto* stemWidth=number("foundationStemWidth",150,1,10000);
  auto* stemHeight=number("foundationStemHeight",600,1,10000);
  auto* footWidth=number("foundationFootWidth",pads?300:450,1,10000);
  auto* footHeight=number("foundationFootHeight",150,1,10000);
  auto* padDepth=number("foundationPadDepth",300,1,10000);padDepth->setVisible(pads);
  auto* packing=number("foundationPacking",20,0,1000);
  auto* top=number("foundationTop",400,-1000000,1000000);
  auto* slabThickness=number("foundationSlabThickness",150,1,10000);
  auto* edgeWidth=number("foundationEdgeWidth",300,1,10000);
  auto* edgeDepth=number("foundationEdgeDepth",150,0,10000);
  auto* innerWidth=number("foundationInnerWidth",300,1,10000);
  auto* innerDepth=number("foundationInnerDepth",0,0,10000);
  auto* passages=new QLineEdit(&dialog);passages->setObjectName(QStringLiteral("foundationPassages"));
  passages->setPlaceholderText(tr("例：1500,1000; 4500,2730（内部立上りの上の点 X,Y mm）"));
  auto* passageWidth=number("foundationPassageWidth",600,1,8000);
  auto* passageHeight=number("foundationPassageHeight",330,1,2000);
  stemWidth->setVisible(!pads);stemHeight->setVisible(!pads);packing->setVisible(!pads);
  auto* stemHeightLabel=new QLabel(&dialog);
  if(!pads) {form->addRow(tr("立上り幅 mm"),stemWidth);form->addRow(stemHeightLabel,stemHeight);}
  auto* footWidthLabel=new QLabel(pads?tr("束石の幅（世界X）mm"):tr("底盤幅 mm"),&dialog);
  form->addRow(footWidthLabel,footWidth);
  if(pads) form->addRow(tr("束石の奥行（世界Y）mm"),padDepth);
  auto* footHeightLabel=new QLabel(pads?tr("束石の高さ mm"):tr("底盤厚 mm"),&dialog);
  form->addRow(footHeightLabel,footHeight);
  auto* slabLabel=new QLabel(tr("耐圧盤の厚さ mm"),&dialog);
  auto* edgeLabel=new QLabel(tr("外周の根入れ部 幅・深さ mm"),&dialog);
  const auto edgeTip=tr("外周の立上りの下に、耐圧盤の下面から下へ作る帯（根入れ部）。深さは耐圧盤の下面から。0 で作りません。\n"
                        "地盤面からの根入れ深さ・凍結深度は確認していません");
  for(QWidget* w:std::initializer_list<QWidget*>{edgeLabel,edgeWidth,edgeDepth}) w->setToolTip(edgeTip);
  auto* innerLabel=new QLabel(tr("内部の地中梁 幅・深さ mm"),&dialog);
  const auto innerTip=tr("内部の立上りの下に、耐圧盤の下面から下へ作る帯（地中梁）。深さ 0 で作りません（初期値）。\n"
                         "耐圧盤の外にはみ出す分と外周の根入れ部との重なりは除きます。断面・配筋は確認していません");
  for(QWidget* w:std::initializer_list<QWidget*>{innerLabel,innerWidth,innerDepth}) w->setToolTip(innerTip);
  auto* innerSize=new QHBoxLayout;innerSize->addWidget(innerWidth);innerSize->addWidget(innerDepth);
  auto* edgeSize=new QHBoxLayout;edgeSize->addWidget(edgeWidth);edgeSize->addWidget(edgeDepth);
  auto* passagesLabel=new QLabel(tr("人通口の位置"),&dialog);
  auto* passageSizeLabel=new QLabel(tr("人通口の幅・高さ mm"),&dialog);
  auto* passageSize=new QHBoxLayout;passageSize->addWidget(passageWidth);passageSize->addWidget(passageHeight);
  if(!pads) {form->addRow(slabLabel,slabThickness);form->addRow(edgeLabel,edgeSize);form->addRow(innerLabel,innerSize);
    form->addRow(passagesLabel,passages);form->addRow(passageSizeLabel,passageSize);}
  else {for(QWidget* w:std::initializer_list<QWidget*>{slabLabel,slabThickness,edgeLabel,edgeWidth,edgeDepth,innerLabel,innerWidth,innerDepth,
                                                      passagesLabel,passages,passageSizeLabel,passageWidth,passageHeight,stemHeightLabel}) w->hide();}
  // Show the fields of the chosen type; the mat stem height is its rise above the slab top.
  auto showType=[=]{
    if(pads) return;
    const bool isMat=kind->currentData().toString()==QStringLiteral("mat");
    stemHeightLabel->setText(isMat?tr("立上り高さ（耐圧盤の上から）mm"):tr("立上り高さ mm"));
    for(QWidget* w:std::initializer_list<QWidget*>{footWidthLabel,footWidth,footHeightLabel,footHeight}) w->setVisible(!isMat);
    for(QWidget* w:std::initializer_list<QWidget*>{slabLabel,slabThickness,edgeLabel,edgeWidth,edgeDepth,innerLabel,innerWidth,innerDepth,
                                                    passagesLabel,passages,passageSizeLabel,passageWidth,passageHeight}) w->setVisible(isMat);
  };
  auto typeDefaults=[=]{
    // Planning placeholders per type (not design values); a loaded plan overwrites them.
    if(!pads) stemHeight->setValue(kind->currentData().toString()==QStringLiteral("mat")?400:600);
    showType();
  };
  connect(kind,qOverload<int>(&QComboBox::currentIndexChanged),&dialog,typeDefaults);typeDefaults();
  if(!pads) form->addRow(tr("土台下のパッキン高さ mm"),packing);
  auto* automatic=new QCheckBox(pads?tr("各床束・柱の下面に天端を合わせる"):tr("土台下面−パッキンで基礎天端を算出"),&dialog);automatic->setChecked(true);
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
    text<<tr("状態：%1").arg(state=="current"?tr("参照部材・生成形状に変更なし（構造の合否ではありません）"):
        state=="needs_regeneration"?tr("参照部材の変更あり・再生成が必要"):state=="modified"?tr("基礎に手編集あり"):tr("参照部材がない・未対応"));
    text<<(p["top_z"].is_number()?tr("基礎天端 Z=%1 mm").arg(p["top_z"].get<double>()):tr("基礎天端：各部材の下面に合わせた高さ"));
    if(p["generated_parts_volume_m3"].is_number()) text<<tr("コンクリート %1 m³").arg(p["generated_parts_volume_m3"].get<double>(),0,'f',3);
    for(const auto& [role,q]:p["quantities"].items()) text<<tr("%1：%2 m³／連続体 %3 個").arg(
        role=="foundation_stem"?tr("立上り"):role=="foundation_pad"?tr("束石・独立基礎"):role=="foundation_slab"?tr("耐圧盤"):role=="foundation_edge_footing"?tr("外周の根入れ部"):role=="foundation_ground_beam"?tr("内部の地中梁"):tr("底盤")).arg(q["volume_m3"].get<double>(),0,'f',3).arg(q["solids"].get<int>());
    for(const auto& s:p["sources"]) {
      text<<tr("%1：部材下面から基礎天端 %2 mm").arg(QString::fromStdString(s["name"])).arg(s["packing_gap_mm"].get<double>(),0,'f',1);
      if(s["packing_gap_matches"]==false) text<<(pads?tr("  要確認：部材下面と基礎天端が一致しません。"):tr("  要確認：設定したパッキン高さと一致しません。"));
    }
    for(const auto& reason:p["reasons"]) text<<QString::fromStdString(reason);
    text<<tr("地盤・構造・配筋・開口等の設計確認は含みません。");report->setPlainText(text.join(QStringLiteral("\n")));
  };
  auto loadPlan=[&] {
    const auto id=plans->currentData().toString().toStdString();overwrite->setChecked(false);
    kind->setEnabled(id.empty());  // a plan keeps its type; make a new plan for another type
    if(id.empty()) { selectedSources=initialSources;if(pads) frames->setCurrentIndex(0);refreshReport();return; }
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
    else { selectedSources=in["sources"];frames->setCurrentIndex(pads?1:0); }
    if(pads) {footWidth->setValue(in["width"]);padDepth->setValue(in["depth"]);footHeight->setValue(in["height"]);}
    else {
      const bool isMat=in.value("type",std::string("strip"))=="mat";
      { const QSignalBlocker block(kind);kind->setCurrentIndex(isMat?1:0); }showType();
      stemWidth->setValue(in["stem_width"]);stemHeight->setValue(in["stem_height"]);packing->setValue(in["packing_mm"]);
      if(isMat) {
        slabThickness->setValue(in["slab_thickness"]);QStringList points;
        edgeWidth->setValue(in.value("edge_width",300.0));edgeDepth->setValue(in.value("edge_depth",150.0));
        innerWidth->setValue(in.value("inner_width",300.0));innerDepth->setValue(in.value("inner_depth",0.0));
        for(const auto& p:in.value("passages",json::array())) {
          points<<QStringLiteral("%1,%2").arg(p["at"][0].get<double>()).arg(p["at"][1].get<double>());
          passageWidth->setValue(p["width"]);passageHeight->setValue(p["height"]);
        }
        passages->setText(points.join(QStringLiteral("; ")));
      } else {footWidth->setValue(in["footing_width"]);footHeight->setValue(in["footing_thickness"]);}
    }
    automatic->setChecked(!in.contains("top_z"));if(manifest["top_z"].is_number()) top->setValue(manifest["top_z"]);refreshReport();
  };
  connect(plans,qOverload<int>(&QComboBox::currentIndexChanged),&dialog,[&]{loadPlan();});
  for(int i=1;i<plans->count();++i) if(selection_.size()==1 && plans->itemData(i).toString().toStdString()==selection_[0]) plans->setCurrentIndex(i);
  loadPlan();
  auto* buttons=new QDialogButtonBox(QDialogButtonBox::Cancel,&dialog);
  buttons->button(QDialogButtonBox::Cancel)->setText(tr("キャンセル"));
  auto* preview=buttons->addButton(tr("プレビュー"),QDialogButtonBox::AcceptRole);preview->setObjectName(QStringLiteral("foundationPreview"));
  auto* inspect=buttons->addButton(tr("検査・数量を更新"),QDialogButtonBox::ActionRole);inspect->setObjectName(QStringLiteral("foundationInspect"));
  connect(inspect,&QPushButton::clicked,&dialog,refreshReport);
  connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
  connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);layout->addWidget(buttons);
  if(dialog.exec()!=QDialog::Accepted) return;
  if(modeler_.history().revision()!=openedRevision) { reportError(tr("モデルが変更されました。基礎の設定を開き直してください"));return; }
  json inputs={{"stem_width",stemWidth->value()},{"stem_height",stemHeight->value()},
      {"footing_width",footWidth->value()},{"footing_thickness",footHeight->value()},{"packing_mm",packing->value()}};
  if(!pads && kind->currentData().toString()==QStringLiteral("mat")) {
    json list=json::array();
    for(const auto& item:passages->text().split(QLatin1Char(';'),Qt::SkipEmptyParts)) {
      if(item.trimmed().isEmpty()) continue;
      const auto xy=item.split(QRegularExpression(QStringLiteral("[,\\s]+")),Qt::SkipEmptyParts);
      bool okX=false,okY=false;const double x=xy.value(0).toDouble(&okX),y=xy.value(1).toDouble(&okY);
      if(xy.size()!=2 || !okX || !okY) { reportError(tr("人通口の位置は「X,Y」をセミコロンで区切って入力してください: %1").arg(item.trimmed()));return; }
      list.push_back({{"at",{x,y}},{"width",passageWidth->value()},{"height",passageHeight->value()}});
    }
    inputs={{"type","mat"},{"stem_width",stemWidth->value()},{"stem_height",stemHeight->value()},
            {"slab_thickness",slabThickness->value()},{"edge_width",edgeWidth->value()},{"edge_depth",edgeDepth->value()},
            {"inner_width",innerWidth->value()},{"inner_depth",innerDepth->value()},
            {"packing_mm",packing->value()},{"passages",list}};
  }
  const auto source=frames->currentData().toString().toStdString();
  if(pads) inputs={{"type","pads"},{"width",footWidth->value()},{"depth",padDepth->value()},{"height",footHeight->value()}};
  if(pads) inputs["sources"]=frames->currentIndex()==0?initialSources:selectedSources;
  else if(source.empty()) { inputs["sources"]=selectedSources;inputs["frame"]=nullptr; }
  else { inputs["frame"]=source;inputs["sources"]=nullptr; }
  inputs["top_z"]=automatic->isChecked()?json():json(top->value());
  // merge-patch null removes a previous manual height when switching to auto.
  const auto replace=plans->currentData().toString().toStdString();
  if(!previewFoundation(inputs.dump(),replace,overwrite->isChecked())) reportError(lastError_);
}
}
