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

## パラメトリック化
- **回転体はパラメトリック**: 面プロファイルを回転すると、プロファイルのレシピに `RevolveOp{軸, 角度}` が追加される（`.sbr` に保存）。回転角はパネルで度単位（`op<N>.angle`）で変更でき、保存・再読込後も再編集できる。
- **ロフトはパラメトリック**: 断面（面・曲線）のレシピと配置をスナップショットとして保持し、断面の寸法を `in<i>.<key>`（例: `in1.width`）で後から変更できる。断面オブジェクト自体はドキュメントに残る。
- スイープ・曲線は現状、検証済みの非パラメトリック形状として保存。

## 未着手（次の作業）
1. `doc::Recipe` へのベース形状追加（曲線・Sweep をパラメトリックに保持。Revolve・Loft は対応済み）。`Modeler` API（`createCurve` / `loftObjects` / `revolveObject` / `sweepObject`、結果は非パラメトリック形状・Undo 可）は実装済み
2. GUI: 「曲面」メニューの Loft / 回転体 / スイープ（選択に対して実行）は実装済み。曲線ツール（C キー: 通過点をクリック、Enter / ダブルクリックで確定、始点クリックで閉曲線、推論・スナップ・プレビュー付き）も実装済み。回転軸の対話的指定は未実装
3. 3DM 入出力（openNURBS）、トリム曲面、曲面の Push/Pull・オフセット
4. 複雑形状の描画最適化
5. Windows 実機・CI でのビルド確認（引き続き最優先）
