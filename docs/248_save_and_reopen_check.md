# 248. 保存して開き直しても同じか、を AI が確かめる（2026-11-22）

docs/241〜247 の続き（5 番目）。docs/105 の「保存して開き直す」（14-25 など）を、台本（docs/246）の 1 手にした。

## 道具

台本の手 `{"roundtrip": true}`（`tools/handson/run_handson.py`）：

1. 今のモデルから読める物を集める：室の表・建具表・面積の表・設備・計画概要・工程 1 と 9 の照合、図面一式のシートの一覧と、全シートの字（`render_drawing_set` の `texts`。表題欄の日付は、その日の日付なので除く）。
2. 一時フォルダへ `save_copy` → `open`。
3. もう一度集めて比べる。違いは「消えた／増えた字」「前／後」としてレポートに出し、不合格にする。

台本「保存・開き直し」：家の見本に、壁・床・天井の断熱、窓の U・η、設備、計画概要、天井、付近見取図（見本の座標）を入れてから保存して開き直す。

## 結果（2026-11-22、本番の `C:\Apps\SketchBrep` に入れた 641870a で）

- 台本 8 項目すべて AI の確認で合格（`docs/validation/handson-2026-11-22/report.json`）。保存して開き直しても、表・工程・17 枚の図面の字は全部同じ。
- 台本が sbr_mcp からアプリを起動し（docs/244）、終わった後に sbr_mcp は残っていない（docs/243）。

## 配布（641870a）

| 項目 | 内容 |
|---|---|
| CI | run 44983655450（push）成功。Linux 825 件すべて成功 |
| ZIP | `SketchBRep-windows-x64-641870a-local.zip`（83,108,725 bytes、SHA256 `afd95ecef7a5fa7c0154e16ca7ef88f479dc93b9efe95f72cf00b495a56a8dad`） |
| ZIP の検証 | `docs/validation/2026-11-22-portable-641870a.json`・`-mat.json`（GUI 18 + 4 条件、失敗 0） |
| `C:\Apps\SketchBrep` へ | `sbr.py install` で入れた（古いのは `C:\Apps\SketchBrep.old-20261122-131149`）。新しい sbr_mcp：ツール 209、create_box 成功 |
