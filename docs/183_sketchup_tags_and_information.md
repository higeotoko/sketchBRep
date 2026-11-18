# SketchUp に物の情報とタグを持っていく（2026-11-18）

[docs/181](181_sketchup_skp.md)（SketchUp の読み書き）の続き。連携の案の 1（BIM の情報）と 2 のうちタグ。作業は Opus 5.5。
オーナーの指示：2026-11-18（「おすすめ順で」）。

## できること

| 向き | 内容 |
|---|---|
| 書き出し | 物ごとのグループに、**属性の辞書「SketchBRep」**：物の属性ぜんぶ（壁の仕様 `wall_type`・層 `wall_layers`、建具・室などの情報）と、物の id（`sbr_id`）。文字の値で入る。SketchUp の「エンティティ情報」や Ruby（`group.get_attribute("SketchBRep", "wall_type")`）で読める |
| 書き出し | 物のタグ（レイヤ）を、同じ名前の **SketchUp のタグ**に（「Default」は SketchUp の既定のタグのまま） |
| 読み込み | グループ・コンポーネントのタグを物のタグに（SketchUp の既定のタグ「Layer0 / Untagged」は「Default」） |
| 読み込み | 辞書「SketchBRep」があれば、中身を物の属性 `skp_attributes`（JSON）に**そのまま**残し、書き出した元の物の id を `skp_source_id` に |

## 決めたこと

| 項目 | こうした | 理由 |
|---|---|---|
| 読み込んだ情報の置き場 | 物の属性に戻さず、`skp_attributes` にまとめて残す | 読み込んだ物は形（メッシュから作った立体）で、壁ツールの壁・屋根などではない。`kind = wall` などを戻すと、表・数量・つながりが壁として読もうとして合わなくなる。情報は落とさずに残す |
| 元の id | `skp_source_id` | 次の「SketchUp で直した物を戻す」（連携の案 5）で、元の物を見つけるため |
| COLLADA | 今まで通り（タグ・情報は書かない） | DAE には決まった置き場が無い |

## まだやっていないこと

- IFC の分類（SketchUp の「分類」）を付けること（SketchUp にスキーマを読み込む作りが要る）。
- ほかのソフトが付けた辞書（「SketchBRep」以外）を読むこと。
- コンポーネントを共有して書き出すこと（連携の案 2 の残り）、LayOut（案 4）。docs/184 の「次にやる事」。

## 確かめたこと

| 段階 | 内容 |
|---|---|
| 計算テスト済み | SKP・DAE のテスト 10 件中 9 件成功・1 件スキップ（ローカル、SDK あり）。新規 1 件（`Skp.TagsAndInformationGoAndComeBack`）：外壁 サイディングの壁（タグ「1F 外壁」）と箱を書き出して読み戻す → どちらも `skp_source_id` が元の id、壁はタグ「1F 外壁」、箱は「Default」、壁の `skp_attributes` に `wall_type = exterior_siding`・`kind = wall`・層の名前（窯業系サイディング）、物の属性には `kind`・`wall_type` を戻していない／もう一度書き出して読み戻してもタグが同じ |
| CI | （記録待ち） |
| Windows 実機確認 | まだ（チェック表 13-4） |

## 配布の記録

（記録待ち）
