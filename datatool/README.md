# SSX Vita data tool

Builds the port's data folder (`ssx/`, to copy to `ux0:data/`) from the SSX Android APK, so that
no game data has to be distributed. Users get it packaged from the releases; see the main
[README](../README.md) for using it, and [docs/BUILDING.md](../docs/BUILDING.md) for building it.

| Module | Step |
|---|---|
| `ssxdata/apk.py` | Checks the APK (SHA-256 of its `libgame.so`), extracts the libraries and `assets/` |
| `ssxdata/audio.py` | Audio banks: channels past the first two dropped, environment loops to mono |
| `ssxdata/patches.py` | `memory.cfg` heap sizes, `main.scrape` render size, `nav.lua` garbage collection |
| `ssxdata/textures.py` | Big 3D textures and the loading screens at the Vita's screen size |
| `ssxdata/movies.py` | The movies re-encoded to H.264 960×544 and AAC, which the Vita's decoder plays (needs ffmpeg) |
| `ssxdata/layout.py` | Big folders split into subfolders, small files packed into `assets.pak` |
| `ssxdata/pipeline.py` | Runs the steps in order, with progress |
| `ssxdata/gui.py`, `ssxdata/cli.py` | The window and the command line, both over `pipeline.build` |

Each module starts with what it changes and why, with what it measured on the Vita. The port
reads the result as it is: the hash functions and file formats in `layout.py` must match
`source/ssx/assets.c` and `source/reimpl/asset_manager.cpp`.
