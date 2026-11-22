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
