/*
 *  cpu_engine.cpp - CPU Engine Registry and Global Dispatcher
 *
 *  CockatriceIII Multi-Engine Architecture
 *  (C) 2026 CockatriceIII Project
 */

#include <stdio.h>
#include <string.h>
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "prefs.h"

#define MAX_CPU_ENGINES 8

extern const CPUEngine musashi_cpu_engine;
extern const CPUEngine winuae_cpu_engine;
extern const CPUEngine emu68_cpu_engine;

static const CPUEngine *s_engines[MAX_CPU_ENGINES] = {
	&musashi_cpu_engine,
	&winuae_cpu_engine,
	&emu68_cpu_engine
};
static int s_engine_count = 3;
static const CPUEngine *s_active_engine = &musashi_cpu_engine;

bool UseJIT = false;
bool UseJITFPU = false;
uint32 JITCacheSize = 2048;

void RegisterCPUEngine(const CPUEngine *engine)
{
	if (!engine || !engine->id)
		return;

	for (int i = 0; i < s_engine_count; i++) {
		if (strcmp(s_engines[i]->id, engine->id) == 0) {
			s_engines[i] = engine;
			return;
		}
	}

	if (s_engine_count < MAX_CPU_ENGINES) {
		s_engines[s_engine_count++] = engine;
	}
}

const CPUEngine *GetCPUEngine(const char *id)
{
	if (!id)
		return NULL;

	for (int i = 0; i < s_engine_count; i++) {
		if (strcmp(s_engines[i]->id, id) == 0)
			return s_engines[i];
	}
	return NULL;
}

bool SetActiveCPUEngine(const char *id)
{
	const CPUEngine *engine = GetCPUEngine(id);
	if (engine) {
		s_active_engine = engine;
		printf("[CPU-ENGINE] Active 680x0 CPU Engine: %s (%s)\n", engine->name, engine->id);
		fflush(stdout);
		return true;
	}
	return false;
}

const CPUEngine *GetActiveCPUEngine(void)
{
	return s_active_engine;
}

int GetRegisteredCPUEngineCount(void)
{
	return s_engine_count;
}

const CPUEngine *GetRegisteredCPUEngineByIndex(int index)
{
	if (index >= 0 && index < s_engine_count)
		return s_engines[index];
	return NULL;
}

static void EnsureEnginesRegistered(void)
{
	if (s_engine_count == 0) {
		RegisterCPUEngine(&musashi_cpu_engine);
		RegisterCPUEngine(&winuae_cpu_engine);
		RegisterCPUEngine(&emu68_cpu_engine);
	}
}

/*
 * Global Basilisk II CPU Emulation Interface Dispatchers
 */

bool Init680x0(void)
{
	EnsureEnginesRegistered();

	const char *requested = PrefsFindString("cpu_emulator");
	if (!requested || !requested[0]) {
		requested = "musashi";
	}

	if (!SetActiveCPUEngine(requested)) {
		printf("[CPU-ENGINE] Warning: Requested engine '%s' not available, falling back to 'musashi'\n", requested);
		if (!SetActiveCPUEngine("musashi")) {
			if (s_engine_count > 0) {
				s_active_engine = s_engines[0];
				printf("[CPU-ENGINE] Using first available engine: %s\n", s_active_engine->name);
			} else {
				printf("[CPU-ENGINE] FATAL: No CPU engines registered!\n");
				return false;
			}
		}
	}

	UseJIT = PrefsFindBool("jit");
	UseJITFPU = PrefsFindBool("jitfpu");
	int32 cachesize = PrefsFindInt32("jitcachesize");
	if (cachesize > 0)
		JITCacheSize = (uint32)cachesize;

	if (s_active_engine && strcmp(s_active_engine->id, "emu68") == 0) {
		UseJIT = true; // Emu68 is always JIT
	} else if (s_active_engine && strcmp(s_active_engine->id, "musashi") == 0) {
		UseJIT = false; // Musashi is interpreter only
	}

	if (s_active_engine && s_active_engine->init) {
		return s_active_engine->init();
	}
	return false;
}

void Exit680x0(void)
{
	if (s_active_engine && s_active_engine->exit)
		s_active_engine->exit();
}

void Start680x0(void)
{
	if (s_active_engine && s_active_engine->start)
		s_active_engine->start();
}

void Reset680x0(void)
{
	if (s_active_engine && s_active_engine->reset)
		s_active_engine->reset();
}

extern "C" void Execute68k(uint32 addr, M68kRegisters *r)
{
	if (s_active_engine && s_active_engine->execute_68k)
		s_active_engine->execute_68k(addr, r);
}

extern "C" void Execute68kTrap(uint16 trap, M68kRegisters *r)
{
	if (s_active_engine && s_active_engine->execute_68k_trap)
		s_active_engine->execute_68k_trap(trap, r);
}

void TriggerInterrupt(void)
{
	if (s_active_engine && s_active_engine->trigger_interrupt)
		s_active_engine->trigger_interrupt();
}

void TriggerNMI(void)
{
	if (s_active_engine && s_active_engine->trigger_nmi)
		s_active_engine->trigger_nmi();
}

int intlev(void)
{
	if (s_active_engine && s_active_engine->intlev)
		return s_active_engine->intlev();
	return 0;
}
