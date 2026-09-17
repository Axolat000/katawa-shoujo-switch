# Python 2.7 tool (run with the game's bundled interpreter): parses Python sources with the real
# Python 2 grammar and writes their ASTs as JSON.
# input : JSON list of [mode, source]   (mode = "exec" | "eval")
# output: JSON list of AST (or {"error": msg})
import ast, json, sys, textwrap


def conv(n):
    if isinstance(n, ast.AST):
        out = {"_": n.__class__.__name__}
        for f in n._fields:
            out[f] = conv(getattr(n, f, None))
        if hasattr(n, "lineno"):
            out["@l"] = n.lineno
        return out
    if isinstance(n, list):
        return [conv(i) for i in n]
    if isinstance(n, str):
        try:
            return n.decode("utf-8")
        except UnicodeDecodeError:
            return n.decode("latin-1")
    if isinstance(n, float):
        if n != n or n in (float("inf"), float("-inf")):
            return {"_": "#float", "v": repr(n)}
        return {"_": "#f", "v": n}
    if isinstance(n, (int, long)):
        return n
    return n


def main():
    items = json.load(open(sys.argv[1], "rb"))
    out = []
    for mode, src in items:
        s = src.encode("utf-8")
        if mode == "exec":
            s = textwrap.dedent(s.replace("\r", ""))
        else:
            s = s.strip()
        try:
            tree = ast.parse(s, "<ks>", mode)
            out.append(conv(tree))
        except SyntaxError, e:
            out.append({"error": "%s: %r" % (e, s[:200])})
    f = open(sys.argv[2], "wb")
    json.dump(out, f, separators=(",", ":"))
    f.close()


main()
