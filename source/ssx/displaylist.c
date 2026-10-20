/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  displaylist.c
 * @brief Scaleform's child-by-name lookup without calls and refcount churn.
 *
 * Every ActionScript access to a child clip by name (this.button.label...)
 * ends in DisplayList::GetDisplayObjectByName, a linear scan of the parent's
 * children: for each child it called DisplayObject::GetName(), which hands
 * back the name with a reference taken, then dropped it. UI screens have
 * parents with hundreds of children: AvmSprite::GetMember took ~14 us per
 * call on the main menu.
 *
 * This is the same scan with the name read in place: the CharacterHandle at
 * +68 holds it at +8, and a name's lowercase twin (case-insensitive SWFs) is
 * at +8 of the name. A child without a handle yet, or a name whose lowercase
 * twin isn't made yet, goes to the original function, which makes them as
 * GetName() does: the result is the same in every case.
 */

#include "ssx/displaylist.h"

#include <stdint.h>

#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define OFF_GetDisplayObjectByName 0xac30f8 // DisplayList::GetDisplayObjectByName(ASString const&, bool) const

// DisplayList: entries (12 bytes, the object first) +0, count +4, last found +16
#define DL_ENTRIES(dl) (*(uint8_t **)((uint8_t *)(dl) + 0))
#define DL_COUNT(dl) (*(uint32_t *)((uint8_t *)(dl) + 4))
#define DL_LAST(dl) (*(void **)((uint8_t *)(dl) + 16))
#define ENTRY_SIZE 12
// DisplayObject: flags (16 bits) +62, CharacterHandle +68
#define OBJ_FLAGS(o) (*(uint16_t *)((uint8_t *)(o) + 62))
#define OBJ_HANDLE(o) (*(uint8_t **)((uint8_t *)(o) + 68))
#define FLAG_NAMED 0x100
// CharacterHandle: name (ASStringNode*) +8. ASStringNode: lowercase twin +8, size +20
#define HANDLE_NAME(h) (*(uint8_t **)((h) + 8))
#define NODE_LOWER(n) (*(uint8_t **)((n) + 8))
#define NODE_SIZE(n) (*(uint32_t *)((n) + 20))

typedef void *(*ByNameFn)(void *dl, uint8_t **name, int case_sensitive);
static ByNameFn ByName_orig;

#ifdef DEBUG_SOLOADER
static volatile uint32_t lookups, scanned, slow;
#endif

static void *call_orig(void *dl, uint8_t **name, int case_sensitive) {
#ifdef DEBUG_SOLOADER
    slow++;
#endif
    return ByName_orig(dl, name, case_sensitive);
}

static void *GetDisplayObjectByName_hook(void *dl, uint8_t **name, int case_sensitive) {
    uint8_t *node = *name;
    if (!NODE_SIZE(node))
        return NULL; // no object has an empty name
    uint8_t *want = node;
    if (!case_sensitive) {
        want = NODE_LOWER(node);
        if (!want)
            return call_orig(dl, name, case_sensitive);
    }
#ifdef DEBUG_SOLOADER
    lookups++;
#endif
    // The last one found first, as the original does
    void *last = DL_LAST(dl);
    if (last) {
        uint8_t *h = OBJ_HANDLE(last);
        if (!h)
            return call_orig(dl, name, case_sensitive);
        uint8_t *n = HANDLE_NAME(h);
        if (!case_sensitive) {
            n = NODE_LOWER(n);
            if (!n)
                return call_orig(dl, name, case_sensitive);
        }
        if (n == want)
            return last;
    }
    uint8_t *e = DL_ENTRIES(dl);
    uint32_t count = DL_COUNT(dl);
    for (uint32_t i = 0; i < count; i++, e += ENTRY_SIZE) {
        void *obj = *(void **)e;
        if (!obj || !(OBJ_FLAGS(obj) & FLAG_NAMED))
            continue;
        uint8_t *h = OBJ_HANDLE(obj);
        if (!h)
            return call_orig(dl, name, case_sensitive);
        uint8_t *n = HANDLE_NAME(h);
        if (!case_sensitive) {
            n = NODE_LOWER(n);
            if (!n)
                return call_orig(dl, name, case_sensitive);
        }
#ifdef DEBUG_SOLOADER
        scanned++;
#endif
        if (n == want) {
            DL_LAST(dl) = obj;
            return obj;
        }
    }
    DL_LAST(dl) = NULL;
    return NULL;
}

void ssx_displaylist_patch(void) {
    ByName_orig = (ByNameFn)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_GetDisplayObjectByName,
                                                (uintptr_t)GetDisplayObjectByName_hook);
    if (!ByName_orig)
        l_error("[DISPLAYLIST] could not hook DisplayList::GetDisplayObjectByName");
}

void ssx_displaylist_report(void) {
#ifdef DEBUG_SOLOADER
    if (lookups || slow)
        l_info("[PERF] child lookups by name: %u (%u children compared), %u passed to the original", lookups, scanned,
               slow);
    lookups = scanned = slow = 0;
#endif
}
