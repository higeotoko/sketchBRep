# 02. アーキテクチャ

## 全体構成

```
┌──────────────────────── src/app (Qt 6, 実行ファイル SketchBRep) ────────────────────────┐
│ MainWindow ─ メニュー / 左ツールバー / 右パネル(PropertiesPanel) / 下部 数値入力・ガイド │
│ Tools      ─ Select / Line / Rectangle / PushPull（SketchUp 風の直接操作）              │
│ Viewport   ─ OCCT V3d/AIS ビュー、カメラ、ピック、プレビュー、スナップマーカー           │
└──────────────┬───────────────────────────────────────────────────────────────────────────┘
               │ すべての編集は doc::Modeler を経由（GUI も将来の AI/スクリプトも同じ経路）
┌──────────────▼──────────── src/core (静的ライブラリ sbr_core, Qt 非依存) ────────────────┐
│ Document layer  doc::Modeler / Document / Object / Recipe / CommandStack / NativeFormat  │
│ Geometry layer  geom::Profile / Operations / Validation / Measure / ShapeIO / FaceRef    │
│ Display layer   display::DisplayMesh（正規形状のコピーからテッセレーション）            │
│ Inference       snap::InferenceEngine（CAD 形状から頂点・中点・エッジ・面・軸推論）      │
└───────────────────────────────────────────────────────────────────────────────────────────┘
               │
          Open CASCADE Technology 7.6+
```

`src/core` は Qt に依存しないため、CLI（`tools/phase0`）・単体テスト・将来のヘッドレス API から同じコードを使える。

## 3 層データモデル

| 層 | 保持するもの | 実装 |
|---|---|---|
| **Geometry** | 正規の B-rep（`TopoDS_Shape`）。単位 mm | `doc::Object::shape`（レシピの計算結果）、`geom::*` |
| **Display** | 描画用三角形・エッジ折れ線・法線 | `display::makeDisplayCopy` / `buildDisplayMesh`、Viewport の AIS（**常にコピーを表示**） |
| **Document** | ID・名前・レイヤ・表示・色・変換行列・パラメトリックレシピ・編集履歴 | `doc::Object`, `doc::Recipe`, `doc::CommandStack` |

### 「描画メッシュの解像度を下げても CAD 精度が損なわれない」の担保

- Viewport は `BRepBuilderAPI_Copy` した **表示専用コピー** にしかテッセレーションを掛けない。
- 保存（BRep テキスト）は三角形なし（`withTriangles = false`）で書き出す。
- スナップは表示メッシュではなく正規形状の頂点・曲線・面から評価する。
- テスト `DisplayMesh.QualityChangesMeshButNotGeometry` で、粗/精細メッシュの三角形数が変わっても正規形状に三角形が付かず体積が不変であることを検証。

## パラメトリックレシピ

```
Recipe = BaseFeature + [Modifier...]
  BaseFeature: RectangleProfile{plane, width, height} | PolygonProfile{points}
             | PolylineCurve{points} | ImportedShape{shape}
  Modifier:    ExtrudeOp{distance}           （先頭のみ・プロファイル面→ソリッド）
             | PushPullOp{FaceRef, distance} （ソリッドの平面フェースを押し引き）
```

- 再計算 `doc::regenerate()` は毎回ゼロから組み立て、各段階で妥当性検証する。
- 公開パラメータ: `width`, `height`, `op<N>.distance`（UI・数値入力・将来の AI API 共通のキー）。
- Push/Pull の解釈（`Modeler::pushPull`）:
  1. プロファイル面 → `ExtrudeOp` を追加（パラメトリック押出）
  2. 押出しただけのソリッドの **上面** → `ExtrudeOp.distance` を直接変更（3000→4500 の再編集と同じ）
  3. それ以外の平面フェース → `PushPullOp` を追加（面は `FaceRef` で幾何参照）
- 失敗（不正形状・参照面消失・ゼロ寸法）は **ドキュメントを一切変更せず** エラーを返す。

### Topological Naming への対処（現段階）

`geom::FaceRef` = 外向き法線 + 支持平面上の重心 + 面積。再計算後の形状から「同じ平面・同じ向き」の面を探し、重心が最も近いものに解決する。
- 上流で高さを変えても側面の Push/Pull は追従する（テスト `SideFacePushPullIsRecordedAndReplayed`）。
- 参照平面そのものが動く編集（幅を広げて +X 面が移動）は **解決失敗として拒否**（テスト `TopologicalNamingFailureIsReportedAndRolledBack`）。

## コマンド基盤・Undo/Redo

`doc::Modeler` の各操作は `doc::Command{name, [ObjectDelta{before, after}]}` を `CommandStack::execute` に渡す。
Undo はスナップショットを戻すだけなので、モデリング演算の再実行に依存せず **形状が正確に復元**される。
`isClean()` で未保存変更を追跡する。

## オブジェクト ID

UUID v4 形式（128bit 乱数）。保存・再読込で保持される。面・稜線などのサブ要素は配列番号を永続 ID にしない（`FaceRef` を使用）。

## ネイティブファイル形式（`.sbr`, version 1）

UTF-8 JSON。

```jsonc
{
  "format": "SketchBRep", "version": 1,
  "units": "mm", "linearTolerance": 0.0001,
  "camera": { "eye": [...], "target": [...], "up": [...], "perspective": true, "scale": 0 },
  "objects": [{
    "id": "8f3c…", "name": "Room A", "layer": "Default", "visible": true,
    "color": [0.86, 0.86, 0.84], "transform": [12 values, row-major 3x4],
    "recipe": { "base": { "type": "rectangle", "plane": {...}, "width": 4000, "height": 6000 },
                "modifiers": [ { "type": "extrude", "distance": 3000 } ] },
    "geometry": { "brep": "<OCCT BRep text, no triangulation>", "volume": 7.2e10 }
  }]
}
```

- **保存**: `<path>.tmp` に書き込み → 読み戻してバイト一致とパース可能性を検証 → rename で置換（途中失敗で既存ファイルを壊さない）。
- **読込**: レシピを再計算し保存済みジオメトリと体積・外接箱で比較。不一致や再計算失敗時は保存済みジオメトリを優先し警告を出す（データを失わない）。
- バージョンが新しすぎる・形式不正・単位不明は読み込み拒否。

## 将来拡張の差し込み口

| 拡張 | 差し込み口 |
|---|---|
| Move / Rotate / Copy | `Object::transform` は既に保存・表示・ピック時の逆変換まで対応済み |
| Group / Component | `Document` に階層ノードと定義共有（`TopoDS_Shape` 共有 + インスタンス変換）を追加 |
| AI / スクリプト API | `doc::Modeler` を JSON-RPC / Python で公開（GUI と同じ検証・Undo 経路） |
| 3DM 入出力 | `geom::ShapeIO` に openNURBS 変換を追加 |
| 独自レンダラ | `display::DisplayMesh` をそのまま GPU バッファへ |
