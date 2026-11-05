# SketchBRep — Claude Code への指示（リポジトリ常設）

Qt 6 + Open CASCADE（OCCT）の B-rep モデラー。SketchUp のような直接操作、正確な B-rep（Rhino 的な形状）、建築機能（通り芯・レベル・部材・木造架構・基礎・図面）。

## 最初に読む

1. 一番新しい引継書：`docs/70_handover_cloud_to_local_2026-11-05.md`（ローカル開発への移行）と `docs/69_handover_codex_to_claude_2026-11-05.md`（直近の実装・手順・SumiCAD MCP）。
2. `git status` と `git log --oneline -15`。完了済みの実装を作り直さない。引継ぎのためだけに再ビルド・全テストをしない。
3. 機能の記録は `docs/NN_*.md`（番号順）。AI のコマンド一覧は docs/06。構造種別の手引きの設計は docs/52。

## 守ること

- ブランチ `claude/sketchbrep-modeler-spec-4wnp0b` だけで作業。通常のコミットと push はしてよい（依頼済み）。**main へのマージ、amend、rebase、force-push はしない。** draft PR higeotoko/sketchBRep#1 を勝手にマージしない。
- 認証値（SumiCAD MCP の Authorization など）、オーナーの非公開 DXF、Jw_cad 付属サンプル（.jww）、**SketchUp SDK**（Trimble の書面許可まで）を Git・配布物・文書に入れない。MCP で取った SumiCAD のプロジェクト情報も Git に入れない（読んだ範囲と採用した考え方だけ docs に書く）。
- 有料 AI API を前提にしない。B-rep が正規データ。GUI も AI も同じ Modeler・Undo の経路を使う。
- 普通のモデリングを構造ルールで止めない。形が合っていることを、耐力・構造の合格に読み替えない。情報が足りない時は「未評価」（合格にしない）。RC の配筋設計は対象外。
- 壁量などの係数表は、いつ時点の基準かの但し書きを結果に必ず出す（docs/52 §7.3）。

## 報告のしかた

- オーナーへは日本語で、専門用語はかみくだく。何をしたか・何を決めたかを必ず書く。口調はオーナーの好み（知的なギャル口調）に合わせる。
- 確認は 3 段階で分けて書く：**計算テスト済み / GUI 自動確認済み / Windows 実機確認（人の操作）**。やっていないことを確認済みと書かない。
- 変更ごとに：コードのコミット → Windows ZIP（`tools/package_windows_local.py`、docs/63）→ 検証 → 配布記録の文書コミット → push → **ZIP の場所（リンク）をオーナーに渡す**。既存の ZIP を上書きしない（ファイル名にコミット番号）。
- インストーラーは今は作らない（CI でも `if: false` で停止中）。ポータブル ZIP で配る。

## ビルドとテスト（このPC：Windows）

- MSVC 19.44 / OCCT 8.0.1 / Qt 6.12 / 3DM 有効。vcvars64 を呼んで、DLL の PATH を通してから CMake。手順と補助スクリプトの場所は docs/69「ローカルで再開する手順」。
- GUI テストは Windows の実表示で。Qt Test の `-o` は `path,junitxml` を 1 つの引数に。成功は終了コードと XML/JSON で判断する。
- Linux（CI と同じ）：`cmake -S . -B build -G Ninja && ninja -C build`、GUI テストは `Xvfb :99` と `DISPLAY=:99`（offscreen では止まる）。
- OCCT 7 と 8 の API 差（`DynamicType` なし → `typeid`）、MSVC のマクロ（`near` など）に注意。OCCT の Boolean で共有形状を変えないよう、読取の検査は `SetNonDestructive(true)`、表示用 Compound には `BRepBuilderAPI_Copy` のコピーを入れる。

## CI

- GitHub Actions（`.github/workflows/ci.yml`）。docs だけの変更では走らない。
- **2026-11-04 09:11 UTC（`5d2a4e1`）以降、CI が 1 度も起動していない**（docs/70 §3）。直近の検証はすべてローカル。CI が復活するまで、ローカル検証を「CI 済み」と書かない。
