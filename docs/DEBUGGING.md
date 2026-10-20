# Debugging

## The debug build

`./build.sh` makes the debug build, "SSX (2012) Debug" (SSXD00001). It runs like the release
build, plus:

- a log in `ux0:data/ssx/log.txt`, written by a background thread from two buffers (only
  errors are written at once, so logging doesn't stall the game);
- reports every 5-10 s on performance and memory;
- a watchdog: if the game stops completing frames, it logs every thread's state and crashes on
  purpose, so that the Vita writes a crash dump.

Release builds have no log file, and compile out `l_debug`, `l_info`, `l_warn`, `l_success`
and `l_wait` (only `l_error` and `l_fatal` remain). **Never put side effects in a log macro's
arguments**: they disappear in release builds. That once caused a release-only crash.

### Log lines

| Tag | What |
|---|---|
| `[FPS]` | Frames per second, draws per frame, vitaGL's free memory |
| `[PERF]` | Every 5 s: frame time and swap wait, time per phase of a frame, render-thread waits, Scaleform heap use, buffer updates, CPU per thread (every 10 s) |
| `[LOOPS]` | Every 5 s: the game's simulation and render loops, their rate and the resulting game speed, what the simulation waited on |
| `[FPSCAP]` | The game loops' rates: 60 Hz in menus; in races, the simulation at 30 Hz and the render loop at 60 |
| `[MEM]` | Every 10 s: free system memory and the newlib heap; with `SSX_DIAG`, each EA MemoryFramework heap too (size, largest free block; the game doesn't keep their used and peak counters) |
| `[MEMPEAK]` | Every 10 s, since boot: the newlib heap's highest extent and use, the least free main, CDRAM and contiguous memory |
| `[GLMEM]`, `[TEXMEM]`, `[BUFMEM]` | Video memory: vitaGL's pools (used, size, peak), textures, game buffers |
| `[ABTEST]` | With `AB_TEST` set in `ssx/perf.c`: `GameRender::Draw`'s time with the optimizations under test on and off (switched every 10 s) |
| `[HITCH]` | A render frame over 100 ms: CPU of the busiest threads, waits, I/O, uploads, shader links |
| `[SIMHITCH]`, `[SIMSLOW]` | A logic (simulation) frame over 150 ms, or a run of frames over 50 ms |
| `[STALL]` | A thread waited over 200 ms on a lock, condition or semaphore, with the game's stack |
| `[GFXHEAP]` | Scaleform allocations of a long logic frame, and their time |
| `[GFXBIG]` | A big Scaleform block put in system memory because the UI pool was short |
| `[SFGC]`, `[LUAGC]` | Scaleform's ActionScript garbage collector and full Lua collections, timed |
| `[SLOWOPEN]`, `[SLOW]` | Asset files slow to open or read |
| `[SCHED]`, `[THREAD]` | The game's threads: names, priorities, cores |
| `[AUDIO]` | Audio output: underruns (`mixer late`), streams |
| `[COPY]`, `[SAVE]` | Save file copies and writes |

### Crash dumps

When the game crashes, the Vita writes `ux0:data/psp2core-*.psp2dmp`. To read one, use
[vita-parse-core](https://github.com/xyzz/vita-parse-core) with the ELF of the same build
(`dist/ssx_vita_debug.elf` or `dist/ssx_vita.elf` from `./build.sh both`):

```sh
python3 main.py psp2core-....psp2dmp dist/ssx_vita_debug.elf
```

`libgame.so` is loaded at `0x98000000`: an address `0x98xxxxxx` in the dump is
`libgame.so + (address - 0x98000000)`. With the game's symbol table (`arm-vita-eabi-nm -C
libgame.so`), that gives the game function.

## Profiling options

Off by default because they slow the game. Turn one on with CMake, for a debug build:

```sh
CMAKE_ARGS="-DSSX_FNPROF=ON" ./build.sh
```

| Option | What it logs | Cost |
|---|---|---|
| `SSX_GLPROF` | Time in each GL function, per frame (`[PERF]`) | ~1 µs per GL call, 2-3 ms per race frame |
| `SSX_FNPROF` | Own and inclusive time of the game functions in `source/ssx/fnprof_list.h` (`[FNPROF]`, and `[FRAMEPROF]` per long logic frame) | High; leaf functions called tens of thousands of times read high |
| `SSX_ALLOCPROF` | Who allocates through MemoryFramework, and what it costs | A stack scan per allocation |
| `SSX_RENDERPROF` | Which code holds the most memory in the `RENDER_GEN` heap | High |
| `SSX_ASPROF` | ActionScript functions and clip member reads of long UI frames (`[ASPROF]`) | Every script call and member read |
| `SSX_POOL_WALK` | The UI memory pool walked block by block every 30 s: used, free, largest run (`[POOL]`) | Holds the pool's lock during the walk |
| `SSX_DIAG` | Verbose logs, and the diagnostics of past investigations, kept for when one comes back: debug-level lines (every file opened, stat'ed and closed, the game's own debug messages), vitaGL's draw statistics every 30 frames, every asset and audio file loaded (`[ASSET]`, `[AUDIOFILE]`), the ghost save's parts and errors (`[GHOSTSAVE]`), every UI file loaded and freed (`[MOVIELIFE]`), the globe's timeline and slow uploads (`[GLOBE]`, `[UPLOAD]`), the post-race actions (`[PROGRESS]`), the GPU lock's waits by caller (`[GPUWAIT]`), big GPU allocations (`[GPUALLOC]`), the render passes of a frame (`[FBPROBE]`), the terrain chunks drawn and culled (`[TERRAIN]`), the presses' counts (`[PRESS]`), where the simulation waits (`[SIMWAIT]`), math call counts (`[MATH]`, through counting wrappers), the parts of the frame swap, the EA heap table | A log several times longer; the counters cost a little CPU |

`fnprof_list.h` is a list of `{ offset, name }` entries for the functions to time, taken from the
game's symbol table (`nm -S` on `libgame.so`); any list of functions in that form works.

## Hooking the game

The port patches `libgame.so` by offset, for v0.0.8430 only. `so_util` offers:

- `hook_arm`: overwrites a function's start with a jump to ours;
- `hook_arm_trampoline`: the same, keeping the original callable. It copies the first two
  instructions, and refuses functions whose first two instructions branch or use the PC,
  except PC-relative literal loads, which it relocates. Check with a disassembly first.

The library isn't stripped: its full symbol table (C++ names) is the best map of the game.
`arm-vita-eabi-objdump -d libgame.so` gives a disassembly to search for functions and their
callers.

Things that bit before:

- Calls from the game into GL go through the game's `EA::Graphics::OpenGLES20` wrappers, so
  `__builtin_return_address` doesn't point at the real caller. Scan the stack for libgame
  addresses instead (`game_stack()` in `source/utils/glutil.c`), and stay within the thread's
  stack: reading past its top crashes.
- Scaleform loads movies on its own threads: a profile that adds up all threads puts that work
  into logic frames.
- vitaGL's buffer, framebuffer and renderbuffer names are pointers; texture names are small
  indices.
