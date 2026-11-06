"""Tiny MCP stdio client for SketchBRep (sbr_mcp.exe). Usage: mcpclient.py <tool> <args.json | -> [out.json]
or: mcpclient.py --batchfile cmds.json [chunk] [out.json] to send a list of batches."""
import json, os, subprocess, sys, time

EXE = os.environ.get("SBR_MCP", r"C:\Apps\SketchBrep\sbr_mcp.exe")

class Client:
    def __init__(self):
        self.p = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.n = 0
        self.req('initialize', {'protocolVersion': '2024-11-05', 'capabilities': {}, 'clientInfo': {'name': 'survey-script', 'version': '1'}})
        self.notify('notifications/initialized', {})

    def notify(self, m, params):
        self.p.stdin.write((json.dumps({'jsonrpc': '2.0', 'method': m, 'params': params}) + '\n').encode()); self.p.stdin.flush()

    def req(self, m, params):
        self.n += 1
        self.p.stdin.write((json.dumps({'jsonrpc': '2.0', 'id': self.n, 'method': m, 'params': params}) + '\n').encode()); self.p.stdin.flush()
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError('server closed: ' + self.p.stderr.read().decode(errors='replace'))
            msg = json.loads(line)
            if msg.get('id') == self.n:
                return msg

    def call(self, tool, args):
        r = self.req('tools/call', {'name': tool, 'arguments': args})
        if 'error' in r: raise RuntimeError(json.dumps(r['error']))
        res = r['result']
        txt = ''.join(c.get('text', '') for c in res.get('content', []))
        if res.get('isError'): raise RuntimeError(txt)
        try: return json.loads(txt)
        except Exception: return txt

    def close(self):
        self.p.stdin.close(); self.p.wait(timeout=10)

if __name__ == '__main__':
    c = Client()
    if sys.argv[1] == '--tools':
        r = c.req('tools/list', {}); print(len(r['result']['tools']), [t['name'] for t in r['result']['tools']][:5])
    else:
        args = json.load(sys.stdin) if sys.argv[2] == '-' else json.loads(sys.argv[2])
        t0 = time.time(); out = c.call(sys.argv[1], args)
        print(json.dumps(out, ensure_ascii=False)[:3000]); print('dt', round(time.time() - t0, 2), file=sys.stderr)
    c.close()
