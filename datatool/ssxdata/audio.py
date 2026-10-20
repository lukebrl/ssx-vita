"""
Audio banks (.abk) with fewer channels.

The Vita outputs stereo, but some banks hold 4-channel sounds (the ambience
loops in ambience_ext.abk are 60 s of quad audio each, 29 MB loaded into RAM
at once). Every channel past the first two is dropped, and the environment
loops (ambience, wind), which are most of the audio a race keeps in memory,
go to mono. The samples are EA-XAS v1, which stores each channel's
128-sample frame as its own 0x4C bytes, frame by frame:

    [frame 0: ch0 ch1 ch2 ch3] [frame 1: ch0 ch1 ch2 ch3] ...

so channels can be cut out without re-encoding anything. Going to mono keeps
the left channel as it is.

File layout (little-endian ABKC container, big-endian S10A sample bank):

    ABKC header      +0x14 file size, +0x18 bank offset, +0x24 bank size,
                     absolute offsets into the data that follows the bank
    S10A bank        "S10A", 0, sound count, offset of each sound (from "S10A")
      sound          SNR header: codec/channels/rate word, flags/samples word,
                     loop start if looping, then SNS blocks (8-byte header +
                     frames); the last block has bit 0x80 in its flag byte
    trailing data    module tables and names, with absolute file offsets
"""

import fnmatch
import os
import struct

XAS_V1 = 4
FRAME = 0x4C  # bytes per channel per 128-sample frame
MONO = ("ambience*.abk", "wind_tunnel_*.abk")  # environment loops


def channels_for(name):
    return 1 if any(fnmatch.fnmatch(name, p) for p in MONO) else 2


def parse_sound(d, p):
    h1, h2 = struct.unpack_from(">II", d, p)
    codec = (h1 >> 24) & 0x0F
    channels = ((h1 >> 18) & 0x3F) + 1
    kind = h2 >> 30
    looped = (h2 >> 29) & 1
    q = p + 8 + (4 if looped else 0)
    blocks = []
    while True:
        bh, nsamples = struct.unpack_from(">II", d, q)
        size = bh & 0xFFFFFF
        blocks.append((q, bh, size, nsamples))
        q += size
        if bh & 0x80000000:
            break
        if sum(b[3] for b in blocks) >= (h2 & 0x1FFFFFFF):
            break
    return h1, codec, channels, kind, looped, blocks, q


def downmix_sound(d, p, end, keep):
    """The bytes of the sound at d[p:end], with at most keep channels if it can be done."""
    h1, codec, channels, kind, looped, blocks, q = parse_sound(d, p)
    if codec != XAS_V1 or channels <= keep or kind != 0 or q != end:
        return d[p:end]
    for _, _, size, _ in blocks:
        if (size - 8) % (FRAME * channels):
            return d[p:end]

    h1 = (h1 & ~(0x3F << 18)) | ((keep - 1) << 18)
    out = bytearray(struct.pack(">I", h1))
    out += d[p + 4:blocks[0][0]]  # flags/samples word and loop start
    for q, bh, size, nsamples in blocks:
        frames = (size - 8) // (FRAME * channels)
        data = bytearray()
        for f in range(frames):
            o = q + 8 + f * FRAME * channels
            data += d[o:o + FRAME * keep]
        out += struct.pack(">II", (bh & 0xFF000000) | (len(data) + 8), nsamples)
        out += data
    return bytes(out)


def convert(d, keep):
    """The bank with at most keep channels per sound, or None if nothing changes."""
    if d[:4] != b"ABKC":
        raise ValueError("not an ABKC file")
    hdr = list(struct.unpack_from("<16I", d, 0))
    file_size, bank, bank_size = hdr[5], hdr[6], hdr[9]
    if file_size != len(d) or d[bank:bank + 4] != b"S10A":
        raise ValueError("unexpected ABKC header")
    bank_end = bank + bank_size

    count, = struct.unpack_from(">I", d, bank + 8)
    offs = list(struct.unpack_from(">%dI" % count, d, bank + 12))
    ends = offs[1:] + [bank_size]
    if any(not (12 + 4 * count <= o < e <= bank_size) for o, e in zip(offs, ends, strict=True)):
        raise ValueError("sound table not in file order")

    new_bank = bytearray(d[bank:bank + offs[0]])
    new_offs = []
    for o, e in zip(offs, ends, strict=True):
        new_offs.append(len(new_bank))
        new_bank += downmix_sound(d, bank + o, bank + e, keep)
    struct.pack_into(">%dI" % count, new_bank, 12, *new_offs)

    delta = len(new_bank) - bank_size
    if delta == 0:
        return None

    # Absolute offsets that point past the bank move with the trailing data
    def relocate(buf):
        buf = bytearray(buf)
        for i in range(0, len(buf) - 3, 4):
            v, = struct.unpack_from("<I", buf, i)
            if bank_end <= v <= file_size:
                struct.pack_into("<I", buf, i, v + delta)
        return buf

    head = relocate(d[:bank])
    struct.pack_into("<I", head, 0x24, len(new_bank))  # bank size (not an offset)
    tail = relocate(d[bank_end:])
    out = bytes(head + new_bank + tail)  # the file size at +0x14 was relocated too
    assert struct.unpack_from("<I", out, 0x14)[0] == len(out)
    return out


def run(assets, log, progress):
    banks = []
    for root, _, files in os.walk(assets):
        banks += [os.path.join(root, f) for f in files if f.endswith(".abk")]
    banks.sort()
    saved = 0
    for i, path in enumerate(banks):
        with open(path, "rb") as f:
            d = f.read()
        try:
            out = convert(d, channels_for(os.path.basename(path)))
        except ValueError as e:
            log(f"  {os.path.basename(path)}: left as is ({e})")
            out = None
        if out is not None:
            with open(path + ".tmp", "wb") as f:
                f.write(out)
            os.replace(path + ".tmp", path)
            saved += len(d) - len(out)
            rel = os.path.relpath(path, assets).replace(os.sep, "/")
            log(f"  {rel}: {len(d) / 1048576:.1f} -> {len(out) / 1048576:.1f} MB")
        progress((i + 1) / len(banks))
    log(f"  {len(banks)} banks, {saved / 1048576:.1f} MB less")
