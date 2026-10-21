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
- **スイープはパラメトリック**: 断面と経路（曲線）のスナップショットを保持し、断面寸法を `in0.<key>` で変更できる。
- **NURBS 曲線は通過点をレシピとして保存**（`.sbr` の `{"type": "curve"}`）。通過点はパネルの「詳細」で数値編集できる（`Modeler::setCurvePoints`、Undo 可）。
- **円弧（A）**: `ArcCurve{始点, 終点, 平面法線, 膨らみ, 閉じる}`。`GC_MakeArcOfCircle` による**正確な円弧**（NURBS 近似ではない）。既定は弦で閉じた弓形の面で、そのまま押し出してアーチ開口・アーチ窓の型にできる。膨らみ `bulge` はパラメータで後から変更可
- **回転体ツール（V）**: 面 → 軸の 2 点をクリックで 360° の回転体。続けて角度を入力すると変更（パラメトリック）。

## 未着手（次の作業）
1. `doc::Recipe` へのベース形状追加（曲線・Revolve・Loft・Sweep・通過点の数値編集は対応済み。次は通過点をビュー上でドラッグする UI）。`Modeler` API（`createCurve` / `loftObjects` / `revolveObject` / `sweepObject`、結果は非パラメトリック形状・Undo 可）は実装済み
2. GUI: 「曲面」メニューの Loft / 回転体 / スイープ（選択に対して実行）は実装済み。曲線ツール（N）、回転体ツール（V、回転軸を 2 点で指定）も実装済み
3. 3DM 入出力（openNURBS）、トリム曲面、曲面の Push/Pull・オフセット
4. 複雑形状の描画最適化
5. Windows: CI ビルド・ポータブル zip（約 54MB、`sbr_cli` / `sbr_mcp` 同梱）は完了、実機で起動・Push/Pull・オフセット確認済み。次はインストーラ
6. 線と円弧をつないだ輪郭の面化（線で面を分割する機能と合わせて）

## その他の追加
- 寸法・テキスト（注記）: `LinearDimension` / `TextNote` をレシピとして保持（`.sbr` の `"dimension"` / `"text"`）。線は本物のエッジなので選択・移動・消しゴムが効き、文字はビューに常に正面向きで表示。既定のタグ「Annotations」、STEP には書き出さない。寸法は測った点の座標を持つだけで、形状の変更には追従しない（関連付けは今後）
- シーン（保存ビュー）: ビュー上部のタブ。`.sbr` に保存（`"scenes": [{name, camera}]`、未知キーは旧版でも無視される）
- AI 連携: [06_ai_integration.md](06_ai_integration.md)
