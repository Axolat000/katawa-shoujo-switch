#!/usr/bin/env bash
# Build the Switch .nro from Git Bash (or any MSYS shell) using devkitPro's msys2.
#
# devkitPro's makefiles break on spaces in the project path, so when the path
# contains spaces this script maps the project dir to a free drive letter with
# `subst` (e.g. W: -> /w in msys2), builds there, then removes the mapping.
#
# Usage: ./build_switch.sh [make args...]      e.g. ./build_switch.sh clean
# Env:   DKP_BASH=path to devkitPro msys2 bash (default /c/devkitPro/msys2/usr/bin/bash)
#        SUBST_DRIVE=X   preferred drive letter
#        KEEP_SUBST=1    keep the drive mapping after the build
set -euo pipefail

DKP_BASH="${DKP_BASH:-/c/devkitPro/msys2/usr/bin/bash}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ ! -x "$DKP_BASH" ]]; then
    echo "error: devkitPro msys2 bash not found at $DKP_BASH (set DKP_BASH)" >&2
    exit 1
fi

SUBST_EXE="$(cygpath -u "${SYSTEMROOT:-C:\\Windows}")/System32/subst.exe"
CREATED_DRIVE=""

cleanup() {
    if [[ -n "$CREATED_DRIVE" && "${KEEP_SUBST:-0}" != "1" ]]; then
        MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$SUBST_EXE" "${CREATED_DRIVE}:" /D || true
    fi
}
trap cleanup EXIT

if [[ "$SCRIPT_DIR" != *" "* ]]; then
    BUILD_DIR="$SCRIPT_DIR"
else
    WIN_DIR="$(cygpath -w "$SCRIPT_DIR")"
    # Reuse an existing mapping of this directory if there is one.
    # subst output lines look like:  W:\: => C:\path\to\dir
    DRIVE="$("$SUBST_EXE" | tr -d '\r' | WIN_DIR="$WIN_DIR" awk '
        { d = ENVIRON["WIN_DIR"]; i = index($0, ": => ");
          if (i && tolower(substr($0, i + 5)) == tolower(d)) { print substr($0, 1, 1); exit } }')"
    if [[ -z "$DRIVE" ]]; then
        for L in ${SUBST_DRIVE:-} W V U T S R Q P O N M L K J; do
            l="$(echo "$L" | tr 'A-Z' 'a-z')"
            if [[ ! -e "/$l/" ]] && MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$SUBST_EXE" "${L}:" "$WIN_DIR" 2>/dev/null; then
                DRIVE="$L"; CREATED_DRIVE="$L"
                break
            fi
        done
    fi
    if [[ -z "$DRIVE" ]]; then
        echo "error: could not map '$WIN_DIR' to a drive letter with subst" >&2
        exit 1
    fi
    BUILD_DIR="/$(echo "$DRIVE" | tr 'A-Z' 'a-z')"
    echo ">> path contains spaces: building via ${DRIVE}: ($BUILD_DIR)"
fi

MAKE_ARGS=("$@")
if [[ " ${MAKE_ARGS[*]:-} " != *" -j"* ]]; then
    MAKE_ARGS=("-j$(nproc 2>/dev/null || echo 4)" ${MAKE_ARGS[@]+"${MAKE_ARGS[@]}"})
fi
QUOTED_ARGS="$(printf '%q ' "${MAKE_ARGS[@]}")"

status=0
MSYSTEM=MSYS CHERE_INVOKING=1 "$DKP_BASH" -lc "cd $(printf '%q' "$BUILD_DIR") && make $QUOTED_ARGS" || status=$?

if [[ $status -eq 0 && -f "$SCRIPT_DIR/KatawaShoujo.nro" ]]; then
    echo ">> OK: $SCRIPT_DIR/KatawaShoujo.nro"
fi
exit $status
