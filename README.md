# SSX (2012) for PS Vita

An unofficial port of EA's **SSX** (the 2012 Android version, `ssx-v0_0_8430.apk`) to the PS Vita.

The game's own ARM code runs unmodified on the Vita through a *so loader*: this project
provides everything Android would provide around it (the C library, OpenGL ES through
[vitaGL](https://github.com/Rinnegatamante/vitaGL), the Java side of the app) and patches
the game where the Vita needs it.

**No game data is included.** You need your own copy of the game's APK; a tool turns it into
the data the port needs.

## Status

- Menus, the campaign and races play from start to finish, with sound, saving and the in-game
  videos.
- Menus run at **60 FPS**. Races run at **35-60 FPS**, depending on how demanding the map is.
- Not working: the online features (EA's servers are long gone).

## What you need

- A PS Vita or PS TV with [HENkaku](https://henkaku.xyz/) / [Ensō](https://enso.henkaku.xyz/)
  (any firmware they support).
- [kubridge](https://github.com/bythos14/kubridge/releases): put `kubridge.skprx` in
  `ur0:tai/` and add it under `*KERNEL` in `ur0:tai/config.txt`, then reboot.
- `libshacccg.suprx` (the runtime shader compiler), extracted with
  [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D) into `ur0:data/`.
- **The SSX APK, version 0.0.8430**, from your own copy of the game. The data tool checks it.
- About **2 GB** free on `ux0:`.
- A PC (Windows, Linux or macOS) to run the data tool.

## Installation

1. Download the latest [release](../../releases): `ssx_vita.vpk` and the data tool for your
   PC: `ssx-datatool-windows.exe`, `ssx-datatool-linux.tar.gz` or `ssx-datatool-macos.zip` 
   (for linux and macos, it needs to be extracted)
2. Run the data tool. Pick your SSX APK, then press **Build**. It takes a few minutes and
   writes an `ssx` folder next to the tool.
3. Copy that `ssx` folder to `ux0:data/` on your Vita (for example over USB or FTP with
   [VitaShell](https://github.com/TheOfficialFloW/VitaShell)). You should then have
   `ux0:data/ssx/libgame.so`.
4. Install `ssx_vita.vpk` with VitaShell.
5. Start **SSX (2012)** from the LiveArea.

**The first start takes longer, around 4 minutes: the shader cache is built as the game runs.
Be patient, it only happens once.**

### Updating

Install the new VPK. If the release notes say the data changed, build the data again with the
new data tool and replace `ux0:data/ssx/assets` and `ux0:data/ssx/assets.pak`; your saves
(`ux0:data/ssx/files`) stay.

### Running the data tool from source

The tool is Python 3.10 or later with numpy and Pillow:

```sh
cd datatool
python -m pip install -r requirements.txt
python ssx_datatool.py                      # the window
python ssx_datatool.py path/to/ssx.apk out/ # or the command line
```

## Controls

The face buttons, D-pad, sticks, Start and Select map as on a PS3 pad. The game uses all four
shoulder buttons: on the handheld, L and R are L1 and R1, and touching the left or right half of
the rear touchpad is L2 or R2. On a PS TV, the DualShock's shoulder buttons map directly.

**Nose and tail presses are back**, as on the console versions: hold **Square** (nose) or
**Circle** (tail) while riding on the ground. They were never removed from the Android version,
which still has them in its code and data; it only had no control for them. Details in
[docs/TECHNICAL.md](docs/TECHNICAL.md#ground-presses-nose-and-tail).

**FPS counter**: hold **Select** and press **Start** to show or hide it (hidden by default).

## Troubleshooting

- **The game doesn't start**: check that `ux0:data/ssx/libgame.so` exists, and that kubridge
  and `libshacccg.suprx` are installed.
- **A crash**: the Vita writes a crash dump (`ux0:data/psp2core-*.psp2dmp`). The debug build
  (`ssx_vita_debug.vpk`, installed as "SSX (2012) Debug" next to the normal one) also writes
  `ux0:data/ssx/log.txt`. Please attach both to an issue.

## How it works

- [docs/TECHNICAL.md](docs/TECHNICAL.md): how the port is put together, what was changed for
  the Vita and why, with what each change measured.
- [docs/BUILDING.md](docs/BUILDING.md): building the VPK and the data tool.
- [docs/DEBUGGING.md](docs/DEBUGGING.md): the debug build's logs and the profiling options.

## Credits

- Port by **lukebrl**.
- **AI was used heavily** in this project for most of the reverse engineering.
- Built on the work of others:
  - [so_util](https://github.com/Rinnegatamante/so_util) and the so loader approach of
    [gtasa_vita](https://github.com/TheOfficialFloW/gtasa_vita): Andy Nguyen (TheOfficialFloW)
    and Rinnegatamante;
  - [soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate) and
    [FalsoJNI](https://github.com/v-atamanenko/FalsoJNI): Volodymyr Atamanenko;
  - [vitaGL](https://github.com/Rinnegatamante/vitaGL): Rinnegatamante;
  - [kubridge](https://github.com/bythos14/kubridge): bythos14;
  - the [VitaSDK](https://vitasdk.org/) team.

SSX is a trademark of Electronic Arts Inc. This project is not affiliated with or endorsed by
Electronic Arts. It contains no game code or data.

## License

The port's own code is under the MIT license ([LICENSE](LICENSE)). The libraries in `lib/` keep
their own licenses (see [docs/BUILDING.md](docs/BUILDING.md#third-party-code)).
