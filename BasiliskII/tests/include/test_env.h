/*
 * test_env.h - Shared Macintosh memory + CPU-engine setup for Cockatrice tests
 *
 * Every test binary links this instead of duplicating RAM/ROM sizing, RESET
 * vector planting, and SetActiveCPUEngine()/init() sequencing.
 */

#ifndef TEST_ENV_H
#define TEST_ENV_H

#include "sysdeps.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int g_pass;
extern int g_fail;

#ifdef __cplusplus
}

struct TestEngineConfig {
	const char *id;      /* "musashi", "syn68k", "uae", "emu68" */
	bool jit;
	bool jitfpu;
	const char *label;   /* printed in CHECK messages, e.g. "uae+jit" */
};

extern const TestEngineConfig kTestEngineConfigs[];
extern const int kTestEngineConfigCount;

/*
 * True when --engine FILTER should run this config.
 *
 * Arguments:
 *   filter: NULL/empty means all engines; otherwise matches id or label.
 *   cfg: Engine config from kTestEngineConfigs.
 */
bool test_engine_matches(const char *filter, const TestEngineConfig *cfg);

/*
 * Switches the active 680x0 engine and re-inits so interpreter vs JIT tables match.
 *
 * SetActiveCPUEngine() only updates the dispatch pointer; UAE JIT vs interpreter
 * is chosen inside engine->init() from UseJIT / JITCacheSize.
 *
 * Arguments:
 *   id: Engine identifier ("musashi", "syn68k", "uae", or "emu68").
 *   jit: True to enable UseJIT (Musashi and syn68k ignore this).
 *   jitfpu: True to enable UseJITFPU when JIT is enabled.
 *
 * Returns:
 *   true if the engine was selected and init() succeeded.
 */
bool activate_cpu_engine(const char *id, bool jit, bool jitfpu = false);

/*
 * Locates dist/Quadra800.rom (or QUADRA_ROM / TEST_REPO_ROOT).
 *
 * Returns:
 *   Heap-allocated path string the caller must free, or NULL if missing.
 */
char *test_find_quadra_rom(void);

/*
 * Loads a Quadra 800 ROM image into ROMBaseHost.
 *
 * Arguments:
 *   path: Filesystem path, or NULL to use test_find_quadra_rom().
 *
 * Returns:
 *   Number of bytes loaded, or 0 on failure (ROM-dependent tests should skip).
 */
size_t test_load_quadra_rom(const char *path);

/*
 * Injects a prefs string used by DiskInit / PatchROM (name may repeat).
 */
void test_prefs_clear(void);
void test_prefs_add(const char *name, const char *value);
void test_prefs_set_bool(const char *name, bool value);
void test_prefs_set_int32(const char *name, int32 value);

/*
 * Trap stubs for DiskOpen without a Mac heap: NewPtrSysClear bump-allocates
 * from a RAM arena; AddDrive is a no-op. Call after activate_cpu_engine().
 *
 * Arguments:
 *   heap_addr: Macintosh address of a zeroed RAM arena.
 *   heap_size: Arena size in bytes.
 */
void test_install_disk_trap_stubs(uint32 heap_addr, uint32 heap_size);

#endif /* __cplusplus */

#endif /* TEST_ENV_H */
