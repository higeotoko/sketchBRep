# 271. SketchUpのタグ・戻し込みと建具配置の確認（2026-11-23）

## 13-4 タグと壁の属性

配布版 `2eb59bb` で例題Aのコピーの1階西壁に「1F 外壁」のタグを付け、SKPに書き出した。同梱SDKで読み戻した壁のタグは「1F 外壁」。`SketchBRep` 辞書に由来する `skp_attributes` に `kind=wall`、`wall_type=exterior_siding`、サイディングから石膏ボードまでの5層を確認した。コアテスト `Skp.TagsAndInformationGoAndComeBack` も成功。

配布版 `92763c0` で対象を例題Aの壁12枚に広げた。各階の外壁4枚と間仕切り2枚に「1FL 外壁」「1FL 間仕切り」「2FL 外壁」「2FL 間仕切り」を付け、壁だけを日本語名のSKPへ出力した。同梱のSketchUp SDKのC APIでSKPのタグと `SketchBRep` 属性辞書を直接読み、SketchBRepに読み戻したソリッド12個とも照合した。元のID・基準階・壁の種類・仕様名・材料層の名前と厚さが一致した。外壁は5層、間仕切りは3層。配布フォルダの `outputs/skp-wall-tag-matrix/` に検証用SKPと部材別の結果 `result.json` がある。

## 13-6 編集の戻し込み

コアテスト `Skp.ChangesInSketchUpComeBack` が成功した。壁を455 mm動かすと壁の種類を保って隣の壁が伸び、編集・削除された箱は検出される。置き換え・削除の指定で結果が変わり、動かした壁と箱は取り消し1回で元に戻る。別のSKPからの追加も確認している。例題Aでは2026-11-18にSketchUp本体で作った移動・形状変更・削除・追加を戻した記録がある。 [以前の本体確認](186_sketchup_round_trip.md)

今回のSketchUp本体は起動時にサインインを要求した。13-4のSketchUp上の表示と、13-6のSketchUp本体での編集は今回未確認。 [SketchUpのRuby起動機能](https://github.com/SketchUp/sketchup-extension-vscode-project/blob/main/CLAUDE.md)

配布版 `2eb59bb` の「ファイル > SketchUp で直した物を戻す...」をMCPで操作した。例題Aのコピーから、同梱SDKで西壁を−1,000 mm動かし、箱1つを削除し、別の箱の高さを500 mmから700 mmに拡大し、机を追加したSKPを用意した。ダイアログには移動・追加・形状変更・削除が各1件と表示された。

| 選択 | 結果 |
|---|---|
| Cancel | 元の51オブジェクトのID・種類・外形・体積・属性が一致した |
| いいえ（動かす・足すだけ） | 西壁は壁のまま−1,000 mm動き、つながる壁の西端も追従。机（600 × 600 × 700 mm）が加わり、両方の箱が元の形で残った |
| はい（置き換え・消す） | 西壁とつながる壁の移動、机の追加に加え、削除した箱が消え、もう一方の箱は高さ700 mmになった |

「はい」と「いいえ」の各操作後に編集メニューの「元に戻す」を1回選び、元の51オブジェクトに戻ったことをキャンセル時と同じ項目で照合した。確認画像と数値は配布フォルダの `outputs/skp-sync-gui-check/confirmation.png` と `result.json`。

### キャンセル時の履歴

B-08（編集を壊さない）の確認。配布版 `4dbf133` と同じ処理で2件を再現した。SKP側に削除しかないと確認画面を出す前に直前の編集が取り消され、移動もあると既存の「やり直し」が仮適用の操作に置き換わった。

修正後は、移動・追加の仮適用を確認前に履歴へ登録しない。確認が必要なら仮適用を破棄し、画面を出す。キャンセルのGUIテスト2件は直前の編集とやり直しの履歴を照合し、やり直しで元の編集を復元できることを確認する。確認データは配布フォルダの `outputs/skp-sync-history-check/before.xml` と `after.xml`。

### 履歴修正の配布版

展開したZIPの実際の画面で、例題Aの戻し込みを再確認した。キャンセルで元の51オブジェクトを保ち、「はい」と「いいえ」はそれぞれ選んだ内容を適用して取り消し1回で元に戻った。SKPに壁の移動しかない場合は確認画面を出さずに反映し、1回の取り消しで天井などの追従も含めて元に戻った。結果は配布フォルダの `outputs/skp-sync-history-check/result.json`。SketchUp本体での再編集はサインイン待ち。

- Windowsコアは810件成功・8件スキップ。ファイル交換GUIは回帰2条件を含む13条件成功。
- ZIPは通常18条件・べた基礎4条件・キャンセルの回帰2条件が成功。回帰結果は配布フォルダの `outputs/skp-sync-history-check/portable.xml`。 [通常のZIP検証](validation/2026-11-23-portable-dc5da59.json)・[べた基礎のZIP検証](validation/2026-11-23-portable-dc5da59-mat.json)
- Linux CIは831件すべて成功。 [run 45138480699](https://github.com/higeotoko/sketchBRep/actions/runs/45138480699)

| 項目 | 値 |
|---|---|
| コード | `dc5da59` |
| ZIP | `SketchBRep-windows-x64-dc5da59-local.zip` |
| サイズ | 83,317,945 bytes |
| SHA256 | `d96bfddb09b5dd16ad7024fd759e65f861e93a6b3bbe372109b0d415cdad45db` |

### 鍵付き部材がSKP側で削除された場合

B-08（編集を壊さない）の確認。配布版 `dc5da59` で、例題AのSKP側から削除した箱にSketchBRepで鍵を掛けて戻し込むと、鍵付き削除でエラーになり、同じSKPの壁の移動・別の箱の形状変更・机の追加も反映されなかった。

修正後は、SKPに含まれなくなった鍵付き部材を削除候補から外し、`sync_skp` の `locked` で返す。回帰テスト `Skp.ChangesInSketchUpComeBack` では、鍵付きの箱を残し、ほかの移動・置き換えを反映し、取り消し1回で戻ることを照合する。

配布版 `92763c0` のCLIとGUIで同じ例題Aを再確認した。削除された鍵付きの箱は `locked` で返り、確認画面の削除候補から外れた。「はい」を選ぶと、西壁とつながる壁の西端が1,000 mm動き、別の箱は高さ700 mmになり、机が追加された。取り消し1回で戻った。配布フォルダの `outputs/skp-sync-locked-check/` にCLIの修正前後とGUIの結果がある。SketchUp本体での再編集はサインイン待ち。

- Windowsコアは810件成功・8件スキップ。ZIPは通常18条件・べた基礎4条件が成功。 [通常のZIP検証](validation/2026-11-23-portable-92763c0.json)・[べた基礎のZIP検証](validation/2026-11-23-portable-92763c0-mat.json)

| 項目 | 値 |
|---|---|
| コード | `92763c0` |
| Linux CI | 831件すべて成功。[run 45141973094](https://github.com/higeotoko/sketchBRep/actions/runs/45141973094) |
| ZIP | `SketchBRep-windows-x64-92763c0-local.zip` |
| サイズ | 83,320,678 bytes |
| SHA256 | `91d1f547fae28f75d51a0bf44bbeb868a62467aa24f3098505a96cd91d3f97af` |

## 13-8 平面で片開きドアを配置

展開したWindows ZIPのGUIテスト3件が成功。建具一覧から片開きドアを選び、壁をクリックすると開口ができた。マウス位置に応じて開き方の記号が出て、反対側を選べば開く方向も変わった。取り消し1回で開口と建具が戻った。 [建具の配置](189_place_a_fitting_in_plan.md)

配布版 `2eb59bb` をMCPで操作し、平面の建具一覧から引違い窓と、はめ殺し窓を選んで配置した。開口と建具の種類・寸法・腰高を確認した。引違い窓（1,690 × 1,170 mm、腰高900 mm）は平面に開き方の記号が出た。はめ殺し窓は600 × 600 mm、腰高1,200 mm。どちらも編集メニューの「元に戻す」1回で開口ごと消えた。人による操作・見え方の確認は未実施。確認画像と数値は配布フォルダの `outputs/window-fitting-check/`。

確認用のSKPとログは配布フォルダの `outputs/skp-roundtrip-review/`。
