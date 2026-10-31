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

手書きの小さな面生成確認用 DXF（単位mm、UTF-8）:

- `small_open_rectangle.dxf`: 2000×1000の矩形の3辺。最後の1辺を描いて閉じる。
- `small_closed_lines.dxf`: 同じ矩形を4本のLINEで構成。
- `small_closed_polyline.dxf`: 同じ矩形を閉じたLWPOLYLINEで構成。
- `small_arc_outline.dxf`: 半径1000の半円弧と直径の直線。
- `small_layer_with_extra_line.dxf`: 矩形と離れた線を同じレイヤに格納。輪郭の4辺だけを選ぶ確認用。
