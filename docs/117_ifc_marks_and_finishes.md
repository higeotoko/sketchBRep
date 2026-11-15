# IFC に符号・品番・用途・仕上げを書く（G-13 の続き、2026-11-15）

[docs/101](101_ifc_export.md)（IFC の書き出し）に、[docs/104](104_schedules_and_tags.md)・[docs/109](109_opening_marks_and_products.md)・[docs/115](115_room_finishes.md) で持てるようになった物を足した。作業は Opus 5.5。
オーナーの指示：2026-11-15（「続けて」）。

## 書く物

| モデルの物 | IFC の所 |
|---|---|
| 開口の符号（手で決めた物、無ければ自動の W1・D1） | 窓・扉（IfcWindow・IfcDoor）の **Name** |
| 品番 | 窓・扉の **Description** |
| 室の用途 | 室（IfcSpace）の **ObjectType** |
| 床・壁・天井の仕上げ | 室のプロパティセット **Pset_SpaceCoveringRequirements** の FloorCovering・WallCovering・CeilingCovering |
| 天井高 | 室の数量 **Qto_SpaceBaseQuantities** の FinishCeilingHeight |

- 入れていない物は書かない（空のプロパティを作らない）。
- 前は窓・扉の Name が「Window」「Door」だった。今は符号になる（建具表・平面の符号と同じ）。

## 確かめていないこと

- **ほかの BIM ソフトで開いて、これらが見えるか**はまだ（docs/101 と同じ。この PC に読むソフトが無い）。
- `Pset_SpaceCoveringRequirements` の項目名は、IFC4 の決まりを記憶で書いている。原文（buildingSMART の IFC4 の文書）との照合はまだ。

## 確かめたこと

| 段階 | 内容 |
|---|---|
| 計算テスト済み | IFC の書き出しのテストに追加：扉に手で符号 SD-1・品番 P-900 → IfcDoor の Name が 'SD-1'、Description が 'P-900'／窓は自動の 'W1'、Description は無し／室に用途 Living・床・天井・天井高 2400 → IfcSpace の ObjectType が 'Living'、FloorCovering・CeilingCovering が入った Pset_SpaceCoveringRequirements、FinishCeilingHeight 2400。ファイルの形の検査（エンティティごとの引数の数）も通る |
| GUI 自動確認済み | IFC の書き出しの画面テストが成功（ローカル）。新しい画面の操作は無い |
| CI | run 43689629680（push、`13dfde1`）：Linux 703 件成功、Windows 699 件＋3 件成功・GUI 236 件成功（7 件スキップ） |
| AI が実物のアプリで確認 | 配った ZIP（`13dfde1`）の `sbr_cli` で作った家の IFC の中身を読んだ：窓・扉の Name が AW-1・D2・W1・D1、AW-1 の Description に品番、室の ObjectType に用途、仕上げ 3 つ、天井高 2400（docs/118） |
| Windows 実機確認 | まだ（ほかの BIM ソフト・ビューアで開いて、窓・扉の名前と室の仕上げが見えるか） |

## 配布の記録

| 項目 | 値 |
|---|---|
| コード | `13dfde1`（Opus 5.5 で作業） |
| CI | run 43689629680（push）：Linux・Windows とも成功 |
| ZIP | `SketchBRep-windows-x64-13dfde1-local.zip`（Codex のチャットフォルダの `outputs\`） |
| サイズ | 71,875,072 bytes |
| SHA256 | `95bb1fe373ba035903308bd4fdef66605ab483777e6f3f2890de7b61cc61d399` |
| ZIP の検証 | `verify_portable_pads.py`・`verify_portable_mat.py` とも成功（docs/validation/2026-11-15-portable-13dfde1*.json） |
