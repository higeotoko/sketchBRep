# 通り芯から外す（docs/122・129 の続き、2026-11-16）

[docs/122](122_pin_to_grid_later.md)・[docs/129](129_walls_on_the_grid.md) の「まだ」（付けた物を外すこと。今は取り消しだけ）。作業は Opus 5.5。
オーナーの指示：2026-11-16（「とりあえず実装を進めて」）。

## できること

| 項目 | 内容 |
|---|---|
| どこで | 階のボタン →「通り芯から外す（選んだ物、無ければ全部）」。AI は `unpin_from_grid {ids}` |
| 外れる物 | 通り芯に付いた柱・梁など（`place_members_on_grid`・後から付けた物）と、通り芯に付けた壁 |
| 外した後 | その場に残る。通り芯の間隔を変えても、いっしょに付けたレベルを変えても、付いてこない |
| 取り消し | 1 回 |
| 断る時 | 付いている物が 1 つも無い時（「通り芯に付いている物がありません」） |

鍵のかかった物は外さない（そのまま）。

## 確かめたこと

| 段階 | 内容 |
|---|---|
| 計算テスト済み | `GridPinLater.WallsOnTheGridFollowItsSpacings` に追加：東の壁だけ外す → 取り消し 1 回、属性が消える、X3 を動かしても東の壁は 7280 のまま／取り消すと付いた状態に戻る／付けていない壁を外すのは断る。`GridPinLater.MembersDrawnBeforeFollowTheGridOncePinned` に追加：全部外す → 2 本、柱の付き先が消える、もう一度は断る |
| GUI 自動確認済み | （メニューを 1 つ足しただけ。押す画面テストは足していない） |
| CI | run 43809475149（push、`64b51ef`）：Linux 726 件成功、Windows 722 件＋3 件成功・GUI 237 件成功（7 件スキップ） |
| Windows 実機確認 | まだ（チェック表 12-27） |

## 配布の記録

| 項目 | 値 |
|---|---|
| コード | `64b51ef`（らせん階段を階段ツールで docs/143 と通り芯から外す docs/144。Opus 5.5 で作業） |
| CI | run 43809475149（push）：Linux・Windows とも成功 |
| ZIP | `SketchBRep-windows-x64-64b51ef-local.zip`（Codex のチャットフォルダの `outputs\`） |
| サイズ | 72,359,043 bytes |
| SHA256 | `16e94fc214ebea5d5d8ad9a8ff2c137301088b4dcd5e72c9f1d41287bf025c1a` |
| ZIP の検証 | `verify_portable_pads.py`・`verify_portable_mat.py` とも成功（docs/validation/2026-11-16-portable-64b51ef*.json） |
