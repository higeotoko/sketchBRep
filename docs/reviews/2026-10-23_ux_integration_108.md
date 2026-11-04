# SketchBRep：直接操作・操作感の統合実装指示書

2026年10月23日／対象：Windows版SketchUp Pro 2026に近い直接操作と、正規B-repを両立するSketchBRep

## 1. この資料で依頼すること

**ツールの有無だけでなく、選択→方向を示す→数値入力→視点変更→確定→再入力→Undoまで、利用者の一続きの作業として整えてください。** 18分野108項目を、期待する処理、現状の確度、優先度、確認例へ分解しました。全部を一度に実装する指示ではありません。既存の機能を作り直さず、欠けている受け渡しと操作条件を順に修正します。

最優先の思想は「直感的に造形でき、人間とAIが同じ3D環境を扱える」。面数が増えることや開いた形状になることだけで作業を止めません。形状の種類ごとに成立条件を検証し、不正な結果は保全して拒否します。OCCTの正規B-repと解析曲線は維持し、画面の操作感を近づけるために幾何を粗いメッシュへ置き換えません。有料のAIモデルAPIを必須にする変更は不要です。

### 対象コミットと調査限界

- 実装ブランチ：claude/sketchbrep-modeler-spec-4wnp0b。
- 固定HEAD：**e3c312485ee1f97ef94bbeade72650e978017050**。すべてのコードリンクをこのSHAに固定。
- コア・GUI・設計・API・保存・関連テストの17ファイルを取得し、主要経路を抽出照合。全行・全分岐の監査ではありません。ローカルビルド、テスト実行、実機操作、Windows入力機器・GPU計測はしていません。
- 実装ブランチは会話中にも更新されています。前の差分表3462646、コピー配列の指示書3f8cdfb、直近の尺度説明a15a8a8とは固定点が異なります。**以前「未対応」とした内容を、そのまま現在の未対応一覧へ移さないでください。**
- SketchUp公式資料を操作の比較基準として参照。以下は公式文書の翻訳・転載ではなく、ユーザー要望とコード確認から作ったSketchBRepの製品要件です。参考欄がある項目も、その節の全細部が公式に明記されているという意味ではありません。
- 入力の時間しきい値、スクロール倍率、連続配列再入力、同位置の重複処理などは、Windows版2026で観測して決める対象です。未確認の挙動を「完全互換」と表現しないでください。

### 状態の読み方

| 表記 | 意味 |
|---|---|
| 基盤あり | 関連する実装経路あり。行の要件全体の達成・テスト合格を保証しない |
| 追加済・要検証 | 最近の関連実装・テストを確認。実機で直ったかは未確認 |
| 差分あり | 記載した要件と異なるコード経路を確認 |
| 部分対応／明示制限あり | 一部の経路はあるが不足・拒否条件あり |
| 要照合／要設計 | 受入基準案。今回の抽出確認では網羅的な実装判定をしていない |
| 実機確認 | コードだけで操作感の一致を判定できない |
| 静的懸念 | 危険な経路を読めるが、実行して発生を確認していない |

P0：誤った対象の変更、形状消失、基本操作の停止、履歴・確定状態の破綻を先に防ぐ。P1：日常の造形に必要な操作の一貫性。P2：後続の互換・補助機能。現状が十分なら修正せず受入確認で閉じます。

## 2. すでに追加された個別要望と、今見るべき差分

| 個別要望 | 最新コードの状態 | この資料で行うこと |
|---|---|---|
| 面を消して線を残す・面辺混在削除 | 共通deleteSelectionとGUI回帰テストあり | 5面＋独立線、境界線を含むケース、部分選択を比較 |
| 開いた形状からPush/Pull | volumeFromOpenFaceの経路あり | 残存面の保全、体積との混在、保存再読込 |
| オフセット後の枠領域 | punchHoleとSolid内側Insetあり | 枠・中央の選択と、再入力・元寸法変更 |
| 経路を事前選択するフォローミー | 選択辺列を読み取る経路あり | 断線・分岐、断面向き、中空断面 |
| 屋根の棟・自動分割 | 棟変形とAutofoldの経路・テストあり | 共有境界、穴、材質、グループ境界 |
| 線のコピーと/N・*N | copyEdgesと基準距離保持あり | 記法の連続変更はSketchUp実機比較 |
| 縦・横だけの尺度 | 尺度箱、グリップ、stretchあり | 回転対象、グループ、コンポーネント配置の制限 |
| スナップのカーソル横表示 | SnapTip追加済み | DPI、画面端、表示と実座標の一致 |
| ホイール | 入力正規化・基準点・専用ズームあり | 実機での速さ、室内、空白、作図継続 |

これらの関連テストは[tests/test_gui.cpp:267](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/tests/test_gui.cpp#L267)、[tests/test_gui.cpp:1078](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/tests/test_gui.cpp#L1078)、[tests/test_gui.cpp:1147](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/tests/test_gui.cpp#L1147)、[tests/test_gui.cpp:2718](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/tests/test_gui.cpp#L2718)、[tests/test_gui.cpp:2800](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/tests/test_gui.cpp#L2800)、[tests/test_gui.cpp:3086](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/tests/test_gui.cpp#L3086)、[tests/test_gui.cpp:3478](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/tests/test_gui.cpp#L3478)など。存在を確認しただけで、今回実行した結果ではありません。

### 今回、新しく優先する差分・懸念

1. **線の数値確定後の空配列参照。** [src/app/Tools.cpp:661](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L661)でaddPointの後にpoints_.backを使います。一方[src/app/Tools.cpp:595](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L595)は面分割・閉路完了等でpoints_をclearします。その経路で空配列参照となり得るので、数値で反対辺へ届かせるケースを先に再現・修正します。カーネル例外のcatchが、このC++の不正参照まで保護するとは限りません。
2. **Ctrl+C/Xの部分選択。** [src/app/MainWindow.cpp:1362](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/MainWindow.cpp#L1362)は所有IDのselectionを渡し、Ctrl+Xは所有IDをremoveします。Moveの辺コピーが直っていても、クリップボードは別経路です。面・辺を選んだつもりで立体全体をコピー／切取しないようにします。
3. **矩形の片側だけの寸法と符号。** [src/app/Tools.cpp:1962](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L1962)は2数値必須でabsを取っています。2000,／,1500と、負方向を指定する数値は分けて実装します。
4. **選択の修飾キーと範囲。** [src/app/Tools.cpp:163](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L163)はShift中心、[src/app/Tools.cpp:220](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L220)は境界箱・ID単位の範囲判定。要素を選ぶUIと処理対象を一致させます。
5. **コンポーネント配置の尺度。** [src/core/src/Modeler.cpp:2972](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/Modeler.cpp#L2972)は配置の非一様尺度を拒否します。1配置だけの幅変更と共有定義の編集を区別する設計が必要です。
6. **中＋左パンのクリック配送。** [src/app/Viewport.cpp:355](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Viewport.cpp#L355)は中ボタンでカメラ状態に入り、別の左押下をツールへ渡せる構成です。追加点が生じるかを実機・イベント列で確認します。

## 3. 操作の処理一覧：18分野・108項目

行の「確認例」は未実施の受入課題です。既存機能を維持しつつ、場面ごとに修正・検証します。

### 1. 選択・範囲・右クリック

面・辺・隣接選択とグループ編集の入口はある。Shift以外の選択修飾キーと、要素単位の範囲選択は差分が残る。

確認した入口：[src/app/Tools.cpp:112](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L112)、[src/app/Tools.cpp:220](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L220)、[src/app/MainWindow.cpp:2193](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/MainWindow.cpp#L2193)。 操作の参考：[S01]。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| SEL-01 対象単位 | P0 | 基盤あり | 面1枚・辺1本・まとまりの選択を保持し、ツールへ所有IDだけを渡して対象を広げない。 | 箱の1辺選択からコピー・削除・移動を順に確認。 |
| SEL-02 複数選択 | P1 | 差分あり | Ctrl追加、Shift反転、Ctrl＋Shift除外をそろえ、空白クリックと修飾付き空白クリックを区別する。 | 既選択と未選択の面・辺を混在させて切替。 |
| SEL-03 ダブル・トリプル | P1 | 基盤あり | 隣接選択と接続全体を区別。クリック間隔・移動量をOS設定と整合し、内部編集の開始と衝突させない。 | 離れた形状を含むID、別ID間の接続、グループで確認。 |
| SEL-04 範囲選択 | P1 | 差分あり | 左右の包含／交差判定を面・辺にも適用。境界箱の空白を囲んだだけで実形状を選ばない。 | L字の空白、箱の1辺、斜め線の一部を囲う。 |
| SEL-05 右クリック | P1 | 基盤あり | 既選択上では選択を保ち、未選択上では対象を明示してメニューを出す。右クリックだけでモデルを変更しない。 | 複数選択上と未選択の面上でメニューを開閉。 |
| SEL-06 選択可視性 | P1 | 要照合 | ホバーと確定選択を区別。材質を認識できる表示、隠れた対象の選択範囲、ロック対象の扱いを統一する。 | 表裏の重なる面と透明面で、ハイライトと実対象を比較。 |

### 2. 削除・消しゴム・残す形状

最新では共通deleteSelectionが追加済み。面だけ削除、孤立境界保持、混在選択のテストもある。過去の不具合として回帰を確認する。

確認した入口：[src/app/MainWindow.cpp:2078](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/MainWindow.cpp#L2078)、[src/core/src/Modeler.cpp:1848](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/Modeler.cpp#L1848)、[src/app/Tools.cpp:4068](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L4068)。 操作の参考：[S02]、[S03]。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| DEL-01 面だけ消す | P0 | 追加済・要検証 | 面だけを消して外周・内周の線を残す。閉輪郭だからと直後に面を再生成しない。 | 単独四角面と穴あり面を消し、線を選択・保存。 |
| DEL-02 5面と別の線 | P0 | 追加済・要検証 | 上面以外の5面＋無関係な線の削除で上面を残す。上面境界も削除した場合は依存面の消失と区別する。 | 上面境界を含む場合／含まない場合の両方。 |
| DEL-03 線のDelete | P0 | 追加済・要検証 | 孤立線、ポリラインの一部分、Solid/Shellの境界線、分割線を適切な種類として処理する。 | 各種を単独と複数IDで削除。消しゴムとも比較。 |
| DEL-04 なぞり消去 | P1 | 基盤あり | 押下中は候補を示し、離して一括確定。途中で生じた後継形状だけを追い、別オブジェクトへ探索を広げない。 | 交差する別の線を置き、1ストローク後Undo。 |
| DEL-05 消去と非表示 | P1 | 基盤あり | 消す・隠す・ソフト／スムーズを別操作として扱う。隠した線を幾何から削除しない。 | 隠した分割線の両側で材質を維持。 |
| DEL-06 混在削除の原子性 | P0 | 追加済・要検証 | 対象を変更前に解決し、一括で適用。1件の失敗で全て戻し、エラー時に一部だけ消さない。 | 複数対象の最後を無効にして変更数・履歴・形状を比較。 |

### 3. 吸着・推論・カーソル横の説明

端点・中点・辺・面・軸・平行垂直・追跡・stickyがあり、カーソル横のSnapTipも最新で追加済み。

確認した入口：[src/core/src/Inference.cpp:417](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/Inference.cpp#L417)、[src/app/Tools.cpp:277](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L277)、[src/app/Viewport.cpp:1698](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Viewport.cpp#L1698)。 操作の参考：[S02]。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| SNAP-01 表示と座標の一致 | P0 | 追加済・要検証 | 採用したSnapResultを、文字・色・点・補助線・確定座標の共通根拠にする。 | 端点／中点／交点／面上の表示時に確定座標を記録。 |
| SNAP-02 候補の安定性 | P1 | 実機確認 | 密集部で候補が飛ばず、抜けたときは遅れず解除。遮蔽・背面・X線表示の規則を分ける。 | 近接する2本の柱角を低速で横断する。 |
| SNAP-03 点の追跡 | P1 | 基盤あり | ホバーした点を参照し、離れた位置でも同じ高さ・位置へ合わせる。古い参照を無制限に残さない。 | 窓角を参照し、壁の別位置に同じ高さの点を作図。 |
| SNAP-04 拘束キー | P1 | 差分あり | 赤緑青の矢印、Shift、斜め方向固定を一貫化。下キーを推論方向固定へ使うかを公式操作と照合する。 | 斜め辺から平行・垂直を固定したままOrbit。 |
| SNAP-05 対象の説明 | P1 | 追加済・要検証 | 面上と仮想作業平面上を区別。画面端やDPI変更でもラベルを切らず、入力を遮らない。 | 画面四隅とDPI100/150/200％で表示。 |
| SNAP-06 追加推論 | P2 | 要設計 | 円・円弧の中心、接線、円弧端の接線方向、グループ内の対象説明を用途順に追加する。 | 接する円弧や中心基準の配置を正規形状で確認。 |

### 4. 寸法入力・単位・直後の修正

直接入力とRecentEdit、mm/cm/mはある。矩形入力は2つの数値必須で、空欄の片側寸法は未対応。符号も絶対値化している。

確認した入口：[src/app/Tools.cpp:58](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L58)、[src/app/Tools.cpp:1962](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L1962)、[src/core/src/Units.cpp:28](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/Units.cpp#L28)。 操作の参考：[S04]、[S05]。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| NUM-01 直接入力 | P1 | 基盤あり | 数値欄をクリックせず入力。テンキー、Backspace、Enter、ショートカットとの競合を確認する。 | P/M/R中に桁を修正し、不要なツール切替を起こさない。 |
| NUM-02 片側寸法 | P1 | 差分あり | 矩形の2000,と,1500で片側だけ更新。空欄はゼロや不正値ではなく現寸法保持とする。 | 2000×1000を幅だけ3000、次に高さだけ1500。 |
| NUM-03 単位と符号 | P1 | 部分対応 | 単位付き値を換算し、負数を許す操作は方向反転として明示。全ツールで機械的にabsへ置換しない。 | 3000mm＝300cm＝3m、矩形の符号、移動の負方向。 |
| NUM-04 直後の再入力 | P1 | 基盤あり | 同じ対象・同じ基準で結果を置換し、不要なコピーと履歴を積まない。 | 矩形・押出・尺度をそれぞれ3回修正後Undo。 |
| NUM-05 入力失敗の保全 | P0 | 要照合 | 不正値、空入力、桁あふれ、許容差以下の寸法で、直前の正常結果を壊さない。 | 正常値→不正値→正常値で形状と履歴を比較。 |
| NUM-06 座標・追加単位 | P2 | 要設計 | 絶対／相対座標と英米単位は明示的な文法で追加。日本語IMEの確定Enterをモデル確定と二重解釈しない。 | 座標入力とIMEの編集中／確定直後を分けて確認。 |

### 5. 線の連続作図・接続・面化

連続点・閉路・横断分割・交差面化はある。数値確定後の状態遷移に静的な危険箇所を確認したため、ここは先に再現する。

確認した入口：[src/app/Tools.cpp:535](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L535)、[src/app/Tools.cpp:595](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L595)、[src/app/Tools.cpp:661](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L661)、[src/core/src/Modeler.cpp:558](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/Modeler.cpp#L558)。 操作の参考：[S02]。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| LINE-01 連続作図の終了 | P1 | 基盤あり | 確定済みの線と次の仮線を分け、Esc・ダブルクリック・ツール変更で、何が残るかを統一する。 | 3辺確定→4辺目仮線→Escで確定3辺を確認。 |
| LINE-02 辺の途中へ接続 | P1 | 要照合 | T字・十字の接続点を幾何上も認識し、近くに見えるだけの非接続と区別する。 | 長線の中点に別線を接続し、その後の移動・削除を確認。 |
| LINE-03 輪郭を閉じる | P1 | 基盤あり | 閉じる直前に面化を予告。微小隙間を許容差以上に勝手に埋めず、失敗時は線を保全する。 | 直線＋円弧、凹輪郭、隙間あり輪郭で確認。 |
| LINE-04 面を横断する線 | P1 | 基盤あり | 分割した領域を選べ、材質と内周を維持。途中で止めた線を勝手に延長しない。 | 矩形を横断／途中で終了の2例を比較。 |
| LINE-05 数値確定時の状態安全 | P0 | 静的懸念 | addPointで面分割や閉路完了によりpoints_が空になった後、points_.back()を参照しない。 | 辺上で始め、数値で反対辺に届かせて分割する。 |
| LINE-06 立体を参照した閉路 | P1 | 基盤あり | 既存面・辺を輪郭の一部に使って面化し、他のグループの形状を勝手に取り込まない。 | 箱の辺と新規2辺で閉じる輪郭を作る。 |

### 6. 矩形・円・円弧の描き心地

矩形・円・円弧と作業平面がある。円は正規の円として維持し、SketchUpの分割数表示を幾何精度の基準にしない。

確認した入口：[src/app/Tools.cpp:1919](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L1919)、[src/app/Tools.cpp:1721](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L1721)、[src/app/Tools.cpp:1496](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L1496)。 操作の参考：[S05]、[S06]。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| SHAPE-01 作図平面 | P1 | 基盤あり | 地面・壁・傾斜面で始点時の平面を保持し、視点変更で地面へ飛ばない。 | 垂直壁に矩形を描き始めOrbitして確定。 |
| SHAPE-02 形状の推論 | P2 | 要設計 | 正方形・半円など有用な比率や角度を予告し、必要な場合だけ拘束できるようにする。 | 正方形近傍で強制されずに細長い矩形も描けるか。 |
| SHAPE-03 回転矩形 | P2 | 要設計 | 角度を持つ矩形を3点などで指定できる。通常矩形の平面決定と共通化する。 | 斜めの敷地線に沿った矩形を寸法指定。 |
| SHAPE-04 円の寸法 | P1 | 基盤あり | 中心・半径を明示し、直後の半径再入力に対応。直径指定を追加する場合は記法を区別する。 | 半径500を750へ更新し、実測半径を確認。 |
| SHAPE-05 円弧の段階復帰 | P1 | 基盤あり | 始点・終点・膨らみの段階を見せ、Escで一段戻れる。線と扇形面のモードを表示する。 | 膨らみ段階でEsc→終点変更→再確定。 |
| SHAPE-06 接する曲線 | P2 | 要設計 | 直線と円弧の接線・接続を支援。曲率や中心が変わる場合は利用者に分かるプレビューを出す。 | 丸めた角を正確な円弧で作り、押出まで確認。 |

### 7. 移動・つかむ点・伸縮

基点移動、選択辺の変形、コピー辺の経路、棟変形がある。単独面Moveの法線押出と任意方向変形の区別は引き続き確認対象。

確認した入口：[src/app/Tools.cpp:2248](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L2248)、[src/app/Tools.cpp:2428](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L2428)、[src/core/src/Modeler.cpp:845](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/Modeler.cpp#L845)。 操作の参考：[S07]、[S08]。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| MOVE-01 基点を正確に置く | P1 | 基盤あり | 掴んだ角・中点をコピー先の参照点に一致させる。途中で対象中心へ基点が変わらない。 | 柱の下端角を別の柱の上端角へ移動。 |
| MOVE-02 事前／後選択 | P1 | 部分対応 | 選んでからMと、Mで対象を掴む操作を両立。辺だけの選択を全体移動へ拡大しない。 | 同じ辺を2通りの選択順で動かす。 |
| MOVE-03 平面と軸の拘束 | P1 | 基盤あり | 通常平面、青軸移動、斜め方向を明示し、数値入力まで同じ拘束を保持する。 | 上げる／横へずらす／斜め辺に沿うを比較。 |
| MOVE-04 移動とコピー切替 | P1 | 追加済・要検証 | 配置途中にCtrlを切替え、元の形状を先に確定しない。コピー辺と変形頂点を別状態にする。 | 基点指定→変形プレビュー→Ctrl→戻す。 |
| MOVE-05 面の移動 | P1 | 要照合 | 面を任意方向へ移動する意図を、厚み生成のPush/Pullと区別。接続する面は必要に応じて追従する。 | 箱上面を法線方向と横方向へ動かす。 |
| MOVE-06 棟・頂点の変形 | P1 | 追加済・要検証 | 棟を上げる切妻と必要なAutofoldに対応。グループを跨がず、穴・材質・閉鎖性を保つ。 | 6000×4000の上面の棟を1500上げる。 |

### 8. 線コピー・等分割・配列の再入力

選択辺コピー、面分割への接続、基準距離保持が最新で追加済み。連続記法の解釈はコードコメントでも暫定とされている。

確認した入口：[src/app/Tools.cpp:2485](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L2485)、[src/core/src/Modeler.cpp:2014](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/Modeler.cpp#L2014)、[tests/test_gui.cpp:267](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/tests/test_gui.cpp#L267)。 操作の参考：[S04]。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| ARRAY-01 等間隔コピー | P1 | 追加済・要検証 | 600mmコピー→*5で元を含め6位置。最初のコピーを二重計上しない。 | x=0,600,1200,1800,2400,3000を実測。 |
| ARRAY-02 間隔の等分割 | P1 | 追加済・要検証 | 3000mmコピー→/3で1000mm間隔の4位置。線自体の長さ分割と区別する。 | 元から終点まで3区間になること。 |
| ARRAY-03 数量の置換 | P1 | 追加済・要検証 | *5→*3や/3→/5で古い生成物を置換し、元形状と他の線を保つ。 | 入力を3回変えてUndo1回。 |
| ARRAY-04 記法の連続変更 | P1 | 実機確認 | *5↔/3の基準範囲と距離の再入力をSketchUp2026と比較して仕様固定。暫定実装を完全互換と呼ばない。 | 初回600mmから各記法と距離を入れ替えて座標を記録。 |
| ARRAY-05 面上の配列線 | P1 | 追加済・要検証 | 線のコピーを面分割へつなぎ、既存境界との重複を抑える。生成物の由来を追う。 | 幅3000の面を/3し、各領域を選択。 |
| ARRAY-06 選択単位と種類 | P1 | 追加済・要検証 | 1辺・辺集合・曲線・グループの各単位を守り、曲線を直線化せず相対配置を保持する。 | L字の片辺だけ／両辺／グループをコピー。 |

### 9. 回転・尺度・鏡像

回転コピーと尺度グリップがある。非一様尺度でコンポーネント配置を拒否する制限があり、共有定義と配置を分けた設計が必要。

確認した入口：[src/app/Tools.cpp:2629](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L2629)、[src/app/Tools.cpp:3237](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L3237)、[src/core/src/Modeler.cpp:2972](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/Modeler.cpp#L2972)。 操作の参考：[S09]、[S10]。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| XFORM-01 回転の中心と平面 | P1 | 基盤あり | 中心・回転面・基準方向・角度を予告。面法線と任意軸を使い分ける。 | 壁の面上と斜め軸の回転を比較。 |
| XFORM-02 回転配列 | P1 | 基盤あり | Ctrlコピーから角度配列を作る。360度で同位置になるコピーの重複規則を決める。 | 30度コピー→*11と、閉じる指定を比較。 |
| XFORM-03 1方向・2方向尺度 | P1 | 追加済・要検証 | 面グリップは1軸、辺グリップは2軸、角は比率保持。反対側固定と中心基準を切り替える。 | 1000箱を幅2000、高さ3000へ順に変更。 |
| XFORM-04 倍率と実寸 | P1 | 追加済・要検証 | 2などの倍率と2000mmなどの実寸を区別し、直後修正と部分寸法入力を統一する。 | 同じグリップで2と2000mmを比較。 |
| XFORM-05 ローカル軸と配置 | P1 | 明示制限あり | 回転した対象は分かる軸で伸縮。コンポーネント1配置の尺度を定義全体の変更と区別する。 | 同じ窓の2配置の片方だけ幅を変える。 |
| XFORM-06 反転と縮退 | P2 | 要設計 | ゼロ近傍、負倍率、鏡像は明示した規則で扱う。表裏・法線を直し、無効形状を確定しない。 | 片側を越えるドラッグと専用鏡像を比較。 |

### 10. Push/Pull・開いた形状・到達面

通常押出、同量反復、開いた形状からvolumeFromOpenFaceを呼ぶ経路あり。作れることと元の周辺面との接続が自然なことを別に確認する。

確認した入口：[src/app/Tools.cpp:2017](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L2017)、[src/app/Tools.cpp:2115](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L2115)、[src/core/src/Modeler.cpp:2162](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/Modeler.cpp#L2162)、[src/core/src/Modeler.cpp:2275](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/Modeler.cpp#L2275)。 操作の参考：[S11]。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| PULL-01 事前選択 | P1 | 要照合 | 選択済み面→Pで対象を引き継ぎ、密集部で選び直しを要求しない。 | 奥まった面を選んでP、視点を変えて開始。 |
| PULL-02 領域と内周 | P0 | 追加済・要検証 | 枠・中央・複数穴を区別し、指定領域の全内周を保持して押し出す。 | 4000×3000の内側200枠を高さ1000。 |
| PULL-03 他面までの押出 | P1 | 部分対応 | 別の面・頂点を参照した高さ合わせと、貫通・接触を区別してプレビューする。 | 高さ3000の柱に合わせる、反対面まで穴を抜く。 |
| PULL-04 同量と開始面保持 | P1 | 部分対応 | 別面ダブルクリックで同量適用。Ctrl等で開始面を残す操作を採用するなら状態表示も加える。 | 2面へ同量、開始面保持のオン・オフ。 |
| PULL-05 開いた形状から体積 | P0 | 追加済・要検証 | 残った有効な平面から新しい体積を作り、他の面・線を保つ。全体を誤って単一ソリッドと扱わない。 | 箱の5面を消す／1面を消すの2例で押出。 |
| PULL-06 プレビューと材質 | P1 | 部分対応 | 押出と切削の確定結果を予測できる表示にし、生成側面・移動面の材質継承を統一する。 | 削り操作のプレビュー、確定、Undoの差を比較。 |

### 11. オフセット・フォローミー

単独面の枠化、Solid内側Inset、事前辺選択Follow Meあり。穴ありオフセットと角用スイープの中空断面には明示制限が残る。

確認した入口：[src/core/src/Modeler.cpp:2428](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/Modeler.cpp#L2428)、[src/app/Tools.cpp:4246](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L4246)、[src/core/src/Freeform.cpp:162](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/Freeform.cpp#L162)。 操作の参考：[S12]、[S13]。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| PATH-01 内外オフセット | P0 | 追加済・要検証 | 内外の領域が実際に分かれ、枠と中央の両方を選択・編集できる。 | 内側200、外側200、元寸法変更後の再生成。 |
| PATH-02 辺列と同量反復 | P2 | 要設計 | 同一平面の辺列、別面への同量オフセットを追加。外側が面外へ出る場合も範囲を説明する。 | L字辺列と2つの面へ同量適用。 |
| PATH-03 凹形と内周 | P1 | 明示制限あり | 凹輪郭の消滅・分裂・自己交差を判断し、穴を黙って埋めない。未対応形状は具体的に説明する。 | 狭い凹部と穴あり面の内外オフセット。 |
| PATH-04 選択経路のスイープ | P1 | 追加済・要検証 | 複数辺の順序・向きをそろえ、直線＋円弧を1経路として扱う。断線・分岐を区別する。 | 選択順を逆転、微小隙間、T字分岐を比較。 |
| PATH-05 断面と角の扱い | P1 | 部分対応 | 始点・断面向き・角での挙動を予告し、中空断面を中実へ変えない。 | 枠断面で直線と90度曲がる経路を比較。 |
| PATH-06 手動追従と復帰 | P2 | 要設計 | 手動で経路をなぞるモードは自動経路と共通化。失敗時は選び直せる状態と入力を残す。 | 途中でEsc、経路変更、スイープ失敗後の再実行。 |

### 12. グループ・コンポーネント・編集範囲

内部編集・コンポーネント定義編集・独立化の経路あり。各ツールが所有IDだけで処理しないことを横断的に確認する。

確認した入口：[src/app/MainWindow.cpp:1190](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/MainWindow.cpp#L1190)、[src/core/src/Modeler.cpp:3972](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/Modeler.cpp#L3972)、[src/core/src/Modeler.cpp:4151](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/Modeler.cpp#L4151)。 操作の参考：[S14]。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| GROUP-01 編集の入口と出口 | P1 | 基盤あり | ダブルクリックで内部、空白や明示操作で外部へ。Escの段階戻りと編集終了を区別する。 | 円弧作図途中、確定後、選択中の順でEsc。 |
| GROUP-02 外側を変えない | P0 | 基盤あり | 同位置の外側頂点・辺に接しても内部編集が伝播しない。外側は参照だけ可能とする。 | 接する2グループで片方の棟と線を編集。 |
| GROUP-03 入れ子と階層表示 | P1 | 要照合 | 現在の編集階層を表示し、ビューポートと一覧の選択を同期。外側のフェードを一定にする。 | 3階層の窓部材で内外を往復。 |
| GROUP-04 共通定義と独立化 | P1 | 基盤あり | 共有変更が全配置へ及ぶことを示し、1個だけ変えたい場合に独立化できる。 | 同じ窓3個の定義編集→1個独立化。 |
| GROUP-05 グループ化と展開 | P1 | 要照合 | 面・辺の部分選択をグループ化しても全所有オブジェクトを巻き込まない。展開で見た目の位置と材質を維持する。 | 箱の1面をまとめる／回転グループを展開。 |
| GROUP-06 ロックとタグ | P1 | 基盤あり | ロックは編集を防ぎつつ参照可能、非表示はピック対象外。タグは可視性であり幾何の分離と混同しない。 | 下絵をロックし、隠した階の辺に吸着しないか確認。 |

### 13. コピー・切り取り・貼り付け

現在のCtrl+C/Xはselectionの所有IDを渡す。面・辺の部分選択が対象全体へ拡大する差分がある。ここは配列コピーとは別経路。

確認した入口：[src/app/MainWindow.cpp:1362](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/MainWindow.cpp#L1362)、[src/app/Tools.cpp:3194](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L3194)、[src/core/src/Modeler.cpp:3636](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/Modeler.cpp#L3636)。 以下はSketchBRepの製品要件。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| CLIP-01 選択した要素だけコピー | P1 | 差分あり | 1辺・1面のコピーで所有Solid全体を複製しない。要素集合を共通の対象表現で抽出する。 | 箱の1辺をCtrl+C→Ctrl+V。 |
| CLIP-02 選択した要素だけ切取 | P0 | 差分あり | 部分切取でオブジェクト全体を削除しない。コピー成功後の削除は共通Delete規則と原子的に適用する。 | 箱の1面をCtrl+Xし、残る5面を確認。 |
| CLIP-03 貼付基点 | P1 | 基盤あり | 貼付形状がカーソルへ追従し、掴む基準点と配置先が分かる。任意基点が必要なら明示指定できる。 | 窓の左下を壁の参照点へ合わせる。 |
| CLIP-04 同じ位置へ貼付 | P1 | 基盤あり | 編集階層が違ってもワールド位置とローカル変換を正しく扱う。 | 回転グループ内から外側へ同位置貼付。 |
| CLIP-05 連続貼付と取消 | P1 | 要照合 | 配置の反復、Esc、ツール変更で不要な形を残さない。クリップボードとモデルの確定状態を区別する。 | 貼付プレビュー→Esc→再貼付。 |
| CLIP-06 属性と共有関係 | P1 | 要照合 | 材質・タグ・階層・コンポーネント参照を保持し、別文書へは必要な依存だけ移す。 | 材質付き窓の貼付と保存再読込。 |

### 14. メジャー・分度器・補助線・寸法

測定、辺から平行ガイド、角度ガイド、寸法ツールの土台あり。測定後に実寸へモデル全体を拡縮する操作は今回取得した測定入力経路にはない。

確認した入口：[src/app/Tools.cpp:2978](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L2978)、[src/app/Tools.cpp:3052](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L3052)、[src/app/Tools.cpp:3813](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L3813)。 操作の参考：[S15]。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| GUIDE-01 測ると作るを区別 | P1 | 基盤あり | 距離を読むだけか補助線を作るかをカーソルと案内で明示。測定だけで形状を変更しない。 | 2点測定と辺からの200mmガイドを比較。 |
| GUIDE-02 補助線の交点 | P1 | 基盤あり | 平行ガイド・角度ガイド・交点を作図の参照に使い、面の実境界にはしない。 | 2本のガイド交点に矩形の角を置く。 |
| GUIDE-03 管理と削除 | P1 | 要照合 | 補助線を一括非表示／削除でき、実線を消さない。Undoと編集中の階層を守る。 | 実線とガイドを重ね、一括操作を戻す。 |
| GUIDE-04 角度の指定 | P1 | 基盤あり | 分度器の平面・基点・基準方向を示し、正負の角度を数値で指定する。 | 30度ガイド→線→回転ツールへ引継ぎ。 |
| GUIDE-05 実寸への補正 | P2 | 要設計 | 測定後の実寸入力で対象を較正する場合は、文書全体／グループ／下絵の範囲を明示する。 | 既知の3000mm長さで下絵と模型を別々に較正。 |
| GUIDE-06 寸法の追従 | P2 | 要照合 | 寸法線が参照している点を追い、対象消失時に別頂点へ誤接続しない。追従しない寸法ならその性質を表示する。 | 柱高さ変更と柱削除の後に寸法を確認。 |

### 15. 材質・表裏・隠す・見える範囲

面へのペイント、全体への割当、スポイト、非表示辺、表裏反転あり。soft/smoothとhiddenの意味は区別したまま拡張する。

確認した入口：[src/app/Tools.cpp:4362](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L4362)、[src/app/Tools.cpp:4389](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L4389)、[src/core/src/Modeler.cpp:736](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/Modeler.cpp#L736)。 操作の参考：[S16]。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| LOOK-01 面とまとまりの材質 | P1 | 基盤あり | 面塗りとグループの継承材質を区別し、クリックした面だけを変更できる。 | グループ材質の上から1面だけ塗る。 |
| LOOK-02 スポイト | P1 | 基盤あり | 表示されている実効材質を取得し、別の面へ適用できる。無効な素材を選んだ状態にしない。 | 継承材質と面個別材質を拾って比較。 |
| LOOK-03 形状編集後の材質 | P1 | 要照合 | 分割・押出・尺度・Autofold後も、移動面と新規側面の材質の規則を保つ。 | 色分けした屋根を棟移動し保存。 |
| LOOK-04 面の表裏 | P1 | 基盤あり | 開いた面では表裏が分かり、反転できる。鏡像後や体積化後の法線を正しく維持する。 | 面反転→押出、鏡像→表示を比較。 |
| LOOK-05 隠すとソフト化 | P2 | 要設計 | hiddenは表示だけ、softは面をまとまりとして扱う補助、smoothは陰影として区別する。正規曲面の面数を不要に増やさない。 | 折れた屋根の棟と丸い側面で選択・陰影を比較。 |
| LOOK-06 表示範囲 | P1 | 要照合 | タグ・断面・X線・隠し形状の表示で、見た目とピック・スナップの対象規則を一致させる。 | 断面裏・非表示階・X線で同じ点を狙う。 |

### 16. カメラ・一時視点操作・ズーム

ホイールは正規化と専用アンカー処理へ更新済み。中ボタンの一時視点操作あり。中＋左のパン配送は確認が必要。

確認した入口：[src/app/Viewport.cpp:355](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Viewport.cpp#L355)、[src/app/Viewport.cpp:448](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Viewport.cpp#L448)、[src/app/Viewport.cpp:1969](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Viewport.cpp#L1969)。 操作の参考：[S17]、[S18]。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| CAM-01 一時カメラと作図 | P0 | 部分対応 | 中ボタン操作を作図クリックへ漏らさず、始点・ロック・数値入力・コピー状態を保つ。 | 始点後に中＋左パンして、余計な点を置かない。 |
| CAM-02 ズームの速度と基準 | P1 | 追加済・要検証 | 近景・遠景・空白・室内で位置が飛ばず、小入力と分割入力を安定して扱う。 | 同総入力を一括／細分化して比較。 |
| CAM-03 回転中心と重力 | P1 | 実機確認 | 回転中心を予測でき、極付近でも急反転しない。重力維持と自由回転を必要に応じて区別する。 | 屋根上・室内・地面なしの場所でOrbit。 |
| CAM-04 全体／選択／前の視点 | P1 | 部分対応 | 全体表示と選択へ寄る操作を分け、見失った時に戻れる。遠い補助線で過剰に引かない。 | 遠方ガイドを置いて選択対象へ寄る。 |
| CAM-05 投影と見え方 | P2 | 部分対応 | 平行・透視の切替で形状を変更せず、FOV変更と距離移動を区別。2点透視は建築用途で別途検討する。 | 室内のFOVとホイールを別々に操作。 |
| CAM-06 DPIと入力機器 | P1 | 実機確認 | 通常マウス・高解像度ホイール・トラックパッド・複数画面で座標、表示サイズ、速度を確認する。 | DPI100→150→200％の画面間を移動。 |

### 17. 中断・履歴・保存・重い処理

段階Esc、直後修正トークン、ネイティブ保存、自動保存回復、長時間処理の保全がある。以下は横断的な製品要件。

確認した入口：[src/app/MainWindow.cpp:285](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/MainWindow.cpp#L285)、[src/app/Tools.cpp:58](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/Tools.cpp#L58)、[src/core/src/NativeFormat.cpp:778](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/NativeFormat.cpp#L778)、[tests/test_gui.cpp:2659](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/tests/test_gui.cpp#L2659)。 以下はSketchBRepの製品要件。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| STATE-01 一段戻るEsc | P1 | 基盤あり | 未確定段階を1つ戻し、確定線を勝手に消さない。ツール終了とグループ終了の優先順位を統一する。 | 線・円弧・回転・尺度・貼付でEsc連打。 |
| STATE-02 Undoの単位 | P0 | 基盤あり | 線→面化、コピー→配列、削除→孤立辺生成を利用者の1操作として戻す。Redoも同じ結果とする。 | 各操作を1回Undoし、要素数と材質を比較。 |
| STATE-03 保存で状態を壊さない | P1 | 要照合 | 作図途中の保存・自動保存で仮形状を混ぜず、最後の確定状態を保存する。失敗は説明し回復可能にする。 | 始点だけ置いて保存→再読込。 |
| STATE-04 古いプレビューを確定しない | P0 | 要設計 | 重い演算を非同期化した場合、要求番号と文書世代で結果を照合。最後の入力の結果だけを確定する。 | 高速ドラッグ→Esc→別編集の後に旧演算が完了。 |
| STATE-05 応答性とキャンセル | P1 | 実機確認 | 候補表示と操作入力を止めず、重い形状生成は間引き・キャッシュ・ジョブ化を適用。キャンセルで半端な変更を残さない。 | 建築モデルで選択・コピー・押出中に取消。 |
| STATE-06 失敗の説明と記録 | P1 | 基盤あり | 何が原因かと次にできる操作を短く示す。診断ログは必要時のみで、操作内容とバージョンを記録する。 | 閉路失敗・自己交差・古い対象の各エラーを比較。 |

### 18. 人間とAIの同じ3D環境

共通Modelerとget_selectionはある。select入力は引き続きIDsのみ。幾何コマンドの共通化と、対象を見せるUIの同等性を分けて評価する。

確認した入口：[src/app/MainWindow.cpp:812](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/MainWindow.cpp#L812)、[src/app/MainWindow.cpp:822](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/app/MainWindow.cpp#L822)、[src/core/src/CommandApi.cpp:1504](https://github.com/higeotoko/sketchBRep/blob/e3c312485ee1f97ef94bbeade72650e978017050/src/core/src/CommandApi.cpp#L1504)。 以下はSketchBRepの製品要件。

| ID／処理 | 優先 | 現状の確度 | 実装・受入要件 | 確認例 |
|---|---|---|---|---|
| AI-01 同じ対象を指す | P1 | 部分対応 | GUIとAIが同じ面・辺集合を指定できる。所有IDだけで面選択をオブジェクト全体へ広げない。 | GUIで1辺を選択し、AIが同じ辺を再選択。 |
| AI-02 提案を見て適用 | P1 | 基盤あり | 生成・削除・変更を画面で区別し、適用する内容がプレビューと一致する。 | 枠押出の提案を表示→承認→Undo。 |
| AI-03 読む操作で作図を壊さない | P0 | 基盤あり | AIの読み取り・提案表示で始点や入力中の値を消さない。 | 線作図中にget_selectionとモデル照会。 |
| AI-04 対象が変わった時 | P0 | 要照合 | 文書世代と対象参照を確認し、別の面を代わりに編集しない。曖昧な対象は明示して再指定する。 | AI提案後に人間が対象面を分割して適用。 |
| AI-05 書込と履歴 | P0 | 基盤あり | 人間・AI・自動保存の変更順序を制御し、共通Undoと原子的バッチで保全する。 | 長演算中のAI書込と取消を組合せる。 |
| AI-06 同じ処理と利用条件 | P1 | 製品原則 | 同じ入力は共通コアで同じ結果にする。GUI専用の幾何裏口を作らず、有料モデルAPIを必須にしない。 | GUI/APIで同じ例を実行し、形状・変更対象・履歴を比較。 |

## 4. ツールを横断する共通処理

### 4.1 選択対象を共有する

「オブジェクト全体」と「オブジェクト内の面・辺集合」を明示的に区別します。画面表示の選択、Move、Delete、コピー・切取、グループ化、APIが同じ意味を使います。所有IDは探索の入口であり、部分選択を全体操作へ昇格する理由にはしません。

対象には編集コンテキストと文書世代も付け、幾何変更の前に全対象を解決します。再生成でインデックスが変わったときは履歴／幾何参照で追い、曖昧なら拒否。全モデルから似た面を無条件に探して代替しません。

### 4.2 入力状態をそろえる

| 状態 | 画面と入力 | Esc・視点・モデル変更 |
|---|---|---|
| 対象待ち | 選べる対象と必要な操作を表示 | Escで選択／ツール既定へ。視点操作可 |
| 基点／作業面取得 | 固定した基点・面を表示 | Escで前段階へ。カメラ移動で固定値を変えない |
| 方向／終点／膨らみ取得 | 仮形状と拘束を表示 | Escで一段戻る。確定部分を勝手に消さない |
| 数値編集中 | 入力欄と単位・対象軸を表示 | Backspaceは編集。IME確定とモデル確定を区別 |
| 計算中 | 最新要求のプレビューと進捗 | Escで取消。旧演算結果は文書世代と要求番号で破棄 |
| 確定直後の再入力可 | 何の寸法・数量を更新できるか表示 | 視点変更・読み取りで維持。別モデル編集・対象消失で終了 |
| エラー | 理由と次の操作を表示 | 直前の正常モデルを維持し、再指定できる |

同じキーでも道具によって意味は異なります。MoveのCtrlはコピー、ScaleのCtrlは中心基準、円弧のCtrlは現在のモード切替など、ツールの状態表に明記します。全ツールへ同じ修飾処理を無条件に共用しません。

クリック／ドラッグ双方を対応させるかはツールごとに明示します。Windowsのダブルクリックしきい値・移動量・DPIを確認し、シングルの副作用がダブル・トリプルで二重に確定しないようにします。

### 4.3 スナップと画面説明を同じ結果から出す

スナップの文字、マーカー、補助線、数値、確定座標を1つの採用結果から導きます。単なるホバーで面を指していることと、入力点がその面上に拘束されたことを区別します。ロック時には「カーソル下は面だが確定点はロック軸上」という状態を正しく説明します。

吸着の画面距離とB-repの幾何許容差を混同しません。画面で近いことを理由に大きな実寸の隙間を勝手に接続しません。視点変更後の再ピックやプレビュー更新も共通化します。

### 4.4 プレビュー・変更・履歴をそろえる

プレビューと確定は同じ対象・パラメータ・幾何処理を使い、プレビューのために確定モデルを書き換えません。計算負荷が高ければ更新頻度を制御し、確定時は最新入力と一致した結果を検証します。計算できていないプレビューを成功した結果として表示しません。

再入力は直前操作の置換。生成物をその場の座標だけで消して作り直すのではなく、その操作が生成・変更した対象の由来を追います。1操作=1 Undo、途中失敗は全体を戻します。

### 4.5 面・線・開いた形状・体積を区別する

有効な線・面・開いたShellも編集可能な正規状態です。面からの体積生成と、既存ソリッドの加減算は別経路にします。閉じた立体には閉鎖性、体積、自己交差を確認し、開いた形状に同じ「閉じていること」を要求しません。

折れ線で面が増えることや、非一様尺度で円が楕円になることは意図した幾何変化です。正規B-repとして表現し、材質・隠し辺・参照・保存を追従させます。単に見た目のメッシュだけ変える対応は採用しません。

## 5. 実装前に決めるべき曖昧な点

| 決定事項 | 初期方針 | 未確認部分の扱い |
|---|---|---|
| 配列の*→/など連続変更 | 最新コードは初回配置距離を保持 | 公式の配列表だけでは不十分。SketchUp実機で座標を測り確定 |
| ホイール中心 | カーソル基準の現行処理を実機評価 | 公式S17とS18は他ツール中の中心説明が矛盾。ツール別に観測 |
| 選択中の面が消える依存関係 | 選択辺を削除して成立しない面は適切に処理 | 「未選択面は絶対に残す」という不可能な要件にしない |
| 確定直後の数値修正の終了条件 | 別のモデル編集・対象失効・明示的な別操作で終了 | 視点・読み取りでは維持。ツール切替を跨ぐかを状態表で固定 |
| 重複位置へのコピー | 既存辺を不要に重複させない | 初回終点が既存境界と一致しても配列の基準を失わない |
| 360度の円形配列 | 同位置の重複を明示して抑制する案 | 生成数・角度の解釈を線形配列と混同しない |
| 部分選択のグループ化・尺度 | 選んだ面・辺を対象として扱う | 共有境界と隣接形状の切離しが必要ならプレビューで示す |
| 1配置だけの非一様尺度 | 共有定義と配置の変形を分離する設計 | 自動で共有定義を変えない。保持形式と再生成への影響を先に検討 |
| 開いた形状へのPush/Pull | 選択面から体積を作り残存形状を保全 | 元形状との接続、内外方向、重複面は受入モデルで確認 |
| 尺度の負倍率 | 正の伸縮を優先し鏡像は別扱い | グリップ越境を黙って裏返しにしない |

これらは利用者に毎回確認ダイアログを出す項目ではありません。開発時に方針を決め、画面で予測できる既定動作にします。

## 6. 実装の作業単位と順序

規模は相対的なS/M/L。実際の工数は対象参照と保存形式への影響で変わります。

| 作業 | 優先・規模 | 対応範囲 | 効果・リスク・完了条件 |
|---|---|---|---|
| W0 数値確定の状態安全 | P0/S | LINE-05、NUM-05 | 空点列参照を再現し修正。数値で面分割しても落ちず、同じ幾何が作れる |
| W1 選択対象の統一 | P0/M | SEL-01、DEL、CLIP-01〜02、GROUP-05、AI-01 | 誤って立体全体を切取・変更しない。共有境界、コピー抽出、原子性に注意 |
| W2 数値入力の一貫性 | P1/M | NUM、SHAPE-01/04、STATE-01/02 | 片側寸法・符号・再入力・IME。入力文法と履歴が副作用を起こさない |
| W3 推論とカメラの継続 | P0〜P1/M | SNAP、CAM-01/02/06 | 最新ラベル・ホイールを再実装せず回帰確認。パンの左クリック漏れを修正 |
| W4 コピーと反復の仕上げ | P1/M | MOVE-04、ARRAY、XFORM-02 | 既存配列を検証し連続入力仕様を固定。面分割でIDが変わる経路に注意 |
| W5 造形の一続きの確認 | P0〜P1/M〜L | LINE-04/06、MOVE-06、PULL、PATH | 枠押出・面削除→再押出・棟・スイープを端から端まで。幾何・参照・材質を同時確認 |
| W6 まとまりと尺度 | P1/L | XFORM-03〜05、GROUP | コンポーネント配置と共有定義を整理。UIだけの修正では完了しない |
| W7 建築モデルとAI共存 | P1/L | STATE-03〜06、AI、LOOK-06 | 古い非同期結果や割込を適用せず、操作が詰まらない。ジョブ化は1経路ずつ |
| W8 補助互換 | P2/個別 | 回転矩形、接線、追加単位、ガイド較正、soft/smooth | 利用頻度順に個別採用。P0修正を止めて全面追随しない |

W0を先に実施し、W1〜W3を優先。新規実装が既存の最新追加で不要になっていれば、受入確認で閉じます。共通基盤は実際に直す経路から最小限ずつ揃え、先に全アーキテクチャを作り直しません。

## 7. 実機比較用の20課題

各課題は「できた／できない」だけでなく、対象の取り違え、余分なクリック、視点変更後の飛び、数値入力の失敗、Undo回数を記録します。

| 課題 | 作業手順 | 合格を判断する点 |
|---|---|---|
| SC01 枠から壁 | 4000×3000の矩形→内側200→枠を3000押出 | 中央を埋めず、断面積2,640,000mm²、押出体積7,920,000,000mm³。中央底面の有無は別扱い |
| SC02 片側寸法 | 矩形2000×1000→3000,→,1500 | 3000×1500。変えていない寸法と基準角が保持される |
| SC03 数値で面分割 | 面の一辺で始点→方向を固定→反対辺に届く長さを入力 | 正しく分割し、空点列参照・異常終了・余計な仮線がない |
| SC04 線の等分割 | 3000mm先へ辺コピー→/3→/5 | 4位置→6位置。元から終点は一定、古い分割線を残さない |
| SC05 部分辺コピー | 箱の1辺→M→Ctrl→600→*5 | 元体積を変えず、その辺のコピー計5本。1 Undoで戻る |
| SC06 5面＋別の線削除 | 上面以外5面＋独立線を選んでDelete | 上面と非選択線を保全。上面境界も選んだ別ケースと区別 |
| SC07 開いた形状の造形 | 箱の面を消す→残る平面を選ぶ→厚み200 | 体積を作り、他の残存面・線を保全。全体種別が正しい |
| SC08 屋根 | 6000×4000×3000箱→上面を中央分割→棟を1500上げる | 両屋根と端部が接続し、自己交差・開口・材質脱落がない |
| SC09 非一様尺度 | 1000箱→幅2000→高さ3000→中心基準を切替 | 非操作軸の寸法と固定側が保たれる。体積・プレビュー一致 |
| SC10 1配置の窓 | 同一コンポーネント窓2個→片方だけ幅変更 | 他方と共有定義の寸法を意図せず変えない。未対応なら理由を明示 |
| SC11 グループ境界 | 接する2グループの片方だけ内部編集 | 外側へ結合・削除・棟変形を伝播しない。外側は参照可 |
| SC12 曲線の閉路 | 直線＋接する円弧を閉じて押出 | 正規円弧、閉路、接線、面化の案内と結果が一致 |
| SC13 複合経路 | 直線＋円弧を事前選択→断面→Follow Me | 接続順序に依存しない。断線・分岐・中空角断面を別ケースで説明 |
| SC14 ガイドで窓配置 | 壁から200の平行線＋高さ900ガイド→交点へ矩形 | 補助線が面を分割せず、参照点と寸法が正しい |
| SC15 段階Esc | 円弧膨らみ→Esc→終点変更、回転と貼付も実施 | 1段階ずつ戻り、確定形状を勝手に消さない |
| SC16 ラベルと拘束 | 端点→中点→面上→作業平面→軸ロック、DPI変更 | 文字・点・確定座標一致。画面端で切れずピックを遮らない |
| SC17 室内の視点 | 線の始点→室内Zoom→中＋左Pan→数値確定 | 不要な点を作らず、方向・始点・入力を維持 |
| SC18 切取と貼付 | 箱の1面／1辺をCtrl+X→別位置へ貼付→Undo | 所有Solid全体を切り取らない。元と貼付先を正しく戻す |
| SC19 編集後の保存 | 屋根・枠・開いた面・配列・材質を含め保存→再読込 | 形状種別、寸法、曲線、内周、階層、材質、後続編集が一致 |
| SC20 人間とAI | 作図中の照会→提案→人間が対象分割→適用、長演算取消 | 読取で作図を壊さず、古い対象・結果を適用せず、履歴が一貫 |

SC01の寸法値は検証用に設定した値で、SketchUp公式の許容差仕様ではありません。数値比較はプロジェクトの幾何許容差を使用し、画面ピクセルの吸着範囲とは別に扱います。

### 記録方法と性能の判定

同じモデル、同じ表示寸法・投影・FOV、同じ入力機器でSketchUpとSketchBRepを録画します。OS、両アプリのバージョン／SHA、Qt・OCCT、DPI、入力機器、GPU、モデル要素数を記録。操作手順・期待値・結果・未確認事項を各IDへ残します。

小モデルに加え、窓・柱をコンポーネントとして増やした建築モデルで繰り返します。最初は要素数1000、10000などの再現可能な段階を用意し、ピック・候補更新・プレビュー・確定・メモリを別計測します。絶対時間の合格値は対象機で測ってから固定し、根拠なく「60fps必須」などを全演算に課しません。

自動テストは対象・幾何・入力状態・Undoの検証に使い、操作感の最終評価を置き換えません。WindowsのGUI実機と、既存Linux CIの回帰を区別して記録してください。

## 8. Claudeの提出形式と完了の定義

各変更には対応ID、原因、変更ファイル、確認した課題、残る制限を記載。108行の管理表へ「要修正／検証待ち／自動確認済／実機確認済／後回し」を記録します。実装があるだけで実機確認済みにしません。

完了条件は、指定した一続きの作業が成立し、対象外の形状を変えず、失敗時に保全でき、Undo/Redo・保存再読込・GUI/APIの等価性を確認できること。対応しないものは理由と代替操作を明記します。

ファイルを分割しても、選択対象・スナップ結果・変更セット・エラーの意味を揃えてください。表示だけのパッチで幾何の欠落を隠したり、テストの期待値だけを現実装へ合わせて終えないでください。

## 9. 参考資料

- [S01 選択](https://help.sketchup.com/en/sketchup/selecting-geometry)
- [S02 線・面・推論](https://help.sketchup.com/en/sketchup/introducing-drawing-basics-and-concepts)
- [S03 削除とUndo](https://help.sketchup.com/en/sketchup/erasing-and-undoing)
- [S04 数値入力・配列](https://help.sketchup.com/en/using-measurements-box)
- [S05 矩形・円・多角形](https://help.sketchup.com/en/sketchup/drawing-basic-shapes)
- [S06 円弧](https://help.sketchup.com/en/sketchup/drawing-arcs)
- [S07 移動](https://help.sketchup.com/en/sketchup/moving-entities-around)
- [S08 伸縮・Autofold](https://help.sketchup.com/en/sketchup/stretching-geometry)
- [S09 回転・鏡像](https://help.sketchup.com/en/sketchup/flipping-mirroring-rotating-and-arrays)
- [S10 尺度](https://help.sketchup.com/en/sketchup/scaling-your-model-or-parts-your-model)
- [S11 Push/Pull](https://help.sketchup.com/en/sketchup/pushing-and-pulling-shapes-3d)
- [S12 オフセット](https://help.sketchup.com/en/sketchup/offsetting-line-existing-geometry)
- [S13 フォローミー](https://help.sketchup.com/en/sketchup/extruding-follow-me)
- [S14 グループ](https://help.sketchup.com/en/sketchup/grouping-geometry)
- [S15 測定・補助線](https://help.sketchup.com/en/sketchup/measuring-angles-and-distances-model-precisely)
- [S16 非表示・ソフト・スムーズ](https://help.sketchup.com/en/sketchup/softening-smoothing-and-hiding-geometry)
- [S17 視点操作](https://help.sketchup.com/en/sketchup/viewing-model)
- [S18 マウス操作](https://help.sketchup.com/en/sketchup/using-your-mouse)

S08は今回の再取得がタイムアウトしたため、この会話で先に確認した同じ公式ページを根拠にしています。S17とS18のズーム中心の説明は一致しないため、実機確認事項として残しました。公式にない細部は本書の製品要件・設計案です。

[S01]: https://help.sketchup.com/en/sketchup/selecting-geometry
[S02]: https://help.sketchup.com/en/sketchup/introducing-drawing-basics-and-concepts
[S03]: https://help.sketchup.com/en/sketchup/erasing-and-undoing
[S04]: https://help.sketchup.com/en/using-measurements-box
[S05]: https://help.sketchup.com/en/sketchup/drawing-basic-shapes
[S06]: https://help.sketchup.com/en/sketchup/drawing-arcs
[S07]: https://help.sketchup.com/en/sketchup/moving-entities-around
[S08]: https://help.sketchup.com/en/sketchup/stretching-geometry
[S09]: https://help.sketchup.com/en/sketchup/flipping-mirroring-rotating-and-arrays
[S10]: https://help.sketchup.com/en/sketchup/scaling-your-model-or-parts-your-model
[S11]: https://help.sketchup.com/en/sketchup/pushing-and-pulling-shapes-3d
[S12]: https://help.sketchup.com/en/sketchup/offsetting-line-existing-geometry
[S13]: https://help.sketchup.com/en/sketchup/extruding-follow-me
[S14]: https://help.sketchup.com/en/sketchup/grouping-geometry
[S15]: https://help.sketchup.com/en/sketchup/measuring-angles-and-distances-model-precisely
[S16]: https://help.sketchup.com/en/sketchup/softening-smoothing-and-hiding-geometry
[S17]: https://help.sketchup.com/en/sketchup/viewing-model
[S18]: https://help.sketchup.com/en/sketchup/using-your-mouse

