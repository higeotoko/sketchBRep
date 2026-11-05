"""Portable ZIP verification (run by sbr.py verify): extracts the ZIP for RELEASE and checks
hashes, dependencies, CLI/MCP chains (foundation, supports, pads, wall bays, house sample) and GUI
conditions with only the package DLLs on PATH. Delivery folder: SBR_DELIVERY_ROOT; repo: SBR_REPO.
"""
from pathlib import Path
import sys
import hashlib
import json
import os
import shutil
import subprocess
import zipfile
import xml.etree.ElementTree as ET

root = Path(os.environ.get('SBR_DELIVERY_ROOT', r'C:\Users\enomo\Documents\Codex\2026-11-04\files-pasted-by-the-user-codex'))
repo = Path(os.environ.get('SBR_REPO', Path(__file__).resolve().parents[4]))
release=sys.argv[1]
archive = root / f'outputs/SketchBRep-windows-x64-{release}-local.zip'
unpack = root / f'work/zip-verify-{release}'
unpack.mkdir(exist_ok=True)
with zipfile.ZipFile(archive) as z:
    assert z.testzip() is None
    z.extractall(unpack)
dist = unpack / 'SketchBRep'
info = json.loads((dist / 'BUILD_INFO.json').read_text())
for name, digest in info['executables_sha256'].items():
    assert hashlib.sha256((dist / name).read_bytes()).hexdigest() == digest
env = os.environ.copy()
for key in list(env):
    if key.upper().startswith(('QT_', 'QML')):
        del env[key]
env['PATH'] = str(dist) + ';' + env['WINDIR'] + r'\System32;' + env['WINDIR']
env['QT_PLUGIN_PATH'] = str(dist)
env['TEMP'] = env['TMP'] = str(root / 'work/test-temp')

def run(exe, args=(), text=None, timeout=60):
    result = subprocess.run([str(dist / exe), *map(str, args)], input=text, capture_output=True,
                          text=True, encoding='utf-8', env=env, cwd=unpack,
                          timeout=timeout)
    if result.returncode:
        raise RuntimeError((exe, result.returncode, result.stdout, result.stderr))
    return result

run('sbr_phase0.exe', [1000, 2000, 3000, unpack / 'box.step'])
assert (unpack / 'box.step').stat().st_size > 1000
requests = [
    {'command': 'create_box', 'args': {'origin': [0, 0, 0], 'width': 10, 'depth': 20, 'height': 30}},
    {'command': 'export_3dm', 'args': {'path': str(unpack / 'box.3dm')}},
]
r = run('sbr_cli.exe', text=''.join(json.dumps(x) + '\n' for x in requests))
answers = [json.loads(line) for line in r.stdout.splitlines() if line.strip()]
assert len(answers) == 2 and all(x['ok'] for x in answers), answers
assert (unpack / 'box.3dm').stat().st_size > 1000
batch = {'command': 'batch', 'args': {'commands': [
    {'command':'create_wall', 'args':{'points':[[0,0],[6000,0]], 'thickness':120, 'height':3000}},
    {'command':'place_openings', 'args':{'host':'$1.ids.0', 'face_normal':[0,1,0], 'count':1, 'width':1000, 'height':1000, 'sill':700}},
    {'command':'list_wall_bays', 'args':{'elevation':0}},
]}}
r = run('sbr_cli.exe', ['--save', unpack/'wall-bays.sbr'], text=json.dumps(batch)+'\n')
response = json.loads(r.stdout)
assert response['ok'], response
bays = response['result']['results'][-1]['result']['bays']
assert len(bays) == 2 and all(abs(b['length_mm']-2500)<1e-6 for b in bays), bays
r = run('sbr_cli.exe', ['--open', unpack/'wall-bays.sbr'], text=json.dumps({'command':'list_wall_bays'})+'\n')
assert json.loads(r.stdout)['result']['bays'] == bays
frame_steps = [{'command':'create_wall','args':{'points':[[0,0],[3000,0]],'thickness':120,'height':3000}}]
for x in [0,1000,2000,3000]:
    frame_steps.append({'command':'create_member','args':{'start':[x,0,0],'end':[x,0,2880],
        'section':{'kind':'rectangle','width':105,'depth':105},'role':'column','material_class':'timber'}})
for z, role in [(-60,'sill'),(2940,'beam')]:
    frame_steps.append({'command':'create_member','args':{'start':[0,0,z],'end':[3000,0,z], 'up':[0,0,1],
        'section':{'kind':'rectangle','width':120,'depth':120},'role':role,'material_class':'timber'}})
frame_steps.append({'command':'list_wall_bays'})
r=run('sbr_cli.exe',['--save',unpack/'four-member-frame.sbr'],text=json.dumps({'command':'batch','args':{'commands':frame_steps}})+'\n')
response=json.loads(r.stdout)
assert response['ok'],response
frames=response['result']['results'][-1]['result']['bays']
assert len(frames)==3 and all(b['frame_geometry']=='touching' and b['verified'] is False for b in frames),frames
r=run('sbr_cli.exe',['--open',unpack/'four-member-frame.sbr'],text=json.dumps({'command':'list_wall_bays'})+'\n')
assert json.loads(r.stdout)['result']['bays']==frames

foundation_steps=[]
for a,b in [([0,0,480],[3000,0,480]),([3000,0,480],[3000,2000,480]),([3000,2000,480],[0,2000,480]),([0,2000,480],[0,0,480])]:
    foundation_steps.append({'command':'create_member','args':{'start':a,'end':b,'up':[0,0,1],
        'section':{'kind':'rectangle','width':120,'depth':120},'role':'sill','material_class':'timber'}})
foundation_steps.append({'command':'generate_foundation','args':{'inputs':{'sources':['$1.id','$2.id','$3.id','$4.id']},'preview':False}})
foundation_steps.append({'command':'check_foundation','args':{'foundation':'$5.foundation'}})
r=run('sbr_cli.exe',['--save',unpack/'foundation.sbr'],text=json.dumps({'command':'batch','args':{'commands':foundation_steps}})+'\n'+json.dumps({'command':'export_step','args':{'path':str(unpack/'foundation.step')}})+'\n')
answers=[json.loads(line) for line in r.stdout.splitlines() if line.lstrip().startswith('{')]
assert len(answers)==2 and all(x['ok'] for x in answers),answers
plan=answers[0]['result']['results'][-1]['result']['plans'][0]
assert plan['status']=='current' and plan['verified'] is False and abs(plan['generated_parts_volume_m3']-1.575)<1e-8,plan
assert plan['top_z']==400 and all(s['packing_gap_matches'] for s in plan['sources'])
assert (unpack/'foundation.step').stat().st_size>1000
r=run('sbr_cli.exe',['--open',unpack/'foundation.sbr'],text=json.dumps({'command':'check_foundation'})+'\n')
restored=json.loads(r.stdout)['result']['plans'][0]
assert restored['status']=='current' and abs(restored['generated_parts_volume_m3']-1.575)<1e-8,restored
assert len(restored['sources'])==4 and all(q['solids']==1 for q in restored['quantities'].values())


support_commands=[
 {'command':'create_member','args':{'start':[1500,0,540],'end':[1500,0,3000],
  'section':{'kind':'rectangle','width':105,'depth':105},'role':'column','material_class':'timber'}},
 {'command':'create_member','args':{'start':[1500,1000,400],'end':[1500,1000,800],
  'section':{'kind':'rectangle','width':90,'depth':90},'role':'floor_post','material_class':'timber'}},
 {'command':'check_foundation_supports'}]
r=run('sbr_cli.exe',['--open',unpack/'foundation.sbr','--save',unpack/'foundation-supports.sbr'],text=json.dumps({'command':'batch','args':{'commands':support_commands}})+'\n')
answer=json.loads(r.stdout)
assert answer['ok'],answer
supports=answer['result']['results'][-1]['result']
assert supports['version']=='foundation-supports-1' and not supports['incomplete'],supports
columns=[r for r in supports['rows'] if r['role']=='column']
posts=[r for r in supports['rows'] if r['role']=='floor_post']
assert len(columns)==1 and columns[0]['path_status']=='geometry_detected' and columns[0]['via'][0]['status']=='detected',columns
assert len(posts)==1 and posts[0]['status']=='missing' and posts[0]['verified'] is False,posts
r=run('sbr_cli.exe',['--open',unpack/'foundation-supports.sbr'],text=json.dumps({'command':'check_foundation_supports'})+'\n')
again=json.loads(r.stdout)['result']
assert [(r['id'],r['status'],r['path_status']) for r in again['rows']]==[(r['id'],r['status'],r['path_status']) for r in supports['rows']]


requests=[{'command':'generate_foundation','args':{'inputs':{'type':'pads','sources':[posts[0]['id']]},'preview':False}},
 {'command':'check_foundation_supports','args':{'ids':[posts[0]['id']]}}, {'command':'check_foundation'}]
r=run('sbr_cli.exe',['--open',unpack/'foundation-supports.sbr','--save',unpack/'foundation-pads.sbr'],text=json.dumps({'command':'batch','args':{'commands':requests}})+'\n')
reply=json.loads(r.stdout);assert reply['ok'],reply
results=reply['result']['results'];assert results[1]['result']['rows'][0]['path_status']=='geometry_detected'
padplan=next(p for p in results[2]['result']['plans'] if p['inputs'].get('type')=='pads')
assert padplan['status']=='current' and abs(padplan['generated_parts_volume_m3']-.0135)<1e-9
r=run('sbr_cli.exe',['--open',unpack/'foundation-pads.sbr'],text=json.dumps({'command':'check_foundation_supports','args':{'ids':[posts[0]['id']]}})+'\n')
assert json.loads(r.stdout)['result']['rows'][0]['path_status']=='geometry_detected'
house_steps=[{'command':'generate_frame','args':{'inputs':{'x_bays':4,'y_bays':4,'sections':{'floor_post':'TIMBER-90x90'}},'preview':False}},
 {'command':'generate_foundation','args':{'inputs':{'frame':'$1.frame'},'preview':False}}, {'command':'check_foundation_supports'}]
r=run('sbr_cli.exe',['--save',unpack/'house-before-pads.sbr'],text=json.dumps({'command':'batch','args':{'commands':house_steps}})+'\n')
reply=json.loads(r.stdout);assert reply['ok'],reply
initial=reply['result']['results'][-1]['result']
missing=[r['id'] for r in initial['rows'] if r['role']=='floor_post' and r['status']=='missing']
assert len(missing)==2,missing
sample=root/f'outputs/SketchBRep-foundation-pad-house-{release}.sbr'
drawing=root/f'outputs/SketchBRep-foundation-pad-house-{release}.dxf'
house_steps=[{'command':'generate_foundation','args':{'inputs':{'type':'pads','sources':missing},'preview':False}},
 {'command':'check_foundation_supports'}, {'command':'check_foundation'}]
requests=[{'command':'batch','args':{'commands':house_steps}},
 {'command':'export_drawing','args':{'kind':'framing','preset':'foundation','path':str(drawing)}},
 {'command':'export_step','args':{'path':str(unpack/'house-foundations.step')}}]
r=run('sbr_cli.exe',['--open',unpack/'house-before-pads.sbr','--save',sample],text=''.join(json.dumps(x)+'\n' for x in requests),timeout=120)
answers=[json.loads(line) for line in r.stdout.splitlines() if line.lstrip().startswith('{')]
assert len(answers)==3 and all(x['ok'] for x in answers),answers
house_result=answers[0]['result']['results'];rows=house_result[1]['result']['rows']
floor_posts=[r for r in rows if r['role']=='floor_post'];assert len(floor_posts)==6 and all(r['path_status']=='geometry_detected' for r in floor_posts),floor_posts
plans=house_result[2]['result']['plans'];assert len(plans)==2 and all(p['status']=='current' for p in plans)
r=run('sbr_cli.exe',['--open',sample],text=json.dumps({'command':'check_foundation_supports'})+'\n')
restored_posts=[r for r in json.loads(r.stdout)['result']['rows'] if r['role']=='floor_post']
assert len(restored_posts)==6 and all(r['path_status']=='geometry_detected' for r in restored_posts)
assert sample.stat().st_size>1000 and drawing.stat().st_size>1000 and (unpack/'house-foundations.step').stat().st_size>1000
house_record={'sample_sbr':sample.name,'sample_dxf':drawing.name,'real_floor_posts':6,'missing_before':2,'pads_generated':2,'missing_after':0,'save_reopen':'passed','foundation_dxf_and_step':'passed'}

session = (repo / 'tests/data/mcp_session.jsonl').read_text(encoding='utf-8')
r = run('sbr_mcp.exe', ['--headless'], text=session)
messages = [json.loads(line) for line in r.stdout.splitlines() if line.strip()]
assert not any('error' in message for message in messages)
assert any(message.get('result', {}).get('isError') is False for message in messages)
assert any(any(t['name']=='list_wall_bays' and 'split_at_posts' in t['inputSchema']['properties'] for t in message.get('result',{}).get('tools',[])) for message in messages)
assert any({'generate_foundation','check_foundation','check_foundation_supports'}.issubset({t['name'] for t in message.get('result',{}).get('tools',[])}) for message in messages)
assert any(any(t['name']=='generate_foundation' and 'isolated pads' in t['description'] for t in m.get('result',{}).get('tools',[])) for m in messages)
# Run the same compiled UI against DLLs/plugins in the extracted ZIP only.
# The test executable is copied after extraction and is not shipped.
shutil.copy2(repo / 'build/tests/sbr_gui_tests.exe', dist / 'sbr_gui_tests.exe')
shutil.copy2(Path(r'C:\Users\enomo\Documents\Codex\sbr-build-env\Library\bin\Qt6Test.dll'), dist / 'Qt6Test.dll')
result_path = root / f'outputs/portable-{release}-gui-results.xml'
run('sbr_gui_tests.exe', ['foundationPadsFromMissingFloorPostsAndRegeneration','foundationPadsWithDifferentTopsReloadWithoutChangingTheModel','timberFloorPostChoicePersistsInRegeneration','foundationSupportsSelectActualPartsAndInvalidate','foundationPreviewApplyRejectAndStaleRevision','foundationDialogCreatesAndRegeneratesPlan','wallBaySupportsSelectMembersAndInvalidateAfterDeletion', 'wallBayCandidatesHighlightAndInvalidateWithoutEditing', 'architectureToolbarDrawsAtChosenLevel',
    'planOpeningChoosesFloorAndPreservesUndo', 'planOpeningOnRotatedWalls',
    'planOpeningRejectsAmbiguousHiddenAndLockedWalls', 'slabToolAndOpeningTool',
    '-o', str(result_path) + ',junitxml'], timeout=120)
suite = ET.parse(result_path).getroot()
assert suite.attrib['failures'] == '0' and suite.attrib['errors'] == '0', suite.attrib
record = {
    'code_commit': info['code_commit'], 'zip': archive.name,
    'bytes': archive.stat().st_size, 'sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
    'runtime_dlls_verified': info['runtime_dlls_verified'],
    'binary_dependencies_checked': info['binary_dependencies_checked'],
    'zip_crc_and_executable_hashes': 'passed',
    'path_limited_to_package_and_windows': True,
    'pads_cli_generated_support_and_save_reopen': 'passed', 'two_storey_house':house_record,
    'foundation_supports_cli_chain_missing_and_save_reopen': 'passed',
    'foundation_cli_net_volume_save_reopen_step': 'passed', 'foundation_mcp_tools_exposed': 'passed',
    'phase0_step_export': 'passed', 'cli_3dm_export': 'passed',
    'mcp_initialize_tools_and_create_box': 'passed', 'wall_bays_cli_save_reopen': 'passed', 'four_member_contacts_cli_save_reopen': 'passed',
    'gui_actual_conditions': int(suite.attrib['tests']) - 2,
    'gui_including_init_cleanup': int(suite.attrib['tests']),
    'gui_failures': 0, 'gui_process_exit': 0, 'gui_seconds': suite.attrib['time'],
    'test_only_files_added_after_extraction': ['sbr_gui_tests.exe', 'Qt6Test.dll'],
    'ci_verified': False, 'human_windows_verified': False,
}
(root / f'outputs/portable-{release}-verification.json').write_text(
    json.dumps(record, indent=2) + '\n', encoding='utf-8')
print(json.dumps(record))
