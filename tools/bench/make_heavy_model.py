# Builds a heavy test model (3 floors on a 4 x 3 bay grid: slabs, 60 columns,
# walls with windows and doors) through sbr_cli, for real-machine
# responsiveness checks (STATE-05).
#   python3 tools/bench/make_heavy_model.py build/tools/cli/sbr_cli out.sbr
import json, subprocess, sys, time
cli, out = sys.argv[1], sys.argv[2]
p = subprocess.Popen([cli, "--save", out], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
def call(cmd, **args):
    p.stdin.write(json.dumps({"command": cmd, "args": args}) + "\n"); p.stdin.flush()
    r = json.loads(p.stdout.readline())
    if not r.get("ok"): print("FAIL", cmd, r, file=sys.stderr)
    return r.get("result", {})
t = time.time()
call("create_grid", x_spacings=[6000]*4, y_spacings=[6000]*3)
H = 3500; floors = 3
normals = [[0,-1,0],[1,0,0],[0,1,0],[-1,0,0]]
for f in range(floors):
    base = f*H
    call("create_slab", grid={"x":["X1","X5"],"y":["Y1","Y4"]}, overhang=300, thickness=200, top=base)
    col = call("create_box", origin=[-300,-300,base], width=600, depth=600, height=H-200)["id"]
    call("place_on_grid", source=col)
    walls = call("create_wall", points=["X1/Y1","X5/Y1","X5/Y4","X1/Y4","X1/Y1"], thickness=200, height=H-200, base=base, justification="right")["ids"]
    for w, n in zip(walls, normals):
        count = 8 if n[1] else 6
        call("place_openings", host=w, count=count, width=1800, height=1500, sill=900, face_normal=n)
    # Interior walls with doors.
    for y in ["Y2","Y3"]:
        iw = call("create_wall", points=[f"X1/{y}", f"X5/{y}"], thickness=120, height=H-200, base=base)["ids"]
        for w in iw:
            call("place_openings", host=w, count=4, width=900, height=2100, sill=0, face_normal=[0,-1,0])
    for x in ["X2","X3","X4"]:
        call("create_wall", points=[f"{x}/Y1", f"{x}/Y2"], thickness=120, height=H-200, base=base)
        call("create_wall", points=[f"{x}/Y3", f"{x}/Y4"], thickness=120, height=H-200, base=base)
call("create_slab", grid={"x":["X1","X5"],"y":["Y1","Y4"]}, overhang=600, thickness=250, top=floors*H)
call("assign_levels", levels=[{"name":"1F","elevation":0},{"name":"2F","elevation":H},{"name":"3F","elevation":2*H},{"name":"RF","elevation":3*H}])
p.stdin.close(); rest = p.stdout.read(); p.wait()
print("done in %.1fs" % (time.time()-t), rest[:300], "exit", p.returncode)
