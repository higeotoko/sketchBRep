# 291. 使用構造材料一覧表

様式2（構造受付チェックシート）表2 の「使用構造材料一覧表」にあたる表を、木造架構の部材と、設計者が入れた材料・等級から作る。docs/285「ここから先」の 3 の一部。

## 入力

部材の役割ごとに、材料（樹種等）・等級・規格・資料を入れる。役割は施工ガイドの木造の役割（sill、through_column、column、stud、floor_beam、brace、rafter など）。`default` は、入れていない役割すべてに使う。全体を入れ替える形で、取り消しは1回。モデルの設定 `structuralMaterials` に保存する。

- AI：`set_structural_materials {items: [{role, material, grade, source}]}`。樹種や等級を推測で入れない。
- 同じ役割を2回入れた時、役割名が違う時、材料が空の時は受け付けない。

## 表

拾い出し（`take_off`）の行を役割と断面でまとめ、部位・断面（mm）・数・材料・等級・規格・資料を出す。並びは土台→大引→…→垂木（施工ガイドの役割の順）。材料が入っていない部位は「未入力」とし、未入力の行数を注に書く。品質の確認はしていない。

- 図面一式では、木造架構の部材がある時に「使用構造材料一覧表」の表の図面を入れる。
- AI：`structural_material_table`（読み取りのみ）。行・注・入力値・CSV。
- 受付チェックシートの照合（docs/286）で、様式2 表2「使用構造材料一覧表」が「図面あり」になる。

## 残り

接合金物の一覧、含水率・防腐防蟻の仕様、構造用合板等の面材の規格の欄。


## 検証と配布

docs/291 と docs/292 はまとめて検証した。人による操作確認は未実施。

| 項目 | 結果 |
|---|---|
| コード | `5114891` |
| Windows | コア838件成功・8件スキップ、関連GUIテスト26条件成功 |
| Linux CI | [run 45778833241](https://github.com/higeotoko/sketchBRep/actions/runs/45778833241) で859件成功 |
| Windows ZIP | `SketchBRep-windows-x64-5114891-local.zip`（84,407,795 bytes） |
| SHA256 | `9d6febddaddab9ba86ac8b66c15134af02f49b12594d79e7b1e021fb6eccc55b` |
| ZIPの検証 | [標準18条件](validation/2026-11-26-portable-5114891.json)・[べた基礎4条件](validation/2026-11-26-portable-5114891-mat.json)成功 |
