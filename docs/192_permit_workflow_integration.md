# 192. 申請図の工程・接地外周の再照合・居室と開口の入力確認（統合 2026-11-19）

Codex から引き継いだ3つの差分（引き継ぎZIP `SketchBRep-Claude-引き継ぎ-2026-11-19.zip`、調査資料 第2.8版 §8〜10）を、`504cda0` の上に順番に取り込んだ。差分の中身は変えずに当てた。差分ごとにビルドと関連テストを回している。

| 順 | コミット | 内容 |
|---|---|---|
| 1 | `55f50c4` | 接地外周の再照合。建物IDと照合したときのワールド変換を `SiteGround::placement` に保存する。建物や親グループの移動・回転・削除を検知したら、配置図・PDF・DXF・図面一式で参考平均と計算表を保留する。コマンド `get_site_ground_status` を追加。`set_site_ground` に `building` と `confirm_building_placement` を追加。 |
| 2 | `b9f3d83` | 申請図の工程ドック（`PermitWorkflowPanel`）。「ファイル → 申請図の工程」から開く。12工程を行き来でき、敷地の入力状況と足りない理由を表示し、既存の入力画面と検討用の配置図を開ける。申請用出力は保留のまま。 |
| 3 | `0740804` | 居室・開口の入力確認（第4工程）。室ごとの用途・内法面積・天井高と、直線の壁芯区間に収まる矩形開口の候補、どの室にも対応しなかった開口を表示する。読み取りコマンド `room_opening_inputs` を追加。採光・自然換気・機械換気は未評価のまま。 |

BIM 操作の約束（docs/90）との関係：室と開口は ID で追跡し、表から元の室の選択や建具表へ移れる（B 系の「同じ物を図と表でたどる」）。開口候補は幾何で拾った参考で、法令の判定には使わない。

## 確認

**計算テスト済み**
- Windows ローカル：コア全件 759件成功・8件スキップ・失敗0。
- [Linux CI（`0740804`）](https://github.com/higeotoko/sketchBRep/actions/runs/44489517158)：CTest 780件、失敗0（GUI を含む）。

**GUI 自動確認済み**
- ローカル：関連 GUI 5件（`siteNorthFromTwoPlanPoints`、`sitePlanFromASelectedBoundary`、`permitWorkflowTracksSiteInputsAndStaleGround`、`permitWorkflowShowsRoomAndOpeningInputs`、`toolsMenuIsGroupedAndProposalBarIsReadable`）が成功。
- ローカルの GUI 全件は 237件成功・8件失敗・7件スキップ。失敗した8件は画面の画素やクリック位置を読むテスト（`copySelectedEdgeArrays`、`curveToolOpenAndClosed`、`topEdgesCopiedDownDivideTheSides`、`offsetToolOnSolidFace`、`selectionDotsKeepTheMaterialColour`、`selectionOutlineOnOnePlacementOnly`、`paintedFaceFollowsItsMaterial`、`movedObjectsPickWhereTheyAre`）。同じコミットの Linux CI では成功しているので、表示倍率175%によるローカルだけの失敗と判断した（docs/71 §4）。
- 展開した ZIP：通常検証18シナリオ、べた基礎4シナリオとも失敗0。記録は [通常検証](validation/2026-11-19-portable-0740804.json) と [べた基礎](validation/2026-11-19-portable-0740804-mat.json)。

**架空モデルの通し確認（sbr_cli）**
作ったモデル：敷地20 m×15 m、南側に道路、4 m×5 m の二室が中壁を共有、南壁に窓2つ、壁は「建物」グループにまとめた。次の14項目がすべて期待どおりだった。
- 接地外周の状態が、未登録で `unassigned`、建物と関連付けて `ready`（参考平均 175 mm）になる。
- 建物グループを動かすと `needs_review` になり、参考平均を返さない。この状態の配置図DXFには「接地外周の参考平均は保留。建物の配置が変わりました…」と出て、計算表は出ない。
- 確認なしの再登録は拒否される。Undo で `ready` と同じ参考平均に戻る。SBR を保存して開き直しても `ready` で、関連付けた建物も残る。もう一度動かしてから `confirm_building_placement: true` で確定すると `ready` に戻る。
- 二室それぞれに窓が1つずつ候補として付く。用途と天井高を読み、未入力の天井高は `null`、採光は「未評価」。
- 窓を中壁をまたぐ位置へ動かすと、その窓はどの室の候補からも外れて「未対応」になる。窓の幅を変えると表の値が追従する。

**Windows 実機確認（人の操作）**：まだ。

## 見つけたこと（今回の差分とは別）

- 平らな面（たとえば敷地の輪郭）を z=0 に先に描き、その上に `create_wall` で壁を描くと、壁が面にくっついて部屋判定で「読めない壁」（`unreadable_walls`）になり、部屋が閉じない。壁を先に描いてから輪郭を描けば問題ない。今回の3差分は部屋判定に触れていないので、以前からある挙動と考えられる。SketchUp から組み直すときに起きやすいので、別の作業で調べる。

## 配布

- Windows ZIP：`SketchBRep-windows-x64-0740804-local.zip`、81,994,501 bytes、SHA256 `e155bab9a97dee38802b6e4ec7f58f83b2e5456b2732fbe4055c9e0b2c06d2f6`（Codex のチャットフォルダの `outputs\`）。SketchUp SDK の DLL は docs/182 の決定どおり同梱。
- Windows のフルCI（`ci-*` タグ）はまだ実行していない。
