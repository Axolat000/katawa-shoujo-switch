#pragma once
#include "value.h"
#include <deque>
#include <map>
#include <unordered_set>

// A small Python 2 interpreter working on ASTs produced by the real Python 2.7 parser.

enum NK : uint8_t {
    // expressions
    E_NAME,
    E_CONST,
    E_ATTR,
    E_SUBSCRIPT,
    E_CALL,
    E_BINOP,
    E_UNARY,
    E_BOOLOP,
    E_COMPARE,
    E_LIST,
    E_TUPLE,
    E_DICT,
    E_SET,
    E_LAMBDA,
    E_IFEXP,
    E_LISTCOMP,
    E_GENEXP,
    E_DICTCOMP,
    E_REPR,
    E_UNSUPPORTED,
    // slices
    X_INDEX,
    X_SLICE,
    X_ELLIPSIS,
    // statements
    S_EXPR,
    S_ASSIGN,
    S_AUGASSIGN,
    S_IF,
    S_FOR,
    S_WHILE,
    S_BREAK,
    S_CONTINUE,
    S_RETURN,
    S_PASS,
    S_GLOBAL,
    S_DEF,
    S_CLASS,
    S_PRINT,
    S_IMPORT,
    S_IMPORTFROM,
    S_TRY,
    S_TRYFINALLY,
    S_RAISE,
    S_DELETE,
    S_ASSERT,
    S_EXEC,
    S_WITH,
    S_UNSUPPORTED,
    // helpers
    H_COMPREHENSION,
    H_HANDLER,
};

enum BinOpK { B_ADD, B_SUB, B_MUL, B_DIV, B_FLOORDIV, B_MOD, B_POW, B_LSHIFT, B_RSHIFT, B_BITOR, B_BITXOR, B_BITAND };
enum CmpOpK { C_EQ, C_NE, C_LT, C_LE, C_GT, C_GE, C_IS, C_ISNOT, C_IN, C_NOTIN };
enum UnOpK { U_NOT, U_NEG, U_POS, U_INVERT };

struct FuncInfo;

struct Node {
    NK k = E_UNSUPPORTED;
    int op = 0;
    Value v;
    std::string s;
    Node *a = nullptr, *b = nullptr, *c = nullptr;
    std::vector<Node *> l1, l2, l3;
    std::vector<std::string> names, names2;
    std::vector<int> ops;
    std::shared_ptr<FuncInfo> fi;
};

struct FuncInfo {
    std::string name;
    std::vector<Node *> params; // E_NAME or E_TUPLE (unpacking)
    std::vector<Node *> defaults;
    std::string vararg, kwarg;
    std::vector<Node *> body;
    Node *expr = nullptr; // lambda
    std::unordered_set<std::string> locals, globals;
};

struct Frame : std::enable_shared_from_this<Frame> {
    std::unordered_map<std::string, Value> locals;
    std::shared_ptr<Frame> parent;
    FuncInfo *fi = nullptr; // null: module level (names live in the store)
};
using FrameP = std::shared_ptr<Frame>;

// Engine control flow that must cross Python try/except blocks untouched.
struct EngineSignal {
    virtual ~EngineSignal() {}
};

struct Interp {
    InstObj *store = nullptr;  // global namespace
    Value storeValue;
    AttrMap builtins;
    std::map<std::string, Value> natives;                         // qualified name -> callable
    std::map<std::string, std::map<std::string, NativeFn>> methods; // class -> method -> fn
    std::map<std::string, NativeFn> instCall;                       // class -> __call__
    std::map<std::string, Value> modules;                           // import name -> module
    std::deque<Node> arena;
    std::vector<Node *> codes; // code table (Module / Expression roots)
    std::string currentFile;
    int currentLine = 0;

    void init();
    // Compile the "codes" list of the data file.
    void loadCodes(const Value &codes, const Value &astFields);

    Value eval(int code, FrameP frame = nullptr);
    void exec(int code, FrameP frame = nullptr, bool hide = false);
    Value evalNode(Node *n, Frame *f);
    Value call(const Value &fn, CallArgs &args);
    Value call(const Value &fn, std::vector<Value> pos = {}) {
        CallArgs a;
        a.pos = std::move(pos);
        return call(fn, a);
    }

    Value getAttr(const Value &obj, const std::string &name);
    bool tryGetAttr(const Value &obj, const std::string &name, Value &out);
    void setAttr(const Value &obj, const std::string &name, const Value &v);
    Value getItem(const Value &obj, const Value &key);
    void setItem(const Value &obj, const Value &key, const Value &v);
    Value iterList(const Value &obj); // returns a list/tuple value with the items
    Value binop(int op, const Value &a, const Value &b);
    bool contains(const Value &container, const Value &item);

    Value getGlobal(const std::string &name);
    bool hasGlobal(const std::string &name) const { return store->attrs.count(name) != 0; }
    void setGlobal(const std::string &name, const Value &v) { store->attrs[name] = v; }

    void registerNative(const std::string &qualified, NativeFn fn);
    Value module(const std::string &name); // creates if missing

    // statement execution result
    enum Flow { F_NORMAL, F_BREAK, F_CONTINUE, F_RETURN };
    Flow execBody(const std::vector<Node *> &body, Frame *f, Value &ret);

  private:
    Node *compile(const Value &v);
    std::vector<Node *> compileList(const Value &v);
    Flow execStmt(Node *n, Frame *f, Value &ret);
    Value lookup(const std::string &name, Frame *f);
    void assign(Node *target, const Value &v, Frame *f);
    void del(Node *target, Frame *f);
    Value callPy(FuncObj *fn, CallArgs &args);
    Value makeFunction(const std::shared_ptr<FuncInfo> &fi, Frame *f);
    Value comprehension(Node *n, Frame *f);
    Value slice(const Value &obj, Node *sl, Frame *f);
    void collectLocals(FuncInfo &fi, const std::vector<Node *> &body);
    std::map<std::string, std::vector<std::string>> astFields;
    int field(const std::string &node, const char *name);
    Value fieldOf(const Value &tup, const std::string &node, const char *name);
    std::string methodName;
};

extern Interp PY;

// argument helpers for natives
double argNum(const CallArgs &a, size_t i, const char *name, double def);
std::string argStr(const CallArgs &a, size_t i, const char *name, const std::string &def = "");
bool argBool(const CallArgs &a, size_t i, const char *name, bool def);
