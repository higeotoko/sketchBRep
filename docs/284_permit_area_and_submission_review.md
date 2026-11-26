# 確認申請の面積欄と横浜市の提出図書

## 面積を転記する

「計画概要の項目」の「建築物」タブに、両方の率の算定に使う敷地面積、建蔽率用の建築面積、容積率用の延べ面積、確認資料を入れる。「確認申請のWord様式」では算定の分子・分母・百分率と出典が見える。

- 敷地全体に申請建築物だけがある単一棟の新築を基準とする。登録した敷地面積と、申請用に確認した敷地面積を帳票の表示桁で照合する。入力した算定基礎面積はモデルの面積と照合し、算定式に使う値は丸める前の入力値とする。単一棟の転記を選択した場合に両様式の算定面積・百分率を埋める。
- 算定基礎面積と総面積に差がある場合は、控除等の根拠・内訳を様式に記入する。同一敷地に既存棟・別棟がある場合、複数用途地域、法第52条第12項等で容積率と建蔽率の分母が異なる場合、控除の明細・上限の設定は個別に確認する。割合の算出は法令適合の判定と別。 [建築基準法第52・53条](https://laws.e-gov.go.jp/law/325AC0000000201)
- 第二号様式第四面には、地上・地下の申請階数とモデルの階名が対応し、面積の各階合計が一致した場合に、最上階から最大6階の床面積を転記する。第五面は各階の用途別・構造寸法等の明細と合わせて確認する。

## 提出図書の照合

敷地メニューの「横浜市の提出図書を照合」、または申請工程1から開く。横浜市へ直接申請する木造2階建て新築戸建てを基準に、意匠・設備と構造の受付チェックシート、手数料算定表、建築協定等手続状況届出書などを表示する。市のページから原本を取得できる。 [横浜市の受付書式](https://www.city.yokohama.lg.jp/business/bunyabetsu/kenchiku/tetsuduki/kakunin/dl/dl.html) · [横浜市の確認申請手続き](https://www.city.yokohama.lg.jp/business/bunyabetsu/kenchiku/tetsuduki/kakunin/tetsuzuki.html)

- 「モデルの図面を確認」は、その場で標準の図面一式を作って図面名の候補を照合する。付近見取図は位置登録から得られる候補を示す。
- 「提出フォルダを選ぶ」は選択した階層のファイル名から候補を探す。一式PDFのページ構成、記入内容、最新版と案件の適用条件は別途照合する。フォルダ内の書類は読み取りも保存も行わず、ファイル名だけを表示する。

## 手数料算定表との照合

提出図書の照合画面で、単一棟の新築の申請部分に全床面積を含めると選択すると、確認申請書第三面11欄イと同じ延べ面積を「確認申請手数料算定表」の行に候補表示する。延べ面積を確定できない場合は、確認が必要なことを表示する。金額、加算、様式3への記入は市の書式と手数料表で照合する。 [横浜市の受付書式](https://www.city.yokohama.lg.jp/business/bunyabetsu/kenchiku/tetsuduki/kakunin/dl/dl.html) · [横浜市の手数料表](https://www.city.yokohama.lg.jp/business/bunyabetsu/kenchiku/tetsuduki/kakunin/tesuryoudir/tesuryou.html)

手数料欄追加のコードは `50bfcb0`。Windowsコア833件成功、8件スキップ。照合画面とWord転記のGUIテスト2件が成功。配布ZIP `SketchBRep-windows-x64-50bfcb0-local.zip` は84,172,897 bytes、SHA256 `9eb526217bca463abd129bf0690858a1bc0e8292a00e6f1baf20a059b249e0d8`。 [標準18条件](validation/2026-11-26-portable-50bfcb0.json)・[べた基礎4条件](validation/2026-11-26-portable-50bfcb0-mat.json)成功。 [Linux CI](https://github.com/higeotoko/sketchBRep/actions/runs/45730963017)

## 検証と配布

- Windowsのコアは833件成功、8件スキップ。Windows GUIの3条件が成功。配布ZIPの展開先のDLL・プラグインでも同じ3条件が成功した。申請書2種への転記、敷地面積・階数の不一致で値を入れないこと、確認申請書と概要書を別々に探すこと、モデルで見つかる図面数を確認した。
- Wordで原本と同じ用紙・ページ数（第二号16、第三号6）だった。架空敷地120㎡で建蔽率34.58%、容積率66.67%、第四面に2階42㎡、1階42㎡、合計84㎡が対応欄に入った。人による操作検証は未実施。画面と紙面の記録は配布フォルダの `outputs/permit-area-review-2026-11-26/` にある。

| 項目 | 結果 |
|---|---|
| コード | `b577fed` |
| Linux CI | [run 45729406704](https://github.com/higeotoko/sketchBRep/actions/runs/45729406704) で854件成功 |
| Windows ZIP | `SketchBRep-windows-x64-b577fed-local.zip`（84,168,833 bytes） |
| SHA256 | `a74dd4c1782195db0774cab09e6ea6dadc7300210c593d3ee72d75803dcd51dd` |
| ZIPの検証 | [標準18条件](validation/2026-11-26-portable-b577fed.json)・[べた基礎4条件](validation/2026-11-26-portable-b577fed-mat.json)成功 |
