# 268. 例題AのIFCを外部ソフトで確認（2026-11-23）

[7-1の書き出しで作った例題AのIFC4](267_ifc_export_menu_check.md)を IfcOpenShell 0.9.0.post1 で開き、世界座標で取り出した形状を Blender 5.3.0 で表示した。確認用の表示では庇を橙色にし、1階の画像では上階・屋根・天井を隠した。[IfcOpenShellの形状処理](https://docs.ifcopenshell.org/ifcopenshell-python/geometry_processing.html)

## 7-2 階・部品・建具符号・室の仕上げ

- 1FL（0 mm）と2FL（2,800 mm）を読み取れた。壁12、開口14、床2、屋根1、階段1、室6、天井6の形状が得られた。全景と1階の画像で屋根、外壁・間仕切り、床、開口と階段の位置を確認した。室6つはそれぞれ所属階の高さから始まっていた。
- 窓8・扉6の名前は `AW1`、`AW-1`、`AD1`、`WD1` などの符号で、14件とも壁の開口に対応し、配置点と高さが開口の位置・高さと合っていた。初回検証のIFCでは窓・扉の形状がなく、画像では壁の穴として見えていた。[IFCの窓・扉の書き方](101_ifc_export.md)
- 室の仕上げは `Pset_SpaceCoveringRequirements` で取得できた。和室の床・壁・天井は「畳 t55」「じゅらく」「杉板」だった。仕上げの確認はプロパティ値まで。

## 7-3 庇と外壁の接続

- 庇1つを `IfcSlab` として読み取れた。IFC4の `PredefinedType` は `USERDEFINED`、`ObjectType` は `CANOPY`。ホストの外壁とは `IfcRelConnectsElements` で関連付けられ、庇の取付側と壁の外面が世界座標 Y = 7.3755 m で一致した。拡大画像でも開口の上で庇が外壁に接していた。[IFC4のIfcSlabの型](https://standards.buildingsmart.org/IFC/RELEASE/IFC4/FINAL/HTML/schema/ifcsharedbldgelements/lexical/ifcslabtypeenum.htm)

確認データは配布フォルダの `outputs/ifc-external-review/`。内容は `ifc-review.json` と全景・1階・庇拡大のPNG。今回は外部ソフトを使ったAI確認で、人による画面操作と別のBIMビューアでの表示比較は未実施。

## 建具形状の追加確認

B-07（他のBIMへ渡せること）の確認。窓と扉の `Body` に、SketchBRepの3D表示と同じ建具形状を出力するようにした。枠・ガラス・扉などは開口IDでまとめ、従来の符号・配置と `IfcRelFillsElement` の関係を保つ。

IfcOpenShell 0.9.0.post1で例題Aを再度開き、窓8件・扉6件の形状を取り出せた。各建具の符号と世界座標の配置は以前のIFCと一致し、各建具は対応する開口と幅・高さが一致した。検証したIFCと数値は配布フォルダの `outputs/ifc-axis-and-fitting-check/`。

配布版 `4dbf133` のIFCからIfcOpenShellで抽出したメッシュをBlender 5.3.0で表示した。1FL・2FLの各階に窓4件・扉3件が入り、14件とも対応する開口との関連がある。階ごとの俯瞰で壁と建具の位置を照合し、拡大画像では南面の引違い窓2枚の枠と障子、北側の玄関扉の扉板・取っ手・庇を確認した。

画像は配布フォルダの `outputs/ifc-fittings-visual-check/` にある。階ごとの俯瞰は `first-floor.png`・`second-floor.png`、拡大は `south-windows.png`・`north-entry.png`。確認用に天井・屋根を隠し、窓を青、扉を茶、庇を橙に着色した。
