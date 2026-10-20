"""
Small edits to the game's text files, in place in the extracted assets.

- common/config/memory.cfg: the size of each EA MemoryFramework heap.
  Android's file asks for ~573 MB, which only works because Android backs
  pages lazily; the Vita backs every page, so heaps are sized from the peak
  usage measured on the Vita (~260 MB in all).
- mobile/scrape/main.scrape: the render script draws the 3D scene into a
  hard-coded 1280x720 target and depth buffer, then scales it down to the
  screen: 76% more pixels than are shown. Rendered at 960x544 instead.
- common/fe/lua/packages/nav.lua: the UI navigation (a Lua state chart) runs
  a full Lua garbage collection after every navigation event, 15-60 ms each
  on the Vita, several per screen change. Lua's incremental collector, paced
  by allocation, keeps up on its own.
"""

import os
import re

MEMORY_CFG = "common/config/memory.cfg"
SCRAPE = "mobile/scrape/main.scrape"
NAV = "common/fe/lua/packages/nav.lua"

# Heap -> size on the Vita. Comments: Android's size, and why this one.
# The heaps are carved out of the newlib heap, which also serves every plain
# malloc: with STL_GEN, FE_GEN, FE_SFGFX_ASCRIPT, FE_UX_GEN and GLOBAL_ASSETTMP
# 12 MB bigger it had 2.6-4.5 MB free in a long session, and file reads of
# 175-250 KB failed during race loads.
HEAP_SIZES = {
    "AUDIODATA_GEN": "36M",        # 60M; with ambience_ext.abk cut to stereo (15 MB)
    "GAMEPLAY_GEN": "20M",         # 20M; a race needs 6.5 MB in one block (rewind)
    "GAMEWORLD_GEN": "4M",         # 20M; only ~10 KB used in menus, races and the tutorial
    "ONLINE_GEN": "2M",            # 2M; 512K ran out (ghost replays)
    "GLOBAL_GEN": "10M",           # 15M
    "STL_GEN": "2M",               # 10M; ~0.1M used at the main menu, at most 0.6M in a long session
    "RENDER_SBA": "5M",            # 5M
    "FE_SBA": "4M",                # 4M
    "FE_GEN": "4M",                # 12M; at most 1.7M in use in a long session
    "FE_SFGFX_GEN_A": "35M",       # 35M; 16M ran out at the main menu, 24M after the tutorial, 32M on a 2nd race
    "FE_SFGFX_GEN_B": "2M",        # 5M; its category has a 0M budget
    "FE_SFGFX_AS_SBA": "3M",       # 4M
    "FE_SFGFX_ASCRIPT": "2M",      # 12M; at most 0.1M in use in a long session
    "FE_SFGFX_REN_SBA": "3M",      # 10M
    "FE_SFGFX_RENDER": "3M",       # 10M
    "FE_UX_GEN": "2M",             # 5M; at most 0.3M in use in a long session
    "GLOBAL_ASSETTMP": "6M",       # 35M; at most ~3 MB in use when sampled
    "GAMEWORLD_SLOTALLOC": "25M",  # 25M; a race takes one 24.6 MB block
    "ASSETSTREAM_READBUF": "5M",   # 5M
    "AUDIO_RWAC": "10M",           # 10M; 6M suspected too small
    "MEMORY_RESIDENT_GEN": "1M",   # 5M; never used through boot, menus and race load
    "RENDER_GEN": "52M",           # 250M; 48M ran out on the first level after the tutorial
}

ALLOCATOR_LINE = re.compile(r"^(\s*AddAllocator(?:\.(\w+))?\s+(\w+)\s+\w+\s+\[)(.*)$")


def _read(path):
    with open(path, newline="", encoding="latin-1") as f:
        return f.read()


def _write(path, text):
    with open(path + ".tmp", "w", newline="", encoding="latin-1") as f:
        f.write(text)
    os.replace(path + ".tmp", path)


def _size_span(params):
    """Span of the value after "size=", which may hold {{cond}?a:b} with nesting."""
    m = re.search(r"size\s*=\s*", params)
    if not m:
        return None
    i = j = m.end()
    depth = 0
    while j < len(params):
        if params.startswith("{{", j) or params[j] == "{":
            depth += 1
        elif params[j] == "}":
            depth -= 1
        elif depth <= 0 and params[j] in ",]":
            break
        j += 1
    return i, j


def memory_cfg(path):
    """Only the size= of heaps that apply to Android (no platform suffix, or .android) changes."""
    lines = _read(path).splitlines(keepends=True)
    changed = set()
    for k, line in enumerate(lines):
        m = ALLOCATOR_LINE.match(line)
        if not (m and m.group(2) in (None, "android") and m.group(3) in HEAP_SIZES):
            continue
        head, params = m.group(1), m.group(4)  # params stops before the line's end
        span = _size_span(params)
        if span:
            i, j = span
            ending = line[len(line.rstrip("\r\n")):]
            lines[k] = head + params[:i] + HEAP_SIZES[m.group(3)] + params[j:].rstrip("\r\n") + ending
            changed.add(m.group(3))
    missing = set(HEAP_SIZES) - changed
    if missing:
        raise RuntimeError("memory.cfg: heaps not found: " + ", ".join(sorted(missing)))
    _write(path, "".join(lines))


def main_scrape(path):
    text = _read(path)
    text, n1 = re.subn(r"^(var\s+texture\s+FrameBufferTexture\s+)1280 720", r"\g<1>960 544", text, flags=re.M)
    text, n2 = re.subn(r"^(var\s+surface\s+FrameBufferSurfaceDepth\s+edramdepth\s+)1280 720", r"\g<1>960 544",
                       text, flags=re.M)
    if (n1, n2) != (1, 1):
        if text.count("960 544") >= 2:
            return  # already done
        raise RuntimeError("main.scrape: the 1280x720 render targets were not found")
    _write(path, text)


def nav_lua(path):
    text = _read(path)
    nl = "\r\n" if "\r\n" in text else "\n"
    new = ("function P:collect()" + nl +
           "\t-- SSX Vita: no collection here (a full one took 15-60 ms); Lua's" + nl +
           "\t-- incremental collector runs as memory gets allocated" + nl +
           "end" + nl)
    if new in text:
        return
    text, n = re.subn(r"function P:collect\(\).*?\r?\nend\r?\n", lambda m: new, text, count=1, flags=re.S)
    if n != 1:
        raise RuntimeError("nav.lua: P:collect() not found")
    _write(path, text)


def run(assets, log, progress):
    for i, (rel, fn) in enumerate(((MEMORY_CFG, memory_cfg), (SCRAPE, main_scrape), (NAV, nav_lua))):
        fn(os.path.join(assets, rel))
        log(f"  {rel}")
        progress((i + 1) / 3)
