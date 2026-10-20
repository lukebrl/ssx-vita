# Building

## The VPK

The game's libraries use the **softfp** float ABI, so the port must be built with a softfp
VitaSDK, not the regular (hardfp) one.

1. Install [VitaSDK softfp](https://github.com/vitasdk-softfp) on Linux or WSL. `build.sh`
   looks for it in `~/vitasdk-softfp`; set `VITASDK` to use another place.
2. Install CMake 3.14 or later.
3. From the repository root:

   ```sh
   ./build.sh           # debug:   build/ssx_vita_debug.vpk, "SSX (2012) Debug" (SSXD00001)
   ./build.sh release   # release: build-release/ssx_vita.vpk, "SSX (2012)" (SSXV00001)
   ./build.sh both      # both, copied with their ELFs to dist/ (or $OUT_DIR)
   ```

The two builds install as separate apps and share `ux0:data/ssx/`. The debug build writes
`ux0:data/ssx/log.txt` (see [DEBUGGING.md](DEBUGGING.md)); the release build has no logging.
Keep the `.elf` files of a build you hand out: they turn crash dumps into function names.

vitaGL is built by the project's CMake from `lib/vitagl` with the flags in `CMakeLists.txt`
(`VITAGL_MAKE_FLAGS`). Its object files live in `lib/vitagl/source`, shared by both build
folders: switching between debug and release rebuilds it (`cmake/vitagl_flags.cmake`).
vitaGL's makefile doesn't track headers: after editing `shared.h` or `vitaGL.h`, run
`touch lib/vitagl/source/*.c lib/vitagl/source/utils/*.c`.

## The data tool

Python 3.10 or later:

```sh
cd datatool
python -m pip install -r requirements.txt
python -m unittest discover -s tests        # tests on synthetic data, no game needed
python ssx_datatool.py path/to/ssx-v0_0_8430.apk out/
```

The movies need ffmpeg: the `SSX_FFMPEG` environment variable, the imageio-ffmpeg package
(`python -m pip install imageio-ffmpeg`) or an `ffmpeg` on the PATH. Without it they are left out
and the game skips them.

Packaged builds (one file, no Python needed) are made with PyInstaller, as the
[workflow](../.github/workflows/datatool.yml) does on Windows, Linux and macOS:

```sh
python -m pip install pyinstaller
pyinstaller --onefile --windowed --name ssx-datatool --icon ssxdata/icon.png \
    --add-data "ssxdata/icon.png:ssxdata" ssx_datatool.py      # ";" instead of ":" on Windows
```

## Layout

| Path | What |
|---|---|
| `source/main.c` | Entry point: loads `libgame.so`, patches it, starts the game |
| `source/dynlib.c` | Import table: every libc/GL symbol the game imports, mapped to newlib, vitaGL or a wrapper |
| `source/patch.c` | Installs the hooks into `libgame.so` (offsets are for v0.0.8430) |
| `source/ssx/` | One file per fix or feature: what it fixes and why is at the top of each file |
| `source/reimpl/` | Android/Bionic pieces rewritten for the Vita: pthreads, file I/O, assets, sparse saves |
| `source/jni/` | The APK's Java classes, rewritten in C (EGL, audio, assets, video, device info) |
| `source/java.c` | FalsoJNI tables: Java method and field names to C implementations |
| `source/utils/` | Logger, GL wrappers, init |
| `datatool/` | The data tool (Python): one module per conversion, `pipeline.py` runs them in order |
| `extras/` | Files that ship in the VPK: LiveArea images, fake `/proc` files |
| `lib/` | Vendored libraries, modified for this port (see below) |

## Third-party code

| Library | Author | License | Changes for this port |
|---|---|---|---|
| [vitaGL](https://github.com/Rinnegatamante/vitaGL) (`lib/vitagl`) | Rinnegatamante | LGPL-3.0 | Shader cache in one pack file, Scaleform mesh upload paths, buffer rings, state caching, attribute offset fix, deleted buffers kept until no vertex array uses them, mip chains allocated whole, per-pool memory peaks (see [TECHNICAL.md](TECHNICAL.md)) |
| [so_util](https://github.com/Rinnegatamante/so_util) (`lib/so_util`) | Andy Nguyen | MIT | Trampoline hooks relocate PC-relative literal loads |
| [FalsoJNI](https://github.com/v-atamanenko/FalsoJNI) (`lib/falso_jni`) | Volodymyr Atamanenko | MIT | Class-qualified method lookup, minimal exceptions, per-thread receiver object, log mirroring |
| [kubridge](https://github.com/bythos14/kubridge) (`lib/kubridge`) | bythos14 | See upstream | Header and stubs only |
| fios (`lib/fios`), from [gtasa_vita](https://github.com/TheOfficialFloW/gtasa_vita) | Andy Nguyen | See upstream | |
| SHA-1 (`lib/sha1`) | Brad Conte | Public domain | |
| `lib/falso_jni/jni.h` | The Android Open Source Project | Apache-2.0 | |
| [FFmpeg](https://ffmpeg.org/), through [imageio-ffmpeg](https://github.com/imageio/imageio-ffmpeg) (packaged data tool only) | The FFmpeg developers | GPL (the build includes x264) | None: run as a separate program to re-encode the movies |

The port itself started from [soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate)
(MIT). vitaGL is linked statically; its full source, with the changes, is in `lib/vitagl`, so the
VPK can be rebuilt against a modified vitaGL as the LGPL requires.
