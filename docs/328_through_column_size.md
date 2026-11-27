# 328. 通し柱の丸の大きさ

docs/327 の通し柱の丸が大きすぎた（オーナーの指摘。参考の図では、通し柱の丸は柱の四角にぴったり外接している）。

## 原因

丸の半径を「柱の大きい辺の 0.8 倍と、紙の上で 1.8 mm の大きい方」にしていた。1:100 では 120 角の柱は紙の上で 1.2 mm なので、半径 1.8 mm の丸は柱の倍くらいの大きさになっていた。

## 直したこと

- 丸の半径：柱の切り口の対角の半分 × 1.08。四角の角のすぐ外を通る大きさにした。紙の上の最小の大きさは付けない。
- 凡例の見本：構造材の切り口の × を入れた四角を、すぐ外の丸で囲む形にした（参考の図の「通し柱」と同じ形）。
- 例題の1階平面図と凡例を画像で確かめた（`outputs/review-2026-11-27-columns2/`）。

## 確認

- `Quantities.PlansCircleTheThroughColumns`：丸の半径が 0 より大きく、120 mm 未満（柱の対角の半分程度）。
- 図面・申請の GUI テストが通る。

## 検証と配布

人による操作確認は未実施。

| 項目 | 結果 |
|---|---|
| コード | `ecb3db8` |
| Windows | コア862件成功・8件スキップ、図面・申請の関連GUIテスト成功 |
| Linux CI | [run 45950797750](https://github.com/higeotoko/sketchBRep/actions/runs/45950797750)（`ecb3db8`、883件成功） |
| Windows ZIP | `SketchBRep-windows-x64-ecb3db8-local.zip`（84871703 bytes） |
| SHA256 | `74d27f7b2d16241073f51dc2c1472a702f3f5c3737779bfba26f9dd3d934d5ed` |
| ZIPの検証 | [標準18条件](validation/2026-11-27-portable-ecb3db8.json)・[べた基礎4条件](validation/2026-11-27-portable-ecb3db8-mat.json)成功 |
