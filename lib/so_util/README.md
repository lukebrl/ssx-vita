# so_util

ELF loader for running Android shared libraries (`.so`) on the PlayStation Vita. Originally written by [TheFloW](https://github.com/TheOfficialFloW). This repo extends his work with broader compatibility and additional features.

## What it does

Takes an ARM Android `.so`, loads it into Vita memory, applies relocations, resolves imports against a user-provided symbol table, runs init arrays, and hands back addresses to exported symbols. Handy function hooking and patching is available. Also supports loading several `.so` modules that depend on each other. 

Requires [kubridge](https://github.com/bythos14/kubridge) for RWX memory and cache flushing.

## Usage

```c
// 1. Load
so_module mod;
so_file_load(&mod, "ux0:data/libgame.so", LOAD_ADDRESS); // LOAD_ADDRESS is historically usually 0x98000000

// 2. Deal with imports
so_relocate(&mod);

static const so_default_dynlib dynlib[] = {
    { "malloc",  (uintptr_t)malloc },
    { "free",    (uintptr_t)free   },
    // ...
};
so_resolve(&mod, dynlib, sizeof(dynlib), 0);

// 3. Flush caches, run init arrays
so_flush_caches(&mod);
so_initialize(&mod);

// 4. Call into the library
int (* ExampleFun)(void *arg) = (void *)so_symbol(&mod, "ExampleFun");
ExampleFun(arg);
```

Use `so_mem_load` instead of `so_file_load` to load from a buffer already in memory.

Hooking and patching is done as follows:

```c
// Hook a function to a custom implementation via a named symbol
hook_addr((uintptr_t)so_symbol(&mod, "alc_opensles_init"), (uintptr_t)&ret0);

// Hook a function to a custom implementation via an address
hook_addr((uintptr_t) (LOAD_ADDRESS + 0x1243), (uintptr_t)&ret0); // 0x1243 would correspond to the address you see in reverse engineering tools

// Hook a function to a custom implementation while still making use of the original
so_hook sample_hook;
int sampleFuncExtended(int param) {
	sceClibPrintf("sampleFuncExtended called! Custom code is running.\n");
	sceClibPrintf("Now let's run the original with modified params:\n");
	int ret = SO_CONTINUE(int, sample_hook, param + 1);
	sceClibPrintf("The original function body has been executed and returned 0x%x\n", ret);
	return ret + 10;
}

sample_hook = hook_addr((uintptr_t)so_symbol(&mod, "sampleFunc"), (uintptr_t)&sampleFuncExtended);
```

**Important:** don't forget to flush caches after applying patches.

## API

| Function | Description |
|---|---|
| `so_file_load(mod, path, addr)` | Load `.so` from filesystem at `addr` |
| `so_mem_load(mod, buf, size, addr)` | Load `.so` from memory buffer at `addr` |
| `so_relocate(mod)` | Apply ELF relocations |
| `so_resolve(mod, dynlib, size, only)` | Resolve imports; unresolved symbols get an error stub |
| `so_resolve_with_dummy(...)` | Same, but unresolved symbols silently return 0 |
| `so_initialize(mod)` | Call init array functions |
| `so_flush_caches(mod)` | Flush caches after patching |
| `so_symbol(mod, name)` | Look up an exported symbol address |
| `hook_addr(addr, dst)` | Hook ARM or Thumb function (auto-detected) |
| `hook_arm(addr, dst)` | Hook ARM function |
| `hook_thumb(addr, dst)` | Hook Thumb function |
| `so_symbol_fix_ldmia(mod, sym)` | Patch misaligned LDMIA instructions in a symbol |
| `SO_CONTINUE(type, hook, ...)` | Call the original function from within a hook |

Refer to the source code for more detailed description of the API.

## Credits

- [TheFloW](https://github.com/TheOfficialFloW) — original implementation
- [Rinnegatamante](https://github.com/Rinnegatamante)

## License

MIT — see [LICENSE](LICENSE).
