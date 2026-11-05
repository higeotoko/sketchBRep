---
name: sketchbrep-dev
description: >-
  SketchBRep の開発の決まった流れ（ビルド・テスト・Windows ZIP・ZIP の検証・CI・配布記録）を、
  要点だけ返すヘルパー（scripts/sbr.py）で回すためのスキル。このPC（Windows、MSVC/OCCT 8/Qt 6）の環境の
  落とし穴とコミット前の確認項目も持つ。SketchBRep のリポジトリでコードを変えた・テストしたい・ZIP を作る・
  CI を確かめる・docs に配布記録を書く・「次の機能を実装して」と言われた時は、手でコマンドを組み立てる前に必ず使う。
---

# SketchBRep 開発の流れ

このスキルは、ビルド → テスト → コミット → push（CI は push で自動起動）→ ZIP → 検証 → 配布記録、を
**短い出力で**回すためのもの。長いログを読むのはトークンの無駄なので、まず `sbr.py` の要約を見て、
失敗した時だけ詳しいログ（要約に出るファイルパス）を読む。

リポジトリの決まり（ブランチ・禁止事項・報告の仕方）は直下の `CLAUDE.md` が正。ここは「どう回すか」だけ。

## ヘルパー

```bash
PY=/c/Users/enomo/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe
S=.claude/skills/sketchbrep-dev/scripts/sbr.py   # リポジトリ直下から
"$PY" -I $S build                      # 既定: core tests, cli, gui tests, app, mcp, phase0
"$PY" -I $S build sbr_core_tests       # 対象を絞る
"$PY" -I $S test core                  # コア全件。失敗だけ詳しく
"$PY" -I $S test core --filter 'FoundationMat.*'
"$PY" -I $S test gui NAME1 NAME2       # GUI の関数名を指定
"$PY" -I $S test gui --match 'foundation|timber'   # 関数名の正規表現
"$PY" -I $S package                    # HEAD のコミットで ZIP（既存は上書きしない）
"$PY" -I $S verify                     # 展開した ZIP で従来＋べた基礎等の検証
"$PY" -I $S ci                         # HEAD の CI の状態（--wait で終わるまで待つ）
"$PY" -I $S record                     # 検証 JSON を docs/validation/ へ。ZIP のサイズ・SHA256 を表示
```

時間がかかるもの（`test core` 約1分、`build` 数分、`package`+`verify` 約10分、`ci --wait` 約25分）は
Bash の `run_in_background` で回し、終わりの通知を待つ（ポーリングしない）。

## 変更ごとの流れ

1. 実装 → `build` → 関係するテスト（`test core --filter` / `test gui --match`）→ `test core`（全件）。
2. コミット（コードだけ。記録の docs は別コミット）→ push。**push で CI が自動起動**する（docs だけの push では起動しない）。
3. `package` → `verify`。ZIP は HEAD のコミット名。
4. `ci --wait` で緑を確認してから、docs に記録（CI の run 番号と件数、ZIP のサイズ・SHA256、`record` の JSON）→ 別コミット → push。
5. オーナーに ZIP の場所を渡す。確認は「計算テスト済み／GUI 自動確認済み／Windows 実機確認（人の操作）」に分けて書く。

## 落とし穴（このPC）

- **CI の取り消し**：同じブランチの新しい実行が古い実行を取り消す。CI の途中でコードを push すると前の結果は出ない。
  記録用に結果が欲しい時は、終わってから次を push する（docs だけの push は CI を起動しないので安全）。
- **表示倍率 175%**：画素に敏感な GUI テスト（辺のクリック等）はローカルで落ちることがある。GUI 全件は CI が正。
  1倍に近づけるには `test gui ... --scale1x`（`QT_SCALE_FACTOR=0.5714`）。それでも近似。
- **クリップボード**：`helpWindowAndCrashLog` は、オーナーが他のアプリでクリップボードを使っていると「空」で落ちる。環境要因。
- **文字コード**：テスト・CLI の出力は CP932 と UTF-8 が混ざる。`sbr.py` は両方試して読む。自分で読む時も同じ。
- **ヒアドキュメント**：Bash ツールで `<<'EOF'` の中に引用符が多いとシェルが壊れることがある。
  パッチや長い Python は Write でファイルに書いてから実行する。Python で書き戻す時は `newline=''` で改行を保つ。
- **改行**：リポジトリは LF（`git ls-files --eol`）。「LF will be replaced by CRLF」の警告は無害。
- **パス**：リポジトリ、Python、CMake、vcvars、配布フォルダの場所は `sbr.py` の先頭（`CONFIG`）にある。別PCではそこを直す。

## コミット前の確認

- C++：一時 JSON のメンバーを範囲 for で回さない（`for (x : json::parse(s).at("k"))` は一時が先に消える）。
  nlohmann/json の `{要素1つ}` は配列にならない（`json::array({...})` を使う）。OCCT 7 と 8 の差（拡大縮小つきの位置は OCCT 7 で例外）。
  ウィンドウを閉じる時に先に壊れる部品（Viewport など）には QPointer で触る。
- 「CI 済み」と書くのは、その変更を含むコミットの CI 結果を見てから。ZIP はローカルのビルドで、CI の生成物ではない。
- 構造・法令：形の一致を耐力・合格に読み替えない。情報が足りなければ「未評価」。法令の値は原文で確かめてから使い、
  出典と確認日を結果と docs に書く。PDF は `pdfminer`（`pypdf` は日本語の縦書き・SJIS で崩れることがある）で取り出し、
  縦書きは改行を消してから読む。取り出せない資料（国交省の補足資料など）は「未照合」と明記する。
- 認証値・オーナーの非公開データ・SumiCAD のプロジェクト情報は Git にも ZIP にも入れない。
