# Codex → Claude 引継ぎ（2026-11-05 JST）

## 最初に読むこと

ユーザーのCodex週間使用量が93%になったため、追加実装を止めてClaudeへ引き継ぐ。
束石までの実装・ローカル検証・最終ZIP作成は完了。残りの基礎機能は未実装として下記に区別した。
再開時はこの文書、`git status`・直近ログ、ユーザーの最新指示を確認し、完了済みを作り直さない。
この引継書と配布記録の最終コミットは文書更新のみ。ZIPのコードは以下の `ccddc4a`。

## 場所・ブランチ

| 項目 | 内容 |
|---|---|
| GitHub | https://github.com/higeotoko/sketchBRep |
| 作業ブランチ | `claude/sketchbrep-modeler-spec-4wnp0b` |
| コード最新 | `ccddc4a9ea6332a252dd51d1cbc0549cece54620` |
| 束石実装 | `30c329825341cc6fa4e033991a1606658e15cc5c` |
| 引継ぎ前の基準 | `c1448ea9d184f89369ea2ce87f0b6cc90f504152` |

実リポジトリ（**ここでGit・CMakeを実行**）：

```text
C:\Users\enomo\Documents\Codex\2026-11-04\higeotoko-sketchbrep-claude-sketchbrep-modeler-spec\work\sketchBRep
```

このチャットの成果物・補助スクリプト（Gitリポジトリではない）：

```text
C:\Users\enomo\Documents\Codex\2026-11-04\files-pasted-by-the-user-codex
  outputs\  配布ZIP、サンプル、検証JSON/XML、画面画像
  work\     ビルド用cmd、検証Python、ログ、MCP取得結果
```

## ユーザーの目的と守ること

- SketchBRepの「SketchUpのような直接操作＋正確なB-rep」を維持し、散在するBIM機能を直感的な建築の操作へまとめる。
- SumiCADをMCPで調べ、合う操作・思想を学んで実装する。コードや法規式の単純移植ではない。
- 普通のモデリングを構造ルールで止めない。形状の対応と接合耐力・構造の合否を区別し、情報不足を合格にしない。
- 同じブランチで作業。**mainへマージ、amend、rebase、force-pushをしない**。既存draft PR #1を勝手にマージしない。
- GitHubへの通常push・更新記録・**変更ごとの実行可能なWindows ZIPリンク**は明示的に依頼済み。毎回、必要のない許可確認で止めない。
- 「計算テスト／GUI自動確認／人によるWindows実機確認」を分けて報告。日本語で、専門語をかみくだく。
- 有料AI APIを前提にしない。B-repを正規データとし、GUIもAIも同じModeler・Undo経路を使う。
- RC配筋設計は対象外という既存方針。非公開DXF・Jw_cad付属サンプル・SketchUp SDK・認証情報をGitや配布物に入れない。
- 以前の指示・ビルド背景は [docs/53](53_handover_2026-11-04.md)。その「最新コミット・次工程」は古いので、本書を優先して現在位置を読む。

## 完了した機能

| 文書 | 内容 |
|---|---|
| [60](60_timber_opening_reference_audit.md) | 開口と木造架構の座標・参照修正、競合警告 |
| [61](61_architecture_workflow_toolbar.md)・[62](62_plan_opening_placement.md) | 建築ツールバー、レベルを選んで平面から壁・スラブ・開口を操作 |
| [64](64_wall_bay_candidates.md)・[65](65_wall_bay_member_references.md) | 開口を除く壁区間、実柱で分割、両端柱・上下材・接触を読み取り確認。壁量へ自動計上しない |
| [66](66_foundation_planning.md) | 実土台から布基礎の立上り・底盤、交点の和演算と正味体積、変更検知・再生成 |
| [67](67_foundation_support_paths.md) | 柱→土台→基礎、床束下の支持先・下面の重なり・隙間、実部材の選択・強調 |
| [68](68_foundation_pads.md) | 実床束・柱から束石／矩形独立基礎を計画。未検出床束の選択、寸法・再生成・参照の選び直し。架構画面で木製床束を選べる |

現在の画面の流れ：木造架構で木製床束を選ぶ → 布基礎を計画 → 支持先一覧 →「未検出の床束に束石を計画」→寸法・プレビュー・適用 → 支持先を再確認。
鋼製束・製品未選定は目印のまま。実部材や成立した支持として扱わない。

### 実装を読む入口

- `src/core/src/ConstructionFoundation.cpp`：布基礎と束石の入力・参照・マニフェスト・生成・検査。
- `src/core/src/FoundationSupports.cpp`：水平な下面／支持上面の交差面積、隙間、土台を経由する経路。
- `src/core/src/CommandApi.cpp`：`generate_foundation`、`check_foundation`、`check_foundation_supports`。
- `src/app/FoundationDialog.cpp`：布基礎・束石の共通設定、プレビュー。`FoundationSupportsDialog.*`：一覧と未検出床束の入口。
- `src/app/TimberFrameDialog.cpp`：`timberFloorPostSection`で床束を選ぶ。既存の他の断面指定を消さず、`sections.floor_post`だけ更新する。
- `src/app/MainWindow.cpp`：メニュー、共通のpropose。プレビュー用B-repは独立コピー。
- `tests/test_foundation.cpp`、`test_foundation_supports.cpp`、`test_foundation_pads.cpp`、`test_gui.cpp`。

### 維持すべき契約・落とし穴

1. `Modeler::generateFoundation` は一つの書込み操作。**CommandApi側**が初期状態をscratchコピーのプレビューにしている。適用はrevision確認、または既存batch/propose経路。
2. 計画は独立した文書直下のグループ。`attributes["foundation_plan"]`にJSONを保存する。木造用の `GroupNode.constructionFrame` と混同しない。
3. `inputs.type:"pads"` は `sources:[実床束・柱ID]` の明示指定のみ。幅・奥行・高さの初期300×300×150mmは仮の形状入力で、設計基準ではない。
4. 同位置・同天端の参照は一つの束石へまとめる。束石同士・既存立体との正の体積の重なりは生成前に拒否。全て準備した後に一つのUndoで確定し、途中の失敗で部分配置しない。
5. 手編集・追加・削除があれば明示的 `on_modified:"replace"` まで保持。同じ入力の再実行ではUndoも形状も増やさない。
6. 複数束石は `quantities.foundation_pad.ids` と合計体積。`.id`は単一のときだけ。複数天端の計画全体 `top_z` はnull、各参照の `planned_top_z` を使う。
7. 架構を再生成して部材IDが変わったら、参照は自動で近隣へ付け替えない。新部材を選択し、束石画面で既存計画＋「選択した実部材の床束・柱」に切り替えて再指定できる。
8. 形状の接触・全面の重なりを、耐力や安全の合格にしない。`verified:false`。部分支持を足し合わせて全面支持にせず、複数候補・未評価・計算上限を区別する。
9. OCCTの和・積やCompoundへの追加は共有TShapeの管理フラグ等を書き換えることがある。読取検査のCommonは `SetNonDestructive(true)`、強調・プレビュー用Compoundには `BRepBuilderAPI_Copy` のコピーを追加する。保存用B-repの前後比較で確認済み。
10. 配布サンプルで「短い床束より全体が上の土台」を下部支持として拾う問題を発見・修正した。候補の `target.low > own.low + tolerance` は除外。実際に部材下面へ食い込む候補の診断は維持する。回帰テストを削らない。

## 最終成果物と確認済み範囲

すべて上記チャットの `outputs\` 配下：

| 成果物 | 内容 |
|---|---|
| `SketchBRep-windows-x64-ccddc4a-local.zip` | 最終Windowsローカル検証版、69,962,313 bytes |
| 同 `.zip.sha256` | `28351b881cec80eff247f17c7d4a6036dfc855e066908e77b00c90fe5b691fb4` |
| `SketchBRep-foundation-pad-house-ccddc4a.sbr` | 二階建て検証モデル、639,963 bytes。ZIPには同梱せず別配布 |
| 同 `.dxf` | 基礎伏図、33,450 bytes |
| `pad-core-results.json`・`pad-gui-results.xml` | 最終の計算／GUI結果 |
| `portable-ccddc4a-verification.json` | 展開したZIPの最終検証 |
| `pad-dialog.png` | 自動テスト時に確認した束石画面 |

- **計算・API：122件成功、18.798秒。** 新規束石13件＋支持判定修正1件を含む関連範囲。全コアスイートの宣言ではない。
- **GUI自動確認：24条件成功**（初期化・終了込み26件）、32.519秒。却下後の保存用B-rep、寸法保持、Undo、参照再指定、床束選択を含む。
- **展開ZIP：18 GUI条件成功**（同20件）、22.957秒。100 DLLハッシュ・104バイナリ依存・CRC・EXEハッシュも確認。
- PATHを配布物とWindowsだけにして、CLI・MCP・STEP・3DM・従来の壁区間／四辺接触が成功。
- 二階建てモデル：実木製床束6本、外周4本は布基礎上、内部2本に束石追加。6本の下部の幾何経路を保存再読込後も確認。基礎DXF・STEP出力成功。住宅の構造照査完了を意味しない。
- **人による一連のWindows実機操作、クリーンPC、今回のCI・Linux・全GUIは未確認。** `ci_verified:false`、`human_windows_verified:false`。

途中の `30c3298` ZIPとサンプルもローカルには残っているが、支持判定の修正前。**最終配布はccddc4aだけ**。
起動中のアプリやインストール先は更新していない。ユーザーは最終ZIPを全部展開して、そのSketchBRep.exeを起動する必要がある。
結果のサマリーは `docs/validation/2026-11-05-foundation-pads.json` と `2026-11-05-portable-ccddc4a.json` にも保存。

## ローカルで再開する手順

このPCの既存環境：MSVC 19.44／OCCT 8.0.1／Qt 6.12／3DM有効。
補助cmdはチャットの `work\build-opening.cmd`（コア・CLI・GUIテスト・アプリ）と `build-package.cmd`（配布4 EXE）。
どちらもvcvars64を呼び、必要なDLLのPATHを設定してからCMakeを実行する。ビルド中のソースを編集しない。

```text
Python: C:\Users\enomo\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe
CMake:  C:\Users\enomo\Documents\Codex\sbr-build-env\Library\bin\cmake.exe
vcvars: C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
```

テスト時のPATHは次の順。TEMP/TMPはチャットの `work\test-temp` にする。

```text
C:\Users\enomo\Documents\Codex\2026-11-04\higeotoko-sketchbrep-claude-sketchbrep-modeler-spec\work\Library\bin
C:\Users\enomo\Documents\Codex\sbr-build-env\Library\bin
```

`work\run_pad_tests.py` が122コア・24GUI条件を実行する今回の補助スクリプト。
Qtテストの `-o` はパスと `,junitxml` をまとめて一つの引数にする。標準出力だけで成功と判断せず、終了コードとJSON/XMLを読む。
GUIテストはWindowsの実表示で動かす。Qt6Test.dllはテスト用にだけ必要で、配布物には入れない。
リンクでShlwapi.lib等が見つからなければvcvars未設定。生成物 `build/src/app/SketchBRep.lib` の一時的な上書き拒否は、その生成ファイルだけを除いて再リンクすると解消した。ユーザーの実行中アプリを終了・置換しない。

### ZIPを毎回作る

[docs/63](63_windows_zip_delivery.md) と `tools/package_windows_local.py` を使う。4 EXEをビルドしてから新コミット名の別ステージ・別ZIPへ出力する。

```text
承認済みランタイム:
C:\Users\enomo\Documents\Codex\2026-11-04\higeotoko-sketchbrep-claude-sketchbrep-modeler-spec\work\timber-refinement-portable
dumpbin:
C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\dumpbin.exe
```

引数は `--runtime ... --build build --stage NEW_DIR --output NEW.zip --dumpbin ...`。
既存ZIPを上書きしない。依存の版を変えたら古いランタイムを流用せず、DLL・ライセンス・マニフェストを収集し直す。
`work\verify_portable_pads.py SHORT_COMMIT` が展開先の限定PATHで今回のCLI・MCP・住宅サンプル・GUIを検証する補助スクリプト。
新コードへ合わせて必要な検証を更新し、コードコミット→ZIP→検証→配布記録の別コミット→通常push→ZIPリンクを渡す。
今回のコードと最終ZIPは既に確認済みなので、**引継ぎのためだけに再ビルド・再テストしなくてよい**。

## SumiCAD MCP

このPCで確認したのはSumiCAD 0.7.1、64ツール。ローカルHTTP MCPは `http://127.0.0.1:47832/mcp`。
Codex側の認証設定は `C:\Users\enomo\.codex\config.toml` の `mcp_servers.sumicad`。
**設定全体やAuthorization値を表示・貼付・Git保存しない。** 既存 `work\check_sumi_connection.py` は認証値をメモリ内で使い、版・ツール名だけを出す。
`runpy.run_path`で初期化済みの `rpc` を再利用すると、同一セッションでread toolを呼べる。接続切断10053が時々あり、再試行で回復した。
Claudeのツール一覧に同じMCPがあるとは限らないため、最初に接続状態を確認する。新しいキーをむやみに求めず既存接続を使う。

今回の原文はチャットの `work\sumi-pad-research.json`。前段は `sumi-support-structure-live.json`、`sumi-foundation-research.json` 等。
ローカル取得結果にはプロジェクト情報を含み得るため、Gitや配布物へコピーしない。読んだ範囲・採用した考え方だけをdocsに記録する。
SumiCADの内部コード・法規の正しさ・束石固有の生成挙動を検証したと誤記しない。

## 未実装・次の候補

今回の束石は計画形状。地盤・支持力・沈下・凍結・根入れ・必要断面・アンカー・接合・荷重伝達の照査はない。
べた基礎、内部耐力壁の下部軸組と基礎の対応、梁経由の上階支持、段差・地形・床下地盤、人通口・配管・換気が残る。
鋼製束の製品選定・実形状、重なる束石の連続基礎へのまとめ、既存基礎の自動切欠きも未実装。
次はユーザーの最新指示に沿って、内部支持との連携やべた基礎を一つの通る操作として整えるのが候補。
SumiCADを参照しつつ、人が対象・寸法・仕様を確認する流れを優先し、ルールや自動配置だけを増やさない。
