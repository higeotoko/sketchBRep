# 更新版ZIPを毎回渡す（2026-11-05）

所有者から「変更版のZIPも毎度リンク」「書き出したら次に進めて」と指示があった。
以降は、変更記録・コミット・検証結果に加え、変更後のWindows ZIPへのリンクを毎回渡す。
作成済みのZIPを上書きせず、コミット番号をファイル名に含める。
CIで生成した配布物と、ローカル通常ビルドで検証した配布物を区別する。

## 今回の配布

- 実装コミット：`1870d1d60eba4d8c90a44fecf495062048155cb5`
- ZIP：`SketchBRep-windows-x64-1870d1d-r1-local.zip`
- サイズ：69,643,515 bytes（約66.4 MiB）
- SHA256：`255c2f444accd227e9adf336c33ce97ea4922475ca7b770f0082e97e9b25a32e`
- 内容：開口・架構の基準線修正、建築ツールバー、平面での窓・扉の配置。
- 全て展開して `SketchBRep/SketchBRep.exe` を起動する。

`tools/package_windows_local.py` を追加した。ビルド済みのアプリと検証済みのランタイム一式を
新しい作業フォルダーへコピーし、依存DLL・既存ランタイムのハッシュ・ライセンスを確認する。
Gitコミットと実行ファイルのSHA256を `BUILD_INFO.json` に入れ、変更記録・検証サマリーも同梱する。
Qtプラグインを同梱フォルダーから読む `qt.conf` を入れる。ZIPのCRCとSHA256も確認する。

```text
python tools/package_windows_local.py --runtime VERIFIED_PORTABLE_DIR --build build \
  --stage NEW_STAGE_DIR --output NEW_COMMIT_LOCAL.zip --dumpbin DUMPBIN_EXE
```

先にMSVC開発環境で `SketchBRep sbr_cli sbr_mcp sbr_phase0` をビルドすること。
別バージョンのQt/OCCT等へ更新した場合は、以前のランタイムを再利用せず、
現在の依存関係とライセンスを収集し直して新しいマニフェストを作る。

## 配布物の確認

- 計算・依存確認：100 DLLのハッシュと104実行ファイル/DLLの依存を確認。
  ZIPを別フォルダーへ展開し、実行ファイルのハッシュも照合。
  PATHを配布物とWindowsだけに制限してSTEP出力、CLIの3DM出力、MCPの初期化・ツール一覧・形状作成が成功。
- GUI自動確認：展開したZIPのDLL・プラグインで、建築ツールバーと平面開口、回転壁の6条件、
  非表示・ロック・曖昧な壁の除外、従来の開口操作の10条件が成功（初期化・終了込み12件、終了コード0）。
  試験実行のためだけに `sbr_gui_tests.exe` と `Qt6Test.dll` を展開後に追加した。この2ファイルはZIPに含めない。
- Windows実機確認：人による一連の操作は未実施。CI検証版やクリーンPCでの実機確認を示すものではない。

今回の実装の関連コア51件・GUI16条件の記録はdocs/62にも残してある。
起動中のアプリを置換したり、インストーラーを作成・有効化したりはしていない。
