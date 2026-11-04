# 構造種別の手引き：段階1

docs/52 §11 の段階1。木造の断面・役割・プリセットをアプリへ埋め込み、
同じデータからヘルプ、AIコマンド、MCPリソースの説明を作る。
鉄骨の既存カタログはそのまま。鉄骨・RCの手引きは段階6で追加する。

## データと上書き

- `assets/construction/timber/{sections,roles,presets}.json`。
- 各表は `source`（name/version/date/url）、`normative`、IDをキーにした `entries`。
- 出典は docs/52 の初期値。流通寸法の目安で、法令の値や照査結果ではない。
- 梁の候補は105と120〜390の30刻み。既定240を含め、柱幅105/120を選べる。
  docs/52の「105〜390（30刻み）」は両端を同じ30刻みで結べないため、この解釈を明記した。
- 合板は厚みだけ。鋼製束・鋼製火打は製品未選定。架空の幅・せいを補って部材を作らない。
- `set_construction_overrides` は元の表とは別のプロジェクト設定へ保存する。
  `.sbr` で共有でき、Undo/Redo、batchの取り消し、previewにも対応する。
  既存部材の寸法は自動では変えない。
- 上書きはJSON Merge Patch（entriesをIDで更新、nullで削除）。適用後の全表を検証し、
  存在しない断面への参照、寸法0以下、出典の欠落などは何も変えずに失敗する。
  元の出典と利用者の出典を別々に返す。

## AIから使う

1. `get_construction_guide {"system":"timber"}`：`data` と `markdown`。
2. `list_sections {"system":"timber","role":"column"}`：管柱の候補と `default_section`。
3. `create_member {"section":"TIMBER-105x105","start":[0,0,0],"length":2900,"role":"column"}`。
   `kind=column`、`material_class=timber` という既存の属性が付く。
4. 断面寸法を上書きしたプロジェクトでは、そのIDで作る部材にも上書き後の値を使う。
   返された `section` オブジェクトを使う場合は `material_class` も明示する。

上書き例（既定の管柱を120角、モジュールを1000にする）：

```json
{"command":"set_construction_overrides","args":{"system":"timber","overrides":{
  "source":{"name":"利用者の初期値","version":"1","date":"2026-11-04","url":"https://example.org/defaults"},
  "normative":"user",
  "roles":{"entries":{"column":{"default_section":"TIMBER-120x120"}}},
  "presets":{"entries":{"module":{"value":1000}}}
}}}
```

`overrides:{}` で同梱の初期値に戻る。`get_construction_guide.data.overrides` を別の
プロジェクトへ登録すれば共有できる。新しい矩形断面のIDは `TIMBER-` から始める。

`list_sections` の `system` を省略した場合は従来の鉄骨検索。
未知のsystem/roleや未実装のRCは明示的にエラーを返す。
MCPリソース `sketchbrep://construction/timber` も同じプロジェクトのMarkdownを返す。
ヘルプの「木造の手引き」は上書き変更・Undo・プロジェクト読み込み後に更新する。
文書にしたい場合はコマンドが返すMarkdownを保存できる。

## 検証

`Construction.*`：読み込み、全矩形断面、出典、上書き保存、Undo/Redo、
検索、AIで柱を作成・編集、エラー時に変更なし、preview/batch、MCPリソース。
GUIの `helpWindowAndCrashLog`：トピック表示、上書きとUndoの表示反映。

レシピ生成・実際のレベル作成は段階2、診断は段階3、壁量の表と計算は段階5。

### ローカルの実行結果（2026-11-04）

- 計算テスト済み：Windows / MSVC 19.44 / OCCT 8.0.1 / Qt 6.12でビルド成功。
  ctestの非GUI・3DM以外449件は442成功、7件スキップ、失敗0（13.60秒）。
  スキップは環境指定が必要なベンチとJw_cad付属サンプル。段階1の8件はすべて成功。
- GUI自動確認済み：`helpWindowAndCrashLog` 成功。手引き表示、上書き反映、Undo反映。
- Windows実機確認：人による操作確認は未実施。
- ローカルは `SBR_WITH_OPENNURBS=OFF`。3DM関連4件は検証対象から除外。
  3DMを含む全構成・Linux・配布物はCIで別途確認する。

### CIの結果（2026-11-04）

実装コミット `ff535797cce249b19c6d486d8c5417ff9318df98` の
[CI #233](https://github.com/higeotoko/sketchBRep/actions/runs/41773608702) はLinux・Windowsとも成功。

- 計算テスト済み：Linuxは3DMを含む通常構成で460件中453件成功、7件スキップ、失敗0。Windowsの非GUI456件も失敗0。
- GUI自動確認済み：Linuxの全GUI、WindowsのGUI3件と大きいGUIスイート（193成功、7スキップ、失敗0）が成功。
- 配布物：Windows zipのDLL・起動確認、インストーラーのインストール・起動・アンインストール確認が成功。
  成果物名は `SketchBRep-windows-x64` と `SketchBRep-windows-x64-setup`。
- Windows実機確認：人による操作確認は引き続き未実施。
