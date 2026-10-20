"""
How the assets are laid out on the memory card, for fast file opens.

The memory card's file system is slow to open files: an open costs
10-100 ms whatever the size, and it looks a name up by reading its folder's
entries one by one.

- Buckets: folders with more than BUCKET_THRESHOLD files get their files
  moved into BUCKETS subfolders "_00".."_3f", picked by a hash of the
  lowercase file name. In worlds/geometry (4554 files) each open took
  ~420 ms, most of a race's two-minute load. The split folders are listed in
  assets/_buckets.txt; the port adds the subfolder when it opens a file
  (ssx_asset_path in source/ssx/assets.c, same hash).
- Pack: every file of up to PACK_MAX_SIZE goes into assets.pak as well, so
  the game reads those with one file kept open (UI screens open dozens of
  small files). The port reads a file from the pack when its path is there
  and its size matches (source/reimpl/asset_manager.cpp); the loose files
  stay for everything else.

assets.pak format (little-endian):
    header  "SSXPAK01", u32 count, u32 index offset, u32 max size, u32 0
    data    the files, each at a multiple of PACK_ALIGN
    index   count entries {u32 hash1, u32 hash2, u32 offset, u32 size},
            sorted by (hash1, hash2); hash1 is FNV-1a and hash2 djb2-xor of
            the lowercase path relative to assets/ (bucket folders included)
"""

import os
import struct

BUCKET_THRESHOLD = 300
BUCKETS = 64
BUCKET_LIST = "_buckets.txt"
PACK_MAX_SIZE = 256 * 1024
PACK_ALIGN = 512


def fnv1a(text):
    h = 0x811C9DC5
    for c in text.encode("utf-8"):
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


def djb2_xor(text):
    h = 0x1505
    for c in text.encode("utf-8"):
        h = ((h * 33) ^ c) & 0xFFFFFFFF
    return h


def bucket_of(name):
    return "_%02x" % (fnv1a(name.lower()) % BUCKETS)


def split_folders(assets, log, progress):
    split = []
    for d, dirs, files in os.walk(assets):
        if len(files) <= BUCKET_THRESHOLD:
            continue
        for name in files:
            sub = os.path.join(d, bucket_of(name))
            os.makedirs(sub, exist_ok=True)
            os.replace(os.path.join(d, name), os.path.join(sub, name))
        rel = os.path.relpath(d, assets).replace(os.sep, "/") + "/"
        split.append(rel)
        log(f"  {rel}: {len(files)} files into {BUCKETS} subfolders")
        dirs[:] = [x for x in dirs if not (len(x) == 3 and x.startswith("_"))]  # the new subfolders
    with open(os.path.join(assets, BUCKET_LIST), "w", newline="\n") as f:
        for rel in sorted(split):
            f.write(rel + "\n")
    progress(1.0)


def build_pack(assets, out, log, progress):
    files = []
    for dirpath, _, names in os.walk(assets):
        for n in names:
            full = os.path.join(dirpath, n)
            size = os.path.getsize(full)
            if size <= PACK_MAX_SIZE:
                rel = os.path.relpath(full, assets).replace(os.sep, "/").lower()
                files.append((rel, full, size))
    files.sort()  # a folder's files next to each other

    index = []
    seen = set()
    with open(out + ".tmp", "wb") as f:
        f.write(b"\0" * 24)
        for i, (rel, full, _size) in enumerate(files):
            key = (fnv1a(rel), djb2_xor(rel))
            if key in seen:
                raise RuntimeError(f"assets.pak: hash collision on {rel}")
            seen.add(key)
            pos = f.tell()
            if pos % PACK_ALIGN:
                f.write(b"\0" * (PACK_ALIGN - pos % PACK_ALIGN))
                pos = f.tell()
            with open(full, "rb") as src:
                data = src.read()
            f.write(data)
            index.append((key[0], key[1], pos, len(data)))
            if i % 200 == 0:
                progress(i / len(files))
        index.sort()
        index_off = f.tell()
        for e in index:
            f.write(struct.pack("<IIII", *e))
        f.seek(0)
        f.write(b"SSXPAK01" + struct.pack("<IIII", len(index), index_off, PACK_MAX_SIZE, 0))
    os.replace(out + ".tmp", out)
    log(f"  {len(index)} files, {sum(e[3] for e in index) / 1048576:.0f} MB")
    progress(1.0)
