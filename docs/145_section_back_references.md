# 断面図から平面図の図番を指す（docs/132 の続き、2026-11-16）

[docs/132](132_sections_in_the_set.md) の「まだ」（断面図の上に、平面図の図番を書くこと＝逆向きの参照）。作業は Opus 5.5。
オーナーの指示：2026-11-16（「とりあえず実装を進めて」）。

## できること

| 項目 | 内容 |
|---|---|
| 断面図の見出し | 図面一式の断面図の下の見出しが「A-A 断面図（切る位置：A-02・A-03） S=1:100」のように、**切る位置を描いた平面図の図番**を出す |
| 行き来 | 平面図の切る位置の記号に断面図の図番（docs/132）、断面図の見出しに平面図の図番。どちらからでも、もう一方の紙が分かる |

## 確かめたこと

| 段階 | 内容 |
|---|---|
| 計算テスト済み | `Schedules.TheSetHasSectionsMarkedOnThePlans` に追加：A-A・B-B どちらの断面図の中にも「切る位置：（1FL 平面図の図番）」の文字がある |
| CI | run 43812081468（push、`e885e98`）：Linux 727 件成功、Windows 723 件＋3 件成功・GUI 237 件成功（7 件スキップ） |
| Windows 実機確認 | まだ（チェック表 12-28） |

## 配布の記録

| 項目 | 値 |
|---|---|
| コード | `e885e98`（断面図から平面図の図番 docs/145 と建具の形 docs/146。Opus 5.5 で作業） |
| CI | run 43812081468（push）：Linux・Windows とも成功 |
| ZIP | `SketchBRep-windows-x64-e885e98-local.zip`（Codex のチャットフォルダの `outputs\`） |
| サイズ | 72,383,667 bytes |
| SHA256 | `041debbd7bc36c70c6b9e425fc3a5061cc0910cdedb7b8f397a94fa7932e0f93` |
| ZIP の検証 | `verify_portable_pads.py`・`verify_portable_mat.py` とも成功（docs/validation/2026-11-16-portable-e885e98*.json） |
