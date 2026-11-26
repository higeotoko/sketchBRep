"""Prepare the exact Yokohama Word forms supported by SketchBRep (standard library only)."""

import argparse
import hashlib
import io
from pathlib import Path
from difflib import SequenceMatcher
import re
import struct
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape, unescape
import zipfile


SECOND = "3f19f0666d8234d41f4cf7740c207d8a567316a019000340298ff95b8a40c7c5"
THIRD = "6cd92ac9e8236af3e3ac13c8d89ef88cc6e1691232724df56e8a2d5a5f6f1b00"
# 横浜市 建築協定等手続状況届出書 第１号様式 (0766_20240329.docx, taken 2026-11-26)
AGREEMENT = "42fd2f928d26ab87e86ec7bf1bcf17e1446e56d1500e5b7e9cf8ac33fc4e816f"
W = "{http://schemas.openxmlformats.org/wordprocessingml/2006/main}"


def mark(key):
    return "@@SBR_" + key + "@@"


def patch_document(xml, third):
    # Keep the source XML outside the targeted runs byte-for-byte, including compatibility
    # namespaces. The known templates do not nest paragraphs before the last targeted index.
    spans = list(re.finditer(r"<w:p(?:\s[^>]*)?>(?:(?!<w:p(?:\s|>)).)*?</w:p>", xml, re.S))
    edits = {}

    def edit(index, transform):
        edits.setdefault(index, []).append(transform)

    def after(index, label, key):
        def add(text):
            start = text.index(label) + len(label)
            return text[:start] + mark(key) + text[start:]
        edit(index, add)

    def boxes(index, keys):
        def select(text):
            keys_iter = iter(keys)
            result, count = re.subn("□", lambda _: mark("check_" + next(keys_iter)), text)
            assert count == len(keys)
            return result
        edit(index, select)

    def slots(index, keys):
        def insert(text):
            positions = list(re.finditer(r"\( +\)", text))
            for position, key in reversed(list(zip(positions, keys, strict=False))):
                if key:
                    start, end = position.start() + 1, position.end() - 1
                    text = text[:start] + mark(f"slot_{key}__{end - start}") + text[end:]
            assert len(positions) >= len(keys)
            return text
        edit(index, insert)

    def date(index, key):
        edit(index, lambda text: re.sub(r"([ 　]+年[ 　]+月[ 　]+日)$",
                                      lambda m: mark("date_" + key), text, count=1))

    def registration(index, keys):
        def insert(text):
            spaces = list(re.finditer(r"(?<=[（(])[ 　]+(?=[）)])|(?<=登録第)[ 　]+(?=号)", text))
            assert len(spaces) == len(keys), (index, text)
            for region, key in reversed(list(zip(spaces, keys, strict=True))):
                width = sum(2 if char == "　" else 1 for char in region[0])
                text = text[:region.start()] + mark(f"slot_{key}__{width}") + text[region.end():]
            return text
        edit(index, insert)

    contacts = {}
    client_start = 4 if third else 57
    for delta, label, key in [(0, "】", "kana"), (2, "郵便番号】", "postcode"), (3, "住所】", "address")]:
        after(client_start + delta, label, "client_" + key)
    if not third:
        after(61, "電話番号】", "client_phone")
        after(22, "設計者氏名　", "designer_name")
        contacts[214] = "project_name"
    for role, index in zip(("agent", "designer", "supervisor"), (10, 19, 110) if third else (63, 72, 163)):
        registration(index, [role + "_grade", role + "_authority", role + "_number"])
        after(index + 1, "氏名】", role + "_name")
        registration(index + 2, [role + "_office_grade", role + "_office_prefecture", role + "_office_number"])
        contacts[index + 3] = role + "_office"
        for delta, label, key in [(4, "郵便番号】", "postcode"), (5, "所在地】", "address"), (6, "電話番号】", "phone")]:
            after(index + delta, label, role + "_" + key)
    contractor = 146 if third else 199
    after(contractor, "氏名】", "contractor_name")
    contacts[contractor + 2] = "contractor_office"
    for delta, label, key in [(3, "郵便番号】", "postcode"), (4, "所在地】", "address"), (5, "電話番号】", "phone")]:
        after(contractor + delta, label, "contractor_" + key)

    if third:
        after(5, "氏名】", "client")
    else:
        after(18, "申請者氏名　", "client")
        after(58, "氏名】", "client")
        for index, key in zip(range(206, 213), (
                "struct_submitted", "struct_not_yet", "struct_not_required", None,
                "energy_submitted", "energy_not_yet", "energy_not_required")):
            if key:
                boxes(index, [key])

    offset = 60 if third else 0
    for index, label, key in (
        (217, "地名地番】", "address"), (218, "住居表示】", "residence"),
        (223, "地区又は街区】", "other_zones"),
        (225, "幅員】", "road_width"), (226, "部分の長さ】", "frontage"),
        (235, "(1)", "site_area"),
    ):
        after(index - offset, label, key)

    boxes(220 - offset, ["city_inside", "city_urbanization", "city_control", "city_non_divided"])
    boxes(221 - offset, ["city_quasi", "city_outside"])
    boxes(222 - offset, ["fire_fire", "fire_quasi_fire", "fire_none"])
    slots(228 - offset, ["site_area"])
    slots(230 - offset, ["zone", "zone_reserve", "zone_reserve"])
    after(240 - offset, "(区分", "use_code")
    after(240 - offset, ")", "main_use")
    boxes(242 - offset, ["work_" + kind for kind in (
        "new", "extension", "rebuild", "relocation", "use_change", "major_repair", "major_remodel")])
    slots(244 - offset, ["building_area", None, "building_area"])
    slots(246 - offset, ["coverage_area", None, "coverage_area"])
    after(247 - offset, "率】", "coverage_percent")
    slots(249 - offset, ["floor_area", None, "floor_area"])
    ratio_offset = 61 if third else 0
    after(270 - ratio_offset, "面積】", "ratio_floor_area")
    after(271 - ratio_offset, "率】", "floor_percent")
    edit(279 - (61 if third else 0), lambda text: re.sub(r" +造", mark("structure") + "造", text, count=1))
    date(286 - (61 if third else 0), "start_date")
    date(287 - (61 if third else 0), "finish_date")

    for index, label, key in [
        (273, "建築物の数】", "building_count"), (274, "建築物の数】", "building_other"),
    ]:
        after(index - (61 if third else 0), label, key)
    for index, key in [(276, "building_height"), (277, "building_above"), (278, "building_below")]:
        slots(index - (61 if third else 0), [key])

    if not third:
        for index in range(6):
            key = f"detail_floor_{index}"
            edit(358 + index, lambda text, name=key: text.replace("(    階)", "(" + mark("slot_" + name + "_number__4") + "階)", 1))
            slots(358 + index, [key, None, key])
        slots(364, ["detail_floor_total", None, "detail_floor_total"])
        after(299, "番号】", "building_count")
        # docs/294: the fifth face (建築物の階別概要), one per storey: the app repeats the block between the
        # SBR_FIFTH comments for each storey and fills the fifth_* keys of each copy.
        for index, label, key in [(378, "番号】", "fifth_number"), (380, "階】", "fifth_floor"),
                                  (382, "小径】", "fifth_column"), (384, "垂直距離】", "fifth_clear"),
                                  (386, "高さ】", "fifth_storey_height"), (389, "天井の高さ】", "fifth_ceiling")]:
            after(index, label, key)

        def use_slots(text):
            keys = iter(["fifth_use_code", "fifth_use_name", "fifth_use_area"])
            def slot(m):
                width = 2 * len(m.group(1))
                return "（" + mark(f"slot_{next(keys)}__{width}") + "）"
            return re.sub("（(\u3000+)）", slot, text, count=3)
        edit(394, use_slots)
        after(300, "(区分", "detail_use_code")
        after(300, ")", "detail_main_use")
        boxes(306, ["detail_work_" + kind for kind in (
            "new", "extension", "rebuild", "relocation", "use_change", "major_repair", "major_remodel")])
        edit(307, lambda text: re.sub(r" +造", mark("detail_structure") + "造", text, count=1))
        for index, label, key in [
            (331, "階数】", "building_above"), (332, "階数】", "building_below"),
            (336, "高さ】", "building_height"), (337, "高さ】", "building_eaves"),
        ]:
            after(index, label, key)

    insertions = {
        index: '<w:r><w:t xml:space="preserve">' + mark(key) + '</w:t></w:r>'
        for index, key in {
            **contacts, (223 if third else 284): "permissions", (236 if third else 293): "other_matters",
        }.items()
    }
    if third:
        insertions.update({242: "<!--SBR_IMAGE_MAP-->", 261: "<!--SBR_IMAGE_SITE-->"})

    return apply_edits(xml, spans, edits, insertions)


def apply_edits(xml, spans, edits, insertions):
    paragraphs = list(ET.fromstring(xml).iter(W + "p"))
    for index in sorted(edits.keys() | insertions.keys(), reverse=True):
        span = spans[index]
        fragment = span.group()
        runs = list(re.finditer(r"(<w:t(?:\s[^>]*)?>)([^<]*)</w:t>", fragment))
        pieces = [unescape(run[2]) for run in runs]
        original = "".join(pieces)
        assert original == "".join(t.text or "" for t in paragraphs[index].iter(W + "t")), index
        changed = original
        for transform in edits.get(index, []):
            changed = transform(changed)
        if index in edits:
            assert changed != original, index
        # Keep typography of unaffected text by moving only changed characters between runs.
        starts = [0]
        for piece in pieces:
            starts.append(starts[-1] + len(piece))
        for op, a, b, c, d in reversed(SequenceMatcher(None, original, changed, autojunk=False).get_opcodes()):
            if op == "equal":
                continue
            for j in reversed(range(len(pieces))):
                lo, hi = starts[j:j+2]
                if a < hi and lo < b:
                    pieces[j] = pieces[j][:max(0, a - lo)] + pieces[j][min(hi, b) - lo:]
            if c < d:
                j = next(j for j in range(len(pieces)) if a <= starts[j+1])
                pos = a - starts[j]
                pieces[j] = pieces[j][:pos] + changed[c:d] + pieces[j][pos:]
        # Markers must be wholly in one run for replacement in the app.
        assert "".join(pieces) == changed, index
        assert sum(len(re.findall(r"@@SBR_\w+@@", p)) for p in pieces) == changed.count("@@SBR_"), index
        for run, piece in reversed(list(zip(runs, pieces, strict=True))):
            start, end = run.span(2)
            fragment = fragment[:start] + escape(piece) + fragment[end:]
        if index in insertions:
            fragment = fragment[:-6] + insertions[index] + fragment[-6:]
        xml = xml[:span.start()] + fragment + xml[span.end():]
    ET.fromstring(xml)
    return xml


def nested_spans(xml):
    """Paragraph spans in document order (as ElementTree's iter), paragraphs in text boxes included."""
    found, stack, order = {}, [], 0
    for m in re.finditer(r"<w:p(?=[\s>/])[^>]*?(/?)>|</w:p>", xml):
        if m.group(0).startswith("</"):
            start, index = stack.pop()
            found[index] = (start, m.end())
        elif m.group(1) == "/":
            found[order] = (m.start(), m.end())
            order += 1
        else:
            stack.append((m.start(), order))
            order += 1
    return [_Span(xml, *found[i]) for i in range(order)]


class _Span:
    def __init__(self, xml, start, end):
        self._text, self._start, self._end = xml[start:end], start, end

    def group(self):
        return self._text

    def start(self):
        return self._start

    def end(self):
        return self._end


def patch_agreement(xml):
    # 第１号様式 建築協定等手続状況届出書, first face: the site and building items. The applicant
    # and agent blocks (text boxes laid out with spaces) and the second face stay for the person.
    spans = nested_spans(xml)
    edits = {}

    def edit(index, transform):
        edits.setdefault(index, []).append(transform)

    def width(text):
        return sum(2 if char == "　" else 1 for char in text)

    def slot_spaces(index, keys):
        def insert(text):
            regions = list(re.finditer(r"　+", text))
            for region, key in reversed(list(zip(regions, keys, strict=False))):
                if key:
                    text = text[:region.start()] + mark(f"slot_{key}__{width(region[0])}") + text[region.end():]
            return text
        edit(index, insert)

    def boxes(index, keys):
        def select(text):
            keys_iter = iter(keys)
            result, count = re.subn("□", lambda _: mark("check_" + next(keys_iter)), text)
            assert count == len(keys)
            return result
        edit(index, select)

    def before(index, label, key):
        edit(index, lambda text: text.replace(label, mark(key) + label, 1))

    slot_spaces(15, ["agreement_ward"])
    edit(15, lambda text: text + mark("agreement_address_rest"))
    for index, key in [(20, "low"), (21, "residential"), (25, "mid")]:
        boxes(index, ["agreement_zone_" + key])
        slot_spaces(index, ["agreement_zone_" + key + "_number"])
    for index, key in [(22, "neighborhood_commercial"), (23, "quasi_industrial"), (24, "exclusive_industrial"),
                       (26, "quasi_residential"), (27, "commercial"), (28, "industrial")]:
        boxes(index, ["agreement_zone_" + key])
    boxes(32, ["city_control"])
    before(35, "平方メートル", "agreement_site_area")
    boxes(40, ["work_new", "work_extension", "work_rebuild", "work_relocation", "work_use_change"])
    boxes(41, ["work_major_repair", "work_major_remodel"])
    before(44, "平方メートル", "agreement_floor_area")
    before(46, "平方メートル", "agreement_building_area")
    before(49, "メートル", "agreement_height")
    slot_spaces(51, ["building_above", "building_below"])
    boxes(54, ["agreement_yokohama"])
    insertions = {
        37: '<w:r><w:t xml:space="preserve">' + mark("agreement_main_use") + '</w:t></w:r>',
    }
    return apply_edits(xml, spans, edits, insertions)


def wrap_fifth(xml):
    """Comments around the fifth face (from its page break to before the sixth face)."""
    spans = nested_spans(xml)
    def text(span):
        return "".join(unescape(t) for t in re.findall(r"<w:t(?:\s[^>]*)?>([^<]*)</w:t>", span.group()))
    start = next(s for s in spans if text(s) == "（第五面）")
    end = next(s for s in spans if text(s) == "（第六面）")
    return (xml[:start.start()] + "<!--SBR_FIFTH_BEGIN-->" + xml[start.start():end.start()] +
            "<!--SBR_FIFTH_END-->" + xml[end.start():])


def prepare(source, target, third, agreement=False):
    original = source.read_bytes()
    assert hashlib.sha256(original).hexdigest() == (AGREEMENT if agreement else THIRD if third else SECOND), source
    with zipfile.ZipFile(io.BytesIO(original)) as archive:
        entries = {}
        for name in archive.namelist():
            data = archive.read(name)
            if name == "word/document.xml":
                xml = data.decode("utf-8")
                xml = patch_agreement(xml) if agreement else patch_document(xml, third)
                if not agreement and not third:
                    xml = wrap_fifth(xml)
                data = xml.encode("utf-8")
            if third and name == "word/_rels/document.xml.rels":
                data = data.replace(b"</Relationships>", b"<!--SBR_RELS--></Relationships>")
            if third and name == "[Content_Types].xml":
                data = data.replace(b"</Types>", b'<Default Extension="png" ContentType="image/png"/></Types>')
            entries[name.encode("ascii")] = data
    # Simple embedded container: entry count, then lengths, name and original contents.
    with target.open("wb") as out:
        out.write(struct.pack("<I", len(entries)))
        for name, data in entries.items():
            out.write(struct.pack("<HI", len(name), len(data)))
            out.write(name)
            out.write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("second", type=Path)
    parser.add_argument("third", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--agreement", type=Path, help="0766_20240329.docx (建築協定等手続状況届出書)")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    prepare(args.second, args.output / "second.sbrform", False)
    prepare(args.third, args.output / "third.sbrform", True)
    if args.agreement:
        prepare(args.agreement, args.output / "agreement.sbrform", False, agreement=True)


if __name__ == "__main__":
    main()
