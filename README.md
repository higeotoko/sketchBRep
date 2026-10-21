# SketchBRep（仮称）

建築設計向けハイブリッド 3D モデラー。
**操作は SketchUp、形状の本質は Rhino** — 面をクリックして押し出すだけの直接操作で、
内部では B-rep（Open CASCADE）を正規データとして保持します。

> 現在のステータス: **Phase 2（実用モデリング基礎）**

![Phase 2 screenshot](docs/images/phase2.png)

## できること

### Phase 0〜1

- 矩形・線（閉じると面）を描く。原点・端点・中点・エッジ上・面上スナップ、XYZ 軸推論、Shift/矢印キーでの方向ロック
- Push/Pull: 面 → ソリッド押出、ソリッド平面フェースの移動（Boolean + 面統合、失敗時ロールバック）
- 作図中の数値入力（`3000`, `4.5m`, `4000,6000`）と、直後の再入力による寸法変更
- パラメトリック再編集: 矩形の幅/奥行、押出距離、各 Push/Pull 距離を後から変更（レシピを再計算）
- Undo / Redo、ネイティブ保存（`.sbr`、原子的保存 + 検証）、STEP 入出力
- 表示メッシュと正規形状の分離（表示品質を変えても CAD 形状の精度は不変）

### Phase 2
- 移動 / 回転 / コピー: Ctrl でコピー、確定後に `x4`（同間隔で 4 個）・`/4`（4 等分）で直線・円形配列、数値の打ち直しで再編集
- オフセット: 平面プロファイルの輪郭を内外にオフセット（元の面は残し、新しい面を作成。距離は後から変更可）
- ソリッド演算（和・差・積）: **パラメトリック**。演算後も各入力の寸法（例: 開口の高さ）を変更すると結果が再計算される
- グループ / コンポーネント: 階層と配置の入れ子、定義を共有するインスタンス（定義の寸法を変えると全インスタンスが更新）、分解
- 推論の強化: 点から（追跡点を通る軸線）、軸と追跡点の交点、エッジに平行・垂直
- オブジェクト管理: 階層ツリー、タグ、色、位置の数値編集、表示/非表示
- STEP 連携の安定化: オブジェクト名・色を保持（XCAF）、グループ配置を反映、アセンブリ読込の平坦化

### 操作感（実機フィードバック反映）
- **面の上に描いて押し込むと穴・開口、引き出すと増築**: 壁に矩形を描いてプッシュ/プルで押し込むと開口になる（1 オブジェクトのまま、深さは後から変更可、Undo 1 回で元通り）

  ![壁に描いた矩形を押し込んで開口](docs/images/opening.png)
- 角やエッジから描き始めても、カーソル下の面の上に矩形・円・線が描ける（線は面から外れても同じ平面上）。背面に隠れた点・エッジにはスナップしない
- オフセット（F）はソリッドの面にも使える。面の内側に輪郭を作り、押し込めば枠やくぼみになる
- ツールごとのカーソルとツールバーのアイコン（鉛筆・矩形・プッシュ/プル・移動・回転・オフセット・曲線・回転体、オービット中は手のひら）
- プッシュ/プルや作図の後にオブジェクト全体が選択状態にならない
- 右パネルは「名前・選択・主要寸法」だけを表示し、タグ・色・位置・履歴・ID・入力の寸法などは折りたたみの「詳細」に格納

### AI 連携（MCP）
- メニュー「AI 連携 > 外部 AI からの操作を許可」（既定 OFF）を ON にすると、Claude Desktop / Claude Code などが `sbr_mcp` 経由で開いているモデルを操作できる（作成・押し出し・寸法変更・配列・ブーリアン・選択の読み取り・ビュー画像・シーンなど 36 ツール）
- AI の操作も GUI と同じコマンド基盤: 形状チェック・失敗時に何も変えない・Ctrl+Z で戻せる。まとめて実行（`batch`）は Undo 1 回分で、失敗すれば全部取り消し
- `sbr_cli` でアプリなしのスクリプト実行（JSON Lines）・ヘッドレス MCP。詳しくは [docs/06_ai_integration.md](docs/06_ai_integration.md)

## ビルド

依存: C++17 / CMake 3.21+ / Open CASCADE 7.6+ / Qt 6.2+ / nlohmann-json / GoogleTest

### Windows 11

**すぐ試す:** PR の CI（Actions）の成果物 `SketchBRep-windows-x64` をダウンロードして展開し、`SketchBRep.exe` を実行（インストール不要のポータブル版。実機で動作確認済み）。

**自分でビルド（vcpkg + Visual Studio 2022）:**

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install opencascade qtbase nlohmann-json gtest --triplet x64-windows

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build\src\app\Release\SketchBRep.exe
```

（vcpkg の opencascade 初回ビルドには時間がかかります。配布時は `windeployqt` で Qt DLL を同梱してください。）

### Linux（開発・CI 用。Ubuntu 24.04）

```bash
sudo apt install cmake ninja-build g++ libocct-*-dev libtbb-dev libfontconfig-dev qt6-base-dev libgl-dev \
                 nlohmann-json3-dev libgtest-dev xvfb
cmake -S . -B build -G Ninja && cmake --build build
xvfb-run -a ctest --test-dir build --output-on-failure   # GUI テストはディスプレイが必要
./build/src/app/SketchBRep
```

CMake オプション: `SBR_BUILD_APP`（GUI）, `SBR_BUILD_TESTS`, `SBR_BUILD_TOOLS`（すべて既定 ON）。
GUI なしでコアだけ検証する場合は `-DSBR_BUILD_APP=OFF`。

## 使い方

| 操作 | 内容 |
|---|---|
| ホイール / ホイールドラッグ / Shift+ホイールドラッグ | ズーム（カーソル位置へ）/ オービット（カーソル下の点が中心）/ パン |
| 選択ツール: クリック / ダブルクリック / トリプルクリック | 面・エッジ / ＋隣接（面→周囲のエッジ、エッジ→両側の面）/ オブジェクト全体（Shift で追加・解除）。グループ・コンポーネントはクリックで全体 |
| グループをダブルクリック / Esc | グループ編集モード（中の要素を直接選択・編集、新しく描いた形はそのグループに入る。外側は半透明）/ 終了 |
| Delete | 選択を削除（ソリッドの面・エッジ単体は不可。単独の面・線はそのまま削除できる） |
| 何もない所をダブルクリック・中ボタンのダブルクリック | 選択対象へフォーカス（表示メニューで ON/OFF） |
| F2〜F7 / F8 / Shift+Z | 標準ビュー（アイソメ・平面・正面・右・背面・左）/ 透視⇔平行 / 全体表示 |
| Space / L / R / P | 選択 / 線 / 矩形 / プッシュ/プル |
| M / Q / F | 移動 / 回転 / オフセット（移動・回転中は Ctrl でコピー切替） |
| C | 円（中心 → 半径。面の上にも描ける。数値で半径、直後の再入力で変更） |
| A | 円弧（始点 → 終点 → 膨らみ。半円にスナップ。既定は押し出せる弓形の面、Ctrl で線だけ。直後の数値で膨らみを変更） |
| ビュー上部の「＋ シーン」/ タブ | 今のビューをシーンとして保存 / クリックでそのビューへ（右クリックで更新・名前変更・削除。ファイルに保存される） |
| N / V / 曲面メニュー | NURBS 曲線（Enter で確定・始点で閉じる）/ 回転体ツール（面 → 軸の 2 点、数値で角度変更）/ ロフト・スイープ |
| プッシュ/プルでダブルクリック | 直前と同じ距離で押し出し |
| T | メジャー（2 点をクリックして距離と ΔX/ΔY/ΔZ を表示。モデルは変更しない） |
| B | ペイント（選ぶたびに色を指定、クリックで塗る、Alt+クリックで色を拾う。グループは丸ごと） |
| E | 消しゴム（クリックで削除。グループ・コンポーネントは丸ごと、グループ編集中は中の要素） |
| 面を選んで M | 面をつかんで移動 = ソリッドを法線方向に伸縮 |
| `x3` / `/3` を打って Enter | コピー直後に配列化 / 等分割 |
| Ctrl+G / Ctrl+Shift+G / Ctrl+K | グループ化 / グループ解除 / コンポーネント作成 |
| ソリッドメニュー | 和・差（最初に選んだものから他を引く）・積 |
| 数字を打って Enter | 寸法指定（作図中）・直前の操作の寸法変更（作図後） |
| Shift（押している間） / → ← ↑ / ↓ | 推論方向ロック / X・Y・Z 軸ロック / ロック解除 |
| Esc / Ctrl+Z / Ctrl+Y / Delete | 中止 / 元に戻す / やり直し / 削除 |

**最初の成功体験:** `R` → 原点をクリック → マウスで方向を示して `4000,6000` Enter →
`P` → 面をクリック → 上に動かして `3000` Enter → `Ctrl+S`。
右パネルの「押出距離」を `4500` に変えれば高さが再計算されます。

Phase 0 の CLI 検証: `build/tools/phase0/sbr_phase0 1000 2000 3000 box.step`
スクリプト実行: `build/tools/cli/sbr_cli --save out.sbr tests/data/columns.jsonl`（コマンド一覧は `sbr_cli --schema`）

## ドキュメント

- [docs/01_feasibility.md](docs/01_feasibility.md) — 技術的成立性・難易度・代替案の分析
- [docs/02_architecture.md](docs/02_architecture.md) — アーキテクチャ・データモデル・ファイル形式
- [docs/03_phase0-1_report.md](docs/03_phase0-1_report.md) — Phase 0〜1 実装報告（確認方法・テスト・既知の制限・次フェーズ課題）
- [docs/04_phase2_report.md](docs/04_phase2_report.md) — Phase 2 実装報告（確認方法・テスト・既知の制限・Phase 3 への課題）
- [docs/05_phase3_progress.md](docs/05_phase3_progress.md) — Phase 3 進捗
- [docs/06_ai_integration.md](docs/06_ai_integration.md) — AI 連携（コマンド API・MCP・安全設計・活用方法・ロードマップ）

## ライセンスに関する注意

Open CASCADE Technology は LGPL 2.1（+ 例外）、Qt 6 は LGPL 3 / 商用のデュアルライセンスです。
配布形態（動的リンク・再リンク可能性の確保など）は [docs/01_feasibility.md](docs/01_feasibility.md#ライセンス) を参照してください。
