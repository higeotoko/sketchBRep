# 271. SketchUpのタグ・戻し込みと建具配置の確認（2026-11-23）

## 13-4 タグと壁の属性

配布版 `2eb59bb` で例題Aのコピーの1階西壁に「1F 外壁」のタグを付け、SKPに書き出した。同梱SDKで読み戻した壁のタグは「1F 外壁」。`SketchBRep` 辞書に由来する `skp_attributes` に `kind=wall`、`wall_type=exterior_siding`、サイディングから石膏ボードまでの5層を確認した。コアテスト `Skp.TagsAndInformationGoAndComeBack` も成功。

## 13-6 編集の戻し込み

コアテスト `Skp.ChangesInSketchUpComeBack` が成功した。壁を455 mm動かすと壁の種類を保って隣の壁が伸び、編集・削除された箱は検出される。置き換え・削除の指定で結果が変わり、動かした壁と箱は取り消し1回で元に戻る。別のSKPからの追加も確認している。例題Aでは2026-11-18にSketchUp本体で作った移動・形状変更・削除・追加を戻した記録がある。 [以前の本体確認](186_sketchup_round_trip.md)

今回のSketchUp本体は起動時にサインインを要求した。13-4のSketchUp上の表示、13-6の本体での編集とSketchBRepの戻し込みダイアログは今回の確認に含まれていない。 [SketchUpのRuby起動機能](https://github.com/SketchUp/sketchup-extension-vscode-project/blob/main/CLAUDE.md)

## 13-8 平面で片開きドアを配置

展開したWindows ZIPのGUIテスト3件が成功。建具一覧から片開きドアを選び、壁をクリックすると開口ができた。マウス位置に応じて開き方の記号が出て、反対側を選べば開く方向も変わった。取り消し1回で開口と建具が戻った。 [建具の配置](189_place_a_fitting_in_plan.md)

配布版 `2eb59bb` をMCPで操作し、平面の建具一覧から引違い窓と、はめ殺し窓を選んで配置した。開口と建具の種類・寸法・腰高を確認した。引違い窓（1,690 × 1,170 mm、腰高900 mm）は平面に開き方の記号が出た。はめ殺し窓は600 × 600 mm、腰高1,200 mm。どちらも編集メニューの「元に戻す」1回で開口ごと消えた。人による操作・見え方の確認は未実施。確認画像と数値は配布フォルダの `outputs/window-fitting-check/`。

確認用のSKPとログは配布フォルダの `outputs/skp-roundtrip-review/`。
