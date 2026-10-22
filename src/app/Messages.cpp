#include "Messages.h"

#include <QObject>
#include <QRegularExpression>

namespace sbr::app {
namespace {

struct Entry {
  const char* core;   // substring of the core message
  const char* user;   // explanation, ideally with the next step
};

// Ordered: more specific phrases first.
const Entry kEntries[] = {
    {"Cancelled by the user", "計算を中止しました"},
    {"Operation would split the solid", "この操作ではソリッドが分断されてしまいます。距離を小さくするか、貫通させたい場合は開口として面に描いた形を押し込んでください"},
    {"Push/pull requires a valid solid", "ソリッドが閉じていないためプッシュ/プルできません"},
    {"Only planar faces of profiles can be pushed/pulled", "曲面のプッシュ/プルはソリッドの面だけでできます"},
    {"Only planar faces can be pushed/pulled", "この面はプッシュ/プルできません"},
    {"Distance must be non-zero", "距離が 0 です。マウスで方向を示すか、0 以外の値を入力してください"},
    {"Push/pull distance must be non-zero", "距離が 0 です"},
    {"Extrusion distance must be non-zero", "押出距離が 0 です"},
    {"Component instances cannot be pushed/pulled", "コンポーネントは直接プッシュ/プルできません。定義の寸法をパネルで変えるか、分解してから編集してください"},
    {"Referenced face is ambiguous", "編集の基準にしていた面が、前の編集で 2 つ以上に分かれ、どれのことか決められません（違う面を編集しないよう中止しました）。面を選び直してください"},
    {"Referenced curved face is ambiguous", "編集の基準にしていた曲面がどれか決められません（違う面を編集しないよう中止しました）。面を選び直してください"},
    {"Referenced face no longer exists (topology changed by an earlier edit)", "編集の基準にしていた面が、前の編集で形が変わったため見つかりません（取り違えて別の面を編集しないよう中止しました）。面を選び直してください"},
    {"Referenced face no longer exists", "編集の基準にしていた面が見つからなくなりました（取り違えて別の面を編集しないよう中止しました）。面を選び直してください"},
    {"Referenced curved face no longer exists", "編集の基準にしていた曲面が見つからなくなりました。面を選び直してください"},
    {"The loop is not planar", "輪郭が同じ平面上にありません（曲面はロフトなどで作れます）"},
    {"The loop crosses itself", "輪郭が自分自身と交差しています"},
    {"Polygon points are not coplanar", "点が同じ平面上にないため面にできません"},
    {"Polygon is invalid (self-intersecting?)", "輪郭が自己交差しているため面にできません"},
    {"Polygon has zero area", "面積が 0 です"},
    {"The line must run from edge to edge of the face", "面を分割するには、線を面の辺から辺まで引いてください"},
    {"The line leaves the face", "線が面からはみ出しています"},
    {"The line does not divide the face", "この線では面が分かれません"},
    {"The line does not cross a face from edge to edge", "面を辺から辺まで横切る線ではありません"},
    {"Rectangle dimensions must be non-zero", "幅と奥行きは 0 にできません"},
    {"Circle radius must be positive", "半径は正の値にしてください"},
    {"Nothing is hidden", "非表示の物はありません（タグで隠した物はタグ一覧で表示を切り替えてください）"},
    {"Components cannot be scaled", "コンポーネントは拡大縮小できません。定義の寸法を変えるか、分解してから拡大縮小してください"},
    {"Scale factor must be positive", "倍率は正の値にしてください"},
    {"A polygon needs 3 to 256 sides", "多角形の辺の数は 3〜256 にしてください"},
    {"Polygon radius must be positive", "半径は正の値にしてください"},
    {"Grid spacings must be positive", "通り芯の間隔は正の値にしてください"},
    {"There are no guides", "削除するガイドがありません（通り芯は「Grid」タグで表示を切り替えるか、選択して削除してください）"},
    {"Arc is flat", "円弧の膨らみが 0 です"},
    {"Arc ends coincide", "円弧の始点と終点が同じです"},
    {"Dimension points coincide", "寸法の 2 点が同じ位置です"},
    {"Text must not be empty", "文字が空です"},
    {"Name must not be empty", "名前が空です"},
    {"Offset produced an invalid face", "このオフセット距離では輪郭がつぶれてしまいます"},
    {"Faces with holes cannot be offset yet", "穴のある面はまだオフセットできません"},
    {"Boolean operands must be solids", "ソリッド演算はソリッド同士でのみできます（開いた形状は穴をふさいでから）"},
    {"Select at least two solids", "ソリッドを 2 つ以上選んでください"},
    {"Boolean result is empty", "演算結果が空になります（重なっていない可能性があります）"},
    {"Objects must be in the same group", "異なるグループの物はまとめられません"},
    {"Nested components are not supported yet", "コンポーネントの入れ子はまだ対応していません"},
    {"Sweep path must be a curve", "スイープの経路には線か曲線を選んでください"},
    {"Loft needs at least 2 sections", "ロフトには断面を 2 つ以上選んでください"},
    {"Nothing selected", "何も選択されていません"},
    {"Nothing to export", "書き出す物がありません"},
    {"Unknown parameter", "その寸法は見つかりません"},
    {"would overlap", "既にある開口と重なります。位置か大きさを変えてください（穴がつながって 1 つになるのを防いでいます）"},
    {"The openings do not fit along the wall", "壁の長さに収まりません。個数・幅・間隔を小さくしてください"},
    {"The opening does not fit on the wall there", "その位置では壁からはみ出します。壁の内側寄りに置くか、幅を小さくしてください"},
    {"The openings are taller than the wall", "腰の高さ＋開口の高さが壁の高さを超えています"},
    {"Openings go into a vertical face", "開口は壁の縦の面に入れてください"},
    {"No face of this object faces that way", "その向きの面がありません"},
    {"The opening did not cut the wall", "壁を貫通できませんでした"},
    {"Opening sizes must be positive", "開口の幅・高さは正の値、腰は 0 以上にしてください"},
    {"This object has no rectangular openings", "この物には開口がありません"},
    {"A slab needs at least three corners", "スラブには角が 3 つ以上必要です"},
    {"The slab outline has no area", "スラブの輪郭の面積が 0 です"},
    {"Slab thickness must be positive", "スラブの厚さは正の値にしてください"},
    {"This edge is a corner of the shape", "この辺は形の角なので消せません（形が壊れるため）。部材ごと消すには面をクリックしてください"},
    {"This line cannot be erased on its own", "この線だけは消せません（面を分けた線ではないか、後の編集がこの線に頼っています）"},
    {"The other part of this face is gone", "分けたもう片方の面が押し出し・削除されているため、一枚に戻せません"},
    {"Open shapes cannot be pushed/pulled", "開いた形状（面を消したもの）はプッシュ/プルできません。編集 > 穴をふさぐ でソリッドに戻してください"},
    {"A hole is not flat", "平らでない穴は一枚の面でふさげません"},
    {"There are no holes to fill", "ふさぐ穴がありません"},
    {"Select an open shape", "穴をふさぐ開いた形状（面を消したもの）を選んでください"},
    {"Solids always face outwards", "ソリッドの面はいつも外向きです。反転できるのは開いた形状と単独の面です"},
    {"Nothing would be left", "すべての面は消せません（部材ごと消すには部材を選んで削除）"},
    {"Only faces of solids and open shells", "面だけを消せるのはソリッドと開いた形状です"},
    {"is not a solid", "ソリッドではありません（開いた形状は 編集 > 穴をふさぐ でソリッドに戻せます）"},
    {"Object not found", "対象が見つかりません"},
};

bool hasJapanese(const QString& s) {
  for (const QChar c : s) {
    if (c.unicode() >= 0x3040) return true;
  }
  return false;
}

}  // namespace

QString userMessage(const QString& text) {
  QString out = text;
  bool known = false;
  for (const Entry& e : kEntries) {
    const QString core = QString::fromUtf8(e.core);
    if (out.contains(core)) {
      out.replace(core, QObject::tr(e.user));
      known = true;
      break;
    }
  }
  // Recipe step prefix ("Step 2: ...") -> which step failed.
  static const QRegularExpression step(QStringLiteral("Step (\\d+): "));
  out.replace(step, QObject::tr("手順 \\1 で: "));
  if (known || !hasJapanese(text)) {
    if (!out.contains(QObject::tr("モデルは変更されていません"))) {
      out += QObject::tr("（モデルは変更されていません）");
    }
  }
  return out;
}

}  // namespace sbr::app
