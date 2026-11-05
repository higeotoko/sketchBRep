# CI の再開と、止まっていた間の赤の修正（2026-11-05）

docs/70 §3 の「CI が止まっている」件の調査・修正・配布の記録。

## 1. CI が起動しなかった原因

- GitHub は PR の CI（`pull_request`）を、PR を main に試しにマージした版で動かす。
  PR #1 では、この試しマージが **2026-11-04 09:11 UTC の `5d2a4e1` のまま更新されていない**
  （`mergeable` が `null` のまま。試しマージのコミットの親が `5d2a4e1`）。
  そのため、その後の push に対して `pull_request` の CI が 1 度も作られなかった。
- Actions は有効（`allowed_actions: all`）、ワークフローは `active`、main との衝突なし（main より 273 進み、0 遅れ）。
  GitHub の障害ではなく、PR #1 の試しマージが固まっていることが原因。
- **手動実行（`workflow_dispatch`）は普通に動く。** 今後は push のあとに
  `gh workflow run CI -R higeotoko/sketchBRep --ref claude/sketchbrep-modeler-spec-4wnp0b -f windows=true`
  で CI を回し、結果を `gh run view` で確かめる。

## 2. 止まっていた間（21 コミット）の CI の結果

`e02208c` の手動実行（run 41960877709）で 3 件が赤だった。

| テスト | 環境 | 原因 |
|---|---|---|
| `Quantities.UniformScaleAndMirroredSharedMembersKeepTheirOwnSizes` | Linux | OCCT 7 は拡大縮小・鏡映つきの位置（`TopLoc_Location`）を拒否する（"Moving with scaling transformation is forbidden"）。OCCT 8 は受け付けるので、ローカルでは出なかった |
| `GuiPhase1::topEdgesCopiedDownDivideTheSides` | Linux・Windows | 建築ツールバー（`e492b34`）が上に 2 段目の行を足し、テストの土台が測る「以前の作業領域」が 1 行ぶん小さくなった。画素が粗くなり、辺のクリックが面を拾った |
| `GuiPhase1::typedLengthSplitsOrClosesSafely` | Windows | 同上（辺のクリックで線が始まらなかった） |

## 3. 修正（`fa9e9a2`）

- `Document.cpp` の `placed()`：まず `Moved()`（形を共有したまま位置を付ける）を使い、
  OCCT が拒否した時だけ `BRepBuilderAPI_Transform` の変換コピーにする。OCCT 8（配布版）では動きが変わらない。
- GUI テストの土台（`Fixture::setUp`）：作業領域を測る間だけ建築ツールバーを隠し、
  その行のぶんウィンドウを大きくする。アプリ本体の画面は変えていない。

## 4. 確認

- **CI（run 41972783340、`fa9e9a2`）：成功。** Linux 572 件すべて成功（GUI を含む）。
  Windows はコア 568 件・GUI 216 件成功（失敗 0、7 件は従来どおりスキップ）。
  止まっていた間の 21 コミット（段階 4〜束石）は、これで CI 済みになった。
- ローカル（Windows、OCCT 8）：コア 559 件中 552 件成功・7 件スキップ。
  オーナーの PC は表示倍率 175% で、`topEdges…` は修正前の `5d2a4e1` でも落ちる（画素の丸めが違う）。
  `QT_SCALE_FACTOR=0.5714` で 1 倍に近づけると 2 件とも通り、修正を戻すと 2 件とも落ちることを確かめた。
  この設定では `lineToolCrossingLinesMakeFace` の面積が許容 1% を少し超えるが、CI（本当の 1 倍）では成功している。
  ローカルの GUI 全件は、表示倍率の影響を受けるので CI の結果を正とする。

## 5. 配布

| 項目 | 内容 |
|---|---|
| コード | `fa9e9a23e965adbd84a7cc5de5f6ce9c21e25e13` |
| ZIP | `SketchBRep-windows-x64-fa9e9a2-local.zip`（Codex のチャットフォルダの `outputs\`） |
| サイズ | 69,974,727 bytes |
| SHA256 | `ade302beac233ed21fc269508c15e380c2e9302d16d28838e5a55fa1fe8a3cb3` |
| 中身の変化 | 拡大縮小・鏡映した物の扱い（OCCT 8 では従来どおり）。GUI の変更なし |

- 展開した ZIP で：100 DLL のハッシュ・104 個の依存・CRC・EXE のハッシュを確認。
  PATH を配布物と Windows だけにして、CLI・MCP・STEP・3DM・壁区間・布基礎・支持経路・束石・二階建てサンプルが成功。
  GUI 18 条件（初期化・終了込み 20 件）成功。
- この ZIP はローカルのビルド。同じコードは CI で成功しているが、ZIP そのものは CI の生成物ではない（`ci_verified:false`）。
- **人による Windows 実機での一連の操作は未確認**（`human_windows_verified:false`）。
- 記録：`docs/validation/2026-11-05-portable-fa9e9a2.json`。
