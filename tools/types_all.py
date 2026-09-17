import sys, glob, os
sys.path.insert(0, os.path.dirname(__file__))
from rpyc_dump import load_rpyc, Stub
G = r"C:\Program Files (x86)\Steam\steamapps\common\Katawa Shoujo\game"
types = {}
def walk(n):
    if isinstance(n, Stub):
        types[n._cls] = types.get(n._cls, 0) + 1
        for v in n.__dict__.values(): walk(v)
    elif isinstance(n, (list, tuple)):
        for v in n: walk(v)
    elif isinstance(n, dict):
        for v in n.values(): walk(v)
for f in sorted(glob.glob(G + r"\*.rpyc")):
    if any(f.endswith(s) for s in ("_ES.rpyc","_FR.rpyc","_JP.rpyc")): continue
    d, stmts = load_rpyc(f)
    walk(stmts)
for k, v in sorted(types.items()): print(v, k)
