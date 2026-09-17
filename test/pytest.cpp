#include "../source/py.h"
#include <SDL.h>

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::vector<uint8_t> data;
    double t0 = nowSeconds();
    if (!readFile(argc > 1 ? argv[1] : "romfs/data/game.bin", data)) {
        printf("no game.bin\n");
        return 1;
    }
    ValueFile vf;
    bool ok = vf.load(data);
    printf("load ok=%d strings=%zu objects=%zu in %.2fs\n", ok, vf.strings.size(), vf.objects.size(), nowSeconds() - t0);
    DictObj *root = vf.root.dict();
    auto R = [&](const char *k) {
        const Value *v = root->find(Value::str(k));
        return v ? *v : Value();
    };
    PY.init();
    t0 = nowSeconds();
    PY.loadCodes(R("codes"), R("ast_fields"));
    printf("codes %zu compiled in %.2fs, arena %zu\n", PY.codes.size(), nowSeconds() - t0, PY.arena.size());
    Value store = mkInst("store");
    PY.store = store.inst();
    PY.storeValue = store;
    for (auto &kv : R("store").dict()->items)
        PY.store->attrs[kv.first.s()] = kv.second;
    PY.store->attrs["config"] = mkInst("config");
    PY.store->attrs["persistent"] = mkInst("persistent");
    printf("store vars %zu\n", PY.store->attrs.size());
    int defsOk = 0, defsErr = 0;
    for (auto &c : R("defs").list()->v) {
        Node *blk = PY.codes[c.i];
        if (!blk)
            continue;
        for (Node *s : blk->l1) {
            if (s->k != S_DEF)
                continue;
            if (getenv("TRACE"))
                printf("def %s\n", s->s.c_str());
            try {
                Value ret;
                std::vector<Node *> one{s};
                PY.execBody(one, nullptr, ret);
                defsOk++;
            } catch (PyError &e) {
                defsErr++;
                printf("def err %s: %s %s\n", s->s.c_str(), e.type.c_str(), e.msg.c_str());
            } catch (std::exception &e) {
                printf("std exc in %s: %s\n", s->s.c_str(), e.what());
            }
        }
    }
    printf("defs ok %d err %d\n", defsOk, defsErr);
    auto tryCall = [&](const char *name, std::vector<Value> args) {
        try {
            Value r = PY.call(PY.getGlobal(name), args);
            printf("%s -> %s\n", name, valueRepr(r).c_str());
        } catch (PyError &e) {
            printf("%s ERR %s: %s\n", name, e.type.c_str(), e.msg.c_str());
        }
    };
    tryCall("acdc_warp", {Value::real(0.3)});
    tryCall("scaled_runtime", {Value::real(4.0), Value::real(1.0)});
    tryCall("make_sprite_path", {Value::str("emi"), Value::str("basic_grin"), Value::str("gym"), Value::boolean(true)});
    tryCall("time_from_seconds", {Value::real(3725.4)});
    tryCall("make_percentage", {Value::integer(3), Value::integer(7)});
    tryCall("prefix_dict", {PY.store->attrs["displayStrings"].inst()->get("styleoverrides"), Value::str("what_"), Value::boolean(true)});
    tryCall("name_from_label", {Value::str("A1")});
    tryCall("get_available_scenes", {Value::str("Act 1"), Value::boolean(true)});
    return 0;
}
