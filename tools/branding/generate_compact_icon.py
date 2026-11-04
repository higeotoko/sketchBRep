#!/usr/bin/env python3
"""Render the existing vector cube for small app icons (development only).

Requires CairoSVG and Pillow; neither is an application/build dependency.
The full logo and original artwork remain unchanged.
"""
import io
import json
from pathlib import Path
import xml.etree.ElementTree as ET

import cairosvg
from PIL import Image

ROOT = Path(__file__).resolve().parents[2] / "assets" / "branding"
SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", NS)
tree = ET.parse(ROOT / "03-app-icon/sketchbrep-app-icon.svg")
svg = tree.getroot()
svg.set("viewBox", "330 212 640 640")
svg.find(f"{{{NS}}}title").text = "SketchBRep compact app icon"
omit = {"arrow-right-gap", "extrusion-arrow-shaft", "extrusion-arrow-head"}
for parent in svg.iter():
    for node in list(parent):
        if node.get("id") in omit:
            parent.remove(node)
vector = ET.tostring(svg, encoding="utf-8", xml_declaration=True)
(ROOT / "03-app-icon/sketchbrep-app-icon-compact.svg").write_bytes(vector)


def render(size):
    return Image.open(io.BytesIO(cairosvg.svg2png(
        bytestring=vector, output_width=size, output_height=size))).convert("RGBA")


frames = [render(size) for size in SIZES]
frames[-1].save(ROOT / "05-windows/sketchbrep-compact.ico", format="ICO",
                sizes=[(s, s) for s in SIZES], append_images=frames[:-1])
frames[-1].save(ROOT / "03-app-icon/sketchbrep-app-icon-compact-256.png")
render(1024).save(ROOT / "03-app-icon/sketchbrep-app-icon-compact-1024.png")

report = []
ico = Image.open(ROOT / "05-windows/sketchbrep-compact.ico")
assert ico.info["sizes"] == {(s, s) for s in SIZES}
for size in SIZES:
    image = ico.ico.getimage((size, size)).convert("RGBA")
    bounds = image.getchannel("A").getbbox()
    assert bounds and bounds[2] - bounds[0] >= size * .85
    assert bounds[3] - bounds[1] >= size * .85
    assert image.getpixel((0, 0))[3] == 0
    report.append({"size": size, "content_bounds": bounds})
(ROOT / "compact-validation.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
