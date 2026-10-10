# Katawa Shoujo — native Nintendo Switch port

Native homebrew port (`.nro`) of the visual novel *Katawa Shoujo* (Four Leaf Studios, Ren'Py 6.16).
Ren'Py does not run on the console: a C++17 engine (SDL2 + OpenGL ES 2) executes the original scripts.

This repository contains **no game content** (images, audio, video, text). All data is generated from
your own copy of the game (Steam release).

## How it works

- **Init dump** (`tools/dump_init.py`): the game's real Ren'Py 6.16 runs on PC up to the end of its
  init phase, then the image registry, transforms, transitions, characters, styles and the strings of
  every language are serialized.
- **Converter** (`tools/convert.py`): the `.rpyc` files are linearized into bytecode (one file per
  language), and every Python snippet is parsed by the Python 2.7 interpreter shipped with the game
  (`tools/py2ast.py`).
- **Engine** (`source/`):
  - a small Python 2 interpreter (`py*.cpp`);
  - Ren'Py-style displayables, ATL and transitions (`disp`, `transform`, `transition`, `scene`);
  - rich text layout (`text`);
  - a multi-channel audio mixer built on stb_vorbis (`audio`);
  - MPEG-1 video playback with pl_mpeg (`video`);
  - the game's own screens rewritten natively (`screens`, `extras`).

## Building

Requirements: devkitPro (devkitA64, libnx, `switch-sdl2`, `switch-sdl2_image`, `switch-sdl2_ttf`,
`switch-mesa`), Python 3 with Pillow, bash (Linux, or Git Bash on Windows), ffmpeg, and a Steam copy of Katawa Shoujo.

```sh
# 1. Extract the game and language archives
GAME="$(python tools/gamedir.py)"   # Steam install of the game (or set KS_GAME_ROOT)
python tools/unrpa.py extracted "$GAME/game/data.rpa"
for f in "$GAME"/game/lang-*.rpa; do python tools/unrpa.py extracted_lang "$f"; done

# 2. Dump the Ren'Py init state (the Steam install is found automatically; set KS_GAME_ROOT to override)
./tools/run_dump.sh

# 3. Convert the scripts, copy the assets, download the icon and transcode the videos
python tools/convert.py
python tools/assets.py --videos

# 4. Build
./build_switch.sh      # -> KatawaShoujo.nro
./build_switch.sh nsp  # -> KatawaShoujo.nsp (optional, see below)
./build_pc.sh          # Windows test build -> build_pc/ks.exe
```

## Installing

Copy `KatawaShoujo.nro` to `sdmc:/switch/`. Saves and the log file are written to
`sdmc:/switch/KatawaShoujo/`. Title takeover (hold R while launching a game) is recommended.

### NSP

`make nsp` / `./build_switch.sh nsp` packs an installable NSP (title ID `01004B5300000000`) with
its own Home Menu icon, running as a full application without title takeover. It needs:

- [hacBrewPack](https://github.com/rlaphoenix/hacBrewPack) v3.05 on the `PATH` (or `HACBREWPACK=`);
  the original repository was deleted, that mirror's `745b16e` is the original v3.05 commit;
- `prod.keys` dumped from your own console: `~/.switch/prod.keys`, Eden's or yuzu's keys
  directory, or `PROD_KEYS=/path/to/prod.keys`.

The NSP is not officially signed: installing it needs custom firmware with sigpatches. Like the
`.nro`, it contains the game's data, so do not distribute it.

## Controls

| Button | Action |
|---|---|
| A / touch | Advance / confirm |
| B / + | Game menu / back |
| X | Hide the window |
| Y | Text history |
| L | Rollback |
| R | Toggle skip |
| ZR (held) | Skip |
| ZL | Auto mode |

## Testing

A `test.txt` file in the user directory (or environment variables) drives automated tests:
`KS_SHOTS=10,20` takes screenshots, `KS_KEYS=12:A,15:S` injects inputs.

## Status

Working and verified: main menu, prologue, the whole of Act 1 in skip mode, choice menus, chapter
videos, and the build running under the Eden emulator. Written but not yet fully tested: saving and
loading, rollback, text history, the Options / Language / Extras screens, NVL mode, written notes,
two-character dialogue, and the credits.

## License

The original code and tools of this port are available under the [MIT License](LICENSE).
Copyright (c) 2026 Axolat000.

This license does not cover Katawa Shoujo's original scripts, artwork, music, videos,
fonts, or other game assets. Those materials remain subject to their respective licenses.
Third-party libraries and code retain their own licenses and copyright notices.

## Credits

*Katawa Shoujo* © Four Leaf Studios (CC BY-NC-ND 3.0); the app icon, built from its key art, is
downloaded from SteamGridDB at build time. Libraries: SDL2, stb_vorbis (public domain), pl_mpeg (MIT).
