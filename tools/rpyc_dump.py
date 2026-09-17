"""Load .rpyc files with stub classes (no Ren'Py needed) and dump the AST as a tree."""
import sys, zlib, pickle, struct, io

class Stub:
    _cls = None
    def __init__(self, *a, **k):
        self._args = a
    def __setstate__(self, state):
        if isinstance(state, tuple) and len(state) == 2:
            d, slots = state
            if d: self.__dict__.update(d)
            if slots: self.__dict__.update(slots)
        elif isinstance(state, dict):
            self.__dict__.update(state)
        else:
            self._state = state
    def __repr__(self):
        return '<%s>' % self._cls

_classes = {}
class StubUnpickler(pickle.Unpickler):
    def find_class(self, module, name):
        if module in ('builtins', '__builtin__', 'collections', 'copy_reg', 'copyreg', 'datetime', 'decimal'):
            if module == '__builtin__': module = 'builtins'
            if module == 'copy_reg': module = 'copyreg'
            return super().find_class(module, name)
        key = module + '.' + name
        if key not in _classes:
            base = Stub
            if name in ('PyExpr',):
                # PyExpr is a str subclass
                cls = type(name, (str,), {'_cls': key, '__new__': lambda c, s='', *a: str.__new__(c, s), '__setstate__': lambda self, st: None})
                cls.__module__ = module
                _classes[key] = cls
                return cls
            if name in ('RevertableList',):
                cls = type(name, (list,), {'_cls': key}); _classes[key] = cls; return cls
            if name in ('RevertableDict',):
                cls = type(name, (dict,), {'_cls': key}); _classes[key] = cls; return cls
            if name in ('RevertableSet',):
                cls = type(name, (set,), {'_cls': key, '__setstate__': lambda self, st: self.update(st[0] if isinstance(st, tuple) else st)}); _classes[key] = cls; return cls
            cls = type(name, (Stub,), {'_cls': key})
            cls.__module__ = module
            _classes[key] = cls
        return _classes[key]

def load_rpyc(path):
    data = open(path, 'rb').read()
    if data.startswith(b'RENPY RPC2'):
        pos = 10
        slots = {}
        while True:
            slot, start, length = struct.unpack('III', data[pos:pos+12])
            if slot == 0: break
            slots[slot] = data[start:start+length]
            pos += 12
        raw = zlib.decompress(slots[1])
    else:
        raw = zlib.decompress(data)
    return StubUnpickler(io.BytesIO(raw), encoding='bytes').load()

if __name__ == '__main__':
    d, stmts = load_rpyc(sys.argv[1])
    print(d)
    types = {}
    def walk(n, depth=0):
        if isinstance(n, Stub):
            types[n._cls] = types.get(n._cls, 0) + 1
            for v in n.__dict__.values(): walk(v, depth+1)
        elif isinstance(n, (list, tuple)):
            for v in n: walk(v, depth+1)
        elif isinstance(n, dict):
            for v in n.values(): walk(v, depth+1)
    walk(stmts)
    for k, v in sorted(types.items()): print(v, k)
