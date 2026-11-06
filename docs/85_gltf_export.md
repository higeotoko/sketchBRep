# glTF の書き出し（レンダリング用）（2026-11-06）

検討書（docs/reviews/2026-11-06_visualization_engine_study.md）の段階 2。SketchBRep のモデルを、材質の見た目ごと、ほかのレンダラー（Blender、Twinmotion、Unreal Engine など）に渡す。glTF 2.0 は、多くの 3D ソフト・ゲームエンジンが読める標準の形式。

## 使い方

「ファイル > エクスポート...」で「glTF・レンダリング用（Blender・Twinmotion・UE）」を選ぶ。

| 拡張子 | 中身 |
|---|---|
| `.glb` | 1 つのファイル（おすすめ） |
| `.gltf` | 文字のファイルと、横に置く `.bin` |

- 選んでいる物があればそれだけ、無ければ見えている物すべて。
- 書き出す物：面のある物（立体・面・開いた形）。線・寸法・通り芯は書き出さない（レンダリングされないため）。
- 単位はメートル、Y が上（glTF の決まり）。SketchBRep の mm・Z 上から変換する。
- 物ごとに 1 つの塊（面をまとめる）。レンダラーでの描画が軽くなる。

## 材質

| SketchBRep | glTF |
|---|---|
| 色 | 基本色（baseColorFactor） |
| 透明度 | 基本色の 4 番目（アルファ）＋半透明の指定 |
| 金属感 | metallicFactor |
| 粗さ | roughnessFactor |
| 材質の無い物 | 表示の色、非金属、粗さ 0.6 |

- 同じ材質の物は、glTF でも同じ材質を共有する。
- OCCT 7.6 の書き出しでは、材質の名前が `mat_0`、`mat_1`… になる（値は正しい）。OCCT 8（Windows 版）で名前が残るかは未確認。
- まだ書き出さないもの：面ごとに塗った材質（物の材質だけ）、画像（テクスチャ）、太陽・照明・カメラ。

## 実装

- `geom::writeGltf`（`src/core/src/Gltf.cpp`）：OCCT の XCAF 文書に形と材質（`XCAFDoc_VisMaterial` の PBR）を入れ、`RWGltf_CafWriter` で書く。
- 形は**コピーを三角形に分けて**書き出す。モデルの形には三角形を残さない（テストで確認）。細かさは 1 mm・15°（AI からは変えられる）。
- `Modeler::exportGltf`、AI の `export_gltf`（ファイル操作なので、アプリ内の AI からは使えない。CLI・MCP のヘッドレスでは使える）。
- CMake：OCCT 7.8 以降は `TKDEGLTF`、それより前は `TKRWMesh`。

## 確認

| 区分 | 内容 |
|---|---|
| 計算テスト済み | `Gltf.MaterialsMetresAndYUp`：glTF 2.0、線は書き出さない、鋼（金属 1・粗さ 0.25・色）と材質なし（非金属・粗さ 0.6）の 2 材質、1×2×3 m の箱がメートル・Y 上で 1×3×2、物ごとに 1 塊、モデルの形に三角形が残らない、`.glb` の先頭が `glTF`、AI の `export_gltf`、拡張子違いを断る |
| GUI 自動確認済み（Linux） | `gui_phase1: realisticDisplay` で、画面の書き出しの経路から `.glb` が書けること |
| Windows 実機確認 | 未。Blender・Twinmotion で開いた時の見た目、OCCT 8 での材質の名前 |
