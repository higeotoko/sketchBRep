# 階段の段鼻の出（docs/135 の続き、2026-11-16）

[docs/135](135_stair_cut_stringer_risers_newels.md) の「まだ」（段鼻の出）。作業は Opus 5.5。
オーナーの指示：2026-11-16（「自動で進んでいっていい」）。

## できること

| 項目 | 内容 |
|---|---|
| 段鼻の出 | 各段の踏板（段々の塊では段）が、**前の縁から前へ**出る長さ。0〜50（初めは 0） |
| どこで | 階段のプロパティ「段鼻の出」。AI は `create_stair`・`change_stair` の `nosing`、`stair_info` にも出る |
| 前の縁 | 歩く線でいちばん下側の辺（蹴込み板と同じ決め方）。回り段の三角の段も、入ってくる側の辺が前に出る |
| 法の照合 | 踏面（踏み幅）は、出を入れずに測る（平面の段の形のまま）。照合の値は変わらない |
| 蹴込み板 | 前の縁のまま。段鼻が蹴込み板の前に出る |
| 保存 | ファイルに残る。前のファイルの階段は 0 |

## まだやっていないこと

- 段鼻の形（面取り・丸め）。
- 平面図に段鼻の線を描くこと（今は段の線のまま）。

## 確かめたこと

| 段階 | 内容 |
|---|---|
| 計算テスト済み | コア全 708 件中 701 件成功・7 件スキップ（ローカル）。新規 1 件（`Stair.ANosingRunsOnPastTheFrontEdge`）：踏板の階段（13 段）に 20 → 体積が 13 × 20 × 800 × 30 増える、いちばん下の段が足元より 20 前（X −20）、踏面の最小・最大は同じ／蹴込み板と一緒でも 13 枚／段々の塊・回り段でも作れる／80 と −1 は断る。`Stairs.AHandrailIsChangedOnTheStairAndKept` に追加：`change_stair` で 20 → `stair_info` に出る |
| GUI 自動確認済み | 階段の画面テストが成功（ローカル）。プロパティの欄を押す画面テストは足していない |
| CI | run 43738309397（push、`a7aebbd`）：Linux 721 件成功、Windows 717 件＋3 件成功・GUI 237 件成功（7 件スキップ） |
| Windows 実機確認 | まだ（チェック表 12-20） |

## 配布の記録

| 項目 | 値 |
|---|---|
| コード | `a7aebbd`（Opus 5.5 で作業） |
| CI | run 43738309397（push）：Linux・Windows とも成功 |
| ZIP | `SketchBRep-windows-x64-a7aebbd-local.zip`（Codex のチャットフォルダの `outputs\`） |
| サイズ | 72,217,541 bytes |
| SHA256 | `3f11c71a3fdf3ad2569681a17580d3f7aa21418e675877f71c1cbf6d9d0c1589` |
| ZIP の検証 | `verify_portable_pads.py`・`verify_portable_mat.py` とも成功（docs/validation/2026-11-16-portable-a7aebbd*.json） |
