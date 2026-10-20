#!/usr/bin/env python3
"""
SSX Vita data tool: builds the port's data folder from the SSX Android APK.

    ssx_datatool.py                          the window
    ssx_datatool.py <apk> [output folder]    the command line (--help)
"""

import sys

if __name__ == "__main__":
    # Packaged window builds have no console (sys.stdout is None): window only
    if len(sys.argv) > 1 and sys.stdout is not None:
        from ssxdata.cli import main
    else:
        from ssxdata.gui import main
    sys.exit(main())
