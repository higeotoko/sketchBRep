# SketchUp に物の情報・タグ・コンポーネントを持っていく（2026-11-18）

[docs/181](181_sketchup_skp.md)（SketchUp の読み書き）の続き。連携の案の 1（BIM の情報）と 2（タグ・コンポーネント）。作業は Opus 5.5。
オーナーの指示：2026-11-18（「おすすめ順で」）。

## できること

| 向き | 内容 |
|---|---|
| 書き出し | 物ごとのグループに、**属性の辞書「SketchBRep」**：物の属性ぜんぶ（壁の仕様 `wall_type`・層 `wall_layers`、建具・室などの情報）と、物の id（`sbr_id`）。文字の値で入る。SketchUp の「エンティティ情報」や Ruby（`group.get_attribute("SketchBRep", "wall_type")`）で読める |
| 書き出し | 物のタグ（レイヤ）を、同じ名前の **SketchUp のタグ**に（「Default」は SketchUp の既定のタグのまま） |
| 読み込み | グループ・コンポーネントのタグを物のタグに（SketchUp の既定のタグ「Layer0 / Untagged」は「Default」） |
| 読み込み | 辞書「SketchBRep」があれば、中身を物の属性 `skp_attributes`（JSON）に**そのまま**残し、書き出した元の物の id を `skp_source_id` に |
| 書き出し | SketchBRep のコンポーネント（形を変えていない置き方）は、**SketchUp のコンポーネント**：定義 1 つ（定義の名前）に形を 1 回だけ書き、置いた所ごとにインスタンス（物の名前・色・タグ・情報）。定義の面に付けた色も共有する。置き方ごとに形や面の色を変えた物（パラメータ・反転・伸ばし・配置ごとの面色）はグループ |
| 読み込み | 入れ子がないコンポーネントは、同じ SketchUp 定義と引き継ぐ色ごとに SketchBRep の定義を共有する。面ごとの色は定義に保存し、配置ごとの位置・回転・名前・タグ・元の ID も取り込む |

## 決めたこと

| 項目 | こうした | 理由 |
|---|---|---|
| 読み込んだ情報の置き場 | 物の属性に戻さず、`skp_attributes` にまとめて残す | 読み込んだ物は形（メッシュから作った立体）で、壁ツールの壁・屋根などではない。`kind = wall` などを戻すと、表・数量・つながりが壁として読もうとして合わなくなる。情報は落とさずに残す |
| 元の id | `skp_source_id` | 次の「SketchUp で直した物を戻す」（連携の案 5）で、元の物を見つけるため |
| COLLADA | 今まで通り（タグ・情報は書かない） | DAE には決まった置き場が無い |

## まだやっていないこと

- IFC の分類（SketchUp の「分類」）を付けること（SketchUp にスキーマを読み込む作りが要る）。
- ほかのソフトが付けた辞書（「SketchBRep」以外）を読むこと。
- 入れ子・拡大縮小・鏡像・面と独立線が混在するコンポーネントの共有。これらは配置ごとの形として読み込む。
- LayOut（案 4）。docs/184 の「次にやる事」。

## 確かめたこと

| 段階 | 内容 |
|---|---|
| 計算テスト済み | SKP・DAE のテスト 11 件中 10 件成功・1 件スキップ（ローカル、SDK あり）。新規 1 件（`Skp.TagsAndInformationGoAndComeBack`）：外壁 サイディングの壁（タグ「1F 外壁」）と箱を書き出して読み戻す → どちらも `skp_source_id` が元の id、壁はタグ「1F 外壁」、箱は「Default」、壁の `skp_attributes` に `wall_type = exterior_siding`・`kind = wall`・層の名前（窯業系サイディング）、物の属性には `kind`・`wall_type` を戻していない／もう一度書き出して読み戻してもタグが同じ。新規 1 件（`Skp.ComponentsAreShared`）：柱（105 × 105 × 2700）をコンポーネント「柱 105」にして 910 おきに 4 本 → 書き出すと SketchUp のインスタンス 4・グループ 0、読み戻すと 4 本とも体積ぴったり、x 0・910・1820・2730、名前は「柱」から。コア全 756 件中 748 件成功・8 件スキップ |
| CI | run 44278615114（push、`54e5c3e`）：Linux 773 件成功、Windows 769 件＋3 件成功・GUI 238 件成功（7 件スキップ）。CI には SketchUp SDK が無いので、SKP・LayOut のテストはスキップ |
| Windows 実機確認 | まだ（チェック表 13-4） |

## 配布の記録

| 項目 | 値 |
|---|---|
| コード | `1597527`（タグと物の情報）・`84ad3df`（コンポーネント）。Opus 5.5 で作業。まとめて配ったのは `54e5c3e` |
| CI | run 44278615114（push、`54e5c3e`）：Linux・Windows とも成功 |
| ZIP | `SketchBRep-windows-x64-54e5c3e-local.zip`（Codex のチャットフォルダの `outputs\`）。SketchUp SDK の DLL 2 つ入り（LayOut は入れていない） |
| サイズ | 81,771,001 bytes |
| SHA256 | `e5cd445e41f9fc7169e1b5fedc60f1ae6c4b0afd431e38ee8806a56cc92ae56a` |
| ZIP の検証 | `verify_portable_pads.py`・`verify_portable_mat.py` とも成功（docs/validation/2026-11-18-portable-54e5c3e*.json） |

### コンポーネントの読み戻し（2026-11-19）

- コア 752 件成功・8 件スキップ。SDK ありの環境で SKP・DAE 関連 11 件成功・1 件スキップ。柱4本が一つの定義を共有し、位置・回転・元の ID と、取り消し・やり直し、SBR の保存・再読込を確認した。
- 関連 GUI 6 件成功。Windows ZIP の通常・べた基礎の検証も成功。展開した ZIP の CLI と同梱 SDK で、SKP から読み込んだ4本が SBR を開き直した後も一つの定義を共有した。
- Windows ZIP `SketchBRep-windows-x64-a6bf3e9-local.zip`、81,810,822 bytes、SHA256 `09a7bd581d654cf87a124ec5e3d497aa1a7ed7536506be061b22c11c07e31fb7`。記録は `docs/validation/2026-11-19-portable-a6bf3e9*.json`。人の操作による Windows 実機確認は未実施。
- GitHub Actions は [run 44446849081](https://github.com/higeotoko/sketchBRep/actions/runs/44446849081) で Linux 773 件成功。SKP は同梱 SDK を使って Windows で確認した。
