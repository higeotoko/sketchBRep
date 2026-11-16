# IFC で庇を床板の一種として書く（docs/147 の続き、2026-11-16）

[docs/147](147_canopies.md)・[docs/154](154_canopy_sizes.md) の「まだ」（IFC で庇を屋根・板の種類として書くこと。今までは「その他の立体」）。作業は Opus 5.5。
オーナーの指示：2026-11-16（「続けて」）。

## できること

| 項目 | 内容 |
|---|---|
| 書き方 | 庇（`kind = canopy`）を `IfcSlab`（種類 `.USERDEFINED.`、ObjectType `'CANOPY'`）で書く。数量は `Qto_SlabBaseQuantities`（厚さ・体積） |
| 壁とのつながり | 庇と、それが出ている壁を `IfcRelConnectsElements` でつなぐ |
| 数 | `export_ifc` の答えに `canopies`。床（`slabs`）とその他（`proxies`）には数えない |

## 決めたこと

| 項目 | こうした | 理由 |
|---|---|---|
| 何の要素にするか | `IfcSlab` の USERDEFINED・`CANOPY` | IFC4 には庇の決まった要素が無い。形は板（スラブと同じ作り）なので床板の一種とし、種類名で庇と分かるようにした。`IfcShadingDevice`（日よけ）・`IfcRoof` も考えたが、庇は日よけだけの物ではなく、屋根として数えると屋根の数・面積が変わる |
| 種類名 | 英字の `CANOPY` | ほかのソフトで読みやすい。名前（Name）は日本語の物の名前のまま |

## まだやっていないこと

- ほかの BIM ソフト・ビューアで開いて見ること（IFC 全体でまだ、チェック表 7-2）。
- 床・屋根の層を IFC（`IfcMaterialLayerSet`）に書くこと。

## 確かめたこと

| 段階 | 内容 |
|---|---|
| 計算テスト済み | コア全 718 件中 711 件成功・7 件スキップ（ローカル）。`IfcExport.AHouseIsWrittenWithItsStoreysPartsAndRelations` に追加：扉に庇を付けた家 → `canopies` 1、その他（proxies）は前のまま 1、`IFCSLAB` 2 つ（床と庇）、`IFCRELCONNECTSELEMENTS` 1 つ、文の中に `'CANOPY'` と `.USERDEFINED.)`。どの要素も IFC4 の属性の数どおり（`IfcRelConnectsElements` は 7） |
| GUI 自動確認済み | 画面は変えていない |
| CI | __CI__ |
| Windows 実機確認 | まだ（チェック表 7-3。ビューアで庇が床板として、壁とつながって見えるか） |

## 配布の記録

__DELIVERY__
