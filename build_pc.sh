#!/usr/bin/env bash
# PC (Windows) test build with the WinLibs mingw toolchain. Incremental, parallel.
# Usage: ./build_pc.sh [clean]      Env: JOBS, EXTRA_CXXFLAGS
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
OBJ_DIR=build_pc/obj
EXE=build_pc/ks.exe
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
DKP_INC="$(cygpath -m /c/devkitPro/portlibs/switch/include)"
export FLAGS="-O2 -g -DPC_BUILD -DSDL_MAIN_HANDLED -I$DKP_INC/SDL2 -I$DKP_INC ${EXTRA_CXXFLAGS:-}"
if [[ "${1:-}" == "clean" ]]; then rm -rf "$OBJ_DIR" "$EXE"; echo clean.; exit 0; fi
mkdir -p "$OBJ_DIR"
export OBJ_DIR

compile_one() {
    local src="$1" b obj
    b="$(basename "$src")"; obj="$OBJ_DIR/${b%.*}.o"
    if [[ "$src" == *.c ]]; then
        gcc $FLAGS -w -MMD -MP -c "$src" -o "$obj"
    else
        g++ -std=gnu++17 $FLAGS -Wall -Wno-unused-function -MMD -MP -c "$src" -o "$obj"
    fi
    echo "  CC $src"
}
export -f compile_one

needs_build() {
    local src="$1" b obj dep f
    b="$(basename "$src")"; obj="$OBJ_DIR/${b%.*}.o"; dep="${obj%.o}.d"
    [[ -f "$obj" && -f "$dep" ]] || return 0
    [[ "$src" -nt "$obj" ]] && return 0
    while read -r f; do
        [[ -z "$f" || "$f" == *: ]] && continue
        [[ ! -e "$f" || "$f" -nt "$obj" ]] && return 0
    done < <(sed -e 's/\\$//' "$dep" | tr ' \t' '\n\n')
    return 1
}

shopt -s nullglob
SOURCES=(source/*.cpp source/*.c)
STALE=()
for s in "${SOURCES[@]}"; do needs_build "$s" && STALE+=("$s"); done
if [[ ${#STALE[@]} -gt 0 ]]; then
    printf '%s\n' "${STALE[@]}" | xargs -P "$JOBS" -I{} bash -c 'compile_one "$1"' _ {}
fi
OBJS=()
for s in "${SOURCES[@]}"; do b="$(basename "$s")"; OBJS+=("$OBJ_DIR/${b%.*}.o"); done
g++ -o "$EXE" "${OBJS[@]}" build_pc/SDL2.dll build_pc/SDL2_image.dll build_pc/SDL2_ttf.dll build_pc/libGLESv2.dll \
    -mconsole -static-libgcc -static-libstdc++ -Wl,--stack,67108864
echo ">> OK: $EXE"
