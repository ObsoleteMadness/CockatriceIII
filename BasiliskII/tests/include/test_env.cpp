/*
 * test_env.cpp - Memory/engine activation, ROM discovery, and Line-A trap stubs
 */

#include "test_env.h"
#include "test_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <string>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "main.h"
#include "macos_util.h"
#include "rom_patches.h"
#include "prefs.h"

extern "C" {
int g_pass = 0;
int g_fail = 0;
}

const TestEngineConfig kTestEngineConfigs[] = {
	{ "musashi", false, false, "musashi" },
	{ "m68k_rs", false, false, "m68k_rs" },
	{ "uae",     false, false, "uae" },
	{ "uae",     true,  false, "uae+jit" },
	{ "uae",     true,  true,  "uae+jit+jitfpu" },
};
const int kTestEngineConfigCount = (int)(sizeof(kTestEngineConfigs) / sizeof(kTestEngineConfigs[0]));

struct PrefsEntry {
	std::string name;
	std::string value;
	bool is_bool;
	bool bool_val;
	bool is_int;
	int32 int_val;
};

static std::vector<PrefsEntry> g_prefs;

bool test_engine_matches(const char *filter, const TestEngineConfig *cfg)
{
	if (!filter || !filter[0] || strcmp(filter, "all") == 0)
		return true;
	return strcmp(filter, cfg->id) == 0 || strcmp(filter, cfg->label) == 0;
}

void test_prefs_clear(void)
{
	g_prefs.clear();
}

void test_prefs_add(const char *name, const char *value)
{
	PrefsEntry e;
	e.name = name;
	e.value = value ? value : "";
	e.is_bool = false;
	e.is_int = false;
	e.bool_val = false;
	e.int_val = 0;
	g_prefs.push_back(e);
}

void test_prefs_set_bool(const char *name, bool value)
{
	PrefsEntry e;
	e.name = name;
	e.is_bool = true;
	e.bool_val = value;
	e.is_int = false;
	e.int_val = 0;
	g_prefs.push_back(e);
}

void test_prefs_set_int32(const char *name, int32 value)
{
	PrefsEntry e;
	e.name = name;
	e.is_bool = false;
	e.is_int = true;
	e.int_val = value;
	e.bool_val = false;
	g_prefs.push_back(e);
}

bool PrefsFindBool(const char *name)
{
	for (size_t i = 0; i < g_prefs.size(); i++) {
		if (g_prefs[i].name == name && g_prefs[i].is_bool)
			return g_prefs[i].bool_val;
	}
	/* Match the historical integration stub: LocalTalk-over-UDP is on. */
	if (strcmp(name, "ltoudp") == 0)
		return true;
	return false;
}

int32 PrefsFindInt32(const char *name)
{
	for (size_t i = 0; i < g_prefs.size(); i++) {
		if (g_prefs[i].name == name && g_prefs[i].is_int)
			return g_prefs[i].int_val;
	}
	if (strcmp(name, "modelid") == 0)
		return 29; /* Quadra 800 */
	return 0;
}

const char *PrefsFindString(const char *name, int index)
{
	int n = 0;
	for (size_t i = 0; i < g_prefs.size(); i++) {
		if (g_prefs[i].name == name && !g_prefs[i].is_bool && !g_prefs[i].is_int) {
			if (n == index)
				return g_prefs[i].value.c_str();
			n++;
		}
	}
	return NULL;
}

void PrefsRemoveItem(const char *name, int index)
{
	(void)name;
	(void)index;
}

void PrefsReplaceString(const char *name, const char *val, int index)
{
	(void)name;
	(void)val;
	(void)index;
}

int16 PrefsFindInt16(const char *name)
{
	return (int16)PrefsFindInt32(name);
}

void PrefsReplaceBool(const char *name, bool b)
{
	test_prefs_set_bool(name, b);
}

/* PrefsAdd and PrefsReplaceInt helpers match prefs.h so PatchROM/DiskInit can link. */
void PrefsAddString(const char *name, const char *s)
{
	test_prefs_add(name, s);
}

void PrefsAddBool(const char *name, bool b)
{
	test_prefs_set_bool(name, b);
}

void PrefsAddInt16(const char *name, int16 val)
{
	test_prefs_set_int32(name, val);
}

void PrefsAddInt32(const char *name, int32 val)
{
	test_prefs_set_int32(name, val);
}

void PrefsReplaceInt16(const char *name, int16 val)
{
	test_prefs_set_int32(name, val);
}

void PrefsReplaceInt32(const char *name, int32 val)
{
	test_prefs_set_int32(name, val);
}

bool activate_cpu_engine(const char *id, bool jit, bool jitfpu)
{
	const CPUEngine *cur = GetActiveCPUEngine();
	if (cur && cur->exit)
		cur->exit();

	RAMSize = 1024 * 1024;
	RAMBaseMac = 0;
	ROMSize = 1024 * 1024;
	ROMBaseMac = 0x40800000;
	TwentyFourBitAddressing = false;
	ROMVersion = 0x067c;

	UseJIT = jit;
	UseJITFPU = (jit && jitfpu);
	JITCacheSize = 8192;
	CPUType = 4;
	FPUType = 1;

	/* Plant RESET ISP/PC before engine init. UAE's m68k_reset() reads guest 0 and 4. */
	memory_init();
	Mac_memset(RAMBaseMac, 0, RAMSize);
	Mac_memset(ROMBaseMac, 0, ROMSize);
	WriteMacInt32(0, 0x10000);
	WriteMacInt32(4, 0x4000);

	if (!SetActiveCPUEngine(id))
		return false;

	const CPUEngine *eng = GetActiveCPUEngine();
	if (!eng || !eng->init)
		return false;
	return eng->init();
}

/*
 * Tries path and returns a heap copy if the file exists.
 */
static char *dup_if_exists(const char *path)
{
	struct stat st;
	if (!path || stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return NULL;
	return strdup(path);
}

char *test_find_quadra_rom(void)
{
	const char *env = getenv("QUADRA_ROM");
	if (env && env[0]) {
		char *p = dup_if_exists(env);
		if (p)
			return p;
	}

	const char *root = getenv("TEST_REPO_ROOT");
#ifdef TEST_REPO_ROOT
	if (!root || !root[0])
		root = TEST_REPO_ROOT;
#endif
	if (root && root[0]) {
		char buf[1024];
		snprintf(buf, sizeof(buf), "%s/dist/Quadra800.rom", root);
		char *p = dup_if_exists(buf);
		if (p)
			return p;
	}

	static const char *candidates[] = {
		"../../dist/Quadra800.rom",
		"../../../dist/Quadra800.rom",
		"dist/Quadra800.rom",
		"../dist/Quadra800.rom",
		"Quadra800.rom",
	};
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		char *p = dup_if_exists(candidates[i]);
		if (p)
			return p;
	}
	return NULL;
}

size_t test_load_quadra_rom(const char *path)
{
	char *owned = NULL;
	if (!path) {
		owned = test_find_quadra_rom();
		path = owned;
	}
	if (!path) {
		printf("  [SKIP] dist/Quadra800.rom not found (set QUADRA_ROM)\n");
		return 0;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("  [SKIP] could not open ROM %s\n", path);
		free(owned);
		return 0;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0) {
		fclose(f);
		free(owned);
		return 0;
	}
	if ((uint32)sz > ROMSize)
		sz = (long)ROMSize;

	size_t n = fread(ROMBaseHost, 1, (size_t)sz, f);
	fclose(f);
	free(owned);
	printf("  Loaded %zu bytes of Quadra ROM into ROMBaseHost\n", n);
	return n;
}

void test_install_disk_trap_stubs(uint32 heap_addr, uint32 heap_size)
{
	/*
	 * Line-A vector (offset 0x28) runs for Execute68kTrap. NewPtrSysClear
	 * (0xA71E) bump-allocates from heap_addr; AddDrive (0xA04E) is a no-op.
	 * The trap word lives at the stacked PC; we skip it with ADDQ.L #2,(2,A7)
	 * before RTE so the engine returns to M68K_EXEC_RETURN.
	 */
	WriteMacInt32(heap_addr, heap_addr + 16); /* bump pointer at heap_addr */
	WriteMacInt32(heap_addr + 4, heap_addr + heap_size);
	(void)heap_size;

	const uint32 handler = 0xC000;
	uint32 p = handler;
	/* move.l 2(a7),a1 */
	WriteMacInt16(p, 0x226F); p += 2;
	WriteMacInt16(p, 0x0002); p += 2;
	/* move.w (a1),d1 */
	WriteMacInt16(p, 0x3211); p += 2;
	/* addq.l #2,2(a7) */
	WriteMacInt16(p, 0x54AF); p += 2;
	WriteMacInt16(p, 0x0002); p += 2;
	/* cmpi.w #0xA71E,d1 ; NewPtrSysClear */
	WriteMacInt16(p, 0x0C41); p += 2;
	WriteMacInt16(p, 0xA71E); p += 2;
	/* beq newptr (forward) */
	uint32 beq_newptr = p;
	WriteMacInt16(p, 0x6700); p += 2;
	WriteMacInt16(p, 0x0000); p += 2;
	/* cmpi.w #0xA04E,d1 ; AddDrive */
	WriteMacInt16(p, 0x0C41); p += 2;
	WriteMacInt16(p, 0xA04E); p += 2;
	/* beq adddrive */
	uint32 beq_add = p;
	WriteMacInt16(p, 0x6700); p += 2;
	WriteMacInt16(p, 0x0000); p += 2;
	/* rte (other traps) */
	uint32 other_rte = p;
	WriteMacInt16(p, 0x4E73); p += 2;

	uint32 newptr = p;
	/* A0 = current bump pointer stored at heap_addr (the allocated block). */
	WriteMacInt16(p, 0x2079); p += 2;
	WriteMacInt32(p, heap_addr); p += 4;
	/* A1 = address of the bump storage so ADD updates the pointer, not the block. */
	WriteMacInt16(p, 0x227C); p += 2;
	WriteMacInt32(p, heap_addr); p += 4;
	/* add.l d0,(a1) ; bump += requested size, A0 still holds the old bump */
	WriteMacInt16(p, 0xD191); p += 2;
	/* rte */
	WriteMacInt16(p, 0x4E73); p += 2;

	uint32 adddrive = p;
	WriteMacInt16(p, 0x4E73); p += 2;

	int16 rel_new = (int16)(newptr - (beq_newptr + 2));
	WriteMacInt16(beq_newptr + 2, (uint16)rel_new);
	int16 rel_add = (int16)(adddrive - (beq_add + 2));
	WriteMacInt16(beq_add + 2, (uint16)rel_add);
	(void)other_rte;

	WriteMacInt32(0x28, handler);
	WriteMacInt32(0x308, 0); /* empty drive queue for FindFreeDriveNumber */
}
