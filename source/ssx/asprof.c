/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  asprof.c
 * @brief Which ActionScript functions the menus spend their freezes in (debug aid).
 *
 * The long logic frames on UI screens are Scaleform's AS2 interpreter running
 * the screens' scripts. Every call of a script function goes through
 * AS2::AsFunctionObject::Invoke, which runs ActionBuffer::Execute on the
 * function's own bytecode: the ActionBuffer at +60 of the function object,
 * from +76 (start) for +80 bytes. That pair identifies the function; the
 * ActionBuffer's data (+8) holds the bytecode (+8) and its size (+12).
 *
 * Calls on the logic thread are timed (own time: minus the script functions
 * they call) and grouped per function, per logic frame; perf.c has the frames
 * over its hitch threshold logged with their top functions, by file and
 * bytecode offset and the bytes around it, to find them in the .gfx files.
 */

#include "ssx/asprof.h"

#include <stdint.h>
#include <string.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#if defined(DEBUG_SOLOADER) && SSX_ASPROF

#define OFF_AsFunctionObject_Invoke 0x8ea39c // AS2::AsFunctionObject::Invoke(FnCall const&, LocalFrame*, char const*)
#define FN_BUFFER(f) (*(uint8_t **)((uint8_t *)(f) + 60))
#define FN_START(f) (*(uint32_t *)((uint8_t *)(f) + 76))
#define FN_LEN(f) (*(uint32_t *)((uint8_t *)(f) + 80))
#define BUF_DATA(b) (*(uint8_t **)((b) + 8))

#define NFUNCS 1024 // power of two
#define MAX_DEPTH 256

typedef uint64_t (*InvokeFn)(void *, void *, void *, const char *);
static InvokeFn Invoke_orig;
static volatile SceUID sim;

static struct {
    void *buf;
    uint32_t start, len, calls;
    uint64_t self_us, total_us;
} funcs[NFUNCS];

static struct {
    uint32_t t0, child;
} stack[MAX_DEPTH];
static int depth;

void asprof_set_thread(SceUID uid) {
    sim = uid;
}

static int slot_of(void *buf, uint32_t start, uint32_t len) {
    uint32_t h = ((((uintptr_t)buf >> 4) * 2654435761u) ^ (start * 40503u)) & (NFUNCS - 1);
    for (int i = 0; i < 16; i++, h = (h + 1) & (NFUNCS - 1)) {
        if (funcs[h].buf == buf && funcs[h].start == start)
            return h;
        if (!funcs[h].buf) {
            funcs[h].buf = buf;
            funcs[h].start = start;
            funcs[h].len = len;
            return h;
        }
    }
    return -1;
}

static uint64_t Invoke_hook(void *fn, void *call, void *frame, const char *name) {
    if (!sim || sceKernelGetThreadId() != sim || depth >= MAX_DEPTH)
        return Invoke_orig(fn, call, frame, name);
    void *buf = FN_BUFFER(fn);
    uint32_t start = FN_START(fn), len = FN_LEN(fn);
    int d = depth++;
    stack[d].t0 = sceKernelGetProcessTimeLow();
    stack[d].child = 0;
    uint64_t r = Invoke_orig(fn, call, frame, name);
    uint32_t dt = sceKernelGetProcessTimeLow() - stack[d].t0;
    depth = d;
    if (d > 0)
        stack[d - 1].child += dt;
    if (buf) {
        int s = slot_of(buf, start, len);
        if (s >= 0) {
            funcs[s].calls++;
            funcs[s].total_us += dt;
            funcs[s].self_us += dt - stack[d].child;
        }
    }
    return r;
}

// AvmSprite::GetMember, per member name: which reads of a clip's members
// cost the time (the name is an ASStringNode, its text at +0)
#define OFF_AvmSprite_GetMember 0x8c1c50 // AS2::AvmSprite::GetMember(Environment*, ASStringContext*, ASString const&, Value*)
#define NMEMBERS 256 // power of two
typedef int (*GetMemberFn)(void *, void *, void *, void **, void *);
static GetMemberFn GetMember_orig;
static struct {
    void *node;
    uint32_t calls, us;
} members[NMEMBERS];
static int member_depth;

static int GetMember_hook(void *self, void *env, void *ctx, void **name, void *val) {
    if (!sim || sceKernelGetThreadId() != sim || member_depth)
        return GetMember_orig(self, env, ctx, name, val);
    member_depth++;
    uint32_t t0 = sceKernelGetProcessTimeLow();
    int r = GetMember_orig(self, env, ctx, name, val);
    uint32_t dt = sceKernelGetProcessTimeLow() - t0;
    member_depth--;
    void *node = name ? *name : NULL;
    uint32_t h = (((uintptr_t)node >> 3) * 2654435761u) >> 24; // 8 bits
    for (int i = 0; i < 8; i++, h = (h + 1) & (NMEMBERS - 1)) {
        if (members[h].node == node || !members[h].node) {
            members[h].node = node;
            members[h].calls++;
            members[h].us += dt;
            break;
        }
    }
    return r;
}

static void members_report(uint32_t prev_frame_ms, const char *ui) {
    char line[1536];
    int len = sceClibSnprintf(line, sizeof(line), "[ASPROF] %u ms logic frame on %s: clip members read, by time "
                              "(ms, calls, name):", prev_frame_ms, ui);
    static uint8_t taken[NMEMBERS];
    memset(taken, 0, sizeof(taken));
    for (int k = 0; k < 16 && len < (int)sizeof(line) - 80; k++) {
        int best = -1;
        for (int i = 0; i < NMEMBERS; i++)
            if (members[i].calls && !taken[i] && (best < 0 || members[i].us > members[best].us))
                best = i;
        if (best < 0)
            break;
        taken[best] = 1;
        const char *text = members[best].node ? *(const char **)members[best].node : NULL;
        len += sceClibSnprintf(line + len, sizeof(line) - len, " %.1f %u %.40s;", members[best].us / 1000.0f,
                               members[best].calls, text ? text : "?");
    }
    l_info("%s", line);
}

void ssx_asprof_patch(void) {
    GetMember_orig = (GetMemberFn)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_AvmSprite_GetMember,
                                                      (uintptr_t)GetMember_hook);
    if (!GetMember_orig)
        l_error("[ASPROF] could not hook AvmSprite::GetMember");
    Invoke_orig = (InvokeFn)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_AsFunctionObject_Invoke,
                                                (uintptr_t)Invoke_hook);
    if (!Invoke_orig)
        l_error("[ASPROF] could not hook AsFunctionObject::Invoke");
}

// Called on the logic thread at the start of each logic frame
void asprof_frame(uint32_t prev_frame_ms, const char *ui, int report) {
    if (report) {
        int top[12];
        int n = 0;
        uint64_t all = 0;
        static uint8_t taken[NFUNCS];
        memset(taken, 0, sizeof(taken));
        for (int i = 0; i < NFUNCS; i++)
            all += funcs[i].self_us;
        for (; n < 12; n++) {
            int best = -1;
            for (int i = 0; i < NFUNCS; i++)
                if (funcs[i].calls && !taken[i] && (best < 0 || funcs[i].self_us > funcs[best].self_us))
                    best = i;
            if (best < 0)
                break;
            taken[best] = 1;
            top[n] = best;
        }
        char line[4096];
        int len = sceClibSnprintf(line, sizeof(line),
                                  "[ASPROF] %u ms logic frame on %s: %.0f ms in script functions; top by own time "
                                  "(ms own/total, calls, @start+length/action buffer size, bytes around the start):",
                                  prev_frame_ms, ui, all / 1000.0f);
        for (int k = 0; k < n && len < (int)sizeof(line) - 200; k++) {
            int i = top[k];
            uint8_t *data = BUF_DATA((uint8_t *)funcs[i].buf);
            const uint8_t *code = data ? *(const uint8_t **)(data + 8) : NULL;
            uint32_t bufsize = data ? *(const uint32_t *)(data + 12) : 0;
            len += sceClibSnprintf(line + len, sizeof(line) - len, " %.1f/%.1f %u @%u+%u/%u", funcs[i].self_us / 1000.0f,
                                   funcs[i].total_us / 1000.0f, funcs[i].calls, funcs[i].start, funcs[i].len, bufsize);
            // The bytes around the start (the function's header is just before it): to find it in the .gfx files
            if (code && k < 8 && funcs[i].start < bufsize) {
                uint32_t from = funcs[i].start > 32 ? funcs[i].start - 32 : 0;
                uint32_t to = funcs[i].start + 24 < bufsize ? funcs[i].start + 24 : bufsize;
                len += sceClibSnprintf(line + len, sizeof(line) - len, " [");
                for (uint32_t b = from; b < to && len < (int)sizeof(line) - 8; b++)
                    len += sceClibSnprintf(line + len, sizeof(line) - len, b == funcs[i].start ? "|%02X" : "%02X", code[b]);
                len += sceClibSnprintf(line + len, sizeof(line) - len, "]");
            }
            len += sceClibSnprintf(line + len, sizeof(line) - len, ";");
        }
        l_info("%s", line);
        members_report(prev_frame_ms, ui);
    }
    memset(funcs, 0, sizeof(funcs));
    memset(members, 0, sizeof(members));
}

#else
void ssx_asprof_patch(void) {}
void asprof_set_thread(SceUID uid) { (void)uid; }
void asprof_frame(uint32_t prev_frame_ms, const char *ui, int report) {
    (void)prev_frame_ms;
    (void)ui;
    (void)report;
}
#endif
