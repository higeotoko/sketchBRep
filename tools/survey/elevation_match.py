"""Match elevation texts (dd.dd, metres) to point symbols: distance from the rotated text box to each
symbol centre, then one global one-to-one assignment (Hungarian); texts with no symbol within DUMMY mm
stay unmatched. Experimental (docs/86).
usage: elevation_match.py ents.json out.json [glyph_width_ratio]
Marker radii are drawing-specific: set POINT_R / BOUNDARY_R / MANHOLE_R for your drawing."""
import json, math, re, sys
sys.stdout.reconfigure(encoding="utf-8")
POINT_R = [75.904]          # elevation point symbol radius (mm)
BOUNDARY_R = [150.0]        # boundary marker radius
MANHOLE_R = [308.17, 200.0] # manhole circles (with "M")
def has_r(c, rs): return any(abs(c['r'] - r) < 0.5 for r in rs)
d = json.load(open(sys.argv[1], encoding='utf-8'))
CW = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5   # glyph width / height
E = d['entities']
for i, e in enumerate(E): e['idx'] = i
texts = [e for e in E if e['type'] == 'TEXT']
elev_t = [e for e in texts if re.fullmatch(r'\d{2}\.\d{2}', e['text'].strip())]
circles = [e for e in E if e['type'] == 'CIRCLE']
pt_mk = [c for c in circles if has_r(c, POINT_R)]
bd_mk = [c for c in circles if has_r(c, BOUNDARY_R)]
mh_mk = [c for c in circles if has_r(c, MANHOLE_R)]
cands = [('P', c) for c in pt_mk] + [('B', c) for c in bd_mk] + [('M', c) for c in mh_mk]

def box_dist(t, p):
    h = t['h']; w = len(t['text'].strip()) * h * CW
    a = math.radians(t['rot']); ca, sa = math.cos(a), math.sin(a)
    dx, dy = p[0] - t['pos'][0], p[1] - t['pos'][1]
    u = dx * ca + dy * sa; v = -dx * sa + dy * ca      # text-local coords
    du = max(0, -u, u - w); dv = max(0, -v, v - h)
    return math.hypot(du, dv), (u, v)

def hungarian(cost):
    n, m = len(cost), len(cost[0]); INF = float('inf')
    u = [0] * (n + 1); v = [0] * (m + 1); p = [0] * (m + 1); way = [0] * (m + 1)
    for i in range(1, n + 1):
        p[0] = i; j0 = 0; minv = [INF] * (m + 1); used = [False] * (m + 1)
        while True:
            used[j0] = True; i0 = p[j0]; delta = INF; j1 = 0
            for j in range(1, m + 1):
                if not used[j]:
                    cur = cost[i0 - 1][j - 1] - u[i0] - v[j]
                    if cur < minv[j]: minv[j] = cur; way[j] = j0
                    if minv[j] < delta: delta = minv[j]; j1 = j
            for j in range(m + 1):
                if used[j]: u[p[j]] += delta; v[j] -= delta
                else: minv[j] -= delta
            j0 = j1
            if p[j0] == 0: break
        while True:
            j1 = way[j0]; p[j0] = p[j1]; j0 = j1
            if j0 == 0: break
    res = [None] * n
    for j in range(1, m + 1):
        if p[j]: res[p[j] - 1] = j - 1
    return res

DUMMY = 1200.0  # mm: farther than this from every symbol = unmatched
n = len(elev_t); m = len(cands)
cost = [[box_dist(t, c['center'])[0] for _, c in cands] + [DUMMY] * n for t in elev_t]
res = hungarian(cost)
out = []
for i, t in enumerate(elev_t):
    j = res[i]
    ds = sorted((box_dist(t, c['center'])[0], k + str(c['idx'])) for k, c in cands)
    if j >= m:
        out.append(dict(text_idx=t['idx'], value=t['text'], marker=None, kind=None, dist=None, near=ds[:3])); continue
    k, c = cands[j]; dd, (uu, vv) = box_dist(t, c['center'])
    other = [x for x in ds if x[1] != k + str(c['idx'])][0][0]
    out.append(dict(text_idx=t['idx'], value=t['text'], marker=c['idx'], kind=k, dist=round(dd), second=round(other),
                    local=(round(uu), round(vv)), xy=c['center'][:2], near=ds[:3]))
for o in out:
    print(o['text_idx'], o['value'], o['kind'], o['marker'], o.get('dist'), o.get('second'), o.get('local'), [(round(a), b) for a, b in o['near']])
used = {o['marker'] for o in out}
print('unused P', [c['idx'] for c in pt_mk if c['idx'] not in used])
print('unused B', [c['idx'] for c in bd_mk if c['idx'] not in used])
json.dump(out, open(sys.argv[2], 'w', encoding='utf-8'), ensure_ascii=False)
