#!/usr/bin/env python3
"""Converts the Steel Member Generator section data into SketchBRep's
built-in catalog (docs/41).

  python3 tools/import_steel_catalog.py <steel-member-generator>/src/higemegane_steel/data \
      src/core/data/steel_sections.json --commit <sha> --version <x.y.z>

Each section keeps its id, name, family, standard, subtype and nominal
dimensions, and its exact outline: closed loops of [x, y, bulge] (bulge =
tan(sweep / 4), as in DXF), x across and y up, moved so that the middle of
the outline's exact bounding box (arcs included) is the origin. Nothing is
rescaled or rounded beyond 1e-9 mm, and the loops are checked to close.
"""

import argparse
import glob
import json
import math
import os
import sys


def arc_points(a, b, bulge):
    """The arc's centre, radius, start angle and signed sweep."""
    sweep = 4 * math.atan(bulge)
    chord = math.dist(a, b)
    radius = chord / (2 * abs(math.sin(sweep / 2)))
    mx, my = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
    # Sagitta point: right of the chord for a positive (counter-clockwise) bulge.
    px, py = mx + (b[1] - a[1]) * bulge / 2, my - (b[0] - a[0]) * bulge / 2
    # Centre on the chord's bisector, `radius` away from the sagitta point.
    dx, dy = mx - px, my - py
    d = math.hypot(dx, dy)
    cx, cy = px + dx / d * radius, py + dy / d * radius
    start = math.atan2(a[1] - cy, a[0] - cx)
    return (cx, cy), radius, start, sweep


def loop_bounds(loop):
    xs, ys = [], []
    n = len(loop)
    for i, (x, y, bulge) in enumerate(loop):
        xs.append(x)
        ys.append(y)
        if bulge == 0:
            continue
        nx, ny, _ = loop[(i + 1) % n]
        (cx, cy), r, start, sweep = arc_points((x, y), (nx, ny), bulge)
        # The axis directions the arc passes through.
        for k in range(-8, 9):
            angle = k * math.pi / 2
            t = (angle - start) / sweep if sweep else -1
            if 0 < t < 1:
                xs.append(cx + r * math.cos(angle))
                ys.append(cy + r * math.sin(angle))
    return min(xs), min(ys), max(xs), max(ys)


def loops_of(profile):
    if "vertices_y_z_bulge" in profile:  # the H data: one loop
        return [profile["vertices_y_z_bulge"]], profile.get("closed", True)
    loops = [profile["outer_loop"]["vertices_y_z_bulge"]]
    loops += [inner["vertices_y_z_bulge"] for inner in profile.get("inner_loops", [])]
    closed = profile["outer_loop"].get("closed", True) and all(
        inner.get("closed", True) for inner in profile.get("inner_loops", []))
    return loops, closed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("data_dir")
    parser.add_argument("output")
    parser.add_argument("--commit", required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()

    sections = []
    seen = set()
    for path in sorted(glob.glob(os.path.join(args.data_dir, "*_sections.json"))):
        data = json.load(open(path, encoding="utf-8"))
        if data.get("units", "mm") != "mm":
            sys.exit(f"{path}: units must be mm")
        for s in data["sections"]:
            loops, closed = loops_of(s["profile"])
            if not closed or any(len(loop) < 2 for loop in loops):
                sys.exit(f"{s['id']}: open or empty outline")
            if s["id"] in seen:
                sys.exit(f"{s['id']}: duplicate id")
            seen.add(s["id"])
            x0, y0, x1, y1 = loop_bounds(loops[0])
            cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
            moved = [[[round(x - cx, 9), round(y - cy, 9), bulge] for x, y, bulge in loop] for loop in loops]
            sections.append({
                "id": s["id"],
                "name": s.get("name", s["id"]),
                "family": s.get("family", s.get("section_type", "")),
                "standard": s.get("standard", ""),
                "subtype": s.get("subtype", ""),
                "dims": s.get("dimensions_mm", {}),
                "width": round(x1 - x0, 9),
                "depth": round(y1 - y0, 9),
                "loops": moved,
                "file": os.path.basename(path),
            })
    out = {
        "source": {
            "repository": "higeotoko/steel-member-generator",
            "version": args.version,
            "commit": args.commit,
            "note": "outlines as published there (from the DXF sources listed in each data file); "
                    "centred on their bounding box",
        },
        "units": "mm",
        "sections": sections,
    }
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, separators=(",", ":"))
    print(f"{len(sections)} sections -> {args.output}")


if __name__ == "__main__":
    main()
