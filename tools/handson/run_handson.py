"""docs/246: the hands-on checklist (docs/105) run by the AI through MCP, into one report page.

    python -I tools/handson/run_handson.py [--mcp C:\\Apps\\SketchBrep\\sbr_mcp.exe] [--only 14-46,14-47]
                                           [--out folder]

Starts sbr_mcp (which starts the app with AI access on when it is not running, docs/244), and for
each scenario in scenarios.json: a new document, the steps (MCP tool calls), the checks the AI can
make (texts on the sheets, answers of the tools), and pictures (sheets, windows). Writes
report.html (pictures inside) and report.json in the out folder, and prints one JSON line.

The owner then looks only at what the AI cannot judge (the "look" line of each item) in the page.

Safety: the app's own work is never thrown away: if the app has unsaved changes at the start, the
run stops (save or close them first). The scenarios' own changes are discarded between scenarios.
"""
from __future__ import annotations

import argparse
import base64
import datetime as dt
import html
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


class Mcp:
    """A small MCP client over stdio (one JSON per line)."""

    def __init__(self, exe: str):
        self.proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                     cwd=str(Path(exe).parent))
        self.next_id = 1
        self.lock = threading.Lock()
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "sbr-handson", "version": "1"}})
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def send(self, message: dict) -> None:
        self.proc.stdin.write((json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8"))
        self.proc.stdin.flush()

    def request(self, method: str, params: dict) -> dict:
        with self.lock:
            rid = self.next_id
            self.next_id += 1
            self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
            while True:
                line = self.proc.stdout.readline()
                if not line:
                    raise RuntimeError("sbr_mcp ended")
                msg = json.loads(line.decode("utf-8"))
                if msg.get("id") == rid:
                    return msg

    def call(self, tool: str, args: dict) -> dict:
        """{'ok', 'result' (parsed JSON or text), 'text', 'images': [bytes]}"""
        msg = self.request("tools/call", {"name": tool, "arguments": args})
        res = msg.get("result", {})
        text = "".join(c.get("text", "") for c in res.get("content", []) if c.get("type") == "text")
        images = [base64.b64decode(c["data"]) for c in res.get("content", []) if c.get("type") == "image"]
        try:
            parsed = json.loads(text)
        except ValueError:
            parsed = text
        return {"ok": not res.get("isError", True) and "error" not in msg, "result": parsed, "text": text, "images": images}

    def close(self) -> None:
        try:
            self.proc.stdin.close()
            self.proc.wait(10)
        except Exception:
            self.proc.kill()


def lookup(saved: dict, path: str):
    parts = path.split(".")
    value = saved[parts[0]]
    for p in parts[1:]:
        value = value[int(p)] if isinstance(value, list) else value[p]
    return value


FILES = {"dir": ""}  # docs/255: the run's own folder for files the scenarios write ({tmp} in a string)


def substitute(value, saved: dict):
    """"$name.field.0" anywhere in the arguments: a value saved from an earlier step; "{tmp}" in a
    string: the run's own folder for files (export_*, save, open)."""
    if isinstance(value, str) and value.startswith("$"):
        return lookup(saved, value[1:])
    if isinstance(value, str) and "{tmp}" in value:
        return value.replace("{tmp}", FILES["dir"])
    if isinstance(value, list):
        return [substitute(v, saved) for v in value]
    if isinstance(value, dict):
        return {k: substitute(v, saved) for k, v in value.items()}
    return value


def settle(mcp: Mcp, seconds: float = 0.6) -> dict:
    time.sleep(seconds)
    return mcp.call("ui_windows", {})


def modal_of(windows: dict):
    for w in (windows.get("result") or {}).get("windows", []) if isinstance(windows.get("result"), dict) else []:
        if w.get("modal"):
            return w
    return None


DISCARD = ("Discard", "破棄", "保存しない", "Don't Save", "Close without Saving", "保存せずに閉じる")
CANCEL = ("Cancel", "キャンセル")


def new_document(mcp: Mcp, may_discard: bool) -> str | None:
    """A new document; None when done, else why not."""
    r = mcp.call("ui_trigger", {"action": "ファイル/新規"})
    if not r["ok"]:
        return r["text"]
    w = modal_of(settle(mcp))
    if not w:
        return None
    listed = mcp.call("ui_list", {"window": "modal"})
    buttons = [x for x in listed["result"].get("widgets", []) if x.get("kind") == "button"]
    wanted = DISCARD if may_discard else CANCEL
    for b in buttons:
        if b.get("text", "").replace("&", "") in wanted:
            mcp.call("ui_click", {"window": "modal", "widget": b["ref"]})
            settle(mcp)
            return None if may_discard else "the app has unsaved work: save it or close it, then run again"
    return f"a window '{w.get('title')}' is open and has no button to go on: {[b.get('text') for b in buttons]}"


def check(expect: dict, step_text: str, ok: bool) -> list[str]:
    """The AI's checks of one step; the list of what failed."""
    failed = []
    if expect.get("fails"):
        if ok:
            failed.append("expected to be refused, but it was done")
        return failed
    if not ok:
        failed.append("the tool failed: " + step_text[:300])
    for want in expect.get("contains", []):
        if want not in step_text:
            failed.append(f"「{want}」が出ていない")
    for unwanted in expect.get("not_contains", []):
        if unwanted in step_text:
            failed.append(f"「{unwanted}」が出ている")
    return failed


def snapshot(mcp: Mcp) -> dict:
    """docs/248: what the model gives, as the AI reads it: the tables, the stages, every sheet's words."""
    snap: dict = {}
    for tool, args in [("room_schedule", {}), ("opening_schedule", {}), ("area_table", {}), ("list_equipment", {}),
                       ("project_outline", {}), ("permit_checks", {"stage": 9}), ("permit_checks", {"stage": 1})]:
        r = mcp.call(tool, args)
        snap[f"{tool} {json.dumps(args, ensure_ascii=False)}"] = r["text"] if r["ok"] else "（失敗）" + r["text"]
    listing = mcp.call("render_drawing_set", {})
    sheets = listing["result"].get("sheets", []) if isinstance(listing["result"], dict) else []
    snap["sheets"] = [f'{s["number"]} {s["name"]}' for s in sheets]
    for s in sheets:
        r = mcp.call("render_drawing_set", {"sheet": s["index"], "texts": True})
        texts = (r["result"].get("sheet") or {}).get("texts", []) if isinstance(r["result"], dict) else []
        # The date in the title block is the day's: not the model.
        snap[f'sheet {s["name"]}'] = sorted(t for t in texts if not (len(t) == 10 and t[4] == "-" and t[7] == "-"))
    return snap


def roundtrip(mcp: Mcp, path: Path) -> list[str]:
    """Saved as a copy, opened again: the same tables, stages and words on every sheet."""
    before = snapshot(mcp)
    if path.exists():
        path.unlink()
    saved = mcp.call("save_copy", {"path": str(path)})
    if not saved["ok"]:
        return ["保存できない: " + saved["text"][:200]]
    opened = mcp.call("open", {"path": str(path)})
    if not opened["ok"]:
        return ["開けない: " + opened["text"][:200]]
    after = snapshot(mcp)
    try:
        path.unlink()
    except OSError:
        pass
    diffs = []
    for key in sorted(set(before) | set(after)):
        a, b = before.get(key), after.get(key)
        if a == b:
            continue
        if isinstance(a, list) and isinstance(b, list):
            gone = [x for x in a if x not in b][:5]
            new = [x for x in b if x not in a][:5]
            diffs.append(f"{key}：消えた {gone}　増えた {new}")
        else:
            diffs.append(f"{key}：前 {str(a)[:150]}　後 {str(b)[:150]}")
    return diffs


def run_scenario(mcp: Mcp, sc: dict, bases: dict, out: Path) -> dict:
    saved: dict = {}
    record = {"id": sc["id"], "title": sc["title"], "look": sc.get("look", ""), "docs": sc.get("docs", ""),
              "steps": [], "pictures": [], "failed": [], "findings": []}
    why = new_document(mcp, may_discard=True)
    if why:
        record["failed"].append("new document: " + why)
        return record
    steps = []
    for base in sc.get("base", []):
        steps += bases[base]
    steps += sc["steps"]
    for n, step in enumerate(steps, 1):
        if "wait" in step:
            time.sleep(step["wait"])
            continue
        if step.get("roundtrip"):
            diffs = roundtrip(mcp, Path(tempfile.gettempdir()) / f"sbr-handson-{os.getpid()}.sbr")
            record["steps"].append({"n": n, "tool": "save_copy → open（保存して開き直す）", "args": {}, "ok": not diffs,
                                    "failed": diffs, "answer": ""})
            record["failed"] += [f"step {n} 保存して開き直す: {d}" for d in diffs]
            continue
        tool = step["tool"]
        args = substitute(step.get("args", {}), saved)
        r = mcp.call(tool, args)
        text = r["text"]
        if tool == "render_drawing_set" and isinstance(r["result"], dict):
            sheet = r["result"].get("sheet") or {}
            text += "\n" + "\n".join(sheet.get("texts", []))
            # docs/247: what the sheet check points at on this sheet (to look at; not a failure).
            if sheet.get("name"):
                found = mcp.call("check_drawing_set", {"sheet": sheet["name"]})
                if found["ok"] and isinstance(found["result"], dict):
                    for s in found["result"].get("sheets", []):
                        for f in s.get("findings", []):
                            record["findings"].append(f'{s["name"]}：{f["kind"]}「{f["text"]}」{f.get("other", "")}')
        if "save" in step and isinstance(r["result"], dict):
            saved[step["save"]] = r["result"]
        failed = check(step.get("expect", {}), text, r["ok"])
        entry = {"n": n, "tool": tool, "args": args, "ok": r["ok"], "failed": failed,
                 "answer": text[:600] if (failed or step.get("show")) else ""}
        for i, img in enumerate(r["images"]):
            if step.get("picture"):
                name = f"{sc['id']}-{n}-{i}.png"
                (out / name).write_bytes(img)
                record["pictures"].append({"file": name, "caption": step["picture"]})
        if step.get("ui"):  # a window was opened by a press: let it come up
            settle(mcp, step.get("ui_wait", 0.8))
        record["steps"].append(entry)
        record["failed"] += [f"step {n} {tool}: {f}" for f in failed]
    # Close whatever window the scenario left waiting (Cancel), so the next one starts clean.
    for _ in range(3):
        w = modal_of(mcp.call("ui_windows", {}))
        if not w:
            break
        listed = mcp.call("ui_list", {"window": "modal"})
        for b in listed["result"].get("widgets", []):
            if b.get("kind") == "button" and b.get("text", "").replace("&", "") in CANCEL + DISCARD:
                mcp.call("ui_click", {"window": "modal", "widget": b["ref"]})
                break
        else:
            mcp.call("ui_key", {"window": "modal", "key": "Escape"})
        settle(mcp)
    return record


def write_report(records: list[dict], out: Path, started: str, mcp_exe: str) -> Path:
    rows = []
    for r in records:
        state = "AI の確認：合格" if not r["failed"] else "AI の確認：不合格"
        cls = "pass" if not r["failed"] else "fail"
        pics = "".join(
            f'<figure><img src="data:image/png;base64,{base64.b64encode((out / p["file"]).read_bytes()).decode()}" '
            f'alt="{html.escape(p["caption"])}"><figcaption>{html.escape(p["caption"])}</figcaption></figure>'
            for p in r["pictures"])
        fails = "".join(f"<li>{html.escape(f)}</li>" for f in r["failed"])
        finds = ""
        if r.get("findings"):
            finds = ("<div class='finds'><b>図面の点検の指摘（docs/247。読みにくいかもしれない所）</b><ul>"
                     + "".join(f"<li>{html.escape(x)}</li>" for x in sorted(set(r["findings"]))) + "</ul></div>")
        steps = "".join(
            f'<tr class="{"ng" if s["failed"] or not s["ok"] else ""}"><td>{s["n"]}</td><td>{html.escape(s["tool"])}</td>'
            f'<td>{"OK" if s["ok"] else "失敗"}</td><td>{html.escape("; ".join(s["failed"]))}'
            f'{("<pre>" + html.escape(s["answer"]) + "</pre>") if s["answer"] else ""}</td></tr>'
            for s in r["steps"])
        rows.append(f"""
<section class="item {cls}">
  <h2>{html.escape(r['id'])}　{html.escape(r['title'])}</h2>
  <p class="state">{state}　<span class="docs">{html.escape(r['docs'])}</span></p>
  {"<ul class='fails'>" + fails + "</ul>" if fails else ""}
  <div class="look"><b>オーナーが見る所（AI では決められない事）</b><br>{html.escape(r['look'])}</div>
  {finds}
  <div class="pics">{pics}</div>
  <details><summary>AI がした操作（{len(r['steps'])} 手）</summary><table>{steps}</table></details>
</section>""")
    passed = sum(1 for r in records if not r["failed"])
    page = f"""<!doctype html><html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>SketchBRep 確認レポート</title><style>
:root{{--bg:#fff;--fg:#1d1d1f;--muted:#666;--line:#ddd;--ok:#1a7f37;--ng:#c62828;--box:#f6f8fa}}
@media (prefers-color-scheme: dark){{:root{{--bg:#141414;--fg:#eee;--muted:#aaa;--line:#333;--ok:#4ac26b;--ng:#ff6b6b;--box:#1f1f1f}}}}
body{{background:var(--bg);color:var(--fg);font-family:system-ui,"Yu Gothic UI",sans-serif;margin:0 auto;max-width:1100px;padding:16px;line-height:1.6}}
h1{{font-size:1.4rem}} h2{{font-size:1.1rem;margin:0}} .item{{border:1px solid var(--line);border-radius:8px;padding:12px 16px;margin:16px 0}}
.pass .state{{color:var(--ok)}} .fail .state{{color:var(--ng)}} .fails{{color:var(--ng)}} .docs{{color:var(--muted);font-size:.9em}}
.finds{{color:var(--muted);font-size:.9em}} .look{{background:var(--box);padding:8px 12px;border-radius:6px;margin:8px 0}} figure{{margin:8px 0}} img{{max-width:100%;border:1px solid var(--line)}}
figcaption{{color:var(--muted);font-size:.9em}} table{{border-collapse:collapse;width:100%;font-size:.85em}} td{{border-top:1px solid var(--line);padding:4px;vertical-align:top}}
tr.ng td{{color:var(--ng)}} pre{{white-space:pre-wrap;margin:4px 0;font-size:.85em}}
</style></head><body>
<h1>SketchBRep 確認レポート</h1>
<p>{html.escape(started)}　AI の確認：{passed} / {len(records)} 項目が合格。sbr_mcp：{html.escape(mcp_exe)}<br>
各項目の「オーナーが見る所」だけを見て、docs/105 の結果の欄に書いてください。</p>
{''.join(rows)}
</body></html>"""
    path = out / "report.html"
    path.write_text(page, encoding="utf-8")
    (out / "report.json").write_text(json.dumps(records, ensure_ascii=False, indent=1), encoding="utf-8")
    return path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mcp", default=r"C:\Apps\SketchBrep\sbr_mcp.exe")
    ap.add_argument("--only", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--scenarios", default=str(HERE / "scenarios.json"))
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8")  # the JSON line has Japanese (a cp932 console cannot print it)
    except (AttributeError, ValueError):
        pass
    data = json.loads(Path(a.scenarios).read_text(encoding="utf-8"))
    only = {s.strip() for s in a.only.split(",") if s.strip()}
    scenarios = [s for s in data["scenarios"] if not only or s["id"] in only]
    started = dt.datetime.now().strftime("%Y-%m-%d %H:%M")
    out = Path(a.out) if a.out else Path(tempfile.gettempdir()) / "sbr-handson" / dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    (out / "files").mkdir(exist_ok=True)
    FILES["dir"] = str((out / "files").resolve())
    mcp = Mcp(a.mcp)
    try:
        # The app's own work first: never thrown away.
        first = mcp.call("ui_windows", {})
        if not first["ok"]:
            print(json.dumps({"ok": False, "error": first["text"]}, ensure_ascii=False))
            return 1
        why = new_document(mcp, may_discard=False)
        if why:
            print(json.dumps({"ok": False, "error": why}, ensure_ascii=False))
            return 1
        records = [run_scenario(mcp, sc, data.get("bases", {}), out) for sc in scenarios]
        new_document(mcp, may_discard=True)
    finally:
        mcp.close()
    report = write_report(records, out, started, a.mcp)
    print(json.dumps({"ok": True, "report": str(report), "items": len(records),
                      "passed": sum(1 for r in records if not r["failed"]),
                      "failed": {r["id"]: r["failed"] for r in records if r["failed"]}}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
