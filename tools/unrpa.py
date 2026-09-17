"""Extract Ren'Py RPA-3.0 archives."""
import os, sys, pickle, zlib

def extract(path, out):
    with open(path, 'rb') as f:
        header = f.readline().decode()
        parts = header.split()
        offset, key = int(parts[1], 16), int(parts[2], 16)
        f.seek(offset)
        index = pickle.loads(zlib.decompress(f.read()), encoding='bytes')
        for name, entries in index.items():
            if isinstance(name, bytes):
                name = name.decode('utf-8')
            dest = os.path.join(out, name)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            data = b''
            for e in entries:
                off, ln = e[0] ^ key, e[1] ^ key
                prefix = e[2] if len(e) > 2 else b''
                if isinstance(prefix, str):
                    prefix = prefix.encode('latin-1')
                f.seek(off)
                data += prefix + f.read(ln - len(prefix))
            with open(dest, 'wb') as o:
                o.write(data)
            print(name, len(data))

if __name__ == '__main__':
    out = sys.argv[1]
    for p in sys.argv[2:]:
        extract(p, out)
