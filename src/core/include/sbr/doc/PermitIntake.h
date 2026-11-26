#pragma once

// docs/286: the Yokohama intake forms (受付チェックシート 様式1-2 / 様式2, 手数料算定表 様式3) held up
// against what the model gives. What is found or missing only: the sheets' "確認した" boxes stay the
// designer's, and nothing here is a legal judgement.

#include "sbr/doc/Schedules.h"

#include <optional>
#include <string>
#include <vector>

namespace sbr::doc {

inline constexpr const char* kYokohamaIntakeForms =
    "https://www.city.yokohama.lg.jp/business/bunyabetsu/kenchiku/tetsuduki/kakunin/dl/dl.html";
// The forms as read: 様式1-2 0931_20261029.docx, 様式2 0846_20250330.docx, 様式3 0932_20261029.docx
// (taken from the page above on 2026-11-26).
inline constexpr const char* kYokohamaIntakeVersion = "横浜市 受付書式（様式1-2・様式3 は 2026-10-29 掲載、様式2 は 2025-03-30 掲載、2026-11-26 取得）";

struct IntakeRow {
  std::string form;     // 様式1-2 / 様式2
  std::string group;    // 表1（い） etc., as the form groups it
  std::string kind;     // 図書の種類 as the form names it
  std::string need;     // 必要 / 該当時 / 特定木造建築物を除く / 構造の申請ルートによる
  std::string state;    // 図面あり / 要確認 / 図面一式にない / 該当時に確認 / アプリ外
  std::vector<std::string> sheets;  // "A-04 1FL 平面図", ...
  std::string note;
};

struct IntakeFee {
  // 申請部分の床面積 (申請書第三面 11 欄イ) when it can be read and the person said the whole floor
  // area of a single new building is the application part.
  bool known = false;
  std::string state;     // 未選択 / 工事種別を確認 / 床面積を確認 / 候補 / 区分の境界を確認
  double area = 0.0;     // m2, as written on the form (2 decimals)
  std::string bracket;   // e.g. "30㎡を超え100㎡以下"
  int fee = 0;           // yen (①), 0 when not known
  std::string note;
};

struct IntakeDrawing {
  std::string number, name;
  std::vector<std::string> kinds;  // 様式1-2 図書の種類 the sheet's name stands for
};

struct IntakeReview {
  std::vector<IntakeRow> rows;
  std::vector<IntakeDrawing> drawingList;  // 様式1-2 別紙 図面リストの記載例
  IntakeFee fee;
  std::string drawingError;  // when the drawing set could not be made
  std::string source = kYokohamaIntakeVersion;
};

// `set`: the model's drawing set (drawingSet()); nullopt when it could not be made (`setError`).
// `wholeFloorForFee`: the person's choice that a single new building's whole floor area is the
// application part (the same choice as the submission review's check box).
IntakeReview yokohamaIntakeReview(const Document& document, const DrawingSet* set, const std::string& setError,
                                  bool wholeFloorForFee);

// The fee for the application area on 様式3 (新築・増築・改築: the area itself). 0 when area <= 0.
int yokohamaConfirmationFee(double squareMetres, std::string* bracket = nullptr);

// The drawing list in the form of 様式1-2's 別紙 (図面番号, 図面名, 図書の種類, 設計者 left blank).
draw::Table intakeDrawingList(const IntakeReview& review);

// Whether the area table gives the application floor area (each storey read, no walls left unread).
bool applicationFloorAreaKnown(const AreaTable& area);

}  // namespace sbr::doc
