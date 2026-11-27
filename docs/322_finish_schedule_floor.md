# 322. 仕上げ表に床高の列

docs/321 で入れた室の床高（階の FL から）を、仕上げ表の最後の列「床高」にも出す。

- 書き方：`±0`、`-180`、`+100`（mm）。入れていない室は空欄。
- 注記：床高は階の FL から mm、と書き足した。
- 列は最後に足したので、これまでの列の並びは変わらない。
- ゴールデン（docs/249）を更新した。仕上げ表のシートの線と文字が増えた分だけ。

## 確認

- GUI（`schedulesAreReadFromTheModel…`）：室のプロパティに -180 を入れると、仕上げ表の CSV に `,2400,-180` が出る。
- ゴールデン：更新後に通る。

## 検証と配布

人による操作確認は未実施。

| 項目 | 結果 |
|---|---|
| コード | `5885371` |
| Windows | コア860件成功・8件スキップ、関連GUIテスト成功 |
| Linux CI | [run 45926369443](https://github.com/higeotoko/sketchBRep/actions/runs/45926369443)（`5885371`、881件成功） |
| Windows ZIP | `SketchBRep-windows-x64-5885371-local.zip`（84826089 bytes） |
| SHA256 | `f3c5484cee34e120f6483ee9752b2d847cf2902092de31db4223c811dadb048b` |
| ZIPの検証 | [標準18条件](validation/2026-11-27-portable-5885371.json)・[べた基礎4条件](validation/2026-11-27-portable-5885371-mat.json)成功 |
