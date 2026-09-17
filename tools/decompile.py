"""Decompile Ren'Py 6.16 .rpyc files (Katawa Shoujo) into readable .rpy text."""
import sys, os, glob
sys.path.insert(0, os.path.dirname(__file__))
from rpyc_dump import load_rpyc, Stub


def s(v):
    if isinstance(v, bytes):
        return v.decode('utf-8', 'replace')
    return v


def norm(n):
    """Recursively convert bytes keys/values to str."""
    if isinstance(n, Stub):
        d = {s(k): norm(v) for k, v in list(n.__dict__.items())}
        n.__dict__.clear()
        n.__dict__.update(d)
        return n
    if isinstance(n, bytes):
        return s(n)
    if isinstance(n, str):
        return n
    if isinstance(n, list):
        return [norm(x) for x in n]
    if isinstance(n, tuple):
        return tuple(norm(x) for x in n)
    if isinstance(n, dict):
        return {norm(k): norm(v) for k, v in n.items()}
    return n


def cls(n):
    return n._cls.rsplit('.', 1)[1] if isinstance(n, Stub) else type(n).__name__


def pycode(pc):
    st = pc.__dict__.get('_state')
    return s(st[1])


def q(t):
    return '"' + t.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'


def imspec(im):
    name, expression, tag, at_list, layer, zorder, behind = im[:7]
    out = ' '.join(name) if not expression else 'expression ' + expression
    if tag:
        out += ' as ' + tag
    if at_list:
        out += ' at ' + ', '.join(at_list)
    if layer and layer != 'master':
        out += ' onlayer ' + layer
    if zorder:
        out += ' zorder ' + zorder
    if behind:
        out += ' behind ' + ', '.join(behind)
    return out


def args(ai):
    if ai is None:
        return ''
    parts = []
    for k, v in ai.arguments:
        parts.append(v if k is None else '%s=%s' % (k, v))
    if ai.extrapos:
        parts.append('*' + ai.extrapos)
    if ai.extrakw:
        parts.append('**' + ai.extrakw)
    return '(' + ', '.join(parts) + ')'


def params(pi):
    if pi is None:
        return ''
    parts = []
    for k, v in pi.parameters:
        parts.append(k if v is None else '%s=%s' % (k, v))
    if pi.extrapos:
        parts.append('*' + pi.extrapos)
    if pi.extrakw:
        parts.append('**' + pi.extrakw)
    return '(' + ', '.join(parts) + ')'


def atl(block, ind, out):
    p = '    ' * ind
    for st in block.statements:
        c = cls(st)
        if c == 'RawMultipurpose':
            parts = []
            if st.warp_function:
                parts.append('warp %s %s' % (st.warp_function, st.duration))
            elif st.warper:
                parts.append('%s %s' % (st.warper, st.duration))
            elif st.duration not in ('0', None):
                parts.append('pause %s' % st.duration)
            if st.revolution:
                parts.append(st.revolution)
            if st.circles not in ('0', None):
                parts.append('circles ' + st.circles)
            for k, v in st.properties:
                parts.append('%s %s' % (k, v.strip()))
            for e in st.expressions:
                e0, w = e
                parts.append(e0 + (' with ' + w if w else ''))
            for sp in st.splines:
                parts.append('knot ' + repr(sp))
            out.append(p + (' '.join(parts) if parts else 'pass'))
        elif c == 'RawBlock':
            out.append(p + 'block:')
            atl(st, ind + 1, out)
        elif c == 'RawRepeat':
            out.append(p + 'repeat' + (' ' + st.repeats if st.repeats else ''))
        elif c == 'RawParallel':
            for b in st.blocks:
                out.append(p + 'parallel:')
                atl(b, ind + 1, out)
        elif c == 'RawChoice':
            for ch, b in st.choices:
                out.append(p + 'choice %s:' % ch)
                atl(b, ind + 1, out)
        elif c == 'RawFunction':
            out.append(p + 'function ' + st.expr)
        elif c == 'RawOn':
            for name, b in st.handlers.items():
                out.append(p + 'on %s:' % name)
                atl(b, ind + 1, out)
        elif c == 'RawTime':
            out.append(p + 'time ' + st.time)
        elif c == 'RawEvent':
            out.append(p + 'event ' + st.name)
        elif c == 'RawContainsExpr':
            out.append(p + 'contains ' + st.expression)
        elif c == 'RawChild':
            for b in st.children:
                out.append(p + 'contains:')
                atl(b, ind + 1, out)
        else:
            out.append(p + '# ATL?? ' + c + ' ' + repr(st.__dict__))


def block(stmts, ind, out):
    p = '    ' * ind
    for n in stmts:
        c = cls(n)
        if c == 'Label':
            out.append(p + 'label %s%s:' % (n.name, params(n.__dict__.get('parameters'))))
            block(n.block, ind + 1, out)
        elif c == 'Say':
            w = (n.who + ' ') if n.who else ''
            if n.__dict__.get('attributes'):
                w += ' '.join(n.attributes) + ' '
            line = p + w + q(n.what)
            if not n.interact:
                line += ' nointeract'
            if n.with_:
                line += ' with ' + n.with_
            out.append(line)
        elif c == 'Show':
            out.append(p + 'show ' + imspec(n.imspec) + (':' if n.atl else ''))
            if n.atl:
                atl(n.atl, ind + 1, out)
        elif c == 'Scene':
            out.append(p + 'scene' + (' ' + imspec(n.imspec) if n.imspec else '') + (':' if n.atl else ''))
            if n.atl:
                atl(n.atl, ind + 1, out)
        elif c == 'Hide':
            out.append(p + 'hide ' + imspec(n.imspec))
        elif c == 'With':
            if n.paired:
                out.append(p + '# with (paired) ' + n.expr)
            else:
                out.append(p + 'with ' + n.expr)
        elif c == 'Python':
            code = pycode(n.code)
            if '\n' in code.strip():
                out.append(p + 'python%s:' % (' hide' if n.hide else ''))
                for l in code.split('\n'):
                    out.append(p + '    ' + l)
            else:
                out.append(p + '$ ' + code.strip())
        elif c == 'EarlyPython':
            out.append(p + 'python early:')
            for l in pycode(n.code).split('\n'):
                out.append(p + '    ' + l)
        elif c == 'Init':
            out.append(p + 'init %d:' % n.priority)
            block(n.block, ind + 1, out)
        elif c == 'Image':
            if n.atl:
                out.append(p + 'image %s:' % ' '.join(n.imgname))
                atl(n.atl, ind + 1, out)
            else:
                out.append(p + 'image %s = %s' % (' '.join(n.imgname), pycode(n.code).strip()))
        elif c == 'Transform':
            out.append(p + 'transform %s%s:' % (n.varname, params(n.parameters)))
            atl(n.atl, ind + 1, out)
        elif c == 'Call':
            out.append(p + 'call %s%s%s' % ('expression ' if n.expression else '', n.label, args(n.arguments)))
        elif c == 'Jump':
            out.append(p + 'jump %s%s' % ('expression ' if n.expression else '', n.target))
        elif c == 'Return':
            out.append(p + 'return' + (' ' + n.expression if n.expression else ''))
        elif c == 'Pass':
            out.append(p + 'pass')
        elif c == 'If':
            for i, (cond, blk) in enumerate(n.entries):
                kw = 'if' if i == 0 else 'elif'
                if i > 0 and cond == 'True':
                    out.append(p + 'else:')
                else:
                    out.append(p + '%s %s:' % (kw, cond))
                block(blk, ind + 1, out)
        elif c == 'While':
            out.append(p + 'while %s:' % n.condition)
            block(n.block, ind + 1, out)
        elif c == 'Menu':
            out.append(p + 'menu:' + ('  # with ' + n.with_ if n.with_ else '') + ('  # set ' + n.set if n.set else ''))
            for label, cond, blk in n.items:
                if blk is None:
                    out.append(p + '    ' + q(label))
                else:
                    out.append(p + '    ' + q(label) + ('' if cond == 'True' else ' if ' + cond) + ':')
                    block(blk, ind + 2, out)
        elif c == 'UserStatement':
            out.append(p + n.line)
            if n.__dict__.get('block'):
                out.append(p + '    # block ' + repr(n.block))
        elif c == 'Define':
            out.append(p + 'define %s = %s' % (n.varname, pycode(n.code).strip()))
        elif c == 'Screen':
            out.append(p + '# screen ' + repr(n.__dict__))
        else:
            out.append(p + '# ?? ' + c + ' ' + repr(n.__dict__)[:300])


def decompile(path):
    d, stmts = load_rpyc(path)
    stmts = norm(stmts)
    out = []
    block(stmts, 0, out)
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    files = sorted(glob.glob(os.path.join(src, '*.rpyc'))) if os.path.isdir(src) else [src]
    for f in files:
        name = os.path.splitext(os.path.basename(f))[0] + '.rpy'
        with open(os.path.join(dst, name), 'w', encoding='utf-8') as o:
            o.write(decompile(f))
        print(name)
