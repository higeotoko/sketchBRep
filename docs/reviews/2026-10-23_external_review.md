# SketchBRep 読み取り調査・改善提案
調査日：2026年10月23日（日本時間）／対象：GitHub公開ソースとGitHub上の開発状況

## 1. 結論と調査範囲

**SketchBRepは、目指す思想の骨格をすでに実装している。次に優先すべきは機能数ではなく、対象の同一性、操作の原子性、重い処理からのUI分離である。** SketchUp風の直接操作をOCCTの正規B-repへ接続し、GUIとAIが共通のModeler・Undoを使う方針は維持したい。PartModeへの置換や機械CAD機能の追随は勧めない。

最初に注意すべき公開状態の違いがある。

| 対象 | 調査した固定点 | 公開状態 |
|---|---|---|
| SketchBRep main | 5716215170b39f17fade2a044465249beeda3b0c | READMEの見出しだけ。実装済み機能をmainの機能として扱えない |
| SketchBRep 実装 | **7bb03562d595ad8134fd6f875ca3292ffd49db24** | Draft PR #1の開発ブランチ。最初にbd908abを読み、調査中の追加1コミット（消しゴムのドラッグ操作）も差分と変更4ファイルを確認 |
| PartMode | fe88558a34c8d7a3e03b34c2a374ba5ce6febe9f | main。主要比較対象 |
| SolidCraft | edf57cec293dcea301201f1d5173b33e16351466 | main。共通コマンド基盤・検証・配布の補助比較 |

参照：[SketchBRep main][main]、[Draft PR #1][pr1]、[PartMode][part]、[SolidCraft][solid]。

### 調査の方法と確度

- SketchBRepの実装ブランチのテキストファイル124件を取得。README、設計資料01〜15、コア、GUI、API/MCP、テスト、CMake、CIを対象に、主要経路と制限を照合した。取得した全ファイルの全分岐を検証したという意味ではない。
- SketchBRepのIssue/PR一覧、PR本文・コメント、最近20件のCI実行と代表ジョブ、Releasesを確認。通常Issueは一覧取得時点で0件、Draft PRは1件、Releaseは0件だった。
- PartModeはREADME、アーキテクチャ、エージェント資料、関連実装・テスト一覧・CI、公開中Issue11件とPR一覧を確認。SolidCraftは関連する設計・コマンド・カーネル・MCP・CI/配布資料、直近のIssue/PRを抽出確認。SolidCraftの全Issue数千件を精査したものではない。
- 本調査は**静的なコードレビュー**。アプリの手動操作、ローカルビルド、テスト実行、Rhinoとの実機比較、GPU・メモリ計測はしていない。既存テストの存在、GitHub CIの結果、文書中の実測値を区別した。
- 「実装あり」はコードの経路を確認した意味。「未対応」は明示的制限または拒否経路。「未確認」は証拠不足であり、未実装とは異なる。
- リポジトリのソース・コミット・Issue/PR・設定は変更していない。作成物は本レポートと調査記録のみ。

## 2. 実装済み・未対応・課題の整理

| 分野 | 現在の実装 | 未対応／残る課題 | 主な根拠 |
|---|---|---|---|
| 作図・推論 | 線・矩形・円・円弧・NURBS、閉輪郭の自動面化、交差線の面化、正規形状からのスナップ、軸固定・追跡・ヒステリシス | 密集形状・透視・高DPIでの体験品質は手動比較未確認。曲面上の分割は未対応 | [Tools.cpp][tools]、[Inference.cpp][infer]、[UX追補][ux] |
| Push/Pull・直接編集 | プロファイル押出、平面の押し引き、円柱半径変更・曲面オフセット、面分割・内周穴、Follow Me、平面を維持する頂点／辺移動、面削除・穴ふさぎ | 任意の非平面変形は拒否。上流変更で参照が消える／曖昧になるケースは残る | [Operations.cpp][ops]、[Recipe.cpp][recipe]、[変形仕様][reshape] |
| 選択・グループ | 面／辺／全体、左右方向の囲み選択、グループ編集、コンポーネントのその場編集、個別寸法・鏡像・独立化・軸変更 | **入れ子コンポーネントは明示的に拒否**。個別寸法で面数が変わると面素材の対応がずれる制限 | [Modeler.cpp:2880][component-code]、[コンポーネント仕様][components] |
| 消しゴム | 区間削除・面統合・開いた形状への変化・線を隠す、最新コミットでなぞって一括削除 | 最新の一括操作は、再探索が元オブジェクト以外へ広がる問題を持つ。P0-01 | [Tools.cpp:3571][eraser] |
| カメラ | カーソル下を中心にオービット、Shift+中ボタンでパン、カーソル位置ズーム、標準ビュー・シーン・断面 | 複雑モデルでの入力遅延、Windows実機のDPI・ドライバ差は未確認 | [Viewport.cpp:296][camera]、test_gui.cppのorbitKeepsPointUnderCursorFixed等 |
| B-rep | OCCT、妥当性・閉鎖・正体積検証、幾何参照のFaceRef/SurfaceRef、描画用形状の分離 | Rhino級の総合品質を保証する証拠はない。許容差分散、履歴に基づく面・辺の永続参照は不足 | [Validation.cpp][validation]、[FaceRef.cpp][faceref]、[Types.h][types] |
| 人間とAI | GUI→Modeler、MCP/CLI→CommandApi→同じModeler。共通Undo、batch、参照、preview、expected_revision、提案の適用／却下、検索・集計・edit_each | GUI全体がJSON APIを通る構成ではないが、共通内部APIという要件は満たす方向。I/Oとプレビューの境界、型・エラー契約、実行中の排他を強化したい | [CommandApi.cpp][api]、[AiBridge.cpp][bridge]、[AI設計][ai-doc] |
| 大規模性能 | ID/親子索引、変更ログ、増分表示・選択、形状ごとの表示コピー共有、局所スナップBVH、読取キャッシュ、共有保存、遅延レシピ照合 | 真の幾何ワーカー未導入。AIS presentation共有・GPU instancingは次段。BVHは編集後に全体再構築。メモリ上限の計測不足 | [性能資料][perf]、[Viewport.cpp:523][display-code]、[Inference.cpp:109][infer-bvh] |
| 保存・交換 | .sbr共有形状表、保存検証、自動保存・世代バックアップ・復旧、STEP、openNURBS経由3DM入出力 | 3DMの不対応・変換失敗の通知、単位別許容差、複雑な外部モデルの検証が不足。STEP読込は階層を平坦化する | [NativeFormat.cpp:869][save]、[復旧仕様][recovery]、[Rhino3dm.cpp:355][rhino]、[ShapeIO.cpp][step] |
| CI・配布 | Linuxコア＋Xvfb GUI、Windowsコア、依存DLL検査・PATHを絞った配布物smoke、Windows artifact | Windows GUIを除外。依存版の再現性、Linux配布物、正式Release、性能回帰ゲートを整えたい | [ci.yml][ci]、[tests/CMakeLists.txt][tests-cmake] |

**古い記述への補正：** PR本文の「コンポーネント編集・中断・3DMが未対応」「CIが手順前に失敗」は、固定した最新実装の全体像としては古い。分割線保持もOperations.cpp:146のKeepShapeとRecipe.cpp:243以降に実装され、テストGate3.SplitLineSurvivesLaterPushPullがある。これらを新規実装項目に再計上しない。[PR本文][pr1]、[最新UX追補][ux]。

## 3. 競合との比較

記号：実＝実装経路を確認、部＝制限付き／一部、資料＝主に公開資料による確認、未確＝今回十分に確認できない。機能数やテスト数は操作品質・精度の順位ではない。

| 評価軸 | SketchBRep | PartMode（主要比較） | SolidCraft（補助比較） | 採るべき示唆 |
|---|---|---|---|---|
| 設計の主役 | 直接作図と押し引き。建築向けの軽い操作 | 拘束スケッチと履歴を中心とする機械CAD | パラメトリック履歴中心の機械CAD | SketchBRepの直接操作を主役に保つ |
| 推論・スナップ | 実：3D形状、画面距離、追跡、軸固定、ヒステリシス | 実：スケッチの一致・水平垂直推論。SketchUp相当の3D連続操作は未確 | スケッチ・拘束は資料。SketchUp相当の体験は未確 | PartModeのスケッチ推論をそのまま置換しない |
| Push/Pull・直接編集 | 実：平面／曲面、面分割、辺・頂点変形の制限 | 実：push-pull / replace-face / delete-faceを型付き履歴に記録 | 機能・コマンドを備えるが、この比較で直接操作の全経路は未精査 | 操作の意味・Undo・失敗条件を共通にする |
| グループ・部品共有 | 実、入れ子コンポーネントは未対応 | アセンブリ・再利用部品あり。ただし入れ子のEdit in ContextはIssue #9で未対応 | アセンブリ／jointsは資料 | 入れ子対応は段階的に。競合も万能ではない |
| 幾何カーネル | OCCTネイティブ | 同系統のOCCTをreplicad＋WASMで利用 | truck＋独自処理、公開境界で型を隔離 | 競合へのカーネル変更に精度上の根拠はない |
| 正確性の証拠 | BRepCheck、体積、閉鎖、回帰・往復テスト | 正規ドキュメント、B-rep evidence、persistent topology、輸出決定性テスト | action matrix、oracle、Boolean guard | 成功した見た目ではなく、意図と幾何不変条件を検証 |
| 人間とAIの共通性 | 実：共通Modeler／CommandStack | 実：同じschema-5とカーネル。エージェントはCadCommandService | 実／設計：engineのcommand registryにUI・CLI・MCPを接続 | 共通内部APIを維持し、型付き仕様を一元化 |
| プレビュー・競合 | 実：scratchコピー→差分→同じコマンド再実行、revision照合 | 実：previewIdを保存してrevision照合後commit | 共通engine・headless/bridge。全プレビュー規約は未確 | 適用対象・文書世代・提案内容を一緒に固定する |
| 非同期境界 | 部：進捗・取消、アイドル時分割。幾何はUIスレッド | 実：ブラウザWorker、headlessもworker_threads | 機能別jobや増分評価を持つ。全幾何のUI分離は未確 | Worker境界の考えを採用、UI技術の全面移植は不要 |
| 建築大規模性能 | 増分同期・部品共有・BVHあり。実GPU／大規模実建物の検証不足 | 部品・アセンブリ、再構築計画あり。建築規模の定量比較は未確 | 増分評価あり。直近PRにもフレームごとの再計算問題 | 同一コーパス・同一機で測るまで順位を付けない |
| Windows/Linux | ネイティブ両OS CI。GUIはLinuxのみ、Windows artifact | ブラウザ／Node、CIはUbuntu。ネイティブWindows配布の比較対象ではない | Windows/Linuxほか配布パイプラインあり | 当面2OSに絞り、配布物の起動検証を増やす |
| 課金・接続 | 外部MCPクライアントを接続。内蔵モデルAPIを必須にする根拠なし | ブラウザローカル作業と、任意のホスト型接続を分離 | MCPのheadless/bridgeあり | **CAD本体とAI接続を分離し、モデルAPI課金を必須にしない** |

比較根拠：[PartMode architecture][part-arch]、[推論][part-infer]、[直接編集][part-direct]、[エージェントサービス][part-agent]、[headless worker][part-worker]、[SolidCraft architecture][solid-arch]、[MCP][solid-mcp]、[action matrix][solid-matrix]、[Boolean guard][solid-guard]。

### 比較対象から、そのまま採用しないもの

- PartModeのアカウント・クラウドrelay・ホスト型の権限体系は、ローカル共同編集に必須ではない。採用対象は「型付き操作、分離プレビュー、明確な権限と文書境界」である。
- PartModeもIssue #12で、MCPの外側のtool schemaが配列を十分に宣言していない問題が報告されている。「内部が型付き」だけで実クライアントとの相互運用性は保証されない。SketchBRepではtools/listから実呼出まで通して検証する。[Issue #12][part-issue12]。
- PartMode PR #1はclosedだが**未マージ**。外部の図面バックエンド実験の報告として参考にし、mainに入った機能の証拠として扱わない。[PR #1][part-pr1]。
- SolidCraftのPR #5223は、比較的大きな設計で毎フレーム非表示スケッチまで境界を再計算していたと報告している。約60ms/frame等はPR著者の測定であり、修正後の同条件測定は本文にない。未マージPRの解決策を実装済みと数えない。[PR #5223][solid-pr5223]。
- SolidCraftのSTEP表示問題も利用者報告で、こちらで再現したものではない。競合の機能表から「読める＝すべて正しい」と推論しない。[Issue #5210][solid-issue5210]。

## 4. アーキテクチャの強みと弱み

### 強み

1. **形状の正本がB-repである。** 描画用コピーを分け、表示品質を変えても形状を変えない。スナップも正規形状に戻る。これが「軽い操作」と「精度」を両立する土台である。[DisplayMesh.cpp:19][mesh]。
2. **共通の編集基盤がすでにある。** GUIの道具もCommandApiもModelerを呼び、CommandStackに変更を記録する。Undoは形状の再計算ではなくbefore/afterを戻す。モデルAPIをGUIと別に新設する必要はない。[CommandStack.cpp][stack]、[CommandApi.h][api-h]。
3. **曖昧なら拒否する設計判断がよい。** 新しいFaceRefは内点・範囲・分割線の側を使い、決められなければ拒否する。失敗より「別の面を正常に編集できてしまう」方が悪いという基準を一貫させたい。[FaceRef.cpp:123][faceref]。
4. **建築らしい反復配置の最適化が始まっている。** 定義と配置、正規形状と表示、変更ログと差分同期が分かれている。全面レンダラ置換より、現在の境界を活用できる。[性能資料][perf]。
5. **ユーザー操作の意味をテストにしている。** 直後の数値再入力、Esc、作図途中のカメラ、提案適用、窓の開口などのGUI／シナリオテストがある。単なる関数単位の検証より思想に近い。[test_gui.cpp][gui-tests]、[test_scenarios.cpp][scenarios]。

### 弱み

- **「共通の入口」と「共通の規約」がまだ完全には一致しない。** GUIはModeler直接、APIはJSON handler。schemaはあるが実行時は各handlerの手書き検査が中心。preview・batch・外部I/O・GUI拡張の組合せ規約を機械的に守らせたい。[CommandApi.cpp:681][api-reg]、[CommandApi.cpp:1923][api-run]。
- **面・辺の参照が幾何近傍や面番号に依存する箇所が残る。** FaceRef旧形式、配置の素材、最新消しゴムの再探索は、同じ「意図した対象」の問題である。1つの参照設計として整理すべき。
- **幾何演算とイベント処理が同じスレッドにある。** 取消ダイアログは実装済みだが、processEventsで反応させる構成はワーカー実行とは異なる。socketやtimer等の再入も検証対象になる。[ProgressDialog.cpp][progress]、[AiBridge.cpp:59][bridge]。
- **増分表示はあるが、レシピ再生成は履歴を組み直す。** 深いBooleanや共有定義更新のコストは別問題。増分表示があることだけで全編集が局所時間になるとはいえない。[Recipe.cpp:243][recipe]。
- **ファイルが大きく責務が集中している。** 調査初期点でModeler.cpp約3,660行、Tools.cpp約3,706行、MainWindow.cpp約2,395行、CommandApi.cpp約2,101行。最新コミットでToolsはさらに増加。行数自体を欠陥とせず、変更理由の異なる責務を分ける指標とする。
- **文書とコードの時点が揃わない。** 初期アーキテクチャの形式version 2と現在の共有保存、PR本文とGate追補などが混在する。AIが古い制限を信じて重複実装する原因にもなる。

## 5. 優先順位付き改善案

工数は、コードに慣れた開発者1名の実装・レビュー・必要な回帰検証を含む**概算人日**。既存不具合やOS差で増える。P0は正しい対象・データ保全・プレビュー契約、P1は主操作の品質と規模対応、P2は利用範囲の拡張とする。

### P0-01：消しゴムの再探索を「選んだ対象の系譜」に限定する

**観測：** 最新Tools.cpp:3591〜3615では、各Markに対してドキュメント全体を走査し、記録点から最短の辺を探す。元オブジェクトがあっても同じ親の別物を候補に含め、元オブジェクトが消えた場合は親の絞込みもなくなる。等距離候補の拒否もない。[該当コード][eraser]。

**問題と確度：** 元とは別の重なる辺を選び得る探索規則をコードで確認。実アプリで誤削除を再現したという主張ではない。既存の新規GUIテストは離れた4本の線であり、重複・入れ子・最初の削除でIDが変わるケースを扱わない。

**提案：** コアにerase_stroke相当の操作を置き、元ID＋辺参照＋編集コンテキスト＋演算後の後継IDを追う。後継が一意でなければ拒否し、他のオブジェクトへ近傍探索を拡大しない。1ストロークの部分成功を認めるかを明示し、標準は全体取消可能にする。

**期待効果：** なぞった線だけが消える予測可能性と、GUI/AIの同値性。**工数：3〜6日。リスク：** 面統合でIDが変わる経路との調整。**判断：即採用。** 重複辺、同じ点の別部材、別グループ、ロック対象、途中失敗、Undoを受入条件にする。

### P0-02：previewとatomic batchから外部I/Oを排除する

**観測：** CommandApi::OptionsはallowFileCommands=trueが既定。runPreviewはそのoptionsをscratch側へ引き継ぎ、runBatchはコマンドを通常実行する。save/export等をpreview内で呼ぶと、モデルコピーは分離されても実ファイルへの書出経路は残る。atomic batchも、書出後に後段が失敗してもファイルはUndoされない。[CommandApi.h:23][api-h]、[runBatch:1737][batch]、[runPreview:1898][preview]、[dispatch:1939][api-run]。

**範囲：** アプリ内AiBridgeはallowFileCommands=falseなので、この経路はそこで遮断されている。主に既定optionsのCLI／ヘッドレス利用の契約問題。ファイル書出の再現実行はしていない。

**提案：** プレビューには常に外部I/O禁止の実行コンテキストを渡す。atomicモデル編集のbatchもExternalIoを事前拒否する。exportは確定済みrevisionを指定する別操作にする。操作ごとのeffect／許可／プレビュー可否をregistryで共有する。

**期待効果：** 「試すだけで書き換わらない」「失敗したら元通り」を本当に守る。**工数：1〜3日。リスク：** I/O入り既存スクリプトの互換性。**判断：即採用。** CLIの通常save/exportは残し、変更点は複合操作内の規約に限定する。

### P0-03：3DMの部分読込を通知し、成功の意味を明確にする

**観測：** fromOnBrepは変換できない面・trim等をcontinueし、read3dmも非B-repと変換失敗をcontinueする。読めたものが1つあれば結果を返せるため、失われた要素数・理由が利用者へ届かない。[Rhino3dm.cpp:355][rhino]、[read3dm:517][rhino-read]。Modeler::addImportedの再構築は、すでに省略された元要素の存在を回復できない。[Modeler.cpp:3482][import-code]。

**提案：** ImportReportに総数、変換成功、不対応、失敗、修復内容、元の単位を持たせる。不対応種別の省略とB-rep変換失敗を区別し、後者を含む読込は標準で停止、明示的な部分読込なら残す。面数・閉鎖性・面積・体積・配置の検証を追加する。

**期待効果：** 「開けたが一部が欠けた」を成功に見せない。**工数：3〜7日。リスク：** これまで読めたように見えたファイルが警告／停止になる。**判断：即採用。** 既存の箱・円柱・球・開口・名前／タグ往復テストに、欠損trimと混在オブジェクトを足す。

### P0-04：長時間演算中のモデル書込みを一元的に直列化する

**観測：** GuiProgress::UserBreakはprocessEventsを呼び、AiBridge::onReadyReadは即座に同じModelerを操作する。モーダルダイアログだけではsocket・timerの処理順序を規定できない。再入による破損は今回未再現だが、明示的な編集実行ゲートを追加すべき構造である。[ProgressDialog.cpp:36][progress]、[AiBridge.cpp:59][bridge]。

**提案：** ModelSession側に実行中状態とキューを置く。幾何演算中の新規書込みはbusyか予約として返し、読取は確定済みsnapshotに限定する。取消・文書切替・自動保存・提案適用の順序を定義する。取引をRAIIで管理し、例外でもgroupが残らないようにする。

**期待効果：** 人間とAIの同時利用時も「何がいつ確定したか」が明確になる。**工数：2〜5日。リスク：** 既存GUI hookの実行順。**判断：採用。** ワーカー化を待たず、小さな排他・再入回帰から始める。

### P1-01：長時間幾何演算・読込検証・自動保存をジョブ化する

**根拠：** 取消はあるがUIスレッドで実行。15msのレシピ照合はwhileの入口で時間を見ているだけで、verifyRecipes内の1演算や最初の定義全走査を15msで中断できない。自動保存もGUIスレッド。[MainWindow.cpp:2261][verify-ui]、[Modeler.cpp:3395][verify-core]、[復旧仕様][recovery]。PartModeのworker境界が参考。[part-worker][part-worker]。

**提案：** 不変の入力snapshot→幾何worker→候補結果→文書世代/revision照合→短い確定、にする。OCCT形状への同時書込みと表示テッセレーションを避け、最初は単一worker。非協調的な処理の強制停止が必要になった段階で別プロセスを検討する。

**効果：** カメラ・選択・取消の応答を維持。**工数：15〜30日。リスク：** OCCT共有形状・スレッド局所進捗・寿命管理。**判断：段階採用。** まず読込検証かpreviewを縦に1経路移し、UIを全面改修しない。受入は「遅い1演算中でも入力が応答し、旧文書への結果を捨てる」。

### P1-02：面・辺・素材を共通の永続参照に寄せる

**根拠：** 新FaceRefは曖昧拒否するが、旧形式は最寄り重心へ戻る。平面自体が移る上流変更は追えないケースがある。コンポーネント個別寸法の素材対応は面番号依存の制限が残る。[FaceRef.cpp:149][faceref]、[コンポーネント仕様の制限][components]。

**提案：** feature/opのID＋生成役割＋OCCTのGenerated/Modified/IsDeleted等の対応を保存し、幾何signatureは検証／修復候補にする。旧参照が多義なら自動で選ばず修復UIへ。素材・隠し辺・Push/Pull・選択・AI参照を同じresolverへ接続する。

**効果：** 寸法変更後にも同じ壁面・素材・開口を扱える。**工数：10〜25日。リスク：** 形式移行と、Boolean／Unify後の履歴対応。**判断：段階採用。** 既存の曖昧拒否を弱めない。PartModeの「永続名がある場合は失効後にsignatureで勝手に別物へ繋がない」規則を参考にする。[resolver][part-resolver]。

### P1-03：許容差と幾何の成功条件を製品契約にする

**根拠：** kLinear=1e-4mm、最小寸法=1e-3mmに対しFaceRefは1e-3、縫合は1e-6／1e-3等が分散。3DMは元単位の面を縫合した後でmmへスケールしている。Booleanは妥当性と正体積を検査するが、意図した領域だけが変わったかは別問題。[Types.h][types]、[Operations.cpp:280][boolean]、[Rhino3dm.cpp:355][rhino]、[Validation.cpp][validation]。

**提案：** 接続・平面判定・最小feature・import修復・比較誤差を別ポリシーとしてmmで定義する。単純に1値へ統合しない。3DMは正規単位へ変換してから修復するか、元単位へ換算した許容差を渡す。Booleanに体積の増減則・変更範囲・想定solid数の確認を追加。自己交差検査は高リスク演算や診断モードに重点化する。

**効果：** mm／inch／微小モデルで同じ基準を守る。**工数：5〜12日。リスク：** 既存形状の合否変化・検査コスト。**判断：採用。** Rhino級という語を、接線接触・ほぼ共面・薄肉・巨大座標・周期面・trim・G0/G1/G2の測定項目へ分解し、対応範囲を宣言する。自由曲面の全面対応は別段階。

### P1-04：コマンド仕様・effect・エラーを一元化する

**根拠：** CommandDefにschemaとhandler、別のbuiltinEffect、Modelerの直接呼出が並存する。現在の共通Modelerを捨てる理由はない。[registry:681][api-reg]、[effectOf:1980][api-effect]。

**提案：** 型付きCommandSpecに引数、効果、編集コンテキスト、プレビュー可否、取消可否、戻り値を集約する。GUIは型付き内部呼出のままでよく、JSON文字列化を強制しない。MCP schemaと実行時検査を同じ定義から生成。エラーには安定code、対象ID、理由、修正案を持たせ、翻訳文字列の解釈に依存しない。

**効果：** GUI/APIの振舞いずれを抑え、新しい道具を一度の実装で公開できる。**工数：5〜12日。リスク：** 全操作を一括移行すると差分が大きい。**判断：Push/Pull・移動・消しゴム・batchから採用。** 配列・nullable・追加引数・実MCPクライアントを通す検証を行う。

### P1-05：提案を文書世代・対象集合・内容の組で固定する

**根拠：** expected_revisionは実装済み。ただしscratchで試したコマンドを確定時に再実行し、新規IDは変わる。テストもIDを除いて差分を比較する。history.clear／loadはrevisionを0へ戻す。[runPreview][preview]、[test_transactions.cpp:158][transactions]、[Modeler.cpp:3416][load-code]。

**提案：** document/session epoch＋単調な変更世代＋proposalId＋command hash＋対象集合を固定する。Undo用の履歴位置と競合検出用世代を分ける。query結果を適用時に再評価する場合は対象数だけでなくID集合も照合する。プレビューIDと確定IDの対応を返す。

**効果：** 開き直し・Undo/Redo・集合編集でも「見た案」と「実行した案」を照合できる。**工数：4〜8日。リスク：** 既存CLIのrevision契約。**判断：採用。** epochを足す小さな互換拡張から行う。現行で文書間誤適用が実際に起きたとは断定しない。

### P1-06：建築モデルの性能・メモリを回帰基準にする

**根拠：** sbr_benchはinstances／unique、照会・保存等を測る。GPU時間、RSS、VRAM、履歴による保持量の評価が不足。CommandStackは件数上限500で、形状・レシピの保持バイト数では制限しない。BVHは編集後に再構築。[bench][bench]、[CommandStack.h:51][stack-h]、[性能資料][perf]。

**提案：** 反復建具・unique部材・深いグループ・曲面・画像素材・複雑Booleanのコーパスを用意。1k/10k/100k配置で初回表示、オービット、snap p95/p99、局所編集、定義一括編集、保存／復旧、Undo/Redo、peak RSS／VRAMを測る。履歴は参照共有を考慮した概算バイト予算を導入する。

**効果：** 体感とメモリ劣化を防ぎ、必要な最適化を選べる。**工数：計測4〜8日、最適化は別。リスク：** GPU・CIのばらつき。**判断：即着手。** 初期の仮目標はsnap p95≦8ms、実機オービットp95≦33ms、主入力応答≦100msとし、対応機・fixtureを定義して調整する。これは現性能の保証ではない。

最適化の順番は、計測→AIS presentation共有→定義と配置の2段BVH/refit→選択・propertiesの大規模対策→必要ならGPU instancing。表示メッシュはLOD化してよいが、正規B-rep、最終snap、保存を粗くしない。DisplayMeshのfloat配列を将来の独自描画へ流用する際は、巨大座標でローカル原点を使う。

### P1-07：Windows GUIと配布物・依存版を検証する

**根拠：** Windowsはctest -LE gui。LinuxはXvfb GUI。Windows packageのDLL走査と絞ったPATHでのheadless smokeは良い実装だが、Qtウィンドウ・入力・OpenGLの起動確認にはならない。Linux配布artifactはこのworkflowにない。Windows conda依存は版固定されていない。[ci.yml:70以降][ci]、[CMakeLists.txt][cmake]。

**提案：** 日常PRは既存Linux＋Windowsコアを維持。Windows実機／適切なrunnerで短いGUI smokeを追加し、初回起動→矩形→Push/Pull→保存→再起動まで確認する。conda環境とLinux基準環境を固定し、更新PRで追従。SBR_WITH_OPENNURBS ON/OFFのビルドを確認。Linuxの配布形式を1つ決め、隔離環境で起動試験する。

**効果：** 「ビルドは通るが配ったら起動しない」を減らす。**工数：5〜10日。リスク：** GPU runner、CI費用。**判断：採用。** 全プラットフォーム拡大や署名・自動更新より先に2OSの配布品質を整える。

### P2-01：直接操作の体験を録画と少数の課題で磨く

**根拠：** ACとGUIテストはあるが、UX資料もクリック数・迷いの比較を手動評価に残している。[UX資料6.9][ux]、[GUIテスト][gui-tests]。

**提案：** 部屋→開口→庇、片流れ屋根、窓の反復配置、他者作成形状の修正、AI提案後の手直しを同じ条件で試す。推論の取り違え、ロック表示、非平面拒否時の代替案、Esc、文脈選択を観察する。計測の結果、造形を阻む問題が見つかればP1へ上げる。

**効果：** 機能の有無から「迷わず作れる」へ評価を移せる。**工数：3〜6日。リスク：** 評価者の習熟差。**判断：採用。** 新しい設定やツールを増やす前に表示・既定値・操作順を直す。

### P2-02：入れ子コンポーネントを、必要になった建築階層から導入する

**根拠：** Modeler::makeComponentはNested components are not supported yetで拒否する。[Modeler.cpp:2891][component-code]。PartModeも入れ子編集に未解決Issueがある。[Issue #9][part-issue9]。

**提案：** 建具→住戸→階の2〜3段をまず対象に、定義DAG、循環検出、依存更新、occurrence path、編集影響範囲を実装する。単にComponentInstanceを許すだけでは済まない。

**効果：** 建築の階層と反復を保てる。**工数：10〜20日。リスク：** 参照・素材・保存・Undo・性能すべてへ波及。**判断：永続参照と性能fixtureを先行し、その後に採用。** 直近の実案件で不可欠ならP1へ繰り上げる。

### P2-03：責務分離と状態表を、機能追加のついでに進める

**根拠：** 大きなTools／MainWindow／Modeler／CommandApiと、時点の違う文書が保守負荷を生む。[アーキテクチャ資料][arch]。

**提案：** Toolsを操作別、MainWindowを文書lifecycle／AI提案／recovery／paletteへ分割。コアは幾何操作、編集transaction、参照解決、queryを分ける。操作ID→実装→テスト→対応OS→既知制限の状態表を一元管理する。履歴資料は日付付きの記録とし、現在仕様の入口を別に置く。

**効果：** 小さな変更とレビューがしやすくなり、AIによる重複実装を減らす。**工数：小分けに5〜12日。リスク：** 一括分割の回帰。**判断：採用。** 全面リライトやカーネル型の完全な抽象化は当面見送る。

### P2-04：自由曲面の拡張は、建築の具体的用途から選ぶ

**根拠：** 曲面分割は未対応、非平面になる頂点移動は意図的に拒否する。一方、NURBS／loft／sweep／曲面Push/Pullは存在する。[UX追補][ux]、[変形仕様][reshape]。

**提案：** 曲面屋根の開口、円筒壁の面分割などを候補とし、操作の意味・曲面品質・再編集条件を先に定義する。任意の角移動を無条件に三角形化して成功させる方向へ変えない。

**効果：** 思想を保ちながら利用範囲を広げられる。**工数：調査3〜5日、実装10〜30日以上。リスク：** trim・周期面・接続品質と命名。**判断：現在は用途別検討。** 一般Rhino相当の全曲面機能、板金・CAM・機械拘束の大量追加は見送る。

## 6. 検証と導入の進め方

### CIの現時点で言えること

- 2026-10-23 11:48頃JSTに開始したrun 39891598382（0945876）では、Linux・Windowsの両ジョブが成功。Windowsの配布・DLL検査・headless smokeも成功している。[実行結果][ci-green]。
- bd908abのrun 39902457432は最終的にcancelled。後続の**調査対象7bb0356のrun 39902758718では、最終取得時点でLinux・Windows両ジョブのsuccessを確認した。** これはGitHub上のCI結果であり、本調査環境で独自実行した結果ではない。 [対象run][ci-head]。
- PR本文の古い「利用枠／課金上限かもしれない」という推測を、現在の障害原因として採用しない。後日の成功ジョブが確認できる。
- ソース上のGoogleTest宣言は211件を確認したが、GUIは複数ケースを1つのCTest登録にまとめる。PR記載の130件と単純比較せず、対象SHAの実行ログで数を確定すべき。今回、全テストが通るとは独自に主張しない。

### 小さく確認できる導入順

| 段階 | 内容 | 完了の判定 |
|---|---|---|
| 最初の1〜2週 | P0群。対象限定消しゴム、I/O隔離、3DM報告、実行ゲート。対象SHAと状態表を固定 | 重複対象を誤編集しない。試行で外部副作用がない。部分読込が見える。演算中のAI書込みが直列化される |
| 次の2〜4週 | workerの縦1経路、CommandSpecの少数操作、epoch、計測fixture | GUIとAIの同一操作が同じ形状・失敗・Undoを返す。遅い計算中もカメラが反応する |
| 次の4〜8週 | 永続参照・単位別許容差・Windows GUI／Linux配布・計測で必要と分かった最適化 | 保存後・上流変更後も対象が一致。基準機とfixtureで性能予算を満たす |
| その後 | 利用者観察、入れ子、曲面用途を選定 | 機能数ではなく、建築課題を少ない迷いで完了できる |

週数は並行作業を含めた計画の目安で、各案の工数を単純合算した納期ではない。

### 人間とAIの同値性に使う受入条件

1. GUIとAPIで「壁を作る→開口→分割した片側を押す→寸法変更→Undo→保存／再読込」を行い、solid数、体積、面積、外接箱、参照先、編集コンテキストを比較する。
2. 同じ失敗入力で、既存形状・素材・設定・履歴が変わらず、機械可読エラーと人向け理由が返ることを確認する。
3. AIはquery→対象集合固定→preview→人の確認または既存許可に沿うcommitを使う。人間は同じ部材を直接触り、同じUndoで戻せる。
4. 重い処理中の文書切替・取消・外部クライアント・自動保存を組み合わせ、古い結果や中途半端な取引が残らない。
5. 正規形状の精度を表示メッシュの誤差で代用しない。往復比較も体積だけでなく欠落面・穴・配置・向きと形状距離を対象にする。

**設計判断：** 競合から採るのは、型付きの共通操作、検証可能な結果、非同期の境界、計測に基づく改善である。SketchBRepの中心は「人が直接造形でき、その同じ形と編集履歴をAIも扱える」ことに置く。内蔵AIサービス、モデルAPIキー、トークン購入、特定モデルの契約をCADの前提条件として追加しない。

## 7. 根拠索引

以下のソースリンクは、変動するブランチ名ではなく調査したコミットへ固定した。リンクの行から該当関数全体を確認できる。Issue/PRとActionsはその時点の公開状態の参照であり、後日更新される。

- SketchBRep：README、docs/01〜15、src/core、src/app、tools、tests、.github/workflows/ci.yml。
- PartMode：README、docs/architecture.md、docs/help/agents.md、関連studioモジュール、headless worker、package.json、CI/nightly、Issue #2〜12、PR #1。
- SolidCraft：README、docs/architecture.md、docs/mcp.md、docs/action-matrix.md、crates/engine/src/lib.rs、crates/kernel/src/lib.rsとboolean_guard.rs、crates/mcp/src/backend.rs、CI/release、最近のIssue/PRを抽出確認。
- 各リポジトリのライセンスや実装言語は異なる。ここでの採用判断は設計パターンの採用であり、比較先コードの移植提案ではない。


[main]: https://github.com/higeotoko/sketchBRep/tree/5716215170b39f17fade2a044465249beeda3b0c
[pr1]: https://github.com/higeotoko/sketchBRep/pull/1
[part]: https://github.com/BOMWiki/partmode/tree/fe88558a34c8d7a3e03b34c2a374ba5ce6febe9f
[solid]: https://github.com/tangency-labs/solidcraft/tree/edf57cec293dcea301201f1d5173b33e16351466
[tools]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/app/Tools.cpp
[infer]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/Inference.cpp
[ux]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/docs/08_ux_gap_analysis.md#L149
[ops]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/Operations.cpp
[recipe]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/Recipe.cpp#L243
[reshape]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/docs/15_reshape.md
[component-code]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/Modeler.cpp#L2880
[components]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/docs/14_components.md
[eraser]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/app/Tools.cpp#L3571
[camera]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/app/Viewport.cpp#L296
[validation]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/Validation.cpp#L35
[faceref]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/FaceRef.cpp#L123
[types]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/include/sbr/Types.h#L11
[api]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/CommandApi.cpp
[bridge]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/app/AiBridge.cpp#L59
[ai-doc]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/docs/06_ai_integration.md
[perf]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/docs/10_performance.md
[display-code]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/app/Viewport.cpp#L523
[infer-bvh]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/Inference.cpp#L109
[save]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/NativeFormat.cpp#L869
[recovery]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/docs/12_recovery.md
[rhino]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/Rhino3dm.cpp#L355
[step]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/ShapeIO.cpp
[ci]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/.github/workflows/ci.yml
[tests-cmake]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/tests/CMakeLists.txt
[part-arch]: https://github.com/BOMWiki/partmode/blob/fe88558a34c8d7a3e03b34c2a374ba5ce6febe9f/docs/architecture.md
[part-infer]: https://github.com/BOMWiki/partmode/blob/fe88558a34c8d7a3e03b34c2a374ba5ce6febe9f/src/static/studio-sketch-infer.js
[part-direct]: https://github.com/BOMWiki/partmode/blob/fe88558a34c8d7a3e03b34c2a374ba5ce6febe9f/src/static/studio-direct-edit.js
[part-agent]: https://github.com/BOMWiki/partmode/blob/fe88558a34c8d7a3e03b34c2a374ba5ce6febe9f/src/static/studio-agent-service.js#L3415
[part-worker]: https://github.com/BOMWiki/partmode/blob/fe88558a34c8d7a3e03b34c2a374ba5ce6febe9f/src/headless/kernel-host.ts#L81
[part-resolver]: https://github.com/BOMWiki/partmode/blob/fe88558a34c8d7a3e03b34c2a374ba5ce6febe9f/src/static/studio-topology-reference-resolver.js
[part-issue12]: https://github.com/BOMWiki/partmode/issues/12
[part-issue9]: https://github.com/BOMWiki/partmode/issues/9
[part-pr1]: https://github.com/BOMWiki/partmode/pull/1
[solid-arch]: https://github.com/tangency-labs/solidcraft/blob/edf57cec293dcea301201f1d5173b33e16351466/docs/architecture.md
[solid-mcp]: https://github.com/tangency-labs/solidcraft/blob/edf57cec293dcea301201f1d5173b33e16351466/docs/mcp.md
[solid-matrix]: https://github.com/tangency-labs/solidcraft/blob/edf57cec293dcea301201f1d5173b33e16351466/docs/action-matrix.md
[solid-guard]: https://github.com/tangency-labs/solidcraft/blob/edf57cec293dcea301201f1d5173b33e16351466/crates/kernel/src/boolean_guard.rs
[solid-pr5223]: https://github.com/tangency-labs/solidcraft/pull/5223
[solid-issue5210]: https://github.com/tangency-labs/solidcraft/issues/5210
[mesh]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/DisplayMesh.cpp#L19
[stack]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/CommandStack.cpp#L124
[api-h]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/include/sbr/api/CommandApi.h#L23
[gui-tests]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/tests/test_gui.cpp
[scenarios]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/tests/test_scenarios.cpp
[api-reg]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/CommandApi.cpp#L681
[api-run]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/CommandApi.cpp#L1923
[progress]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/app/ProgressDialog.cpp#L16
[batch]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/CommandApi.cpp#L1737
[preview]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/CommandApi.cpp#L1898
[rhino-read]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/Rhino3dm.cpp#L517
[import-code]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/Modeler.cpp#L3482
[verify-ui]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/app/MainWindow.cpp#L2261
[verify-core]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/Modeler.cpp#L3395
[boolean]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/Operations.cpp#L280
[api-effect]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/CommandApi.cpp#L1980
[transactions]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/tests/test_transactions.cpp#L158
[load-code]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/src/Modeler.cpp#L3416
[bench]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/tools/bench/main.cpp#L94
[stack-h]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/src/core/include/sbr/doc/CommandStack.h#L51
[cmake]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/CMakeLists.txt
[arch]: https://github.com/higeotoko/sketchBRep/blob/7bb03562d595ad8134fd6f875ca3292ffd49db24/docs/02_architecture.md
[ci-green]: https://github.com/higeotoko/sketchBRep/actions/runs/39891598382
[ci-head]: https://github.com/higeotoko/sketchBRep/actions/runs/39902758718

