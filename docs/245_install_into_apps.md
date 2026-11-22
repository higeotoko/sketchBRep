# 245. ZIP を C:\Apps\SketchBrep に入れ替える道具（2026-11-22）

オーナーの決め事（2026-11-22）：「B 案で、最新の ZIP の中身で Apps の中のフォルダを更新する」。MCP の登録（`C:\Apps\SketchBrep\sbr_mcp.exe`）はそのまま、中身を入れ替える。今まではオーナーが手で、残った sbr_mcp を `taskkill /F /T /IM sbr_mcp.exe` で止めてから入れ替えていた（docs/243）。

## 道具

- `tools/install_windows_local.py --zip <ZIP> [--dest C:\Apps\SketchBrep] [--close-app]`（オーナーも使える）
- `sbr.py install [--close-app]`（開発用。HEAD の ZIP を `C:\Apps\SketchBrep` へ。置き場所は `SBR_INSTALL_DEST` で変えられる）

## すること

1. ZIP を `.sha256` と照らす（ある時）。
2. フォルダの中の exe から動いているプロセスを見る。
   - sbr_mcp.exe など（作業を持たない物）：止める。
   - SketchBRep.exe（アプリ）：`--close-app` の時だけ、閉じるボタンと同じに閉じてもらう（保存していない作業があれば、アプリが聞く）。20 秒で閉じなければ、何も変えずにやめる。`--close-app` が無ければ、何も変えずに「閉じてください」と返す。
3. ZIP をフォルダの隣に展開し、古いフォルダを `SketchBrep.old-日時` に移してから、新しいのを入れる。途中で失敗したら古いのを戻す。`.old-` は新しい 3 つを残し、それより古い物（この道具が作った物）は消す。
4. 新しい sbr_mcp.exe が、アプリ無し（`--headless`）で MCP の短いやり取り（initialize・tools/list・create_box）に答えるか確かめる。
5. 結果を JSON 1 行で出す（止めたプロセス、古いフォルダの場所、確かめた結果）。

## 確認（Windows、2026-11-22、作業用のフォルダで）

- 空の所へ de01f12 の ZIP を入れる：SHA256 一致、MCP の答え 3 つ、ツール 208、create_box 成功。
- 直す前の sbr_mcp（docs/243 の前の物）を、そのフォルダから「入力の終わりが来ない」形で動かしたまま入れる：その sbr_mcp を止め（`ended: sbr_mcp.exe <番号>`）、古いのを `SketchBrep.old-20261122-120349` に移し、新しいのが答えた。

**Windows 実機確認**：最新の ZIP を作った後、`C:\Apps\SketchBrep` へ実際に入れる（AI がやり、結果を報告する）。
