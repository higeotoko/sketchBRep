# 05. Phase 3 進捗（NURBS と Rhino 連携）— 着手分

時間制約のある作業枠で、Phase 3 のうち **幾何カーネル層** を先行実装した。

## 実装済み（`src/core/include/sbr/geom/Freeform.h`）
| 機能 | 実装 | テスト |
|---|---|---|
| NURBS 曲線（通過点補間） | `makeInterpolatedCurve` — `GeomAPI_Interpolate` による 3 次 B スプライン（閉曲線は周期） | 点を通ること、不正入力の拒否 |
| Loft | `loft` — `BRepOffsetAPI_ThruSections`（ソリッド / サーフェス） | 正方形断面の錐台体積が解析解と一致 |
| Revolve | `revolve` — `BRepPrimAPI_MakeRevol` | 矩形の 360° 回転 = 円柱体積 |
| Sweep | `sweep` — `BRepOffsetAPI_MakePipe` | 曲線パスに沿った有効ソリッド |

すべて結果を `validateShape` で検証し、不正な形状は失敗として返す（Phase 0 からの方針どおり）。

## 未着手（次の作業）
1. `doc::Recipe` へのベース形状追加（曲線・Loft・Revolve・Sweep をパラメトリックに保持）。`Modeler` API（`createCurve` / `loftObjects` / `revolveObject` / `sweepObject`、結果は非パラメトリック形状・Undo 可）は実装済み
2. GUI: 「曲面」メニューの Loft / 回転体 / スイープ（選択に対して実行）は実装済み。曲線をマウスで描くツールと、回転軸の対話的指定は未実装
3. 3DM 入出力（openNURBS）、トリム曲面、曲面の Push/Pull・オフセット
4. 複雑形状の描画最適化
5. Windows 実機・CI でのビルド確認（引き続き最優先）
