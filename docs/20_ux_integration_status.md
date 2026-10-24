# 操作感の統合：108項目の管理表

基準：`docs/reviews/2026-10-23_ux_integration_108.md`（指示書、固定 HEAD e3c3124）。
この表は、指示書 §8 の「要修正／検証待ち／自動確認済／実機確認済／後回し」で各項目の状態を記録する。

## 状態の意味

| 状態 | 意味 |
|---|---|
| 自動確認済 | 自動テスト（Linux CI、Xvfb 上の GUI テストを含む）で、行の要件の中心を確かめた。操作感の最終評価ではない |
| 検証待ち | 実装の経路はあるが、行の要件全体をまだ自動確認していない。または実機で比べる必要がある |
| 要修正 | 要件と違う動きを確認した（直すまで残す） |
| 後回し | P2・設計が先の項目。今は手を付けない |
| 実機確認済 | Windows の実機で人が確かめた（**現時点では 0 件**） |

**実機の操作確認はまだしていない。**
- 自動テストは、Linux の Xvfb 上で動く合成イベントによるもの。
- DPI の確認は、`QT_SCALE_FACTOR` で倍率を変えたスクリーンショットを見ただけ。

## 作業単位の記録

### W0：LINE-05、NUM-05（968d532）

**原因**：
- 数値で長さを打つと、`LineTool::applyMeasurement` は `addPoint` の後で `points_.back()` を読んでいた。
- ところが `addPoint` は、面を分割したときや閉路を閉じたときに `points_` を空にする。空の配列の末尾を読むのは未定義動作。
- `_GLIBCXX_ASSERTIONS` を有効にした状態で、アサーションが落ちることを確かめて再現した。

**修正**：
- 線が終わったら、その後は何も読まないようにした。
- Linux ビルドで `_GLIBCXX_ASSERTIONS` を常に有効にした。今後、同じ種類の誤りはテストで止まる。

**テスト**：`typedLengthSplitsOrClosesSafely`
- SC03：箱の上面とばらの面を、数値で分割する
- 数値の 4 辺で閉じる
- 線が終わった後の数値入力は、何も作らない

### W1：CLIP-01、CLIP-02、SEL-01、GROUP-05、AI-01（5088cee）

**原因**：
- Ctrl+C / Ctrl+X は、選んだ面や辺を持つオブジェクトの ID を渡していた。そのため、面 1 枚・辺 1 本のつもりでも、オブジェクト全体をコピー・切り取りしていた。
- Move + Ctrl で面を選んでいると、コピーではなく面の押し出しになっていた。

**修正**：
- 選択対象を 1 つの表現にした（`ToolHost::selectionTargets`）。オブジェクトごとに「選んだ面・辺」か「オブジェクト全体」かを持ち、Delete で使っていたものと同じ。
- コピー・切り取り・Move / Rotate のコピーは、すべてこの選択対象を使う。
- `Modeler::copyToClipboard(targets)`：面は独立した面としてコピーする（穴と材質を保つ）。辺は正確な線としてコピーする（円弧や曲線はそのまま）。
- `Modeler::copyElements`：配列コピー。
- 切り取りは「コピー＋共通の `deleteSelection`」。削除が断られたときは、クリップボードを元に戻す。
- オブジェクトの一部だけを選んだときは、全体に広げず、理由を出して断る：
  - 移動・回転（角や面の変形として扱えるものを除く）
  - グループ化・コンポーネント化
- API の `copy_array` は、`delete` と同じ `elements` 指定を受け取る。

**テスト**：
- `cutCopyAndMoveCopyTakeOnlyPickedElements`
  - SC18：上面を Ctrl+X すると、箱は 5 面の開いた形状になる。同位置に貼り付け、Undo
  - 辺 1 本を Ctrl+C / Ctrl+V すると線になる
  - 面を Move + Ctrl すると面のコピーになる
  - 一部だけのグループ化は断られる
- `SelectionTargets.ElementsCopyOnlyThemselves`（材質、配列、Undo、API）
- SC06 は既存の `DeleteSelection.*` と `deleteMixedSelection` で確認済み

### W2：NUM-02、NUM-03、NUM-04、NUM-05（NUM-06 の一部）（5868478）

**原因**：
- 矩形は 2 つの値が必須で、`abs` を取って符号を捨てていた。
- 移動を直後に再入力するとき、負の値を受け付けていなかった。
- IME で入力した全角数字が読めなかった。

**修正**：
- 矩形：
  - 片方だけの入力：`3000,` は幅だけ、`,1500` は奥行きだけ。もう一方の辺と最初の角はそのまま（`units::parseLengthSlots`）。
  - 負の値は、その辺の向きを反転する。
  - 0 や解釈できない入力は断り、何も変えない。
- 移動：負の距離は逆向き。直後の再入力でも、マウスで示した向きを基準に読む。0 は断る。
- IME：全角の数字・記号・単位と「、」を半角として読む。注釈などの文章は、入力したまま残す。

**テスト**：
- `rectangleOneSideSignsAndReentry`
  - SC02
  - 単位、テンキー、全角
  - 不正な値
  - 負の値
  - Undo が 1 回で戻ること
  - 移動の負の値（入力中と直後の再入力）
- `Units.LengthSlotsMayBeEmpty`

### W3：CAM-01（0e6edf8）

**原因**：中ボタンで回転している間に左ボタンを押すと、左の押下が作図ツールに届き、線ツールに余計な点が入っていた。修正前のビルドで、テストが落ちることを確かめた。

**修正**：
- 中ボタンのドラッグ中に左を押したら、パンに切り替える。
- その離しとダブルクリックは、カメラの操作として扱う。
- 始点・方向・入力中の値は、そのまま続く。

**テスト**：`middleLeftPanAddsNoPoint`（SC17）

ホイールと SnapTip は、既存の `wheelZoom*` と `snapTipNamesTheAdoptedPoint` で回帰を確認した（すべて通過）。

### W4：MOVE-04、ARRAY-03、XFORM-02（a95f946）

**原因**：
- 回転配列の「/n」は、その時点の配列全体の角度を割っていた。Move の「最初に置いた距離を基準にする」決まり（ARR-02）と食い違っていた。
- 30°で `*12` にすると、12 個目が元の位置にぴったり重なって二重になっていた。

**修正**：
- 回転は、与えた角度（回転した角度、またはその後に入力した角度）を基準にする。
  - `*n` は、その角度を n 回。
  - `/n` は、その角度を n 等分。
  - 入力し直した角度は、分割中なら分割を保つ。
  - 負の角度は逆回り、0 は断る。
- コピー・配列（`copy`、`copyEdges`、`copyElements`）では、元と同じ位置に戻るコピー（一周した回転や、移動量ゼロ）は作らない。

**テスト**：
- `polarArrayFullCircleAndCtrlMidMove`
  - 30°×12 で、コピー 11 個（元を含め 12）
  - `/3` で 3 個（10°刻み）
  - Undo が 1 回で戻ること
  - MOVE-04：変形の途中で Ctrl をオン／オフしても、確定するまで何も変わらない。最後は変形として 1 段で確定する
- `SelectionTargets.PolarArrayFullCircleSkipsTheOriginal`
- `Overlaps.CopiesAndMoves`：その場コピーは何も作らない（重複しない）。この新しい規則に合わせて期待値を変えた

**暫定**（ARRAY-04、実機で比べる）：記法を続けて変えたときの基準（最初の配置か、直前の結果か）。

### W5：一続きの造形（SC01、SC07、SC08、SC13、SC19）（5624e4e）

**目的**：作る→直す→保存→再読込→さらに編集、を端から端まで通す。

**テスト**：`tests/test_flows.cpp`
- `Flows.SC01_FrameWall`
  - 4000×3000 の面を内側に 200 オフセットし、枠を 3000 押し出す
  - 枠の面積 2,640,000 mm²、体積 7,920,000,000 mm³
  - 中央は埋めずに、面のまま残る
- `Flows.SC07_SC08_SC19_EditSaveReloadEdit`
  - SC07：箱の上面を消して、前面に厚み 200 を付ける。残りは開いた形状のまま
  - SC08：6000×4000×3000 の上面を塗って中央で分割し、棟を 1500 上げる。両方の屋根面が塗装を保つ
  - コピーした線で床を 3 分割する
  - 正確な円弧
  - SC19：保存して再読込しても、種類・体積・塗装・円弧が同じ。その後、分割した中央を押し出せる
- `Flows.SC13_HollowSectionAlongLineAndArc`：中空の断面を、直線＋接する円弧に沿ってスイープする。選ぶ順を逆にしても同じで、穴は貫通する
- `Flows.SC13_HollowSectionAroundASharpCorner`：中空の断面で直角の角を回る

**修正**（PATH-05）：
- 角のある経路の予備スイープは、穴のある断面を拒否していた。
- 外周と各内周を別々に留め継ぎでスイープし、内周の分をくり抜くようにした。中空のまま作れる。

### W6：XFORM-05（SC10）（2bef301）

**原因**：コンポーネントの配置を非一様に伸縮しようとすると、共有の定義が変わってしまうため、一律に拒否していた。

**修正**：
- 配置ごとの伸縮（`ComponentInstance::stretches`）を足した。配置ごとの鏡像と同じ扱い。
  - 配置の座標で記録し、その配置の形状だけを、正確な B-rep として変換する。
  - 同じ設定の配置どうしは、形状を共有する（`placementKey`）。
- 共有の定義と、ほかの配置は変わらない。
- 保存・再読込・Undo に対応した。
- 伸縮した配置から、定義をその場で編集することと、分解は、理由を出して断る。

**テスト**：`Stretch.OnePlacementOnItsOwn`
- SC10：2 つの配置のうち片方だけを、幅 2 倍にする
- もう片方と定義は変わらない
- 保存と再読込
- その場編集の拒否
- Undo

**残り**：
- 伸縮した配置から定義を編集する方法（伸縮を保ったまま編集する）。
- 「固有にする」で伸縮を定義に焼き込む操作。

## 108 項目

### 1. 選択・範囲・右クリック
| ID | 状態 | 根拠・残り |
|---|---|---|
| SEL-01 | 自動確認済 | W1。一部だけの移動・回転・グループ化は断る。ポリラインの 1 辺だけを移動する操作は、まだできない |
| SEL-02 | 要修正 | Shift での追加・解除だけ。Ctrl で追加、Ctrl+Shift で除外は未実装 |
| SEL-03 | 検証待ち | `selectFacesEdgesAndWholeObject`。OS のダブルクリック間隔との整合は実機で確認する |
| SEL-04 | 要修正 | 範囲選択はオブジェクトのバウンディングボックス単位（`boxSelectionWindowAndCrossing`）。面・辺単位の判定はない |
| SEL-05 | 自動確認済 | `rightClickMenuFollowsTheSelection`、ロック時のメニュー |
| SEL-06 | 検証待ち | ホバーで強調しない／選択は青／ロックは赤。透明面・重なった面の確認はまだ |

### 2. 削除・消しゴム
| ID | 状態 | 根拠・残り |
|---|---|---|
| DEL-01 | 自動確認済 | `DeleteFaces.*`、`deleteLooseFaceKeepsItsOutline` |
| DEL-02 | 自動確認済 | `DeleteSelection.*`（上面の境界を含む場合と含まない場合） |
| DEL-03 | 自動確認済 | `DeleteSelection.*`、`EraseLines.*`、`eraserWorksOnLines` |
| DEL-04 | 自動確認済 | `eraserDragErasesCrossedLines`、`eraserDragFollowsWhatTheLineBecame` |
| DEL-05 | 自動確認済 | `eraserShiftHidesEdgeLine`、`HiddenEdges` |
| DEL-06 | 自動確認済 | `DeleteSelection`（ロック対象を含むと全体が戻る） |

### 3. 吸着・推論
| ID | 状態 | 根拠・残り |
|---|---|---|
| SNAP-01 | 自動確認済 | `snapTipNamesTheAdoptedPoint`（表示した点＝確定した点） |
| SNAP-02 | 検証待ち | 実機で、密集した部分を横切って確認する |
| SNAP-03 | 検証待ち | `Inference.*` に追跡のテストあり。古い参照の扱いはまだ確認していない |
| SNAP-04 | 検証待ち | 矢印キーで軸、Shift で固定はある。下キーの意味は公式の操作と照合する |
| SNAP-05 | 自動確認済 | ラベルがビューの中に収まり、カーソルと点を隠さないことを確認。DPI はスクリーンショットだけ |
| SNAP-06 | 後回し | 円の中心・接線などの推論 |

### 4. 寸法入力
| ID | 状態 | 根拠・残り |
|---|---|---|
| NUM-01 | 自動確認済 | `typedUnitsDoNotTriggerShortcuts`、テンキー（W2） |
| NUM-02 | 自動確認済 | W2（矩形）。円・多角形には該当しない |
| NUM-03 | 自動確認済 | W2（矩形・移動）。そのほかのツールの符号は、まだ全部は見ていない |
| NUM-04 | 自動確認済 | `typedRevisionOnlyRightAfter`、`rectangleOneSideSignsAndReentry`、`pushPullDoubleClickRepeatsDistance`、`scaleToolFactorAndLength` |
| NUM-05 | 自動確認済 | 線・矩形・移動（W0 / W2）。そのほかのツールは検証待ち |
| NUM-06 | 後回し | 座標入力・英米単位。IME の全角は W2 で対応した |

### 5. 線
| ID | 状態 | 根拠・残り |
|---|---|---|
| LINE-01 | 自動確認済 | `escStepsBackAndConstraintsShow`（Esc で確定済みの線は残る） |
| LINE-02 | 検証待ち | 単独の線への T 字接続は、接続点を作る（`Overlaps.Lines`）。面の辺の途中への接続はまだ |
| LINE-03 | 検証待ち | 閉じる前の案内あり。隙間のある輪郭はまだ確認していない |
| LINE-04 | 自動確認済 | `lineAcrossRectangleSplitsProfile`、`lineAcrossWallSplitsFace`、`SplitAcross.*`（はみ出した線、複数の面、コピー、後から描いた面、穴）、`lineAcrossStripsInAGroupDividesEach` |
| LINE-05 | 自動確認済 | W0 |
| LINE-06 | 自動確認済 | `lineAgainstVolumeMakesFace` |

### 6. 矩形・円・円弧
| ID | 状態 | 根拠・残り |
|---|---|---|
| SHAPE-01 | 検証待ち | `lineToolStaysOnWallPlane`。描いている途中の Orbit はまだ確認していない |
| SHAPE-02 | 後回し | |
| SHAPE-03 | 後回し | |
| SHAPE-04 | 自動確認済 | `circleToolTypedRadius` |
| SHAPE-05 | 自動確認済 | `arcToolSemicircleAndBulgeEdit`、`escStepsBackAndConstraintsShow` |
| SHAPE-06 | 後回し | |

### 7. 移動
| ID | 状態 | 根拠・残り |
|---|---|---|
| MOVE-01 | 検証待ち | |
| MOVE-02 | 検証待ち | 後から選ぶ場合は、オブジェクト全体を掴む |
| MOVE-03 | 検証待ち | |
| MOVE-04 | 自動確認済 | `copySelectedEdgeArrays`、W1（面のコピー）、W4（変形中の Ctrl 切替） |
| MOVE-05 | 要修正 | 面は法線方向にしか動かせない（`moveSelectedFaceStretchesSolid`）。横方向への移動は未対応 |
| MOVE-06 | 自動確認済 | `moveToolLiftsRidgeIntoGable`、`Roof.*` |

### 8. 線コピー・配列
| ID | 状態 | 根拠・残り |
|---|---|---|
| ARRAY-01 | 自動確認済 | `copySelectedEdgeArrays`、`moveCopyAndArray` |
| ARRAY-02 | 自動確認済 | 同上 |
| ARRAY-03 | 自動確認済 | 同上（Undo 1 回）、回転も同じ規則（W4） |
| ARRAY-04 | 検証待ち | 暫定仕様。SketchUp の実機と比べてから決める |
| ARRAY-05 | 自動確認済 | `copyOntoEdgeThenDivideKeepsSolid`、`EdgeArrays.DividingAFace` |
| ARRAY-06 | 検証待ち | 円弧は円弧のまま（`EdgeArrays.ArcsStayArcs`）。グループ単位はまだ |

### 9. 回転・尺度・鏡像
| ID | 状態 | 根拠・残り |
|---|---|---|
| XFORM-01 | 検証待ち | `rotateByTypedAngle` |
| XFORM-02 | 自動確認済 | W4：一周して元に戻るコピーは作らない。`/n` は与えた角度を割る |
| XFORM-03 | 自動確認済 | `scaleGripsStretchEachDirection`、`Stretch.*` |
| XFORM-04 | 自動確認済 | 同上 |
| XFORM-05 | 自動確認済 | W6：その配置だけを伸縮する（`Stretch.OnePlacementOnItsOwn`）。伸縮した配置からのその場編集と分解は、理由を出して断る |
| XFORM-06 | 後回し | |

### 10. Push/Pull
| ID | 状態 | 根拠・残り |
|---|---|---|
| PULL-01 | 検証待ち | |
| PULL-02 | 自動確認済 | `Offset.*`、`Flows.SC01_FrameWall` |
| PULL-03 | 検証待ち | |
| PULL-04 | 検証待ち | 同じ量の繰り返しはあり（`pushPullDoubleClickRepeatsDistance`）。開始面を残す操作は Ctrl（開いた形状のみ、状態はステータスに表示。`pushPullOnOpenShape`）。ソリッドでの Ctrl はまだない |
| PULL-05 | 自動確認済 | 2026-10-24 から既定は SketchUp の面と同じ（面が動いて形状が広がる）。新しい体積は Ctrl（開始面を残す）で作る。`OpenPushPull.*`、`pushPullOnOpenShape`、`Flows.SC07_*` |
| PULL-06 | 検証待ち | |

### 11. オフセット・フォローミー
| ID | 状態 | 根拠・残り |
|---|---|---|
| PATH-01 | 自動確認済 | `Offset.*`、`offsetToolInwards` |
| PATH-02 | 後回し | |
| PATH-03 | 検証待ち | 制限あり（理由は表示する） |
| PATH-04 | 自動確認済 | `FollowMeEdges.*`、`Flows.SC13_*`（選ぶ順を逆にしても同じ） |
| PATH-05 | 自動確認済 | W5：中空の断面を、直線＋円弧と直角の角で（`Flows.SC13_*`）。断面の向きの案内表示はまだ |
| PATH-06 | 後回し | |

### 12. グループ・コンポーネント
| ID | 状態 | 根拠・残り |
|---|---|---|
| GROUP-01 | 自動確認済 | `groupEditModeAndLooseFaceDelete`、`groupEditShowsDottedBox` |
| GROUP-02 | 検証待ち | 重なりの整理はグループの境界を越えない（`Overlaps.OtherGroupsSolidsAndHeightsStayApart`） |
| GROUP-03 | 検証待ち | 編集中は点線の箱を表示。階層の表示はまだ |
| GROUP-04 | 自動確認済 | `componentEditInPlace`、`Components.*` |
| GROUP-05 | 自動確認済 | W1（一部だけは断る）。部分選択から作るグループはまだない |
| GROUP-06 | 自動確認済 | `lockedObjectsSelectableNotEditable`、`imageUnderlayCalibrateLockTrace` |

### 13. コピー・切り取り・貼り付け
| ID | 状態 | 根拠・残り |
|---|---|---|
| CLIP-01 | 自動確認済 | W1 |
| CLIP-02 | 自動確認済 | W1 |
| CLIP-03 | 自動確認済 | `copyPasteFollowsCursor` |
| CLIP-04 | 検証待ち | 回転したグループの中から外への貼り付けは、まだ確認していない |
| CLIP-05 | 検証待ち | |
| CLIP-06 | 検証待ち | `Clipboard.*` |

### 14. メジャー・補助線
| ID | 状態 | 根拠・残り |
|---|---|---|
| GUIDE-01 | 自動確認済 | `tapeMeasureReadsDistance`、`tapeMeasurePullsGuideOffEdge` |
| GUIDE-02 | 自動確認済 | `gridLinesSnapAtCrossings` |
| GUIDE-03 | 検証待ち | |
| GUIDE-04 | 自動確認済 | `protractorPlacesAngledGuide` |
| GUIDE-05 | 後回し | 下絵の較正はある（`imageUnderlayCalibrateLockTrace`） |
| GUIDE-06 | 検証待ち | |

### 15. 材質・表示
| ID | 状態 | 根拠・残り |
|---|---|---|
| LOOK-01 | 自動確認済 | `paintToolAndEyedropper`、`Materials.*` |
| LOOK-02 | 自動確認済 | 同上 |
| LOOK-03 | 自動確認済 | `Stretch.*`、`Overlaps.*`、屋根（`Flows.SC07_SC08_SC19_*`、両方の屋根面が塗装を保つ） |
| LOOK-04 | 検証待ち | |
| LOOK-05 | 後回し | |
| LOOK-06 | 検証待ち | |

### 16. カメラ
| ID | 状態 | 根拠・残り |
|---|---|---|
| CAM-01 | 自動確認済 | W3、`wheelZoomKeepsDrawing` |
| CAM-02 | 自動確認済 | `wheelZoom*`。速さの感じ方は実機で確認する |
| CAM-03 | 検証待ち | 実機 |
| CAM-04 | 検証待ち | 左ダブルクリックでの全体表示はやめた（中ボタンだけ） |
| CAM-05 | 検証待ち | |
| CAM-06 | 検証待ち | 実機 |

### 17. 中断・履歴・保存
| ID | 状態 | 根拠・残り |
|---|---|---|
| STATE-01 | 自動確認済 | `escStepsBackAndConstraintsShow` |
| STATE-02 | 自動確認済 | 各テストの Undo 回数 |
| STATE-03 | 検証待ち | 保存と再読込は `Flows.SC07_SC08_SC19_*`。作図の途中で保存したときはまだ確認していない |
| STATE-04 | 後回し | 非同期の演算はまだない |
| STATE-05 | 検証待ち | 実機 |
| STATE-06 | 検証待ち | |

### 18. 人間と AI
| ID | 状態 | 根拠・残り |
|---|---|---|
| AI-01 | 自動確認済 | `delete` と `copy_array` の `elements`（W1）。`select` は ID のみ |
| AI-02 | 自動確認済 | `aiProposalApplyAndReject` |
| AI-03 | 自動確認済 | `aiReadsKeepDrawingInProgress` |
| AI-04 | 検証待ち | FaceRef は曖昧なときに拒否する（`FaceRefTest`） |
| AI-05 | 自動確認済 | `Transactions.*`、`aiWaitsForLongOperation` |
| AI-06 | 検証待ち | |
