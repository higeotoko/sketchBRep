# Claude Codeへの引継書（2026-10-29 JST）

Codexから作業の主軸をClaude Codeへ戻すための資料です。新機能の依頼書ではありません。次の実装範囲は利用者と決めてください。

> **その後（2026-10-29）：** 優先課題だった曲面分割は、利用者の承認を得て第1段階（円柱・円錐）を反映した。[39_curved_face_split.md](39_curved_face_split.md)。

## 1. 再開時に最初に確認すること

1. この資料、[UX原則](07_ux_principles.md)、[構成](02_architecture.md)を読む。
2. `git status` と `git log` を確認する。未保存の利用者変更があれば保持する。
3. 下記の基準コミットが作業ブランチに含まれることを確認する。
4. 担当分野の詳細資料と回帰テストを読む。
5. 小さな再現モデルから始める。大規模処理や新しい設計を先に入れない。

| 項目 | 引継時の状態 |
| --- | --- |
| リポジトリ | `higeotoko/sketchBRep` |
| 作業ブランチ | `claude/sketchbrep-modeler-spec-4wnp0b` |
| 実装の基準コミット | `7120aaf`（ドックガイドのモーションまで実装） |
| PR | [#1](https://github.com/higeotoko/sketchBRep/pull/1)。mainへ未マージ |
| 今回の引継書作成前 | 実装はコミット・push済み。作業ツリーに変更なし |
| 最新実装のCI | [40656247630](https://github.com/higeotoko/sketchBRep/actions/runs/40656247630)。Linux・Windowsとも成功。引継書作成時に再確認 |
| Windows配布物 | [SketchBRep-windows-x64](https://github.com/higeotoko/sketchBRep/actions/runs/40656247630/artifacts/12801228893)（EXE・DLL・操作説明・サンプル・参考録画） |

**[旧引継書](23_handover_2026-10-25.md)は作業履歴です。** 冒頭のコミット・件数、途中の「未コミット」「CI確認予定」は当時の記録です。現在の確定状況は本書を入口にしてください。本書を含む文書コミットは基準コミットより後に追加されます。

Actions成果物は保持期限があります。Azureの期限付きダウンロードURLを再利用せず、上記Actionsから取得してください。失効時はCIを再実行します。今回の引継ぎは文書のみで、既存EXEを変更しません。

## 2. 利用者が重視していること

- **直感的で、見やすく、軽快な道具であること。** 見た目の追加だけで操作が不自然になる変更を避ける。
- 操作はSketchUpを参考にする。形状の正規データはOCCTのB-rep。表示メッシュへの置き換えはしない。
- 重い改修は先に難度・規模・処理量を調べる。大量処理で重くなるものを、軽度な修正として反映しない。
- 過剰なバウンス・発光・大きなプレビュー・大きく伸びるボタンを避ける。
- 作業完了時はWindowsのポータブルZIPが必要。ソースやLinuxビルドだけを渡して完了にしない。
- 計算テスト、GUI自動テスト、Windows実機での確認を分ける。CI成功をWindowsでの操作感の実証として扱わない。
- 有料AI APIや特定AIサービスへの依存を必須にしない。
- GUIとAIは共通の操作APIを使う。形状検証・失敗時の取り消し・Undo・既存の制限を維持する。
- 同じブランチへコミットを積む。amend・rebase・force-pushで履歴を書き換えない。

## 3. 一時引継ぎ中に実装したこと

| 分野 | 現在の実装・注意点 | 主なコミット／詳細 |
| --- | --- | --- |
| DXF／DWG | ASCII DXFの2D曲線を正確な辺・ワイヤとして取り込む。mm換算、レイヤ→タグ、部分取り込みの確認。DWGは利用者が選んだ外部ODA File Converter方式。ODAは別途必要 | `c6b3d39`、[24](24_dxf_dwg_import.md) |
| 曲線輪郭の面生成 | 円弧・曲線と直線の閉ループ、消しゴム後の曲線途中の交点を扱う。曲線区間を履歴に保持。隙間を無理に埋めない | `c6b3d39`、[25](25_curve_texture_followup.md) |
| テクスチャ | 再メッシュ前に画像表示を組み立てていた順序を修正。形状再生成・Undo/Redo時の画像表示を回帰確認。素材全般を改修したわけではない | `c6b3d39`、[25](25_curve_texture_followup.md) |
| 大規模DXF | レイヤ別表示バッチ、採番改善、移動時の誤った線整理を回避。入力中だけ平滑化を抑え、停止180ms後に戻す。正規形状は維持 | `11df073`、`944a03c`、[26](26_dxf_interaction_review.md) |
| 選択輪郭から面 | 右クリック「選択した線から面を生成」。レイヤの部分的な辺選択にも対応。1輪郭・4096辺まで。元図面を残す | `6f3f4c9`、[27](27_dxf_face_creation.md) |
| STL書き出し | 選択または表示モデルをバイナリSTLへ。既定精度0.1mm。元のB-repを保持。断面表示による切断は出力へ反映しない | `475ff2d`、[28](28_stl_export.md) |
| BREP／IGES | 正確な形状の入出力。素材・履歴を保存する形式ではない。既存STEP／3DMも維持 | `1c458ff`、`72b1923`、[30](30_geometry_exchange_formats.md) |
| 平面の曲線分割 | 円弧・スプラインが平面を境界から境界へ横切る場合に分割。傾斜面・ソリッドの平面も対応。曲面分割とは別 | `864fcc2`、[31](31_planar_curve_splitting.md) |
| 入出力の入口 | ファイルメニューのインポート／エクスポートへ集約。ダイアログの種類欄で対応形式を選ぶ。画像下敷き・PNGも統合 | `3529399`、[32](32_unified_import_export.md) |
| HTTP MCP | 起動中アプリへStreamable HTTPで接続。既存ローカルMCPも保持。接続画面名は「リモートMCP接続」 | `fd35cdb`、`e7ee8ed`、[33](33_chatgpt_connection.md) |
| 断面 | 保存済み断面と平面表示／カット／塗りつぶしを分離。配置後は選択ツールへ戻る。表示キャッシュを再利用 | `75c41b7`、`649d3c4`、[34](34_sections_and_tools_palette.md) |
| 室内へのズーム | 透視投影の壁手前で永久に止まる条件を修正。近接時は細かく、続ければ壁を越える。背後の面は拾わない | `7a0cb00`、[35](35_wheel_surface_navigation.md) |
| パレット／カーソル | 初期36pxボタン・24pxアイコン、幅で列数変更、縦スクロール。名前表示は設定。移動カーソルは四方向矢印を維持し中央に空間と基準点 | `553898a`、[36](36_palette_ui_and_cursors.md) |
| 白い帯 | ビューポート幅変更時の空／地面キャッシュの幅判定漏れを修正済み | `08b307b`、`paletteResizeKeepsBackdropAcrossViewport` |
| グループ選択 | グループ／コンポーネント全体は青い輪郭・元の面色。内部の生形状を直接選んだときだけ従来の面の点表示 | `0d5b249`、`595cab7`、[14](14_components.md) |
| 編集終了 | 選択ツールで修飾キーなしの空白ダブルクリックでも編集終了。Escと同じ確定経路。他の作図ツールの確定は維持 | `0d5b249`、[14](14_components.md) |
| 独立パネル／ドック | 共通PanelDock、テーマに調和する独自タイトル。狭い端判定。ドラッグ中に3D領域をリサイズしない | `308ea87`、[37](37_panel_docking.md) |
| アイコン／About | 小サイズ用の立方体構図・10サイズICO。Aboutはテーマ色の文字と通常QDialogで白い下帯を回避。元ブランド資産も保持 | `b930e43`、`src/app/Branding.h` |
| ドックのモーション | 軽量なガイドだけを補間。配置は即時、静止中は更新停止。下記参照 | `7120aaf`、[37](37_panel_docking.md) |

## 4. 直近のドッキング実装を触る場合

- 主な場所：`src/app/PanelDock.cpp`、`PanelDock.h`、`ToolPalette.cpp`、`MainWindow.cpp`。
- QtのQDockWidgetを置換していない。タイトルのドラッグと小さなDockPreviewを共通化している。
- 吸着開始16論理px、維持28px、アプリ外側許容8px。ガイドの領域は36px幅。
- 出現140ms／OutCubic／3px、通常離脱90ms／OutCubic、配置後160ms／OutCubic。
- 配置後はガイドの厚さを30%縮め、長辺の両端を各3px寄せて消す。実パネルの動きは補間しない。
- 遠い候補へは最大2枚の細いガイドをクロスフェード。同じ端への再入場は現在の濃さから反転する。
- DockPreviewの状態はHidden／Entering／Holding／Exiting／Settling。QVariantAnimationを使い、Holdingでは停止する。
- `holdForPlacement()` → `dockAt()`を1回 → 収束演出。`committingDock_`はQtの再配置時のhide/showによるガイド消去を防ぐ。
- `clearPreview()`の既定は即時消去。Ctrl／Esc／閉鎖／非アクティブ化は即時、通常の離脱だけフェードする。
- 別パネルのドラッグ開始時も前のガイドを消す。途中ドロップで全濃度へ跳ねない。
- `MainWindow::buildUi()`の`AnimatedDocks`無効を維持する。ドロップ時だけ実レイアウトを変える。
- 色はQPaletteから合成。1px境界・3px角丸。ぼかし・大きな影・常時タイマー・毎フレームの画像コピーはない。
- Ctrl吸着無効、Esc取消、ダブルクリック独立／復帰、許可エリア、タブ復元、閉鎖／再表示を維持する。

[約8秒の実GUI録画](images/panel-dock-motion.mp4)と[遷移フレーム](images/panel-dock-motion-phases.png)はクラウドLinuxで撮影。Windows実機の操作感の確認ではない。

## 5. 未解決・保留・誤解しないこと

### 優先して引き継ぐ課題

1. **曲面の線・コピー円弧による分割は未実装。** 利用者がClaude Codeへ戻して対応すると明言した課題。
   - 平面の曲線分割は実装済みなので混同しない。
   - 円筒の正確な線と円で一括分割する独立試作は成功した。ただし192本で9411面、検証込み約4.6秒。大量処理でも軽いという条件には達していない。
   - 非平面のFaceRef、曲面上の線と弦の区別、貼り付け経路、履歴・素材・面同一性・Undo・保存・応答性を設計する必要がある。
   - [調査資料と再現コード](29_curved_face_split_assessment.md)、`docs/repro/curved_split/`、測定CSVを参照。試作をアプリに接続したものではない。
2. **テクスチャは残る懸念。** 再表示順序の修正は済み。全編集操作での割り当て維持、UV、曲面マッピング、長時間運用の完全解決とは報告していない。
3. **大規模形状の既存課題。** [監査](22_perf_audit_2026-10-24.md)のPERF-15（面追加の全体縫い直し）、PERF-03（単一部材計算の中断粒度）、PERF-05／07／10／16／17等は、その後の修正有無を個別に確認して計画する。本作業で一括解決していない。

### 確認が残ること

- Windowsの実GPU・100/150/200%表示・複数モニターを跨ぐパネル移動、実際のドラッグの滑らかさ。
- 新しいアイコンのWindowsタスクバーでの実表示。OSのアイコンキャッシュと画像生成の問題は分ける。
- 実ODA File ConverterによるDWG解読。自動テストは代替変換プロセスの契約を検証している。
- 提供DXFの全倍率・全領域・長時間の操作。既測定では初回処理等に100〜220msの引っ掛かりが残る。常時滑らかと保証しない。
- 20mm壁の通過は理論上の見解のみ。実GUI回帰で使った外壁は200mm厚。
- 断面は全体で有効カット1つ。グループごとの多重カット、断面線の2D出力、平面の直接ドラッグ／回転は未実装。
- 円弧分割後の膨らみの数値再入力は対象外。曲線分割後のデータは更新版で開く。
- DXFの文字・寸法・ハッチ・バイナリDXF・非均等倍率ブロックなどは未対応。取り込み直後に自動で全面を張る仕様ではない。

### 利用者による訂正

- 「グループ内の線が外のモデルを分割する」は、利用者から**干渉していなかったようだ**と訂正された。未確認の干渉不具合として扱わない。今回変えたのは選択表示と編集終了経路。
- パレット幅変更後の白い帯は`08b307b`で修正済み。今回の引継ぎ時点で未修正扱いにしない。
- 移動カーソルの四方向矢印は残す。中央の穴と点で狙いを見せることが利用者の希望。

## 6. AI接続で維持する境界

- Claude Code／Codex CLIはローカルstdio MCP。通常はHTTPSトンネル不要。設定例は[AI連携](06_ai_integration.md)。
- 外部クラウド用はリモートHTTP MCP。通信実装は`McpHttpServer`、実行は`AiBridge::execute`から共通APIへ渡す。
- 起動中アプリ経由ではファイルの保存・開く・取り込み・書き出しを禁止。ヘッドレスCLIのI/Oと混同しない。
- ループバック待受、秘密URL、Host／Origin検証、要求上限、直列実行を維持する。OAuthや利用者別認可は実装していない。
- HTTPS暗号化と認証は別。秘密URLのみで十分なアクセス制御があると説明しない。
- ChatGPTのプラン条件は2026-10-26の**利用者提供の公式記事要約**に基づく。本環境から本文は403で独立取得できなかった。最新条件は公式で再確認する。
- 利用者の実測は「PlusのWindowsデスクトップで登録成功、通常チャットで固有ツールが未検出」。全プランの恒久仕様や編集成功として扱わない。
- 標準Python MCP SDKによる一覧・作成・状態取得・Undoの成功と、ChatGPT実アカウント＋公開HTTPS経由の操作成功は別。後者は本作業者が確認していない。
- UI名は「リモートMCP接続」。ZIPの`CHATGPT_CONNECTION.md`という互換ファイル名だけ残している。
- プラン判定・OpenAI APIキー・有料AI API依存をコードに追加しない。

## 7. ビルドと検証の再現

### このクラウド環境

```bash
set -e
cd /workspace/sketchBRep
source /workspace/deps/activate.sh
cmake --build build -j4
ctest --test-dir build -LE gui --output-on-failure
xvfb-run -a -s '-screen 0 1600x1000x24' ctest --test-dir build -L gui --output-on-failure
```

`/workspace/deps/activate.sh`はこの環境だけの依存設定。リポジトリや配布物には含まれず、別PCでは使えない。通常の構成はREADMEとCIを参照する。openNURBSの固定取得を使うため、初回構成ではネットワークまたは取得済みソースが必要。

### 引継時点の検証結果

| 範囲 | 結果と限界 |
| --- | --- |
| 最新実装CI Linux | ビルド・CTest全体（GUI含む）成功 |
| 最新実装CI Windows | ビルド・非GUI CTest・100/150/200%のラスターUIテスト・DLL依存確認・配布CLI起動・ZIPアップロード成功。実3D GUI対話操作を実機確認したわけではない |
| クラウドCore | 351件成功、任意ベンチ4件スキップ（直近のCore全体確認）。GUIのみの後続変更は最終CIでも検証 |
| クラウドGUI全体 | 最新モーション作業で153成功・3失敗・7スキップ。既知の下記3件のみ |
| モーション個別 | 出現／消失／静止、素早い出入り、候補切替、途中ドロップ、Ctrl／Esc、連続パネル・閉鎖を100/150/200%で確認 |
| 実MainWindow | 明暗テーマ、配置1回、配置後の消失、文書不変を確認。120回ドラッグ移動約0.20〜0.26秒、ビューポートリサイズ0回。静止ガイドの再描画0回 |
| 見た目 | Linux実GUIの参考録画・遷移画像を作成しフレームを確認。Windows実機での手触りは未確認 |

ローカルGUIで従来から再現する失敗：

- `topEdgesCopiedDownDivideTheSides`：辺選択と面選択。変更前コードでも再現。
- `moveToolLiftsRidgeIntoGable`：体積の期待値差。変更前コードでも再現。
- `autosaveWritesInTheBackground`：既定の回復保存先の書き込み制約。書き込み可能な`SBR_RECOVERY_DIR`を指定した単独実行は成功。全体テスト中に別ケースが同変数を解除する点に注意。

クラウドはQt 6.8.2／OCCT 7.8.1、Linux CIは別バージョン、Windows CIはconda環境。これらの環境差を無視して既知失敗を今回の回帰や製品仕様と決めつけない。テストを無効化して緑にする対応もしない。

### 分野別のテスト

| 場所 | 主な確認 |
| --- | --- |
| `tests/test_tool_palette.cpp` | コンパクト配置、タイトル、DPI、ICO、ドック入力・ガイドの寿命と停止 |
| `tests/test_gui.cpp` | 実3D画面。下記の個別ケースなど |
| `tests/test_drawing_import.cpp` | DXF取り込み、右クリック面生成、ODA代替プロセス、任意の実図面性能 |
| `tests/test_dxf.cpp` | DXF形状、単位、部分取り込み、選択辺と履歴 |
| `tests/test_split_across.cpp`等 | 正確な平面分割、履歴・素材・Undo・保存 |
| `tests/test_geometry_exchange_gui.cpp`／`test_stl_gui.cpp` | 種類選択、入出力、拡張子 |
| `tests/test_chatgpt_transport.cpp` | HTTP MCP、要求制限、拒否、停止・再開 |

```bash
# 上記でビルド済みの同じ環境で実行する
xvfb-run -a -s '-screen 0 1600x1000x24' build/tests/sbr_gui_tests \
  panelMotionSettlesAfterNativeDock panelDragPreviewKeepsTheModelViewStill \
  groupsAndComponentsSelectWithOutlines emptyDoubleClickExitsOnlySelectionContext \
  paletteResizeKeepsBackdropAcrossViewport sectionToolboxControlsAreIndependentAndCached \
  wheelZoomThroughWallKeepsOrientationAndInputConsistency brandingResourcesLoad
```

Qt Testの関数名・件数は更新で変わるため、必要なら`build/tests/sbr_gui_tests -functions`で確認する。Xvfb接続失敗は別ディスプレイで再試行し、アプリのアサート失敗と分ける。

録画を再取得する場合は`SBR_MOTION_CAPTURE_DIR`でGUIテストの画面キャプチャを指定できる。小さなオーバーレイの合成を含む記録にはコンポジターが必要。撮影に使ったxcompmgr／ffmpegはQA用で、アプリ依存には追加していない。

## 8. データ・保存互換性・運用

- 利用者提供の約20MBのDXFは私的な実図面。リポジトリ・配布物へ含めていない。引継書だけで元データを復元できるわけではない。
- 実図面ベンチは`SBR_DXF_BENCH`へローカルパスを渡す。実図面がなければ`tests/data/dxf`の小さな公開フィクスチャから再開する。
- 実図面は対応線図35880本・141レイヤーオブジェクト。約3.56秒の取り込み、回転中央値約18msはクラウドの既測定値。Windows GPUの保証値ではない。
- SBRは履歴を保つ正規保存。STEP／IGES／BREP／STLだけでは元の編集履歴を戻せない。
- `CurveIntervalOp`、正確な曲線を持つ`SplitOp`／`FacePart`、断面設定等の拡張を旧EXEで読み込めるとは保証していない。新機能を含むSBRは更新版を使う。
- 基本の変更経路は`Modeler`→`CommandStack`。失敗は全取り消し、1操作はUndo1回。直接Documentを変更する特別経路を増やさない。
- CIは`.github/workflows/ci.yml`。文書だけの変更は通常スキップ。Windowsの3D GUIテストはLinuxと同じ実行範囲ではない。
- `gh`の認証とapi.github.comへの接続はこのクラウドでは利用できなかった。git pushと公開GitHub HTMLは利用できた。認証情報や期限付きURLを文書へ保存しない。
- `/tmp`の計測ログや補助スクリプトは永続的な成果物ではない。本書・詳細docs・テスト・コミットを再開の根拠にする。

## 9. Claude Codeへ渡す開始文

以下をそのまま渡せます。

```text
SketchBRepの主担当を再開してください。
作業ブランチは claude/sketchbrep-modeler-spec-4wnp0b です。
まず docs/38_handover_to_claude_2026-10-29.md を読み、現在のHEADと
作業ツリーを確認してください。実装基準は7120aafで、その後の文書追加も保持してください。
旧引継書の「未コミット」「CI予定」を現在の状況として扱わないでください。
現時点では新機能を勝手に始めず、未解決事項を把握して次の依頼に備えてください。
曲面分割は調査のみで保留、平面の曲線分割は実装済みです。
直感的な操作・軽快さ・正確なB-rep・Undoを維持してください。
実装を変更する際は関連テストとLinux/Windows CIを確認し、Windows ZIPを渡してください。
計算テスト、GUI自動確認、Windows実機確認は分けて報告してください。
履歴の書き換えやmainへの無断マージはせず、同じブランチへ積んでください。
```
