/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  shaderbind.c
 * @brief RNAX::OGLESProgram::Bind without a string compare per parameter.
 *
 * Every material bind binds its program: glUseProgram, then the 17 vertex
 * attribute parameters (coordinate, normal, texCoord0-7...) are looked up
 * and reset, then each of the program's parameters is compared by name with
 * "OGLESRnaAttribute" (Stricmp) to tell the vertex attributes, whose
 * location is set, from the rest, which is marked for upload. In a race
 * every terrain chunk has its own material: 144 binds a frame on the big
 * maps, ~28 us of the function's own time each.
 *
 * The function is replaced by the same steps in C, except that the compare
 * is done once per parameter type: its name is a string of the type
 * descriptor, which doesn't change. The 17 attribute lookups are kept from
 * one bind to the next while no scope changes, including the ones that find
 * nothing (FindParm searches those in full every time).
 */

#include "ssx/shaderbind.h"
#include "ssx/scopecache.h"
#include "ssx/perf.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>

#include <kubridge.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

// libgame.so v0.0.8430
#define OFF_OGLESProgram_Bind 0xd83bb0 // RNAX::OGLESProgram::Bind()
#define OFF_ScopeC_FindParm 0xdb60cc   // RNA::ScopeC::FindParm(char const*, TypeID, char const*, unsigned) const
#define OFF_AttributeNames 0x16fa440   // the 17 attribute names Bind looks up (relocated pointers)
#define OFF_AttributeType 0x15d2898    // "OGLESRnaAttribute": block name and parameter type name
#define NATTRIBUTES 17
#define TYPE_ATTRIBUTE 4
#define GLES_USE_PROGRAM 132 // EA::Graphics::OpenGLES20 vtable slot (byte offset 528)

// OGLESProgram: parameter entries (56 bytes: parameter, location) at +0, their
// count at +8, the device at +16 (its scope at +296), the GL wrapper at +20,
// the GL program at +24
#define PROG_ENTRIES(p) (*(uint8_t **)((uint8_t *)(p) + 0))
#define PROG_NENTRIES(p) (*(int32_t *)((uint8_t *)(p) + 8))
#define PROG_DEVICE(p) (*(uint8_t **)((uint8_t *)(p) + 16))
#define PROG_GL(p) (*(void **)((uint8_t *)(p) + 20))
#define PROG_ID(p) (*(uint32_t *)((uint8_t *)(p) + 24))
#define ENTRY_SIZE 56
// ScopeParmC: owner (dirty mask at +1864) +56, its dirty bits +64 (64-bit),
// type +60 (name at +68), value pointer +76, storage +80, dirty flag +88
#define PARM_OWNER(q) (*(uint8_t **)((uint8_t *)(q) + 56))
#define PARM_MASK(q) ((uint32_t *)((uint8_t *)(q) + 64))
#define PARM_TYPE(q) (*(uint8_t **)((uint8_t *)(q) + 60))
#define PARM_VALUE(q) (*(int32_t **)((uint8_t *)(q) + 76))
#define PARM_STORAGE(q) (*(int32_t **)((uint8_t *)(q) + 80))
#define PARM_DIRTY(q) (*(uint8_t *)((uint8_t *)(q) + 88))
#define TYPE_NAME(t) (*(const char **)((t) + 68))
#define OWNER_DIRTY(o) ((uint32_t *)((o) + 1864))

typedef void *(*FindParmFn)(const void *scope, const char *block, uint32_t type, const char *parm, uint32_t index);
typedef void (*UseProgramFn)(void *gl, uint32_t program);

static FindParmFn FindParm;
static const char *const *attribute_names;
static const char *attribute_type;
volatile uint32_t ssx_shaderbind_attr_null; // attribute lookups that found nothing (debug report)
#ifdef DEBUG_SOLOADER
// The game's own steps, for the A/B test (perf.h)
#define OFF_Stricmp 0x4c9860                    // EA::StdC::Stricmp(char const*, char const*)
#define OFF_TexturePlatC_SetStateValue 0xdb83a4 // RNA::TexturePlatC::SetStateValue(SSStateE, int)
static int (*Stricmp)(const char *, const char *);
static void (*SetStateValue)(void *tex, int state, int value);
#endif

// Whether a parameter type is the vertex attribute type, by its name pointer
#define TYPE_CACHE 64
static struct {
    const char *name;
    uint8_t is_attribute;
} types[TYPE_CACHE];

static int is_attribute(const char *name) {
    uint32_t slot = ((uintptr_t)name >> 3) & (TYPE_CACHE - 1);
    if (types[slot].name == name)
        return types[slot].is_attribute;
    int r = name && strcasecmp(name, attribute_type) == 0;
    types[slot].is_attribute = r;
    types[slot].name = name;
    return r;
}

// Sets a parameter's value and marks it (and its owner) for upload, as Bind does
static inline void set_value(uint8_t *q, int32_t value) {
    int32_t *v = PARM_VALUE(q);
    if (v && *v == value)
        return;
    PARM_VALUE(q) = PARM_STORAGE(q);
    *PARM_STORAGE(q) = value;
    PARM_DIRTY(q) = 1;
    uint32_t *dirty = OWNER_DIRTY(PARM_OWNER(q));
    dirty[0] |= PARM_MASK(q)[0];
    dirty[1] |= PARM_MASK(q)[1];
}

// The 17 attribute parameters of the scope, found or not, while no scope
// changed (scopecache's generation). FindParm doesn't cache a search that
// finds nothing, and most programs' scopes lack some of the 17: ~200 full
// searches a frame in a race. The ones not found are looked up again every
// NULL_RECHECK binds all the same.
#define NULL_RECHECK 256
static struct {
    const void *scope;
    uint32_t gen, binds;
    uint8_t *parm[NATTRIBUTES];
} attrs;

static uint8_t *const *attribute_parms(const void *scope) {
    uint32_t gen = ssx_ab_off ? 0 : ssx_scopecache_generation();
    if (gen && attrs.gen == gen && attrs.scope == scope && ++attrs.binds < NULL_RECHECK)
        return attrs.parm;
    int missing = 0;
    for (int i = 0; i < NATTRIBUTES; i++) {
        attrs.parm[i] = FindParm(scope, attribute_type, TYPE_ATTRIBUTE, attribute_names[i], 1);
        missing += !attrs.parm[i];
    }
    ssx_shaderbind_attr_null += missing;
    // Kept only if no scope changed meanwhile
    attrs.gen = gen && ssx_scopecache_generation() == gen ? gen : 0;
    attrs.scope = scope;
    attrs.binds = 0;
    return attrs.parm;
}

static void Bind(void *prog) {
    void *gl = PROG_GL(prog);
    ((UseProgramFn)(*(void ***)gl)[GLES_USE_PROGRAM])(gl, PROG_ID(prog));

    // Attributes the program doesn't use read -1
    uint8_t *const *parms = attribute_parms(PROG_DEVICE(prog) + 296);
    for (int i = 0; i < NATTRIBUTES; i++)
        if (parms[i])
            set_value(parms[i], -1);

    // Its attributes read their location; its other parameters are uploaded
    uint8_t *e = PROG_ENTRIES(prog);
    for (int i = 0, n = PROG_NENTRIES(prog); i < n; i++, e += ENTRY_SIZE) {
        uint8_t *q = *(uint8_t **)e;
#ifdef DEBUG_SOLOADER
        int attr = ssx_ab_off ? Stricmp(TYPE_NAME(PARM_TYPE(q)), attribute_type) == 0 : is_attribute(TYPE_NAME(PARM_TYPE(q)));
#else
        int attr = is_attribute(TYPE_NAME(PARM_TYPE(q)));
#endif
        if (!attr)
            PARM_DIRTY(q) = 1;
        else
            set_value(q, *(int32_t *)(e + 4));
    }
}

// --- SendParmsToGPU ------------------------------------------------------------
//
// After each bind, the program's parameters marked dirty are uploaded: all
// of them but the vertex attributes, since Bind marks them all. The game
// compares each one's type name with "OGLESRnaAttribute" (Stricmp) on the
// way; here the cached is_attribute does. Same calls, same order otherwise.

#define OFF_OGLESProgram_SendParmsToGPU 0xd8d45c // RNAX::OGLESProgram::SendParmsToGPU(bool)
#define OFF_TexturePlatC_Bind 0xdbbbb8           // RNA::TexturePlatC::Bind()
// ScopeParmC: kind +28, element count +52
#define PARM_KIND(q) (*(uint32_t *)((uint8_t *)(q) + 28))
#define PARM_COUNT(q) (*(int32_t *)((uint8_t *)(q) + 52))
// Entry: location +4, then the 12 sampler states of a texture parameter
#define ENTRY_LOCATION(e) (*(int32_t *)((e) + 4))
#define ENTRY_STATES(e) ((const int32_t *)((e) + 8))
#define NSTATES 12
// TexturePlatC: sampler states at +72 (SetStateValue), their dirty bits +68
#define TEX_STATES(t) ((int32_t *)((uint8_t *)(t) + 72))
#define TEX_STATE_DIRTY(t) (*(uint32_t *)((uint8_t *)(t) + 68))
// EA::Graphics::OpenGLES20 vtable slots (byte offset / 4)
#define GLES_ACTIVE_TEXTURE 2 // 8
#define GLES_UNIFORM1F 113    // 452
#define GLES_UNIFORM1I 115    // 460
#define GLES_UNIFORM_A 118    // 472 (kind 7)
#define GLES_UNIFORM_B 122    // 488 (kind 8)
#define GLES_UNIFORM4FV 126   // 504 (kinds 9 and 11)
#define GL_TEXTURE0 0x84C0

enum { KIND_FLOAT = 3, KIND_INT, KIND_5, KIND_BOOL, KIND_7, KIND_8, KIND_9, KIND_10, KIND_MATRIX, KIND_TEXTURE };

typedef void (*GlI2Fn)(void *gl, int32_t a, int32_t b);
typedef void (*GlArrayFn)(void *gl, int32_t loc, int32_t count, const void *data);
typedef void (*TexBindFn)(void *tex);
static TexBindFn TexturePlatC_Bind;

static void SendParmsToGPU(void *prog, int transpose) {
    void *gl = PROG_GL(prog);
    void **vt = *(void ***)gl;
    int unit = 0;
    uint8_t *e = PROG_ENTRIES(prog);
    for (int i = 0, n = PROG_NENTRIES(prog); i < n; i++, e += ENTRY_SIZE) {
        uint8_t *q = *(uint8_t **)e;
        if (!PARM_DIRTY(q))
            continue;
#ifdef DEBUG_SOLOADER
        if (ssx_ab_off ? Stricmp(TYPE_NAME(PARM_TYPE(q)), attribute_type) == 0 : is_attribute(TYPE_NAME(PARM_TYPE(q))))
            continue; // A/B test: the game's compare when off
#else
        if (is_attribute(TYPE_NAME(PARM_TYPE(q))))
            continue; // attributes stay marked
#endif
        int32_t loc = ENTRY_LOCATION(e);
        const int32_t *v = PARM_VALUE(q);
        switch (PARM_KIND(q)) {
            case KIND_FLOAT:
                ((GlI2Fn)vt[GLES_UNIFORM1F])(gl, loc, *v); // the float's bits (softfp)
                break;
            case KIND_INT:
                ((GlI2Fn)vt[GLES_UNIFORM1I])(gl, loc, *v);
                break;
            case KIND_BOOL:
                ((GlI2Fn)vt[GLES_UNIFORM1I])(gl, loc, *(const uint8_t *)v);
                break;
            case KIND_7:
                ((GlArrayFn)vt[GLES_UNIFORM_A])(gl, loc, PARM_COUNT(q), v);
                break;
            case KIND_8:
                ((GlArrayFn)vt[GLES_UNIFORM_B])(gl, loc, PARM_COUNT(q), v);
                break;
            case KIND_9:
                ((GlArrayFn)vt[GLES_UNIFORM4FV])(gl, loc, PARM_COUNT(q), v);
                break;
            case KIND_MATRIX:
                if (transpose) {
                    // The game transposes the first matrix only (count 1 in practice)
                    float m[16];
                    const float *s = (const float *)v;
                    for (int r = 0; r < 4; r++)
                        for (int c = 0; c < 4; c++)
                            m[r * 4 + c] = s[c * 4 + r];
                    ((GlArrayFn)vt[GLES_UNIFORM4FV])(gl, loc, PARM_COUNT(q) * 4, m);
                } else {
                    ((GlArrayFn)vt[GLES_UNIFORM4FV])(gl, loc, PARM_COUNT(q) * 4, v);
                }
                break;
            case KIND_TEXTURE: {
                void *tex = *(void *const *)v;
                if (!tex)
                    break;
                ((GlI2Fn)vt[GLES_UNIFORM1I])(gl, loc, unit);
                // TexturePlatC::SetStateValue for the 12 sampler states
                const int32_t *states = ENTRY_STATES(e);
                int32_t *cur = TEX_STATES(tex);
#ifdef DEBUG_SOLOADER
                if (ssx_ab_off) {
                    for (int k = 0; k < NSTATES; k++)
                        SetStateValue(tex, k, states[k]);
                } else
#endif
                for (int k = 0; k < NSTATES; k++)
                    if (cur[k] != states[k]) {
                        TEX_STATE_DIRTY(tex) |= 1u << k;
                        cur[k] = states[k];
                    }
                ((void (*)(void *, uint32_t))vt[GLES_ACTIVE_TEXTURE])(gl, GL_TEXTURE0 + unit);
                unit++;
                TexturePlatC_Bind(tex);
                break;
            }
            default:
                break;
        }
        PARM_DIRTY(q) = 0;
    }
}

void ssx_shaderbind_patch(void) {
    uintptr_t base = so_mod.text_base;
    FindParm = (FindParmFn)(base + OFF_ScopeC_FindParm);
    attribute_names = (const char *const *)(base + OFF_AttributeNames);
    attribute_type = (const char *)(base + OFF_AttributeType);
    if (strcmp(attribute_type, "OGLESRnaAttribute") != 0 || strcmp(attribute_names[0], "coordinate") != 0 ||
        strcmp(attribute_names[NATTRIBUTES - 1], "BLENDINDICES") != 0) {
        l_error("[SHADERBIND] unexpected data, OGLESProgram::Bind left as is");
        return;
    }
    hook_addr(base + OFF_OGLESProgram_Bind, (uintptr_t)&Bind);
    TexturePlatC_Bind = (TexBindFn)(base + OFF_TexturePlatC_Bind);
#ifdef DEBUG_SOLOADER
    Stricmp = (void *)(base + OFF_Stricmp);
    SetStateValue = (void *)(base + OFF_TexturePlatC_SetStateValue);
#endif
    hook_addr(base + OFF_OGLESProgram_SendParmsToGPU, (uintptr_t)&SendParmsToGPU);
    l_info("[SHADERBIND] OGLESProgram::Bind and SendParmsToGPU without a string compare per parameter");
}
