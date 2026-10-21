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
| `api::CommandApi` | `src/core/.../api/CommandApi.h` | `{"command", "args"}` → `{"ok", "result" \| "error"}`。41 コマンド + `batch` + `describe`。Qt 非依存 |
| `api::McpServer` | `src/core/.../api/McpServer.h` | MCP の `initialize` / `tools/list` / `tools/call` / `ping` を CommandApi に変換。通信路に依存しない |
| `sbr_cli` | `tools/cli` | ヘッドレス実行。JSON Lines のスクリプト、`--schema`、`--mcp`（ヘッドレス MCP） |
| `sbr_mcp` | `tools/mcp` | MCP サーバー。起動中のアプリへ転送（`--headless` でアプリなし） |
| `AiBridge` | `src/app/AiBridge.*` | アプリ内のローカルサーバー。メニュー「AI 連携 > 外部 AI からの操作を許可」で ON |

## コマンド（ツール）一覧

`sbr_cli --schema` で JSON Schema 付きの一覧が出る（そのまま LLM の tool 定義になる）。

- 調べる: `list_objects`, `get_object`（面の番号・法線・重心・面積、パラメータ一覧つき）
- 作る: `create_line`, `face_from_loop`（線・円弧で囲んだ輪郭を面に）, `create_rectangle`, `create_circle`, `create_arc`（正確な円弧・弓形の面）, `create_polygon`, `create_curve`, `create_box`
- 注記: `create_dimension`, `create_text`, `set_text`（寸法・部屋名や面積のラベル。`get_object` で文字も読める）
- 補助: `create_guide`（点と方向で無限長のガイド線。作図の吸着先、出力対象外）, `clear_guides`
- 編集: `split_face`（面を線で分割）, `extrude`, `push_pull`（面は番号か `{"normal": [0,0,1]}`）, `set_parameter`, `offset`, `move`, `rotate`, `copy_array`（直線・円形）, `boolean`, `revolve`, `loft`, `sweep`
- 構造・属性: `group`, `ungroup`, `make_component`, `set_definition_parameter`, `rename`, `set_tag`, `paint`, `delete`
- 表示状態: `set_section`（断面。平面図・断面図の切断）, `list_tags`, `set_tag_visible`
- 3DM: `export_3dm` / `import_3dm`（ファイル系なのでアプリ経由では無効、`sbr_cli` 等で使用）
- 履歴・ファイル: `undo`, `redo`, `save`, `open`, `export_step`（**アプリ経由では無効**）
- アプリ起動中のみ: `get_selection`（ユーザーが選んだ物・面）, `select`, `set_view`, `capture_view`（ビューの PNG を AI に画像で返す）, `list_scenes`, `go_to_scene`（保存ビューの表示・保存）
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

## 使い方

### 1. Claude Desktop / Claude Code から、開いているモデルを操作する
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
| 知らないうちに外部から操作される | アプリ側は **既定 OFF**・明示的に ON。ローカルソケットのみ（ネットワークに開かない）、同じユーザーのみ接続可（`UserAccessOption`）。ステータスバーに接続状態を表示 |
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
| 次 | 寸法線・通り芯・レベルなど建築要素のコマンド化（AI が「通り芯 X3」のような言葉で参照できるように） |
| 中期 | 面・エッジの **意味的な名前**（「上面」「押出 2 の側面 3」）。TNP 強化と合わせて、番号より壊れにくい参照に |
| 中期 | マクロ記録: GUI 操作を JSON Lines に記録 → AI が一般化して再利用 |
| 中期 | MCP の resources（モデル概要・選択状態の購読）と、変更通知 |

## テスト

| テスト | 内容 |
|---|---|
| `CommandApi.*`（7 件） | 箱 → 面の押し出し → パラメータ再編集 → Undo、エラー時に何も変えない、`batch` が Undo 1 回・`$N` 参照、失敗した `batch` の全ロールバック、壁に描いたドアの押し込み、スキーマ、拡張コマンドとファイル系の無効化 |
| `McpServer.*`（2 件） | initialize・通知・tools/list・tools/call（成功・失敗は `isError`）・未知メソッド・パースエラー・ping、アプリ専用コマンドのスキーマ |
| `cli_script` / `cli_reopen` | スクリプトでスラブ + 柱 4 本を作って保存 → 開き直して確認 |
| `mcp_headless` / `mcp_headless_cli` | 実バイナリに MCP セッションを標準入力で流す |
| `gui_phase1: aiBridgeOverLocalSocket` | 既定 OFF で接続拒否 → ON にして実ソケットで作成・選択・選択読み出し・ビュー画像、保存は拒否、Ctrl+Z 相当で AI の操作を戻せる、OFF で切断 |
| Windows CI | zip に `sbr_cli.exe` / `sbr_mcp.exe` を同梱し、zip のフォルダから MCP セッションを実行 |
