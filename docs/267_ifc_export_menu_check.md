# 267. IFC書き出しメニューと完了時の案内（2026-11-23）

[チェック表](105_hands_on_checklist.md)の7-1をGUI自動確認した。「ファイル > エクスポート」のファイル形式にIFCがあり、保存後に内容と注意が表示されるところまでを、`ifcExportFromMenuShowsWhatWasAssumed`で確認する。テストは専用の設定と一時フォルダを使う。

- **GUI自動確認**では、レベルがないモデルに壁を1枚作り、IFCを選んで日本語名を入力した。`家.ifc`にIFC4の壁が入り、完了ダイアログに「壁 1」と、レベルがないため階を仮定した旨が出た。Windowsのファイル入出力GUIテストは10件成功。展開したZIPのDLLでもIFCのテストが成功し、通常18件・べた基礎4件のGUI検証も通った。
- **ファイルの確認**では、配布版`ec28ef8`のCLIで例題AをIFC4に書き出し、2階、壁12、開口14、窓8、扉6、室6が入ったことを確認した。配布フォルダの`outputs/ifc-menu-check/例題A_IFC4.ifc`。
- **Windowsコア**は810件成功、8件スキップ。**Linux CI**は831件すべて成功。[run 45079337444](https://github.com/higeotoko/sketchBRep/actions/runs/45079337444)
- **人によるWindows実機確認**は未実施。他のBIMソフトで階・部品の見え方を確かめる7-2と、庇を確かめる7-3はチェック表に残す。

## 配布

| 項目 | 値 |
|---|---|
| コード | `2eb59bb` |
| ZIP | `SketchBRep-windows-x64-2eb59bb-local.zip` |
| サイズ | 83,293,358 bytes |
| SHA256 | `ee4db8114112f1a0b20f7e40993c6af2632a550ff26d67e62b7e4e69983631d8` |
| ZIPの検証 | [通常](validation/2026-11-23-portable-2eb59bb.json)・[べた基礎](validation/2026-11-23-portable-2eb59bb-mat.json) |
