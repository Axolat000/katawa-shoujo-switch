#include "py.h"
#include <algorithm>
#include <cmath>

Interp PY;

double argNum(const CallArgs &a, size_t i, const char *name, double def) {
    const Value *v = a.get(i, name);
    return (v && v->isNum()) ? v->num() : def;
}
std::string argStr(const CallArgs &a, size_t i, const char *name, const std::string &def) {
    const Value *v = a.get(i, name);
    return (v && v->isStr()) ? v->s() : def;
}
bool argBool(const CallArgs &a, size_t i, const char *name, bool def) {
    const Value *v = a.get(i, name);
    return v ? v->truthy() : def;
}

// ------------------------------------------------------------------ compilation

static const std::string &tupName(const Value &v) {
    static const std::string empty;
    ListObj *l = v.list();
    if (!l || l->v.empty() || !l->v[0].isStr())
        return empty;
    return l->v[0].s();
}

int Interp::field(const std::string &node, const char *name) {
    auto it = astFields.find(node);
    if (it == astFields.end())
        return -1;
    for (size_t i = 0; i < it->second.size(); i++)
        if (it->second[i] == name)
            return (int)i + 1;
    return -1;
}

Value Interp::fieldOf(const Value &tup, const std::string &node, const char *name) {
    int idx = field(node, name);
    ListObj *l = tup.list();
    if (idx < 0 || !l || idx >= (int)l->v.size())
        return Value();
    return l->v[idx];
}

std::vector<Node *> Interp::compileList(const Value &v) {
    std::vector<Node *> out;
    if (ListObj *l = v.list())
        for (auto &x : l->v)
            out.push_back(compile(x));
    return out;
}

static int binopKind(const std::string &n) {
    static const std::map<std::string, int> m = {
        {"Add", B_ADD},       {"Sub", B_SUB},       {"Mult", B_MUL},     {"Div", B_DIV},
        {"FloorDiv", B_FLOORDIV}, {"Mod", B_MOD},   {"Pow", B_POW},      {"LShift", B_LSHIFT},
        {"RShift", B_RSHIFT}, {"BitOr", B_BITOR},   {"BitXor", B_BITXOR}, {"BitAnd", B_BITAND}};
    auto it = m.find(n);
    return it == m.end() ? B_ADD : it->second;
}

static int cmpKind(const std::string &n) {
    static const std::map<std::string, int> m = {{"Eq", C_EQ},   {"NotEq", C_NE}, {"Lt", C_LT},    {"LtE", C_LE},
                                                 {"Gt", C_GT},   {"GtE", C_GE},   {"Is", C_IS},    {"IsNot", C_ISNOT},
                                                 {"In", C_IN},   {"NotIn", C_NOTIN}};
    auto it = m.find(n);
    return it == m.end() ? C_EQ : it->second;
}

Node *Interp::compile(const Value &v) {
    if (v.isNone())
        return nullptr;
    arena.emplace_back();
    Node *n = &arena.back();
    const std::string &t = tupName(v);
    auto F = [&](const char *name) { return fieldOf(v, t, name); };
    auto FS = [&](const char *name) {
        Value x = F(name);
        return x.isStr() ? x.s() : std::string();
    };

    if (t == "Name") {
        n->k = E_NAME;
        n->s = FS("id");
    } else if (t == "Num") {
        n->k = E_CONST;
        n->v = F("n");
    } else if (t == "Str") {
        n->k = E_CONST;
        n->v = F("s");
    } else if (t == "Attribute") {
        n->k = E_ATTR;
        n->a = compile(F("value"));
        n->s = FS("attr");
    } else if (t == "Subscript") {
        n->k = E_SUBSCRIPT;
        n->a = compile(F("value"));
        n->b = compile(F("slice"));
    } else if (t == "Index") {
        n->k = X_INDEX;
        n->a = compile(F("value"));
    } else if (t == "Slice") {
        n->k = X_SLICE;
        n->a = compile(F("lower"));
        n->b = compile(F("upper"));
        n->c = compile(F("step"));
    } else if (t == "Ellipsis") {
        n->k = X_ELLIPSIS;
    } else if (t == "Call") {
        n->k = E_CALL;
        n->a = compile(F("func"));
        n->l1 = compileList(F("args"));
        if (ListObj *kws = F("keywords").list())
            for (auto &kw : kws->v) {
                n->names.push_back(fieldOf(kw, "keyword", "arg").s());
                n->l2.push_back(compile(fieldOf(kw, "keyword", "value")));
            }
        n->b = compile(F("starargs"));
        n->c = compile(F("kwargs"));
    } else if (t == "BinOp") {
        n->k = E_BINOP;
        n->a = compile(F("left"));
        n->b = compile(F("right"));
        n->op = binopKind(tupName(F("op")));
    } else if (t == "UnaryOp") {
        n->k = E_UNARY;
        n->a = compile(F("operand"));
        const std::string &o = tupName(F("op"));
        n->op = o == "Not" ? U_NOT : o == "USub" ? U_NEG : o == "UAdd" ? U_POS : U_INVERT;
    } else if (t == "BoolOp") {
        n->k = E_BOOLOP;
        n->op = tupName(F("op")) == "And" ? 0 : 1;
        n->l1 = compileList(F("values"));
    } else if (t == "Compare") {
        n->k = E_COMPARE;
        n->a = compile(F("left"));
        if (ListObj *ops = F("ops").list())
            for (auto &o : ops->v)
                n->ops.push_back(cmpKind(tupName(o)));
        n->l1 = compileList(F("comparators"));
    } else if (t == "List" || t == "Tuple" || t == "Set") {
        n->k = t == "List" ? E_LIST : t == "Tuple" ? E_TUPLE : E_SET;
        n->l1 = compileList(F("elts"));
    } else if (t == "Dict") {
        n->k = E_DICT;
        n->l1 = compileList(F("keys"));
        n->l2 = compileList(F("values"));
    } else if (t == "IfExp") {
        n->k = E_IFEXP;
        n->a = compile(F("test"));
        n->b = compile(F("body"));
        n->c = compile(F("orelse"));
    } else if (t == "ListComp" || t == "GeneratorExp" || t == "SetComp") {
        n->k = t == "ListComp" ? E_LISTCOMP : E_GENEXP;
        n->a = compile(F("elt"));
        n->l1 = compileList(F("generators"));
    } else if (t == "DictComp") {
        n->k = E_DICTCOMP;
        n->a = compile(F("key"));
        n->b = compile(F("value"));
        n->l1 = compileList(F("generators"));
    } else if (t == "comprehension") {
        n->k = H_COMPREHENSION;
        n->a = compile(F("target"));
        n->b = compile(F("iter"));
        n->l1 = compileList(F("ifs"));
    } else if (t == "Repr") {
        n->k = E_REPR;
        n->a = compile(F("value"));
    } else if (t == "Lambda" || t == "FunctionDef") {
        n->k = t == "Lambda" ? E_LAMBDA : S_DEF;
        auto fi = std::make_shared<FuncInfo>();
        fi->name = t == "Lambda" ? "<lambda>" : FS("name");
        Value args = F("args");
        fi->params = compileList(fieldOf(args, "arguments", "args"));
        fi->defaults = compileList(fieldOf(args, "arguments", "defaults"));
        Value va = fieldOf(args, "arguments", "vararg"), ka = fieldOf(args, "arguments", "kwarg");
        fi->vararg = va.isStr() ? va.s() : "";
        fi->kwarg = ka.isStr() ? ka.s() : "";
        if (t == "Lambda") {
            fi->expr = compile(F("body"));
        } else {
            fi->body = compileList(F("body"));
            collectLocals(*fi, fi->body);
        }
        for (Node *p : fi->params) {
            std::vector<Node *> stack{p};
            while (!stack.empty()) {
                Node *q = stack.back();
                stack.pop_back();
                if (q->k == E_NAME)
                    fi->locals.insert(q->s);
                else
                    for (Node *c : q->l1)
                        stack.push_back(c);
            }
        }
        if (!fi->vararg.empty())
            fi->locals.insert(fi->vararg);
        if (!fi->kwarg.empty())
            fi->locals.insert(fi->kwarg);
        n->fi = fi;
        n->s = fi->name;
    } else if (t == "Module" || t == "Interactive") {
        n->k = S_PASS;
        n->l1 = compileList(F("body"));
    } else if (t == "Expression") {
        n->k = S_EXPR;
        n->a = compile(F("body"));
    } else if (t == "Expr") {
        n->k = S_EXPR;
        n->a = compile(F("value"));
    } else if (t == "Assign") {
        n->k = S_ASSIGN;
        n->l1 = compileList(F("targets"));
        n->a = compile(F("value"));
    } else if (t == "AugAssign") {
        n->k = S_AUGASSIGN;
        n->a = compile(F("target"));
        n->b = compile(F("value"));
        n->op = binopKind(tupName(F("op")));
    } else if (t == "If" || t == "While") {
        n->k = t == "If" ? S_IF : S_WHILE;
        n->a = compile(F("test"));
        n->l1 = compileList(F("body"));
        n->l2 = compileList(F("orelse"));
    } else if (t == "For") {
        n->k = S_FOR;
        n->a = compile(F("target"));
        n->b = compile(F("iter"));
        n->l1 = compileList(F("body"));
        n->l2 = compileList(F("orelse"));
    } else if (t == "Break") {
        n->k = S_BREAK;
    } else if (t == "Continue") {
        n->k = S_CONTINUE;
    } else if (t == "Pass") {
        n->k = S_PASS;
    } else if (t == "Return") {
        n->k = S_RETURN;
        n->a = compile(F("value"));
    } else if (t == "Global") {
        n->k = S_GLOBAL;
        if (ListObj *l = F("names").list())
            for (auto &x : l->v)
                n->names.push_back(x.s());
    } else if (t == "ClassDef") {
        n->k = S_CLASS;
        n->s = FS("name");
    } else if (t == "Print") {
        n->k = S_PRINT;
        n->l1 = compileList(F("values"));
    } else if (t == "Import" || t == "ImportFrom") {
        n->k = t == "Import" ? S_IMPORT : S_IMPORTFROM;
        n->s = FS("module");
        if (ListObj *l = F("names").list())
            for (auto &al : l->v) {
                n->names.push_back(fieldOf(al, "alias", "name").s());
                Value as = fieldOf(al, "alias", "asname");
                n->names2.push_back(as.isStr() ? as.s() : std::string());
            }
    } else if (t == "TryExcept") {
        n->k = S_TRY;
        n->l1 = compileList(F("body"));
        n->l2 = compileList(F("handlers"));
        n->l3 = compileList(F("orelse"));
    } else if (t == "ExceptHandler") {
        n->k = H_HANDLER;
        n->a = compile(F("type"));
        n->b = compile(F("name"));
        n->l1 = compileList(F("body"));
    } else if (t == "TryFinally") {
        n->k = S_TRYFINALLY;
        n->l1 = compileList(F("body"));
        n->l2 = compileList(F("finalbody"));
    } else if (t == "Raise") {
        n->k = S_RAISE;
        n->a = compile(F("type"));
        n->b = compile(F("inst"));
    } else if (t == "Delete") {
        n->k = S_DELETE;
        n->l1 = compileList(F("targets"));
    } else if (t == "Assert") {
        n->k = S_ASSERT;
    } else if (t == "Exec") {
        n->k = S_EXEC;
    } else if (t == "With") {
        n->k = S_WITH;
        n->l1 = compileList(F("body"));
    } else {
        n->k = E_UNSUPPORTED;
        n->s = t;
    }
    return n;
}

void Interp::collectLocals(FuncInfo &fi, const std::vector<Node *> &body) {
    std::function<void(Node *)> target = [&](Node *t) {
        if (!t)
            return;
        if (t->k == E_NAME)
            fi.locals.insert(t->s);
        else if (t->k == E_TUPLE || t->k == E_LIST)
            for (Node *c : t->l1)
                target(c);
    };
    std::function<void(const std::vector<Node *> &)> walk = [&](const std::vector<Node *> &stmts) {
        for (Node *s : stmts) {
            if (!s)
                continue;
            switch (s->k) {
            case S_ASSIGN:
                for (Node *t : s->l1)
                    target(t);
                break;
            case S_AUGASSIGN:
                target(s->a);
                break;
            case S_FOR:
                target(s->a);
                walk(s->l1);
                walk(s->l2);
                break;
            case S_IF:
            case S_WHILE:
                walk(s->l1);
                walk(s->l2);
                break;
            case S_TRY:
                walk(s->l1);
                for (Node *h : s->l2) {
                    target(h->b);
                    walk(h->l1);
                }
                walk(s->l3);
                break;
            case S_TRYFINALLY:
                walk(s->l1);
                walk(s->l2);
                break;
            case S_WITH:
                walk(s->l1);
                break;
            case S_DEF:
            case S_CLASS:
                fi.locals.insert(s->s);
                break;
            case S_IMPORT:
            case S_IMPORTFROM:
                for (size_t i = 0; i < s->names.size(); i++) {
                    std::string nm = s->names2[i].empty() ? s->names[i] : s->names2[i];
                    fi.locals.insert(nm.substr(0, nm.find('.')));
                }
                break;
            case S_GLOBAL:
                for (auto &g : s->names)
                    fi.globals.insert(g);
                break;
            default:
                break;
            }
        }
    };
    walk(body);
    for (auto &g : fi.globals)
        fi.locals.erase(g);
}

void Interp::loadCodes(const Value &codes, const Value &fields) {
    if (DictObj *d = fields.dict())
        for (auto &kv : d->items) {
            std::vector<std::string> names;
            if (ListObj *l = kv.second.list())
                for (auto &x : l->v)
                    names.push_back(x.s());
            astFields[kv.first.s()] = names;
        }
    ListObj *l = codes.list();
    if (!l)
        return;
    this->codes.reserve(l->v.size());
    for (auto &c : l->v)
        this->codes.push_back(compile(c));
}

// ------------------------------------------------------------------ evaluation

Value Interp::getGlobal(const std::string &name) {
    auto it = store->attrs.find(name);
    if (it != store->attrs.end())
        return it->second;
    auto b = builtins.find(name);
    if (b != builtins.end())
        return b->second;
    throwPy("NameError", "name '" + name + "' is not defined");
}

Value Interp::lookup(const std::string &name, Frame *f) {
    for (Frame *fr = f; fr && fr->fi; fr = fr->parent.get()) {
        if (fr->fi->globals.count(name))
            break;
        auto it = fr->locals.find(name);
        if (it != fr->locals.end())
            return it->second;
    }
    return getGlobal(name);
}

void Interp::assign(Node *t, const Value &v, Frame *f) {
    switch (t->k) {
    case E_NAME:
        if (f && f->fi && !f->fi->globals.count(t->s) && f->fi->locals.count(t->s))
            f->locals[t->s] = v;
        else
            store->attrs[t->s] = v;
        return;
    case E_ATTR:
        setAttr(evalNode(t->a, f), t->s, v);
        return;
    case E_SUBSCRIPT: {
        Value obj = evalNode(t->a, f);
        if (t->b->k == X_INDEX) {
            setItem(obj, evalNode(t->b->a, f), v);
        } else if (t->b->k == X_SLICE && obj.isObj(OK::List)) {
            ListObj *l = obj.list();
            int64_t n = l->v.size();
            int64_t lo = t->b->a ? evalNode(t->b->a, f).asInt() : 0;
            int64_t hi = t->b->b ? evalNode(t->b->b, f).asInt() : n;
            if (lo < 0)
                lo += n;
            if (hi < 0)
                hi += n;
            lo = std::max<int64_t>(0, std::min(lo, n));
            hi = std::max<int64_t>(lo, std::min(hi, n));
            Value items = iterList(v);
            std::vector<Value> repl = items.list()->v;
            l->v.erase(l->v.begin() + lo, l->v.begin() + hi);
            l->v.insert(l->v.begin() + lo, repl.begin(), repl.end());
        } else {
            throwPy("TypeError", "unsupported slice assignment");
        }
        return;
    }
    case E_TUPLE:
    case E_LIST: {
        Value items = iterList(v);
        ListObj *l = items.list();
        if (l->v.size() != t->l1.size())
            throwPy("ValueError", "unpack size mismatch");
        for (size_t i = 0; i < t->l1.size(); i++)
            assign(t->l1[i], l->v[i], f);
        return;
    }
    default:
        throwPy("SyntaxError", "cannot assign");
    }
}

void Interp::del(Node *t, Frame *f) {
    if (t->k == E_NAME) {
        if (f && f->fi && f->locals.count(t->s))
            f->locals.erase(t->s);
        else
            store->attrs.erase(t->s);
    } else if (t->k == E_SUBSCRIPT && t->b->k == X_INDEX) {
        Value obj = evalNode(t->a, f);
        Value key = evalNode(t->b->a, f);
        if (DictObj *d = obj.dict()) {
            d->erase(key);
        } else if (obj.isObj(OK::List)) {
            ListObj *l = obj.list();
            int64_t i = key.asInt();
            if (i < 0)
                i += l->v.size();
            if (i >= 0 && i < (int64_t)l->v.size())
                l->v.erase(l->v.begin() + i);
        }
    } else if (t->k == E_ATTR) {
        Value obj = evalNode(t->a, f);
        if (InstObj *in = obj.inst())
            in->attrs.erase(t->s);
    } else if (t->k == E_TUPLE || t->k == E_LIST) {
        for (Node *c : t->l1)
            del(c, f);
    }
}

Value Interp::makeFunction(const std::shared_ptr<FuncInfo> &fi, Frame *f) {
    auto fn = std::make_shared<FuncObj>();
    fn->fk = FuncObj::Py;
    fn->name = fi->name;
    fn->info = fi;
    for (Node *d : fi->defaults)
        fn->defaults.push_back(evalNode(d, f));
    if (f && f->fi)
        fn->closure = f->weak_from_this().lock();
    return Value::obj(fn);
}

Value Interp::comprehension(Node *n, Frame *f) {
    std::vector<Value> out;
    Value dictOut;
    if (n->k == E_DICTCOMP)
        dictOut = mkDict();
    std::function<void(size_t)> loop = [&](size_t gi) {
        if (gi == n->l1.size()) {
            if (n->k == E_DICTCOMP)
                dictOut.dict()->set(evalNode(n->a, f), evalNode(n->b, f));
            else
                out.push_back(evalNode(n->a, f));
            return;
        }
        Node *g = n->l1[gi];
        Value items = iterList(evalNode(g->b, f));
        for (auto &it : items.list()->v) {
            assign(g->a, it, f);
            bool ok = true;
            for (Node *cond : g->l1)
                if (!evalNode(cond, f).truthy()) {
                    ok = false;
                    break;
                }
            if (ok)
                loop(gi + 1);
        }
    };
    loop(0);
    if (n->k == E_DICTCOMP)
        return dictOut;
    return mkList(std::move(out));
}

static int64_t normIndex(int64_t i, int64_t n) {
    if (i < 0)
        i += n;
    return i;
}

Value Interp::slice(const Value &obj, Node *sl, Frame *f) {
    Value lo = sl->a ? evalNode(sl->a, f) : Value();
    Value hi = sl->b ? evalNode(sl->b, f) : Value();
    Value stepV = sl->c ? evalNode(sl->c, f) : Value();
    int64_t step = stepV.isNone() ? 1 : stepV.asInt();
    if (step == 0)
        throwPy("ValueError", "slice step cannot be zero");
    auto bounds = [&](int64_t n, int64_t &a, int64_t &b) {
        if (step > 0) {
            a = lo.isNone() ? 0 : normIndex(lo.asInt(), n);
            b = hi.isNone() ? n : normIndex(hi.asInt(), n);
            a = std::max<int64_t>(0, std::min(a, n));
            b = std::max<int64_t>(0, std::min(b, n));
        } else {
            a = lo.isNone() ? n - 1 : normIndex(lo.asInt(), n);
            b = hi.isNone() ? -1 : normIndex(hi.asInt(), n);
            a = std::max<int64_t>(-1, std::min(a, n - 1));
            b = std::max<int64_t>(-1, std::min(b, n - 1));
        }
    };
    if (obj.isStr()) {
        const std::string &s = obj.s();
        int64_t a, b;
        bounds((int64_t)s.size(), a, b);
        std::string r;
        if (step > 0)
            for (int64_t i = a; i < b; i += step)
                r += s[i];
        else
            for (int64_t i = a; i > b; i += step)
                r += s[i];
        return Value::str(r);
    }
    if (ListObj *l = obj.list()) {
        int64_t a, b;
        bounds((int64_t)l->v.size(), a, b);
        std::vector<Value> r;
        if (step > 0)
            for (int64_t i = a; i < b; i += step)
                r.push_back(l->v[i]);
        else
            for (int64_t i = a; i > b; i += step)
                r.push_back(l->v[i]);
        return obj.o->kind == OK::Tuple ? mkTuple(std::move(r)) : mkList(std::move(r));
    }
    throwPy("TypeError", std::string("'") + typeName(obj) + "' object is not sliceable");
}

static bool isTrueCompare(Interp &I, int op, const Value &a, const Value &b) {
    switch (op) {
    case C_EQ:
        return valueEq(a, b);
    case C_NE:
        return !valueEq(a, b);
    case C_LT:
        return valueCmp(a, b) < 0;
    case C_LE:
        return valueCmp(a, b) <= 0;
    case C_GT:
        return valueCmp(a, b) > 0;
    case C_GE:
        return valueCmp(a, b) >= 0;
    case C_IS:
        if (a.t != b.t)
            return false;
        if (a.t == T::Obj)
            return a.o == b.o;
        if (a.t == T::Str)
            return a.sp == b.sp || a.sp->s == b.sp->s;
        return valueEq(a, b);
    case C_ISNOT:
        return !isTrueCompare(I, C_IS, a, b);
    case C_IN:
        return I.contains(b, a);
    case C_NOTIN:
        return !I.contains(b, a);
    }
    return false;
}

Value Interp::evalNode(Node *n, Frame *f) {
    switch (n->k) {
    case E_NAME:
        return lookup(n->s, f);
    case E_CONST:
        return n->v;
    case E_ATTR:
        return getAttr(evalNode(n->a, f), n->s);
    case E_SUBSCRIPT: {
        Value obj = evalNode(n->a, f);
        if (n->b->k == X_INDEX)
            return getItem(obj, evalNode(n->b->a, f));
        if (n->b->k == X_SLICE)
            return slice(obj, n->b, f);
        throwPy("TypeError", "unsupported subscript");
    }
    case E_CALL: {
        Value fn = evalNode(n->a, f);
        CallArgs args;
        args.pos.reserve(n->l1.size());
        for (Node *a : n->l1)
            args.pos.push_back(evalNode(a, f));
        if (n->b) {
            Value extra = iterList(evalNode(n->b, f));
            for (auto &x : extra.list()->v)
                args.pos.push_back(x);
        }
        for (size_t i = 0; i < n->l2.size(); i++)
            args.kw.emplace_back(n->names[i], evalNode(n->l2[i], f));
        if (n->c) {
            Value kw = evalNode(n->c, f);
            if (DictObj *d = kw.dict())
                for (auto &kv : d->items)
                    args.kw.emplace_back(valueStr(kv.first), kv.second);
        }
        return call(fn, args);
    }
    case E_BINOP:
        return binop(n->op, evalNode(n->a, f), evalNode(n->b, f));
    case E_UNARY: {
        Value v = evalNode(n->a, f);
        switch (n->op) {
        case U_NOT:
            return Value::boolean(!v.truthy());
        case U_NEG:
            if (v.t == T::Float)
                return Value::real(-v.f);
            if (v.isInt())
                return Value::integer(-v.i);
            throwPy("TypeError", "bad operand for unary -");
        case U_POS:
            return v;
        case U_INVERT:
            return Value::integer(~v.asInt());
        }
        return v;
    }
    case E_BOOLOP: {
        Value v;
        for (Node *x : n->l1) {
            v = evalNode(x, f);
            if (n->op == 0 ? !v.truthy() : v.truthy())
                return v;
        }
        return v;
    }
    case E_COMPARE: {
        Value left = evalNode(n->a, f);
        for (size_t i = 0; i < n->ops.size(); i++) {
            Value right = evalNode(n->l1[i], f);
            if (!isTrueCompare(*this, n->ops[i], left, right))
                return Value::boolean(false);
            left = right;
        }
        return Value::boolean(true);
    }
    case E_LIST:
    case E_TUPLE: {
        std::vector<Value> items;
        items.reserve(n->l1.size());
        for (Node *x : n->l1)
            items.push_back(evalNode(x, f));
        return n->k == E_LIST ? mkList(std::move(items)) : mkTuple(std::move(items));
    }
    case E_SET: {
        Value s = mkSet();
        for (Node *x : n->l1)
            s.dict()->set(evalNode(x, f), Value());
        return s;
    }
    case E_DICT: {
        Value d = mkDict();
        for (size_t i = 0; i < n->l1.size(); i++)
            d.dict()->set(evalNode(n->l1[i], f), evalNode(n->l2[i], f));
        return d;
    }
    case E_LAMBDA:
        return makeFunction(n->fi, f);
    case E_IFEXP:
        return evalNode(n->a, f).truthy() ? evalNode(n->b, f) : evalNode(n->c, f);
    case E_LISTCOMP:
    case E_GENEXP:
    case E_DICTCOMP: {
        // comprehension variables leak into the current scope like Python 2 list comprehensions;
        // use a child frame for generator expressions inside functions to keep locals clean
        if (f && f->fi && n->k != E_LISTCOMP) {
            auto child = std::make_shared<FuncInfo>(*f->fi);
            Frame fr;
            fr.fi = child.get();
            fr.locals = f->locals;
            std::function<void(Node *)> addTargets = [&](Node *t) {
                if (!t)
                    return;
                if (t->k == E_NAME)
                    child->locals.insert(t->s);
                for (Node *c : t->l1)
                    addTargets(c);
            };
            for (Node *g : n->l1)
                addTargets(g->a);
            fr.parent = f->parent;
            return comprehension(n, &fr);
        }
        if (f && f->fi) {
            for (Node *g : n->l1) {
                std::function<void(Node *)> addTargets = [&](Node *t) {
                    if (!t)
                        return;
                    if (t->k == E_NAME)
                        f->fi->locals.insert(t->s);
                    for (Node *c : t->l1)
                        addTargets(c);
                };
                addTargets(g->a);
            }
        }
        return comprehension(n, f);
    }
    case E_REPR:
        return Value::str(valueRepr(evalNode(n->a, f)));
    case S_EXPR:
        return evalNode(n->a, f);
    default:
        throwPy("SyntaxError", "unsupported expression " + n->s);
    }
}

Value Interp::eval(int code, FrameP frame) {
    if (code < 0 || code >= (int)codes.size() || !codes[code])
        throwPy("SystemError", "bad code index " + std::to_string(code));
    Node *root = codes[code];
    Frame local;
    Frame *f = frame ? frame.get() : &local;
    if (root->k == S_EXPR)
        return evalNode(root->a, f);
    Value ret;
    execBody(root->l1, f, ret);
    return ret;
}

void Interp::exec(int code, FrameP frame, bool hide) {
    if (code < 0 || code >= (int)codes.size() || !codes[code])
        throwPy("SystemError", "bad code index " + std::to_string(code));
    Node *root = codes[code];
    Value ret;
    if (root->k == S_EXPR) {
        Frame local;
        evalNode(root->a, frame ? frame.get() : &local);
        return;
    }
    if (hide) {
        FuncInfo fi;
        collectLocals(fi, root->l1);
        Frame fr;
        fr.fi = &fi;
        execBody(root->l1, &fr, ret);
        return;
    }
    Frame local;
    execBody(root->l1, frame ? frame.get() : &local, ret);
}

Interp::Flow Interp::execBody(const std::vector<Node *> &body, Frame *f, Value &ret) {
    for (Node *s : body) {
        Flow fl = execStmt(s, f, ret);
        if (fl != F_NORMAL)
            return fl;
    }
    return F_NORMAL;
}

static bool exceptionMatches(Interp &I, const PyError &e, const Value &type) {
    if (type.isNone())
        return true;
    if (ListObj *l = type.list()) {
        for (auto &t : l->v)
            if (exceptionMatches(I, e, t))
                return true;
        return false;
    }
    FuncObj *fn = type.func();
    std::string name = fn ? fn->name : std::string();
    size_t dot = name.rfind('.');
    if (dot != std::string::npos)
        name = name.substr(dot + 1);
    if (name == "Exception" || name == "BaseException" || name == "StandardError")
        return true;
    if (name == e.type)
        return true;
    if (name == "LookupError" && (e.type == "KeyError" || e.type == "IndexError"))
        return true;
    if (name == "ArithmeticError" && e.type == "ZeroDivisionError")
        return true;
    return false;
}

Interp::Flow Interp::execStmt(Node *n, Frame *f, Value &ret) {
    switch (n->k) {
    case S_EXPR:
        evalNode(n->a, f);
        return F_NORMAL;
    case S_ASSIGN: {
        Value v = evalNode(n->a, f);
        for (Node *t : n->l1)
            assign(t, v, f);
        return F_NORMAL;
    }
    case S_AUGASSIGN: {
        Node *t = n->a;
        Value cur;
        Value obj, key;
        if (t->k == E_NAME) {
            cur = lookup(t->s, f);
        } else if (t->k == E_ATTR) {
            obj = evalNode(t->a, f);
            cur = getAttr(obj, t->s);
        } else if (t->k == E_SUBSCRIPT && t->b->k == X_INDEX) {
            obj = evalNode(t->a, f);
            key = evalNode(t->b->a, f);
            cur = getItem(obj, key);
        } else {
            throwPy("SyntaxError", "bad augmented assignment");
        }
        Value rhs = evalNode(n->b, f);
        Value res;
        if (n->op == B_ADD && cur.isObj(OK::List)) {
            Value items = iterList(rhs);
            auto &dst = cur.list()->v;
            auto src = items.list()->v;
            dst.insert(dst.end(), src.begin(), src.end());
            res = cur;
        } else {
            res = binop(n->op, cur, rhs);
        }
        if (t->k == E_NAME)
            assign(t, res, f);
        else if (t->k == E_ATTR)
            setAttr(obj, t->s, res);
        else
            setItem(obj, key, res);
        return F_NORMAL;
    }
    case S_IF:
        if (evalNode(n->a, f).truthy())
            return execBody(n->l1, f, ret);
        return execBody(n->l2, f, ret);
    case S_WHILE: {
        int guard = 0;
        while (evalNode(n->a, f).truthy()) {
            Flow fl = execBody(n->l1, f, ret);
            if (fl == F_BREAK)
                return F_NORMAL;
            if (fl == F_RETURN)
                return fl;
            if (++guard > 10000000)
                throwPy("RuntimeError", "infinite loop");
        }
        return execBody(n->l2, f, ret);
    }
    case S_FOR: {
        Value items = iterList(evalNode(n->b, f));
        std::vector<Value> copy = items.list()->v;
        for (auto &it : copy) {
            assign(n->a, it, f);
            Flow fl = execBody(n->l1, f, ret);
            if (fl == F_BREAK)
                return F_NORMAL;
            if (fl == F_RETURN)
                return fl;
        }
        return execBody(n->l2, f, ret);
    }
    case S_BREAK:
        return F_BREAK;
    case S_CONTINUE:
        return F_CONTINUE;
    case S_RETURN:
        ret = n->a ? evalNode(n->a, f) : Value();
        return F_RETURN;
    case S_PASS:
    case S_GLOBAL:
    case S_PRINT:
    case S_ASSERT:
    case S_EXEC:
        return F_NORMAL;
    case S_DEF: {
        Value fn = makeFunction(n->fi, f);
        Node tmp;
        tmp.k = E_NAME;
        tmp.s = n->s;
        assign(&tmp, fn, f);
        return F_NORMAL;
    }
    case S_CLASS:
        return F_NORMAL;
    case S_IMPORT:
    case S_IMPORTFROM: {
        for (size_t i = 0; i < n->names.size(); i++) {
            Node tmp;
            tmp.k = E_NAME;
            if (n->k == S_IMPORT) {
                std::string full = n->names[i];
                std::string top = full.substr(0, full.find('.'));
                tmp.s = n->names2[i].empty() ? top : n->names2[i];
                assign(&tmp, module(n->names2[i].empty() ? top : full), f);
            } else {
                Value mod = module(n->s);
                tmp.s = n->names2[i].empty() ? n->names[i] : n->names2[i];
                Value v;
                if (!tryGetAttr(mod, n->names[i], v))
                    v = Value();
                assign(&tmp, v, f);
            }
        }
        return F_NORMAL;
    }
    case S_TRY: {
        Flow fl;
        try {
            fl = execBody(n->l1, f, ret);
        } catch (PyError &e) {
            for (Node *h : n->l2) {
                Value type = h->a ? evalNode(h->a, f) : Value();
                if (exceptionMatches(*this, e, type)) {
                    if (h->b) {
                        Value ev = mkInst(e.type);
                        ev.inst()->attrs["message"] = Value::str(e.msg);
                        assign(h->b, ev, f);
                    }
                    return execBody(h->l1, f, ret);
                }
            }
            throw;
        }
        if (fl != F_NORMAL)
            return fl;
        return execBody(n->l3, f, ret);
    }
    case S_TRYFINALLY: {
        Flow fl;
        try {
            fl = execBody(n->l1, f, ret);
        } catch (...) {
            Value r2;
            execBody(n->l2, f, r2);
            throw;
        }
        Value r2;
        Flow f2 = execBody(n->l2, f, r2);
        if (f2 != F_NORMAL) {
            ret = r2;
            return f2;
        }
        return fl;
    }
    case S_RAISE: {
        if (!n->a)
            throwPy("RuntimeError", "re-raise");
        Value t = evalNode(n->a, f);
        std::string type = "Exception", msg;
        if (FuncObj *fn = t.func()) {
            type = fn->name;
            size_t dot = type.rfind('.');
            if (dot != std::string::npos)
                type = type.substr(dot + 1);
        } else if (InstObj *in = t.inst()) {
            type = in->cls;
            msg = valueStr(in->get("message"));
        }
        if (n->b)
            msg = valueStr(evalNode(n->b, f));
        throwPy(type, msg);
    }
    case S_DELETE:
        for (Node *t : n->l1)
            del(t, f);
        return F_NORMAL;
    case S_WITH:
        return execBody(n->l1, f, ret);
    default:
        throwPy("SyntaxError", "unsupported statement " + n->s);
    }
}

// ------------------------------------------------------------------ calls

Value Interp::callPy(FuncObj *fn, CallArgs &args) {
    FuncInfo *fi = fn->info.get();
    auto frame = std::make_shared<Frame>();
    frame->fi = fi;
    frame->parent = fn->closure;
    size_t np = fi->params.size();
    size_t nd = fn->defaults.size();
    std::vector<bool> bound(np, false);
    std::vector<Value> extra;
    for (size_t i = 0; i < args.pos.size(); i++) {
        if (i < np) {
            Node *p = fi->params[i];
            if (p->k == E_NAME)
                frame->locals[p->s] = args.pos[i];
            else
                assign(p, args.pos[i], frame.get());
            bound[i] = true;
        } else {
            extra.push_back(args.pos[i]);
        }
    }
    Value kwextra;
    for (auto &kw : args.kw) {
        bool found = false;
        for (size_t i = 0; i < np; i++)
            if (fi->params[i]->k == E_NAME && fi->params[i]->s == kw.first) {
                frame->locals[kw.first] = kw.second;
                bound[i] = true;
                found = true;
                break;
            }
        if (!found) {
            if (fi->kwarg.empty())
                throwPy("TypeError", fi->name + "() got an unexpected keyword argument '" + kw.first + "'");
            if (kwextra.isNone())
                kwextra = mkDict();
            kwextra.dict()->set(Value::str(kw.first), kw.second);
        }
    }
    for (size_t i = 0; i < np; i++) {
        if (bound[i])
            continue;
        size_t firstDefault = np - nd;
        if (i >= firstDefault) {
            Node *p = fi->params[i];
            if (p->k == E_NAME)
                frame->locals[p->s] = fn->defaults[i - firstDefault];
            else
                assign(p, fn->defaults[i - firstDefault], frame.get());
        } else {
            throwPy("TypeError", fi->name + "() takes more arguments (" + std::to_string(args.pos.size()) + " given)");
        }
    }
    if (!fi->vararg.empty())
        frame->locals[fi->vararg] = mkTuple(extra);
    else if (!extra.empty())
        throwPy("TypeError", fi->name + "() takes at most " + std::to_string(np) + " arguments");
    if (!fi->kwarg.empty())
        frame->locals[fi->kwarg] = kwextra.isNone() ? mkDict() : kwextra;
    if (fi->expr)
        return evalNode(fi->expr, frame.get());
    Value ret;
    execBody(fi->body, frame.get(), ret);
    return ret;
}

Value Interp::call(const Value &fnv, CallArgs &args) {
    if (FuncObj *fn = fnv.func()) {
        switch (fn->fk) {
        case FuncObj::Native:
            return fn->native(*this, args);
        case FuncObj::Py:
            return callPy(fn, args);
        case FuncObj::Bound: {
            args.pos.insert(args.pos.begin(), fn->self);
            return call(fn->fn, args);
        }
        case FuncObj::Ref: {
            auto it = natives.find(fn->name);
            Value target;
            if (it != natives.end()) {
                target = it->second;
            } else if (fn->name.compare(0, 6, "store.") == 0 && hasGlobal(fn->name.substr(6))) {
                target = store->attrs[fn->name.substr(6)];
            } else {
                std::string shortName = fn->name.substr(fn->name.rfind('.') + 1);
                auto st = store->attrs.find(shortName);
                if (st != store->attrs.end() && st->second.func() && st->second.o.get() != fn)
                    target = st->second;
                else
                    throwPy("NameError", "unresolved function " + fn->name);
            }
            FuncObj *tf = target.func();
            if (!tf || tf == fn || tf->fk == FuncObj::Ref)
                throwPy("NameError", "unresolved function " + fn->name);
            {
                // cache resolution
                fn->fk = tf->fk;
                fn->native = tf->native;
                fn->info = tf->info;
                fn->defaults = tf->defaults;
                fn->closure = tf->closure;
                fn->self = tf->self;
                fn->fn = tf->fn;
            }
            return call(target, args);
        }
        }
    }
    if (InstObj *in = fnv.inst()) {
        auto it = instCall.find(in->cls);
        if (it != instCall.end()) {
            args.pos.insert(args.pos.begin(), fnv);
            return it->second(*this, args);
        }
        if (in->cls == "renpy.curry.Curry") {
            CallArgs a2;
            if (ListObj *l = in->get("args").list())
                a2.pos = l->v;
            a2.pos.insert(a2.pos.end(), args.pos.begin(), args.pos.end());
            if (DictObj *d = in->get("kwargs").dict())
                for (auto &kv : d->items)
                    a2.kw.emplace_back(kv.first.s(), kv.second);
            for (auto &kw : args.kw) {
                bool replaced = false;
                for (auto &k2 : a2.kw)
                    if (k2.first == kw.first) {
                        k2.second = kw.second;
                        replaced = true;
                    }
                if (!replaced)
                    a2.kw.push_back(kw);
            }
            return call(in->get("callable"), a2);
        }
        if (in->cls == "#method") {
            Value self = in->get("self");
            return call(getAttr(self, in->get("name").s()), args);
        }
        throwPy("TypeError", "'" + in->cls + "' object is not callable");
    }
    throwPy("TypeError", std::string("'") + typeName(fnv) + "' object is not callable");
}

void Interp::registerNative(const std::string &qualified, NativeFn fn) {
    std::string shortName = qualified.substr(qualified.rfind('.') + 1);
    natives[qualified] = mkNative(shortName, std::move(fn));
}
