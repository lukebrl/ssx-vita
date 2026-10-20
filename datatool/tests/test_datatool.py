"""Tests on synthetic data: none of them needs the game."""

import os
import struct
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from ssxdata import audio, layout, movies, patches, textures  # noqa: E402


def write(path, data, mode="wb"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, mode, **({} if "b" in mode else {"newline": ""})) as f:
        f.write(data)


def read(path, mode="rb"):
    with open(path, mode, **({} if "b" in mode else {"newline": ""})) as f:
        return f.read()


class Layout(unittest.TestCase):
    def test_fnv1a_reference_values(self):
        # FNV-1a 32-bit reference values; source/ssx/assets.c uses the same hash
        self.assertEqual(layout.fnv1a(""), 0x811C9DC5)
        self.assertEqual(layout.fnv1a("a"), 0xE40C292C)

    def test_buckets_are_case_insensitive(self):
        self.assertEqual(layout.bucket_of("Foo.PVR"), layout.bucket_of("foo.pvr"))

    def test_split_folders(self):
        with tempfile.TemporaryDirectory() as d:
            big = os.path.join(d, "big")
            for i in range(layout.BUCKET_THRESHOLD + 1):
                write(os.path.join(big, f"f{i}.bin"), b"x")
            write(os.path.join(d, "small", "a.bin"), b"y")
            layout.split_folders(d, lambda t: None, lambda f: None)
            self.assertEqual(read(os.path.join(d, layout.BUCKET_LIST), "r"), "big/\n")
            self.assertTrue(os.path.exists(os.path.join(big, layout.bucket_of("f0.bin"), "f0.bin")))
            self.assertTrue(os.path.exists(os.path.join(d, "small", "a.bin")))

    def test_pack_round_trip(self):
        with tempfile.TemporaryDirectory() as d:
            files = {"Dir/A.txt": b"hello", "dir/b.bin": bytes(range(256)) * 3}
            for rel, data in files.items():
                write(os.path.join(d, "assets", rel), data)
            write(os.path.join(d, "assets", "huge.bin"), b"\0" * (layout.PACK_MAX_SIZE + 1))
            pak = os.path.join(d, "assets.pak")
            layout.build_pack(os.path.join(d, "assets"), pak, lambda t: None, lambda f: None)
            blob = read(pak)
            self.assertEqual(blob[:8], b"SSXPAK01")
            count, index_off, max_size, _ = struct.unpack_from("<IIII", blob, 8)
            self.assertEqual((count, max_size), (2, layout.PACK_MAX_SIZE))
            entries = {(h1, h2): (off, size) for h1, h2, off, size in
                       (struct.unpack_from("<IIII", blob, index_off + 16 * i) for i in range(count))}
            for rel, data in files.items():
                key = (layout.fnv1a(rel.lower()), layout.djb2_xor(rel.lower()))
                off, size = entries[key]
                self.assertEqual(off % layout.PACK_ALIGN, 0)
                self.assertEqual(blob[off:off + size], data)


class Textures(unittest.TestCase):
    def gradient(self, w, h):
        y, x = np.mgrid[0:h, 0:w]
        return np.dstack([x * 255 // max(w - 1, 1), y * 255 // max(h - 1, 1), (x + y) % 256]).astype(np.uint8)

    def test_etc1_round_trip(self):
        rgb = self.gradient(64, 32)
        back = textures.etc1_decode(textures.etc1_encode(rgb), 64, 32)
        self.assertLess(np.abs(back.astype(int) - rgb.astype(int)).mean(), 8)

    def test_etc1_smooth_gradient(self):
        # A sky-like gradient stays close (40.4 dB). Synthetic gradients don't show
        # the fast encoder's squares (the game's sky did: 38.5 against 45.8 dB,
        # docs/TECHNICAL.md); this guards against a broken encoder.
        y, x = np.mgrid[0:64, 0:64]
        rgb = np.dstack([90 + y // 4, 140 + y // 3, 200 + x // 8]).astype(np.uint8)
        back = textures.etc1_decode(textures.etc1_encode(rgb), 64, 64)
        mse = np.mean((back.astype(float) - rgb.astype(float)) ** 2)
        self.assertGreater(10 * np.log10(255 ** 2 / mse), 39)

    def pvr_v3(self, fmt, w, h, data):
        return struct.pack("<IIQIIIIIIIII", 0x03525650, 0, fmt, 0, 0, h, w, 1, 1, 1, 1, 0) + data

    def test_shrink_rgba4444(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.pvr")
            write(path, self.pvr_v3(textures.RGBA4444, 64, 32, b"\xff\x0f" * 64 * 32))
            self.assertIsNone(textures.shrink(path, 32, 16))
            tex = textures.read_pvr(path)
            self.assertEqual((tex["w"], tex["h"], len(tex["data"])), (32, 16, 32 * 16 * 2))
            # Opaque stays opaque: alpha isn't mixed into the colours
            self.assertTrue(np.all(np.frombuffer(tex["data"], "<u2") & 15 == 15))
            self.assertEqual(textures.shrink(path, 32, 16), "already small enough")

    def test_shrink_etc1_cube(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "cube.pvr")
            face = textures.etc1_encode(self.gradient(32, 32))
            write(path, struct.pack("<IIIIIIIIIIII", 0x03525650, 0, textures.ETC1, 0, 0, 0, 32, 32, 1, 1, 6, 1)
                  + struct.pack("<I", 0) + face * 6)
            self.assertIsNone(textures.shrink(path, 16, 16))
            tex = textures.read_pvr(path)
            self.assertEqual((tex["w"], tex["faces"], len(tex["data"])), (16, 6, 6 * 16 * 16 // 2))


class Patches(unittest.TestCase):
    def test_memory_cfg_keeps_lines_and_other_platforms(self):
        lines = [f"  AddAllocator\t{name}\tPPMallocMutex\t[ size=999M, pagesize=64K ]  \r\n"
                 for name in patches.HEAP_SIZES]
        lines.append("  AddAllocator.ios\tRENDER_GEN\tPPMallocMutex\t[ size=1M ]\r\n")
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "memory.cfg")
            write(path, "".join(lines), "w")
            patches.memory_cfg(path)
            out = read(path, "r").split("\r\n")
            self.assertEqual(len(out), len(lines) + 1)  # no line merged or lost
            self.assertIn("size=52M", out[list(patches.HEAP_SIZES).index("RENDER_GEN")])
            self.assertIn("size=1M", out[-2])  # iOS line left alone

    def test_scrape_and_nav_are_idempotent(self):
        with tempfile.TemporaryDirectory() as d:
            scrape = os.path.join(d, "main.scrape")
            write(scrape, "var texture FrameBufferTexture 1280 720\n"
                          "var surface FrameBufferSurfaceDepth edramdepth 1280 720\n", "w")
            nav = os.path.join(d, "nav.lua")
            write(nav, "function P:collect()\n\tcollectgarbage(\"collect\")\nend\n\nreturn P\n", "w")
            for _ in range(2):
                patches.main_scrape(scrape)
                patches.nav_lua(nav)
            self.assertEqual(read(scrape, "r").count("960 544"), 2)
            self.assertNotIn("collectgarbage", read(nav, "r"))
            self.assertIn("return P", read(nav, "r"))


class Audio(unittest.TestCase):
    def test_channels(self):
        self.assertEqual(audio.channels_for("ambience_ext.abk"), 1)
        self.assertEqual(audio.channels_for("wind_tunnel_alps.abk"), 1)
        self.assertEqual(audio.channels_for("boost_awesome.abk"), 2)

    def test_not_a_bank(self):
        with self.assertRaises(ValueError):
            audio.convert(b"RIFF" + b"\0" * 64, 2)


class Movies(unittest.TestCase):
    def test_command(self):
        cmd = movies.command("ffmpeg", "in.mp4", "out.mp4")
        self.assertEqual((cmd[0], cmd[-1]), ("ffmpeg", "out.mp4"))
        for flag in ("libx264", "scale=960:544:flags=lanczos", "48000", "+faststart"):
            self.assertIn(flag, cmd)

    def test_without_ffmpeg_the_movies_go(self):
        with tempfile.TemporaryDirectory() as d:
            write(os.path.join(d, movies.MOVIE_DIR, "intro.mp4"), b"x")
            old = movies.find_ffmpeg
            movies.find_ffmpeg = lambda: None
            try:
                movies.run(d, lambda t: None, lambda f: None)
            finally:
                movies.find_ffmpeg = old
            self.assertEqual(os.listdir(os.path.join(d, movies.MOVIE_DIR)), [])


if __name__ == "__main__":
    unittest.main()
