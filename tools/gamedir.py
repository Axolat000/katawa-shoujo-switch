"""Locates the Steam install of Katawa Shoujo (the directory holding "Katawa Shoujo.py").

Order: $KS_GAME_ROOT, then every Steam library (libraryfolders.vdf) of the usual Steam roots.
Run directly to print the path (used by run_dump.sh).
"""
import os, re, sys

APP_DIR = "Katawa Shoujo"
MARKER = "Katawa Shoujo.py"


def steam_roots():
    home = os.path.expanduser("~")
    if os.name == "nt":
        roots = [os.path.join(os.environ.get(v, d), "Steam")
                 for v, d in (("ProgramFiles(x86)", r"C:\Program Files (x86)"), ("ProgramFiles", r"C:\Program Files"))]
    elif sys.platform == "darwin":
        roots = [os.path.join(home, "Library", "Application Support", "Steam")]
    else:
        roots = [os.path.join(home, ".steam", "steam"),            # symlink set up by every Steam package
                 os.path.join(home, ".steam", "root"),
                 os.path.join(home, ".local", "share", "Steam"),
                 os.path.join(home, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam")]  # Flatpak
    return roots


def libraries():
    seen = []
    for root in steam_roots():
        cands = [root]
        vdf = os.path.join(root, "steamapps", "libraryfolders.vdf")
        if os.path.isfile(vdf):
            with open(vdf, encoding="utf-8", errors="replace") as f:
                cands += [p.replace("\\\\", "\\") for p in re.findall(r'"path"\s+"([^"]*)"', f.read())]
        for c in cands:
            c = os.path.realpath(c)
            if os.path.isdir(c) and c not in seen:
                seen.append(c)
    return seen


def find_game_root():
    env = os.environ.get("KS_GAME_ROOT")
    if env:
        if not os.path.isfile(os.path.join(env, MARKER)):
            sys.exit("KS_GAME_ROOT=%s does not contain %s" % (env, MARKER))
        return os.path.abspath(env)
    libs = libraries()
    for lib in libs:
        d = os.path.join(lib, "steamapps", "common", APP_DIR)
        if os.path.isfile(os.path.join(d, MARKER)):
            return d
    sys.exit("Katawa Shoujo not found in any Steam library (%s); set KS_GAME_ROOT to the game directory"
             % (", ".join(libs) or "no Steam install found"))


if __name__ == "__main__":
    print(find_game_root())
