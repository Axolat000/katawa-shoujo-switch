import json, sys
d = json.load(open("build/init_dump.json", encoding="utf-8"))
objs = d["objects"]
SKIPCLS = ("renpy.style.Style",)
def show(v, depth, ind=""):
    if isinstance(v, dict) and "@" in v and len(v) == 1:
        o = objs[v["@"]]
        if o is None: return "None?"
        if o.get("#cls") in SKIPCLS:
            return "<Style %s>" % (o.get("name"),)
        if depth <= 0:
            return "<%s #%d>" % (o.get("#cls") or list(o)[0], v["@"])
        return show_obj(o, depth, ind, v["@"])
    if isinstance(v, dict) and "#t" in v:
        return "(" + ", ".join(show(x, depth, ind) for x in v["#t"]) + ")"
    return json.dumps(v, ensure_ascii=False)[:300]
def show_obj(o, depth, ind, n):
    if "#list" in o:
        return "[" + ", ".join(show(x, depth-1, ind) for x in o["#list"][:40]) + "]"
    if "#dict" in o:
        return "{" + ", ".join("%s: %s" % (show(k, depth-1, ind), show(x, depth-1, ind)) for k, x in o["#dict"][:60]) + "}"
    if "#cls" not in o:
        return json.dumps(o)
    s = "%s#%d{\n" % (o["#cls"], n)
    for part in ("f", "s"):
        for k, x in sorted(o.get(part, {}).items()):
            s += ind + "  " + k + " = " + show(x, depth-1, ind + "  ") + "\n"
    return s + ind + "}"
def get(path):
    sec, key = path.split(":", 1)
    return d[sec][key]
for p in sys.argv[2:]:
    print("=====", p)
    print(show(get(p), int(sys.argv[1])))
