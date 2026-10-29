# 06. AI 連携（バイパス）の設計と活用方針

仕様書の「AI からの操作でも同じコマンド基盤を使用」を、**今の段階で配線まで通した**もの。
AI 専用の裏口は作らず、GUI のツールと同じ `doc::Modeler` を JSON で呼べるようにしただけ、というのが基本方針。

## 全体像

```
 Claude Desktop / Claude Code / 自作エージェント（MCP クライアント）
        │  MCP (JSON-RPC 2.0, stdio)
        ▼
  sbr_mcp ──────────────┐  --headless のときはアプリなしでメモリ上のモデルを操作
        │ ローカルソケット │
        │（Windows: 名前付きパイプ。同じユーザーのみ）
        ▼                ▼
  SketchBRep アプリ   api::CommandApi  ←── sbr_cli（JSON Lines スクリプト / --mcp）
  AiBridge（既定 OFF）      │
        └────────────►  doc::Modeler  ← GUI のツールもここを呼ぶ
                            │  検証・失敗時ロールバック・Undo/Redo
                         Geometry / Document 層（OCCT B-rep）
```

| 部品 | 場所 | 役割 |
|---|---|---|
| `api::CommandApi` | `src/core/.../api/CommandApi.h` | `{"command", "args"}` → `{"ok", "result" \| "error"}`。一覧は `schemaJson()` から出力。Qt 非依存 |
| `api::McpServer` | `src/core/.../api/McpServer.h` | MCP の `initialize` / `tools/list` / `tools/call` / `ping` を CommandApi に変換。通信路に依存しない |
| `sbr_cli` | `tools/cli` | ヘッドレス実行。JSON Lines のスクリプト、`--schema`、`--mcp`（ヘッドレス MCP） |
| `sbr_mcp` | `tools/mcp` | MCP サーバー。起動中のアプリへ転送（`--headless` でアプリなし） |
| `AiBridge` | `src/app/AiBridge.*` | アプリ内の共通入口。ローカルパイプと任意のChatGPT用HTTPから同じモデルを操作。「外部AIからの操作を許可」でON |
| `McpHttpServer` | `src/app/McpHttpServer.*` | リモートMCP用のStreamable HTTP。ループバック待受＋秘密URL。利用者が用意するHTTPSトンネルを経由 |

ChatGPTの経路は `ChatGPT → 利用者のHTTPSトンネル → McpHttpServer → AiBridge → doc::Modeler`。上記のローカル接続と同じ検証・Undoを使う。

## コマンド（ツール）一覧

`sbr_cli --schema` で JSON Schema 付きの一覧が出る（そのまま LLM の tool 定義になる）。

- 調べる: `list_objects`, `get_object`（面の番号・法線・重心・面積、パラメータ一覧つき）
- 大きいモデルを読む: `query_objects`（種類・タグ・名前・親・グループ配下・部品定義・可視・範囲で絞り込み、`fields` で返す項目を指定、`limit` と `next_cursor` でページ分け）, `aggregate`（件数・体積・面積の合計、`group_by` で種類・タグ・定義ごと）, `get_changes`（`sequence` 以降に変わった／消えた ID。`full: true` なら読み直し）。体積・面積・外接箱は形状ごとにキャッシュし、同じ部品の配置では 1 回だけ計算
- 作る: `create_line`, `face_from_loop`（線・円弧で囲んだ輪郭を面に）, `create_rectangle`, `create_circle`, `create_regular_polygon`（正多角形。半径・辺の数を後から変更可）, `create_arc`（正確な円弧・弓形の面）, `create_polygon`, `create_curve`, `create_box`
- 注記: `create_dimension`, `create_text`, `set_text`（寸法・部屋名や面積のラベル。`get_object` で文字も読める）
- 補助: `create_guide`（点と方向で無限長のガイド線。作図の吸着先、出力対象外）, `clear_guides`（通り芯は残す）, `create_grid`（通り芯 X1.. / Y1.. を符号・スパン寸法つきで一括作成。Undo 1 回。結果の `lines` で「X3」のように名前から ID を引ける）
- 編集: `split_face`（面を線で分割）, `extrude`, `push_pull`（面は番号か `{"normal": [0,0,1]}`）, `set_parameter`, `offset`, `move`, `rotate`, `scale`（等倍・パラメトリック）, `copy_array`（直線・円形）, `boolean`, `revolve`, `loft`, `sweep`
- 構造・属性: `group`, `ungroup`, `make_component`, `set_definition_parameter`, `rename`, `set_tag`, `paint`, `delete`
- コンポーネントの配置: `placement_parameters`（読み取り）, `set_placement_parameter`, `clear_placement_parameters`, `make_unique`, `mirror_placement {ids, axis}`, `set_component_axes {id, origin, x_axis?}`（docs/14_components.md）
- 形の変形: `move_vertices {id, points | edge, delta}`（面を平らに保てない動きは拒否、docs/15_reshape.md）
- 線の非表示: `set_edges_hidden {id, edges, hidden}`, `show_hidden_edges {ids}`（表示だけ、面は分かれたまま）
- 消しゴム（線）: `erase_line {id, edge}`（区間だけ消す / 面の外周なら面を消して線を残す / ソリッドの角なら 2 面を消して開いた形に）
- 表示状態: `hide` / `unhide_all`（非表示は描画・吸着・出力の対象外）, `set_section`（断面。平面図・断面図の切断）, `list_tags`, `set_tag_visible`
- 3DM: `export_3dm` / `import_3dm`（ファイル系なのでアプリ経由では無効、`sbr_cli` 等で使用）
- 履歴・ファイル: `undo`, `redo`, `save`, `open`, `export_step`（**アプリ経由では無効**）
- STL: `export_stl {path, ids?, linear_deflection_mm?}`（バイナリSTL・mm、近似精度の既定0.1 mm／最小0.001 mm）。ファイル系のためアプリ経由・preview・原子的batchでは実行しない。表示中の面／立体のみが対象で、線・マテリアルは含めない
- BREP / IGES: `import_brep {path}` / `import_iges {path}` / `export_brep {path, ids?}` / `export_iges {path, ids?}`。曲線・面・立体の形状のみ、mm基準。4つともファイルI/Oのためアプリ経由・preview・原子的batchでは実行しない。詳細は [形式追加の説明](30_geometry_exchange_formats.md)。
- アプリ起動中のみ: `get_selection`（ユーザーが選んだ物・面）, `select`, `set_view`, `capture_view`（ビューの PNG を AI に画像で返す）, `list_scenes`, `go_to_scene`（保存ビューの表示・保存）
- 意味の属性: `set_attribute`（`kind=wall`、`level=2F` などのキーと値。複数対象を Undo 1 回で）。`query_objects` / `edit_each` の `attributes` 条件（`"*"` はキーがあれば何でも）と、`aggregate` の `group_by: "attribute:level"` で使える。形状には影響せず、名前や AI の推測を事実扱いしないための「明示的な意味」
- 消しゴム: `erase_edge {id, edge: 番号 か {near: [x,y,z]}}`（split_face や線で分けた面の境の線を消して一枚に戻す。形は変わらない。角の辺は拒否）
- 開いた形状: `delete_faces {id, faces}`（面だけ消す → kind Shell）, `fill_holes {ids}`（平らな穴をふさいでソリッドに）, `reverse_faces {ids}`（開いた形状・単独の面の表裏）
- ロック: `set_locked {ids, locked}`（ロック中は移動・尺度・削除を拒否。画像の下敷きは kind `Image`、読み込みはファイル操作なので GUI から）
- 素材: `list_materials`, `create_material`, `update_material`, `delete_material`, `assign_material`（部材全体、"" で継承）, `paint_faces`（面ごと）, `clear_face_materials`, `material_at`（面の素材と由来＝スポイト）。`query_objects` / `edit_each` の `material` 条件、`aggregate` の `group_by: "material"`。「この面と同じ素材を壁全部に」は material_at → edit_each（preview → batch、1 Undo）。詳細は docs/11_materials.md
- 建築の要素: `create_wall`（点列 `[x, y]` か通り芯の交点 `"X1/Y1"` を結ぶ壁。厚さ・高さ・芯の位置 center/left/right。角は突き付け（直角で正確）、長さ・厚さ・高さはパラメータで後から変更可、`kind=wall`）, `create_slab`（多角形か通り芯の範囲＋はね出しで床スラブ、天端と厚さ、`kind=slab`）, `place_openings`（壁の面に同じ開口を等間隔、または中心間隔 `spacing` で中央振り分け、`at` で指定位置に 1 つ。既にある開口と重なる配置・大きさ変更は断る（穴がつながるのを防ぐ）。`list_openings` は各開口の面の向き `normal` も返す。手で描いて押し抜くのと同じ処理、Undo 1 回）
- 部材（[docs/40](40_members_stage_a.md)）: `list_sections`（鋼材カタログ1,204断面の検索。完全一致 `exact` と部分一致 `matches` を分け、種類 `family`・ページ `limit/offset`。無い規格を別サイズへ置き換えない）, `create_member`（カタログIDか長方形・H形の寸法で、2点間か始点＋長さ・方向に部材を作る。配置基準 anchor、断面の向き up・回転 roll、任意の用途 `role`→`kind`、材料種別 `material_class`）, `member_info`（今の編集項目・値・根拠。加工後の長さは形状から測った値を返し、生成時の値を現在値として返さない）, `change_member`（カタログ交換・独自寸法・直径・絶対長さ・回転・基準を1操作でまとめて変更。壁は厚さ（描いた線を保つ）・高さ・長さ、スラブは厚さ（既定で上面を保つ）。加工が再適用できなければ何も変えずに理由を返す。同じIDのまま）
- 追従（[docs/42](42_follow.md)）: `set_follow`（`ids` を `host` に追従させる。`host: null` で解除。頼まれた物だけ、位置と向きだけ。手で動かしても解除しない。自分自身・包含関係・循環は断る）。`get_object` は `follow: {host, state}` を返す
  - 同じ操作は GUI からも使える: 壁ツール（`W`）、スラブツール（`Shift+W`）、開口ツール（`O`）、ツール > 開口を等間隔に配置 / 階を割り当て / 整列、プロパティパネルの開口サイズ。AI が作った壁を人が手で直す・その逆が同じ Modeler 経由で混ざっても Undo の順序は 1 本
- 建築の意図: `list_openings`（壁などの開口一覧：番号・幅・高さ・中心・窓台の高さ）, `set_opening_size`（開口の幅・高さを変更。既定は横の中心と窓台を保ち、`keep: "center"` で中心を保つ。パラメトリックのまま。全部の壁なら `edit_each` で `host: "$id"`）, `assign_levels`（`levels: [{name, elevation}]` から各オブジェクトの一番下の高さで `level` 属性を付ける。どの階より下の物は返すだけで変えない）, `align`（外接箱の min/max/center を x/y/z 方向で値か最初の物にそろえる）
- まとめて編集: `edit_each`（`where` に query_objects と同じ条件、`commands` の中の `"$id"` が各対象・`"$index"` が順番。`expect_count` で「何個のつもりか」を渡すと、ずれていたら何も変えずに拒否。全体で Undo 1 回、1 件でも失敗したら全部戻す。preview / propose の中でも使える）, `place_on_grid`（柱などを通り芯 X*/Y* の交点にコピー。`x`・`y`・`skip` で範囲指定、同じ名前の通り芯が 2 本あれば曖昧として拒否）
- `preview`: コマンド列を一時コピーで試して差分（追加・変更・削除）を返す。本体は無変更。返された `revision` を `batch` の `expected_revision` に渡すと、その間にモデルが変わっていた場合は適用を拒否
- アプリ起動中のみ `propose` / `proposal_status`: 提案を半透明で表示し、ユーザーが「適用／却下」を選ぶ（AI が勝手に確定しない）
- `batch`: 複数コマンドを **Undo 1 回分**として実行。既定で原子的（途中で失敗したら全部元に戻す）。`"$1.id"` で 1 番目の結果を後のステップから参照できる

```jsonc
{"command": "batch", "args": {"name": "柱を並べる", "commands": [
  {"command": "create_box", "args": {"origin": [0,0,200], "width": 400, "depth": 400, "height": 3000}},
  {"command": "copy_array", "args": {"ids": ["$1.id"], "vector": [3900,0,0], "count": 3}},
  {"command": "paint",      "args": {"ids": ["$1.id"], "color": "#a0a0a0"}}
]}}
```

単位は mm・度、Z が上。数値はすべて CAD の正確な値で扱い、表示メッシュは使わない。

## 効果分類と変更セット（設計レビュー A/B/C 対応）

- **効果分類**: 各コマンドは `read`（読むだけ。`preview` も一時コピーなので read）/ `view_write`（視点・選択・提案表示）/ `model_write`（モデルと永続設定）/ `external_io`（ファイル）に分類され、`describe` の各要素の `effect` と MCP の `annotations.readOnlyHint` で分かる。`batch` は手順の中で一番強い効果。不明なコマンドは `model_write` 扱い
- **ファイルは試さない・巻き戻さない**: `preview` と `batch`（`atomic`、既定）は、`external_io` の手順（save / open / import_* / export_* / set_material_texture など）を含むと**何も実行せずに**拒否する。一時コピーやロールバックではファイルの読み書きを取り消せないため。ファイル操作は単独で、または `atomic: false` の batch で行う（review 2026-10-23 P0-02）
- **3DM の部分読込は明示**: `import_3dm` はメッシュ・点・曲線・変換失敗・面やトリムの欠落があると、何が欠けるかを並べて拒否する（何も読まない）。`allow_partial: true` で読めた分だけ読み込み、結果の `report`（objects / imported / failed / partial / skipped_faces / skipped_trims / unsupported / units / complete）で内訳が分かる（P0-03）
- **長い演算の途中には書かない**: アプリで長い形状計算（進捗ダイアログ）が走っている間、AI の要求はソケットで順番待ちになり、終わってから順に実行される。直接呼び出しの書き込みは `busy` で拒否、自動保存も終わってから（P0-04）
- **作図を消さない**: アプリ側は `model_write` / `external_io` のときだけ作図途中のツールを中断する。読み取り・`preview`・視点変更では、ユーザーが置いた始点や入力中の数値はそのまま
- **設定も原子的**: タグの表示・断面・シーンは Undo 履歴に載る（1 操作 = 1 Undo）。`batch` が途中で失敗すると、形状と一緒にこれらも実行前に戻り、未保存フラグ・Undo 数・`revision` も変わらない
- **変更セット**: `batch` の結果の `changes` と `preview` の `changes` は、実行で記録された Undo 1 手分の差分そのもの（`added` / `removed` / `modified[{id, aspects}]` / `definitions` / `settings`）。aspects は `geometry` / `placement`（回転を含む全行列）/ `visibility` / `attributes`（名前・タグ・色）/ `hierarchy`。提案（`propose`）のゴーストと件数も同じ変更セットから作り、適用時に結果と照合する

### 太い編集の流れ（例：「2 階の窓を全部青く、1 つずつ 10 mm 上げて」。`level=2F`・`kind=window` の属性があれば `attributes` で一発）

1. `query_objects`（`name` / `tag` / `within` / `region` など）で対象を探し、`total` で件数を確認（必要なら `select` でユーザーに見せる）
2. `preview` に `edit_each`（`where` と同じ条件、`expect_count` = 件数）を入れて、変更セットを確認。アプリなら `propose` でゴースト表示
3. 同じ内容を `batch`（`expected_revision` つき）で確定 → Undo 1 回で全部戻せる。人は直後に GUI で数値を直せる

## 使い方

### リモートMCP接続（サービス側の利用条件あり）

「AI連携 > リモートMCP接続...」でHTTP接続口を開始し、必要なHTTPS到達経路のURLからMCPクライアント用接続URLを生成する。MCP登録・読み取り・モデル作成／編集の権限は別条件。ChatGPTのプラン別条件とPlusのWindowsデスクトップでの実測、HTTPSと認証の違いは接続マニュアルを参照する。トンネルはサービス側の利用制限を解除しない。[手順・権限・検証範囲](33_chatgpt_connection.md)を参照。

### 1. ローカルMCPクライアント（Claude Desktop / Claude Code / Codex等）から操作する

同一PCからのローカルMCPは通常HTTPSトンネル不要。クライアントは利用者が選び、特定のAIモデルや有料AI API、OpenAI APIキーを必須としない。ローカルLLMはモデルとクライアント双方のツール呼び出し対応が必要。「MCP 接続設定をコピー」のJSONが使えるかはクライアントの設定形式によるため、各製品の登録手順に従う。Codex等の開発環境で接続できることは、ChatGPT通常チャットでの編集権限を意味しない。

1. SketchBRep を起動 → メニュー「AI 連携 > 外部 AI からの操作を許可」を ON（ステータスバーに「AI 連携 ON」）
2. 「AI 連携 > MCP 接続設定をコピー」→ Claude Desktop の設定（`claude_desktop_config.json`）に貼り付け
   ```json
   { "mcpServers": { "sketchbrep": { "command": "C:\\SketchBRep\\sbr_mcp.exe" } } }
   ```
   Claude Code なら `claude mcp add sketchbrep -- C:\SketchBRep\sbr_mcp.exe`
3. 「選択している壁に幅 900・高さ 2100 のドアを開けて」などと頼む。AI の操作は画面にすぐ反映され、**Ctrl+Z で 1 手ずつ戻せる**

### 2. アプリなしでスクリプト実行（バッチ生成・CI・検証）
```bash
sbr_cli --save out.sbr tests/data/columns.jsonl     # 1 行 1 コマンド。失敗があれば終了コード 1
sbr_cli --open out.sbr --mcp                        # ヘッドレス MCP サーバー
```

## 安全設計

| リスク | 対策 |
|---|---|
| AI が壊れた形状を作る | GUI と同じ `Modeler` 経由なので、B-rep 検証・失敗時に一切変更しない・Undo がそのまま効く |
| 途中まで実行して中途半端になる | `batch` は原子的（失敗したら全ステップを戻す）。成功しても Undo 1 回で全部戻る |
| 知らないうちに外部から操作される | アプリ側は **既定 OFF**・明示的にON。通常のローカルソケットは同じユーザーのみ（`UserAccessOption`）。リモートMCP用HTTPは別途開始し、ループバック待受・秘密URL・Host/Origin検証を使う。HTTPSトンネルは利用者が用意する。OFFで両方停止。詳細は33章 |
| ファイルの上書き・消失 | アプリ経由では `save` / `open` / `export_step` を無効化。保存はユーザーが行う |
| 作図中に割り込まれる | AI のコマンド実行前に、作図途中のツール操作をキャンセルしてから実行 |
| 面の番号がずれる | 番号は `get_object` の結果に対応（表示・選択と同じ順序）。`{"normal": [...]}` 指定なら番号に依存しない。押し出しでトポロジーが変わったら `get_object` で取り直す方針を `instructions` で AI に伝えている |

## 活用方法（建築設計での使いどころ）

1. **言葉でたたき台**: 「8m×12m、階高 3m の 2 階建てのボリュームに、3.6m スパンで柱」→ 寸法はパラメータのまま残るので、後から GUI で直接いじれる
2. **選択 + 指示の合わせ技**: ユーザーが面を選んで「ここに 900×2100 のドア」「この壁を 200 厚く」。`get_selection` で選択を読み、`push_pull` / `set_parameter` を使う。直感操作は GUI、数の多い・面倒な操作は AI という分担
3. **繰り返し・パターン**: 柱割り、窓の等間隔配置、ルーバー、階段の段（`batch` + `copy_array`）。コンポーネント化すれば、定義を変えるだけで全部更新
4. **チェックと説明**: `list_objects` の体積・面積・外接箱から床面積・壁量・概算数量を集計、寸法の食い違い（柱の通り芯ずれなど）の指摘、モデル内容の説明文作成
5. **見て直す**: `capture_view` でビュー画像を AI が見て、見た目の確認や「ここが変」の指摘 → 修正
6. **自動化・連携**: `sbr_cli` で敷地条件 → ボリューム案を量産し STEP 出力、CI での回帰テスト、他ツール（表計算・BIM）とのつなぎ

## 今後の拡張（ロードマップ）

| 段階 | 内容 |
|---|---|
| 次 | **アプリ内 AI パネル**: Claude API の tool use で同じスキーマ（`schemaJson()`）をそのまま渡すチャット欄。API キーはユーザー設定、モデルは最新の Claude を既定に |
| 次 | **提案プレビュー**: AI の `batch` をまず半透明のゴーストで表示し、「適用 / 却下」を選べるモード（Undo に頼らない確認フロー） |
| 済 | 寸法線・通り芯のコマンド化（`create_grid` の `lines` で「通り芯 X3」を ID に引ける）。レベル（階高）は次 |
| 中期 | 面・エッジの **意味的な名前**（「上面」「押出 2 の側面 3」）。TNP 強化と合わせて、番号より壊れにくい参照に |
| 中期 | マクロ記録: GUI 操作を JSON Lines に記録 → AI が一般化して再利用 |
| 中期 | MCP の resources（モデル概要・選択状態の購読）と、変更通知 |

## テスト

| テスト | 内容 |
|---|---|
| `Transactions.*` | 失敗した batch で設定・未保存フラグ・Undo 数・revision が戻る、設定変更が 1 Undo、preview と commit の変更セット一致（回転・非表示・塗り・名前・グループ・パラメータ・定義）、効果分類 |
| `CommandApi.*`（7 件） | 箱 → 面の押し出し → パラメータ再編集 → Undo、エラー時に何も変えない、`batch` が Undo 1 回・`$N` 参照、失敗した `batch` の全ロールバック、壁に描いたドアの押し込み、スキーマ、拡張コマンドとファイル系の無効化 |
| `McpServer.*`（2 件） | initialize・通知・tools/list・tools/call（成功・失敗は `isError`）・未知メソッド・パースエラー・ping、アプリ専用コマンドのスキーマ |
| `cli_script` / `cli_reopen` | スクリプトでスラブ + 柱 4 本を作って保存 → 開き直して確認 |
| `mcp_headless` / `mcp_headless_cli` | 実バイナリに MCP セッションを標準入力で流す |
| `gui_phase1: aiReadsKeepDrawingInProgress` / `proposalShowsRotationAndHide` | 矩形の 1 点目を置いたまま AI が読み取り・preview をしても続きを描ける（書き込みは中断する）、提案が回転と非表示を拾い、適用結果と一致 |
| `gui_phase1: aiBridgeOverLocalSocket` | 既定 OFF で接続拒否 → ON にして実ソケットで作成・選択・選択読み出し・ビュー画像、保存は拒否、Ctrl+Z 相当で AI の操作を戻せる、OFF で切断 |
| Windows CI | zip に `sbr_cli.exe` / `sbr_mcp.exe` を同梱し、zip のフォルダから MCP セッションを実行 |
- マテリアル第 2 段: `update_material` に `opacity` / `texture` / `metallic` / `roughness` / `reflectance`、`duplicate_material`、`set_material_texture {material, file, width, height, colorize}`（docs/11_materials.md）


## DXF の取り込み（ヘッドレス CLI）

`import_dxf` は ASCII DXF のモデル空間を線図として取り込む。
`path` を指定する。単位不明の図面は `unitless_scale_to_mm`（既定 1）で指定する。
未対応要素があると拒否する。`allow_partial: true` のときだけ読める要素を取り込む。
成功時の `report` に取り込み数・除外要素・単位を返す。
アプリ内 AI ではファイル操作として禁止する。
DWG は GUI から ODA File Converter で変換する。
対応範囲は [24_dxf_dwg_import.md](24_dxf_dwg_import.md) を参照。
