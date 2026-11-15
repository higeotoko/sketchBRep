# 引き継ぎと、この先の道筋（2026-11-14）

次に作業する AI（Opus 5.5 など）と、オーナーのための 1 枚。
オーナーの指示：2026-11-14（「重たい作業を重点的に。最悪は Opus 5.5 で触れる所までの道筋を立ててほしい」）。

## 1. 今の状態

| 項目 | 値 |
|---|---|
| ブランチ | `claude/sketchbrep-modeler-spec-4wnp0b`（ここだけで作業。main へのマージ・amend・rebase・force push はしない。PR #1 はマージしない） |
| 最新のコード | `e6bc2fb`（CI 成功 run 43618894525・ZIP 検証済み）。室の札の直し、斜めの角のつながり（docs/107）、斜めの外周の寄棟（docs/108）、符号を手で決める・品番（docs/109）まで入っている |
| 順位表（docs/97） | 1 室・2 屋根・3 IFC・4 壁のつながり・5 階段・6 表と札 まで第 1 段が済。**次は 7** |
| 人の確認 | まだ全部（[docs/105](105_hands_on_checklist.md) のチェック表） |
| 法の数値 | 階段の寸法（docs/103）は、原文との照合がまだ |

続けて push すると、前の CI は途中で取り消しになる（失敗ではない）。**前の CI が終わってから push する**か、最後の push の CI で確かめる。

## 2. 毎回の流れ（これを守れば迷わない）

```
PY=/c/Users/enomo/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe
S=.claude/skills/sketchbrep-dev/scripts/sbr.py
"$PY" -I $S build                         # 全部ビルド
"$PY" -I $S test core [--filter 'Name*']  # 計算テスト
"$PY" -I -X utf8 $S test gui --match '正規表現' --scale1x   # 画面テスト（名前は --match で選ぶ）
```

1. コードを書く → 計算テスト → 画面テスト（関係する物だけ。全体は `--match "."`）。
2. コードだけをコミットして push（メッセージの最後に Co-Authored-By）。
3. `package` → `verify` → `record` → `ci --wait` を 1 本で、裏で回す。回している間はコミットしない。
4. docs/NN を書く（確かめたこと・配布の記録に、CI の run 番号・ZIP の名前・サイズ・SHA256）。docs/97 の進み具合と docs/90 の表も直す。
5. docs だけをコミットして push（docs だけなら CI は走らない）。オーナーに日本語・やさしい言葉で報告。確認は「計算テスト済み／GUI 自動確認済み／Windows 実機確認（人の操作）」に分ける。

### はまった所

- **Bash のヒアドキュメントは `\n`・`\x` を壊す**。パッチは Write ツールで .py を作り、`"$PY" -I -X utf8 x.py` で当てる。バックスラッシュが要る時は `chr(92)`。
- ソースは CRLF と LF が混ざる。パッチは CRLF を LF にして置換し、元に戻す（scratchpad の `sub()` の形）。
- ローカルだけ落ちる画面テスト：`lineToolCrossingLinesMakeFace`（1 倍表示）、クリップボードを使う 2 件。CI では通る。
- 画面の 3D の字体に無い字がある（「㎡」は点になる。「m²」は出る）。**画面に出す文字は、絵で確かめる**。
- QString に通すと先頭の BOM が落ちる。ファイルに書くバイト列は std::string のまま書く。
- `set_levels`（AI）は、`id` を付けないと新しいレベルになり、付いていた部品が離れる。

### 実物のアプリを AI が動かして見る方法（今回できた）

アプリを起動 → 新しい空のファイル → 「AI 連携 > 外部 AI からの操作を許可」。
ZIP の中の `sbr_mcp.exe` に、標準入出力で JSON-RPC（`initialize` → `tools/call`）を送る。`capture_view` で絵が取れる。
これで階段（6 段回り）・床の開口・屋根・表・階高の追従を見て、室の札の字欠けを見つけた。**自動テストは文字列しか見ないので、見え方はこれで見る。**

## 3. 作りの約束（新しい部品を足す時の型）

屋根・階段が手本（`Roof.cpp`・`Stair.cpp`、Modeler.cpp の「roofs」「stairs」の節）。

1. 形の計算は `src/core/src/Xxx.cpp` に、文書を知らない関数として書く（`geom::makeXxx(spec) -> Result<Shape>`）。手計算と比べるテストを書く。
2. 部品は「立体（ImportedShape）＋ 寸法の記録（属性 `xxx_spec` の JSON、部品の座標）＋ `kind`」。形を直接変えたら、寸法の編集だけをやめる（`storedXxxOf` が理由を返す）。
3. Modeler に `createXxx / xxxInfo / changeXxx`。変更は `ChangeSet` に集めて `history_.execute(changes.build(名前))`＝取り消し 1 回。
4. ほかの部品に付いてくる物は「refitなんとか(document, changes, context)」を作り、`transform`・`setLevels`・`commitWallChange`・`remove` の中で呼ぶ（`refitWallsToRoofs`・`refitStairOpenings` が手本）。
5. CommandApi にコマンド、MainWindow にツール・設定・プロパティ、Messages.cpp に日本語、help に説明、IfcExport に種類。
6. 表・札・図面は `doc::Schedules`（文書だけから読む。持たない）。
7. 法の数値・係数は、根拠と確認日を docs に書く。測れない物は「未評価」（合格にしない）。

## 4. この先の道筋（重い順）

**2026-11-15 の時点で済んだ物**：B（斜めの角のつながり → [docs/107](107_slanted_wall_corners.md)）、C のうち寄棟（→ [docs/108](108_hip_roof_on_slanted_outline.md)）、A の 4 番のコア（符号を手で決める・品番 → [docs/109](109_opening_marks_and_products.md)）。
**次にやる順（Opus で可の物から）**：① 符号・品番を画面から入れる欄 → **済（`0e1827b`）** → ② A の 1〜3・5（開口を選ぶ・コピー・記号をつかむ）→ ③ D（階段の平面の記号・手すり）→ ④ F（表をシートに）→ ⑤ G。B の残り（角の留め）と C の残り（斜めの切妻、へこんだ角）と E（法の面積）は、決める事が多い。

「Fable 向き」は、決める事が多くて間違えやすい物。「Opus で可」は、型が決まっていて手本がある物。

### A. 順位 7：開口を「物」として選ぶ・コピー・符号と品番（Fable 向き → 下の設計があれば Opus で可）

今：開口は壁の中の操作対象（id あり）。移動ツールでつかめる（docs/95）。選択は壁ごと。

設計：
1. **選択に「壁の中の開口」を足す**。`MainWindow` の選択を `ObjectId` の集まりから、`{host, openingId}` も持てる形にする（別の入れ物 `selectedOpening_` を 1 つ足すのが安い。複数選択は後）。穴の内側の面をクリック → `Modeler::openingOfFace` で開口が分かる（MoveTool の `openingAt` と同じ）。
2. 選んだ開口：プロパティは今の「開口」の節をその 1 つだけに。Delete → `removeOpenings`。Ctrl+C / V・移動ツール + Ctrl → **コピー**。
3. `Modeler::copyOpening(host, index, worldDelta または 別の壁)`：`cutOpenings` と同じ入口で、同じ幅・高さ・腰高・建具の開口を足す（新しい id。`opening_fittings` も写す）。同じ壁の中は `canMoveOpening` の検査を使う。
4. **符号と品番**：壁の属性 `opening_marks`（JSON：開口の id → `{mark, product}`）。`doc::openingSchedule` は、手で決めた符号があればそれを使い、無ければ今の決まりで付ける。同じ符号なのに大きさが違う時は、表に「大きさが違う」と出す（黙って合わせない）。AI：`set_opening_mark {host, openings, mark, product}`。
5. 平面の記号をつかむ：平面モードで記号の近くをクリック → その開口（`openingSchedule` の `center`・`normal`・`width` で当たりを取る）。

テスト：コピーした開口が表で同じ符号になる／手で付けた符号が開口を足しても変わらない／コピー先が壁の外なら断る／取り消し 1 回。
記録：docs/107。

### B. 直角でない角のつながり（Fable 向き）

docs/102 の「まだ」。`settleWallJoins`・`carryCorners`（Modeler.cpp、docs/96・102）。
今は「相手の動きが自分の線に直角なら運ぶ、沿っていれば伸び縮み」。斜めの角では、運ぶ動きが相手の線に沿う分を持つので、端がずれる。
直し方：端の行き先を「動いた後の 2 本の線（面）の交点」で決める（動きの差ではなく、形から解く）。相手の面の式（点と法線）を動かした後に、自分の線との交点を取り、長さと始点を出す。連鎖は今まで通り 1 段まで。
テスト：45° の角を持つ五角形の部屋で、1 枚を外へ動かす → 全部の角が閉じたまま・室が閉じている。

### C. 屋根：斜めの外周、高さの違う屋根の取り合い（Fable 向き）

今は直角の外周を「最大の長方形」に分けて作る（`Roof.cpp`）。斜めは、直線スケルトン（各辺を同じ速さで内へ寄せた時の棟・谷）が要る。
道筋：寄棟だけ先に（辺ごとに「その辺から上がる斜面の半空間」を作り、全部の共通部分を取る＝凸の外周ならこれで寄棟になる。凹は、今の長方形分けと同じ考えで、凸に分けて和を取る）。OCCT の半空間（`BRepPrimAPI_MakeHalfSpace`）と共通部分で書ける。切妻は「妻にする辺」を選ぶ入力が要る。

### D. 階段の残り（Opus で可）

- 平面の記号（上り矢印・破断線）：`Drawing.cpp` の開口記号の所と同じ場所に、`stairInfo` の `path` と段の線を出す。`geom::makeStair` の `treads[].outline` がそのまま使える。
- 手すり・ささら桁：`StairSpec` に `handrail`（none | inner | outer | both・高さ）を足し、歩く線を幅の半分ずらした折れ線に沿って棒を出す。照合の「手すり」を未評価から外せるのは、高さと有無を持ってから。
- 画面で角ごとに回り方を変える：プロパティの「曲がり角」を角の数だけ並べる（`spec.turns` は、もう角ごとに持てる）。
- 回り段の先を切る（芯から離す）：`Stair.cpp` の `inner` を、内側の角から `offset` だけ外へ。

### E. 法の床面積・建築面積（Fable 向き。数値の根拠が要る）

今は「未評価」。入れるなら：階ごとの「壁で囲まれた面積（壁芯）」から、吹抜け・階段の穴の扱いを決めて引く。建築面積は、外壁の芯（または柱の芯）で囲んだ水平投影＋庇の 1 m を超える分。**算入・不算入の決まりは、条文（法 92 条、令 2 条）を原文で確かめてから**。確かめられない間は、今のまま「未評価」にしておく。

### F. 表をシートに置く・仕上げ表（Opus で可）

`DrawingSheet.cpp` に表のブロックを足す。中身は `doc::openingSchedule` などをそのまま使う。仕上げ（N-02）は、室の目印の属性（`finish_floor` など）に持ち、`roomSchedule` の列に足す。

### G. 順位 8・9（Opus で可）

後から通り芯に付ける（壁の端が通り芯の交点にあれば、その名前を属性に持つ）／階を写す（その階のレベルに付いた部品を、上の階へ複製して付け替える）／3D で 1 階だけ見せる（平面の `storeyBelow` の判定を 3D の表示に使う）／種類の一覧／層の線。

## 5. オーナーにしかできない事

1. [docs/105](105_hands_on_checklist.md) のチェック表（22 項目）。
2. e-Gov で施行令 第 23〜25 条を見る（docs/103 の表）。
3. CSV を Excel で開く。IFC をほかのソフトで開く。
4. A の「符号と品番」で、符号の頭文字（W・D のままか、AW・WD などか）を決める。
