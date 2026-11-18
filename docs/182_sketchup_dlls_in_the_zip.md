# SketchUp SDK の DLL を ZIP に入れる（2026-11-18）

[docs/181](181_sketchup_skp.md)（SketchUp の読み書き）の続き。作業は Opus 5.5。
オーナーの決定：2026-11-18（「SDK の DLL を ZIP に入れていいか」に「入れていいよ」）。

## 直す前

docs/181 では、[docs/53](53_handover_2026-11-04.md) の決まり（SDK のファイルはどこにも置かない）のとおり、ZIP に SDK の DLL を入れず、SDK を置いた PC（`C:\Apps\SDK_Win`）からだけ読み込んでいた。ほかの PC では SKP を使えなかった。

## 直した後

| 置き場所 | SDK のファイル |
|---|---|
| ZIP | **入れる**：`SketchUpAPI.dll`・`SketchUpCommonPreferences.dll`（exe の隣）、規約の案内 `licenses/sketchup-sdk/README.md`。VC ランタイムの `msvcp140_codecvt_ids.dll`（SDK の DLL が使う。ZIP の VC ランタイムと同じ 14.51 の版） |
| Git | 今まで通り入れない（ヘッダ・DLL・見本） |
| docs | 今まで通り入れない（関数の名前と使い方だけ） |

- アプリは、**exe の隣の `SketchUpAPI.dll` を最初に探す**。次に環境変数 `SBR_SKETCHUP_SDK`、次にビルドした時の SDK のフォルダ。
- ZIP を展開すれば、SDK の無い PC でも「SketchUp (*.skp)」が一覧に出て使える。
- AI の `export_skp`・`import_skp` の結果に、使った DLL の場所（`sdk_dll`）を出す。
- ZIP を作るスクリプト（`tools/package_windows_local.py`）に `--sketchup-sdk`（SDK のフォルダ）と `--vc-extra` を足した。開発用のヘルパー（`sbr.py package`）は、ビルドの CMake の設定 `SBR_SKETCHUP_SDK_DIR` があれば自動で渡す。`BUILD_INFO.json` に、入れた DLL と SHA256 を書く。
- 第三者のお知らせ（`THIRD_PARTY_NOTICES.md`）に SketchUp SDK と Trimble Developer Terms を足した。

## 決めたこと

| 項目 | こうした | 理由 |
|---|---|---|
| ZIP に入れる物 | 実行に要る DLL 2 つだけ | ヘッダ・ドキュメント・見本は動くのに要らない |
| docs/53 の決まり | 「Git・docs には置かない。ZIP の DLL はオーナーの決定（2026-11-18）で入れる」に読み替え | オーナーの決定。Trimble の書面の許可そのものは、この作業では確かめていない |
| codecvt の DLL | ビルド環境（sbr-build-env）の 14.51.36247 | ZIP の `MSVCP140.dll` と同じ版。Visual Studio の再配布用フォルダの物は 14.44 で古い |

## まだやっていないこと

- SDK の無い PC での確かめ（この PC には SDK があるので、exe の隣の DLL が使われたことを `sdk_dll` で確かめた）。
- Trimble の規約の全文を ZIP に入れること（案内と URL だけ）。

## 確かめたこと

| 段階 | 内容 |
|---|---|
| 計算テスト済み | コア：SKP・DAE のテスト 9 件中 8 件成功・1 件スキップ（SDK がある時の「SDK が無い時」）（ローカル） |
| AI が配ったアプリで確認 | 展開した ZIP の `sbr_cli` で、箱を SKP に書き出して読み戻せた（面 6 枚・立体 1）。`sdk_dll` は書き出し・読み込みとも ZIP の中の `SketchBRep\SketchUpAPI.dll`（`C:\Apps\SDK_Win` の物ではない）。ZIP を作る時の依存の検査（109 個）も成功 |
| CI | run 44250936541（push、`8f70653`）：Linux 767 件成功、Windows 763 件＋3 件成功・GUI 237 件成功（7 件スキップ）。`Skp.SaysSoWithoutTheSdk` が Linux・Windows とも成功 |
| Windows 実機確認 | まだ（チェック表 13-1・13-2・13-3） |

## 配布の記録

| 項目 | 値 |
|---|---|
| コード | `8f70653`（SketchUp SDK の DLL を ZIP に docs/182。Opus 5.5 で作業） |
| CI | run 44250936541（push）：Linux・Windows とも成功 |
| ZIP | `SketchBRep-windows-x64-8f70653-local.zip`（Codex のチャットフォルダの `outputs\`）。SketchUp SDK の DLL 2 つと `msvcp140_codecvt_ids.dll` 入り（`BUILD_INFO.json` に SHA256） |
| サイズ | 81,622,271 bytes |
| SHA256 | `8752fc487177688c3dde2a8dbba546f03e30a9fa7fd280b7e6d65e389a9f48a6` |
| ZIP の検証 | `verify_portable_pads.py`・`verify_portable_mat.py` とも成功（docs/validation/2026-11-18-portable-8f70653*.json） |
