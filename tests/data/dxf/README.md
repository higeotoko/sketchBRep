# DXF 読み取りの相互運用フィクスチャ

`architectural.dxf` は ezdxf で生成した AutoCAD 2018 ASCII DXF。
単位は m。手書きの最小フィクスチャだけに依存しないために使う。

- 日本語レイヤ「壁」の LINE（4 m）。
- bulge を含む閉じた LWPOLYLINE。
- CIRCLE、ARC、ELLIPSE。
- 制御点・ノットを持つ SPLINE。
- 原点をずらしたブロックと、回転を持つ INSERT。

7 要素を欠落なしで取り込み、mm へ換算できることを確認する。
実際の DWG や ODA File Converter の検証用ファイルではない。
