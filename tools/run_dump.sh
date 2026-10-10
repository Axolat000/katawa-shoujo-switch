#!/usr/bin/env bash
# Runs the real Ren'Py init of the Steam copy of the game and dumps it to build/init_dump.json
# (game located by tools/gamedir.py; set KS_GAME_ROOT to override)
# Works on Linux (bundled linux-x86_64/linux-i686 runtime) and on Windows under Git Bash.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="$(command -v python3 || command -v python)"
REF="$("$PYTHON" "$HERE/tools/gamedir.py" | tr -d '\r')"
L="$REF/lib"
echo ">> game: $REF"
mkdir -p "$HERE/build"
# The init hook only lives in the game directory while the dump runs
cleanup() { rm -f "$REF/game/zz_dump.rpy" "$REF/game/zz_dump.rpyc"; }
trap cleanup EXIT
cleanup
cp "$HERE/tools/zz_dump.rpy" "$REF/game/zz_dump.rpy"

case "$(uname -s)" in
MINGW*|MSYS*|CYGWIN*)
    p() { cygpath -w "$1"; }
    export PYTHONHOME="$(p "$L/pythonlib2.7")"
    export PYTHONPATH="$(p "$L/pythonlib2.7");$(p "$L/windows-i686/Lib")"
    PY=("$L/windows-i686/python.exe" -O)
    ;;
*)
    p() { printf '%s' "$1"; }
    case "$(uname -m)" in
    x86_64|amd64) PLAT=linux-x86_64 ;;
    i?86) PLAT=linux-i686 ;;
    *) echo "unsupported architecture: $(uname -m)" >&2; exit 1 ;;
    esac
    # Same invocation as the game's "Katawa Shoujo.sh" (-E: the binary finds its own stdlib)
    export LD_LIBRARY_PATH="$L/$PLAT${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    # No window is needed: skip the presplash subprocess, the dumper exits at the end of init
    export RENPY_LESS_UPDATES=1
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}" SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
    PY=("$L/$PLAT/python" -EO)
    ;;
esac

export KS_DUMPER="$(p "$HERE/tools/dump_init.py")"
export KS_DUMP_OUT="$(p "$HERE/build/init_dump.json")"
if [[ -f "$HERE/build/exprs.txt" ]]; then export KS_DUMP_EXPRS="$(p "$HERE/build/exprs.txt")"; fi
rm -f "$HERE/build/init_dump.json" "$HERE/build/init_dump.json.err.txt"
cd "$REF"
# Hide Steam init failures (and their traceback header) and the harmless exec_prefix warning
"${PY[@]}" "Katawa Shoujo.py" 2>&1 | awk '
    /^Traceback \(most recent call last\):$/ { held = $0; next }
    tolower($0) ~ /steam/ { held = ""; next }
    /platform dependent libraries <exec_prefix>|Consider setting \$PYTHONHOME/ { next }
    { if (held != "") print held; held = ""; print }
    END { if (held != "") print held }' || true
if [[ -f "$HERE/build/init_dump.json.err.txt" ]]; then cat "$HERE/build/init_dump.json.err.txt"; exit 1; fi
ls -la "$HERE/build/init_dump.json"
