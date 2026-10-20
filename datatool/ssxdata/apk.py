"""
The SSX APK: checking it is the version the port is made for, and taking
what the port needs out of it.

The port loads the game's own ARM code (lib/armeabi/libgame.so and the C++
runtime it links to) and reads the game's assets/ folder. Every patch and
offset in the port is for one build of libgame.so, so the APK is identified
by that library's SHA-256, not by the whole APK's (re-signed or re-zipped
APKs with the same game are fine).
"""

import hashlib
import os
import shutil
import zipfile

GAME_VERSION = "0.0.8430"
LIBGAME = "lib/armeabi/libgame.so"
LIBGAME_SHA256 = "c8ee5dce525875c8466670ead20a9821aa8fab4270c5767869c2aba511693ac2"
LIBRARIES = (LIBGAME, "lib/armeabi/libgnustl_shared.so")
ASSETS = "assets/"


class ApkError(Exception):
    pass


def open_apk(path):
    """The APK as a ZipFile, after checking it holds the expected libgame.so."""
    try:
        z = zipfile.ZipFile(path)
    except (OSError, zipfile.BadZipFile) as e:
        raise ApkError(f"{path} is not a readable APK ({e})") from e
    names = set(z.namelist())
    missing = [n for n in LIBRARIES if n not in names]
    if missing:
        raise ApkError(f"{os.path.basename(path)} has no {missing[0]}: is it the SSX APK?")
    h = hashlib.sha256()
    with z.open(LIBGAME) as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    if h.hexdigest() != LIBGAME_SHA256:
        raise ApkError(f"this APK's libgame.so is not the one of SSX {GAME_VERSION} for Android, "
                       f"the only version the port supports")
    return z


def extract(z, out, log, progress):
    """libgame.so and libgnustl_shared.so into out/, assets/ into out/assets/."""
    for name in LIBRARIES:
        with z.open(name) as src, open(os.path.join(out, os.path.basename(name)), "wb") as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
    entries = [i for i in z.infolist() if i.filename.startswith(ASSETS) and not i.is_dir()]
    total = sum(i.file_size for i in entries) or 1
    done = 0
    for i, info in enumerate(entries):
        rel = info.filename[len(ASSETS):]
        dst = os.path.join(out, "assets", *rel.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with z.open(info) as src, open(dst, "wb") as f:
            shutil.copyfileobj(src, f, 1 << 20)
        done += info.file_size
        if i % 50 == 0:
            progress(done / total)
    log(f"  {len(entries)} files, {total / 1048576:.0f} MB")
    progress(1.0)
