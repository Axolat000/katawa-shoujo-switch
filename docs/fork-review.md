# Noorquacker fork integration review

Reviewed on 2026-10-10. Base: `32d6855` (`main`). Imported upstream:
`Noorquacker/katawa-shoujo-switch@ac25f539b81f7a05b022e089a6b6224371a59b3c`.
Integration commit: `9e583d4`, branch `review/noorquacker-fork`.

All five upstream commits' changes are included in one commit attributed to Noorquacker.
The imported tree differs from upstream only in the existing MIT LICENSE and README
license section. This review document is a separate addition. Uncommitted changes in
the original checkout were not included or modified. No game data or keys are committed.

Recommendation: keep this branch separate until the findings below are addressed.
This review records the fork as imported; it does not fix its behavior.

## Findings

### P1: Preserve existing dump hooks before installing temporary files

Location: `tools/run_dump.sh:13-16`.

The wrapper unconditionally deletes `game/zz_dump.rpy` and `game/zz_dump.rpyc`
before copying its own hook, and deletes them again on exit. When a user points
`KS_GAME_ROOT` at a previously configured reference copy, this destroys their
existing hook, even if the runtime subsequently fails. This is relevant to the
previous documented workflow, which required such a hook in `pc_ref`.

Reproduced with two sentinel files in a disposable fake game directory: after a
failed invocation both files were absent. Back up and restore pre-existing files,
or refuse the collision before touching either file. Working in a disposable game
copy would also avoid modifying the Steam installation.

### P2: Automatic NSP key discovery fails when HOME contains spaces

Location: `Makefile:49-51`.

`$(wildcard $(HOME)/...)` splits paths at spaces, and `$(firstword ...)` cannot
preserve a multiword filename either. An existing key under a user directory such
as `User With Spaces/.switch/prod.keys` is therefore not detected. The NSP target
then reports a missing key although the documented location contains one.

Reproduced using GNU make and a dummy file in a temporary home directory with
spaces: the exact discovery expression produced an empty value. Use quoted shell
path checks to select a complete filename. An explicit `PROD_KEYS=...` argument
is a workaround; the recipe itself quotes that argument.

### P2: An unavailable icon CDN prevents all first-time asset preparation

Location: `tools/assets.py:52-65`.

The newly mandatory icon download executes before the asset-copying and video
work, has no explicit timeout, and propagates network failures. With no cached
`icon.jpg`, a CDN outage or an offline build prevents preparing locally owned game
assets. The previous version did not require this network step.

Reproduced by making `urlopen` raise a simulated network error: `main()` exited
before creating either output directory. Make icon acquisition separately
invocable or provide a local fallback/input, with a bounded download timeout and
an actionable error. The real download succeeded during this review.

## Executed validation

- Bash syntax validation passed for the build scripts and dump wrapper.
- Python 3 tools parsed successfully; the two Python 2 tools compiled using the
  game's bundled Python 2 runtime. They are intentionally not Python 3 compatible.
- Automatic Steam game detection succeeded on Windows.
- Real initialization dump succeeded on an isolated, unpatched copy of the Steam
  installation: 42,489 objects and 5,277 registered images. Steam was not modified.
- Script conversion succeeded: 3,170 Python snippets, zero parse errors, and base
  plus language data files generated. An initial sandbox run could not launch the
  bundled Python 2 runtime; the unrestricted rerun succeeded.
- Asset preparation copied 1,762 files, indexed 1,646 images, downloaded the icon,
  and transcoded 23 videos successfully.
- A clean PC compilation succeeded. A smoke test visually confirmed the animated
  introductory logo, main menu, snowy prologue and dialogue advancement. Runtime
  initialization reported 473 Python definitions loaded and zero failures.
- A clean Switch NRO compilation succeeded through the Windows drive-substitution
  wrapper: `KatawaShoujo.nro`, 548,803,282 bytes. Existing compiler warnings remain.
- The NSP target generated its executable, NPDM and control staging files, then
  stopped at `hacbrewpack: command not found`. The standalone NPDM conversion
  produced 996 bytes, with warnings about absent optional/defaulted fields.
- The three failure cases above were reproduced with disposable fixtures and
  dummy key data. No real keys were displayed or uploaded.
- A mocked Linux NRO build returned success; this checks wrapper control flow
  only, not native Linux compilation.
- Git whitespace validation passed.

## Validation limits

There is no installed WSL/Linux environment here. Native Linux compilation and
the bundled Linux Ren'Py runtime remain untested. hacBrewPack is not installed,
so complete NSP packaging and installation remain unverified. The newly built NRO
has not been run on a console or emulator in this review. PC smoke tests do not
establish full-route, save/load or hardware compatibility.

Local build logs and isolated reproduction probes are in the ignored `build/`
directory of the review checkout. Screenshots are local in `userdata/` and are
not included in this branch.

## Follow-up: local features and settings viewport

The branch now also includes the original checkout's pending native features:
three aspect modes, optional bicubic filtering, persistent settings and optional
patch support (disabled by default). The original checkout remains untouched.

The Special features screen now measures and wraps its text, clips rendering and
pointer hit tests to a scrollable viewport, follows keyboard/controller focus,
and keeps Return separate from the scrolling content. Its heading stays visible
in zoom mode. Swiping suppresses activation on release.

Validation: PC and Switch NRO builds passed. Automated PC screenshots at 1280x720
confirmed all three aspect modes, controller focus scrolling to the final option,
persisted aspect selection after restart, and the optional toggle initially off.
Injected scroll inputs of +9999/-9999 stopped at the content boundaries; clicks
outside the viewport left the displayed options unchanged. Runtime testing used
the existing local game data. The final local NRO is 576,732,830 bytes; neither
that binary nor game assets are committed. Physical Switch touch/controller input
remains untested. The earlier fork-review findings above are not fixed by this
follow-up.

## Final fixes before merging into main

The three findings above have now been fixed. The dumper restores any existing
`zz_dump.rpy` and `zz_dump.rpyc` after a failed run; both file contents were
verified unchanged using a disposable game fixture. GNU make now detects a key
under a home directory containing spaces. If the icon CDN is unavailable, the
asset tool generates a 256x256 JPEG icon from the user's locally extracted main
menu artwork.

The experimental zoomed 16:9 mode was removed. Existing saves that selected it
fall back to 4:3. The Special features screen now hands focus from touch input
to controller A and moves focus to a visible option when scrolling with the
right stick. A PC test opened the panel through pointer input, scrolled to its
last option, and then toggled it on and back off using only A. This verifies the
reported controller issue without shipping any game or patch assets.
