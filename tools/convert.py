"""Katawa Shoujo -> native engine data converter.

Produces romfs/data/game.bin: a value graph holding the linearized script (all languages), every
Python snippet as an AST, the initial store dumped from the real Ren'Py init (build/init_dump.json),
the image registry, per-language characters/styles/strings and a config subset.
"""
import glob, json, os, struct, subprocess, sys, re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from rpyc_dump import load_rpyc, Stub  # noqa: E402

GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Katawa Shoujo\game"
REF_LIB = os.path.join(ROOT, "..", "pc_ref", "lib")
PY2 = os.path.join(REF_LIB, "windows-i686", "python.exe")
BUILD = os.path.join(ROOT, "build")
OUT = os.path.join(ROOT, "romfs", "data", "game.bin")


# ----------------------------------------------------------------------------- value model

class Code(object):
    __slots__ = ("idx",)

    def __init__(self, idx):
        self.idx = idx


class Inst(object):
    """An object instance: class name + fields."""
    __slots__ = ("cls", "fields")

    def __init__(self, cls, fields=None):
        self.cls = cls
        self.fields = fields if fields is not None else {}


class FuncRef(object):
    __slots__ = ("name",)

    def __init__(self, name):
        self.name = name


class RList(list):
    """A list that must stay a shared mutable object."""
    pass


class RDict(dict):
    pass


# ----------------------------------------------------------------------------- python sources

class Codes(object):
    def __init__(self):
        self.items = []
        self.index = {}

    def add(self, mode, src):
        if src is None:
            return None
        if isinstance(src, bytes):
            src = src.decode("utf-8")
        key = (mode, src)
        i = self.index.get(key)
        if i is None:
            i = len(self.items)
            self.items.append([mode, src])
            self.index[key] = i
        return Code(i)

    def expr(self, src):
        if src is None:
            return None
        return self.add("eval", src)

    def compile(self):
        os.makedirs(BUILD, exist_ok=True)
        inp = os.path.join(BUILD, "py_sources.json")
        out = os.path.join(BUILD, "py_ast.json")
        with open(inp, "w", encoding="utf-8") as f:
            json.dump(self.items, f)
        env = dict(os.environ)
        env["PYTHONHOME"] = os.path.join(REF_LIB, "pythonlib2.7")
        env["PYTHONPATH"] = os.path.join(REF_LIB, "pythonlib2.7") + ";" + os.path.join(REF_LIB, "windows-i686", "Lib")
        subprocess.check_call([PY2, "-O", os.path.join(HERE, "py2ast.py"), inp, out], env=env)
        with open(out, encoding="utf-8") as f:
            asts = json.load(f)
        errors = 0
        for (mode, src), a in zip(self.items, asts):
            if isinstance(a, dict) and "error" in a:
                errors += 1
                if errors < 30:
                    print("PYERR", a["error"][:300])
        print("python snippets: %d (%d errors)" % (len(asts), errors))
        return asts


CODES = Codes()


# ----------------------------------------------------------------------------- rpyc loading

def s(v):
    if isinstance(v, bytes):
        return v.decode("utf-8", "replace")
    return v


def norm(n):
    if isinstance(n, Stub):
        d = {s(k): norm(v) for k, v in list(n.__dict__.items())}
        n.__dict__.clear()
        n.__dict__.update(d)
        return n
    if isinstance(n, bytes):
        return s(n)
    if isinstance(n, list):
        return [norm(x) for x in n]
    if isinstance(n, tuple):
        return tuple(norm(x) for x in n)
    if isinstance(n, dict):
        return {norm(k): norm(v) for k, v in n.items()}
    return n


def cls(n):
    return n._cls.rsplit(".", 1)[1] if isinstance(n, Stub) else type(n).__name__


def pysrc(pycode):
    st = pycode.__dict__.get("_state")
    return s(st[1])


# ----------------------------------------------------------------------------- script linearization

# opcodes (keep in sync with source/script.h)
OP = {n: i for i, n in enumerate([
    "NOP", "LABEL", "SAY", "SHOW", "SCENE", "HIDE", "WITH", "PYTHON", "JUMP", "JIFNOT", "MENU", "CALL",
    "RETURN", "PLAY", "QUEUE", "STOP", "WINDOW", "NVL", "JUMPIN", "JUMPOUT", "PAUSE", "USER",
])}


def split_words(line):
    """Tokenize a user-statement line keeping bracketed / quoted simple expressions together."""
    toks = []
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if c.isspace():
            i += 1
            continue
        start = i
        depth = 0
        while i < n:
            c = line[i]
            if c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
            elif c in "\"'":
                q = c
                i += 1
                while i < n and line[i] != q:
                    if line[i] == "\\":
                        i += 1
                    i += 1
            elif c.isspace() and depth == 0:
                break
            i += 1
        toks.append(line[start:i])
    return toks


def merge_simple_expr(toks):
    """Ren'Py simple expressions may contain '.' attribute chains and calls; tokens are already
    grouped by brackets. Also rejoin things like 'Dissolve' '(1.0)' split by spaces."""
    out = []
    for t in toks:
        if out and t.startswith("(") and not out[-1] in ("fadein", "fadeout", "channel"):
            out[-1] += t
        else:
            out.append(t)
    return out


class Linearizer(object):
    def __init__(self):
        self.ops = []
        self.labels = {}
        self.fixups = []  # (op index, arg index, label name)
        self.unknown = {}
        self.defs = []  # init-time code (function/class defs)
        self.fileorder = 0
        self.is_common = False

    def emit(self, *args):
        self.ops.append(list(args))
        return len(self.ops) - 1

    def pc(self):
        return len(self.ops)

    def imspec(self, im):
        name, expression, tag, at_list, layer, zorder, behind = im[:7]
        return (
            tuple(name) if name else None,
            CODES.expr(expression),
            tag,
            [CODES.expr(a) for a in (at_list or [])],
            layer or "master",
            CODES.expr(zorder),
            list(behind or []),
        )

    def block(self, stmts, filename):
        for n in stmts:
            self.stmt(n, filename)

    def stmt(self, n, fn):
        c = cls(n)
        line = n.__dict__.get("linenumber", 0)
        if c == "Label":
            if n.name in self.labels:
                print("duplicate label", n.name, fn)
            self.labels[n.name] = self.pc()
            params = None
            pi = n.__dict__.get("parameters")
            if pi is not None:
                params = [(k, CODES.expr(v)) for k, v in pi.parameters]
            self.emit(OP["LABEL"], n.name, params)
            self.block(n.block, fn)
        elif c == "Say":
            who = None
            if n.who:
                who = CODES.expr(n.who)
            self.emit(OP["SAY"], who, n.what, bool(n.interact), CODES.expr(n.with_),
                      tuple(n.attributes) if n.__dict__.get("attributes") else None, fn, line)
        elif c == "Show":
            self.emit(OP["SHOW"], self.imspec(n.imspec), convert_atl(n.atl) if n.atl else None)
        elif c == "Scene":
            self.emit(OP["SCENE"], self.imspec(n.imspec) if n.imspec else None, n.layer or "master",
                      convert_atl(n.atl) if n.atl else None)
        elif c == "Hide":
            self.emit(OP["HIDE"], self.imspec(n.imspec))
        elif c == "With":
            self.emit(OP["WITH"], CODES.expr(n.expr), CODES.expr(n.paired) if n.paired else None)
        elif c == "Python":
            self.emit(OP["PYTHON"], CODES.add("exec", pysrc(n.code)), bool(n.hide), fn, line)
        elif c == "EarlyPython" or c == "Init":
            # init code already ran in the dump; keep function/class definitions for the runtime
            prio = n.priority if c == "Init" else -9999
            if c == "Init":
                for sub in n.block:
                    if cls(sub) == "Python":
                        self.defs.append((prio, self.fileorder, CODES.add("exec", pysrc(sub.code)), self.is_common))
            else:
                self.defs.append((prio, self.fileorder, CODES.add("exec", pysrc(n.code)), self.is_common))
        elif c in ("Image", "Transform", "Define"):
            pass
        elif c == "Call":
            args = None
            ai = n.__dict__.get("arguments")
            if ai is not None:
                args = [(k, CODES.expr(v)) for k, v in ai.arguments]
            if n.expression:
                self.emit(OP["CALL"], None, CODES.expr(n.label), args)
            else:
                self.emit(OP["CALL"], n.label, None, args)
        elif c == "Jump":
            if n.expression:
                self.emit(OP["JUMP"], None, CODES.expr(n.target))
            else:
                self.emit(OP["JUMP"], n.target, None)
        elif c == "Return":
            self.emit(OP["RETURN"], CODES.expr(n.expression))
        elif c == "Pass":
            self.emit(OP["NOP"])
        elif c == "If":
            ends = []
            for cond, blk in n.entries:
                j = self.emit(OP["JIFNOT"], CODES.expr(cond), -1)
                self.block(blk, fn)
                ends.append(self.emit(OP["JUMP"], -1, None))
                self.ops[j][2] = self.pc()
            for e in ends:
                self.ops[e][1] = self.pc()
        elif c == "While":
            top = self.pc()
            j = self.emit(OP["JIFNOT"], CODES.expr(n.condition), -1)
            self.block(n.block, fn)
            self.emit(OP["JUMP"], top, None)
            self.ops[j][2] = self.pc()
        elif c == "Menu":
            items = []
            m = self.emit(OP["MENU"], None, n.set and CODES.expr(n.set), CODES.expr(n.with_), fn, line)
            ends = []
            for label, cond, blk in n.items:
                if blk is None:
                    items.append([label, CODES.expr(cond), -1])
                else:
                    target = self.pc()
                    items.append([label, CODES.expr(cond), target])
                    self.block(blk, fn)
                    ends.append(self.emit(OP["JUMP"], -1, None))
            self.ops[m][1] = items
            # no choice / fallthrough: continue after the menu
            for e in ends:
                self.ops[e][1] = self.pc()
            # the MENU op jumps to the chosen target; if nothing chosen we go past all blocks
            self.ops[m].append(self.pc())
        elif c == "UserStatement":
            self.user(n.line, fn, line)
        else:
            self.unknown[c] = self.unknown.get(c, 0) + 1

    def user(self, line, fn, lineno):
        toks = merge_simple_expr(split_words(line))
        kw = toks[0]
        if kw in ("play", "queue"):
            channel = toks[1]
            file = toks[2]
            opts = {"fadein": "0", "fadeout": "None", "channel": None, "loop": None, "if_changed": False}
            i = 3
            while i < len(toks):
                t = toks[i]
                if t in ("fadein", "fadeout", "channel"):
                    opts[t] = toks[i + 1]
                    i += 2
                elif t == "loop":
                    opts["loop"] = True
                    i += 1
                elif t == "noloop":
                    opts["loop"] = False
                    i += 1
                elif t == "if_changed":
                    opts["if_changed"] = True
                    i += 1
                else:
                    print("user stmt parse:", line)
                    i += 1
            ch = CODES.expr(opts["channel"]) if opts["channel"] else channel
            self.emit(OP["PLAY" if kw == "play" else "QUEUE"], ch, CODES.expr(file), CODES.expr(opts["fadein"]),
                      CODES.expr(opts["fadeout"]), opts["loop"], opts["if_changed"])
        elif kw == "stop":
            channel = toks[1]
            fadeout = "None"
            i = 2
            chx = None
            while i < len(toks):
                if toks[i] == "fadeout":
                    fadeout = toks[i + 1]
                    i += 2
                elif toks[i] == "channel":
                    chx = toks[i + 1]
                    i += 2
                else:
                    print("user stmt parse:", line)
                    i += 1
            self.emit(OP["STOP"], CODES.expr(chx) if chx else channel, CODES.expr(fadeout))
        elif kw == "window":
            trans = " ".join(toks[2:]) if len(toks) > 2 else None
            self.emit(OP["WINDOW"], toks[1] == "show", CODES.expr(trans))
        elif kw == "nvl":
            if toks[1] == "clear":
                self.emit(OP["NVL"], "clear", None)
            else:
                self.emit(OP["NVL"], toks[1], CODES.expr(" ".join(toks[2:])))
        elif kw in ("jump_in", "jump_out"):
            if toks[1] == "expression":
                self.emit(OP["JUMPIN" if kw == "jump_in" else "JUMPOUT"], None, CODES.expr(" ".join(toks[2:])))
            else:
                self.emit(OP["JUMPIN" if kw == "jump_in" else "JUMPOUT"], toks[1], None)
        elif kw == "pause":
            self.emit(OP["PAUSE"], CODES.expr(" ".join(toks[1:])) if len(toks) > 1 else None)
        else:
            print("unknown user statement:", line)
            self.emit(OP["USER"], line)


# ----------------------------------------------------------------------------- ATL (from rpyc AST stubs)

def convert_atl(raw):
    """Convert an ATL RawBlock stub (from a .rpyc) into the same Inst structure used for dumped ATL."""
    return atl_stub(raw)


def atl_stub(n):
    c = cls(n)
    d = n.__dict__
    if c == "RawBlock":
        return Inst("atl.Block", {"stmts": RList([atl_stub(x) for x in d["statements"]]),
                                  "animation": bool(d.get("animation"))})
    return atl_generic(c, d, lambda x: atl_stub(x))


def atl_generic(c, d, sub):
    if c == "RawMultipurpose":
        return Inst("atl.Multi", {
            "warper": d.get("warper"),
            "warp_function": CODES.expr(d.get("warp_function")),
            "duration": CODES.expr(d.get("duration") or "0"),
            "properties": RList([(k, CODES.expr(v)) for k, v in d.get("properties") or []]),
            "expressions": RList([(CODES.expr(e), CODES.expr(w)) for e, w in d.get("expressions") or []]),
            "revolution": d.get("revolution"),
            "circles": CODES.expr(d.get("circles") or "0"),
            "splines": RList([(name, RList([CODES.expr(x) for x in exprs])) for name, exprs in d.get("splines") or []]),
        })
    if c == "RawRepeat":
        return Inst("atl.Repeat", {"repeats": CODES.expr(d.get("repeats"))})
    if c == "RawParallel":
        return Inst("atl.Parallel", {"blocks": RList([sub(b) for b in d["blocks"]])})
    if c == "RawChoice":
        return Inst("atl.Choice", {"choices": RList([(CODES.expr(ch), sub(b)) for ch, b in d["choices"]])})
    if c == "RawFunction":
        return Inst("atl.Function", {"expr": CODES.expr(d["expr"])})
    if c == "RawOn":
        return Inst("atl.On", {"handlers": RDict({k: sub(v) for k, v in d["handlers"].items()})})
    if c == "RawTime":
        return Inst("atl.Time", {"time": CODES.expr(d["time"])})
    if c == "RawEvent":
        return Inst("atl.Event", {"name": d["name"]})
    if c == "RawContainsExpr":
        return Inst("atl.ContainsExpr", {"expression": CODES.expr(d["expression"])})
    if c == "RawChild":
        return Inst("atl.Child", {"children": RList([sub(b) for b in d["children"]])})
    raise Exception("unknown ATL node " + c)


# ----------------------------------------------------------------------------- dump conversion

class DumpConverter(object):
    def __init__(self, dump):
        self.d = dump
        self.objs = dump["objects"]
        self.memo = {}
        self.missing_cls = {}

    def v(self, x):
        if x is None or isinstance(x, (bool, int, float, str)):
            return x
        if isinstance(x, dict):
            if "@" in x and len(x) == 1:
                return self.obj(x["@"])
            if "#t" in x:
                return tuple(self.v(i) for i in x["#t"])
            if "#set" in x:
                return Inst("#set", {"items": RList([self.v(i) for i in x["#set"]])})
            if "#float" in x:
                return float(x["#float"])
            if "#err" in x:
                return None
        raise Exception("bad dump value %r" % (x,))

    def obj(self, n):
        if n in self.memo:
            return self.memo[n]
        o = self.objs[n]
        if o is None:
            self.memo[n] = None
            return None
        if "#list" in o:
            r = RList()
            self.memo[n] = r
            r.extend(self.v(i) for i in o["#list"])
            return r
        if "#dict" in o:
            r = RDict()
            self.memo[n] = r
            for k, val in o["#dict"]:
                k = self.v(k)
                if isinstance(k, list):
                    k = tuple(k)
                r[k] = self.v(val)
            return r
        if "#fn" in o:
            r = FuncRef(o["#fn"])
            self.memo[n] = r
            return r
        if "#class" in o:
            r = FuncRef(o["#class"])
            self.memo[n] = r
            return r
        if "#method" in o:
            r = Inst("#method", {})
            self.memo[n] = r
            r.fields["name"] = o["#method"]
            r.fields["self"] = self.v(o["self"])
            return r
        if "#module" in o:
            r = Inst("#module", {"name": o["#module"]})
            self.memo[n] = r
            return r
        c = o["#cls"]
        r = Inst(c, {})
        self.memo[n] = r
        if c == "renpy.style.Style":
            r.fields["name"] = self.v(o.get("name"))
            r.fields["parent"] = self.v(o.get("parent"))
            r.fields["properties"] = self.v(o.get("properties"))
            return r
        fields = {}
        fields.update(o.get("s", {}))
        fields.update(o.get("f", {}))
        conv = ATL_DUMP.get(c)
        if conv:
            r.cls, r.fields = conv(self, fields)
            return r
        if c in SKIP_FIELDS_BY_CLASS:
            for k in SKIP_FIELDS_BY_CLASS[c]:
                fields.pop(k, None)
        for k in GLOBAL_SKIP:
            fields.pop(k, None)
        for k, val in fields.items():
            r.fields[k] = self.v(val)
        return r


GLOBAL_SKIP = ("__doc__", "predict_function", "transform_event_responder")
SKIP_FIELDS_BY_CLASS = {
    "renpy.display.motion.ATLTransform": ("arguments", "state", "atl_state", "block", "children", "offsets",
                                          "st", "at", "st_offset", "at_offset", "child_st_base", "done", "active",
                                          "hide_request", "hide_response", "replaced_request", "replaced_response",
                                          "last_transform_event", "last_child_transform_event", "function"),
    "renpy.display.motion.Transform": ("arguments", "atl_state", "children", "offsets", "st", "at", "st_offset",
                                       "at_offset", "child_st_base", "active", "hide_request", "hide_response",
                                       "replaced_request", "replaced_response", "last_transform_event",
                                       "last_child_transform_event"),
}


def _atl_block(dc, f):
    return "atl.Block", {"stmts": RList(dc.v(f["statements"])), "animation": bool(dc.v(f.get("animation")))}


def _atl_multi(dc, f):
    d = {k: dc.v(val) for k, val in f.items()}
    d["properties"] = [tuple(p) for p in d.get("properties") or []]
    d["expressions"] = [tuple(p) for p in d.get("expressions") or []]
    d["splines"] = [tuple(p) for p in d.get("splines") or []]
    r = atl_generic("RawMultipurpose", d, lambda x: x)
    return r.cls, r.fields


def _atl_conv(name):
    def f(dc, fields):
        d = {k: dc.v(val) for k, val in fields.items()}
        if name == "RawChoice":
            d["choices"] = [tuple(p) for p in d["choices"]]
        r = atl_generic(name, d, lambda x: x)
        return r.cls, r.fields
    return f


def _param_info(dc, fields):
    d = {k: dc.v(val) for k, val in fields.items()}
    return "ParameterInfo", {
        "parameters": RList([(k, CODES.expr(v)) for k, v in (d.get("parameters") or [])]),
        "positional": RList(d.get("positional") or []),
        "extrapos": d.get("extrapos"),
        "extrakw": d.get("extrakw"),
    }


ATL_DUMP = {
    "renpy.atl.RawBlock": _atl_block,
    "renpy.atl.RawMultipurpose": _atl_multi,
    "renpy.atl.RawRepeat": _atl_conv("RawRepeat"),
    "renpy.atl.RawParallel": _atl_conv("RawParallel"),
    "renpy.atl.RawChoice": _atl_conv("RawChoice"),
    "renpy.atl.RawFunction": _atl_conv("RawFunction"),
    "renpy.atl.RawOn": _atl_conv("RawOn"),
    "renpy.atl.RawTime": _atl_conv("RawTime"),
    "renpy.atl.RawEvent": _atl_conv("RawEvent"),
    "renpy.atl.RawContainsExpr": _atl_conv("RawContainsExpr"),
    "renpy.atl.RawChild": _atl_conv("RawChild"),
    "renpy.ast.ParameterInfo": _param_info,
}

STORE_SKIP_CLS = ("renpy.style.StyleManager", "renpy.display.behavior.Keymap", "store.Steamapi",
                  "renpy.persistent.Persistent", "renpy.preferences.Preferences", "_sre.SRE_Pattern",
                  "store._Theme", "renpy.defaultstore._Config", "renpy.python.StoreModule")

CONFIG_KEYS = ("rollback_enabled", "default_text_cps", "screen_width", "screen_height", "enter_transition",
               "exit_transition", "intra_transition", "main_game_transition", "game_main_transition",
               "end_game_transition", "after_load_transition", "window_show_transition", "window_hide_transition",
               "hard_rollback_limit", "rollback_length", "implicit_with_none", "skip_delay", "default_afm_time",
               "nvl_page_ctc", "nvl_page_ctc_position", "empty_window", "font_replacement_map", "label_overrides",
               "allow_skipping", "fast_skipping", "has_autosave", "window_title", "main_menu_music", "r18",
               "overlay_during_with", "with_callback", "skip_indicator", "window_icon", "file_entry_format",
               "time_format", "mouse", "developer", "hyperlink_callback", "say_menu_text_filter", "fade_music",
               "minimumvolume", "auto_choice_delay", "joystick_keys", "main_menu", "game_menu")


def convert_dump(dc):
    d = dc.d
    root = RDict()
    images = RDict()
    for k, v in d["images"].items():
        images[k] = dc.v(v)
    root["images"] = images

    store = RDict()
    for k, v in d["store"].items():
        val = dc.v(v)
        if isinstance(val, Inst) and val.cls in STORE_SKIP_CLS:
            continue
        store[k] = val
    root["store"] = store

    cfg = RDict()
    for k in CONFIG_KEYS:
        if k in d["config"]:
            cfg[k] = dc.v(d["config"][k])
    root["config"] = cfg

    langs = RDict()
    for lang, ld in d["languages"].items():
        L = RDict()
        L["characters"] = RDict({k: dc.v(v) for k, v in ld["characters"].items()})
        L["displayStrings"] = dc.v(ld["displayStrings"])
        L["s_scenes"] = dc.v(ld["s_scenes"])
        styles = RDict()
        for name, sd in ld["styles"].items():
            if "props" not in sd:
                continue
            props = RDict()
            for pk, pv in sd["props"].items():
                props[pk] = dc.v(pv)
            styles[name] = RDict({"parent": dc.v(sd["parent"]), "props": props})
        L["styles"] = styles
        langs[lang] = L
    root["languages"] = langs
    root["available_languages"] = dc.v(d["available_languages"])
    return root


# ----------------------------------------------------------------------------- binary encoder

class Encoder(object):
    T_NONE, T_FALSE, T_TRUE, T_INT, T_FLOAT, T_STR, T_REF, T_CODE, T_TUPLE = range(9)
    K_LIST, K_DICT, K_INST, K_FUNC = 1, 2, 3, 4

    def __init__(self):
        self.strings = []
        self.sindex = {}
        self.objects = []  # encoded bytes per object
        self.oindex = {}
        self.pending = []

    def sid(self, st):
        i = self.sindex.get(st)
        if i is None:
            i = len(self.strings)
            self.strings.append(st)
            self.sindex[st] = i
        return i

    @staticmethod
    def varint(n, out):
        n = (n << 1) if n >= 0 else (((-n) << 1) - 1)
        while True:
            b = n & 0x7F
            n >>= 7
            if n:
                out.append(b | 0x80)
            else:
                out.append(b)
                break

    @staticmethod
    def uvarint(n, out):
        while True:
            b = n & 0x7F
            n >>= 7
            if n:
                out.append(b | 0x80)
            else:
                out.append(b)
                break

    def ref(self, o):
        k = id(o)
        i = self.oindex.get(k)
        if i is None:
            i = len(self.objects)
            self.oindex[k] = i
            self.objects.append(None)
            self.pending.append((i, o))
        return i

    def val(self, v, out):
        if v is None:
            out.append(self.T_NONE)
        elif v is False:
            out.append(self.T_FALSE)
        elif v is True:
            out.append(self.T_TRUE)
        elif isinstance(v, int):
            if v >= (1 << 62) or v < -(1 << 62):
                out.append(self.T_FLOAT)
                out.extend(struct.pack("<d", float(v)))
            else:
                out.append(self.T_INT)
                self.varint(v, out)
        elif isinstance(v, float):
            out.append(self.T_FLOAT)
            out.extend(struct.pack("<d", v))
        elif isinstance(v, str):
            out.append(self.T_STR)
            self.uvarint(self.sid(v), out)
        elif isinstance(v, Code):
            out.append(self.T_CODE)
            self.uvarint(v.idx, out)
        elif isinstance(v, tuple):
            out.append(self.T_TUPLE)
            self.uvarint(len(v), out)
            for x in v:
                self.val(x, out)
        elif isinstance(v, (list, dict, Inst, FuncRef)):
            out.append(self.T_REF)
            self.uvarint(self.ref(v), out)
        else:
            raise Exception("cannot encode %r" % (v,))

    def encode_obj(self, o):
        out = bytearray()
        if isinstance(o, list):
            out.append(self.K_LIST)
            self.uvarint(len(o), out)
            for x in o:
                self.val(x, out)
        elif isinstance(o, dict):
            out.append(self.K_DICT)
            self.uvarint(len(o), out)
            for k, x in o.items():
                self.val(k, out)
                self.val(x, out)
        elif isinstance(o, Inst):
            out.append(self.K_INST)
            self.uvarint(self.sid(o.cls), out)
            self.uvarint(len(o.fields), out)
            for k, x in o.fields.items():
                self.uvarint(self.sid(k), out)
                self.val(x, out)
        elif isinstance(o, FuncRef):
            out.append(self.K_FUNC)
            self.uvarint(self.sid(o.name), out)
        return out

    def encode(self, root):
        rootbuf = bytearray()
        self.val(root, rootbuf)
        while self.pending:
            i, o = self.pending.pop()
            self.objects[i] = self.encode_obj(o)
        out = bytearray(b"KSG1")
        self.uvarint(len(self.strings), out)
        for st in self.strings:
            b = st.encode("utf-8")
            self.uvarint(len(b), out)
            out.extend(b)
        self.uvarint(len(self.objects), out)
        for ob in self.objects:
            out.extend(ob)
        out.extend(rootbuf)
        return bytes(out)


# ----------------------------------------------------------------------------- python AST -> values

AST_FIELDS = {}


def ast_value(n):
    """py2 AST JSON -> compact tuple form: (NodeName, field values in _fields order)."""
    if isinstance(n, dict):
        t = n.get("_")
        if t == "#f":
            return float(n["v"])
        if t == "#float":
            return float(n["v"])
        if t is None:
            return None
        keys = [k for k in n.keys() if k not in ("_", "@l")]
        prev = AST_FIELDS.get(t)
        if prev is None:
            AST_FIELDS[t] = keys
        elif prev != keys:
            keys = prev
        return (t,) + tuple(ast_value(n[k]) for k in keys)
    if isinstance(n, list):
        return RList(ast_value(i) for i in n)
    return n


# ----------------------------------------------------------------------------- main

LANG_RE = re.compile(r"_(DE|ES|FR|JP|KR|PL|PT-BR|RU|ZH|ZH-HANT)\.rpyc$")


def script_files():
    files = []
    for f in sorted(glob.glob(os.path.join(GAME, "*.rpyc"))):
        files.append(f)
    for f in sorted(glob.glob(os.path.join(ROOT, "extracted_lang", "*.rpyc"))):
        files.append(f)
    for f in sorted(glob.glob(os.path.join(GAME, "..", "renpy", "common", "*.rpyc"))):
        files.append(f)
    return files


def write_values(path, root):
    enc = Encoder()
    data = enc.encode(root)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    print("%s: %d bytes, %d strings, %d objects" % (os.path.basename(path), len(data), len(enc.strings), len(enc.objects)))


def main():
    base = Linearizer()
    langs = {}
    for f in script_files():
        m = LANG_RE.search(os.path.basename(f))
        lang = m.group(1).lower() if m else None
        if lang and os.path.basename(f).startswith("ui-strings"):
            lang = None  # init only
        lin = base if lang is None else langs.setdefault(lang, Linearizer())
        data, stmts = load_rpyc(f)
        stmts = norm(stmts)
        lin.is_common = os.path.normpath(os.path.join(GAME, "..", "renpy")) in os.path.normpath(f)
        lin.fileorder += 1
        lin.block(stmts, os.path.basename(f))
    print("base ops: %d, labels: %d, unknown: %r" % (len(base.ops), len(base.labels), base.unknown))
    for lang, lin in sorted(langs.items()):
        print("  %s ops: %d labels: %d defs: %d" % (lang, len(lin.ops), len(lin.labels), len(lin.defs)))

    with open(os.path.join(BUILD, "init_dump.json"), encoding="utf-8") as f:
        dump = json.load(f)
    dc = DumpConverter(dump)
    root = convert_dump(dc)

    asts = CODES.compile()
    codes = RList()
    for a in asts:
        if isinstance(a, dict) and "error" in a:
            codes.append(None)
        else:
            codes.append(ast_value(a))
    root["codes"] = codes
    root["ops"] = RList(tuple(op) for op in base.ops)
    root["labels"] = RDict(base.labels)
    base.defs.sort(key=lambda d: (d[3] is False, d[0], d[1]))
    root["defs"] = RList((d[2], d[3]) for d in base.defs)
    root["ast_fields"] = RDict({k: tuple(v) for k, v in AST_FIELDS.items()})
    root["script_languages"] = RList(sorted(langs.keys()))
    write_values(OUT, root)
    for lang, lin in sorted(langs.items()):
        write_values(os.path.join(os.path.dirname(OUT), "script_%s.bin" % lang),
                     RDict({"ops": RList(tuple(op) for op in lin.ops), "labels": RDict(lin.labels)}))
    with open(os.path.join(BUILD, "ast_fields.json"), "w") as f:
        json.dump(AST_FIELDS, f, indent=1)


if __name__ == "__main__":
    main()
