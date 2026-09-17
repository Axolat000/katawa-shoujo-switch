import sys, glob, os
sys.path.insert(0, os.path.dirname(__file__))
from rpyc_dump import load_rpyc, Stub
G = r"C:\Program Files (x86)\Steam\steamapps\common\Katawa Shoujo\game"
ex = {}
def walk(n):
    if isinstance(n, Stub):
        if n._cls not in ex:
            ex[n._cls] = {k: repr(v)[:150] for k, v in n.__dict__.items()}
        for v in n.__dict__.values(): walk(v)
    elif isinstance(n, (list, tuple)):
        for v in n: walk(v)
    elif isinstance(n, dict):
        for v in n.values(): walk(v)
for f in sys.argv[1:]:
    d, stmts = load_rpyc(os.path.join(G, f))
    print(f, d, type(stmts), len(stmts))
    walk(stmts)
for k, v in sorted(ex.items()):
    print("==", k)
    for a, b in v.items(): print("   ", a, "=", b)
