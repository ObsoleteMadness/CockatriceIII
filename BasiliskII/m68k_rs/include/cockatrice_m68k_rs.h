/*
 *  cockatrice_m68k_rs.h - C ABI for the m68k-rs CPU engine shim
 *
 *  CockatriceIII (C) 2026
 */

#ifndef COCKATRICE_M68K_RS_H
#define COCKATRICE_M68K_RS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct M68kRsCpu M68kRsCpu;

typedef enum M68kRsReg {
	M68K_RS_REG_D0 = 0,
	M68K_RS_REG_D1,
	M68K_RS_REG_D2,
	M68K_RS_REG_D3,
	M68K_RS_REG_D4,
	M68K_RS_REG_D5,
	M68K_RS_REG_D6,
	M68K_RS_REG_D7,
	M68K_RS_REG_A0,
	M68K_RS_REG_A1,
	M68K_RS_REG_A2,
	M68K_RS_REG_A3,
	M68K_RS_REG_A4,
	M68K_RS_REG_A5,
	M68K_RS_REG_A6,
	M68K_RS_REG_A7,
	M68K_RS_REG_PC,
	M68K_RS_REG_SR,
	M68K_RS_REG_PPC
} M68kRsReg;

typedef enum M68kRsCpuType {
	M68K_RS_CPU_68000 = 0,
	M68K_RS_CPU_68010,
	M68K_RS_CPU_68020,
	M68K_RS_CPU_68030,
	M68K_RS_CPU_68040
} M68kRsCpuType;

typedef enum M68kRsRunExit {
	M68K_RS_EXIT_BUDGET = 0,
	M68K_RS_EXIT_STOPPED,
	M68K_RS_EXIT_BOUNDARY,
	M68K_RS_EXIT_TRAP_UNHANDLED,
	M68K_RS_EXIT_HALTED
} M68kRsRunExit;

typedef struct M68kRsRegs {
	uint32_t d[8];
	uint32_t a[8];
	uint16_t sr;
} M68kRsRegs;

typedef struct M68kRsHostCallbacks {
	uint8_t (*read_byte)(void *ctx, uint32_t addr);
	uint16_t (*read_word)(void *ctx, uint32_t addr);
	uint32_t (*read_long)(void *ctx, uint32_t addr);
	void (*write_byte)(void *ctx, uint32_t addr, uint8_t val);
	void (*write_word)(void *ctx, uint32_t addr, uint16_t val);
	void (*write_long)(void *ctx, uint32_t addr, uint32_t val);
	int (*handle_illegal)(void *ctx, uint16_t opcode, M68kRsRegs *io_regs);
	int (*handle_aline)(void *ctx, uint16_t opcode, M68kRsRegs *io_regs);
	void (*boundary_hook)(void *ctx, uint32_t cycles);
	int (*get_irq)(void *ctx);
	/* Reports a contiguous, side-effect-free guest RAM window to the batch
	 * executor: writes *base / *len and returns the host pointer backing
	 * *base, or NULL when no direct window is available. Re-queried at the
	 * start of every m68k_rs_run_batch() call. */
	uint8_t *(*fast_mem)(void *ctx, uint32_t *base, uint32_t *len);
	void *host_ctx;
} M68kRsHostCallbacks;

typedef struct M68kRsRunResult {
	M68kRsRunExit exit;
	uint32_t cycles;
	uint32_t instructions;
	uint16_t trap_opcode;
} M68kRsRunResult;

M68kRsCpu *m68k_rs_create(const M68kRsHostCallbacks *callbacks);
void m68k_rs_destroy(M68kRsCpu *cpu);

/* Upper bound on committed ranges accepted by m68k_rs_set_mapped_ranges();
 * must match memory.cpp's MEMORY_MAX_RANGES. */
#define M68K_RS_MAX_MAPPED_RANGES 16

/* Pushes the host's committed-range table (RAM/ROM/framebuffer/etc, from
 * memory_get_mapped_ranges()) into the bus's local cache so checked memory
 * accesses (anything outside the FastMem window) validate locally instead
 * of calling back into the host per access. Call once after m68k_rs_create()
 * and memory_init(); the table is static for the process lifetime today, so
 * no later refresh is needed. `count` is clamped to M68K_RS_MAX_MAPPED_RANGES.
 *
 * `scc_24bit_mirror`: legacy 24-bit-addressing flag, always 0 now that the
 * emulated machine is fixed at 68040/32-bit addressing. The caller appends
 * the SCC's single 0x50000000-0x51000000 window as a normal range instead.
 * Kept as a parameter for ABI stability rather than changing this signature. */
void m68k_rs_set_mapped_ranges(M68kRsCpu *cpu, const uint32_t *starts, const uint32_t *ends,
                                uint32_t count, int scc_24bit_mirror);

int m68k_rs_init(M68kRsCpu *cpu, M68kRsCpuType cpu_type);
void m68k_rs_pulse_reset(M68kRsCpu *cpu);
void m68k_rs_invalidate_prefetch(M68kRsCpu *cpu);

uint32_t m68k_rs_get_reg(const M68kRsCpu *cpu, M68kRsReg reg);
void m68k_rs_set_reg(M68kRsCpu *cpu, M68kRsReg reg, uint32_t value);

void m68k_rs_set_irq(M68kRsCpu *cpu, int level);
void m68k_rs_request_stop(M68kRsCpu *cpu);

M68kRsRunResult m68k_rs_run_cycles(M68kRsCpu *cpu, int32_t cycle_budget);

/* Throughput path: instruction-budgeted, no cycle accounting and no
 * per-instruction boundary hook, so the host polls interrupts between
 * batches. Uses the decoded-op cache, the fast_mem window and the trace JIT. */
M68kRsRunResult m68k_rs_run_batch(M68kRsCpu *cpu, uint32_t max_instructions);

/* Non-zero when the Cranelift trace JIT was compiled in. The batch path works
 * either way; without it hot traces run on the portable executor. */
int m68k_rs_jit_available(void);

#ifdef __cplusplus
}
#endif

#endif /* COCKATRICE_M68K_RS_H */
