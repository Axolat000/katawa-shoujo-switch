#include "value.h"
#include <cmath>
#include <cstdarg>

bool Value::isObj(OK k) const { return t == T::Obj && o->kind == k; }

ListObj *Value::list() const {
    return (t == T::Obj && (o->kind == OK::List || o->kind == OK::Tuple)) ? static_cast<ListObj *>(o.get()) : nullptr;
}
DictObj *Value::dict() const {
    return (t == T::Obj && (o->kind == OK::Dict || o->kind == OK::Set)) ? static_cast<DictObj *>(o.get()) : nullptr;
}
InstObj *Value::inst() const { return (t == T::Obj && o->kind == OK::Inst) ? static_cast<InstObj *>(o.get()) : nullptr; }
FuncObj *Value::func() const { return (t == T::Obj && o->kind == OK::Func) ? static_cast<FuncObj *>(o.get()) : nullptr; }

bool Value::truthy() const {
    switch (t) {
    case T::None:
        return false;
    case T::Bool:
    case T::Int:
        return i != 0;
    case T::Float:
        return f != 0.0;
    case T::Str:
        return !sp->s.empty();
    case T::Code:
        return true;
    case T::Obj:
        switch (o->kind) {
        case OK::List:
        case OK::Tuple:
            return !static_cast<ListObj *>(o.get())->v.empty();
        case OK::Dict:
        case OK::Set:
            return static_cast<DictObj *>(o.get())->size() != 0;
        default:
            return true;
        }
    }
    return false;
}

size_t VHash::operator()(const Value &v) const {
    switch (v.t) {
    case T::None:
        return 0x9e3779b9;
    case T::Bool:
    case T::Int:
        return std::hash<int64_t>()(v.i);
    case T::Float:
        if (std::floor(v.f) == v.f && std::fabs(v.f) < 9e18)
            return std::hash<int64_t>()((int64_t)v.f);
        return std::hash<double>()(v.f);
    case T::Str:
        return std::hash<std::string>()(v.sp->s);
    case T::Code:
        return (size_t)v.i * 31 + 7;
    case T::Obj:
        if (v.o->kind == OK::Tuple) {
            size_t h = 0x345678;
            for (auto &x : static_cast<ListObj *>(v.o.get())->v)
                h = h * 1000003 ^ (*this)(x);
            return h;
        }
        return std::hash<Obj *>()(v.o.get());
    }
    return 0;
}

bool VEq::operator()(const Value &a, const Value &b) const { return valueEq(a, b); }

Value *DictObj::find(const Value &k) {
    auto it = index.find(k);
    return it == index.end() ? nullptr : &items[it->second].second;
}
const Value *DictObj::find(const Value &k) const {
    auto it = index.find(k);
    return it == index.end() ? nullptr : &items[it->second].second;
}
void DictObj::set(const Value &k, const Value &v) {
    auto it = index.find(k);
    if (it != index.end()) {
        items[it->second].second = v;
        return;
    }
    index.emplace(k, items.size());
    items.emplace_back(k, v);
}
bool DictObj::erase(const Value &k) {
    auto it = index.find(k);
    if (it == index.end())
        return false;
    size_t pos = it->second;
    index.erase(it);
    if (pos != items.size() - 1) {
        items[pos] = std::move(items.back());
        index[items[pos].first] = pos;
    }
    items.pop_back();
    return true;
}

const Value *CallArgs::get(size_t idx, const char *name) const {
    if (idx < pos.size())
        return &pos[idx];
    if (name)
        for (auto &k : kw)
            if (k.first == name)
                return &k.second;
    return nullptr;
}
Value CallArgs::arg(size_t idx, const char *name, const Value &def) const {
    const Value *v = get(idx, name);
    return v ? *v : def;
}

Value mkList(std::vector<Value> items) {
    auto l = std::make_shared<ListObj>(false);
    l->v = std::move(items);
    return Value::obj(l);
}
Value mkTuple(std::vector<Value> items) {
    auto l = std::make_shared<ListObj>(true);
    l->v = std::move(items);
    return Value::obj(l);
}
Value mkDict() { return Value::obj(std::make_shared<DictObj>(false)); }
Value mkSet() { return Value::obj(std::make_shared<DictObj>(true)); }
Value mkInst(const std::string &cls) { return Value::obj(std::make_shared<InstObj>(cls)); }
Value mkNative(const std::string &name, NativeFn fn) {
    auto f = std::make_shared<FuncObj>();
    f->fk = FuncObj::Native;
    f->name = name;
    f->native = std::move(fn);
    return Value::obj(f);
}
Value mkFuncRef(const std::string &name) {
    auto f = std::make_shared<FuncObj>();
    f->fk = FuncObj::Ref;
    f->name = name;
    return Value::obj(f);
}

[[noreturn]] void throwPy(const std::string &type, const std::string &msg) { throw PyError(type, msg); }

bool valueEq(const Value &a, const Value &b) {
    if (a.isNum() && b.isNum()) {
        if (a.t != T::Float && b.t != T::Float)
            return a.i == b.i;
        return a.num() == b.num();
    }
    if (a.t != b.t)
        return false;
    switch (a.t) {
    case T::None:
        return true;
    case T::Str:
        return a.sp == b.sp || a.sp->s == b.sp->s;
    case T::Code:
        return a.i == b.i;
    case T::Obj: {
        if (a.o == b.o)
            return true;
        ListObj *la = a.list(), *lb = b.list();
        if (la && lb && a.o->kind == b.o->kind) {
            if (la->v.size() != lb->v.size())
                return false;
            for (size_t k = 0; k < la->v.size(); k++)
                if (!valueEq(la->v[k], lb->v[k]))
                    return false;
            return true;
        }
        DictObj *da = a.dict(), *db = b.dict();
        if (da && db && a.o->kind == b.o->kind) {
            if (da->size() != db->size())
                return false;
            for (auto &kv : da->items) {
                const Value *o = db->find(kv.first);
                if (!o || !valueEq(*o, kv.second))
                    return false;
            }
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

static int typeRank(const Value &v) {
    switch (v.t) {
    case T::None:
        return 0;
    case T::Bool:
    case T::Int:
    case T::Float:
        return 1;
    case T::Obj:
        if (v.o->kind == OK::Dict)
            return 2;
        if (v.o->kind == OK::List)
            return 3;
        if (v.o->kind == OK::Tuple)
            return 5;
        return 6;
    case T::Str:
        return 4;
    default:
        return 7;
    }
}

int valueCmp(const Value &a, const Value &b) {
    if (a.isNum() && b.isNum()) {
        if (a.t != T::Float && b.t != T::Float)
            return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
        double x = a.num(), y = b.num();
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    int ra = typeRank(a), rb = typeRank(b);
    if (ra != rb)
        return ra < rb ? -1 : 1;
    if (a.t == T::Str) {
        int c = a.sp->s.compare(b.sp->s);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    ListObj *la = a.list(), *lb = b.list();
    if (la && lb) {
        size_t n = std::min(la->v.size(), lb->v.size());
        for (size_t k = 0; k < n; k++) {
            int c = valueCmp(la->v[k], lb->v[k]);
            if (c)
                return c;
        }
        return la->v.size() < lb->v.size() ? -1 : (la->v.size() > lb->v.size() ? 1 : 0);
    }
    if (a.t == T::Obj && b.t == T::Obj)
        return a.o.get() < b.o.get() ? -1 : (a.o.get() > b.o.get() ? 1 : 0);
    return 0;
}

static std::string fmtFloat(double f) {
    if (std::isinf(f))
        return f > 0 ? "inf" : "-inf";
    if (std::isnan(f))
        return "nan";
    char buf[64];
    snprintf(buf, sizeof buf, "%.12g", f);
    std::string s = buf;
    if (s.find_first_of(".eEn") == std::string::npos)
        s += ".0";
    return s;
}

static std::string quoteStr(const std::string &s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'' || c == '\\')
            r += '\\';
        if (c == '\n') {
            r += "\\n";
            continue;
        }
        r += c;
    }
    return r + "'";
}

const char *typeName(const Value &v) {
    switch (v.t) {
    case T::None:
        return "NoneType";
    case T::Bool:
        return "bool";
    case T::Int:
        return "int";
    case T::Float:
        return "float";
    case T::Str:
        return "str";
    case T::Code:
        return "code";
    case T::Obj:
        switch (v.o->kind) {
        case OK::List:
            return "list";
        case OK::Tuple:
            return "tuple";
        case OK::Dict:
            return "dict";
        case OK::Set:
            return "set";
        case OK::Inst:
            return static_cast<InstObj *>(v.o.get())->cls.c_str();
        case OK::Func:
            return "function";
        }
    }
    return "?";
}

std::string valueRepr(const Value &v) {
    switch (v.t) {
    case T::Str:
        return quoteStr(v.sp->s);
    case T::Obj: {
        if (ListObj *l = v.list()) {
            bool tup = v.o->kind == OK::Tuple;
            std::string r = tup ? "(" : "[";
            for (size_t k = 0; k < l->v.size(); k++) {
                if (k)
                    r += ", ";
                r += valueRepr(l->v[k]);
            }
            if (tup && l->v.size() == 1)
                r += ",";
            return r + (tup ? ")" : "]");
        }
        if (DictObj *d = v.dict()) {
            bool set = v.o->kind == OK::Set;
            std::string r = set ? "set([" : "{";
            bool first = true;
            for (auto &kv : d->items) {
                if (!first)
                    r += ", ";
                first = false;
                r += valueRepr(kv.first);
                if (!set)
                    r += ": " + valueRepr(kv.second);
            }
            return r + (set ? "])" : "}");
        }
        if (InstObj *in = v.inst())
            return "<" + in->cls + " object>";
        if (FuncObj *fn = v.func())
            return "<function " + fn->name + ">";
        return "<object>";
    }
    default:
        return valueStr(v);
    }
}

std::string valueStr(const Value &v) {
    switch (v.t) {
    case T::None:
        return "None";
    case T::Bool:
        return v.i ? "True" : "False";
    case T::Int:
        return std::to_string(v.i);
    case T::Float:
        return fmtFloat(v.f);
    case T::Str:
        return v.sp->s;
    case T::Code:
        return "<code " + std::to_string(v.i) + ">";
    case T::Obj:
        return valueRepr(v);
    }
    return "";
}

std::string pyFormat(const std::string &fmt, const Value &args) {
    std::string out;
    size_t argi = 0;
    ListObj *tup = args.isObj(OK::Tuple) ? args.list() : nullptr;
    DictObj *dict = args.isObj(OK::Dict) ? args.dict() : nullptr;
    auto nextArg = [&]() -> Value {
        if (tup) {
            if (argi >= tup->v.size())
                throwPy("TypeError", "not enough arguments for format string");
            return tup->v[argi++];
        }
        if (argi++ == 0)
            return args;
        throwPy("TypeError", "not enough arguments for format string");
    };
    for (size_t i = 0; i < fmt.size(); i++) {
        char c = fmt[i];
        if (c != '%') {
            out += c;
            continue;
        }
        if (++i >= fmt.size())
            break;
        if (fmt[i] == '%') {
            out += '%';
            continue;
        }
        Value arg;
        bool haveArg = false;
        if (fmt[i] == '(') {
            size_t e = fmt.find(')', i);
            if (e == std::string::npos)
                break;
            std::string key = fmt.substr(i + 1, e - i - 1);
            i = e + 1;
            if (dict) {
                const Value *p = dict->find(Value::str(key));
                if (!p)
                    throwPy("KeyError", key);
                arg = *p;
            }
            haveArg = true;
        }
        std::string spec = "%";
        while (i < fmt.size() && strchr("-+ #0123456789.", fmt[i]))
            spec += fmt[i++];
        if (i >= fmt.size())
            break;
        char conv = fmt[i];
        if (!haveArg)
            arg = nextArg();
        char buf[256];
        switch (conv) {
        case 's':
        case 'r': {
            std::string sv = conv == 's' ? valueStr(arg) : valueRepr(arg);
            if (spec.size() > 1) {
                snprintf(buf, sizeof buf, (spec + "s").c_str(), sv.c_str());
                out += buf;
            } else {
                out += sv;
            }
            break;
        }
        case 'd':
        case 'i':
        case 'u':
            snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)(arg.t == T::Float ? (int64_t)arg.f : arg.i));
            out += buf;
            break;
        case 'x':
        case 'X':
        case 'o':
            snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (long long)arg.asInt());
            out += buf;
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'g':
            snprintf(buf, sizeof buf, (spec + conv).c_str(), arg.num());
            out += buf;
            break;
        case 'c':
            out += (char)arg.asInt();
            break;
        default:
            out += spec + conv;
        }
    }
    return out;
}

// ------------------------------------------------------------------ binary value graph

namespace {
enum { T_NONE, T_FALSE, T_TRUE, T_INT, T_FLOAT, T_STR, T_REF, T_CODE, T_TUPLE };
enum { K_LIST = 1, K_DICT, K_INST, K_FUNC };

struct GraphReader {
    ValueFile &vf;
    ByteReader r;
    std::vector<Value> strs; // shared string values
    GraphReader(ValueFile &f, const std::vector<uint8_t> &d) : vf(f), r(d.data(), d.size()) {}

    void skipValue() {
        uint8_t tag = r.u8();
        switch (tag) {
        case T_INT:
            r.svar();
            break;
        case T_FLOAT:
            r.f64();
            break;
        case T_STR:
        case T_REF:
        case T_CODE:
            r.uvar();
            break;
        case T_TUPLE: {
            uint64_t n = r.uvar();
            for (uint64_t k = 0; k < n && r.ok; k++)
                skipValue();
            break;
        }
        default:
            break;
        }
    }

    Value readValue() {
        uint8_t tag = r.u8();
        switch (tag) {
        case T_NONE:
            return Value();
        case T_FALSE:
            return Value::boolean(false);
        case T_TRUE:
            return Value::boolean(true);
        case T_INT:
            return Value::integer(r.svar());
        case T_FLOAT:
            return Value::real(r.f64());
        case T_STR: {
            uint64_t k = r.uvar();
            return k < strs.size() ? strs[k] : Value::str("");
        }
        case T_REF: {
            uint64_t k = r.uvar();
            return k < vf.objects.size() ? vf.objects[k] : Value();
        }
        case T_CODE:
            return Value::code((int)r.uvar());
        case T_TUPLE: {
            uint64_t n = r.uvar();
            std::vector<Value> items;
            items.reserve(n);
            for (uint64_t k = 0; k < n && r.ok; k++)
                items.push_back(readValue());
            return mkTuple(std::move(items));
        }
        }
        r.ok = false;
        return Value();
    }

    bool run() {
        if (r.n < 4 || memcmp(r.p, "KSG1", 4) != 0)
            return false;
        r.pos = 4;
        uint64_t ns = r.uvar();
        vf.strings.reserve(ns);
        strs.reserve(ns);
        for (uint64_t k = 0; k < ns && r.ok; k++) {
            vf.strings.push_back(r.str());
            strs.push_back(Value::str(vf.strings.back()));
        }
        uint64_t no = r.uvar();
        std::vector<size_t> offsets(no);
        vf.objects.resize(no);
        // pass 1: create empty objects
        for (uint64_t k = 0; k < no && r.ok; k++) {
            offsets[k] = r.pos;
            uint8_t kind = r.u8();
            if (kind == K_LIST) {
                uint64_t n = r.uvar();
                for (uint64_t j = 0; j < n && r.ok; j++)
                    skipValue();
                auto l = std::make_shared<ListObj>(false);
                l->v.reserve(n);
                vf.objects[k] = Value::obj(l);
            } else if (kind == K_DICT) {
                uint64_t n = r.uvar();
                for (uint64_t j = 0; j < n && r.ok; j++) {
                    skipValue();
                    skipValue();
                }
                vf.objects[k] = mkDict();
            } else if (kind == K_INST) {
                uint64_t cls = r.uvar();
                uint64_t n = r.uvar();
                for (uint64_t j = 0; j < n && r.ok; j++) {
                    r.uvar();
                    skipValue();
                }
                vf.objects[k] = mkInst(cls < vf.strings.size() ? vf.strings[cls] : "?");
            } else if (kind == K_FUNC) {
                uint64_t name = r.uvar();
                vf.objects[k] = mkFuncRef(name < vf.strings.size() ? vf.strings[name] : "?");
            } else {
                return false;
            }
        }
        size_t rootPos = r.pos;
        // pass 2: fill
        for (uint64_t k = 0; k < no && r.ok; k++) {
            r.pos = offsets[k];
            uint8_t kind = r.u8();
            if (kind == K_LIST) {
                uint64_t n = r.uvar();
                auto *l = vf.objects[k].list();
                for (uint64_t j = 0; j < n && r.ok; j++)
                    l->v.push_back(readValue());
            } else if (kind == K_DICT) {
                uint64_t n = r.uvar();
                auto *d = vf.objects[k].dict();
                d->items.reserve(n);
                for (uint64_t j = 0; j < n && r.ok; j++) {
                    Value key = readValue();
                    Value val = readValue();
                    d->set(key, val);
                }
            } else if (kind == K_INST) {
                r.uvar();
                uint64_t n = r.uvar();
                auto *in = vf.objects[k].inst();
                for (uint64_t j = 0; j < n && r.ok; j++) {
                    uint64_t key = r.uvar();
                    Value val = readValue();
                    if (key < vf.strings.size())
                        in->attrs[vf.strings[key]] = val;
                }
            }
        }
        r.pos = rootPos;
        vf.root = readValue();
        return r.ok;
    }
};
} // namespace

bool ValueFile::load(const std::vector<uint8_t> &data) {
    GraphReader g(*this, data);
    return g.run();
}
