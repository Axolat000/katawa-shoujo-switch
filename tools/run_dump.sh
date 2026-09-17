#!/usr/bin/env bash
# Runs the real Ren'Py init of ../pc_ref and dumps it to build/init_dump.json
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF="$HERE/../pc_ref"
L="$REF/lib"
mkdir -p "$HERE/build"
export KS_DUMPER="$(cygpath -w "$HERE/tools/dump_init.py")"
export KS_DUMP_OUT="$(cygpath -w "$HERE/build/init_dump.json")"
if [[ -f "$HERE/build/exprs.txt" ]]; then export KS_DUMP_EXPRS="$(cygpath -w "$HERE/build/exprs.txt")"; fi
export PYTHONHOME="$(cygpath -w "$L/pythonlib2.7")"
export PYTHONPATH="$(cygpath -w "$L/pythonlib2.7");$(cygpath -w "$L/windows-i686/Lib")"
rm -f "$HERE/build/init_dump.json" "$HERE/build/init_dump.json.err.txt"
cd "$REF"
"$L/windows-i686/python.exe" -O "Katawa Shoujo.py" 2>&1 | grep -v -i steam || true
if [[ -f "$HERE/build/init_dump.json.err.txt" ]]; then cat "$HERE/build/init_dump.json.err.txt"; exit 1; fi
ls -la "$HERE/build/init_dump.json"
