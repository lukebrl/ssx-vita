/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  trace.c
 * @brief Debug: lines that follow the game's boot and UI flow.
 *
 * The boot state machine ([BOOTFLOW]), the audio engine's startup ([AUDIO]),
 * the game's threads ([THREAD]), asset and audio file loads ([ASSET],
 * [AUDIOFILE]), UI navigation and the UI views loaded ([UI], also kept for
 * the [HITCH] lines), operations completing ([OPERATION]). Written while
 * getting the game to boot (black screens, audio and loading waits), kept
 * because they still say where the game is when something goes wrong.
 * Debug builds only; [ASSET], [AUDIOFILE] and [GHOSTSAVE] only with SSX_DIAG.
 */

#include "ssx/trace.h"

#ifdef DEBUG_SOLOADER

#include <stdint.h>
#include <string.h>

#include <so_util/so_util.h>

#include "ssx/perf.h"
#include "utils/logger.h"

extern so_module so_mod;

// Debug tracing of the boot sequence (black screen investigation)

// BootFlowOp::Update(float); the state machine's state is at this+28.
// Must return the operation status: OpProcessor::Update destroys the
// operation once Update returns 2 (complete). Dropping it kept the finished
// BootFlowOp alive, completing every frame, so the loading spinner never ended.
static so_hook bootflow_update_hook;
static int BootFlowOp_Update(void *self, float dt) {
	static int last = -1;
	int state = *(int *)((char *)self + 28);
	if (state != last) {
		l_info("[BOOTFLOW] state %d -> %d", last, state);
		last = state;
	}
	return SO_CONTINUE(int, bootflow_update_hook, self, dt);
}

// BootFlowOp::DoError(int)
static so_hook bootflow_error_hook;
static void BootFlowOp_DoError(void *self, int err) {
	l_error("[BOOTFLOW] DoError(%d), state %d", err, *(int *)((char *)self + 28));
	SO_CONTINUE(int, bootflow_error_hook, self, err);
}

// Audio startup tracing (Dac::Start waits for a command the audio thread
// never processes)
static so_hook dac_create_hook, dac_platform_init_hook, dac_resume_submit_hook,
	dac_create_thread_hook, thread_begin_hook;

static int Dac_CreateInstance(void *plugin, const void *params) {
	l_info("[AUDIO] Dac::CreateInstance(%p)...", plugin);
	int r = SO_CONTINUE(int, dac_create_hook, plugin, params);
	l_info("[AUDIO] ...Dac::CreateInstance = %d", r);
	return r;
}

static int Dac_PlatformAudioInit(void *dac, void *system) {
	l_info("[AUDIO] Dac::PlatformAudioInit(%p, %p) flags112=%d flags113=%d...", dac, system,
	       *((uint8_t *)dac + 112), *((uint8_t *)dac + 113));
	int r = SO_CONTINUE(int, dac_platform_init_hook, dac, system);
	l_info("[AUDIO] ...Dac::PlatformAudioInit = %d", r);
	return r;
}

static int Dac_ResumeSubmitThread(void *dac) {
	l_info("[AUDIO] Dac::ResumeSubmitThread(%p)", dac);
	return SO_CONTINUE(int, dac_resume_submit_hook, dac);
}

static int Dac_CreateDacThread(void *dac) {
	l_info("[AUDIO] Dac::CreateDacThread(%p)", dac);
	return SO_CONTINUE(int, dac_create_thread_hook, dac);
}

// EA::Thread::Thread::Begin(int (*)(void*), void*, ThreadParameters const*, wrapper)
typedef struct {
	void *stack;
	uint32_t stack_size;
	int32_t priority;
	int32_t processor;
	uint8_t disable_priority_boost;
	const char *name;
} EAThreadParameters;

static int EAThread_Begin(void *self, void *func, void *ctx, const EAThreadParameters *p, void *wrapper) {
	int r = SO_CONTINUE(int, thread_begin_hook, self, func, ctx, p, wrapper);
	l_info("[THREAD] Begin(func game+0x%x, \"%s\", stack %p/%u, prio %d, cpu %d) = 0x%x",
	       (uintptr_t)func - so_mod.text_base, p && p->name ? p->name : "?",
	       p ? p->stack : NULL, p ? p->stack_size : 0, p ? p->priority : 0,
	       p ? p->processor : 0, r);
	return r;
}

// Audio file loading (InitFileManager waits forever on an AssetStream load)

// AudioFramework::FileManager::UnmanagedLoadFile(char const*, int, int, unsigned int*)
static so_hook unmanaged_load_hook;
static void *FileManager_UnmanagedLoadFile(const char *path, int a, int b, unsigned *size) {
	l_info("[AUDIOFILE] UnmanagedLoadFile(%s)...", path ? path : "(null)");
	void *r = SO_CONTINUE(void *, unmanaged_load_hook, path, a, b, size);
	l_info("[AUDIOFILE] ...UnmanagedLoadFile(%s) = %p, %u bytes", path ? path : "(null)", r, size ? *size : 0);
	return r;
}

// AssetStream::Internal::Get<AssetStream::RawAsset>(char const*, TranslatorData const&)
static so_hook rawasset_get_hook;
static void *AssetStream_GetRawAsset(const char *path, const void *data) {
	void *r = SO_CONTINUE(void *, rawasset_get_hook, path, data);
	l_info("[ASSET] Get<RawAsset>(%s) = %p", path ? path : "(null)", r);
	return r;
}

// Front-end flow tracing: navigation actions and the UI views they load

// UX::NavDispatcherGFx::ProcessNavAction(INavActionCallback*, char const*, Types::Array*)
static so_hook nav_action_hook;
static int NavDispatcherGFx_ProcessNavAction(void *self, void *cb, const char *action, void *args) {
	l_info("[UI] action %s", action ? action : "(null)");
	return SO_CONTINUE(int, nav_action_hook, self, cb, action, args);
}

// UX::NavMessageClient::Event(INavEventCallback*, char const*, Types::BaseType*): game -> Lua menu
static so_hook nav_event_hook;
static int NavMessageClient_Event(void *self, void *cb, const char *name, void *arg) {
	l_info("[UI] event %s", name ? name : "(null)");
	return SO_CONTINUE(int, nav_event_hook, self, cb, name, arg);
}

// UX::NavMessageServer::NavActionHandler / UX::NavDispatcher::NavActionHandle
// (INavActionCallback*, void*, char const*, Types::Array*): Lua menu -> game
static so_hook nav_server_action_hook, nav_dispatch_action_hook;
static int NavMessageServer_NavActionHandler(void *cb, void *ctx, const char *action, void *args) {
	l_info("[UI] action (server) %s", action ? action : "(null)");
	return SO_CONTINUE(int, nav_server_action_hook, cb, ctx, action, args);
}
static int NavDispatcher_NavActionHandle(void *cb, void *ctx, const char *action, void *args) {
	l_info("[UI] action (dispatcher) %s", action ? action : "(null)");
	return SO_CONTINUE(int, nav_dispatch_action_hook, cb, ctx, action, args);
}

// UX::ViewLoader::LoadView(char const*, IViewLoadCallback*)
static so_hook load_view_hook;
static int ViewLoader_LoadView(void *self, const char *name, void *cb) {
	l_info("[UI] load view %s", name ? name : "(null)");
	perf_note_ui(name);
	return SO_CONTINUE(int, load_view_hook, self, name, cb);
}

// Operation::Complete(int result, void const* data)
static so_hook operation_complete_hook;
static void Operation_Complete(void *self, int result, const void *data) {
	// Log each distinct (operation, caller) pair a few times; libgame offsets
	// resolve to names with addr2line (vtable -> class, caller -> function)
	static struct { void *op; uintptr_t caller; int n; } seen[32];
	uintptr_t caller = (uintptr_t)__builtin_return_address(0);
	int i;
	for (i = 0; i < 32 && seen[i].op; i++)
		if (seen[i].op == self && seen[i].caller == caller)
			break;
	if (i < 32) {
		if (!seen[i].op) {
			seen[i].op = self;
			seen[i].caller = caller;
		}
		if (seen[i].n++ < 3) {
			uintptr_t vtbl = self ? *(uintptr_t *)self : 0;
			l_info("[OPERATION] %p (vtable game+0x%x) complete, result %d, from game+0x%x (#%d)",
			       self, vtbl - so_mod.text_base, result, caller - so_mod.text_base, seen[i].n);
		}
	}
	SO_CONTINUE(int, operation_complete_hook, self, result, data);
}

// The ghost save ([GHOSTSAVE]): SsxGhostsSaveGroup reads and writes the file
// in parts, its state at +124 (0 the table of contents, 1 one ghost's slot,
// 2 the 8-byte end marker, 3 done), the ghost's slot at +136 (-1: none).
// Each part starts with a CRC that IsDataValid checks; a failed check or
// operation error shows "the file may be corrupted".
#define GHOST_STATE(g) (*(int32_t *)((uint8_t *)(g) + 124))
#define GHOST_SLOT(g) (*(int32_t *)((uint8_t *)(g) + 136))

static int (*GhostIsDataValid_orig)(void *, const void *, const uint8_t *, uint32_t, uint32_t);
static int GhostIsDataValid(void *g, const void *info, const uint8_t *data, uint32_t size, uint32_t x) {
	int ok = GhostIsDataValid_orig(g, info, data, size, x);
	uint32_t stored = 0, version = 0;
	if (data && size >= 8) {
		memcpy(&stored, data, 4);
		memcpy(&version, data + 4, 4);
	}
	l_info("[GHOSTSAVE] check part %d (slot %d): %u bytes, CRC %08x, version %u: %s", GHOST_STATE(g), GHOST_SLOT(g),
	       size, stored, version, ok ? "valid" : "INVALID");
	return ok;
}

// SaveGroupFileAccessInfo: the context at +4, the file offset at +8, the size at +12
static void (*GhostGetFileAccessInfo_orig)(void *, uint32_t *);
static void GhostGetFileAccessInfo(void *g, uint32_t *info) {
	GhostGetFileAccessInfo_orig(g, info);
	l_info("[GHOSTSAVE] access (context %u) part %d (slot %d): offset %u, %u bytes", info[1], GHOST_STATE(g),
	       GHOST_SLOT(g), info[2], info[3]);
}

static void (*GhostPrepareId_orig)(void *, int, void *);
static void GhostPrepareId(void *g, int slot, void *handler) {
	l_info("[GHOSTSAVE] ghost slot %d (was %d), from game+0x%x", slot, GHOST_SLOT(g),
	       (unsigned)((uintptr_t)__builtin_return_address(0) - so_mod.text_base));
	GhostPrepareId_orig(g, slot, handler);
}

// SaveLoad::Operation::SetLastError(code): every save/load error
static void (*SetLastError_orig)(void *, int);
static void SetLastError(void *op, int code) {
	uintptr_t vtbl = op ? *(uintptr_t *)op : 0;
	l_info("[GHOSTSAVE] save operation (vtable game+0x%x) error %d, from game+0x%x", (unsigned)(vtbl - so_mod.text_base),
	       code, (unsigned)((uintptr_t)__builtin_return_address(0) - so_mod.text_base));
	SetLastError_orig(op, code);
}

void ssx_trace_patch(void) {
	if (SSX_DIAG) {
		// The ghost save's parts and errors, and every asset and audio file loaded
		GhostIsDataValid_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + 0x23d864, (uintptr_t)&GhostIsDataValid);
		GhostGetFileAccessInfo_orig =
		    (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + 0x23d99c, (uintptr_t)&GhostGetFileAccessInfo);
		GhostPrepareId_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + 0x23ddac, (uintptr_t)&GhostPrepareId);
		SetLastError_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + 0xcc2564, (uintptr_t)&SetLastError);
		if (!GhostIsDataValid_orig || !GhostGetFileAccessInfo_orig || !GhostPrepareId_orig || !SetLastError_orig)
			l_error("[GHOSTSAVE] hooks failed");
		unmanaged_load_hook = hook_addr(so_mod.text_base + 0x11dabe0, (uintptr_t)&FileManager_UnmanagedLoadFile);
		rawasset_get_hook = hook_addr(so_mod.text_base + 0x66a73c, (uintptr_t)&AssetStream_GetRawAsset);
	}
	bootflow_update_hook = hook_addr(so_mod.text_base + 0x2ede94, (uintptr_t)&BootFlowOp_Update);
	bootflow_error_hook = hook_addr(so_mod.text_base + 0x2ecdd8, (uintptr_t)&BootFlowOp_DoError);
	operation_complete_hook = hook_addr(so_mod.text_base + 0x23d498, (uintptr_t)&Operation_Complete);
	dac_create_hook = hook_addr(so_mod.text_base + 0x543808, (uintptr_t)&Dac_CreateInstance);
	dac_platform_init_hook = hook_addr(so_mod.text_base + 0x542434, (uintptr_t)&Dac_PlatformAudioInit);
	dac_resume_submit_hook = hook_addr(so_mod.text_base + 0x540b3c, (uintptr_t)&Dac_ResumeSubmitThread);
	dac_create_thread_hook = hook_addr(so_mod.text_base + 0x541f2c, (uintptr_t)&Dac_CreateDacThread);
	thread_begin_hook = hook_addr(so_mod.text_base + 0x4ade24, (uintptr_t)&EAThread_Begin);
	nav_action_hook = hook_addr(so_mod.text_base + 0x6e9aa8, (uintptr_t)&NavDispatcherGFx_ProcessNavAction);
	load_view_hook = hook_addr(so_mod.text_base + 0x705ca0, (uintptr_t)&ViewLoader_LoadView);
	nav_event_hook = hook_addr(so_mod.text_base + 0x6ff534, (uintptr_t)&NavMessageClient_Event);
	nav_server_action_hook = hook_addr(so_mod.text_base + 0x6fff30, (uintptr_t)&NavMessageServer_NavActionHandler);
	nav_dispatch_action_hook = hook_addr(so_mod.text_base + 0x6fde7c, (uintptr_t)&NavDispatcher_NavActionHandle);
}

#else
void ssx_trace_patch(void) {}
#endif
