# Claude（Opus 5.5）→ Codex への引き継ぎ（2026-11-18）

オーナーの指示（2026-11-18）：Claude の週の利用量が上限に近づいたら Codex に渡せるように。Claude はギリギリまで作業し、この文書をその都度直す。**ここに書いた「今の状態」が最新**。

## 決まり（変えない）

- ブランチ `claude/sketchbrep-modeler-spec-4wnp0b` だけ。main へマージしない、amend・rebase・force-push しない、draft PR higeotoko/sketchBRep#1 をマージしない。
- 返事は日本語（くだけた口調）。法の値は根拠と確認日。測れない物は「未評価」（合格にしない）。
- 確かめた事は段階を分ける：計算テスト済み / GUI 自動確認済み / AI が配ったアプリで確認 / Windows 実機確認。確かめていない事を済みと書かない。
- Git・ZIP・docs に入れない物：資格情報、オーナーの非公開 DXF、Jw_cad 付属の .jww、SumiCAD の MCP で得た案件の情報。**SketchUp SDK は Git・docs に入れない**。ZIP には実行に要る DLL 2 つだけ入れてよい（オーナーの決定 2026-11-18、docs/182）。
- 流れ：コード → commit → push → ZIP（`package`）→ 検証（`verify`）→ 記録（`record`）→ CI（`ci --wait`）→ docs の記録 → push。ZIP は上書きしない。インストーラーは作らない。
- コマンドの出力は最後まで読んでから次へ（オーナーの指示）。

## 環境（この PC）

- リポジトリ：`C:\Users\enomo\Documents\Codex\2026-11-04\higeotoko-sketchbrep-claude-sketchbrep-modeler-spec\work\sketchBRep`
- ZIP の置き場：`C:\Users\enomo\Documents\Codex\2026-11-04\files-pasted-by-the-user-codex\outputs`
- ヘルパー：`python -X utf8 .claude/skills/sketchbrep-dev/scripts/sbr.py <build [targets] | test core [--filter F] | test gui --match RE --scale1x | package | verify | record | ci --wait>`（Python は `C:\Users\enomo\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe`）。`.claude/skills/sketchbrep-dev/SKILL.md` に落とし穴。
- `package` は**作業ツリー**をビルドする（ZIP の名前は HEAD）。ZIP の行が出るまでファイルを触らない。`ci --wait` は始めた時の HEAD を見る。CI が走っている間は push しない。
- ビルドの CMake は SketchUp SDK を指している（`build/CMakeCache.txt` の `SBR_SKETCHUP_SDK_DIR=C:/Apps/SDK_Win`）。作り直す時は `cmake -S . -B build -DSBR_SKETCHUP_SDK_DIR=C:/Apps/SDK_Win`。`package` はそこから SketchUp の DLL を ZIP に入れる。
- この PC だけで落ちる画面テスト：`lineToolCrossingLinesMakeFace`（線ツールのクリックの位置）と、クリップボードの 2 件。CI では通る。

## 今の状態（2026-11-18、Claude が最後に書いた所）

| 物 | 状態 |
|---|---|
| docs/164〜182 | 済。最新の配布は `8f70653`（CI 成功 run 44250936541、ZIP `SketchBRep-windows-x64-8f70653-local.zip`、記録 `3a33750`） |
| docs/183（SketchUp にタグ・物の情報・コンポーネント） | タグと情報は `1597527` で push・ZIP 済み（CI run 44259529231 は待ち）。コンポーネントの共有は手元でコミット（push 待ち） |

### 最後に済んだ事

- `1597527`：タグと情報（docs/183 の前半）、この引き継ぎ書。ZIP `SketchBRep-windows-x64-1597527-local.zip`（81,640,950 bytes、検証は両方成功）。CI run 44259529231 の結果を見て docs/183 に記録する。
- その次のコミット：コンポーネントの共有（`Skp.ComponentsAreShared`）。CI が終わってから push → ZIP → 検証 → CI → docs/183 の記録。

## 次にやる事（オーナーが選んだ順：SketchUp 連携の 1 → 2 → 4）

1. ~~物の情報を SketchUp の属性に~~（docs/183）
2. ~~タグ・コンポーネントの共有の書き出し~~（docs/183）。残り：読み込みで SketchUp のコンポーネントを SketchBRep のコンポーネントに戻す（`Reader` と `Modeler::addColouredParts`、`Modeler::makeComponent`）。
3. **LayOut（.layout）に図面一式を書き出す**：SDK の `headers/LayOutAPI`・`binaries/layout/x64`。SketchUp と同じく、Git にヘッダを入れず、動く時に DLL を読み込む作り（`Skp.cpp` の `SBR_SU_FUNCTIONS` と同じやり方）。図面は `doc::DrawingView`（`Drawing.cpp`）と図面一式（docs/125・130・131）から。
4. その先の案（オーナーにまだ聞いていない）：テクスチャ、SketchUp で直した物を戻す（`skp_source_id` を使う）、SketchUp の Ruby 拡張とのライブ連携、シーン・断面、寸法、敷地の位置。

### SketchUp まわりの作り（docs/181〜183）

- `src/core/include/sbr/geom/Skp.h`・`src/core/src/Skp.cpp`：SDK の関数は `SBR_SU_FUNCTIONS` の一覧を `GetProcAddress` で引く（リンクしない）。足す関数はこの一覧に足す。`SBR_WITH_SKETCHUP` が無い時（CI）は断るだけの版。
- 部品の受け渡しは COLLADA と同じ `geom::ColladaPart`（形・名前・色・面ごとの色・タグ・属性）。読み込みの面の縫い合わせは `geom::joinFaces`（`Collada.cpp`）。
- テスト：`tests/test_skp.cpp`（SDK が無いとスキップ。CI では `Skp.SaysSoWithoutTheSdk` だけ動く）。

## オーナーにしかできない事

- docs/105 のチェック表（特に 13 章：SketchUp で開く・SketchUp の物を読む・SDK の無い PC）。
