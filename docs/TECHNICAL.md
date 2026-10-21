# How the port works

SSX for Android is a native game: almost all of it is `libgame.so`, 40 MB of ARMv7 code
(softfp float ABI), with a thin Java layer around it. The library isn't stripped: its symbol
table has every C++ function name, which made most of this work possible.

The port is a *so loader*, the approach of [gtasa_vita](https://github.com/TheOfficialFloW/gtasa_vita):

1. `main.c` loads `libgame.so` and `libgnustl_shared.so` into memory with
   [so_util](https://github.com/Rinnegatamante/so_util) and resolves their imports against
   `dynlib.c`: the Vita's newlib C library, [vitaGL](https://github.com/Rinnegatamante/vitaGL)
   for OpenGL ES 2, and wrappers where Android and the Vita differ.
2. `patch.c` and `source/ssx/` hook the game by offset (for v0.0.8430 only) where it needs to
   behave differently on the Vita.
3. `source/ssx/lifecycle.c` plays the part of the APK's Java activity: it calls the game's JNI
   entry points in the order Android would. The Java classes the game calls back into are
   rewritten in C (`source/jni/`) and reached through
   [FalsoJNI](https://github.com/v-atamanenko/FalsoJNI).

The game runs as on Android, with its own threads: a render thread (`FGGameRender`), the game
logic (`FGGameSim`), five EA job workers, audio, file streaming.

All measurements below are from the Vita (overclocked to 444 MHz, as the port runs), mostly with
the debug build. "Not measured" means the change was made for a reason found in the code or the
logs, but its effect wasn't measured on its own.

## Running Android code on the Vita

| Problem | Fix |
|---|---|
| Bionic's `struct stat`, `dirent` and error numbers differ from newlib's | Converted in the wrappers (`source/reimpl/`) |
| The game's mutexes, condition variables and semaphores are Bionic's | Rewritten on Vita kernel objects with Bionic's layouts (`reimpl/pthr.c`); waits use a parking lot on pooled semaphores. A first condition variable leaked kernel objects, then froze |
| The Vita doesn't time-slice threads of equal priority: a few EA loops wait for another thread by re-reading a value without yielding, and the thread they wait for never ran again. The game froze a few minutes into a race | The backward branch of each such loop goes through a short sleep (`ssx/spinwait.c`) |
| GL calls from threads without the GL context: on Android they do nothing; on the Vita they reached vitaGL, which isn't thread-safe (a `glTexSubImage2D` from a worker crashed the GPU driver) | Skipped, as on Android (`utils/glutil.c`). Deletes from those threads are queued and run on the render thread: Scaleform deletes textures from its own threads, and dropping those leaked ~5.5 MB of textures per race (the next race ran out of memory) |
| Android-style paths (`/data/data/com.ea.ssx/...`, `/sdcard`) | Mapped to `ux0:data/ssx/` (`ssx/assets.c`) |
| On first boot the game copies ~750 MB of audio streams and movies out of the APK | Never made: reads of those copies are served from the assets (`ssx/assets.c`, `patch.c`) |
| The 25 MB ghost save was copied 2 KB at a time at startup (8 s behind the saving icon), and its writes were slow | The file is almost all zeros: stored sparsely, 8 KB on the memory card, as a page table in RAM while open (`reimpl/sparse_save.c`); writes buffered in 64 KB blocks, small sequential reads read ahead (`reimpl/io.c`). Ghost save copies are made in one pass (`ssx/filecopy.c`, ~1 s freeze after races before) |
| Audio output | `jni/jni_audio.c` plays 4 grains of 16 ms queued on its own thread. The first double buffer gave 21 ms of slack, and a busy core meant dropouts |
| A new ghost record sometimes made the game report the ghost file corrupted: `store()` allocated the whole sparse file (~400 KB) to write it, the allocation failed when memory was short in a race, and the close failed silently | Written through a static 64 KB buffer (`reimpl/sparse_save.c`) |
| Movies: the APK's player is Java (`MediaPlayer`) | `jni/jni_video.c` plays them with the Vita's `sceAvPlayer` on the GL thread and draws each frame over the game's with a textured quad. The decoder takes 4 MB of frames and a 14 MB block of physically contiguous memory, which vitaGL leaves free (`MOVIE_PHYCONT`, 20 MB); the player's own ~3.8 MB comes from a memory block made per movie. The data tool re-encodes the movies to H.264 960×544 with AAC audio (`movies.py`): 681 → 127 MB |
| Controls | PS3 pad layout; L2/R2 on the halves of the rear touchpad. Select + Start opens the port's settings menu (`ssx/overlay.c`, drawn into the finished frame from vitaGL's display callback, no GL calls; the game gets no buttons while it is open; settings in `ssx/settings.c`); Square and Circle are the ground presses (below) |
| Games only get CPU cores 0-2; core 3 is the system's | With GrapheneCt's CapUnlocker plugin a game thread's affinity may include core 3. As the game names its background threads (EA job workers, file streaming, texture work), they widen their mask to all 4 cores (`ssx/core3.c`); render, simulation and audio stay on 0-2, where the system's work can't interrupt them. Without the plugin the kernel refuses the mask and nothing changes. One debug run each on the same map: race speed median 100% instead of 93%, the simulation waiting 18.9 ms per step for the riders' jobs instead of 24.5, same frame rate |

## Memory

The game's own heaps get ~260 MB of the Vita's RAM, and the GPU 128 MB of video memory; Android
phones of the time had more, and backed memory only as it was used. What was done, roughly in
order of size:

| Change | Effect |
|---|---|
| Heap sizes (`memory.cfg`, written by the data tool, `datatool/ssxdata/patches.py`): EA's MemoryFramework allocates every heap at its full size at startup | ~573 MB on Android, ~260 MB here, each heap sized from the peak use measured on the Vita (the reason for each size is next to it in the code) |
| 4-channel audio banks cut to stereo, the ambience and wind loops to mono, without re-encoding (EA-XAS stores channels separately) (`audio.py`) | 39 MB less of audio banks; `ambience_ext.abk` alone 29 → 7.4 MB, all loaded in a race |
| RAM copies of textures and static vertex/index buffers freed once uploaded (`ssx/texmem.c`, `ssx/bufmem.c`) | Not measured on its own. An allowlist of creator call sites: freeing all of them broke character meshes |
| Video memory leak: the game's buffer `Destroy()` skips `glDeleteBuffers` when the RAM copy is gone, so every buffer whose copy was freed leaked | ~10 MB per race; terrain went missing and the game crashed by the fourth race. Fixed in the `Destroy` hook |
| The 3D scene rendered into a hard-coded 1280×720 target, then scaled down to the screen (`main.scrape`, `patches.py`) | Rendered at 960×544: 44% fewer pixels to draw and store |
| Big 3D textures at the screen's size (`textures.py`): the globe 2048×1024 → 1024×512, front-end backdrops 1280×720 → 960×540, sky cubes 6×1024² → 6×512² | 28.6 MB less on disk. In a race they were ~14 MB of the 40 MB of textures in video memory (globe 8 MB, a backdrop 1.8 MB, two skies 6 MB). Only 3D textures: they are sampled by UV, so their size on screen doesn't change. The skies are encoded again in ETC1 by the tool's own encoder (Android's reference method): a fast encoder (etcpak) left visible squares in the blue, 38.5 dB against 45.8 dB on the sky |
| Race loading screens (`textures.py`): uncompressed 1280×720 images despite their `.etc1.pvr` name, 1.8 MB each in the UI's memory pool, in one piece | 960×540, 1.0 MB each; 59 MB less on disk. They still fill the screen: the loading screen sizes them to their placeholder |
| More memory for vitaGL: the common dialog memory (~8.8 MB) becomes a vitaGL pool | 8.8 MB more for GPU data |
| Reads from `assets.pak` failed in race loads (175-257 KB files): no memory left for the read buffer, the EA heaps leaving only 2.6-4.5 MB of the newlib heap free | Five heaps that never got near their size made 12 MB smaller (`patches.py`); a file whose buffer can't be allocated is read from the pack on demand (`reimpl/asset_manager.cpp`) |

How full each pool gets, measured over a session (boot, two movies, menus, a race on the big map;
debug `[MEMPEAK]` lines and the pools' own peaks in `[GLMEM]`): video memory 89.9 of 96 MB, the
common dialog pool 7.3 of 8.8 MB, vitaGL's small RAM (3 MB) and contiguous (6 MB) pools full
(they overflow into the others, as designed), the movie decoder 19 of its 20 MB, the newlib heap
283.7 of 296 MB at its highest. No allocation failed, and no system memory is left unassigned.

### The UI memory pool

Scaleform (the UI) has its own 35 MB pool. Long sessions ended with a crash when a race loaded:
a 1.8 MB allocation failed. A walk of the pool with the allocator's own report function
(`SSX_POOL_WALK`) showed it wasn't a leak but **fragmentation**: in use 19.3 MB in a first race
and 19.6 MB in the second, free 16.5 and 16.3 MB, but the largest free run fell from 15.2 to
11.5 MB, because a few long-lived blocks land in the middle of it while menus are open. The
1.8 MB blocks were the loading screen image (a 16-bit copy, and a temporary one for the upload).

Fixes: the loading screens are smaller (above), and Scaleform blocks of 512 KB and up go to
system memory when the pool has no free run that holds them with 2 MB to spare
(`ssx/gfxcache.c`, `[GFXBIG]` lines).

## Loading times

| Change | Effect |
|---|---|
| The memory card's file system looks a name up by reading its folder's entries one by one: folders with more than 300 files are split into 64 hashed subfolders (`layout.py`, `ssx/assets.c`) | Opening a file in `worlds/geometry` (4554 files) took ~420 ms. A race load: ~2 min → ~15 s (with the buffered log writes below) |
| An open on the memory card costs 10-100 ms whatever the file's size: every asset up to 256 KB is also packed in `assets.pak`, read from one open file (`layout.py`, `reimpl/asset_manager.cpp`) | 10605 files. Part of the UI freeze fixes below |
| Texture files are read without holding the GPU lock (`ssx/texload.c`) | Render-thread waits during UI loads: 300-400 ms → ≤120 ms |
| vitaGL's shader cache opened a ~1.3 KB file per shader, two per program link, 5-25 links per UI frame | The cache is one file read whole (`lib/vitagl`, `custom_shaders.c`); UI frames linked shaders for 80-130 ms before |
| Unloading a race's or the globe's files took the GPU lock (`GameRender::AcquireThread`) once per file, and the render thread holds it nearly all the time at 60 Hz, so each file waited for a frame | The lock is taken once for the whole batch (`ssx/assetunload.c`): the character screen after a race 6-19 s → ~1.5 s; the globe's unload on a race load, 2.8 s of waits, now waits once (its new time not measured on its own) |
| The globe loads a mountain range's assets two at a time, pausing 0.15 s in between | Up to four in flight, no pause (`ssx/globeload.c`). Not measured on its own |
| After a race that unlocks the next one, the campaign screen sat still for 4-5 s, moved the selection, then 4-5 s more before taking input. That is the original game (Android does the same): its post-race sequence waits for a menu animation that shows nothing | Each wait held 1.5 s instead (`ssx/unlockwait.c`); with no wait at all it felt abrupt |

## Frame rate

### Graphics and CPU

The CPU is what limits the game on the Vita. In a race the GPU waits under 1 ms per frame,
and the render thread is busy almost all the time.

| Change | Effect |
|---|---|
| Characters were skinned on the CPU. The game has a GPU skinning path that was never used: it's built, wired into the character's draw calls, and given the shaders' skinned variants (`ssx/hwskin.c`). Merged skeletons stay off: a merged palette holds 50 bones, and a rider has more (stretched hands, misplaced board) | A full race: 15-28 FPS → 27.7-29.8 FPS (average 29.2) |
| Text and icons were drawn with other meshes' vertices: vitaGL put each vertex attribute's absolute offset in a 16-bit field, and Scaleform's mesh buffer is 1.7 MB | Fixed (the stream pointer carries the base offset) |
| Scaleform re-uploaded its whole 1.7 MB mesh buffer once or twice a frame (7 ms each, to uncached memory) | Only the pages it handed out are copied, into a ring of storages (`ssx/sfmesh.c`). Not measured on its own |
| Terrain: one full mesh setup per chunk | Chunks after the first of a run bind only buffers and attribute pointers (`ssx/terrainmerge.c`): bind 7.7 → 4.3 µs, draw 9.6 → 8.5 µs per chunk, ~200 chunks per frame |
| Shader parameter lookups by name (`RNA::ScopeC::FindParm`, ~2000 a frame) | Cached, ~85% hits (`ssx/scopecache.c`): ~2-3 ms per frame |
| The game's `glFinish` | A no-op: nothing in the port needs the GPU to be idle |
| vitaGL sets the same programs and textures again | Skipped when unchanged since the frame began |
| The render thread lost its high priority (the game names its threads to set it, and the name was missing) | Named on its first frame, priority back (`ssx/perf.c`) |
| CPU clock | 444 MHz (`utils/init.c`, `SSX_OVERCLOCK`). Not compared with the stock clock |
| Terrain chunks outside the view were drawn: the engine's visibility system (`EA::CoreWorld::Bvm2`) tests each chunk's box against the frustum and stores a class per chunk, but writes the distance and LOD values the draw loop uses only for chunks in view, and the loop never reads the class. Every loaded chunk was drawn, about half of them off screen | `TerrainGameObject::SelectLOD` returns no level for a chunk classed outside (`ssx/terraincull.c`). Big map with riders: 75-107 chunks drawn per frame → 30-58, `GameRender::Draw` 26.6 → 19.6 ms in the profiling build (different runs) |
| Each material bind looked up its shader's 17 vertex attributes and compared every parameter's type name with `Stricmp` (`OGLESProgram::Bind`), looked up blocks by name (`ScopeC::FindBlockByName`, ~6 µs each), and did the same compare again for each parameter it uploaded (`SendParmsToGPU`). The attribute lookups that find nothing aren't cached by `FindParm`: ~200 full searches a frame | `Bind` and `SendParmsToGPU` rewritten in C with the type check cached (`ssx/shaderbind.c`), block lookups by literal name cached (`ssx/scopecache.c`), the attribute lookups kept while no scope changes, found or not. Per terrain chunk, the material bind 71 → 47 µs; the full searches 200 a frame → 0 |
| Every mesh draw set its vertex elements up one by one (attribute location, buffer, enable, pointer) and undid them after, ~15 GL calls per draw. The game has a vertex array object path, but only for meshes marked static (none are), with one object per mesh although the attribute locations belong to each shader program | Each mesh keeps its setup in a vertex array object, along with what it depends on (locations, buffers, strides, index buffer, format); a bind that would set up the same just binds the object (`ssx/vaocache.c`): ~110 of ~115 mesh binds a frame |
| Square roots and floors called the C library (1864 and 360 call sites) | The VFP instruction and an exact integer version (`reimpl/fastmath.c`), the same results bit for bit. Not measured on its own |

The attribute lookups kept, `SendParmsToGPU` in C and the vertex array objects were measured
together, switched on and off every 10 s within one race on the big map with riders (`AB_TEST`
in `ssx/perf.c`): `GameRender::Draw` 14.8 ms against 17.9,
and 16.6 against 20.8, about 17-20% more frames in that (debug) build. A function profiler had
put these costs higher; its own overhead on such small functions misleads, so measure that way.

### 60 FPS in menus, up to 60 in races

The game paces itself: its render loop and its simulation are both `GameKernel::Loop` objects
made at a fixed 30 Hz. At 60 Hz (`ssx/fpscap.c`), menus run at 60. The simulation can't in races:
it advances game time by a fixed 1/60 s per step and catches up at most one step, so when it
makes fewer than 60 steps a second the race plays in slow motion. Measured in a race with the
simulation at 60 Hz: ~31 steps a second (game at ~52% speed).

What holds the simulation back is the riders' animation: their scene operations (EA::Ant) take
~17.5 ms of CPU a step, spread over the job threads, but ~22 ms of wall time, because they depend
on one another and mostly run one after the other. Scheduling can't fix that (see below).

So the simulation runs at 30 Hz in races, and the render loop at 60 Hz anyway: the game's render
objects interpolate between two simulation states (`InterpolateState`), so the frames in between
show motion rather than repeats. Races then run between ~35 and 60 FPS depending on the map.

At 30 Hz the same limit applies, with less margin: a step has 33 ms, and the riders' jobs take
~21 ms of it. When all cores are busy (demanding maps, many riders on screen) the jobs wait for
CPU time, steps run longer than 33 ms, and the race slows down for a moment while the frame rate
can look fine. Two settings leave the race more CPU: the 4th core with CapUnlocker (above), and
**RACE FPS 30** in the settings menu, which sets the render loop to 30 Hz in races (`ssx/fpscap.c`)
so the render thread draws half as many frames.

### UI freezes and in-race stutters

Measured with the debug build, profilers off:

| Screen | Before | After |
|---|---|---|
| Race results (first frame) | ~640 ms | 368 ms |
| Post-race menu | ~455 ms | 341 ms |
| Main menu, campaign | frames of 160-330 ms | none over 150 ms |
| Race | 3-6 frames over 100 ms | none |

What fixed it, by effect:

1. **The debug logger** (`utils/logger.c`) wrote warnings to the memory card from the thread
   that logged them: vitaGL's statistics, logged inside the frame swap, held the render thread
   80-270 ms whenever the card was busy. These were the in-race stutters (debug builds only).
2. **Lua garbage collection in the UI** (`patches.py`, `ssx/luagc.c`): the UI navigation ran a
   full Lua collection after every navigation event, 15-60 ms each, several per screen change.
   Lua's incremental collector keeps up on its own.
3. **Scaleform's ActionScript garbage collector** (`ssx/sfgc.c`): it ran every 60 frames whenever
   it had anything to look at, 23-36 ms each time, and a full collection ran on every movie load.
   Now every 900 frames, with heap growth starting the rest. On the results screen, collections
   started by a 2 MB heap limit used to run hundreds of times in a row (~2 s of frozen frame): the
   limit is raised to 12 MB.
4. **Scaleform's small allocations** (`ssx/gfxcache.c`): ~21500 allocations and ~18000 frees in
   the results screen's first frame, each through two allocators and a lock. Freed blocks up to
   1 KB are kept and reused as they are: 70-75% of the allocations in menus, ~90% in races, at no
   extra memory per block.
5. **File opens**: the asset pack (above).
6. Smaller: child clips looked up by name without reference-count traffic
   (`ssx/displaylist.c`).

What's left on the results screen is Scaleform's own work spread over its interpreter, object
creation and text layout, with nothing wasteful found in its code.

## Ground presses (nose and tail)

The console versions let you press the board's nose or tail while riding on the ground. The
Android version has no control for it, but the feature wasn't removed: only its input was left unconnected.

What is still in the game:

- **Code**: `EA::Ant::Ssx::CarvingController`, the ground riding controller, has the press
  logic (`ComputeBoardPressBlendValue`, the press speed checks) and reads two inputs,
  `BoardPressNoseInput` and `BoardPressTailInput`, plus `...ClassicInput` variants used with
  the classic control scheme (schema entries 352-355 of its asset).
- **Data**: the rider animation graph (`common/anim/anim_all.cba`) wires those inputs and has
  the press tuning (`BoardPressStartSpeed`, `BoardPressStopSpeed`, `BoardPressFrictionModifer`,
  `BoardPressBlendValueGain`), animations, and scoring (`BoardPressScoringAssets`,
  `ScoringBoardPressTimeAsset`, `TrickComboTimeBoardPressPercentage`); the trick names
  `TXT_SSX_TRICK_NOSEPRESS` and `TXT_SSX_TRICK_TAILPRESS` are in the text.

How the input is read: `CarvingController::GatherGameStateParameters` evaluates the two inputs
through evaluator objects of the controller's asset (+1432/+1436, or +1440/+1444 with classic
controls), stores them in its `InputParameters` (+217 nose, +218 tail), and right away decides
whether the board presses (+216): one of them set, the speed above `BoardPressStartSpeed`
(`BoardPressStopSpeed` once pressing), and the rider in a state that allows it. On Android
nothing ever sets the inputs.

What the port does (`source/ssx/boardpress.c`): the two stores are patched to jump to small
stubs that add **Square** (nose) and **Circle** (tail) to what the evaluators gave, before the
decision is made; the press physics, animation and scoring then run as the game has them.
Setting the inputs after the function returns does nothing: the decision is already taken
inside it. Only the player's rider gets the buttons: the stubs compare the game state table
the controller reads with the one the pad data was last written to
(`Peripheral::ClassicGamepadAsset::UpdateGamepadData`, which `GameplayBase::
GetClassicGamepadData` calls for controller 0 only).

## What didn't work

Kept here so nobody tries them again blind:

- **Shrinking screen-sized images at upload** (to 960 wide): one of them was a 1280×720 render
  target, and the 3D view came out zoomed and off-centre. Shrinking is done on files, for
  textures known to be safe.
- **Condition variables on light kernel objects** froze; the real cause was the spin-wait loops.
- **A statistical profiler** that sampled the logic thread by faulting on libgame's code pages
  rebooted the console twice. Removed.
- **Keeping the last 24 UI movies loaded** for reuse: no measurable gain, and it held memory.
- **Scaleform block reuse, first versions**: cutting blocks from chunks kept per UI file ran the
  UI pool out of memory after ~55 minutes (Scaleform keeps one heap per UI file for the whole
  session, so freed blocks were stranded); a version with a header of its own on every block
  fragmented the pool twice as fast. The current version reuses blocks as the game made them.
- **Comparing Scaleform's mesh buffer with a RAM copy** before uploading it: the compare (~20 ms)
  cost more than the upload it saved.
- **Caching `DBProfile::GetItem`** (10 calls a frame) and **raising the logic thread's priority**:
  no effect on the menus.
- **60 Hz for the simulation in races**: slow motion (above).
- **Pinning the game's threads to cores**: the render thread took 31.1 ms a frame against 26.3
  without.
- **The simulation running the riders' animation jobs itself** (`WaitOn` instead of `SleepOn`):
  a step took 33 ms against 20; the jobs take as long, and the render thread lost CPU.
- **Stricter terrain culling**: the visibility system's frustum is exact (no margin); its far
  plane is pushed out, but the projection's is ~8000 units away and drawn chunks are within
  ~1250, so nothing lies past it. The engine can also cull with occluders (mountains), but the
  game ships no occluder data, on Android either.
- **Texture upload tweaks against the globe "freeze"** (staging ETC1 swizzles in a CPU buffer,
  allocating a texture's mip chain at once): uploads took as long (~16 ms), and the freeze was
  the game's own waits (above).

## Open questions

- Why Scaleform keeps a second 16-bit copy of the loading screen images
  (`ImageSource::CreateCompatibleImage`) when the files already are 16-bit.
- The race HUD (Scaleform) costs ~5 ms of the render thread a frame. It changes at most 30 times
  a second (the simulation's rate): drawing it into a texture every other frame and reusing it in
  between could save about half of that. Not tried.
