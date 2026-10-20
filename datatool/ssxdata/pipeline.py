"""
Builds the port's data folder from the APK, step by step.

    <output>/ssx/                  copy to ux0:data/ on the Vita
        libgame.so                 the game's code
        libgnustl_shared.so        the C++ runtime it uses
        assets/                    the game's assets, converted in place
        assets.pak                 the small asset files, packed

Every step reports through log(text) and progress(fraction of the whole job,
0..1). Steps run in this order because the conversions use the APK's paths
and the pack is built from the final layout.
"""

import os
import shutil
import time

from . import apk, audio, layout, movies, patches, textures

DATA_DIR = "ssx"


def _assets(data):
    return os.path.join(data, "assets")


def _extract(z, data, log, progress):
    apk.extract(z, data, log, progress)


def _audio(z, data, log, progress):
    audio.run(_assets(data), log, progress)


def _patches(z, data, log, progress):
    patches.run(_assets(data), log, progress)


def _textures(z, data, log, progress):
    textures.run(_assets(data), log, progress)


def _movies(z, data, log, progress):
    movies.run(_assets(data), log, progress)


def _buckets(z, data, log, progress):
    layout.split_folders(_assets(data), log, progress)


def _pack(z, data, log, progress):
    layout.build_pack(_assets(data), os.path.join(data, "assets.pak"), log, progress)


# (title, rough share of the total time, step)
STEPS = (
    ("Extracting the game from the APK", 0.17, _extract),
    ("Audio banks: stereo, environment loops in mono", 0.05, _audio),
    ("Game settings: memory sizes, render size, UI garbage collection", 0.005, _patches),
    ("Big textures at the Vita's screen size", 0.03, _textures),
    ("Movies in H.264 for the Vita's decoder (the longest step)", 0.68, _movies),
    ("Large folders split into subfolders", 0.015, _buckets),
    ("Packing the small files", 0.05, _pack),
)


def data_dir(output):
    return os.path.join(output, DATA_DIR)


def build(apk_path, output, log=print, progress=lambda f: None, overwrite=False):
    """Builds <output>/ssx from the APK. Raises apk.ApkError or RuntimeError on failure."""
    log(f"Checking {os.path.basename(apk_path)}...")
    z = apk.open_apk(apk_path)
    data = data_dir(output)
    if os.path.exists(data):
        if not overwrite:
            raise RuntimeError(f"{data} already exists")
        log(f"Removing the old {data}...")
        shutil.rmtree(data)
    os.makedirs(data)

    start = time.monotonic()
    done = 0.0
    with z:
        for title, share, fn in STEPS:
            log(title + "...")
            fn(z, data, log, lambda f, base=done, share=share: progress(base + share * min(max(f, 0.0), 1.0)))
            done += share
            progress(done)
    log(f"Done in {(time.monotonic() - start) / 60:.0f} min: {data}")
    log("Copy the ssx folder to ux0:data/ on your Vita (so that ux0:data/ssx/libgame.so exists).")
    return data
