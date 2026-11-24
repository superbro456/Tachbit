import os, sys, subprocess, shutil, json
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from l1 import L as L1
try:
    from l2 import L as L2
except ImportError:
    L2 = []
L = L1 + L2
TB = os.path.join(os.path.dirname(HERE), "..", "build", "tb")
LIB = os.path.join(os.path.dirname(HERE), "..", "lib")
W = "/tmp/tb-learn-work"
OUT = os.path.join(HERE, "lang_out")
os.makedirs(OUT, exist_ok=True)
only = set(int(a) for a in sys.argv[1:])
bad = 0
for n, les in enumerate(L, 1):
    if only and n not in only:
        continue
    for k, part in enumerate(les["parts"]):
        head, txt, code, mode = part[:4]
        if mode == "none":
            continue
        d = os.path.join(W, f"{n:02d}_{k}")
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        f = os.path.join(d, "main.tb")
        open(f, "w").write(code)
        for extra in part[4:5]:
            for name, body in extra.items():
                open(os.path.join(d, name), "w").write(body)
        for lb in ("ui.tb", "anim.tb", "physics.tb"):
            shutil.copy(os.path.join(LIB, lb), d)
        stdin = ""
        if mode.startswith("in:"):
            stdin = mode[3:]
            mode = "run"
        cmd = [TB, "check" if mode in ("check", "lib-check") else "run", "main.tb"]
        p = subprocess.run(cmd, cwd=d, input=stdin, capture_output=True, text=True, timeout=300)
        o = p.stdout + p.stderr
        ok = p.returncode == 0 or part[5:6] == ["fails"] if len(part) > 5 else p.returncode == 0
        open(os.path.join(OUT, f"{n:02d}_{k}.txt"), "w").write(o)
        tag = "ok " if ok else "BAD"
        if not ok:
            bad += 1
        print(f"{tag} {n:02d}.{k} {head}: " + o.strip().replace("\n", " | ")[:300])
print("bad:", bad)
