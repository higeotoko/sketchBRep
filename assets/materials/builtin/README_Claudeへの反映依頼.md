# SketchBRep 定番12素材：画像データと反映依頼

作成日：2026-10-24（30MB制限対応の軽量版）  
対象：[higeotoko/sketchBRep / MaterialsPanel.cpp](https://github.com/higeotoko/sketchBRep/blob/716dd6a6e489a9f26bbca577282e0448d243c3cb/src/app/MaterialsPanel.cpp#L43)  
確認コミット：716dd6a6e489a9f26bbca577282e0448d243c3cb

## この一式で依頼すること

同梱画像とmaterials.jsonを使い、既存の定番12素材にテクスチャを組み込んでください。既存の素材名・順序を保持し、定番をクリックすれば画像付き素材を追加して塗れる状態にしてください。画像は生成済みで、再生成・外部画像検索・有料APIの追加は不要です。

このZIPは**Claudeへ渡す統合用素材パック**です。現行SketchBRepがZIPやmaterials.jsonを直接インポートできる形式ではありません。下記の既存処理へ接続して完成させてください。

## 内容

- textures/：JPEG12枚、各1024×1024px。ベースカラー（アルベド）、不透明画像。
- materials.json：素材名、旧RGB、画像、実寸mm、透明度、PBR推奨値、画像の既定ON/OFF。本パック独自のマニフェスト。
- integration/materials.qrc：そのまま使えるQtリソース定義。相対配置を維持すること。
- integration/BuiltinMaterialData.h：既存12種と同順序のC++17設定表。データのみでローダーは含まない。
- create_library.jsonl：現行CommandApi形式の、別の空文書へ12素材を作るコマンド。画像ファイルを読むためatomic:false。
- preview_contact_sheet.jpg：素材一覧と2×2反復の見本。素材画像として貼らないこと。
- image_inventory.json：実画像の寸法・バイト数・SHA-256。
- generation_prompts.json：組込image_genの生成プロンプト。
- QA.md：検証範囲と注意点。

## 既存コードへの最小接続

1. 一式をassets/materials/builtin/へ配置。textures/とintegration/の相対関係を保つ。
2. src/app/CMakeLists.txtのSBR_APP_RESOURCESへ、同梱materials.qrcのパスを追加。既存branding.qrcと同様に、起動場所に依存しないQtリソースにする。
3. MaterialsPanelのPresetへ画像・実寸・opacity・PBR値を接続する。同梱BuiltinMaterialData.hは設定テーブルとして利用できる。名前と順序は完全一致で保つ。
4. [MainWindow::addPresetMaterial](https://github.com/higeotoko/sketchBRep/blob/716dd6a6e489a9f26bbca577282e0448d243c3cb/src/app/MainWindow.cpp#L1590)は現在createMaterial(p.name,p.color)だけを呼んでいる。以下をここへ接続する。
   - QFileでリソースのbytesを読む。:/パスを標準ifstreamへ渡さない。
   - Modeler::addTextureImage(bytes, filename)で文書へ画像を登録。
   - 返るハッシュをMaterialTextureのimageへ設定し、width/heightを実寸mm、colorizeをfalseにする。
   - Modeler::editMaterialで画像、opacity、metallic、roughness、reflectanceを保存する。
   - 作成と更新をUndo1回にまとめ、失敗時は中途半端な素材を残さない。
5. 未追加の定番見本は現状swatch(p.color)なので、画像サムネイル表示へ接続する。追加済み素材には既存MaterialEditor::swatchを使える。
6. 既存文書は無断変更しない。「定番画像を適用」などの明示操作時のみ、対象のMaterial.idを保って画像を追加する。既存の独自画像は既定で上書きしない。同名が複数ある場合は対象を明示する。

## ガラス・金属と色

- ガラス画像も同梱。既定は画像OFF＋既存の淡青色＋opacity0.22を推奨。画像は必要時にONにできる。透過は画像アルファではなくMaterial.opacityで設定する。
- 金属に景色や強いハイライトを焼き込んでいない。metallic等は現状保存値で、モデリング表示では未反映。画像追加だけで鏡面反射・屈折まで実装したと報告しない。
- 全てcolorize=false。生成画像の色をそのまま使い、旧色で二重に着色しない。旧RGBは単色フォールバックとして保持する。
- PBR値は汎用の初期提案であり、実測値ではない。ノーマル・高さ・粗さの画像マップは含まない。

## 名前と実寸

| 完全一致の名前 | 画像1枚の幅×高さmm | 画像既定 |
|---|---|---|
| 木（明るい） | 1000×1000 | ON |
| 木（濃い） | 1000×1000 | ON |
| 漆喰（白） | 1000×1000 | ON |
| コンクリート | 2000×2000 | ON |
| モルタル | 1000×1000 | ON |
| レンガ | 840×650 | ON |
| タイル（グレー） | 1200×1200 | ON |
| 畳 | 900×900 | ON |
| ガラス | 1000×1000 | OFF（同梱） |
| 金属（ステンレス） | 500×500 | ON |
| 黒（鉄） | 500×500 | ON |
| 芝 | 1000×1000 | ON |

木とステンレスの主な筋は画像の縦方向。畳は縁なしの織り面。木は板継ぎのない汎用木目。レンガは生成結果の横4モジュール×縦10段に合わせ840×650mm、約210×65mmモジュール。タイルは4×4で1200mm角。実寸値は見た目用の推奨値で、実製品の寸法・製法の保証ではない。

## CLIで設定を試す場合

このREADMEのあるフォルダを作業ディレクトリとし、実環境のsbr_cli.exeのパスを使う。

```powershell
& '実際の配布先\sbr_cli.exe' --save 'material-library.sbr' 'create_library.jsonl'
```

生成されるのは**12素材を持つ別の空文書**。既存モデルへ自動合流するコマンドではない。空文書以外で繰り返すと同名素材を追加し得るため、既存文書は上記6のIDを保つ更新を使う。

set_material_textureはファイル操作であり、GUIのAIブリッジでは無効。MCPからこのJSONLをそのまま流す方法を利用手順にしない。CLIまたはQtリソースからの組込処理を使う。CLIでの生成はこの環境では未実行。

## 合格条件

- 定番12種の名前・順序が一致。11種は画像付き、ガラスは透明設定付きで使える。
- 既存の素材ID、面と部材への割当を保全する。
- 画像を文書へ埋め込み、配布先以外のフォルダでも再読込できる。
- 実寸・透明度・画像OFF・色フォールバック・サムネイルを確認する。
- 素材作成と設定がUndo/Redoでき、保存・再読込後も同じ見た目になる。
- Windows/Linuxのリソース同梱を確認する。
- 形状処理・選択・既存の操作不具合をこの変更に混ぜない。
- 同梱コードと設定は統合用。アプリでのビルド・実機表示確認はまだ行っていないため、実施した検証を別途報告する。


## 軽量版について
画像を1024px角・JPEG品質88（4:4:4）へ変換。元の画像から縮小・圧縮し、素材名・実寸・設定値は維持しています。高画質PNG版と混在させず、この軽量版のmaterials.json・qrc・設定テーブル・JSONLを一式で使ってください。
