"""Minimal DXF (ASCII) parser: ENTITIES + BLOCKS, LWPOLYLINE/POLYLINE/LINE/ARC/CIRCLE/TEXT/MTEXT/INSERT/POINT."""
import sys, json, collections, math
sys.stdout.reconfigure(encoding="utf-8")

def read_pairs(path):
    raw = open(path, 'rb').read()
    try:
        txt = raw.decode('utf-8')
    except UnicodeDecodeError:
        txt = raw.decode('cp932', errors='replace')
    lines = txt.splitlines()
    for i in range(0, len(lines) - 1, 2):
        yield int(lines[i].strip()), lines[i + 1].rstrip()

def group_entities(pairs):
    sections = collections.defaultdict(list)
    sec = None; cur = None; blk = None
    blocks = {}
    expect_name = False
    for code, val in pairs:
        if code == 0 and val == 'SECTION':
            expect_name = True; continue
        if expect_name and code == 2:
            sec = val; expect_name = False; continue
        if code == 0 and val == 'ENDSEC':
            if cur: _flush(cur, sec, sections, blocks, blk)
            cur = None; sec = None; continue
        if sec in ('ENTITIES', 'BLOCKS'):
            if code == 0:
                if cur: _flush(cur, sec, sections, blocks, blk)
                if val == 'BLOCK':
                    blk = {'name': None}; cur = {'type': 'BLOCK', 'g': []}; continue
                if val == 'ENDBLK':
                    blk = None; cur = None; continue
                cur = {'type': val, 'g': []}
            elif cur is not None:
                cur['g'].append((code, val))
        elif sec == 'TABLES':
            sections['TABLES'].append((code, val))
    return sections, blocks

def _flush(cur, sec, sections, blocks, blk):
    if cur['type'] == 'BLOCK':
        name = dict(cur['g']).get(2)
        blk['name'] = name; blocks[name] = []
        blk['list'] = blocks[name]
        return
    if sec == 'BLOCKS' and blk is not None:
        blk['list'].append(cur)
    elif sec == 'ENTITIES':
        sections['ENTITIES'].append(cur)

def parse_entity(e):
    g = e['g']; t = e['type']
    d = {'type': t, 'layer': None}
    for c, v in g:
        if c == 8: d['layer'] = v
        if c == 62: d['color'] = int(v)
        if c == 5: d['handle'] = v
    if t == 'LWPOLYLINE':
        pts = []; cur = None; flags = 0; elev = 0.0; bulges = []
        for c, v in g:
            if c == 10: cur = [float(v), None]; pts.append(cur); bulges.append(0.0)
            elif c == 20: cur[1] = float(v)
            elif c == 42 and pts: bulges[-1] = float(v)
            elif c == 70: flags = int(v)
            elif c == 38: elev = float(v)
        d.update(points=[[p[0], p[1], elev] for p in pts], closed=bool(flags & 1), bulges=bulges)
    elif t in ('LINE',):
        m = {}
        for c, v in g: m[c] = float(v) if c in (10, 20, 30, 11, 21, 31) else v
        d['points'] = [[m.get(10, 0), m.get(20, 0), m.get(30, 0)], [m.get(11, 0), m.get(21, 0), m.get(31, 0)]]
    elif t in ('ARC', 'CIRCLE'):
        m = {}
        for c, v in g:
            if c in (10, 20, 30, 40, 50, 51): m[c] = float(v)
        d.update(center=[m.get(10), m.get(20), m.get(30, 0)], r=m.get(40), a0=m.get(50, 0), a1=m.get(51, 360))
    elif t == 'POINT':
        m = {}
        for c, v in g:
            if c in (10, 20, 30): m[c] = float(v)
        d['pos'] = [m.get(10), m.get(20), m.get(30, 0)]
    elif t in ('TEXT', 'MTEXT', 'ATTRIB'):
        m = {}; txt = ''
        for c, v in g:
            if c in (10, 20, 30, 11, 21, 31, 40, 50): m[c] = float(v)
            if c in (1, 3): txt += v
            if c == 72: d['halign'] = int(v)
            if c == 73: d['valign'] = int(v)
        d.update(text=txt, pos=[m.get(10), m.get(20), m.get(30, 0)], h=m.get(40), rot=m.get(50, 0))
        if 11 in m: d['pos2'] = [m.get(11), m.get(21), m.get(31, 0)]
    elif t == 'INSERT':
        m = {}
        for c, v in g:
            if c in (10, 20, 30, 41, 42, 50): m[c] = float(v)
            if c == 2: d['block'] = v
        d.update(pos=[m.get(10), m.get(20), m.get(30, 0)], sx=m.get(41, 1), sy=m.get(42, 1), rot=m.get(50, 0))
    elif t == 'VERTEX':
        m = {}
        for c, v in g:
            if c in (10, 20, 30, 42): m[c] = float(v)
            if c == 70: d['vflags'] = int(v)
        d['pos'] = [m.get(10), m.get(20), m.get(30, 0)]
    elif t == 'POLYLINE':
        for c, v in g:
            if c == 70: d['flags'] = int(v)
    return d

def assemble(ents):
    out = []; poly = None
    for e in ents:
        d = parse_entity(e)
        if d['type'] == 'POLYLINE':
            poly = d; poly['points'] = []; poly['closed'] = bool(d.get('flags', 0) & 1); continue
        if d['type'] == 'VERTEX' and poly is not None:
            poly['points'].append(d['pos']); continue
        if d['type'] == 'SEQEND' and poly is not None:
            out.append(poly); poly = None; continue
        out.append(d)
    return out

if __name__ == '__main__':
    path, outp = sys.argv[1], sys.argv[2]
    secs, blocks = group_entities(read_pairs(path))
    ents = assemble(secs['ENTITIES'])
    blks = {k: assemble(v) for k, v in blocks.items()}
    json.dump({'entities': ents, 'blocks': blks}, open(outp, 'w', encoding='utf-8'), ensure_ascii=False)
    cnt = collections.Counter((e['layer'], e['type']) for e in ents)
    for (l, t), n in sorted(cnt.items(), key=lambda x: (str(x[0][0]), x[0][1])): print(f'{l:30s} {t:12s} {n}')
    print('blocks used:', collections.Counter(e.get('block') for e in ents if e['type'] == 'INSERT'))
    for k, v in blks.items():
        if not k.startswith('*Model') and not k.startswith('*Paper'):
            print('BLOCK', k, collections.Counter(x['type'] for x in v))
