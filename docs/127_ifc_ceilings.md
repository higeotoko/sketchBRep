# IFC に天井（IfcCovering）を書く（docs/123 の続き、2026-11-15）

[docs/123](123_ceilings.md)（天井を形として）の「まだ」（IFC の天井）。作業は Opus 5.5。
オーナーの指示：2026-11-15（「続けて」）。

## できること

| 項目 | 内容 |
|---|---|
| 天井板 | 「天井を作る」（`make_ceilings`）で作った板は、IFC で **IfcCovering（種類 CEILING）** になる（前は「その他の立体」） |
| 室とのつながり | その板の室（IfcSpace）と **IfcRelCoversSpaces** でつなぐ。ほかの BIM ソフトで「この室の天井」と分かる |
| 階 | 板の下の面の高さの階に入る |
| 数 | 書き出しのステータスに「天井 n」。AI の `export_ifc` は `ceilings` |

## まだやっていないこと

- 天井板の仕上げ（「PB t9.5」など）を、板そのものの属性として書くこと（今は室の `Pset_SpaceCoveringRequirements` の CeilingCovering）。
- 床の仕上げ・壁の仕上げを IfcCovering（FLOORING・CLADDING）として書くこと。
- ほかの BIM ソフトで開いて見ること（この PC に IFC を読むソフトが無い。docs/101 と同じ）。

## 確かめたこと

| 段階 | 内容 |
|---|---|
| 計算テスト済み | コア全 698 件中 691 件成功・7 件スキップ（ローカル）。IFC の家のテストに追加（`IfcExport.AHouseIsWrittenWithItsStoreysPartsAndRelations`）：室に天井を作る → IfcCovering 1 つ（.CEILING.、属性の数は IFC4 どおり 9）、体積が室の内法の面積 × 10、IfcRelCoversSpaces が室の IfcSpace とその天井をつなぐ、1FL に 1 回だけ入る、その他の立体の数は変わらない |
| GUI 自動確認済み | IFC 書き出しの画面テストが成功（ローカル） |
| CI | run 43711993245（push、`340f8ca`）：Linux 712 件成功、Windows 708 件＋3 件成功・GUI 237 件成功（7 件スキップ） |
| Windows 実機確認 | まだ（チェック表 12-10）。ほかの BIM ソフトで開くのは、オーナーの手元にソフトがあれば |

## 配布の記録

| 項目 | 値 |
|---|---|
| コード | `340f8ca`（IFC の天井 docs/127 と階段の側板 docs/128。Opus 5.5 で作業） |
| CI | run 43711993245（push）：Linux・Windows とも成功 |
| ZIP | `SketchBRep-windows-x64-340f8ca-local.zip`（Codex のチャットフォルダの `outputs\`） |
| サイズ | 72,069,004 bytes |
| SHA256 | `ac7779646f1a576153bccf94e185991a357c588b9c20d845b17326d88ad30d7e` |
| ZIP の検証 | `verify_portable_pads.py`・`verify_portable_mat.py` とも成功（docs/validation/2026-11-15-portable-340f8ca*.json） |
