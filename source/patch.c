/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 */

#include <kubridge.h>
#include <so_util/so_util.h>
#include <stdio.h>
#include <string.h>
#include <vitasdk.h>

#ifdef __cplusplus
extern "C"
{
#endif
	extern so_module so_mod;
	extern so_module gnustl_mod;
#ifdef __cplusplus
};
#endif

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)

#include "utils/logger.h"
#include "utils/dialog.h"
#include "reimpl/sys.h"
#include "ssx/assets.h"
#include "ssx/texmem.h"
#include "ssx/spinwait.h"
#include "ssx/bufmem.h"
#include "ssx/scopecache.h"
#include "ssx/shaderbind.h"
#include "ssx/terraincull.h"
#include "ssx/vaocache.h"
#include "ssx/boardpress.h"
#include "ssx/jobprof.h"
#include "ssx/sfmesh.h"
#include "ssx/hwskin.h"
#include "ssx/audioguard.h"
#include "ssx/sfgc.h"
#include "ssx/terrainmerge.h"
#include "ssx/texload.h"
#include "ssx/allocprof.h"
#include "ssx/gfxcache.h"
#include "ssx/asprof.h"
#include "ssx/filecopy.h"
#include "ssx/luagc.h"
#include "ssx/displaylist.h"
#include "ssx/movielife.h"
#include "ssx/assetunload.h"
#include "ssx/globeload.h"
#include "ssx/unlockwait.h"
#include "ssx/fpscap.h"
#include "ssx/trace.h"
#include "ssx/renderprof.h"
#include "ssx/perf.h"
#include "ssx/glprof.h"
#include "jni/jni_ssx.h"
#include <stdbool.h>

void __kuser_memory_barrier(void) {
	__sync_synchronize();
}

static void kuser_patch_module(so_module *mod);

void kuser_patch(void) {
	SceKernelAllocMemBlockKernelOpt opt;
	memset(&opt, 0, sizeof(SceKernelAllocMemBlockKernelOpt));
	opt.size = sizeof(SceKernelAllocMemBlockKernelOpt);
	opt.attr = 0x1;
	opt.field_C = (SceUInt32)0x9A000000;
	if (kuKernelAllocMemBlock("atomic", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, 0x1000, &opt) < 0)
		fatal_error("Error could not allocate atomic block.");
	kuKernelMemProtect((void *)0x9A000000, (SceSize)0x1000, KU_KERNEL_PROT_EXEC | KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE);

	hook_addr(0x9A000FA0, (uintptr_t)__kuser_memory_barrier);
	hook_addr(0x9A000FC0, (uintptr_t)__atomic_cmpxchg);

	kuser_patch_module(&so_mod);
	kuser_patch_module(&gnustl_mod);
}

static void kuser_patch_module(so_module *mod) {
	uint32_t patched_addr;
	for (uint32_t addr = mod->text_base; addr < mod->text_base + mod->text_size; addr += 4) {
		uint32_t *a = (uint32_t *)addr;
		if (*a == 0xFFFF0FC0) {
			l_debug("Patching 0x%x -> __kuser_cmpxchg", a);
			patched_addr = 0x9A000FC0;
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &patched_addr, sizeof(uint32_t));
		}
		else if (*a == 0xFFFF0FA0) {
			l_debug("Patching 0x%x -> __kuser_memory_barrier", a);
			patched_addr = 0x9A000FA0;
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &patched_addr, sizeof(uint32_t));
		}
	}
}

int ret0(void);

// MERCURY::MercuryManager::Init. The front end's Mercury download screen
// (FE::MercuryDownloadProvider::OnEnter) waits until data store items 456 or
// 457 ("package info retrieved") are set, and starts a download unless 453
// ("package downloaded") is set too. MercuryManager::OnPackagesRetrieved would
// queue them through flags this+4 (456), this+5 (457) and this+6 (453), which
// MercuryManager::UpdateDatastore() then writes to the data store every frame.
static int MercuryManager_Init(void *self, void *system, void *activity) {
	uint8_t *mm = (uint8_t *)self;
	mm[4] = 1; // FEDataStore 456: package info retrieved
	mm[6] = 1; // FEDataStore 453: package already downloaded
	l_info("Mercury: reporting the SSX package as already downloaded");
	return 0;
}

// EA::IO::File::Copy(char const* src, char const* dst, bool overwrite)
static so_hook file_copy_hook;

static int File_Copy(const char *src, const char *dst, int overwrite) {
	// The first-boot copy of the streamed files out of the APK; on the Vita
	// those are read straight from the assets (ssx/assets.c)
	if (ssx_is_big_file(dst)) {
		l_info("File::Copy(%s -> %s): skipped, served from assets", src, dst);
		return 1;
	}
	return SO_CONTINUE(int, file_copy_hook, src, dst, overwrite);
}

// GameRender::PlatInit(): starts from a hard-coded 1280x720 render resolution
// and only switches to the display's size if RNA::DevicePlatC::InitEAMTDevice
// succeeds. Full-screen render targets are sized from it; the GPU allocations
// that ran out of VRAM were 1280x720-sized (texture uploads then stalled 4 s
// each, black screen).
// Render at the Vita's native resolution instead.
static so_hook gamerender_platinit_hook;
static int GameRender_PlatInit(void *self) {
	int ret = SO_CONTINUE(int, gamerender_platinit_hook, self);
	int32_t *res = (int32_t *)((char *)self + 28); // render w, h, current w, h
	l_info("GameRender::PlatInit = %d, render %dx%d, current %dx%d",
	       ret, res[0], res[1], res[2], res[3]);
	if (res[0] != SCREEN_W || res[1] != SCREEN_H) {
		res[0] = res[2] = SCREEN_W;
		res[1] = res[3] = SCREEN_H;
		l_info("GameRender: render resolution forced to %dx%d", SCREEN_W, SCREEN_H);
	}
#if SSX_RENDERPROF
	// The memory framework is up by now (PlatInit allocates from it)
	ssx_renderprof_start();
#endif
	return ret;
}

// MemoryFramework::ErrorHandlers::OutOfMemory(MemoryFailure const&): the game
// prints the details through its own printf wrapper, which loses them. Log
// them before it gives up, to know which memory.cfg heap to grow.
typedef struct {
	const char *category;
	const char *name;
	uint32_t size;
} MemoryFailure;

static so_hook oom_hook, oom_hook2;

static void log_oom(const MemoryFailure *f) {
	l_fatal("[MEM] OUT OF MEMORY: %u bytes for '%s' in category '%s'",
	        f ? f->size : 0, f && f->name ? f->name : "?", f && f->category ? f->category : "?");
	ssx_renderprof_report("out of memory");
}

static void OutOfMemory(const MemoryFailure *f) {
	log_oom(f);
	SO_CONTINUE(int, oom_hook, f);
}

static void OutOfMemory2(const MemoryFailure *f) {
	log_oom(f);
	SO_CONTINUE(int, oom_hook2, f);
}

// EA::Audio::Core::Dac::SubmitSamplesThreadFunc(void*): the thread that feeds
// AudioTrack. It runs while Dac::sDacActive and Dac::sSubmitThreadRunning are
// set, but it is started before Dac::Start() sets sDacActive. On Android the
// JNI attach and AudioTrack.play() it does first take long enough to win that
// race; with FalsoJNI they are instant, the thread exits at once and
// Dac::Start() waits for it forever (black screen at boot).
#define OFF_Dac_sDacActive 0x184c650
static so_hook submit_thread_hook;
static int Dac_SubmitSamplesThreadFunc(void *dac) {
	volatile uint8_t *dac_active = (volatile uint8_t *)(so_mod.text_base + OFF_Dac_sDacActive);
	for (int i = 0; i < 500 && !*dac_active; i++)
		sceKernelDelayThread(10 * 1000);
	l_info("Audio submit thread starting (sDacActive=%d)", *dac_active);
	int ret = SO_CONTINUE(int, submit_thread_hook, dac);
	l_info("Audio submit thread exited");
	return ret;
}


void so_patch(void) {
	kuser_patch();

	// Texture RAM copies (RNA::TexturePlatC), see ssx/texmem.c
	ssx_texmem_patch();
	ssx_spinwait_patch();
	ssx_bufmem_patch();
	ssx_scopecache_patch();
	ssx_shaderbind_patch();
	ssx_terraincull_patch();
	ssx_vaocache_patch();
	ssx_boardpress_patch();
	ssx_jobprof_patch();
	ssx_sfmesh_patch();
	ssx_hwskin_patch();
	ssx_audioguard_patch();
	ssx_sfgc_patch();
	ssx_terrainmerge_patch();
	ssx_texload_patch();
	ssx_allocprof_patch();
	ssx_gfxcache_patch();
	ssx_asprof_patch();
	ssx_filecopy_patch();
	ssx_luagc_patch();
	ssx_displaylist_patch();
	ssx_movielife_patch();
	ssx_fpscap_patch();
	ssx_assetunload_patch();
	ssx_globeload_patch();
	ssx_unlockwait_patch();
	perf_sim_patch();
	perf_patch(); // times the phases of a game frame (see ssx/perf.c)
#if SSX_FNPROF
	fnprof_patch(); // times render functions; perf_patch() hooks some of the same
#endif

	// These are local symbols of libgame.so (.symtab only, not exported), so
	// they are hooked by offset. All offsets are for ssx-v0_0_8430.apk.

	// MERCURY::MercuryManager::Init(EA::Blast::ISystem*, _jobject*)
	// Would start EA's DLC downloader (servers long gone); the APK already
	// contains all the content. Report the "SSX" package as downloaded instead.
	hook_addr(so_mod.text_base + 0x16c5b0, (uintptr_t)&MercuryManager_Init);

	// EA::IO::File::Copy(char const*, char const*, bool)
	file_copy_hook = hook_addr(so_mod.text_base + 0x11dd24, (uintptr_t)&File_Copy);

	// GameRender::PlatInit()
	gamerender_platinit_hook = hook_addr(so_mod.text_base + 0xdd7504, (uintptr_t)&GameRender_PlatInit);

	// MemoryFramework::ErrorHandlers::OutOfMemory (and its .constprop clone)
	oom_hook = hook_addr(so_mod.text_base + 0x15a978, (uintptr_t)&OutOfMemory);
	oom_hook2 = hook_addr(so_mod.text_base + 0x156ba8, (uintptr_t)&OutOfMemory2);

	// EA::Audio::Core::Dac::SubmitSamplesThreadFunc(void*)
	submit_thread_hook = hook_addr(so_mod.text_base + 0x518dbc, (uintptr_t)&Dac_SubmitSamplesThreadFunc);

	ssx_trace_patch(); // debug builds: boot and UI flow lines
}