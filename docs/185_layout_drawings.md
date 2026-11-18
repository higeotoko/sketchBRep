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
- ZIP：**LayOut の DLL は入れない**（2026-11-18 オーナーの決定「一旦外しちゃっていいや」）。66957ee の ZIP だけ `layout` フォルダに入っていた（81 → 130 MB）。今の線と文字だけの書き出しは、DXF を LayOut で読み込むのと大差ないため。LayOut の書き出しは SDK のある PC（環境変数 `SBR_SKETCHUP_SDK` かビルドした時の SDK）だけで動き、無い PC では図面の窓の形式の一覧に出ない（AI は断る）。
- LayOut の DLL は、自分のお知らせ（グラフィックスカードの名前など）を標準出力に書く。CLI と MCP は標準出力に JSON で答えるので、LayOut を動かす間だけ標準出力を標準エラーに回す（`QuietStdout`、`4c79ce3`）。作った版の `sbr_cli` で、標準出力が JSON だけになり、お知らせは標準エラーに出ることを確かめた。

## 決めたこと

| 項目 | こうした | 理由 |
|---|---|---|
| 図面一式 | 1 文書・複数ページ | LayOut は 1 文書に何ページも持つのがふつう。DXF・JWW は 1 枚 1 ファイルのまま |
| 紙の大きさ | 文書で 1 つ（いちばん大きい紙） | LayOut の紙の大きさは文書ごと。A4 と A3 が混ざると、A4 の図は A3 の紙の左下に置かれる |
| 文字の大きさ | 図面の文字の高さ（mm）× 1.4 をポイントに | 図面の文字の高さは字の高さで、LayOut はフォントの大きさのため |
| ZIP | LayOut の DLL は入れない | オーナーの決定（2026-11-18）。SketchUp との行って来いを先にする。SketchUp のビューを置く・寸法など、LayOut ならではの所まで作る時に、また考える |

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
| CI | run 44278615114（push、`54e5c3e`）：Linux 773 件成功、Windows 769 件＋3 件成功・GUI 238 件成功（7 件スキップ）。CI には SketchUp SDK が無いので、SKP・LayOut のテストはスキップ |
| Windows 実機確認 | まだ（チェック表 13-5） |

## 配布の記録

| 項目 | 値 |
|---|---|
| コード | `66957ee`・`4c79ce3`（LayOut、お知らせを標準出力からよける）。ZIP から LayOut の DLL を外したのは `7a7a009`。Opus 5.5 で作業。まとめて配ったのは `54e5c3e` |
| CI | run 44278615114（push、`54e5c3e`）：Linux・Windows とも成功 |
| ZIP | `SketchBRep-windows-x64-54e5c3e-local.zip`（Codex のチャットフォルダの `outputs\`）。SketchUp SDK の DLL 2 つ入り（LayOut は入れていない） |
| サイズ | 81,771,001 bytes |
| SHA256 | `e5cd445e41f9fc7169e1b5fedc60f1ae6c4b0afd431e38ee8806a56cc92ae56a` |
| ZIP の検証 | `verify_portable_pads.py`・`verify_portable_mat.py` とも成功（docs/validation/2026-11-18-portable-54e5c3e*.json） |
