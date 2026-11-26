#include "sbr/doc/PermitIntake.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sbr::doc {
namespace {

enum class Need { Required, WhenApplicable, ExceptSpecifiedTimber, ByRoute, Requested };

struct Rule {
  const char* form;
  const char* group;
  const char* kind;
  Need need;
  std::vector<std::string> any;      // a sheet name containing one of these
  std::vector<std::string> exclude;  // ... and none of these
  int atLeast = 1;
  const char* note;
};

const char* needText(Need n) {
  switch (n) {
    case Need::Required: return "必要";
    case Need::WhenApplicable: return "該当時";
    case Need::ExceptSpecifiedTimber: return "特定木造建築物を除き必要";
    case Need::ByRoute: return "構造の規定・ルートによる";
    case Need::Requested: return "添付の協力依頼";
  }
  return "";
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

std::vector<Rule> rules() {
  const std::vector<std::string> sectionsOnly = {"断熱範囲", "高さ制限"};
  return {
      // 様式1-2 ４ 添付図書の確認
      {"様式1-2", "図面リスト", "図面リスト", Need::Requested, {"図面リスト"}, {}, 1,
       "別紙の例の「図書の種類」「設計者」の列は、照合結果の図書の種類と設計者名で確認"},
      {"様式1-2", "表1（い）", "附近見取図", Need::Required, {"付近見取図", "附近見取図"}, {}, 1, ""},
      {"様式1-2", "表1（い）", "配置図", Need::Required, {"配置図"}, {"耐力壁"}, 1, ""},
      {"様式1-2", "表1（い）", "各階平面図", Need::Required, {"平面図"}, {"断熱範囲", "耐力壁"}, 1, ""},
      {"様式1-2", "表1（い）", "床面積求積図", Need::Required, {"床面積求積図"}, {}, 1, ""},
      {"様式1-2", "表1（ろ）", "2面以上の立面図", Need::Required, {"立面図"}, {}, 2, ""},
      {"様式1-2", "表1（ろ）", "2面以上の断面図", Need::Required, {"断面図"}, sectionsOnly, 2,
       "高さ制限・断熱範囲の断面は数に入れていない"},
      {"様式1-2", "表1（ろ）", "地盤面算出表", Need::Required, {"地盤面算出"}, {}, 1,
       "建物の接地外周・高さを入れると図面一式に入る（docs/288）"},
      {"様式1-2", "表1（は）", "基礎伏図", Need::ExceptSpecifiedTimber, {"基礎伏図", "基礎・土台伏図"}, {}, 1,
       "木造架構（基礎・土台）を作ると図面一式に入る（docs/289）。基礎の配筋・断面の図は別"},
      {"様式1-2", "表1（は）", "各階床伏図", Need::ExceptSpecifiedTimber, {"床伏図"}, {}, 1, ""},
      {"様式1-2", "表1（は）", "小屋伏図", Need::ExceptSpecifiedTimber, {"小屋伏図"}, {}, 1, ""},
      {"様式1-2", "表1（は）", "構造詳細図", Need::ExceptSpecifiedTimber, {"構造詳細図"}, {}, 1, ""},
      {"様式1-2", "表2・表3", "法第20条（構造規定）構造図・構造計算書等", Need::ByRoute,
       {"耐力壁配置図", "壁量計算表"}, {}, 1, "様式2 構造受付チェックシートで図書ごとに確認"},
      {"様式1-2", "表1", "耐火構造等の構造詳細図", Need::WhenApplicable, {"耐火構造", "防火構造"}, {}, 1, ""},
      {"様式1-2", "表1", "使用建築材料表", Need::Required, {"使用建築材料"}, {}, 1,
       "仕上げ表がある時は候補に出す。内装の建築材料の区分（令20条の7）の明示は内容で確認"},
      {"様式1-2", "表1", "室内仕上げ表", Need::Required, {"仕上げ表"}, {}, 1, ""},
      {"様式1-2", "表1", "開口部の採光に有効な部分の面積を算定した際の計算書", Need::Required, {"採光"}, {}, 1, ""},
      {"様式1-2", "表1", "有効換気量又は有効換気換算量を算定した際の計算書", Need::Required, {"換気"}, {}, 1, ""},
      {"様式1-2", "表1", "法による許可または認定の図書", Need::WhenApplicable, {}, {}, 1, ""},
      {"様式1-2", "表1", "消火設備の構造詳細図", Need::WhenApplicable, {}, {}, 1, ""},
      {"様式1-2", "表1", "危険物の数量表", Need::WhenApplicable, {}, {}, 1, ""},
      {"様式1-2", "表1", "工場・事業調書など", Need::WhenApplicable, {}, {}, 1, ""},
      {"様式1-2", "表1", "容積率に関する図書（敷地面積求積図など）", Need::Required,
       {"敷地面積求積図", "床面積求積図", "面積の表"}, {}, 1, "敷地面積求積図は敷地境界を登録すると入る"},
      {"様式1-2", "表1", "建ぺい率に関する図書（建築面積求積図など）", Need::Required, {"建築面積求積図"}, {}, 1, ""},
      {"様式1-2", "表1", "法第53条の2第3項等を証する書面", Need::WhenApplicable, {}, {}, 1, ""},
      {"様式1-2", "表1", "天空率に関する図書", Need::WhenApplicable, {"天空率"}, {}, 1, ""},
      {"様式1-2", "表1", "日影に関する図書", Need::WhenApplicable, {"日影"}, {}, 1, ""},
      {"様式1-2", "表1", "既存建築物に対する制限の緩和に関する図書", Need::WhenApplicable, {}, {}, 1, ""},
      {"様式1-2", "第4項 表1", "換気設備関係図書", Need::Required, {"機器表", "換気"}, {}, 1,
       "機器表・採光換気計算表を候補にする。機械換気の図書の明示事項は内容で確認"},
      {"様式1-2", "第4項 表1", "浄化槽関係図書", Need::WhenApplicable, {}, {}, 1, ""},
      {"様式1-2", "第4項 表1", "電気設備関係図書", Need::WhenApplicable, {}, {}, 1, ""},
      {"様式1-2", "第4項 表1", "避雷設備関係図書", Need::WhenApplicable, {}, {}, 1, ""},
      {"様式1-2", "第4項 表1", "昇降機関係図書", Need::WhenApplicable, {}, {}, 1, ""},
      {"様式1-2", "第4項 表1", "排煙設備関係図書", Need::WhenApplicable, {}, {}, 1, ""},
      {"様式1-2", "第4項 表1", "非常用照明関係図書", Need::WhenApplicable, {}, {}, 1, ""},
      {"様式1-2", "第4項 表1", "配管設備関係図書（給排水、ガス等）", Need::WhenApplicable, {}, {}, 1, ""},
      {"様式1-2", "第4項 表1", "その他の建築設備に関する図書", Need::WhenApplicable, {}, {}, 1, ""},
      {"様式1-2", "表1", "建築基準関係規定の図書", Need::Required, {"省エネ", "断熱仕様表", "断熱範囲図", "開口部仕様表"}, {}, 1,
       "省エネ（仕様基準）の図書を候補にする。他の関係規定は案件ごとに確認"},
      {"様式1-2", "表4", "横浜市建築基準条例に関する図書", Need::WhenApplicable, {}, {}, 1,
       "細則第4条の2 別表第1 で確認"},
      // 様式2 規則第1条の3（表2）に掲げる図書（令第3章の節ごとの○は様式で確認）
      {"様式2", "表2", "各階平面図", Need::ByRoute, {"平面図"}, {"断熱範囲", "耐力壁"}, 1, ""},
      {"様式2", "表2", "2面以上の立面図", Need::ByRoute, {"立面図"}, {}, 2, ""},
      {"様式2", "表2", "2面以上の断面図", Need::ByRoute, {"断面図"}, sectionsOnly, 2, ""},
      {"様式2", "表2", "仕様表", Need::ByRoute, {"仕様表"}, {}, 1, "開口部・断熱の仕様表は構造の仕様表ではない"},
      {"様式2", "表2", "基礎伏図", Need::ByRoute, {"基礎伏図", "基礎・土台伏図"}, {}, 1, ""},
      {"様式2", "表2", "各階床伏図", Need::ByRoute, {"床伏図"}, {}, 1, ""},
      {"様式2", "表2", "小屋伏図", Need::ByRoute, {"小屋伏図"}, {}, 1, ""},
      {"様式2", "表2", "2面以上の軸組図", Need::ByRoute, {"軸組図"}, {}, 2,
       "外周の通りの軸組図は木造架構から入る（docs/290）。内部の通りは必要に応じて"},
      {"様式2", "表2", "構造詳細図", Need::ByRoute, {"構造詳細図"}, {}, 1, ""},
      {"様式2", "表2", "使用構造材料一覧表", Need::ByRoute, {"構造材料"}, {}, 1, ""},
      {"様式2", "表2", "基礎・地盤説明書", Need::ByRoute, {"地盤説明"}, {}, 1, ""},
      {"様式2", "表2", "施工方法等計画書", Need::ByRoute, {"施工方法"}, {}, 1, ""},
      {"様式2", "表3", "構造計算書（壁量計算等を含む）", Need::ByRoute, {"壁量計算表", "耐力壁配置図"}, {}, 1,
       "適用する構造計算・仕様規定のルートで必要な計算書の範囲を確認"},
  };
}

// The 様式3 table (確認申請手数料, 面積区分による申請手数料).
struct Bracket {
  double upTo;  // m2, inclusive; < 0: no upper bound
  const char* text;
  int fee;
};
const Bracket kBrackets[] = {
    {30, "30㎡以下", 15000},
    {100, "30㎡を超え100㎡以下", 28000},
    {200, "100㎡を超え200㎡以下", 43000},
    {300, "200㎡を超え300㎡以下", 48000},
    {500, "300㎡を超え500㎡以下", 55000},
    {1000, "500㎡を超え1,000㎡以下", 66000},
    {2000, "1,000㎡を超え2,000㎡以下", 93000},
    {5000, "2,000㎡を超え5,000㎡以下", 160000},
    {10000, "5,000㎡を超え10,000㎡以下", 280000},
    {30000, "10,000㎡を超え30,000㎡以下", 370000},
    {50000, "30,000㎡を超え50,000㎡以下", 460000},
    {-1, "50,000㎡を超えるもの", 900000},
};

}  // namespace

draw::Table intakeDrawingList(const IntakeReview& review) {
  draw::Table t;
  t.title = "図面リスト（図書の種類）";
  t.headers = {"図面番号", "図面名", "規則第1条の3による「図書の種類」", "設計者"};
  for (const auto& d : review.drawingList) {
    std::string kinds;
    for (const auto& k : d.kinds) kinds += (kinds.empty() ? "" : "・") + k;
    t.rows.push_back({d.number, d.name, kinds, ""});
  }
  return t;
}

bool applicationFloorAreaKnown(const AreaTable& area) {
  return area.legalTotal > 0 && std::all_of(area.rows.begin(), area.rows.end(), [](const AreaRow& floor) {
           return floor.legalKnown && floor.unreadableWalls == 0;
         });
}

int yokohamaConfirmationFee(double squareMetres, std::string* bracket) {
  if (!(squareMetres > 0)) return 0;
  for (const Bracket& b : kBrackets)
    if (b.upTo < 0 || squareMetres <= b.upTo) {
      if (bracket) *bracket = b.text;
      return b.fee;
    }
  return 0;
}

IntakeReview yokohamaIntakeReview(const Document& document, const DrawingSet* set, const std::string& setError,
                                  bool wholeFloorForFee) {
  IntakeReview out;
  if (!set) out.drawingError = setError.empty() ? "図面一式を作れません" : setError;
  std::vector<std::vector<std::string>> kindsOf(set ? set->names.size() : 0);
  for (const Rule& r : rules()) {
    IntakeRow row;
    row.form = r.form;
    row.group = r.group;
    row.kind = r.kind;
    row.need = needText(r.need);
    row.note = r.note;
    if (set) {
      for (std::size_t i = 0; i < set->names.size(); ++i) {
        const std::string& name = set->names[i];
        const bool hit = std::any_of(r.any.begin(), r.any.end(), [&](const std::string& p) { return contains(name, p); });
        const bool skip = std::any_of(r.exclude.begin(), r.exclude.end(), [&](const std::string& p) { return contains(name, p); });
        if (hit && !skip) {
          row.sheets.push_back(set->numbers[i] + " " + name);
          if (std::string(r.form) == "様式1-2") kindsOf[i].push_back(r.kind);
        }
      }
    }
    const int found = static_cast<int>(row.sheets.size());
    if (!set) {
      row.state = r.any.empty() ? (r.need == Need::WhenApplicable ? "該当時に確認" : "アプリ外") : "図面一式を確認できない";
    } else if (r.any.empty()) {
      row.state = r.need == Need::WhenApplicable ? "該当時に確認" : "アプリ外";
    } else if (found == 0) {
      row.state = r.need == Need::WhenApplicable ? "該当時に確認" : "図面一式にない";
    } else if (found < r.atLeast) {
      row.state = "要確認";
      row.note = (row.note.empty() ? "" : row.note + "。") + std::to_string(r.atLeast) + " 枚以上が要るが " +
                 std::to_string(found) + " 枚";
    } else {
      row.state = "図面あり";
    }
    // Names the set gives for study use: say so on the row (the sheet is there, its name is not a submission name).
    if (found > 0 && std::any_of(row.sheets.begin(), row.sheets.end(), [](const std::string& s) { return contains(s, "検討用"); })) {
      row.state = "要確認";
      row.note = (row.note.empty() ? "" : row.note + "。") + "図面名が「検討用」。提出用の図面名と明示事項を確認";
    }
    // Plans: one per storey with a floor.
    if (std::string(r.kind) == "各階平面図" && set && found > 0) {
      int storeys = 0;
      for (const AreaRow& f : areaTable(document).rows)
        if (f.legalKnown && f.legalFloor > 0) ++storeys;
      if (storeys > found) {
        row.state = "要確認";
        row.note = (row.note.empty() ? "" : row.note + "。") + "床のある階 " + std::to_string(storeys) + " に対して平面図 " +
                   std::to_string(found) + " 枚";
      }
    }
    out.rows.push_back(std::move(row));
  }

  // 様式1-2 別紙「図面リストの記載例」: each sheet with the kinds of documents it stands for.
  if (set)
    for (std::size_t i = 0; i < set->names.size(); ++i)
      out.drawingList.push_back({set->numbers[i], set->names[i], kindsOf[i]});

  // 様式1-2 sections 1, 2, 5 and 6: what the project outline says, as candidates for the boxes.
  const auto outlineValues = projectOutline(document);
  const auto value = [&](const char* key) {
    const auto it = outlineValues.find(key);
    return it == outlineValues.end() ? std::string() : it->second;
  };
  const auto add = [&](const char* group, const char* kind, std::string need, std::string state, std::string note) {
    out.rows.push_back({"様式1-2", group, kind, std::move(need), std::move(state), {}, std::move(note)});
  };
  add("１ 添付書類", "建築計画概要書（１部）", "必要", "アプリで出力", "確認申請のWord様式の第三号様式");
  {
    const std::string use = value("main_use"), zone = value("fire_zone");
    const bool house = contains(use, "一戸建") || contains(use, "長屋");
    if (use.empty() || zone.empty())
      add("１ 添付書類", "消防定型同意依頼書（１部）", "該当時", "要確認",
          "一戸建住宅（防火・準防火地域内にあるもの）、長屋の場合。主要用途・防火地域を計画概要に入れると候補を出す");
    else if (house && (zone == "fire" || zone == "quasi_fire" || contains(use, "長屋")))
      add("１ 添付書類", "消防定型同意依頼書（１部）", "必要（候補）", "アプリ外",
          "主要用途「" + use + "」・" + (zone == "fire" ? "防火地域" : zone == "quasi_fire" ? "準防火地域" : "長屋"));
    else
      add("１ 添付書類", "消防定型同意依頼書（１部）", "該当しない（候補）", "該当時に確認",
          "主要用途「" + use + "」と防火地域の指定から。様式の条件は一戸建住宅（防火・準防火地域内）・長屋");
  }
  add("１ 添付書類", "建築協定等手続状況届出書（１部）", "必要", "アプリで出力（第一面）",
      "第二面の手続の要否・状況は資料に基づいて記入");
  {
    const std::string agent = value("agent");
    if (agent.empty())
      add("２ 設計者等", "委任状（正本のみ）", "該当時", "該当しない（候補）", "計画概要に代理者がない");
    else
      add("２ 設計者等", "委任状（正本のみ）", "必要（候補）", "アプリ外",
          "代理者「" + agent + "」。委任状と申請書第２面の記載の整合を確認");
    const std::string supervisor = value("supervisor");
    add("２ 設計者等", "工事監理者の記載", "必要", supervisor.empty() ? "要確認" : "記載あり",
        supervisor.empty() ? "計画概要に工事監理者がない（未定の場合は未定と記載）" : "工事監理者「" + supervisor + "」");
  }
  {
    const std::string review = value("structural_review");
    const char* text = review == "submitted" ? "申請済" : review == "not_yet" ? "未申請" : review == "not_required" ? "申請不要" : "";
    add("５ 構造計算適用等", "構造計算適合性判定", "案件による", *text ? "記載あり" : "要確認",
        *text ? std::string("計画概要：") + text + "。特定木造建築物（仕様規定ルート）・ルート2・特定天井・土砂災害特別警戒区域は設計者が記入"
              : std::string("計画概要に構造計算適合性判定の申請の区分がない"));
    const std::string energy = value("energy_plan");
    add("６ 省エネ適判等", "省エネ適判", "案件による",
        energy == "submitted" || energy == "not_required" ? "記載あり" : "要確認",
        energy == "submitted" ? "計画概要：提出済。適合性判定通知書等の添付を確認"
        : energy == "not_required" ? "計画概要：提出不要。様式の「省エネ適判無（仕様基準に適合、住宅のみ）」か他制度の活用かを確認"
        : energy == "not_yet" ? "計画概要：未提出"
                              : "計画概要に省エネ計画の提出の区分がない");
  }

  // 様式3: the fee for a single new building whose whole floor area is the application part.
  IntakeFee& fee = out.fee;
  fee.note = "様式3（2026-10-29 掲載）の面積区分の表から。法第6条の3第1項ただし書の審査の加算（②）は含めない。"
             "横浜市の手数料のページ（最終更新 2026-09-30、令和7年4月1日改正の表）と12区分すべて一致することを確かめた"
             "（2026-11-27）。手数料条例の本文そのものとは照合していない";
  if (!wholeFloorForFee) {
    fee.state = "未選択";
  } else if (value("work_kind") != "new") {
    fee.state = "工事種別を確認";
  } else {
    const AreaTable area = areaTable(document);
    if (!applicationFloorAreaKnown(area)) {
      fee.state = "床面積を確認";
    } else {
      const double raw = area.legalTotal / 1e6;
      fee.area = std::round(raw * 100.0) / 100.0;
      fee.known = true;
      fee.fee = yokohamaConfirmationFee(fee.area, &fee.bracket);
      std::string rawBracket;
      yokohamaConfirmationFee(raw, &rawBracket);
      fee.state = rawBracket == fee.bracket ? "候補" : "区分の境界を確認";
    }
  }
  return out;
}

}  // namespace sbr::doc
