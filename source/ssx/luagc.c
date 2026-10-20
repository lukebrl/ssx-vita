/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  luagc.c
 * @brief Full Lua garbage collections in the UI.
 *
 * The UI's view models and navigation run in Lua states. Full collections
 * (lua_gc(LUA_GCCOLLECT)) walk a whole state; on the Vita they took most of
 * the second results-screen frame (7 in one frame). Two sources:
 *   - common/fe/lua/packages/nav.lua collects after every navigation event:
 *     the data tool (datatool/ssxdata/patches.py) makes it collect nothing.
 *   - luavm::LuaStateManager::ReleaseLuaState collects right before
 *     lua_close, which frees everything and runs the finalizers anyway:
 *     that call is removed here.
 * Debug builds log the full collections still made, with where from.
 */

#include "ssx/luagc.h"

#include <stdint.h>

#include <kubridge.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define OFF_ReleaseLuaState_gc 0xe7c5f0 // luavm::LuaStateManager::ReleaseLuaState: bl lua_gc(L, LUA_GCCOLLECT, 0)
#define OFF_lua_gc 0xe92544
#define ARM_NOP 0xe1a00000 // mov r0, r0
#define LUA_GCCOLLECT 2

#ifdef DEBUG_SOLOADER
static int (*lua_gc_orig)(void *, int, int);

static int lua_gc_hook(void *L, int what, int data) {
    if (what != LUA_GCCOLLECT)
        return lua_gc_orig(L, what, data);
    uint32_t caller = (uint32_t)__builtin_return_address(0) - so_mod.text_base;
    uint32_t t0 = sceKernelGetProcessTimeLow();
    int r = lua_gc_orig(L, what, data);
    l_info("[LUAGC] full collection: %u ms, from game+0x%X", (sceKernelGetProcessTimeLow() - t0) / 1000, caller);
    return r;
}
#endif

void ssx_luagc_patch(void) {
    uint32_t *site = (uint32_t *)(so_mod.text_base + OFF_ReleaseLuaState_gc);
    if ((*site & 0xff000000) == 0xeb000000) { // a bl, as expected
        uint32_t nop = ARM_NOP;
        kuKernelCpuUnrestrictedMemcpy(site, &nop, sizeof(nop));
        kuKernelFlushCaches(site, sizeof(nop));
    } else {
        l_error("[LUAGC] unexpected code at ReleaseLuaState: 0x%08X", *site);
    }
#ifdef DEBUG_SOLOADER
    lua_gc_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_lua_gc, (uintptr_t)lua_gc_hook);
    if (!lua_gc_orig)
        l_error("[LUAGC] could not hook lua_gc");
#endif
}
