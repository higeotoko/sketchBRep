# 図面を LayOut（.layout）に書き出す（2026-11-18）

SketchUp 連携の案の 4。[docs/181](181_sketchup_skp.md)・[docs/182](182_sketchup_dlls_in_the_zip.md) と同じく、SketchUp SDK の LayOut の部分を使う。作業は Opus 5.5。
オーナーの指示：2026-11-18（「おすすめ順で」）。

## できること

| 場所 | 内容 |
|---|---|
| 図面の窓（1 枚） | 「書き出す」の形式に **LayOut (*.layout)**（LayOut の部分がある時だけ）。1 ページの LayOut 文書 |
| AI：`export_drawing` | パスが `.layout` で終わると LayOut 文書に |
| AI：`export_drawing_set` | `format: "layout"` で、図面一式を **1 つの LayOut 文書**（`<接頭>drawings.layout`）に。1 枚 = 1 ページ、ページの名前は図番と図面名（例「A-02 平面図」） |

中身：線・折れ線・円弧・塗り（切り口の灰色）・文字を、紙の上の大きさのまま置く。線の太さと線種（実線・破線・一点鎖線）はペンごと（DXF・PDF と同じ `penWidth`・`penDashes`）。枠・表題欄・方位記号・通り芯・表もそのまま。日本語の文字も出る。

## 作り

- `src/core/src/DrawingLayout.cpp`：`draw::writeLayout(sheets, names, path)`・`layoutAvailable`・`layoutPageCount`・`layoutToPdf`（確かめ用）。SketchUp と同じく、LayOut の関数を名前で引く（リンクしない、ヘッダは Git に入れない）。
- DLL を探す順：exe の隣の `layout` フォルダ → 環境変数 `SBR_SKETCHUP_SDK` → ビルドした時の SDK。
- ZIP：SDK の `binaries\layout\x64` の DLL を全部、`layout` フォルダに入れる（SketchUp の DLL と同じ名前で版の違う物があるので分けた）。

## 決めたこと

| 項目 | こうした | 理由 |
|---|---|---|
| 図面一式 | 1 文書・複数ページ | LayOut は 1 文書に何ページも持つのがふつう。DXF・JWW は 1 枚 1 ファイルのまま |
| 紙の大きさ | 文書で 1 つ（いちばん大きい紙） | LayOut の紙の大きさは文書ごと。A4 と A3 が混ざると、A4 の図は A3 の紙の左下に置かれる |
| 文字の大きさ | 図面の文字の高さ（mm）× 1.4 をポイントに | 図面の文字の高さは字の高さで、LayOut はフォントの大きさのため |
| ZIP の大きさ | LayOut の DLL（約 150 MB、圧縮で増える分は配布の記録を見る）を入れる | オーナーの決定（SDK の DLL は ZIP に入れてよい、2026-11-18）。どの PC でも使えるように |

## まだやっていないこと

- 文字の向き（回した文字は横向きのまま）。
- 塗りの穴（穴のある切り口は穴の上も塗る。線は正しい）。
- LayOut の中に SketchUp のモデル（3D の図）を置くこと。寸法を LayOut の寸法として置くこと（今は線と文字）。
- LayOut のレイヤ（ペンごとに分けること）。

## 確かめたこと

| 段階 | 内容 |
|---|---|
| 計算テスト済み | コア全 757 件中 749 件成功・8 件スキップ（ローカル、SDK あり）。新規 1 件（`Layout.DrawingsAsLayOutPages`）：外壁 サイディングの部屋に窓 → `export_drawing` で `.layout`（1 ページ）、`export_drawing_set` で LayOut 1 文書（ページ数が図面の枚数と同じ、5 枚以上）、LayOut 自身に読ませて PDF に書き出せる |
| AI が配ったアプリで確認 | テストで LayOut が書いた PDF（11 ページ、A4 横）を絵にして見た：図面リスト（表・日本語）、平面図（壁の太線・切る位置 A・B・方位記号の円）、断面図、立面図、建具表、どのページにも枠と表題欄（図番・図面名・縮尺） |
| GUI 自動確認済み | 全部の画面テストで、この PC だけの線ツール 1 件・クリップボード 2 件と、まとめて走らせた時だけのフォローミー 2 件が失敗（フォローミーは単独で 4 条件とも成功）。図面の窓の形式の一覧はテストしていない |
| CI | （記録待ち） |
| Windows 実機確認 | まだ（チェック表 13-5） |

## 配布の記録

（記録待ち）
