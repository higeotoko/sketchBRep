"""docs/243: does sbr_mcp end when the client that started it ends, even when the end of its
input never comes (another process keeps a handle to the pipe open)?

    python -I tools/check_mcp_exit.py path\\to\\sbr_mcp.exe

Prints one JSON line: {"exe": ..., "ended": true/false, "seconds": ...}; exit code 0 when it ended.
"""
import json
import os
import subprocess
import sys
import tempfile
import time


def client(exe: str, ids_file: str) -> None:
    # The "client": starts the server with a pipe for its input, hands a copy of the pipe's
    # writing end to a process that outlives it (so the server never sees the end of its input),
    # prints both ids and ends without closing anything.
    server = subprocess.Popen([exe, "--headless"], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    os.set_inheritable(server.stdin.fileno(), True)
    holder = subprocess.Popen([sys.executable, "-I", "-c", "import time; time.sleep(60)"], close_fds=False)
    with open(ids_file, "w", encoding="utf-8") as f:
        json.dump({"server": server.pid, "holder": holder.pid}, f)
    os._exit(0)


def alive(pid: int) -> bool:
    if os.name == "nt":
        out = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"], capture_output=True, text=True).stdout
        return str(pid) in out
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def kill(pid: int) -> None:
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
    else:
        try:
            os.kill(pid, 9)
        except OSError:
            pass


def main() -> int:
    if len(sys.argv) == 4 and sys.argv[1] == "--client":
        client(sys.argv[2], sys.argv[3])
        return 0
    exe = os.path.abspath(sys.argv[1])
    ids_file = os.path.join(tempfile.mkdtemp(), "ids.json")
    # No pipes to the client: the processes it leaves behind must not hold this script up.
    subprocess.run([sys.executable, "-I", os.path.abspath(__file__), "--client", exe, ids_file],
                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60)
    with open(ids_file, encoding="utf-8") as f:
        ids = json.load(f)
    start = time.time()
    ended = False
    while time.time() - start < 8:
        if not alive(ids["server"]):
            ended = True
            break
        time.sleep(0.25)
    seconds = round(time.time() - start, 2)
    kill(ids["holder"])
    if not ended:
        kill(ids["server"])
    print(json.dumps({"exe": exe, "ended": ended, "seconds": seconds}))
    return 0 if ended else 1


if __name__ == "__main__":
    sys.exit(main())
