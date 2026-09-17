# Runs INSIDE Ren'Py 6.16 (Python 2.7) at the end of the init phase, via pc_ref/game/zz_dump.rpy.
# Serializes the image registry, the store, styles and config to JSON, preserving shared objects.
import os, sys, types, json, math

OUT = os.environ.get("KS_DUMP_OUT", "init_dump.json")
EXPRS = os.environ.get("KS_DUMP_EXPRS")

import renpy
store = renpy.store

SKIP_ATTRS = set([
    "focus_name", "default", "_duplicatable", "transform_event", "transform_event_responder",
    "_uses_scope", "_args", "_main", "_unique", "cache", "surface_cache", "focusable",
])

class Dumper(object):
    def __init__(self):
        self.ids = {}
        self.objects = []   # list of serialized objects (index = id)
        self.keep = []
        self.depth = 0

    def ref(self, obj):
        k = id(obj)
        if k in self.ids:
            return {"@": self.ids[k]}
        n = len(self.objects)
        self.ids[k] = n
        self.keep.append(obj)
        self.objects.append(None)
        self.objects[n] = self.serialize_obj(obj)
        return {"@": n}

    def val(self, v):
        if v is None or isinstance(v, bool):
            return v
        if isinstance(v, (int, long)):
            return v
        if isinstance(v, float):
            if math.isnan(v) or math.isinf(v):
                return {"#float": repr(v)}
            return v
        if isinstance(v, str):
            try:
                return v.decode("utf-8")
            except Exception:
                return v.decode("latin-1")
        if isinstance(v, unicode):
            return v
        if isinstance(v, tuple):
            return {"#t": [self.val(i) for i in v]}
        if isinstance(v, list):
            return self.ref(v)
        if isinstance(v, dict):
            return self.ref(v)
        if isinstance(v, (set, frozenset)):
            return {"#set": [self.val(i) for i in v]}
        return self.ref(v)

    def serialize_obj(self, o):
        self.depth += 1
        try:
            if isinstance(o, list):
                return {"#list": [self.val(i) for i in o]}
            if isinstance(o, dict):
                items = []
                for k, v in o.items():
                    items.append([self.val(k), self.val(v)])
                return {"#dict": items}
            if isinstance(o, (types.FunctionType, types.BuiltinFunctionType)):
                return {"#fn": "%s.%s" % (getattr(o, "__module__", None), o.__name__)}
            if isinstance(o, types.MethodType):
                return {"#method": o.__func__.__name__, "self": self.val(o.__self__)}
            if isinstance(o, (type, types.ClassType)):
                return {"#class": "%s.%s" % (o.__module__, o.__name__)}
            if isinstance(o, types.ModuleType):
                return {"#module": o.__name__}
            cls = o.__class__
            out = {"#cls": "%s.%s" % (cls.__module__, cls.__name__)}
            if isinstance(o, renpy.style.Style):
                out["name"] = self.val(getattr(o, "name", None))
                out["parent"] = self.val(getattr(o, "parent", None))
                props = getattr(o, "properties", None)
                out["properties"] = self.val(props)
                return out
            d = getattr(o, "__dict__", None)
            if d is not None:
                fields = {}
                for k, v in d.items():
                    if k in SKIP_ATTRS:
                        continue
                    try:
                        fields[k] = self.val(v)
                    except Exception, e:
                        fields[k] = {"#err": str(e)}
                out["f"] = fields
            slots = []
            for c in cls.__mro__:
                slots.extend(getattr(c, "__slots__", ()))
            if slots:
                sf = {}
                for k in slots:
                    if hasattr(o, k) and k not in SKIP_ATTRS:
                        try:
                            sf[k] = self.val(getattr(o, k))
                        except Exception, e:
                            sf[k] = {"#err": str(e)}
                out["s"] = sf
            if d is None and not slots:
                out["repr"] = repr(o)
            return out
        finally:
            self.depth -= 1


def resolve_styles(D):
    rs_mod = renpy.style
    out = {}
    for name, sty in rs_mod.style_map.items():
        if not isinstance(sty, rs_mod.Style):
            continue
        try:
            rs_mod.build_style(sty)
        except Exception, e:
            out[name] = {"#err": str(e)}
            continue
        if not sty.cache:
            continue
        props = {}
        for prefix in ("insensitive_", "idle_", "hover_", "selected_idle_", "selected_hover_"):
            po = rs_mod.prefix_offset[prefix]
            for prop, pn in rs_mod.property_number.items():
                v = sty.cache[po + pn]
                key = prop
                if prefix != "insensitive_":
                    base = sty.cache[rs_mod.prefix_offset["insensitive_"] + pn]
                    if v is base or v == base:
                        continue
                    key = prefix + prop
                props[key] = D.val(v)
        out[name] = {"parent": D.val(sty.parent), "props": props}
    return out


def main():
    sys.setrecursionlimit(20000)
    D = Dumper()
    root = {}

    images = {}
    for k, v in renpy.display.image.images.items():
        images[" ".join(k)] = D.val(v)
    root["images"] = images

    skip_types = (types.ModuleType, type, types.ClassType)
    st = {}
    for k, v in store.__dict__.items():
        if k in ("__builtins__", "renpy", "config", "store", "persistent", "_preferences", "style", "ui", "im",
                 "layout", "theme", "define", "anim", "achievement", "multipersistent", "library", "build",
                 "iap", "gui", "_console", "suppress_overlay", "__name__", "__doc__", "__package__"):
            continue
        if isinstance(v, skip_types):
            continue
        try:
            st[k] = D.val(v)
        except Exception, e:
            st[k] = {"#err": str(e)}
    root["store"] = st

    cfg = {}
    for k in dir(renpy.config):
        if k.startswith("__"):
            continue
        v = getattr(renpy.config, k)
        if isinstance(v, (types.ModuleType, types.FunctionType)):
            continue
        try:
            cfg[k] = D.val(v)
        except Exception, e:
            cfg[k] = {"#err": str(e)}
    root["config"] = cfg

    styles = {}
    for k, v in renpy.style.style_map.items():
        styles[k] = D.val(v)
    root["styles"] = styles

    root["available_languages"] = D.val(list(store.available_languages))
    langs = {}
    for lang in store.available_languages:
        store.switch_language(lang)
        chars = {}
        for k, v in store.__dict__.items():
            if isinstance(v, renpy.character.ADVCharacter):
                chars[k] = D.val(v)
        rs = resolve_styles(D)
        ds = store.displayStrings
        langs[lang] = {"characters": chars, "styles": rs, "displayStrings": D.val(ds),
                       "s_scenes": D.val(store.s_scenes)}
    root["languages"] = langs
    store.switch_language(store.master_language)

    if EXPRS and os.path.exists(EXPRS):
        ev = {}
        for line in open(EXPRS).read().decode("utf-8").split("\n"):
            e = line.strip()
            if not e or e in ev:
                continue
            try:
                ev[e] = D.val(eval(e, store.__dict__))
            except Exception, ex:
                ev[e] = {"#err": "%s: %s" % (ex.__class__.__name__, ex)}
        root["exprs"] = ev

    root["objects"] = D.objects
    f = open(OUT, "wb")
    json.dump(root, f, separators=(",", ":"))
    f.close()
    sys.stderr.write("dump: %d objects, %d images\n" % (len(D.objects), len(images)))

try:
    main()
except Exception:
    import traceback
    f = open(OUT + ".err.txt", "w")
    traceback.print_exc(file=f)
    f.close()
