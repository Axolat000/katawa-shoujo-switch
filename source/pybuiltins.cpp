#include "py.h"
#include <algorithm>
#include <cmath>
#include <random>

static std::mt19937 &rng() {
    static std::mt19937 g((unsigned)(nowSeconds() * 1e6));
    return g;
}

// ------------------------------------------------------------------ attributes

static Value bound(const Value &self, const std::string &name, NativeFn fn) {
    auto f = std::make_shared<FuncObj>();
    f->fk = FuncObj::Bound;
    f->name = name;
    f->self = self;
    f->fn = mkNative(name, std::move(fn));
    return Value::obj(f);
}

static std::string lower(std::string s) {
    for (auto &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

static Value strMethod(const Value &self, const std::string &name);
static Value listMethod(const Value &self, const std::string &name);
static Value dictMethod(const Value &self, const std::string &name);

bool Interp::tryGetAttr(const Value &obj, const std::string &name, Value &out) {
    if (InstObj *in = obj.inst()) {
        auto it = in->attrs.find(name);
        if (it != in->attrs.end()) {
            out = it->second;
            return true;
        }
        auto mc = methods.find(in->cls);
        if (mc != methods.end()) {
            auto m = mc->second.find(name);
            if (m != mc->second.end()) {
                out = bound(obj, name, m->second);
                return true;
            }
            auto ga = mc->second.find("__getattr__");
            if (ga != mc->second.end()) {
                CallArgs a;
                a.pos = {obj, Value::str(name)};
                out = ga->second(*this, a);
                return true;
            }
        }
        if (name == "__class__") {
            out = Value::str(in->cls);
            return true;
        }
        return false;
    }
    if (obj.isStr()) {
        out = strMethod(obj, name);
        return !out.isNone();
    }
    if (obj.isObj(OK::List) || obj.isObj(OK::Tuple)) {
        out = listMethod(obj, name);
        return !out.isNone();
    }
    if (obj.isObj(OK::Dict) || obj.isObj(OK::Set)) {
        out = dictMethod(obj, name);
        return !out.isNone();
    }
    if (FuncObj *fn = obj.func()) {
        if (name == "__name__") {
            out = Value::str(fn->name);
            return true;
        }
        return false;
    }
    return false;
}

Value Interp::getAttr(const Value &obj, const std::string &name) {
    Value out;
    if (tryGetAttr(obj, name, out))
        return out;
    throwPy("AttributeError", std::string("'") + typeName(obj) + "' object has no attribute '" + name + "'");
}

void Interp::setAttr(const Value &obj, const std::string &name, const Value &v) {
    if (InstObj *in = obj.inst()) {
        auto mc = methods.find(in->cls);
        if (mc != methods.end()) {
            auto sa = mc->second.find("__setattr__");
            if (sa != mc->second.end()) {
                CallArgs a;
                a.pos = {obj, Value::str(name), v};
                sa->second(*this, a);
                return;
            }
        }
        in->attrs[name] = v;
        return;
    }
    throwPy("AttributeError", std::string("cannot set attribute on '") + typeName(obj) + "'");
}

// ------------------------------------------------------------------ items / iteration

Value Interp::getItem(const Value &obj, const Value &key) {
    if (DictObj *d = obj.dict()) {
        if (const Value *v = d->find(key))
            return *v;
        throwPy("KeyError", valueRepr(key));
    }
    if (ListObj *l = obj.list()) {
        if (!key.isInt())
            throwPy("TypeError", "list indices must be integers");
        int64_t i = key.i;
        if (i < 0)
            i += l->v.size();
        if (i < 0 || i >= (int64_t)l->v.size())
            throwPy("IndexError", "list index out of range");
        return l->v[i];
    }
    if (obj.isStr()) {
        const std::string &s = obj.s();
        int64_t i = key.asInt();
        if (i < 0)
            i += s.size();
        if (i < 0 || i >= (int64_t)s.size())
            throwPy("IndexError", "string index out of range");
        return Value::str(std::string(1, s[i]));
    }
    if (InstObj *in = obj.inst()) {
        auto mc = methods.find(in->cls);
        if (mc != methods.end()) {
            auto gi = mc->second.find("__getitem__");
            if (gi != mc->second.end()) {
                CallArgs a;
                a.pos = {obj, key};
                return gi->second(*this, a);
            }
        }
    }
    throwPy("TypeError", std::string("'") + typeName(obj) + "' object is not subscriptable");
}

void Interp::setItem(const Value &obj, const Value &key, const Value &v) {
    if (obj.isObj(OK::Dict)) {
        obj.dict()->set(key, v);
        return;
    }
    if (obj.isObj(OK::List)) {
        ListObj *l = obj.list();
        int64_t i = key.asInt();
        if (i < 0)
            i += l->v.size();
        if (i < 0 || i >= (int64_t)l->v.size())
            throwPy("IndexError", "list assignment index out of range");
        l->v[i] = v;
        return;
    }
    throwPy("TypeError", std::string("'") + typeName(obj) + "' object does not support item assignment");
}

Value Interp::iterList(const Value &obj) {
    if (obj.list())
        return obj;
    if (DictObj *d = obj.dict()) {
        std::vector<Value> keys;
        keys.reserve(d->size());
        for (auto &kv : d->items)
            keys.push_back(kv.first);
        return mkList(std::move(keys));
    }
    if (obj.isStr()) {
        std::vector<Value> chars;
        const std::string &s = obj.s();
        for (size_t i = 0; i < s.size();) {
            size_t st = i;
            utf8Decode(s, i);
            chars.push_back(Value::str(s.substr(st, i - st)));
        }
        return mkList(std::move(chars));
    }
    throwPy("TypeError", std::string("'") + typeName(obj) + "' object is not iterable");
}

bool Interp::contains(const Value &c, const Value &item) {
    if (DictObj *d = c.dict())
        return d->find(item) != nullptr;
    if (ListObj *l = c.list()) {
        for (auto &x : l->v)
            if (valueEq(x, item))
                return true;
        return false;
    }
    if (c.isStr())
        return c.s().find(item.s()) != std::string::npos;
    if (InstObj *in = c.inst()) {
        auto mc = methods.find(in->cls);
        if (mc != methods.end()) {
            auto ct = mc->second.find("__contains__");
            if (ct != mc->second.end()) {
                CallArgs a;
                a.pos = {c, item};
                return ct->second(*this, a).truthy();
            }
        }
    }
    throwPy("TypeError", std::string("argument of type '") + typeName(c) + "' is not iterable");
}

// ------------------------------------------------------------------ arithmetic

static double pyMod(double a, double b) {
    double r = std::fmod(a, b);
    if (r != 0 && ((r < 0) != (b < 0)))
        r += b;
    return r;
}

Value Interp::binop(int op, const Value &a, const Value &b) {
    if (a.isNum() && b.isNum()) {
        bool fl = a.t == T::Float || b.t == T::Float;
        switch (op) {
        case B_ADD:
            return fl ? Value::real(a.num() + b.num()) : Value::integer(a.i + b.i);
        case B_SUB:
            return fl ? Value::real(a.num() - b.num()) : Value::integer(a.i - b.i);
        case B_MUL:
            return fl ? Value::real(a.num() * b.num()) : Value::integer(a.i * b.i);
        case B_DIV:
            if (b.num() == 0)
                throwPy("ZeroDivisionError", "division by zero");
            if (!fl) {
                int64_t q = a.i / b.i;
                if ((a.i % b.i != 0) && ((a.i < 0) != (b.i < 0)))
                    q--;
                return Value::integer(q);
            }
            return Value::real(a.num() / b.num());
        case B_FLOORDIV:
            if (b.num() == 0)
                throwPy("ZeroDivisionError", "division by zero");
            if (!fl) {
                int64_t q = a.i / b.i;
                if ((a.i % b.i != 0) && ((a.i < 0) != (b.i < 0)))
                    q--;
                return Value::integer(q);
            }
            return Value::real(std::floor(a.num() / b.num()));
        case B_MOD:
            if (b.num() == 0)
                throwPy("ZeroDivisionError", "modulo by zero");
            if (!fl) {
                int64_t r = a.i % b.i;
                if (r != 0 && ((r < 0) != (b.i < 0)))
                    r += b.i;
                return Value::integer(r);
            }
            return Value::real(pyMod(a.num(), b.num()));
        case B_POW:
            if (!fl && b.i >= 0) {
                int64_t r = 1;
                for (int64_t k = 0; k < b.i; k++)
                    r *= a.i;
                return Value::integer(r);
            }
            return Value::real(std::pow(a.num(), b.num()));
        case B_LSHIFT:
            return Value::integer(a.asInt() << b.asInt());
        case B_RSHIFT:
            return Value::integer(a.asInt() >> b.asInt());
        case B_BITOR:
            if (a.t == T::Bool && b.t == T::Bool)
                return Value::boolean(a.i | b.i);
            return Value::integer(a.asInt() | b.asInt());
        case B_BITXOR:
            return Value::integer(a.asInt() ^ b.asInt());
        case B_BITAND:
            if (a.t == T::Bool && b.t == T::Bool)
                return Value::boolean(a.i & b.i);
            return Value::integer(a.asInt() & b.asInt());
        }
    }
    if (op == B_ADD) {
        if (a.isStr() && b.isStr())
            return Value::str(a.s() + b.s());
        ListObj *la = a.list(), *lb = b.list();
        if (la && lb) {
            std::vector<Value> r = la->v;
            r.insert(r.end(), lb->v.begin(), lb->v.end());
            return a.o->kind == OK::Tuple ? mkTuple(std::move(r)) : mkList(std::move(r));
        }
    }
    if (op == B_MOD && a.isStr())
        return Value::str(pyFormat(a.s(), b));
    if (op == B_MUL) {
        const Value &seq = a.isInt() ? b : a;
        const Value &cnt = a.isInt() ? a : b;
        if (cnt.isInt()) {
            if (seq.isStr()) {
                std::string r;
                for (int64_t k = 0; k < cnt.i; k++)
                    r += seq.s();
                return Value::str(r);
            }
            if (ListObj *l = seq.list()) {
                std::vector<Value> r;
                for (int64_t k = 0; k < cnt.i; k++)
                    r.insert(r.end(), l->v.begin(), l->v.end());
                return seq.o->kind == OK::Tuple ? mkTuple(std::move(r)) : mkList(std::move(r));
            }
        }
    }
    if (op == B_SUB && a.isObj(OK::Set) && b.dict()) {
        Value r = mkSet();
        for (auto &kv : a.dict()->items)
            if (!b.dict()->find(kv.first))
                r.dict()->set(kv.first, Value());
        return r;
    }
    if (InstObj *in = a.inst()) {
        static const char *names[] = {"__add__", "__sub__", "__mul__", "__div__", "__floordiv__", "__mod__",
                                      "__pow__", "__lshift__", "__rshift__", "__or__", "__xor__", "__and__"};
        auto mc = methods.find(in->cls);
        if (mc != methods.end()) {
            auto m = mc->second.find(names[op]);
            if (m != mc->second.end()) {
                CallArgs args;
                args.pos = {a, b};
                return m->second(*this, args);
            }
        }
    }
    throwPy("TypeError", std::string("unsupported operand types: '") + typeName(a) + "' and '" + typeName(b) + "'");
}

// ------------------------------------------------------------------ str methods

static std::vector<std::string> splitStr(const std::string &s, const Value &sep, int64_t maxsplit) {
    std::vector<std::string> out;
    if (sep.isNone()) {
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && isspace((unsigned char)s[i]))
                i++;
            if (i >= s.size())
                break;
            if (maxsplit >= 0 && (int64_t)out.size() == maxsplit) {
                out.push_back(s.substr(i));
                return out;
            }
            size_t j = i;
            while (j < s.size() && !isspace((unsigned char)s[j]))
                j++;
            out.push_back(s.substr(i, j - i));
            i = j;
        }
        return out;
    }
    const std::string &d = sep.s();
    if (d.empty())
        throwPy("ValueError", "empty separator");
    size_t start = 0;
    while (true) {
        if (maxsplit >= 0 && (int64_t)out.size() == maxsplit)
            break;
        size_t p = s.find(d, start);
        if (p == std::string::npos)
            break;
        out.push_back(s.substr(start, p - start));
        start = p + d.size();
    }
    out.push_back(s.substr(start));
    return out;
}

static std::string strip(const std::string &s, const Value &chars, int mode) {
    std::string set = chars.isStr() ? chars.s() : std::string(" \t\r\n\v\f");
    size_t a = 0, b = s.size();
    if (mode & 1)
        while (a < b && set.find(s[a]) != std::string::npos)
            a++;
    if (mode & 2)
        while (b > a && set.find(s[b - 1]) != std::string::npos)
            b--;
    return s.substr(a, b - a);
}

static Value strMethod(const Value &self, const std::string &name) {
    const std::string &s = self.s();
    (void)s;
    auto S = [](CallArgs &a) -> const std::string & { return a.pos[0].s(); };
    if (name == "replace")
        return bound(self, name, [S](Interp &, CallArgs &a) {
            std::string str = S(a);
            std::string from = argStr(a, 1, nullptr), to = argStr(a, 2, nullptr);
            if (from.empty())
                return Value::str(str);
            std::string r;
            size_t pos = 0;
            while (true) {
                size_t p = str.find(from, pos);
                if (p == std::string::npos)
                    break;
                r += str.substr(pos, p - pos) + to;
                pos = p + from.size();
            }
            r += str.substr(pos);
            return Value::str(r);
        });
    if (name == "startswith" || name == "endswith")
        return bound(self, name, [S, name](Interp &, CallArgs &a) {
            const std::string &str = S(a);
            std::vector<std::string> cands;
            Value p = a.arg(1, nullptr);
            if (ListObj *l = p.list())
                for (auto &x : l->v)
                    cands.push_back(x.s());
            else
                cands.push_back(p.s());
            for (auto &c : cands) {
                if (c.size() > str.size())
                    continue;
                if (name == "startswith" ? str.compare(0, c.size(), c) == 0
                                         : str.compare(str.size() - c.size(), c.size(), c) == 0)
                    return Value::boolean(true);
            }
            return Value::boolean(false);
        });
    if (name == "split" || name == "rsplit")
        return bound(self, name, [S](Interp &, CallArgs &a) {
            auto parts = splitStr(S(a), a.arg(1, "sep"), a.has(2, "maxsplit") ? a.arg(2, "maxsplit").asInt() : -1);
            std::vector<Value> out;
            for (auto &p : parts)
                out.push_back(Value::str(p));
            return mkList(std::move(out));
        });
    if (name == "partition" || name == "rpartition")
        return bound(self, name, [S, name](Interp &, CallArgs &a) {
            const std::string &str = S(a);
            std::string sep = argStr(a, 1, nullptr);
            size_t p = name == "partition" ? str.find(sep) : str.rfind(sep);
            if (p == std::string::npos) {
                if (name == "partition")
                    return mkTuple({Value::str(str), Value::str(""), Value::str("")});
                return mkTuple({Value::str(""), Value::str(""), Value::str(str)});
            }
            return mkTuple({Value::str(str.substr(0, p)), Value::str(sep), Value::str(str.substr(p + sep.size()))});
        });
    if (name == "strip" || name == "lstrip" || name == "rstrip")
        return bound(self, name, [S, name](Interp &, CallArgs &a) {
            int mode = name == "strip" ? 3 : name == "lstrip" ? 1 : 2;
            return Value::str(strip(S(a), a.arg(1, nullptr), mode));
        });
    if (name == "lower" || name == "upper")
        return bound(self, name, [S, name](Interp &, CallArgs &a) {
            std::string r = S(a);
            for (auto &c : r)
                c = (char)(name == "lower" ? tolower((unsigned char)c) : toupper((unsigned char)c));
            return Value::str(r);
        });
    if (name == "capitalize" || name == "title")
        return bound(self, name, [S](Interp &, CallArgs &a) {
            std::string r = lower(S(a));
            if (!r.empty())
                r[0] = (char)toupper((unsigned char)r[0]);
            return Value::str(r);
        });
    if (name == "join")
        return bound(self, name, [S](Interp &I, CallArgs &a) {
            Value items = I.iterList(a.arg(1, nullptr));
            std::string r;
            bool first = true;
            for (auto &x : items.list()->v) {
                if (!first)
                    r += S(a);
                first = false;
                r += x.s();
            }
            return Value::str(r);
        });
    if (name == "find" || name == "rfind" || name == "index")
        return bound(self, name, [S, name](Interp &, CallArgs &a) {
            size_t p = name == "rfind" ? S(a).rfind(argStr(a, 1, nullptr)) : S(a).find(argStr(a, 1, nullptr));
            if (p == std::string::npos && name == "index")
                throwPy("ValueError", "substring not found");
            return Value::integer(p == std::string::npos ? -1 : (int64_t)p);
        });
    if (name == "count")
        return bound(self, name, [S](Interp &, CallArgs &a) {
            std::string sub = argStr(a, 1, nullptr);
            int64_t n = 0;
            if (!sub.empty())
                for (size_t p = S(a).find(sub); p != std::string::npos; p = S(a).find(sub, p + sub.size()))
                    n++;
            return Value::integer(n);
        });
    if (name == "isdigit" || name == "isalpha" || name == "isspace")
        return bound(self, name, [S, name](Interp &, CallArgs &a) {
            const std::string &str = S(a);
            if (str.empty())
                return Value::boolean(false);
            for (unsigned char c : str)
                if (!(name == "isdigit" ? isdigit(c) : name == "isalpha" ? isalpha(c) : isspace(c)))
                    return Value::boolean(false);
            return Value::boolean(true);
        });
    if (name == "encode" || name == "decode")
        return bound(self, name, [S](Interp &, CallArgs &a) { return Value::str(S(a)); });
    if (name == "format")
        return bound(self, name, [S](Interp &, CallArgs &a) {
            std::string f = S(a), r;
            size_t argi = 1;
            for (size_t i = 0; i < f.size(); i++) {
                if (f[i] == '{') {
                    size_t e = f.find('}', i);
                    if (e == std::string::npos)
                        break;
                    std::string key = f.substr(i + 1, e - i - 1);
                    Value v;
                    if (key.empty())
                        v = a.arg(argi++, nullptr);
                    else if (isdigit((unsigned char)key[0]))
                        v = a.arg(1 + atoi(key.c_str()), nullptr);
                    else
                        v = a.arg(1000, key.c_str());
                    r += valueStr(v);
                    i = e;
                } else {
                    r += f[i];
                }
            }
            return Value::str(r);
        });
    if (name == "zfill")
        return bound(self, name, [S](Interp &, CallArgs &a) {
            std::string r = S(a);
            int64_t w = argNum(a, 1, nullptr, 0);
            while ((int64_t)r.size() < w)
                r = "0" + r;
            return Value::str(r);
        });
    return Value();
}

// ------------------------------------------------------------------ list methods

static Value listMethod(const Value &self, const std::string &name) {
    auto L = [](CallArgs &a) { return a.pos[0].list(); };
    if (name == "index")
        return bound(self, name, [L](Interp &, CallArgs &a) {
            ListObj *l = L(a);
            Value x = a.arg(1, nullptr);
            for (size_t i = 0; i < l->v.size(); i++)
                if (valueEq(l->v[i], x))
                    return Value::integer((int64_t)i);
            throwPy("ValueError", "x not in list");
        });
    if (name == "count")
        return bound(self, name, [L](Interp &, CallArgs &a) {
            int64_t n = 0;
            for (auto &x : L(a)->v)
                if (valueEq(x, a.arg(1, nullptr)))
                    n++;
            return Value::integer(n);
        });
    if (self.o->kind == OK::Tuple)
        return Value();
    if (name == "append")
        return bound(self, name, [L](Interp &, CallArgs &a) {
            L(a)->v.push_back(a.arg(1, nullptr));
            return Value();
        });
    if (name == "extend")
        return bound(self, name, [L](Interp &I, CallArgs &a) {
            Value items = I.iterList(a.arg(1, nullptr));
            std::vector<Value> copy = items.list()->v;
            L(a)->v.insert(L(a)->v.end(), copy.begin(), copy.end());
            return Value();
        });
    if (name == "insert")
        return bound(self, name, [L](Interp &, CallArgs &a) {
            ListObj *l = L(a);
            int64_t i = a.arg(1, nullptr).asInt();
            if (i < 0)
                i += l->v.size();
            i = std::max<int64_t>(0, std::min<int64_t>(i, l->v.size()));
            l->v.insert(l->v.begin() + i, a.arg(2, nullptr));
            return Value();
        });
    if (name == "pop")
        return bound(self, name, [L](Interp &, CallArgs &a) {
            ListObj *l = L(a);
            if (l->v.empty())
                throwPy("IndexError", "pop from empty list");
            int64_t i = a.has(1, nullptr) ? a.arg(1, nullptr).asInt() : -1;
            if (i < 0)
                i += l->v.size();
            if (i < 0 || i >= (int64_t)l->v.size())
                throwPy("IndexError", "pop index out of range");
            Value r = l->v[i];
            l->v.erase(l->v.begin() + i);
            return r;
        });
    if (name == "remove")
        return bound(self, name, [L](Interp &, CallArgs &a) {
            ListObj *l = L(a);
            for (size_t i = 0; i < l->v.size(); i++)
                if (valueEq(l->v[i], a.arg(1, nullptr))) {
                    l->v.erase(l->v.begin() + i);
                    return Value();
                }
            throwPy("ValueError", "list.remove(x): x not in list");
        });
    if (name == "reverse")
        return bound(self, name, [L](Interp &, CallArgs &a) {
            std::reverse(L(a)->v.begin(), L(a)->v.end());
            return Value();
        });
    if (name == "sort")
        return bound(self, name, [L](Interp &I, CallArgs &a) {
            ListObj *l = L(a);
            Value key = a.arg(1000, "key");
            Value cmp = a.arg(1, "cmp");
            bool rev = argBool(a, 1000, "reverse", false);
            if (!key.isNone()) {
                std::vector<std::pair<Value, Value>> pairs;
                for (auto &x : l->v)
                    pairs.emplace_back(I.call(key, {x}), x);
                std::stable_sort(pairs.begin(), pairs.end(),
                                 [](const std::pair<Value, Value> &p, const std::pair<Value, Value> &q) {
                                     return valueCmp(p.first, q.first) < 0;
                                 });
                for (size_t i = 0; i < pairs.size(); i++)
                    l->v[i] = pairs[i].second;
            } else if (!cmp.isNone()) {
                std::stable_sort(l->v.begin(), l->v.end(), [&](const Value &p, const Value &q) {
                    return I.call(cmp, {p, q}).asInt() < 0;
                });
            } else {
                std::stable_sort(l->v.begin(), l->v.end(),
                                 [](const Value &p, const Value &q) { return valueCmp(p, q) < 0; });
            }
            if (rev)
                std::reverse(l->v.begin(), l->v.end());
            return Value();
        });
    return Value();
}

// ------------------------------------------------------------------ dict methods

static Value dictMethod(const Value &self, const std::string &name) {
    auto D = [](CallArgs &a) { return a.pos[0].dict(); };
    if (name == "get")
        return bound(self, name, [D](Interp &, CallArgs &a) {
            const Value *v = D(a)->find(a.arg(1, nullptr));
            return v ? *v : a.arg(2, nullptr);
        });
    if (name == "keys" || name == "iterkeys")
        return bound(self, name, [D](Interp &, CallArgs &a) {
            std::vector<Value> r;
            for (auto &kv : D(a)->items)
                r.push_back(kv.first);
            return mkList(std::move(r));
        });
    if (name == "values" || name == "itervalues")
        return bound(self, name, [D](Interp &, CallArgs &a) {
            std::vector<Value> r;
            for (auto &kv : D(a)->items)
                r.push_back(kv.second);
            return mkList(std::move(r));
        });
    if (name == "items" || name == "iteritems")
        return bound(self, name, [D](Interp &, CallArgs &a) {
            std::vector<Value> r;
            for (auto &kv : D(a)->items)
                r.push_back(mkTuple({kv.first, kv.second}));
            return mkList(std::move(r));
        });
    if (name == "has_key")
        return bound(self, name, [D](Interp &, CallArgs &a) { return Value::boolean(D(a)->find(a.arg(1, nullptr))); });
    if (name == "setdefault")
        return bound(self, name, [D](Interp &, CallArgs &a) {
            Value k = a.arg(1, nullptr);
            if (const Value *v = D(a)->find(k))
                return *v;
            D(a)->set(k, a.arg(2, nullptr));
            return a.arg(2, nullptr);
        });
    if (name == "pop")
        return bound(self, name, [D](Interp &, CallArgs &a) {
            Value k = a.arg(1, nullptr);
            const Value *v = D(a)->find(k);
            if (!v) {
                if (a.has(2, nullptr))
                    return a.arg(2, nullptr);
                throwPy("KeyError", valueRepr(k));
            }
            Value r = *v;
            D(a)->erase(k);
            return r;
        });
    if (name == "update")
        return bound(self, name, [D](Interp &, CallArgs &a) {
            if (DictObj *src = a.arg(1, nullptr).dict())
                for (auto &kv : src->items)
                    D(a)->set(kv.first, kv.second);
            for (auto &kw : a.kw)
                D(a)->set(Value::str(kw.first), kw.second);
            return Value();
        });
    if (name == "copy")
        return bound(self, name, [](Interp &, CallArgs &a) {
            Value r = a.pos[0].o->kind == OK::Set ? mkSet() : mkDict();
            r.dict()->items = a.pos[0].dict()->items;
            r.dict()->index = a.pos[0].dict()->index;
            return r;
        });
    if (name == "clear")
        return bound(self, name, [D](Interp &, CallArgs &a) {
            D(a)->clear();
            return Value();
        });
    if (name == "add")
        return bound(self, name, [D](Interp &, CallArgs &a) {
            D(a)->set(a.arg(1, nullptr), Value());
            return Value();
        });
    if (name == "discard" || name == "remove")
        return bound(self, name, [D](Interp &, CallArgs &a) {
            D(a)->erase(a.arg(1, nullptr));
            return Value();
        });
    return Value();
}

// ------------------------------------------------------------------ builtins & modules

Value Interp::module(const std::string &name) {
    auto it = modules.find(name);
    if (it != modules.end())
        return it->second;
    Value m = mkInst("#module");
    m.inst()->attrs["__name__"] = Value::str(name);
    modules[name] = m;
    size_t dot = name.rfind('.');
    if (dot != std::string::npos) {
        Value parent = module(name.substr(0, dot));
        parent.inst()->attrs[name.substr(dot + 1)] = m;
    }
    return m;
}

static Value toInt(const Value &v) {
    if (v.isInt())
        return Value::integer(v.i);
    if (v.t == T::Float)
        return Value::integer((int64_t)v.f);
    if (v.isStr()) {
        const char *p = v.s().c_str();
        char *end;
        long long x = strtoll(p, &end, 10);
        if (end == p)
            throwPy("ValueError", "invalid literal for int(): " + v.s());
        return Value::integer(x);
    }
    throwPy("TypeError", "int() argument must be a string or a number");
}

static Value toFloat(const Value &v) {
    if (v.isNum())
        return Value::real(v.num());
    if (v.isStr()) {
        const char *p = v.s().c_str();
        char *end;
        double x = strtod(p, &end);
        if (end == p)
            throwPy("ValueError", "could not convert string to float: " + v.s());
        return Value::real(x);
    }
    throwPy("TypeError", "float() argument must be a string or a number");
}

static Value doCopy(const Value &v, bool deep) {
    if (v.t != T::Obj)
        return v;
    if (ListObj *l = v.list()) {
        std::vector<Value> items;
        for (auto &x : l->v)
            items.push_back(deep ? doCopy(x, true) : x);
        return v.o->kind == OK::Tuple ? mkTuple(std::move(items)) : mkList(std::move(items));
    }
    if (DictObj *d = v.dict()) {
        Value r = v.o->kind == OK::Set ? mkSet() : mkDict();
        for (auto &kv : d->items)
            r.dict()->set(kv.first, deep ? doCopy(kv.second, true) : kv.second);
        return r;
    }
    if (InstObj *in = v.inst()) {
        if (in->cls == "#module")
            return v;
        Value r = mkInst(in->cls);
        for (auto &kv : in->attrs)
            r.inst()->attrs[kv.first] = deep ? doCopy(kv.second, true) : kv.second;
        r.inst()->native = in->native;
        return r;
    }
    return v;
}

void Interp::init() {
    auto B = [&](const std::string &name, NativeFn fn) { builtins[name] = mkNative(name, std::move(fn)); };
    builtins["True"] = Value::boolean(true);
    builtins["False"] = Value::boolean(false);
    builtins["None"] = Value();

    B("len", [](Interp &I, CallArgs &a) -> Value {
        Value v = a.arg(0, nullptr);
        if (v.isStr())
            return Value::integer((int64_t)utf8Count(v.s()));
        if (ListObj *l = v.list())
            return Value::integer((int64_t)l->v.size());
        if (DictObj *d = v.dict())
            return Value::integer((int64_t)d->size());
        if (InstObj *in = v.inst()) {
            auto mc = I.methods.find(in->cls);
            if (mc != I.methods.end() && mc->second.count("__len__")) {
                CallArgs x;
                x.pos = {v};
                return mc->second["__len__"](I, x);
            }
        }
        throwPy("TypeError", std::string("object of type '") + typeName(v) + "' has no len()");
    });
    auto rangeFn = [](Interp &, CallArgs &a) {
        int64_t start = 0, stop, step = 1;
        if (a.pos.size() == 1) {
            stop = a.pos[0].asInt();
        } else {
            start = a.arg(0, nullptr).asInt();
            stop = a.arg(1, nullptr).asInt();
            if (a.pos.size() > 2)
                step = a.pos[2].asInt();
        }
        if (step == 0)
            throwPy("ValueError", "range() step argument must not be zero");
        std::vector<Value> r;
        for (int64_t i = start; step > 0 ? i < stop : i > stop; i += step)
            r.push_back(Value::integer(i));
        return mkList(std::move(r));
    };
    B("range", rangeFn);
    B("xrange", rangeFn);
    B("str", [](Interp &, CallArgs &a) { return Value::str(a.pos.empty() ? "" : valueStr(a.pos[0])); });
    B("unicode", [](Interp &, CallArgs &a) { return Value::str(a.pos.empty() ? "" : valueStr(a.pos[0])); });
    B("basestring", [](Interp &, CallArgs &) -> Value { throwPy("TypeError", "basestring"); });
    B("repr", [](Interp &, CallArgs &a) { return Value::str(valueRepr(a.arg(0, nullptr))); });
    B("int", [](Interp &, CallArgs &a) { return a.pos.empty() ? Value::integer(0) : toInt(a.pos[0]); });
    B("long", [](Interp &, CallArgs &a) { return a.pos.empty() ? Value::integer(0) : toInt(a.pos[0]); });
    B("float", [](Interp &, CallArgs &a) { return a.pos.empty() ? Value::real(0) : toFloat(a.pos[0]); });
    B("bool", [](Interp &, CallArgs &a) { return Value::boolean(!a.pos.empty() && a.pos[0].truthy()); });
    B("abs", [](Interp &, CallArgs &a) {
        Value v = a.arg(0, nullptr);
        return v.t == T::Float ? Value::real(std::fabs(v.f)) : Value::integer(std::llabs(v.i));
    });
    B("round", [](Interp &, CallArgs &a) {
        double x = argNum(a, 0, nullptr, 0);
        int nd = (int)argNum(a, 1, nullptr, 0);
        double m = std::pow(10.0, nd);
        return Value::real(std::round(x * m) / m);
    });
    auto minmax = [](bool isMax) {
        return [isMax](Interp &I, CallArgs &a) {
            std::vector<Value> items;
            if (a.pos.size() == 1)
                items = I.iterList(a.pos[0]).list()->v;
            else
                items = a.pos;
            if (items.empty())
                throwPy("ValueError", "min/max arg is an empty sequence");
            Value best = items[0];
            for (auto &x : items)
                if (isMax ? valueCmp(x, best) > 0 : valueCmp(x, best) < 0)
                    best = x;
            return best;
        };
    };
    B("min", minmax(false));
    B("max", minmax(true));
    B("sum", [](Interp &I, CallArgs &a) {
        Value acc = a.arg(1, nullptr, Value::integer(0));
        for (auto &x : I.iterList(a.arg(0, nullptr)).list()->v)
            acc = I.binop(B_ADD, acc, x);
        return acc;
    });
    B("any", [](Interp &I, CallArgs &a) {
        for (auto &x : I.iterList(a.arg(0, nullptr)).list()->v)
            if (x.truthy())
                return Value::boolean(true);
        return Value::boolean(false);
    });
    B("all", [](Interp &I, CallArgs &a) {
        for (auto &x : I.iterList(a.arg(0, nullptr)).list()->v)
            if (!x.truthy())
                return Value::boolean(false);
        return Value::boolean(true);
    });
    B("list", [](Interp &I, CallArgs &a) {
        if (a.pos.empty())
            return mkList();
        return mkList(I.iterList(a.pos[0]).list()->v);
    });
    B("tuple", [](Interp &I, CallArgs &a) {
        if (a.pos.empty())
            return mkTuple({});
        return mkTuple(I.iterList(a.pos[0]).list()->v);
    });
    B("set", [](Interp &I, CallArgs &a) {
        Value s = mkSet();
        if (!a.pos.empty())
            for (auto &x : I.iterList(a.pos[0]).list()->v)
                s.dict()->set(x, Value());
        return s;
    });
    B("frozenset", builtins["set"].func()->native);
    B("dict", [](Interp &I, CallArgs &a) {
        Value d = mkDict();
        if (!a.pos.empty()) {
            if (DictObj *src = a.pos[0].dict()) {
                d.dict()->items = src->items;
                d.dict()->index = src->index;
            } else {
                for (auto &p : I.iterList(a.pos[0]).list()->v) {
                    ListObj *pl = p.list();
                    if (pl && pl->v.size() == 2)
                        d.dict()->set(pl->v[0], pl->v[1]);
                }
            }
        }
        for (auto &kw : a.kw)
            d.dict()->set(Value::str(kw.first), kw.second);
        return d;
    });
    B("enumerate", [](Interp &I, CallArgs &a) {
        std::vector<Value> r;
        int64_t i = a.has(1, "start") ? a.arg(1, "start").asInt() : 0;
        for (auto &x : I.iterList(a.arg(0, nullptr)).list()->v)
            r.push_back(mkTuple({Value::integer(i++), x}));
        return mkList(std::move(r));
    });
    B("zip", [](Interp &I, CallArgs &a) {
        std::vector<std::vector<Value>> lists;
        size_t n = SIZE_MAX;
        for (auto &p : a.pos) {
            lists.push_back(I.iterList(p).list()->v);
            n = std::min(n, lists.back().size());
        }
        std::vector<Value> r;
        if (lists.empty())
            return mkList();
        for (size_t i = 0; i < n; i++) {
            std::vector<Value> t;
            for (auto &l : lists)
                t.push_back(l[i]);
            r.push_back(mkTuple(std::move(t)));
        }
        return mkList(std::move(r));
    });
    B("sorted", [](Interp &I, CallArgs &a) {
        Value l = mkList(I.iterList(a.arg(0, nullptr)).list()->v);
        CallArgs sa;
        sa.kw = a.kw;
        Value sortFn = I.getAttr(l, "sort");
        I.call(sortFn, sa);
        return l;
    });
    B("reversed", [](Interp &I, CallArgs &a) {
        std::vector<Value> r = I.iterList(a.arg(0, nullptr)).list()->v;
        std::reverse(r.begin(), r.end());
        return mkList(std::move(r));
    });
    B("iter", [](Interp &I, CallArgs &a) { return mkList(I.iterList(a.arg(0, nullptr)).list()->v); });
    B("next", [](Interp &I, CallArgs &a) {
        Value l = I.iterList(a.arg(0, nullptr));
        if (l.list()->v.empty()) {
            if (a.has(1, nullptr))
                return a.arg(1, nullptr);
            throwPy("StopIteration", "");
        }
        return l.list()->v[0];
    });
    B("map", [](Interp &I, CallArgs &a) {
        std::vector<Value> r;
        for (auto &x : I.iterList(a.arg(1, nullptr)).list()->v)
            r.push_back(I.call(a.pos[0], {x}));
        return mkList(std::move(r));
    });
    B("filter", [](Interp &I, CallArgs &a) {
        std::vector<Value> r;
        for (auto &x : I.iterList(a.arg(1, nullptr)).list()->v)
            if (a.pos[0].isNone() ? x.truthy() : I.call(a.pos[0], {x}).truthy())
                r.push_back(x);
        return mkList(std::move(r));
    });
    B("isinstance", [](Interp &, CallArgs &a) {
        Value v = a.arg(0, nullptr), t = a.arg(1, nullptr);
        std::vector<Value> types;
        if (ListObj *l = t.list())
            types = l->v;
        else
            types.push_back(t);
        for (auto &ty : types) {
            FuncObj *fn = ty.func();
            if (!fn)
                continue;
            const std::string &n = fn->name;
            if ((n == "basestring" || n == "str" || n == "unicode") && v.isStr())
                return Value::boolean(true);
            if ((n == "int" || n == "long") && v.isInt())
                return Value::boolean(true);
            if (n == "float" && v.t == T::Float)
                return Value::boolean(true);
            if (n == "tuple" && v.isObj(OK::Tuple))
                return Value::boolean(true);
            if (n == "list" && v.isObj(OK::List))
                return Value::boolean(true);
            if (n == "dict" && v.isObj(OK::Dict))
                return Value::boolean(true);
            if (n == "bool" && v.t == T::Bool)
                return Value::boolean(true);
            if (InstObj *in = v.inst()) {
                std::string cls = in->cls.substr(in->cls.rfind('.') + 1);
                std::string want = n.substr(n.rfind('.') + 1);
                if (cls == want)
                    return Value::boolean(true);
            }
        }
        return Value::boolean(false);
    });
    B("callable", [](Interp &, CallArgs &a) {
        Value v = a.arg(0, nullptr);
        return Value::boolean(v.func() || v.inst());
    });
    B("hasattr", [](Interp &I, CallArgs &a) {
        Value out;
        return Value::boolean(I.tryGetAttr(a.arg(0, nullptr), argStr(a, 1, nullptr), out));
    });
    B("getattr", [](Interp &I, CallArgs &a) {
        Value out;
        if (I.tryGetAttr(a.arg(0, nullptr), argStr(a, 1, nullptr), out))
            return out;
        if (a.has(2, nullptr))
            return a.arg(2, nullptr);
        throwPy("AttributeError", argStr(a, 1, nullptr));
    });
    B("setattr", [](Interp &I, CallArgs &a) {
        I.setAttr(a.arg(0, nullptr), argStr(a, 1, nullptr), a.arg(2, nullptr));
        return Value();
    });
    B("ord", [](Interp &, CallArgs &a) {
        std::string s = argStr(a, 0, nullptr);
        size_t i = 0;
        return Value::integer(s.empty() ? 0 : utf8Decode(s, i));
    });
    B("chr", [](Interp &, CallArgs &a) { return Value::str(std::string(1, (char)a.arg(0, nullptr).asInt())); });
    B("unichr", [](Interp &, CallArgs &a) { return Value::str(utf8Encode((uint32_t)a.arg(0, nullptr).asInt())); });
    B("divmod", [](Interp &I, CallArgs &a) {
        return mkTuple({I.binop(B_FLOORDIV, a.arg(0, nullptr), a.arg(1, nullptr)),
                        I.binop(B_MOD, a.arg(0, nullptr), a.arg(1, nullptr))});
    });
    B("object", [](Interp &, CallArgs &) { return mkInst("object"); });
    B("type", [](Interp &, CallArgs &a) { return Value::str(typeName(a.arg(0, nullptr))); });
    B("id", [](Interp &, CallArgs &a) {
        Value v = a.arg(0, nullptr);
        return Value::integer(v.t == T::Obj ? (int64_t)(intptr_t)v.o.get() : 0);
    });
    B("globals", [](Interp &I, CallArgs &) {
        Value d = mkDict();
        for (auto &kv : I.store->attrs)
            d.dict()->set(Value::str(kv.first), kv.second);
        return d;
    });
    B("locals", builtins["globals"].func()->native);
    B("eval", [](Interp &, CallArgs &) -> Value { throwPy("NotImplementedError", "eval"); });
    for (const char *ex : {"Exception", "ValueError", "KeyError", "IndexError", "TypeError", "AttributeError",
                           "NameError", "RuntimeError", "StopIteration", "ZeroDivisionError", "LookupError",
                           "NotImplementedError", "IOError", "OSError", "ImportError", "AssertionError",
                           "StandardError", "BaseException", "ArithmeticError", "UnicodeError"}) {
        std::string name = ex;
        builtins[name] = mkNative(name, [name](Interp &, CallArgs &a) {
            Value e = mkInst(name);
            e.inst()->attrs["message"] = Value::str(a.pos.empty() ? "" : valueStr(a.pos[0]));
            return e;
        });
    }

    // math
    Value math = module("math");
    auto M = [&](const char *name, double (*fn)(double)) {
        math.inst()->attrs[name] = mkNative(name, [fn](Interp &, CallArgs &a) { return Value::real(fn(argNum(a, 0, nullptr, 0))); });
    };
    M("sin", std::sin);
    M("cos", std::cos);
    M("tan", std::tan);
    M("asin", std::asin);
    M("acos", std::acos);
    M("atan", std::atan);
    M("sqrt", std::sqrt);
    M("exp", std::exp);
    M("fabs", std::fabs);
    M("log10", std::log10);
    math.inst()->attrs["floor"] = mkNative("floor", [](Interp &, CallArgs &a) { return Value::real(std::floor(argNum(a, 0, nullptr, 0))); });
    math.inst()->attrs["ceil"] = mkNative("ceil", [](Interp &, CallArgs &a) { return Value::real(std::ceil(argNum(a, 0, nullptr, 0))); });
    math.inst()->attrs["log"] = mkNative("log", [](Interp &, CallArgs &a) {
        double x = std::log(argNum(a, 0, nullptr, 1));
        if (a.has(1, nullptr))
            x /= std::log(argNum(a, 1, nullptr, M_E));
        return Value::real(x);
    });
    math.inst()->attrs["pow"] = mkNative("pow", [](Interp &, CallArgs &a) {
        return Value::real(std::pow(argNum(a, 0, nullptr, 0), argNum(a, 1, nullptr, 0)));
    });
    math.inst()->attrs["atan2"] = mkNative("atan2", [](Interp &, CallArgs &a) {
        return Value::real(std::atan2(argNum(a, 0, nullptr, 0), argNum(a, 1, nullptr, 0)));
    });
    math.inst()->attrs["pi"] = Value::real(M_PI);
    math.inst()->attrs["e"] = Value::real(M_E);

    // random (also exposed as renpy.random)
    Value random = module("random");
    auto &ra = random.inst()->attrs;
    ra["random"] = mkNative("random", [](Interp &, CallArgs &) {
        return Value::real(std::uniform_real_distribution<double>(0, 1)(rng()));
    });
    ra["randint"] = mkNative("randint", [](Interp &, CallArgs &a) {
        int64_t lo = a.arg(0, nullptr).asInt(), hi = a.arg(1, nullptr).asInt();
        return Value::integer(std::uniform_int_distribution<int64_t>(lo, std::max(lo, hi))(rng()));
    });
    ra["randrange"] = mkNative("randrange", [](Interp &, CallArgs &a) {
        int64_t lo = 0, hi;
        if (a.pos.size() == 1)
            hi = a.pos[0].asInt();
        else {
            lo = a.arg(0, nullptr).asInt();
            hi = a.arg(1, nullptr).asInt();
        }
        if (hi <= lo)
            throwPy("ValueError", "empty range for randrange()");
        return Value::integer(std::uniform_int_distribution<int64_t>(lo, hi - 1)(rng()));
    });
    ra["uniform"] = mkNative("uniform", [](Interp &, CallArgs &a) {
        double lo = argNum(a, 0, nullptr, 0), hi = argNum(a, 1, nullptr, 1);
        return Value::real(lo + (hi - lo) * std::uniform_real_distribution<double>(0, 1)(rng()));
    });
    ra["choice"] = mkNative("choice", [](Interp &I, CallArgs &a) {
        Value l = I.iterList(a.arg(0, nullptr));
        auto &v = l.list()->v;
        if (v.empty())
            throwPy("IndexError", "cannot choose from an empty sequence");
        return v[std::uniform_int_distribution<size_t>(0, v.size() - 1)(rng())];
    });
    ra["shuffle"] = mkNative("shuffle", [](Interp &, CallArgs &a) {
        if (ListObj *l = a.arg(0, nullptr).list())
            std::shuffle(l->v.begin(), l->v.end(), rng());
        return Value();
    });
    ra["getrandbits"] = mkNative("getrandbits", [](Interp &, CallArgs &a) {
        int bits = (int)a.arg(0, nullptr).asInt();
        uint64_t v = ((uint64_t)rng()() << 32) | rng()();
        if (bits < 64)
            v &= (1ull << bits) - 1;
        return Value::integer((int64_t)(v & 0x7fffffffffffffffull));
    });

    // copy
    Value copy = module("copy");
    copy.inst()->attrs["copy"] = mkNative("copy", [](Interp &, CallArgs &a) { return doCopy(a.arg(0, nullptr), false); });
    copy.inst()->attrs["deepcopy"] = mkNative("deepcopy", [](Interp &, CallArgs &a) { return doCopy(a.arg(0, nullptr), true); });

    Value sets = module("sets");
    sets.inst()->attrs["Set"] = builtins["set"];
    module("os");
    module("sys");
    module("locale");
    module("time");
}
