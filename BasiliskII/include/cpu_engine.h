/*
 *  cpu_engine.h - Unified 680x0 CPU Engine Abstraction Layer
 *
 *  CockatriceIII Multi-Engine Architecture
 *  Supports: Musashi (C core), WinUAE (modern 680x0+SoftFloat), Emu68 (ARM64 JIT)
 *
 *  (C) 2026 CockatriceIII Project
 */

#ifndef CPU_ENGINE_H
#define CPU_ENGINE_H

#include "sysdeps.h"
#include "cpu_emulation.h"

#ifdef __cplusplus
extern "C" {
#endif

struct M68kRegisters;

/*
 * CPU Engine Definition Interface
 */
typedef struct CPUEngine {
	const char *id;             /* Unique identifier: "musashi", "uae", "emu68" */
	const char *name;           /* Human-readable description */
	bool is_jit;                /* True if dynamic binary translator */

	/* Lifecycle functions */
	bool (*init)(void);
	void (*exit)(void);
	void (*start)(void);
	void (*reset)(void);

	/* Execution functions */
	void (*execute_68k)(uint32 addr, struct M68kRegisters *r);
	void (*execute_68k_trap)(uint16 trap, struct M68kRegisters *r);

	/* Interrupts */
	void (*trigger_interrupt)(void);
	void (*trigger_nmi)(void);
	int  (*intlev)(void);
} CPUEngine;

/*
 * Engine Registry & Selection API
 */
void RegisterCPUEngine(const CPUEngine *engine);
const CPUEngine *GetCPUEngine(const char *id);
bool SetActiveCPUEngine(const char *id);
const CPUEngine *GetActiveCPUEngine(void);
int GetRegisteredCPUEngineCount(void);
const CPUEngine *GetRegisteredCPUEngineByIndex(int index);

/* Built-in Engine Instances */
extern const CPUEngine musashi_cpu_engine;
extern const CPUEngine winuae_cpu_engine;
extern const CPUEngine emu68_cpu_engine;

#ifdef __cplusplus
}
#endif

#endif /* CPU_ENGINE_H */
