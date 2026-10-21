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
             | BooleanFeature{kind, [Operand{Recipe, placement, name}]}   ← 入れ子のレシピ
             | GroupNode{}                    （階層ノード。形状なし）
             | ComponentInstance{definitionId}（共有定義の配置）
  Modifier:    ExtrudeOp{distance}           （プロファイル面 → ソリッド）
             | OffsetOp{distance}            （プロファイル面の輪郭オフセット。面 → 面）
             | PushPullOp{FaceRef, distance} （ソリッドの平面フェースを押し引き）
```

- 再計算 `doc::regenerate(recipe, RegenContext)` は毎回ゼロから組み立て、各段階で妥当性検証する。
  `RegenContext` はレシピの外にあるデータ（コンポーネント定義の形状）を解決する。
- 公開パラメータ: `width`, `height`, `op<N>.distance`、ブーリアンの入力は `in<i>.` を前置（例: `in1.op0.distance` = 2 番目の入力の押出距離）。UI・数値入力・将来の AI API 共通のキー。
- Push/Pull の解釈（`Modeler::pushPull`）:
  1. プロファイル面（オフセット後の面を含む）→ `ExtrudeOp` を追加（パラメトリック押出）
  2. 最後の操作が押出のソリッドの **上面** → その `ExtrudeOp.distance` を直接変更（3000→4500 の再編集と同じ）
  3. それ以外の平面フェース → `PushPullOp` を追加（面は `FaceRef` で幾何参照）
- 失敗（不正形状・参照面消失・ゼロ寸法・空のブーリアン）は **ドキュメントを一切変更せず** エラーを返す。

### ソリッド演算（Boolean）

入力オブジェクトはドキュメントから取り除かれ、そのレシピとワールド配置が結果のレシピに **コピーとして保存**される。
結果の再計算は入力レシピの再計算 → 配置 → `BRepAlgoAPI_Fuse/Cut/Common` → `ShapeUpgrade_UnifySameDomain`。
入力の寸法を後から変えられる（壁 − 開口 の開口高さ変更など）。入れ子（ブーリアンのブーリアン）も同じ仕組みで再帰的に動く。
結果が複数ソリッド（壁を貫通して二分）でも有効。単一ソリッドのときは後続の Push/Pull が使える。

## 階層・配置・コンポーネント

- `Object::parent` で親（グループ）を持ち、`Object::transform` は **親座標系での配置**。
  ワールド変換 = 親のワールド変換 × 自身の変換（`Document::worldTransform / worldShape`）。
- 移動・回転はワールド空間の変換 `D` として与え、親座標へ変換して `local' = P⁻¹·D·P·local` を適用。グループと子を同時に選んでも二重に動かない（`Modeler::topmost`）。
- コピー / 配列: サブツリーを新しい ID で複製し、k 個目に `Dᵏ` を適用（直線・円形配列を同じ仕組みで扱う）。
- **コンポーネント定義**（`ComponentDefinition`）: オブジェクト群（定義内で独自の階層を持つ）と、そのリーフ形状のコンパウンドをキャッシュ。
  インスタンスは `ComponentInstance{definitionId}` を持ち、形状は定義のコンパウンドを共有（OCCT の参照共有でメモリも共有）。
  定義の原点は選択範囲の最小コーナー。定義のパラメータを変えると、1 つのコマンドで定義と全インスタンスのキャッシュを更新。
- コマンドは `ObjectDelta` に加えて `DefinitionDelta` を持ち、Undo/Redo で定義の追加・変更・削除も正しく戻る（インスタンスより先に定義を適用し、Undo では後に戻す）。

### Topological Naming への対処（現段階）

`geom::FaceRef` = 外向き法線 + 支持平面上の重心 + 面積。再計算後の形状から「同じ平面・同じ向き」の面を探し、重心が最も近いものに解決する。
- 上流で高さを変えても側面の Push/Pull は追従する（テスト `SideFacePushPullIsRecordedAndReplayed`）。
- 参照平面そのものが動く編集（幅を広げて +X 面が移動）は **解決失敗として拒否**（テスト `TopologicalNamingFailureIsReportedAndRolledBack`）。

## コマンド基盤・Undo/Redo

`doc::Modeler` の各操作は `doc::Command{name, [ObjectDelta{before, after}]}` を `CommandStack::execute` に渡す。
Undo はスナップショットを戻すだけなので、モデリング演算の再実行に依存せず **形状が正確に復元**される。
`isClean()` で未保存変更を追跡する。
`beginGroup / endGroup / abortGroup` で複数のコマンドを 1 つの Undo ステップにまとめられる（AI の `batch`、`create_box`）。`abortGroup` は途中までの変更をすべて戻す。

## オブジェクト ID

UUID v4 形式（128bit 乱数）。保存・再読込で保持される。面・稜線などのサブ要素は配列番号を永続 ID にしない（`FaceRef` を使用）。

## ネイティブファイル形式（`.sbr`, version 2）

UTF-8 JSON。

```jsonc
{
  "format": "SketchBRep", "version": 2,
  "units": "mm", "linearTolerance": 0.0001,
  "camera": { "eye": [...], "target": [...], "up": [...], "perspective": true, "scale": 0 },
  "definitions": [{ "id": "…", "name": "Column C1", "objects": [ /* 下と同じ形式 */ ] }],
  "objects": [{
    "id": "8f3c…", "parent": "<group id、省略時はトップレベル>",
    "name": "Room A", "layer": "Default", "visible": true,
    "color": [0.86, 0.86, 0.84], "transform": [12 values, row-major 3x4],
    "recipe": { "base": { "type": "rectangle", "plane": {...}, "width": 4000, "height": 6000 },
                "modifiers": [ { "type": "extrude", "distance": 3000 } ] },
    "geometry": { "brep": "<OCCT BRep text, no triangulation>", "volume": 7.2e10 }
  }]
}
```

- **保存**: `<path>.tmp` に書き込み → 読み戻してバイト一致とパース可能性を検証 → rename で置換（途中失敗で既存ファイルを壊さない）。
- **読込**: レシピを再計算し保存済みジオメトリと体積・外接箱で比較。不一致や再計算失敗時は保存済みジオメトリを優先し警告を出す（データを失わない）。
- バージョンが新しすぎる・形式不正・単位不明・親が存在しない階層は読み込み拒否。version 1 のファイルも読める。
- 追加のベース形状: `{"type": "boolean", "kind": "Subtract", "operands": [{"name", "placement", "recipe"}]}`、`{"type": "group"}`、`{"type": "instance", "definition": "<id>"}`。追加の修飾: `{"type": "offset", "distance"}`。

## 推論エンジン

スナップ候補は正規形状から評価し、画面距離で判定する（`snap::InferenceEngine`）。優先順位:

1. 端点・中点・原点 → 2. エッジ上 → 3. 起点からの軸推論（追跡点との交点を優先）→ 平行 / 垂直（追跡エッジ）
→ 4. 追跡点を通る軸線（点から）→ 5. 面上 → 6. 作業平面

「追跡」は SketchUp と同様に、カーソルが通過した端点・中点（最大 2 点）と直線エッジ（1 本）をツールが覚えて `SnapQuery` に渡す。

## STEP 連携

`geom::writeStepParts / readStepParts`（XCAF: `STEPCAFControl_Writer/Reader`）。
- 書出: オブジェクトごとに 1 製品、名前（UTF-8）と色を付与。グループ・インスタンスの配置はワールド座標で反映。
- 読込: アセンブリを再帰的にたどり、配置を合成したソリッドに平坦化。製品名・コンポーネント名と面色を引き継ぐ。

## 将来拡張の差し込み口

| 拡張 | 差し込み口 |
|---|---|
| コンポーネントの入れ子 | `RegenContext` は定義の再帰解決を想定済み。循環検出と依存更新の伝播を追加する |
| グループ編集モード | 選択時の「コンテキスト」（現在編集中のグループ）を UI 側に追加 |
| AI / スクリプト API | **実装済み**: `api::CommandApi`（JSON）+ `api::McpServer`、`sbr_cli` / `sbr_mcp`、アプリ内 `AiBridge`。[06_ai_integration.md](06_ai_integration.md) |
| 3DM 入出力 | `geom::ShapeIO` に openNURBS 変換を追加 |
| 独自レンダラ | `display::DisplayMesh` をそのまま GPU バッファへ |
