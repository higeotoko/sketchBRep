# 269. 壁と床のIFC材料層を外部ソフトで確認（2026-11-23）

[チェック表](105_hands_on_checklist.md)の12-47。例題AのIFC4を IfcOpenShell 0.9.0.post1 で開き、部材IDでSketchBRepの元モデルと照合した。壁12枚と床2枚で、材料名・厚さ・並び順が一致した。 [例題Aの書き出し記録](267_ifc_export_menu_check.md)

| 部材 | 確認した並び | 合計 |
|---|---|---|
| 外壁・窯業系サイディング 8枚 | サイディング 16 → 通気層 18 → 構造用合板 9 → 柱・間柱 105 → 石膏ボード 12.5 mm | 160.5 mm |
| 内壁・石膏ボード両面 4枚 | 元モデルの3層と一致 | 130 mm |
| 床 F1 2枚 | フローリング 15 → 合板 24 → 床梁 105 mm。上から下へ `AXIS3 / NEGATIVE` | 144 mm |

## 壁の外側

例題Aの外壁8枚は、元モデルでは描いた線に対して外側が左。IFCはサイディングを先頭にし、`AXIS2 / NEGATIVE` とオフセット +95.5 mm を記録していた。壁の形状表現は `Body` のみで、壁の基準線 `Axis` がない。南北に延びる外壁4枚では、IFCのローカルY軸が壁の長手方向を向き、厚さ方向と一致しない。外部ビューアで外面から正しく表示できるかは未確認のため、12-47は一部確認として記録した。 [IFC書き出しの設計](170_layers_in_ifc.md) · [IfcMaterialLayerSetUsage仕様](https://standards.buildingsmart.org/IFC/RELEASE/IFC4/ADD2_TC1/HTML/schema/ifcmaterialresource/lexical/ifcmateriallayersetusage.htm)

部材ごとの確認結果は配布フォルダの `outputs/ifc-layer-review/layer-review.json`。

## 基準線と厚さ方向の修正後

B-07（他のBIMへ渡せること）の確認。壁を描いた線をIFCの `Axis` として出力し、配置のローカルXを壁に沿う方向、ローカルYを左側に向く厚さ方向にした。壁のBrepと開口をこの配置の座標に変換してから書き出す。 [IFC4の壁の座標と軸](https://standards.buildingsmart.org/IFC/RELEASE/IFC4/ADD2_TC1/HTML/schema/ifcsharedbldgelements/lexical/ifcwall.htm)

IfcOpenShellで例題Aの壁12枚の基準線と材料層を照合した。外壁8枚すべてでサイディングの外面がBrepの外面に一致した。壁12枚と開口14件の世界座標の外接範囲、壁の層の名前・厚さ・順番は修正前のIFCと一致した。検証データは配布フォルダの `outputs/ifc-axis-and-fitting-check/result.json`。BIMビューアでの材料層の表示は未確認。

例題Bでも、壁21枚の世界座標での外接範囲を修正前後で照合した。斜めの壁4枚に基準線が入り、壁の外接範囲は0.01 mm以内で一致した。

## 配布版の検証

Windowsコアは810件成功・8件スキップ。IFCのコア4件と書き出し画面のGUIテストも成功した。展開したZIPのCLIで例題Aを再出力し、壁と開口の位置、材料層、建具14件の形状を再確認した。人の操作による確認は未実施。 [通常のZIP検証18条件](validation/2026-11-23-portable-4dbf133.json)・[べた基礎のZIP検証4条件](validation/2026-11-23-portable-4dbf133-mat.json)

| 項目 | 値 |
|---|---|
| コード | `4dbf133` |
| Linux CI | 831件すべて成功。[run 45122801960](https://github.com/higeotoko/sketchBRep/actions/runs/45122801960) |
| ZIP | `SketchBRep-windows-x64-4dbf133-local.zip`（配布フォルダの `outputs/`） |
| サイズ | 83,313,500 bytes |
| SHA256 | `68474fac292ea2061e7a3438d40261d689bce7b65d597498aef917d32679ccc6` |
