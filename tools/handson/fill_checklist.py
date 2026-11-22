"""docs/258: write the AI's results of a hands-on run into the checklist (docs/105).

    python -I tools/handson/fill_checklist.py <report.json> [--date 2026-11-22]

For each checklist item a scenario covers (its id such as "14-6・14-16" names the items), the
"結果" column gets "AI 確認 <date> 合格/不合格（台本 …）。オーナーが見る所：…". A result the owner
wrote (anything not starting with "AI 確認") is never touched; an earlier AI result is replaced.
Prints how many rows were written and the items still without a result.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def items_of(scenario_id: str) -> list[str]:
    out, prefix = [], None
    for part in scenario_id.split("・"):
        part = part.strip()
        m = re.match(r"^(\d+)-(\d+)[a-z]?$", part)
        if m:
            prefix = m.group(1)
            out.append(f"{m.group(1)}-{m.group(2)}")
        elif re.match(r"^\d+$", part) and prefix:
            out.append(f"{prefix}-{part}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("report")
    ap.add_argument("--date", default="2026-11-22")
    ap.add_argument("--checklist", default=str(ROOT / "docs" / "105_hands_on_checklist.md"))
    ap.add_argument("--scenarios", default=str(ROOT / "tools" / "handson" / "scenarios.json"))
    a = ap.parse_args()
    records = {r["id"]: r for r in json.loads(Path(a.report).read_text(encoding="utf-8"))}
    scenarios = json.loads(Path(a.scenarios).read_text(encoding="utf-8"))["scenarios"]
    cover: dict[str, list[dict]] = {}
    for s in scenarios:
        if s["id"] in records:
            for item in items_of(s["id"]):
                cover.setdefault(item, []).append(s)
    raw = Path(a.checklist).read_text(encoding="utf-8")
    nl = "\r\n" if "\r\n" in raw else "\n"
    lines = raw.split(nl)
    written, empty = 0, []
    for k, line in enumerate(lines):
        m = re.match(r"^\| (\d+-\d+) \|", line)
        if not m:
            continue
        cells = line.split("|")
        if len(cells) < 6:
            continue
        current = cells[-2].strip()
        item = m.group(1)
        if item not in cover:
            if not current:
                empty.append(item)
            continue
        if current and not current.startswith("AI 確認"):
            continue  # the owner's
        ss = cover[item]
        ok = all(not records[s["id"]]["failed"] for s in ss)
        names = "・".join("「" + s["id"] + "」" for s in ss)
        look = " / ".join(s.get("look", "") for s in ss).replace("|", "／")
        cells[-2] = (f" AI 確認 {a.date} {'合格' if ok else '不合格'}（台本 {names}、report は docs/validation/handson-{a.date}）。"
                     f"オーナーが見る所：{look} ")
        lines[k] = "|".join(cells)
        written += 1
    Path(a.checklist).write_text(nl.join(lines), encoding="utf-8", newline="")
    print(json.dumps({"written": written, "still_empty": len(empty), "empty": empty}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
