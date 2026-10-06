import os, pty, select, sys, tempfile, threading, time
LUIT = "transcoder/src/luit"
def run(argv, data, label):
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "o")
        pid, fd = pty.fork()
        if pid == 0:
            os.execvp(argv[0], argv + ["sh", "-c", 'stty raw -echo; exec cat > "$0"', out]); os._exit(1)
        time.sleep(0.5)
        t0 = time.time()
        def w():
            v = memoryview(data); p = 0
            while p < len(v): p += os.write(fd, v[p:p+65536])
        th = threading.Thread(target=w, daemon=True); th.start()
        while time.time() - t0 < 120:
            r,_,_ = select.select([fd],[],[],0.1)
            if r:
                try: os.read(fd, 65536)
                except OSError: break
            if os.path.exists(out) and os.path.getsize(out) >= len(data) * 0 + want[label]: break
        dt = time.time() - t0
        size = os.path.getsize(out) if os.path.exists(out) else 0
        os.kill(pid, 9); os.waitpid(pid, 0); os.close(fd)
        print(f"{label:28s} {len(data)/1e6:.2f} MB in {dt:6.2f} s -> {size} bytes arrived", flush=True)
ascii_ = b"abcdefghij" * 100_000
cjk = ("中" * 100_000).encode()
want = {"pty+cat (no luit)": len(ascii_), "luit ISO8859-1 ascii": len(ascii_), "luit GB18030 cjk": 200_000, "luit GB18030 ascii": len(ascii_)}
run([], ascii_, "pty+cat (no luit)") if False else None
# no-luit baseline: exec sh directly
def run_plain():
    run(["env"], ascii_, "pty+cat (no luit)")
run_plain()
run([LUIT, "-encoding", "ISO8859-1", "--"], ascii_, "luit ISO8859-1 ascii")
run([LUIT, "-encoding", "GB18030", "--"], ascii_, "luit GB18030 ascii")
run([LUIT, "-encoding", "GB18030", "--"], cjk, "luit GB18030 cjk")
