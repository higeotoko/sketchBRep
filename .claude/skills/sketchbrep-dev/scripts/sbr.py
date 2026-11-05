"""SketchBRep development helper: build / test / package / verify / ci / record, with short output.

Run from anywhere with the bundled Python, e.g.
  python -I .claude/skills/sketchbrep-dev/scripts/sbr.py test core --filter 'FoundationMat.*'
Full logs go to a temp folder; the summary prints their path when something fails.
Paths for this PC are in CONFIG; adjust them on another machine.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONFIG = {
    "repo": Path(os.environ.get("SBR_REPO", HERE.parents[3])),
    "cmake": Path(r"C:\Users\enomo\Documents\Codex\sbr-build-env\Library\bin\cmake.exe"),
    "vcvars": Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"),
    "dll_paths": [
        Path(r"C:\Users\enomo\Documents\Codex\2026-11-04\higeotoko-sketchbrep-claude-sketchbrep-modeler-spec\work\Library\bin"),
        Path(r"C:\Users\enomo\Documents\Codex\sbr-build-env\Library\bin"),
    ],
    # Delivery folder (not in Git): outputs\ for ZIPs and samples, work\ for extraction and temp.
    "delivery": Path(os.environ.get("SBR_DELIVERY_ROOT", r"C:\Users\enomo\Documents\Codex\2026-11-04\files-pasted-by-the-user-codex")),
    "runtime": Path(r"C:\Users\enomo\Documents\Codex\2026-11-04\higeotoko-sketchbrep-claude-sketchbrep-modeler-spec\work\timber-refinement-portable"),
    "dumpbin": Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\dumpbin.exe"),
    "github": "higeotoko/sketchBRep",
    "branch": "claude/sketchbrep-modeler-spec-4wnp0b",
}
DEFAULT_TARGETS = ["sbr_core_tests", "sbr_cli", "sbr_gui_tests", "SketchBRep", "sbr_mcp", "sbr_phase0"]
LOGS = Path(tempfile.gettempdir()) / "sbr-logs"
LOGS.mkdir(exist_ok=True)


def decode(data: bytes) -> str:
    for enc in ("utf-8", "cp932"):
        try:
            return data.decode(enc)
        except UnicodeDecodeError:
            pass
    return data.decode("utf-8", errors="replace")


def env_with_dlls() -> dict:
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join(str(p) for p in CONFIG["dll_paths"]) + os.pathsep + env.get("PATH", "")
    return env


def run(cmd, log: Path, cwd=None, env=None, timeout=None) -> tuple[int, str]:
    proc = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    text = decode(proc.stdout)
    log.write_text(text, encoding="utf-8")
    return proc.returncode, text


def head(short=True) -> str:
    args = ["git", "rev-parse", "--short=7" if short else "--verify", "HEAD"]
    return subprocess.run(args, cwd=CONFIG["repo"], capture_output=True, text=True).stdout.strip()


# --- build ---------------------------------------------------------------------------
def build(targets: list[str]) -> int:
    targets = targets or DEFAULT_TARGETS
    script = LOGS / "build.cmd"
    dlls = ";".join(str(p) for p in CONFIG["dll_paths"])
    script.write_text(
        "@echo off\r\n"
        f'call "{CONFIG["vcvars"]}" >nul\r\n'
        "if errorlevel 1 exit /b 1\r\n"
        f'set "PATH={dlls};%PATH%"\r\n'
        f'"{CONFIG["cmake"]}" --build "{CONFIG["repo"] / "build"}" --target {" ".join(targets)} -j 4\r\n',
        encoding="ascii",
    )
    log = LOGS / "build.log"
    code, text = run(["cmd", "/c", str(script)], log)
    errors = [l for l in text.splitlines() if re.search(r"error C\d+|: error|error LNK|FAILED:", l)]
    if code == 0 and not errors:
        print(f"build ok ({' '.join(targets)})")
        return 0
    print(f"build FAILED (exit {code}); log: {log}")
    for line in errors[:15]:
        print("  " + line.strip()[:300])
    return 1


# --- tests ---------------------------------------------------------------------------
def test_core(filter_: str | None) -> int:
    exe = CONFIG["repo"] / "build" / "tests" / "sbr_core_tests.exe"
    log = LOGS / "core.log"
    tmp = CONFIG["delivery"] / "work" / "test-temp"
    tmp.mkdir(parents=True, exist_ok=True)
    env = env_with_dlls()
    env["TEMP"] = env["TMP"] = str(tmp)
    cmd = [str(exe), "--gtest_brief=1"] + ([f"--gtest_filter={filter_}"] if filter_ else [])
    code, text = run(cmd, log, env=env, timeout=1800)
    lines = text.splitlines()
    for l in lines:
        if re.match(r"\[(==========|  PASSED  |  SKIPPED )\]", l) and ("ran" in l or "test" in l):
            print(l.strip())
    failed = [l for l in lines if l.startswith("[  FAILED  ]")]
    if failed:
        print(f"FAILED ({len(failed)}); log: {log}")
        for i, l in enumerate(lines):
            if re.search(r"\): error|Failure$", l):
                print("  " + " | ".join(x.strip() for x in lines[i:i + 5])[:400])
        for l in failed[:20]:
            print("  " + l.strip())
    return 0 if code == 0 else 1


def test_gui(names: list[str], match: str | None, scale1x: bool) -> int:
    exe = CONFIG["repo"] / "build" / "tests" / "sbr_gui_tests.exe"
    env = env_with_dlls()
    if scale1x:
        env["QT_SCALE_FACTOR"] = "0.5714"
    if match:
        listing = decode(subprocess.run([str(exe), "-functions"], env=env, capture_output=True).stdout)
        names += [n.strip().rstrip("()") for n in listing.splitlines() if n.strip() and re.search(match, n)]
    if not names:
        print("no GUI test selected")
        return 1
    out = LOGS / "gui.txt"
    code, _ = run([str(exe), *names, "-o", f"{out},txt"], LOGS / "gui.stdout.log", cwd=exe.parent, env=env, timeout=1800)
    text = decode(out.read_bytes()) if out.exists() else ""
    lines = text.splitlines()
    passed = sum(l.startswith("PASS") for l in lines)
    for l in lines:
        if l.startswith("Totals"):
            print(l.strip())
    for i, l in enumerate(lines):
        if l.startswith("FAIL!"):
            detail = [l.strip()] + [x.strip() for x in lines[i + 1:i + 4] if x.startswith((" ", "C:", "/"))]
            print("  " + " | ".join(detail)[:400])
    if code:
        print(f"GUI exit {code}; log: {out}  (175% display and clipboard can cause local-only failures; CI decides)")
    else:
        print(f"GUI ok ({passed - 2 if passed >= 2 else passed} conditions)")
    return 0 if code == 0 else 1


# --- package / verify / record ---------------------------------------------------------
def package() -> int:
    sha = head()
    if build(["SketchBRep", "sbr_cli", "sbr_mcp", "sbr_phase0"]):
        return 1
    d = CONFIG["delivery"]
    zip_path = d / "outputs" / f"SketchBRep-windows-x64-{sha}-local.zip"
    if zip_path.exists():
        print(f"ZIP already exists (not overwritten): {zip_path}")
        return 1
    cmd = [sys.executable, "-I", str(CONFIG["repo"] / "tools" / "package_windows_local.py"), "--runtime", str(CONFIG["runtime"]),
           "--build", "build", "--stage", str(d / "work" / f"stage-{sha}"), "--output", str(zip_path), "--dumpbin", str(CONFIG["dumpbin"])]
    code, text = run(cmd, LOGS / "package.log", cwd=CONFIG["repo"])
    if code:
        print(f"package FAILED; log: {LOGS / 'package.log'}")
        return 1
    info = json.loads([l for l in text.splitlines() if l.startswith("{")][-1])
    print(f"ZIP {info['zip']}  {info['bytes']:,} bytes  sha256 {info['sha256']}")
    print(f"  dlls {info['runtime_dlls_verified']} / deps {info['binary_dependencies_checked']}  -> {zip_path}")
    return 0


def verify() -> int:
    sha = head()
    env = os.environ.copy()
    env["SBR_DELIVERY_ROOT"] = str(CONFIG["delivery"])
    env["SBR_REPO"] = str(CONFIG["repo"])
    env["PYTHONIOENCODING"] = "utf-8"
    ok = 0
    for name in ("verify_portable_pads.py", "verify_portable_mat.py"):
        log = LOGS / f"{name}.log"
        code, text = run([sys.executable, "-I", str(HERE / name), sha], log, env=env, timeout=1800)
        record = [l for l in text.splitlines() if l.startswith("{")]
        if code or not record:
            print(f"{name} FAILED; log: {log}")
            print("  " + " | ".join(text.strip().splitlines()[-3:])[:400])
            ok = 1
            continue
        r = json.loads(record[-1])
        keep = {k: v for k, v in r.items() if k in ("gui_actual_conditions", "gui_failures", "check_structure", "structure_markers",
                                                     "save_reopen_status", "slab_m3", "stem_m3", "edge_footing_m3", "ground_beam_m3")}
        print(f"{name}: ok {json.dumps(keep, ensure_ascii=False)}")
    return ok


def record() -> int:
    sha = head()
    out = CONFIG["delivery"] / "outputs"
    day = dt.date.today().isoformat()
    dest = CONFIG["repo"] / "docs" / "validation"
    for src, suffix in ((f"portable-{sha}-verification.json", ""), (f"portable-{sha}-mat-verification.json", "-mat")):
        if (out / src).exists():
            shutil.copy2(out / src, dest / f"{day}-portable-{sha}{suffix}.json")
            print(f"copied docs/validation/{day}-portable-{sha}{suffix}.json")
        else:
            print(f"missing {src} (run verify first)")
    zip_path = out / f"SketchBRep-windows-x64-{sha}-local.zip"
    if zip_path.exists():
        sha256 = (out / (zip_path.name + ".sha256")).read_text(encoding="utf-8").split()[0]
        print(f"| コード | `{head(False)}` |\n| ZIP | `{zip_path.name}`（Codex のチャットフォルダの `outputs\\`） |\n"
              f"| サイズ | {zip_path.stat().st_size:,} bytes |\n| SHA256 | `{sha256}` |")
    return 0


# --- CI --------------------------------------------------------------------------------
def ci(wait: bool) -> int:
    sha = head(False)
    gh = ["gh", "run", "list", "-R", CONFIG["github"], "--commit", sha, "--json", "databaseId,event,status,conclusion"]
    while True:
        runs = json.loads(subprocess.run(gh, capture_output=True, text=True).stdout or "[]")
        if not runs:
            print(f"no CI run for {sha[:7]} (docs-only push, or not pushed yet)")
            return 1
        r = runs[0]
        if r["status"] == "completed" or not wait:
            break
        time.sleep(30)
    print(f"run {r['databaseId']} ({r['event']}) {r['status']} {r.get('conclusion') or ''}")
    if r["status"] != "completed":
        return 0
    log = decode(subprocess.run(["gh", "run", "view", str(r["databaseId"]), "-R", CONFIG["github"], "--log"], capture_output=True).stdout)
    seen = set()
    for l in log.splitlines():
        m = re.search(r"(Totals:.*|\d+% tests passed.*|The following tests FAILED.*|\*\*\*Failed.*|\*\*\*Exception.*|SEGFAULT.*|FAIL!.*)", l)
        if m and "36;1m" not in l:
            job = l.split("\t", 1)[0]
            text = f"{job}: {m.group(1)[:200]}"
            if text not in seen:
                seen.add(text)
                print("  " + text)
    return 0 if r.get("conclusion") == "success" else 1


def main() -> int:
    p = argparse.ArgumentParser(prog="sbr")
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build"); b.add_argument("targets", nargs="*")
    t = sub.add_parser("test"); t.add_argument("kind", choices=["core", "gui"]); t.add_argument("names", nargs="*")
    t.add_argument("--filter"); t.add_argument("--match"); t.add_argument("--scale1x", action="store_true")
    sub.add_parser("package"); sub.add_parser("verify"); sub.add_parser("record")
    c = sub.add_parser("ci"); c.add_argument("--wait", action="store_true")
    a = p.parse_args()
    if a.cmd == "build":
        return build(a.targets)
    if a.cmd == "test":
        return test_core(a.filter) if a.kind == "core" else test_gui(a.names, a.match, a.scale1x)
    return {"package": package, "verify": verify, "record": record}[a.cmd]() if a.cmd != "ci" else ci(a.wait)


if __name__ == "__main__":
    sys.exit(main())
