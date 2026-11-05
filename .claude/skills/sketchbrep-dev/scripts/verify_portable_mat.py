"""Mat-foundation checks on an extracted local ZIP (run after verify_portable_pads.py RELEASE).

Uses only the package DLLs and Windows on PATH. Writes a two-storey sample with a mat
foundation and one passage, its STEP and foundation DXF, and a JSON record to outputs/.
"""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

root = Path(os.environ.get('SBR_DELIVERY_ROOT', r'C:\Users\enomo\Documents\Codex\2026-11-04\files-pasted-by-the-user-codex'))
repo = Path(os.environ.get('SBR_REPO', Path(__file__).resolve().parents[4]))
release = sys.argv[1]
unpack = root / f'work/zip-verify-{release}'
dist = unpack / 'SketchBRep'
assert (dist / 'BUILD_INFO.json').exists(), 'run verify_portable_pads.py first (it extracts the ZIP)'
env = os.environ.copy()
for key in list(env):
    if key.upper().startswith(('QT_', 'QML')):
        del env[key]
env['PATH'] = str(dist) + ';' + env['WINDIR'] + r'\System32;' + env['WINDIR']
env['QT_PLUGIN_PATH'] = str(dist)
env['TEMP'] = env['TMP'] = str(root / 'work/test-temp')


def run(exe, args=(), text=None, timeout=120):
    result = subprocess.run([str(dist / exe), *map(str, args)], input=text, capture_output=True,
                            text=True, encoding='utf-8', env=env, cwd=unpack, timeout=timeout)
    if result.returncode:
        raise RuntimeError((exe, result.returncode, result.stdout, result.stderr))
    return result


def cli(args, *requests):
    # Errors come back as {"ok": false} lines with a non-zero exit; the caller asserts.
    r = subprocess.run([str(dist / 'sbr_cli.exe'), *map(str, args)], input=''.join(json.dumps(x) + '\n' for x in requests),
                       capture_output=True, text=True, encoding='utf-8', errors='replace', env=env, cwd=unpack, timeout=120)
    replies = [json.loads(line) for line in r.stdout.splitlines() if line.startswith('{')]  # STEP export also prints OCCT statistics
    return replies


# 1. Two-storey frame, saved.
frame_file = unpack / 'mat-frame.sbr'
reply = cli(['--save', frame_file], {'command': 'batch', 'args': {'commands': [
    {'command': 'generate_frame', 'args': {'inputs': {'x_bays': 4, 'y_bays': 4}, 'preview': False}}]}})[0]
assert reply['ok'], reply
frame = reply['result']['results'][0]['result']['frame']

# 2. Preview without a passage to read the sill axes, then find an interior run whose midpoint takes a passage.
reply = cli(['--open', frame_file], {'command': 'generate_foundation', 'args': {'inputs': {'type': 'mat', 'frame': frame}}})[0]
assert reply['ok'] and reply['result']['preview'], reply
# The sill axes come from check_foundation after applying; use the preview's quantities only here.
plain_volume = reply['result']['generated_parts_volume_m3']

xs, ys = set(), set()
probe_file = unpack / 'mat-probe.sbr'
reply = cli(['--open', frame_file, '--save', probe_file], {'command': 'batch', 'args': {'commands': [
    {'command': 'generate_foundation', 'args': {'inputs': {'type': 'mat', 'frame': frame}, 'preview': False}},
    {'command': 'check_foundation'}]}})[0]
assert reply['ok'], reply
frame_plan = reply['result']['results'][-1]['result']['plans'][0]
assert frame_plan['status'] == 'current' and frame_plan['quantities']['foundation_slab']['solids'] == 1, frame_plan
sources = frame_plan['sources']
for s in sources:
    xs.update([s['start'][0], s['end'][0]]); ys.update([s['start'][1], s['end'][1]])
lo_x, hi_x, lo_y, hi_y = min(xs), max(xs), min(ys), max(ys)
# The recipe's sills are the outer ring only: add one interior sill (same section and height)
# between grid lines, and plan the mat from the ring plus that sill.
centre_z = sources[0]['start'][2]
depth = 2 * (centre_z - sources[0]['sill_bottom_z'])
mid_y = lo_y + (hi_y - lo_y) * 3 / 8
house_file = unpack / 'mat-house-before.sbr'
reply = cli(['--open', frame_file, '--save', house_file], {'command': 'batch', 'args': {'commands': [
    {'command': 'create_member', 'args': {'section': {'kind': 'rectangle', 'width': depth, 'depth': depth},
     'start': [lo_x, mid_y, centre_z], 'end': [hi_x, mid_y, centre_z], 'anchor': {'across': 'center', 'up': 'center'},
     'role': 'sill', 'material_class': 'timber', 'name': '内部土台（検証用）'}}]}})[0]
assert reply['ok'], reply
inner = reply['result']['results'][0]['result']['id']
sill_ids = [s['id'] for s in sources] + [inner]
reply = cli(['--open', house_file], {'command': 'generate_foundation', 'args': {'inputs': {'type': 'mat', 'sources': sill_ids}}})[0]
assert reply['ok'], reply
plain_volume = reply['result']['generated_parts_volume_m3']
chosen = [lo_x + (hi_x - lo_x) / 4, mid_y]
reply = cli(['--open', house_file], {'command': 'generate_foundation', 'args': {'inputs': {
    'type': 'mat', 'sources': sill_ids, 'passages': [{'at': chosen}]}}})[0]
assert reply['ok'], reply

# 3. A passage on the outer run is refused and changes nothing.
outer = [(lo_x + hi_x) / 2, lo_y]
reply = cli(['--open', house_file], {'command': 'generate_foundation', 'args': {'inputs': {
    'type': 'mat', 'sources': sill_ids, 'passages': [{'at': outer}]}}})[0]
assert not reply['ok'] and '外周' in reply['error'], reply

# 4. Apply with the passage, save the sample, reopen, check, export STEP and foundation DXF.
sample = root / f'outputs/SketchBRep-mat-foundation-house-{release}.sbr'
drawing = root / f'outputs/SketchBRep-mat-foundation-house-{release}.dxf'
step = unpack / 'mat-house.step'
reply = cli(['--open', house_file, '--save', sample], {'command': 'batch', 'args': {'commands': [
    {'command': 'generate_foundation', 'args': {'inputs': {'type': 'mat', 'sources': sill_ids, 'passages': [{'at': chosen}]}, 'preview': False}},
    {'command': 'check_foundation'}]}})[0]
assert reply['ok'], reply
plan = reply['result']['results'][-1]['result']['plans'][0]
assert plan['status'] == 'current' and plan['verified'] is False, plan
assert plan['quantities']['foundation_slab']['solids'] == 1 and plan['quantities']['foundation_stem']['solids'] == 1, plan
edge = plan['quantities'].get('foundation_edge_footing')
assert edge and edge['solids'] == 1 and edge['volume_m3'] > 0, plan
assert plan['generated_parts_volume_m3'] < plain_volume, (plan['generated_parts_volume_m3'], plain_volume)
replies = cli(['--open', sample], {'command': 'check_foundation'}, {'command': 'check_foundation_supports'},
              {'command': 'export_step', 'args': {'path': str(step)}},
              {'command': 'export_drawing', 'args': {'kind': 'framing', 'preset': 'foundation', 'path': str(drawing)}})
assert all(r['ok'] for r in replies), replies
reopened = replies[0]['result']['plans'][0]
assert reopened['status'] == 'current', reopened
supports = replies[1]['result']['rows']
posts = [r for r in supports if r['role'] == 'floor_post']
assert step.stat().st_size > 0 and drawing.stat().st_size > 0

# 5. MCP lists the mat inputs in the generate_foundation description.
session = (repo / 'tests/data/mcp_session.jsonl').read_text(encoding='utf-8')
messages = [json.loads(line) for line in run('sbr_mcp.exe', ['--headless'], session).stdout.splitlines() if line.strip()]
assert any(any(t['name'] == 'generate_foundation' and 'type:mat' in t['description'] for t in m.get('result', {}).get('tools', [])) for m in messages)

# 6. The mat dialog GUI test against the package DLLs (test files added after extraction only).
shutil.copy2(repo / 'build/tests/sbr_gui_tests.exe', dist / 'sbr_gui_tests.exe')
shutil.copy2(Path(r'C:\Users\enomo\Documents\Codex\sbr-build-env\Library\bin\Qt6Test.dll'), dist / 'Qt6Test.dll')
result_path = root / f'outputs/portable-{release}-mat-gui-results.xml'
run('sbr_gui_tests.exe', ['foundationMatDialogMakesSlabAndPassage', 'foundationDialogCreatesAndRegeneratesPlan',
                          'timberInteriorSillsFromDialog', 'structureDiagnosticsOverlayIsReadOnlyAndClearsOnEdit', '-o', str(result_path) + ',junitxml'], timeout=180)
suite = ET.parse(result_path).getroot()
assert suite.attrib['failures'] == '0' and suite.attrib['errors'] == '0', suite.attrib

# 7. Interior sills from the recipe itself: a frame with interior_sills, a mat from the whole frame with a passage.
inner_file = unpack / 'mat-inner-frame.sbr'
reply = cli(['--save', inner_file], {'command': 'batch', 'args': {'commands': [
    {'command': 'generate_frame', 'args': {'inputs': {'x_bays': 4, 'y_bays': 4, 'interior_sills': [{'from': [0, 2], 'to': [4, 2]}]}, 'preview': False}},
    {'command': 'generate_foundation', 'args': {'inputs': {'type': 'mat', 'frame': '$1.frame', 'passages': [{'at': [1365, 1820]}], 'inner_depth': 200}, 'preview': False}},
    {'command': 'check_foundation'}]}})[0]
assert reply['ok'], reply
inner_plan = reply['result']['results'][-1]['result']['plans'][0]
assert inner_plan['status'] == 'current' and inner_plan['quantities']['foundation_stem']['solids'] == 1, inner_plan
assert len(inner_plan['sources']) == 16 + 4, len(inner_plan['sources'])
beam = inner_plan['quantities'].get('foundation_ground_beam')
assert beam and beam['solids'] == 1 and beam['volume_m3'] > 0, inner_plan

# 8. Structural overview on the sample (read-only).
diag = cli(['--open', sample], {'command': 'check_structure'})[0]
assert diag['ok'] and diag['result']['version'] == 'structural-diagnostics-1' and diag['result']['verified'] is False, diag
assert any('N' in u for u in diag['result']['unassessed']), diag['result']['unassessed']

record = {
    'code_commit': json.loads((dist / 'BUILD_INFO.json').read_text())['code_commit'],
    'frame': '2-storey 4x4 bays (generate_frame defaults) + one interior sill', 'frame_only_mat': 'passed',
    'passage_at': chosen, 'outer_passage_refused': True,
    'mat_volume_without_passage_m3': plain_volume,
    'mat_volume_with_passage_m3': plan['generated_parts_volume_m3'],
    'slab_m3': plan['quantities']['foundation_slab']['volume_m3'],
    'stem_m3': plan['quantities']['foundation_stem']['volume_m3'],
    'edge_footing_m3': edge['volume_m3'],
    'save_reopen_status': reopened['status'],
    'recipe_interior_sills_mat_with_passage': 'passed', 'recipe_interior_sill_sources': len(inner_plan['sources']),
    'ground_beam_m3': beam['volume_m3'],
    'check_structure': 'passed', 'structure_markers': len(diag['result']['markers']),
    'floor_post_support_statuses': sorted({p['status'] for p in posts}),
    'step_and_foundation_dxf': 'passed', 'mcp_mat_description': 'passed',
    'gui_actual_conditions': int(suite.attrib['tests']) - 2, 'gui_failures': 0,
    'sample_sbr': sample.name, 'sample_dxf': drawing.name,
    'ci_verified': False, 'human_windows_verified': False,
}
(root / f'outputs/portable-{release}-mat-verification.json').write_text(json.dumps(record, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
print(json.dumps(record, ensure_ascii=False))
