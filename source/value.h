#pragma once
#include "common.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Dynamic values with Python 2 semantics, used by the script interpreter, the store and the data files.

struct Obj;
using ObjP = std::shared_ptr<Obj>;

enum class T : uint8_t { None, Bool, Int, Float, Str, Code, Obj };

enum class OK : uint8_t { List, Tuple, Dict, Set, Inst, Func };

struct StrBox {
    std::string s;
};

struct Value {
    T t = T::None;
    union {
        int64_t i;
        double f;
    };
    std::shared_ptr<StrBox> sp; // T::Str
    ObjP o;                     // T::Obj

    Value() : i(0) {}
    static Value none() { return Value(); }
    static Value boolean(bool b) {
        Value v;
        v.t = T::Bool;
        v.i = b ? 1 : 0;
        return v;
    }
    static Value integer(int64_t x) {
        Value v;
        v.t = T::Int;
        v.i = x;
        return v;
    }
    static Value real(double x) {
        Value v;
        v.t = T::Float;
        v.f = x;
        return v;
    }
    static Value str(const std::string &s) {
        Value v;
        v.t = T::Str;
        v.sp = std::make_shared<StrBox>();
        v.sp->s = s;
        return v;
    }
    static Value str(std::string &&s) {
        Value v;
        v.t = T::Str;
        v.sp = std::make_shared<StrBox>();
        v.sp->s = std::move(s);
        return v;
    }
    static Value code(int idx) {
        Value v;
        v.t = T::Code;
        v.i = idx;
        return v;
    }
    static Value obj(ObjP p) {
        Value v;
        if (p) {
            v.t = T::Obj;
            v.o = std::move(p);
        }
        return v;
    }

    bool isNone() const { return t == T::None; }
    bool isStr() const { return t == T::Str; }
    bool isNum() const { return t == T::Int || t == T::Float || t == T::Bool; }
    bool isInt() const { return t == T::Int || t == T::Bool; }
    bool isObj(OK k) const;
    const std::string &s() const {
        static const std::string empty;
        return t == T::Str ? sp->s : empty;
    }
    double num() const { return t == T::Float ? f : (double)i; }
    int64_t asInt() const { return t == T::Float ? (int64_t)f : i; }
    bool truthy() const;

    struct ListObj *list() const;  // list or tuple, else null
    struct DictObj *dict() const;  // dict or set, else null
    struct InstObj *inst() const;  // instance, else null
    struct FuncObj *func() const;  // function, else null
};

struct Obj {
    OK kind;
    explicit Obj(OK k) : kind(k) {}
    virtual ~Obj() {}
};

struct ListObj : Obj {
    std::vector<Value> v;
    explicit ListObj(bool tuple = false) : Obj(tuple ? OK::Tuple : OK::List) {}
};

struct VHash {
    size_t operator()(const Value &v) const;
};
struct VEq {
    bool operator()(const Value &a, const Value &b) const;
};

struct DictObj : Obj {
    std::vector<std::pair<Value, Value>> items;
    std::unordered_map<Value, size_t, VHash, VEq> index;
    explicit DictObj(bool set = false) : Obj(set ? OK::Set : OK::Dict) {}
    Value *find(const Value &k);
    const Value *find(const Value &k) const;
    void set(const Value &k, const Value &v);
    bool erase(const Value &k);
    void clear() {
        items.clear();
        index.clear();
    }
    size_t size() const { return items.size(); }
};

using AttrMap = std::unordered_map<std::string, Value>;

struct InstObj : Obj {
    std::string cls;
    AttrMap attrs;
    std::shared_ptr<void> native; // engine-side cache (compiled displayable, ...)
    explicit InstObj(const std::string &c) : Obj(OK::Inst), cls(c) {}
    Value get(const std::string &k) const {
        auto it = attrs.find(k);
        return it == attrs.end() ? Value() : it->second;
    }
    bool has(const std::string &k) const { return attrs.count(k) != 0; }
};

struct Interp;
struct CallArgs {
    std::vector<Value> pos;
    std::vector<std::pair<std::string, Value>> kw;
    // positional index or keyword name
    const Value *get(size_t idx, const char *name) const;
    Value arg(size_t idx, const char *name, const Value &def = Value()) const;
    bool has(size_t idx, const char *name) const { return get(idx, name) != nullptr; }
};
using NativeFn = std::function<Value(Interp &, CallArgs &)>;

struct FuncInfo; // py.h
struct Frame;    // py.h

struct FuncObj : Obj {
    enum Kind { Native, Py, Bound, Ref } fk = Native;
    std::string name;
    NativeFn native;
    // Py
    std::shared_ptr<FuncInfo> info;
    std::vector<Value> defaults;
    std::shared_ptr<Frame> closure;
    // Bound
    Value self;
    Value fn;
    FuncObj() : Obj(OK::Func) {}
};

// ---- construction helpers
Value mkList(std::vector<Value> items = {});
Value mkTuple(std::vector<Value> items);
Value mkDict();
Value mkSet();
Value mkInst(const std::string &cls);
Value mkNative(const std::string &name, NativeFn fn);
Value mkFuncRef(const std::string &name);

// ---- python-like operations
bool valueEq(const Value &a, const Value &b);
int valueCmp(const Value &a, const Value &b); // -1 0 1 (py2 total ordering approximation)
std::string valueStr(const Value &v);         // str()
std::string valueRepr(const Value &v);        // repr()
const char *typeName(const Value &v);

struct PyError {
    std::string type, msg;
    Value payload;
    PyError(std::string t, std::string m) : type(std::move(t)), msg(std::move(m)) {}
};
[[noreturn]] void throwPy(const std::string &type, const std::string &msg);

// String formatting "fmt % args"
std::string pyFormat(const std::string &fmt, const Value &args);

// ---- binary value graph (data files / saves)
struct ValueFile {
    std::vector<std::string> strings;
    std::vector<Value> objects;
    Value root;
    bool load(const std::vector<uint8_t> &data);
};
