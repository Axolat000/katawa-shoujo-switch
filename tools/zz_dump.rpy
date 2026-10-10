# Copied into the game's game/ directory by tools/run_dump.sh for the duration of the dump. When KS_DUMPER is set, runs the dumper at the very
# end of the init phase and exits before Ren'Py opens its window.
init 999999 python:
    import os, sys
    if os.environ.get("KS_DUMPER"):
        try:
            execfile(os.environ["KS_DUMPER"], {"__name__": "ks_dump"})
        finally:
            sys.stdout.flush()
            sys.stderr.flush()
            os._exit(0)
