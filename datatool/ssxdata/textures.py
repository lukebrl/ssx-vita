"""
Big textures written at a size the Vita's 960x544 screen can show.

The game was made for 1280x720 screens. Two kinds of textures are shrunk,
both in their own format, in place in the extracted assets:

- 3D textures (globe, front-end backdrops, sky cubes), listed in TEXTURES.
  They are sampled by UV coordinates, so their size on screen doesn't
  depend on their pixel size. In a race they were ~20 MB of the 40 MB of
  textures in video memory.
- The race loading screens (LOADING_SCREENS). They are Scaleform UI images,
  which are drawn at their pixel size, but the loading screen shows them
  through the ImageLoader component, which sizes the loaded clip to its
  placeholder (ImageLoader.AdjustImageSize) whatever the image's size. They
  are uncompressed RGBA4444 at 1280x720 despite their .etc1.pvr name: 1.8 MB
  each in the UI memory pool, in one piece, every time one is shown.

Other Scaleform images are left alone: they would show smaller.

Formats handled: PVR v3 RGBA4444 / RGB565 (resampled, ordered dithering
back to 4-6 bits per channel), PVR v3 and legacy v2 ETC1, cube maps
included (each face decoded, resampled and encoded again here, see
etc1_encode: a fast encoder left visible squares in the skies).
Channels are resampled one by one: alpha is often a mask (the globe keeps
one over its oceans, with colours under it that are used), and an RGBA
resize would weigh the colours by it.
"""

import os
import struct

import numpy as np
from PIL import Image

FE = "mobile/gameexplorer/frontend/textures/"
SKY = "mobile/gameexplorer/worlds/textures/"
LOADING_SCREENS = "common/fe/assets/loading_screens/"
LOADING_WIDTH = 960

# 3D textures: path under assets/ -> new (width, height)
TEXTURES = {
    # The front-end globe: 2048x1024, 4 MB each, kept loaded during races
    FE + "fe_globe_diff.etc1.pvr": (1024, 512),
    FE + "fe_globe_disp.etc1.pvr": (1024, 512),
    # Front-end 3D scene backdrops at 1280x720
    FE + "fe_background.etc1.pvr": (960, 540),
    FE + "fe_moviescreen_bg.etc1.pvr": (960, 540),
    FE + "fe_mac.etc1.pvr": (960, 540),
    FE + "fe_zoe.etc1.pvr": (960, 540),
    FE + "fe_griff.etc1.pvr": (960, 540),
    FE + "fe_elise.etc1.pvr": (960, 540),
    # Sky cubes: 6 faces of 1024x1024 ETC1, 3 MB each (cube faces must be
    # powers of two: vitaGL pads others up, so 512 is the next size down)
    SKY + "sky_mteverest_cube_large.etc1.pvr": (512, 512),
    SKY + "sky_mteverest_cube_large_sky.etc1.pvr": (512, 512),
}
SKYBOX_PREFIX = "skybox_"  # every SKY + skybox_*.pvr: (512, 512)
NO_DITHER = {FE + "fe_globe_disp.etc1.pvr"}  # a displacement map: values, not colours

RGBA4444 = 0x0404040461626772  # channel names 'r','g','b','a', then bits 4,4,4,4
RGB565 = 0x0005060500626772
ETC1 = 6
V2_ETC1 = 0x36
V2_CUBE = 0x1000

BAYER = (np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], dtype=np.float32) + 0.5) / 16 - 0.5

# ETC1 modifier tables, by codeword: values for pixel indices 0..3
ETC_MOD = np.array([[2, 8, -2, -8], [5, 17, -5, -17], [9, 29, -9, -29], [13, 42, -13, -42],
                    [18, 60, -18, -60], [24, 80, -24, -80], [33, 106, -33, -106], [47, 183, -47, -183]],
                   dtype=np.int32)


def read_pvr(path):
    """The texture as a dict (v, fmt, w, h, faces, header, data), or None if its layout isn't handled."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] == b"PVR\x03":
        fmt, = struct.unpack_from("<Q", d, 8)
        h, w, depth, surfaces, faces, mips, meta = struct.unpack_from("<7I", d, 24)
        if depth != 1 or surfaces != 1 or mips != 1:
            return None
        return dict(v=3, fmt=fmt, w=w, h=h, faces=faces, header=d[:52 + meta], data=d[52 + meta:])
    hs, h, w, mips, flags = struct.unpack_from("<5I", d, 0)
    if hs == 52 and d[44:48] == b"PVR!" and (flags & 0xFF) == V2_ETC1 and mips == 0:
        return dict(v=2, fmt=ETC1, w=w, h=h, faces=6 if flags & V2_CUBE else 1, header=d[:52], data=d[52:])
    return None


def write_pvr(path, tex, w, h, faces):
    header = bytearray(tex["header"])
    if tex["v"] == 3:
        struct.pack_into("<II", header, 24, h, w)
    else:
        struct.pack_into("<II", header, 4, h, w)
        struct.pack_into("<I", header, 20, sum(len(f) for f in faces))
    tmp = path + ".tmp"
    with open(tmp, "wb") as out:
        out.write(header)
        for f in faces:
            out.write(f)
    os.replace(tmp, path)


def unpack16(raw, w, h, fmt):
    """float32 (h, w, 4) in 0..255 from 16-bit pixels."""
    px = np.frombuffer(raw, dtype="<u2", count=w * h).reshape(h, w).astype(np.uint32)
    if fmt == RGBA4444:
        return np.stack([(px >> 12) & 15, (px >> 8) & 15, (px >> 4) & 15, px & 15], -1).astype(np.float32) * 17
    return np.stack([((px >> 11) & 31) * 255 / 31, ((px >> 5) & 63) * 255 / 63, (px & 31) * 255 / 31,
                     np.full((h, w), 255.0)], -1).astype(np.float32)


def pack16(img, fmt, dither):
    h, w = img.shape[:2]
    noise = BAYER[np.arange(h)[:, None] % 4, np.arange(w)[None, :] % 4][..., None] if dither else 0
    if fmt == RGBA4444:
        q = np.clip(np.rint(img / 17 + noise), 0, 15).astype(np.uint32)
        px = (q[..., 0] << 12) | (q[..., 1] << 8) | (q[..., 2] << 4) | q[..., 3]
    else:
        scale = np.array([31, 63, 31], dtype=np.float32) / 255
        q = np.clip(np.rint(img[..., :3] * scale + noise), 0, [31, 63, 31]).astype(np.uint32)
        px = (q[..., 0] << 11) | (q[..., 1] << 5) | q[..., 2]
    return px.astype("<u2").tobytes()


def etc1_decode(raw, w, h):
    """uint8 RGB (h, w, 3) from ETC1 blocks (8 bytes each, big-endian, in raster order)."""
    bw, bh = w // 4, h // 4
    b = np.frombuffer(raw, dtype=">u8", count=bw * bh).astype(np.uint64)
    hi = (b >> np.uint64(32)).astype(np.int64)
    lo = (b & np.uint64(0xFFFFFFFF)).astype(np.int64)
    diff = (hi >> 1) & 1
    flip = hi & 1
    cw1 = (hi >> 5) & 7
    cw2 = (hi >> 2) & 7
    base1 = np.zeros((len(b), 3), np.int64)
    base2 = np.zeros((len(b), 3), np.int64)
    for c, shift in enumerate((24, 16, 8)):
        # Differential mode: a 5-bit colour and a 3-bit signed delta
        v5 = (hi >> (shift + 3)) & 31
        d3 = (hi >> shift) & 7
        d3 = np.where(d3 >= 4, d3 - 8, d3)
        v52 = (v5 + d3) & 31
        # Individual mode: two 4-bit colours
        i1 = (hi >> (shift + 4)) & 15
        i2 = (hi >> shift) & 15
        base1[:, c] = np.where(diff == 1, (v5 << 3) | (v5 >> 2), i1 * 17)
        base2[:, c] = np.where(diff == 1, (v52 << 3) | (v52 >> 2), i2 * 17)
    out = np.zeros((len(b), 4, 4, 3), np.int64)  # block, y, x, channel
    for x in range(4):
        for y in range(4):
            i = x * 4 + y  # pixels are numbered column by column
            idx = (((lo >> (16 + i)) & 1) << 1) | ((lo >> i) & 1)
            second = np.where(flip == 1, y >= 2, x >= 2)
            cw = np.where(second, cw2, cw1)
            base = np.where(second[:, None], base2, base1)
            out[:, y, x, :] = base + ETC_MOD[cw, idx][:, None]
    out = np.clip(out, 0, 255).astype(np.uint8)
    return out.reshape(bh, bw, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(h, w, 3)


def _quantize(v, levels):
    """8-bit values to 0..levels, rounded as Android's reference ETC1 encoder does."""
    t = v * levels + 128
    return (t + (t >> 8)) >> 8


def _encode_blocks(px):
    """(hi, lo) 32-bit words of the ETC1 blocks for px, int (N, 4, 4, 3) as (block, y, x, channel)."""
    n = len(px)
    best = np.full(n, np.iinfo(np.int64).max)
    hi_out = np.zeros(n, np.int64)
    lo_out = np.zeros(n, np.int64)
    for flip in (0, 1):
        # The block's two halves: left/right (flip 0) or top/bottom (flip 1)
        halves = [[(y, x) for x in range(4) for y in range(4) if (y >= 2 if flip else x >= 2) == s] for s in (0, 1)]
        pix = [px[:, [y for y, _ in h], [x for _, x in h], :] for h in halves]  # 2 x (N, 8, 3)
        avg = [(p.sum(axis=1) + 4) >> 3 for p in pix]

        # Differential mode (5-bit colour + 3-bit delta) when the halves are close, else 4-bit colours
        q5 = [_quantize(a, 31) for a in avg]
        delta = q5[1] - q5[0]
        diff = np.all((delta >= -4) & (delta <= 3), axis=1)
        q4 = [_quantize(a, 15) for a in avg]
        bases = [np.where(diff[:, None], (q5[s] << 3) | (q5[s] >> 2), q4[s] * 17) for s in (0, 1)]

        score = np.zeros(n, np.int64)
        tables, indices = [], []
        for s in (0, 1):
            # Every table's 4 colours for this half, every pixel's error with each
            cand = np.clip(bases[s][:, None, None, :] + ETC_MOD[None, :, :, None], 0, 255)  # (N, 8, 4, 3)
            err = ((pix[s][:, :, None, None, :] - cand[:, None, :, :, :]) ** 2).sum(axis=-1)  # (N, 8 px, 8, 4)
            idx = err.argmin(axis=-1)  # (N, 8 px, 8 tables)
            per_table = err.min(axis=-1).sum(axis=1)  # (N, 8 tables)
            t = per_table.argmin(axis=1)
            score += per_table[np.arange(n), t]
            tables.append(t)
            indices.append(idx[np.arange(n), :, t])  # (N, 8 px)

        hi = np.where(diff,
                      (q5[0][:, 0] << 27) | ((delta[:, 0] & 7) << 24) | (q5[0][:, 1] << 19) |
                      ((delta[:, 1] & 7) << 16) | (q5[0][:, 2] << 11) | ((delta[:, 2] & 7) << 8),
                      (q4[0][:, 0] << 28) | (q4[1][:, 0] << 24) | (q4[0][:, 1] << 20) |
                      (q4[1][:, 1] << 16) | (q4[0][:, 2] << 12) | (q4[1][:, 2] << 8))
        hi = hi | (tables[0] << 5) | (tables[1] << 2) | (diff.astype(np.int64) << 1) | flip
        lo = np.zeros(n, np.int64)
        for s in (0, 1):
            for k, (y, x) in enumerate(halves[s]):
                i = x * 4 + y  # pixels are numbered column by column
                v = indices[s][:, k]
                lo |= ((v >> 1) << (16 + i)) | ((v & 1) << i)

        better = score < best
        best = np.where(better, score, best)
        hi_out = np.where(better, hi, hi_out)
        lo_out = np.where(better, lo, lo_out)
    return hi_out, lo_out


def etc1_encode(rgb, chunk=4096):
    """ETC1 blocks (8 bytes each, big-endian, in raster order) for uint8 RGB (h, w, 3).

    The method of Android's reference encoder (etc1.cpp): for each block, both
    ways of splitting it in two halves, each half's average colour as its base,
    and for each half the modifier table and per-pixel modifiers with the least
    squared error. Unlike fast encoders, it doesn't leave visible squares in smooth
    gradients such as skies.
    """
    h, w = rgb.shape[:2]
    px = rgb.astype(np.int64).reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 4, 4, 3)
    words = np.zeros(len(px), np.uint64)
    for i in range(0, len(px), chunk):  # bounded memory: the error array is (chunk, 8, 8, 4)
        hi, lo = _encode_blocks(px[i:i + chunk])
        words[i:i + chunk] = (hi.astype(np.uint64) << np.uint64(32)) | lo.astype(np.uint64)
    return words.astype(">u8").tobytes()


def resample(img, w, h):
    """img (h, w, c) in 0..255, resized to (h', w', c), channel by channel."""
    chans = [np.asarray(Image.fromarray(np.clip(np.rint(img[..., c]), 0, 255).astype(np.uint8), "L").resize(
        (w, h), Image.Resampling.LANCZOS)) for c in range(img.shape[-1])]
    return np.stack(chans, -1).astype(np.float32)


def shrink(path, w, h, dither=True):
    """Rewrites the texture at path at w x h. Returns None, or why it was left as it is."""
    tex = read_pvr(path)
    if not tex:
        return "unexpected layout"
    if tex["w"] <= w:
        return "already small enough"
    faces = []
    if tex["fmt"] in (RGBA4444, RGB565):
        n = tex["w"] * tex["h"] * 2
        for f in range(tex["faces"]):
            img = unpack16(tex["data"][f * n:(f + 1) * n], tex["w"], tex["h"], tex["fmt"])
            faces.append(pack16(resample(img, w, h), tex["fmt"], dither))
    elif tex["fmt"] == ETC1:
        if w % 4 or h % 4:
            return "size not a multiple of 4"
        n = tex["w"] * tex["h"] // 2
        for f in range(tex["faces"]):
            rgb = etc1_decode(tex["data"][f * n:(f + 1) * n], tex["w"], tex["h"])
            faces.append(etc1_encode(resample(rgb.astype(np.float32), w, h).astype(np.uint8)))
    else:
        return "format 0x%x not handled" % tex["fmt"]
    write_pvr(path, tex, w, h, faces)
    return None


def jobs(assets):
    """(path, width, height, dither) for every texture to shrink under assets/."""
    out = [(os.path.join(assets, rel), w, h, rel not in NO_DITHER) for rel, (w, h) in TEXTURES.items()]
    sky = os.path.join(assets, SKY)
    if os.path.isdir(sky):
        out += [(os.path.join(sky, f), 512, 512, True) for f in sorted(os.listdir(sky))
                if f.startswith(SKYBOX_PREFIX) and f.endswith(".pvr")]
    screens = os.path.join(assets, LOADING_SCREENS)
    if os.path.isdir(screens):
        for f in sorted(os.listdir(screens)):
            if not f.endswith(".pvr"):
                continue
            tex = read_pvr(os.path.join(screens, f))
            if tex and tex["fmt"] == RGBA4444 and tex["w"] > LOADING_WIDTH:
                out.append((os.path.join(screens, f), LOADING_WIDTH, round(tex["h"] * LOADING_WIDTH / tex["w"]), True))
    return out


def run(assets, log, progress):
    todo = jobs(assets)
    saved = 0
    for i, (path, w, h, dither) in enumerate(todo):
        rel = os.path.relpath(path, assets).replace(os.sep, "/")
        if not os.path.exists(path):
            log(f"  {rel}: missing, skipped")
            continue
        before = os.path.getsize(path)
        why = shrink(path, w, h, dither)
        if why:
            log(f"  {rel}: {why}, left as is")
        else:
            saved += before - os.path.getsize(path)
        progress((i + 1) / len(todo))
    log(f"  {len(todo)} textures, {saved / 1048576:.1f} MB less")
