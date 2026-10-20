"""Command line: ssx_datatool.py <apk> [output folder] [--overwrite]"""

import argparse
import sys

from . import __version__, apk, pipeline


def main(argv=None):
    p = argparse.ArgumentParser(prog="ssx_datatool",
                                description="Builds the SSX Vita data folder from the SSX Android APK "
                                            f"(version {apk.GAME_VERSION}).")
    p.add_argument("apk", help="the SSX APK")
    p.add_argument("output", nargs="?", default=".", help="where to write the ssx folder (default: here)")
    p.add_argument("--overwrite", action="store_true", help="replace an existing ssx folder there")
    p.add_argument("--version", action="version", version=__version__)
    args = p.parse_args(argv)

    last = [-1]

    def progress(f):
        pct = int(f * 100)
        if pct != last[0]:
            last[0] = pct
            sys.stderr.write(f"\r{pct:3d}%")
            sys.stderr.flush()

    def log(text):
        sys.stderr.write("\r    \r")
        print(text, flush=True)

    try:
        pipeline.build(args.apk, args.output, log, progress, args.overwrite)
    except (apk.ApkError, RuntimeError, OSError) as e:
        sys.stderr.write("\r    \r")
        print(f"Error: {e}", file=sys.stderr)
        return 1
    return 0
