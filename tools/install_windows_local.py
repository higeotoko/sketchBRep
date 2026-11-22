"""docs/245: put a portable ZIP's app into the folder the MCP client starts it from (Windows).

    python -I tools/install_windows_local.py --zip SketchBRep-windows-x64-<sha>-local.zip [--dest C:\\Apps\\SketchBrep]
                                             [--close-app]

1. Checks the ZIP against its .sha256 file when there is one.
2. Ends the sbr_mcp.exe processes started from the folder (they hold its files; they keep no
   work). A SketchBRep.exe started from the folder is asked to close only with --close-app (as
   the window's close button does: it asks to save unsaved work); otherwise nothing is changed.
3. Unpacks the ZIP next to the folder, moves the old folder aside as <dest>.old-<time> (the three
   newest are kept, older ones made by this tool are removed), and moves the new one in. When a
   step fails, the old folder is put back.
4. Checks the new sbr_mcp.exe: it answers a short MCP session on its own (--headless) and lists
   the app's tools.

Prints one JSON line with what was done; exit code 0 when the new app is in place and answers.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path


def processes_in(folder: Path) -> list[dict]:
    """Running processes whose program is inside `folder` (Windows)."""
    script = ("Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath } | "
              "Select-Object ProcessId,Name,ExecutablePath | ConvertTo-Json -Compress")
    out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                         capture_output=True, text=True, encoding="utf-8", errors="replace").stdout.strip()
    if not out:
        return []
    rows = json.loads(out)
    if isinstance(rows, dict):
        rows = [rows]
    root = str(folder.resolve()).lower().rstrip("\\") + "\\"
    return [r for r in rows if str(r.get("ExecutablePath", "")).lower().startswith(root)]


def wait_gone(pids: list[int], seconds: float) -> bool:
    end = time.time() + seconds
    while time.time() < end:
        alive = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True,
                               encoding="utf-8", errors="replace").stdout
        if not any(f'"{pid}"' in alive for pid in pids):
            return True
        time.sleep(0.5)
    return False


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def check_mcp(exe: Path) -> dict:
    session = "\n".join(json.dumps(m) for m in [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize",
         "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "install", "version": "1"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
         "params": {"name": "create_box", "arguments": {"origin": [0, 0, 0], "width": 1000, "depth": 1000, "height": 1000}}},
    ]) + "\n"
    out = subprocess.run([str(exe), "--headless"], input=session, capture_output=True, text=True,
                         encoding="utf-8", errors="replace", timeout=120, cwd=str(exe.parent)).stdout
    answers = [json.loads(line) for line in out.splitlines() if line.strip()]
    tools = next((a["result"]["tools"] for a in answers if a.get("id") == 2 and "result" in a), [])
    box = next((a for a in answers if a.get("id") == 3), {})
    return {"answers": len(answers), "tools": len(tools),
            "box_ok": bool(box.get("result")) and not box["result"].get("isError", True)}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--zip", required=True)
    ap.add_argument("--dest", default=r"C:\Apps\SketchBrep")
    ap.add_argument("--close-app", action="store_true")
    a = ap.parse_args()
    zip_path, dest = Path(a.zip).resolve(), Path(a.dest)
    report: dict = {"zip": str(zip_path), "dest": str(dest)}

    def done(ok: bool, **more) -> int:
        report.update(more, ok=ok)
        print(json.dumps(report, ensure_ascii=False))
        return 0 if ok else 1

    if not zip_path.is_file():
        return done(False, error="no such ZIP")
    sums = zip_path.with_name(zip_path.name + ".sha256")
    if sums.is_file():
        want = sums.read_text(encoding="utf-8").split()[0].lower()
        if sha256(zip_path) != want:
            return done(False, error="the ZIP does not match its .sha256")
        report["sha256"] = want

    # 2. What runs from the folder.
    if dest.exists():
        running = processes_in(dest)
        apps = [r for r in running if r["Name"].lower() == "sketchbrep.exe"]
        if apps and not a.close_app:
            return done(False, error="SketchBRep is open from the folder: close it (or pass --close-app)",
                        running=[r["ProcessId"] for r in apps])
        for r in apps:  # the close button: unsaved work is asked about
            subprocess.run(["taskkill", "/PID", str(r["ProcessId"])], capture_output=True)
        if apps and not wait_gone([r["ProcessId"] for r in apps], 20):
            return done(False, error="SketchBRep did not close (unsaved work?): nothing was changed")
        servers = [r for r in processes_in(dest) if r["Name"].lower() != "sketchbrep.exe"]
        for r in servers:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(r["ProcessId"])], capture_output=True)
        if servers and not wait_gone([r["ProcessId"] for r in servers], 10):
            return done(False, error="processes from the folder did not end: nothing was changed")
        report["ended"] = [f'{r["Name"]} {r["ProcessId"]}' for r in apps + servers]

    # 3. Unpack, swap, keep the old folder aside.
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    staging = Path(tempfile.mkdtemp(prefix="sbr-install-", dir=str(dest.parent)))
    old = dest.with_name(f"{dest.name}.old-{stamp}")
    try:
        with zipfile.ZipFile(zip_path) as z:
            z.extractall(staging)
        inner = staging / "SketchBRep"
        if not (inner / "sbr_mcp.exe").is_file():
            inner = staging
        if not (inner / "sbr_mcp.exe").is_file() or not (inner / "SketchBRep.exe").is_file():
            shutil.rmtree(staging, ignore_errors=True)
            return done(False, error="the ZIP has no SketchBRep.exe / sbr_mcp.exe: nothing was changed")
        if dest.exists():
            dest.rename(old)
            report["old"] = str(old)
        try:
            inner.rename(dest)
        except OSError as e:
            if old.exists():
                old.rename(dest)
            raise e
    finally:
        shutil.rmtree(staging, ignore_errors=True)

    # Keep the three newest old folders made by this tool.
    olds = sorted(dest.parent.glob(f"{dest.name}.old-*"), key=lambda p: p.name, reverse=True)
    removed = []
    for p in olds[3:]:
        shutil.rmtree(p, ignore_errors=True)
        removed.append(p.name)
    if removed:
        report["removed_old"] = removed

    # 4. The new server answers.
    check = check_mcp(dest / "sbr_mcp.exe")
    report["check"] = check
    return done(check["box_ok"] and check["tools"] > 0)


if __name__ == "__main__":
    sys.exit(main())
