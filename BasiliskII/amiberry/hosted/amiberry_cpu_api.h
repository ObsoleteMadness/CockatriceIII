/*
 *  amiberry_cpu_api.h - Cockatrice-owned API in front of the Amiberry 680x0 core
 *
 *  Glue (Basilisk headers) talks only to this interface. Hosted Amiberry
 *  translation units talk to Amiberry headers and implement these functions.
 *  That split avoids Basilisk sysdeps.h colliding with Amiberry sysdeps.h.
 */

#ifndef AMIBERRY_CPU_API_H
#define AMIBERRY_CPU_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initializes Amiberry's 680x0 interpreter (and JIT if jit is non-zero).
 *
 * Arguments:
 *   cpu_type: Basilisk CPUType (0=68000 .. 4=68040).
 *   fpu_type: Basilisk FPUType (0=none, 1=68881, 2=68882, 4=68040).
 *   jit: Non-zero to enable the arch JIT (ARM64 or x86-64).
 *   cache_kb: JIT translation cache size in kilobytes (ignored if jit is 0).
 *   jitfpu: Non-zero to compile FPU ops in the JIT.
 *
 * Returns:
 *   1 on success, 0 if the CPU tables or JIT cache could not be set up.
 */
int amiberry_cpu_init(int cpu_type, int fpu_type, int jit, uint32_t cache_kb, int jitfpu);

/* Releases the JIT cache and marks the core idle. */
void amiberry_cpu_exit(void);

/* Pulses a 680x0 reset and rebuilds prefetch state. */
void amiberry_cpu_reset(void);

/*
 * Runs the interpreter or JIT until SPCFLAG_MODE_CHANGE (timeslice or
 * M68K_EXEC_RETURN). The caller must loop and check its own quit/return flags.
 */
void amiberry_cpu_execute_slice(void);

/*
 * Interpreter-only timeslice for nested Execute68k / Execute68kTrap. Does not
 * re-enter JIT; exits on SPCFLAG_MODE_CHANGE (including M68K_EXEC_RETURN).
 */
void amiberry_cpu_execute_interpreter_slice(void);

/*
 * Nested Execute68k bookkeeping (macemu m68k_execute_depth / quit_program).
 */
void amiberry_cpu_nested_execute_begin(void);
void amiberry_cpu_nested_execute_end(void);
extern "C" int amiberry_cpu_nested_execute_depth(void);
void amiberry_cpu_nested_request_quit(void);
int amiberry_cpu_nested_quit_requested(void);

/* Clears leftover MODE_CHANGE so the next m68k_run() does not return immediately. */
void amiberry_cpu_clear_mode_change(void);

/* Raises SPCFLAG_MODE_CHANGE so the current m68k_run() returns (Execute68k / timeslice). */
void amiberry_cpu_set_mode_change(void);

/* Sets SPCFLAG_BRK so the current interpreter slice returns (nested Execute68k). */
void amiberry_cpu_request_brk(void);

/* Rebuilds 680x0 instruction prefetch after m68k_setpc (macemu fill_prefetch_0). */
void amiberry_cpu_fill_prefetch(void);

/*
 * Dispatches a Basilisk EmulOp (0x7101..) from the UAE CPU opcode table.
 * Called from op_emulop_1 in newcpu.cpp, not via op_illg.
 */
void cockatrice_m68k_emulop(uint32_t opcode);

/* Handles M68K_EXEC_RETURN (0x7100) from op_emulop_return_1. */
void cockatrice_m68k_emulop_return(void);

/* Sets SPCFLAG_INT so the next specialties pass queries intlev(). */
void amiberry_cpu_request_irq(void);

uint32_t amiberry_cpu_get_pc(void);
void amiberry_cpu_set_pc(uint32_t pc);
void amiberry_cpu_inc_pc(int bytes);

/* Register index 0-7 = D0-D7, 8-15 = A0-A7. */
uint32_t amiberry_cpu_get_reg(int n);
void amiberry_cpu_set_reg(int n, uint32_t v);

uint16_t amiberry_cpu_get_sr(void);
void amiberry_cpu_set_sr(uint16_t sr);

/*
 * Macintosh memory callbacks implemented by amiberry_glue.cpp (Basilisk banks).
 * The Amiberry memory bridge calls these instead of Amiga chip RAM.
 */
uint32_t cockatrice_mac_get_long(uint32_t addr);
uint32_t cockatrice_mac_get_word(uint32_t addr);
uint32_t cockatrice_mac_get_byte(uint32_t addr);
void cockatrice_mac_put_long(uint32_t addr, uint32_t v);
void cockatrice_mac_put_word(uint32_t addr, uint32_t v);
void cockatrice_mac_put_byte(uint32_t addr, uint32_t v);
uint8_t *cockatrice_mac_host_addr(uint32_t addr);
int cockatrice_mac_valid_addr(uint32_t addr, uint32_t size);

/*
 * Injects 680x0 vector 2 after a PROT_NONE hole access in the 4GB window.
 *
 * Arguments:
 *   addr: Macintosh address that raised ACCESS_VIOLATION / SIGSEGV.
 */
void amiberry_cpu_bus_error(uint32_t addr);

/*
 * Longjmps to an active memory_guard checkpoint when a write target is not
 * mapped (avoids host SIGSEGV outside Host_Mem_Base + 4GB).
 *
 * Arguments:
 *   addr: 32-bit Macintosh address that would fault.
 */
void cockatrice_memory_raise_guest_fault(uint32_t addr);

/*
 * Logs an F-line (Line 1111 / Mac System Error type 10) before vector 11.
 *
 * Arguments:
 *   opcode: 16-bit F-line instruction word.
 *   pc: Guest PC of the faulting instruction.
 *   from_mmu_op: Non-zero when mmu_op() is about to escalate to op_illg().
 */
void cockatrice_uae_fline_trap(uint32_t opcode, uint32_t pc, int from_mmu_op);

/*
 * Flushes UAE JIT translations covering guest [addr, addr+size).
 *
 * Arguments:
 *   addr: Macintosh start address of modified code (ignored when size is ~0).
 *   size: Byte length of the modified range (~0 flushes the entire cache).
 */
void amiberry_cpu_invalidate_code(uint32_t addr, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* AMIBERRY_CPU_API_H */
