import json, sys, collections
d = json.load(open("build/init_dump.json", encoding="utf-8"))
objs = d["objects"]
c = collections.Counter()
ex = {}
for i, o in enumerate(objs):
    k = "NONE" if o is None else (o.get("#cls") or next(iter(o.keys())))
    c[k] += 1
    ex.setdefault(k, i)
for k, v in c.most_common():
    print(v, k)
print("languages", d["objects"][d["available_languages"]["@"]])
cfg = d["config"]
for k in ("rollback_enabled", "default_text_cps", "screen_width", "enter_transition", "hard_rollback_limit", "rollback_length", "implicit_with_none", "with_callback", "nvl_paged_rollback", "skip_delay", "archives", "developer", "r18", "default_afm_time"):
    print(k, json.dumps(cfg.get(k))[:200])
