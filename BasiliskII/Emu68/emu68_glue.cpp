/*
 *  emu68_glue.cpp - Emu68 AArch64 Dynamic Binary Translation / JIT Engine Bridge
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *  CockatriceIII Multi-Engine Architecture (C) 2026
 *
 *  This file is the Cockatrice hosted TARGET adapter for Emu68 (the same
 *  role as an upstream CMake TARGET such as raspi64 or virt). It is not an
 *  ExpansionBoard and not a device-tree overlay.
 *
 *  It isolates macOS memory management (MAP_JIT and pthread_jit_write_protect_np),
 *  Mac OS EmulOp traps (0x7101..0x713F), return stubs (0x7100), and interrupt
 *  routing, so the translator can track upstream michalsc/Emu68. Hosted
 *  TARGET physics live in hosted/ (adapter, EL1 stand-ins, exceptions,
 *  EmulOp). Unmodified translator files compile from the git submodule at
 *  upstream/, after a mechanical prep pass (Mach-O aliases, WFI, dual-map).
 *
 *  === Architectural Mechanics ===
 *  1. CPU State Representation:
 *     - Global __m68k_state contains registers D0-D7, A0-A7, PC, SR, VBR, CACR,
 *       and the 68882/68040 FPU register file (FP0-FP7, FPSR, FPCR, FPIAR).
 *  2. JIT Translation Buffer Allocation & Protection:
 *     - JIT code memory is allocated via mmap() using MAP_JIT on Darwin.
 *     - Write protection is dynamically switched via pthread_jit_write_protect_np(0)
 *       when translating/emitting code and pthread_jit_write_protect_np(1) before execution.
 *  3. Return Hook & Execution Stacks:
 *     - Subroutine execution (Execute68k) and trap dispatch (Execute68kTrap) allocate
 *       a synthetic stack frame containing M68K_EXEC_RETURN (0x7100).
 *     - When the 680x0 CPU core encounters 0x7100, the emul_op subsystem pops the return
 *       stack and signals completion to return control back to host C++.
 *  4. Unified 4GB HOST_MEM_BASE window:
 *     - Macintosh addresses are offsets into the shared Host_Mem_Base mapping
 *       allocated by memory_init() at startup. emu68_host_mem_base aliases that
 *       pointer; JIT loads/stores emit HOST_MEM_BASE + UXTW(An).
 *     - Instruction fetch and helpers go through cache_read_N / cache_write_N
 *       to ReadMacIntN / WriteMacIntN to Mac2HostAddr (same window, plus SCC
 *       MMIO and ROM write-protect). There is no 64K bank table.
 *     - emu68_init() rebinds RAMBaseHost/ROMBaseHost/MacFrameBaseHost via
 *       memory_init() after ROMBaseMac is known. memory_init() slides the ROM
 *       image if the Mac base changed from the 0x40800000 load address.
 *  5. Bare-metal SVC debug dumps:
 *     - Upstream translators emit svc #0x100/#0x101/#0x103 as a kernel debug dump
 *       for unimplemented opcodes. On Darwin that is a real syscall and raises
 *       SIGSYS ("invalid system call"). Hosted svc() branches over the payload
 *       instead; see A64.h.
 *  6. Basilisk EmulOp traps (illegal MOVEQ 0x7100..0x7130):
 *     - PatchROM() plants 0x7103 (RESET) at ROM 0x4080008A. The JIT calls
 *       emu68_hosted_emulop() so boot gets BootGlobs / AddrMap in D0-D2/A0-A7
 *       instead of taking an illegal-instruction path into low RAM.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stddef.h>
#include <setjmp.h>
#include <math.h>
#include <sys/mman.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "main.h"
#include "macos_util.h"
#include "rom_patches.h"
#include "menu_bar.h"
#include "scc.h"
#include "scsi.h"
#include "sony.h"
#include "disk.h"
#include "audio.h"
#include "ether.h"
#include "timer.h"
#include "prefs.h"
#include "m68k.h"
#include "emu68_darwin_jit.h"
#include "emu68_hosted.h"

#ifndef restrict
#define restrict __restrict
#endif

// Hosted A64/RegLock wraps first so upstream M68k.h sibling includes are skipped.
#include "A64.h"
#include "RegLock.h"

// Emu68 Core Includes (path so Darwin does not resolve this to Musashi m68k.h)
#include "../Emu68/upstream/include/M68k.h"
#include "../Emu68/upstream/include/tlsf.h"
#include "../Emu68/upstream/include/cache.h"

typedef void *tlsf_t;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Global Emu68 680x0 CPU State Structure instance.
 * Aligned to 64 bytes for AArch64 cache line alignment and register allocator efficiency.
 */
static struct M68KState s_m68k_state;
struct M68KState *__m68k_state = &s_m68k_state;

// TLSF memory allocation pool handles for Emu68 JIT code buffer and metadata heap
tlsf_t jit_tlsf = nullptr;
tlsf_t tlsf = nullptr;

// JIT Code Cache Configuration (8 MB executable cache buffer, 4 MB metadata heap)
#define EMU68_CODE_CACHE_SIZE (8 * 1024 * 1024)
#define EMU68_HEAP_SIZE       (4 * 1024 * 1024)

// Host pointers for executable JIT code cache, metadata heap, and unified 4GB host memory window
uint8_t *emu68_host_mem_base = nullptr;
static uint8_t *s_jit_code_buffer = nullptr;
static uint8_t *s_jit_heap_buffer = nullptr;

// Last translated JIT block metadata used to correlate a crash PC with emitted AArch64 bytes
static void *s_last_jit_entry = nullptr;
static uint32_t s_last_jit_insn_count = 0;
static uint32_t s_last_m68k_pc = 0;
static uint16_t s_last_m68k_op = 0;

// When true, each translated unit is printed to stdout before execution (prefs emu68_jit_dump)
static bool s_emu68_jit_dump = false;

// Longjmp reset buffer for handling Mac OS warm resets (Reset680x0)
static jmp_buf s_emu68_reset_jmp;
static volatile bool s_emu68_reset_valid = false;
static bool s_emu68_quit_requested = false;

/*
 * Full 680x0 register snapshot for isolating nested Execute68k from outer guest code.
 */
struct Emu68CpuSnapshot {
	uint32 d[8];
	uint32 a[8];
	uint16 sr;
	uint32 pc;
};

/*
 * Captures the live Musashi-backed register file used by Emu68 into a snapshot.
 *
 * Arguments:
 *   s: Destination snapshot written with current D0-D7, A0-A7, SR, and PC.
 */
static void emu68_snapshot_cpu(Emu68CpuSnapshot *s)
{
	for (int i = 0; i < 8; i++) {
		s->d[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i));
		s->a[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i));
	}
	s->sr = (uint16)m68k_get_reg(NULL, M68K_REG_SR);
	s->pc = m68k_get_reg(NULL, M68K_REG_PC);
}

/*
 * Restores a previously captured register snapshot into the live Emu68 CPU.
 *
 * Arguments:
 *   s: Snapshot to restore; overwrites current D0-D7, A0-A7, SR, and PC.
 */
static void emu68_restore_cpu(const Emu68CpuSnapshot *s)
{
	for (int i = 0; i < 8; i++) {
		m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), s->d[i]);
		m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), s->a[i]);
	}
	m68k_set_reg(M68K_REG_SR, s->sr);
	m68k_set_reg(M68K_REG_PC, s->pc);
}

/*
 * Formatted logging output for Emu68 debug messages.
 *
 * Parameters:
 *   fmt - Standard printf-style format string.
 *   ... - Variable arguments corresponding to format specifiers.
 */
/*
 * Hosted kprintf used by JIT debug strings (EMIT_InjectDebugString) and by
 * translator diagnostics. Always prints: the previous #if DEBUG stub swallowed
 * unimplemented-opcode messages, so a crash looked like a silent SIGSYS.
 *
 * Parameters:
 *   fmt - printf-style format string. When called from JIT it points at a
 *         string embedded in the MAP_JIT buffer (readable while execute-only).
 *   ... - Format arguments.
 */
void kprintf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	fflush(stdout);
}

/*
 * LINE*.c does `extern int debug_not_implemented` and, when the value is
 * non-zero, emits the bare-metal SVC debug dump before an illegal-instruction
 * exception. Upstream Emu68 defines this as an int flag defaulting to 0.
 *
 * A C function with this name was linked instead: the translators loaded the
 * function's first instruction word as the flag (always non-zero), so every
 * unimplemented opcode executed svc #0x100 and Darwin raised SIGSYS.
 * Non-zero here so EMIT_InjectDebugString still prints the opcode; svc() on
 * hosted builds no longer emits a real SVC.
 */
int debug_not_implemented = 1;

/*
 * Disassembly helper stubs (Capstone is not linked on this TARGET).
 * Signatures match include/disasm.h so M68k_Translator.c can call them.
 */
void disasm_init(void) {}
void disasm_open(void) {}
void disasm_close(void) {}
void disasm_print(uint16_t *m68k_addr, uint16_t m68k_count, uint32_t *arm_addr, size_t arm_size, uint32_t *arm_start)
{
	(void)m68k_addr; (void)m68k_count; (void)arm_addr; (void)arm_size; (void)arm_start;
}
void M68K_PrintContext(void) {}

/*
 * Memory Cache Read / Write Hooks routing to the unified 4GB Mac window:
 *
 * Read 8-bit unsigned integer from Macintosh physical address space.
 *
 * Parameters:
 *   type    - Cache access type (instruction / data / direct).
 *   address - 32-bit Macintosh address.
 *
 * Returns:
 *   8-bit value stored at the given address.
 */
uint8_t cache_read_8(enum CacheType type, uint32_t address)
{
	(void)type;
	// Direct read from the shared Host_Mem_Base window (SCC MMIO via ReadMacInt8)
	return ReadMacInt8(address);
}

/*
 * Read 16-bit big-endian unsigned integer from Macintosh physical address space.
 *
 * Parameters:
 *   type    - Cache access type (instruction / data / direct).
 *   address - 32-bit Macintosh address.
 *
 * Returns:
 *   16-bit big-endian converted host integer.
 */
uint16_t cache_read_16(enum CacheType type, uint32_t address)
{
	(void)type;
	// 16-bit fetch from Host_Mem_Base via ReadMacInt16 (endian + SCC)
	return ReadMacInt16(address);
}

/*
 * Read 32-bit big-endian unsigned integer from Macintosh physical address space.
 *
 * Parameters:
 *   type    - Cache access type (instruction / data / direct).
 *   address - 32-bit Macintosh address.
 *
 * Returns:
 *   32-bit big-endian converted host integer.
 */
uint32_t cache_read_32(enum CacheType type, uint32_t address)
{
	(void)type;
	// 32-bit fetch from Host_Mem_Base via ReadMacInt32 (endian + SCC)
	return ReadMacInt32(address);
}

/*
 * Read 64-bit big-endian unsigned integer from Macintosh physical address space.
 *
 * Parameters:
 *   type    - Cache access type (instruction / data / direct).
 *   address - 32-bit Macintosh address.
 *
 * Returns:
 *   64-bit big-endian converted host integer.
 */
uint64_t cache_read_64(enum CacheType type, uint32_t address)
{
	(void)type;
	// Read high and low 32-bit big-endian words and assemble into 64-bit integer
	uint64_t hi = ReadMacInt32(address);
	uint64_t lo = ReadMacInt32(address + 4);
	return (hi << 32) | lo;
}

/*
 * Write 8-bit integer to Macintosh physical address space.
 *
 * Parameters:
 *   type       - Cache access type.
 *   address    - 32-bit Macintosh address.
 *   data       - 8-bit data byte to write.
 *   write_back - Flush policy indicator.
 *
 * Returns:
 *   0 on success.
 */
int cache_write_8(enum CacheType type, uint32_t address, uint8_t data, uint8_t write_back)
{
	(void)type; (void)write_back;
	// Write through WriteMacInt8 (ROM protect + SCC MMIO on Host_Mem_Base)
	WriteMacInt8(address, data);
	return 0;
}

/*
 * Write 16-bit big-endian integer to Macintosh physical address space.
 *
 * Parameters:
 *   type       - Cache access type.
 *   address    - 32-bit Macintosh address.
 *   data       - 16-bit host integer to write in big-endian format.
 *   write_back - Flush policy indicator.
 *
 * Returns:
 *   0 on success.
 */
int cache_write_16(enum CacheType type, uint32_t address, uint16_t data, uint8_t write_back)
{
	(void)type; (void)write_back;
	// Write through WriteMacInt16 (ROM protect + SCC MMIO on Host_Mem_Base)
	WriteMacInt16(address, data);
	return 0;
}

/*
 * Write 32-bit big-endian integer to Macintosh physical address space.
 *
 * Parameters:
 *   type       - Cache access type.
 *   address    - 32-bit Macintosh address.
 *   data       - 32-bit host integer to write in big-endian format.
 *   write_back - Flush policy indicator.
 *
 * Returns:
 *   0 on success.
 */
int cache_write_32(enum CacheType type, uint32_t address, uint32_t data, uint8_t write_back)
{
	(void)type; (void)write_back;
	// Write through WriteMacInt32 (ROM protect + SCC MMIO on Host_Mem_Base)
	WriteMacInt32(address, data);
	return 0;
}

/*
 * Write 64-bit big-endian integer to Macintosh physical address space.
 *
 * Parameters:
 *   type       - Cache access type.
 *   address    - 32-bit Macintosh address.
 *   data       - 64-bit host integer to write in big-endian format.
 *   write_back - Flush policy indicator.
 *
 * Returns:
 *   0 on success.
 */
int cache_write_64(enum CacheType type, uint32_t address, uint64_t data, uint8_t write_back)
{
	(void)type; (void)write_back;
	// Split 64-bit value into high/low 32-bit words and write big-endian
	WriteMacInt32(address, (uint32_t)(data >> 32));
	WriteMacInt32(address + 4, (uint32_t)data);
	return 0;
}

/*
 * Cache Invalidation Hooks called by Emu68 translation engine.
 */
void cache_invalidate_all(enum CacheType cache)
{
	if (cache == ICACHE) {
		extern uint32_t EPOCH;
		/* Soft-flush: bump epoch so cached units miss until retranslated. */
		EPOCH++;
		LRU_InvalidateAll();
	}
}
void cache_invalidate_line(enum CacheType type, uint32_t address)
{
	(void)type;
	LRU_InvalidateByM68kAddress(address);
}
void cache_invalidate_range(enum CacheType type, uint32_t address, uint32_t len)
{
	(void)type;
	(void)len;
	LRU_InvalidateByM68kAddress(address);
}
/* log2(line size in bytes) for CINVA/CPUSHA loops. Upstream reads CTR_EL0. */
int dcache_mask_bits = 6;

#ifdef __cplusplus
}
#endif

/*
 * Synchronizes register state from Emu68 __m68k_state structure into the Musashi core.
 * Ensures consistent register values across CPU execution transitions.
 */
static void emu68_sync_to_m68k(void)
{
	// Ensure valid status register with supervisor mode if unset
	uint32_t sr = __m68k_state->SR ? __m68k_state->SR : 0x2700;
	m68k_set_reg(M68K_REG_SR, sr);
	m68k_set_reg(M68K_REG_VBR, __m68k_state->VBR);
	m68k_set_reg(M68K_REG_PC, __m68k_state->PC);
	m68k_set_reg(M68K_REG_USP, __m68k_state->USP.u32);
	m68k_set_reg(M68K_REG_ISP, __m68k_state->ISP.u32);
	m68k_set_reg(M68K_REG_MSP, __m68k_state->MSP.u32);

	// Synchronize general-purpose data registers D0-D7 and address registers A0-A7
	for (int i = 0; i < 8; i++) {
		m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), __m68k_state->D[i].u32);
		m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), __m68k_state->A[i].u32);
	}
}

/*
 * Synchronizes register state from Musashi core into Emu68 __m68k_state structure.
 * Captures updated data/address registers and condition flags after an execution slice.
 */
static void emu68_sync_from_m68k(void)
{
	// Read back all data registers D0-D7 and address registers A0-A7
	for (int i = 0; i < 8; i++) {
		__m68k_state->D[i].u32 = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i));
		__m68k_state->A[i].u32 = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i));
	}
	// Synchronize Program Counter, Status Register, and Vector Base Register
	__m68k_state->PC = m68k_get_reg(NULL, M68K_REG_PC);
	__m68k_state->SR = (uint16_t)m68k_get_reg(NULL, M68K_REG_SR);
	__m68k_state->VBR = m68k_get_reg(NULL, M68K_REG_VBR);
	__m68k_state->USP.u32 = m68k_get_reg(NULL, M68K_REG_USP);
	__m68k_state->ISP.u32 = m68k_get_reg(NULL, M68K_REG_ISP);
	__m68k_state->MSP.u32 = m68k_get_reg(NULL, M68K_REG_MSP);
}

/*
 * Initializes the Emu68 engine and allocates JIT cache buffers.
 *
 * When JIT is enabled (UseJIT is true), this method allocates the 8 MB executable
 * code cache via mmap with MAP_JIT, sets up the TLSF memory heaps, and calls
 * M68K_InitializeCache() to initialize the instruction cache hash tables.
 * When JIT is disabled (UseJIT is false), it runs using the interpreter fallback.
 *
 * Returns:
 *   true if initialization succeeded, false otherwise.
 */
static bool emu68_init(void)
{
#if !defined(__aarch64__) && !defined(__arm64__)
	printf("[Emu68] Error: Emu68 JIT requires an AArch64 / ARM64 host architecture.\n");
	return false;
#endif

	// Clear leftover exit flags so Execute68k works after a previous emu68_exit()
	s_emu68_quit_requested = false;

	if (!cpu_engine_map_rom_base())
		return false;

	// Bind JIT window to the shared 4GB mapping; refuse to run without it
	if (!Host_Mem_Base) {
		printf("[Emu68] FATAL: unified 4GB Host_Mem_Base window is not allocated\n");
		fflush(stdout);
		return false;
	}
	emu68_host_mem_base = Host_Mem_Base;
	memory_init();

	printf("[Emu68] Unified HOST_MEM_BASE=%p RAMHost=%p ROMHost=%p\n",
	       emu68_host_mem_base, RAMBaseHost, ROMBaseHost);
	fflush(stdout);

	// Initialize CPU core in 68040 mode (interpreter fallback / Execute68k register file)
	m68k_init();
	m68k_set_cpu_type(M68K_CPU_TYPE_68040);

	// Initialize JIT code cache and translation tables if JIT is enabled
	if (UseJIT) {
		if (!s_jit_code_buffer) {
			int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
			int flags = MAP_ANON | MAP_PRIVATE;
#if defined(MAP_JIT)
			// On Apple Silicon, MAP_JIT is required to allow write/execute transitions via pthread_jit_write_protect_np
			flags |= MAP_JIT;
#endif
			s_jit_code_buffer = (uint8_t *)mmap(NULL, EMU68_CODE_CACHE_SIZE, prot, flags, -1, 0);
			if (s_jit_code_buffer == MAP_FAILED) {
				s_jit_code_buffer = nullptr;
				return false;
			}

			// Allocate metadata heap buffer for translation unit nodes and LRU tracking
			s_jit_heap_buffer = (uint8_t *)malloc(EMU68_HEAP_SIZE);
			if (!s_jit_heap_buffer) {
				return false;
			}

			// Initialize TLSF allocator heaps for JIT code buffer and metadata heap
			jit_write_enable();
			jit_tlsf = tlsf_init_with_memory(s_jit_code_buffer, EMU68_CODE_CACHE_SIZE);
			tlsf = tlsf_init_with_memory(s_jit_heap_buffer, EMU68_HEAP_SIZE);

			// Initialize Emu68 translation cache hash tables and LRU lists
			M68K_InitializeCache();
			jit_write_disable();

			// Query Darwin JIT write protection support status
			int supp = pthread_jit_write_protect_supported_np ? pthread_jit_write_protect_supported_np() : -1;
			printf("[Emu68] JIT cache allocated at %p (size %d MB, Darwin JIT support=%d)\n",
			       s_jit_code_buffer, EMU68_CODE_CACHE_SIZE / (1024 * 1024), supp);
			fflush(stdout);
		}
		s_emu68_jit_dump = PrefsFindBool("emu68_jit_dump");
		if (s_emu68_jit_dump)
			printf("[Emu68] JIT instruction dump enabled (set emu68_jit_dump false to disable)\n");
	} else {
		printf("[Emu68] JIT disabled via preferences ('jit false'). Running in interpreter mode.\n");
		fflush(stdout);
	}

	// Zero CPU context, then set boot SR, ISP, and JIT knobs (must be after memset).
	memset(&s_m68k_state, 0, sizeof(s_m68k_state));
	s_m68k_state.SR = CPU_ENGINE_BOOT_SR;
	s_m68k_state.A[7].u32 = CPU_ENGINE_INIT_SP;
	s_m68k_state.ISP.u32 = CPU_ENGINE_INIT_SP;
	/*
	 * JIT_CONTROL matches upstream start.c / include/config.h. Glue cannot
	 * include Emu68 config.h: sysdeps already took _CONFIG_H for OSX64.
	 */
	s_m68k_state.JIT_SOFTFLUSH_THRESH = 500;
	s_m68k_state.JIT_CONTROL = JCCF_SOFT
		| ((256 & JCCB_INSN_DEPTH_MASK) << JCCB_INSN_DEPTH)
		| ((8191 & JCCB_INLINE_RANGE_MASK) << JCCB_INLINE_RANGE)
		| ((8 & JCCB_LOOP_COUNT_MASK) << JCCB_LOOP_COUNT);
	s_m68k_state.JIT_CONTROL2 = (20 << JC2B_CCR_SCAN_DEPTH);
	if (UseJIT)
		s_m68k_state.CACR = 0x80008000;

	/*
	 * Musashi is the interpreter fallback and the register file Execute68k
	 * writes before emu68_sync_from_m68k(). Keep it in supervisor mode with
	 * the same ISP so Line-A frames land in RAM, not at address 0.
	 */
	m68k_set_reg(M68K_REG_SR, CPU_ENGINE_BOOT_SR);
	m68k_set_reg(M68K_REG_A7, CPU_ENGINE_INIT_SP);
	m68k_set_reg(M68K_REG_VBR, 0);

	return true;
}

/*
 * Signals the Emu68 engine to terminate execution and end current slice.
 */
static void emu68_exit(void)
{
	// Set quit flag to break continuous execution loops
	s_emu68_quit_requested = true;
	m68k_end_timeslice();
}

/*
 * Loads 680x0 CPU context into host ARM64 registers.
 *
 * Parameters:
 *   ctx - Pointer to M68KState context to load into CPU registers.
 */
/*
 * Loads 680x0 CPU context into the pinned JIT registers.
 *
 * This function is naked: a normal AAPCS prologue would save x19-x29 / d8-d15
 * and the matching epilogue would restore them, wiping the 68k D/A/FP image
 * (x29 is A7; Darwin also uses it as the frame pointer). The JIT entry point
 * is invoked immediately after this returns, so those registers must still
 * hold guest state.
 *
 * Parameters:
 *   ctx - Pointer to M68KState in x0 (AAPCS). Must remain valid until the
 *         matching M68K_SaveContext call after the JIT block returns.
 */
extern "C" __attribute__((naked)) void M68K_LoadContext(struct M68KState *ctx)
{
	__asm__ volatile(
		"mov " CTX_POINTER_ASM ", x0\n"
		"adrp x1, _emu68_host_mem_base@PAGE\n"
		"ldr x1, [x1, _emu68_host_mem_base@PAGEOFF]\n"
		"ins " HOST_MEM_BASE_ASM ", x1\n"
		"ldr w1, [x0, %[off_usp]]\n"
		"ins " REG_USP_ASM ", w1\n"
		"ldr w1, [x0, %[off_msp]]\n"
		"ins " REG_MSP_ASM ", w1\n"
		"ldr w1, [x0, %[off_isp]]\n"
		"ins " REG_ISP_ASM ", w1\n"
		"ldr x1, [x0, %[off_icnt]]\n"
		"ins " CTX_INSN_COUNT_ASM ", x1\n"
		"ldr w1, [x0, %[off_cacr]]\n"
		"ins " REG_CACR_ASM ", w1\n"
		"ldr w1, [x0, %[off_fpsr]]\n"
		"ins " REG_FPSR_ASM ", w1\n"
		"ldr w1, [x0, %[off_fpiar]]\n"
		"ins " REG_FPIAR_ASM ", w1\n"
		"ldrh w1, [x0, %[off_fpcr]]\n"
		"ins " REG_FPCR_ASM ", w1\n"
		"ldp w19, w20, [x0, %[off_d0]]\n"
		"ldp w21, w22, [x0, %[off_d2]]\n"
		"ldp w23, w24, [x0, %[off_d4]]\n"
		"ldp w25, w26, [x0, %[off_d6]]\n"
		"ldp w13, w14, [x0, %[off_a0]]\n"
		"ldp w15, w16, [x0, %[off_a2]]\n"
		"ldp w17, w27, [x0, %[off_a4]]\n"
		"ldp w28, w29, [x0, %[off_a6]]\n"
		"ldr w18, [x0, %[off_pc]]\n"
		"ldr d8,  [x0, %[off_fp0]]\n"
		"ldr d9,  [x0, %[off_fp1]]\n"
		"ldr d10, [x0, %[off_fp2]]\n"
		"ldr d11, [x0, %[off_fp3]]\n"
		"ldr d12, [x0, %[off_fp4]]\n"
		"ldr d13, [x0, %[off_fp5]]\n"
		"ldr d14, [x0, %[off_fp6]]\n"
		"ldr d15, [x0, %[off_fp7]]\n"
		"ldrh w1, [x0, %[off_sr]]\n"
		"rbit w2, w1\n"
		"bfxil w1, w2, #30, #2\n"
		"ins " REG_SR_ASM ", w1\n"
		"ret\n"
		:
		: [off_usp]   "i"(__builtin_offsetof(struct M68KState, USP)),
		  [off_msp]   "i"(__builtin_offsetof(struct M68KState, MSP)),
		  [off_isp]   "i"(__builtin_offsetof(struct M68KState, ISP)),
		  [off_icnt]  "i"(__builtin_offsetof(struct M68KState, INSN_COUNT)),
		  [off_cacr]  "i"(__builtin_offsetof(struct M68KState, CACR)),
		  [off_fpsr]  "i"(__builtin_offsetof(struct M68KState, FPSR)),
		  [off_fpiar] "i"(__builtin_offsetof(struct M68KState, FPIAR)),
		  [off_fpcr]  "i"(__builtin_offsetof(struct M68KState, FPCR)),
		  [off_d0]    "i"(__builtin_offsetof(struct M68KState, D[0])),
		  [off_d2]    "i"(__builtin_offsetof(struct M68KState, D[2])),
		  [off_d4]    "i"(__builtin_offsetof(struct M68KState, D[4])),
		  [off_d6]    "i"(__builtin_offsetof(struct M68KState, D[6])),
		  [off_a0]    "i"(__builtin_offsetof(struct M68KState, A[0])),
		  [off_a2]    "i"(__builtin_offsetof(struct M68KState, A[2])),
		  [off_a4]    "i"(__builtin_offsetof(struct M68KState, A[4])),
		  [off_a6]    "i"(__builtin_offsetof(struct M68KState, A[6])),
		  [off_pc]    "i"(__builtin_offsetof(struct M68KState, PC)),
		  [off_fp0]   "i"(__builtin_offsetof(struct M68KState, FP[0])),
		  [off_fp1]   "i"(__builtin_offsetof(struct M68KState, FP[1])),
		  [off_fp2]   "i"(__builtin_offsetof(struct M68KState, FP[2])),
		  [off_fp3]   "i"(__builtin_offsetof(struct M68KState, FP[3])),
		  [off_fp4]   "i"(__builtin_offsetof(struct M68KState, FP[4])),
		  [off_fp5]   "i"(__builtin_offsetof(struct M68KState, FP[5])),
		  [off_fp6]   "i"(__builtin_offsetof(struct M68KState, FP[6])),
		  [off_fp7]   "i"(__builtin_offsetof(struct M68KState, FP[7])),
		  [off_sr]    "i"(__builtin_offsetof(struct M68KState, SR))
	);
}

/*
 * Saves pinned JIT registers into M68KState.
 *
 * Naked for the same reason as LoadContext: a C prologue would `mov x29, sp`
 * and destroy A7 before the stores run. Called after a JIT block returns and
 * from the EmulOp trampoline while guest D/A/PC still live in x13-x29 / x18.
 *
 * Parameters:
 *   ctx - Pointer to M68KState in x0. Must be the same object LoadContext used.
 */
extern "C" __attribute__((naked)) void M68K_SaveContext(struct M68KState *ctx)
{
	__asm__ volatile(
		"mov w1, " REG_CACR_ASM "\n"
		"str w1, [x0, %[off_cacr]]\n"
		"mov x1, " CTX_INSN_COUNT_ASM "\n"
		"str x1, [x0, %[off_icnt]]\n"
		"mov w1, " REG_FPSR_ASM "\n"
		"str w1, [x0, %[off_fpsr]]\n"
		"mov w1, " REG_FPIAR_ASM "\n"
		"str w1, [x0, %[off_fpiar]]\n"
		"umov w1, " REG_FPCR_ASM "\n"
		"strh w1, [x0, %[off_fpcr]]\n"
		"stp w19, w20, [x0, %[off_d0]]\n"
		"stp w21, w22, [x0, %[off_d2]]\n"
		"stp w23, w24, [x0, %[off_d4]]\n"
		"stp w25, w26, [x0, %[off_d6]]\n"
		"stp w13, w14, [x0, %[off_a0]]\n"
		"stp w15, w16, [x0, %[off_a2]]\n"
		"stp w17, w27, [x0, %[off_a4]]\n"
		"stp w28, w29, [x0, %[off_a6]]\n"
		"str w18, [x0, %[off_pc]]\n"
		"str d8,  [x0, %[off_fp0]]\n"
		"str d9,  [x0, %[off_fp1]]\n"
		"str d10, [x0, %[off_fp2]]\n"
		"str d11, [x0, %[off_fp3]]\n"
		"str d12, [x0, %[off_fp4]]\n"
		"str d13, [x0, %[off_fp5]]\n"
		"str d14, [x0, %[off_fp6]]\n"
		"str d15, [x0, %[off_fp7]]\n"
		"umov w1, " REG_SR_ASM "\n"
		"rbit w2, w1\n"
		"bfxil w1, w2, #30, #2\n"
		"strh w1, [x0, %[off_sr]]\n"
		"mov w1, " REG_USP_ASM "\n"
		"str w1, [x0, %[off_usp]]\n"
		"mov w1, " REG_MSP_ASM "\n"
		"str w1, [x0, %[off_msp]]\n"
		"mov w1, " REG_ISP_ASM "\n"
		"str w1, [x0, %[off_isp]]\n"
		"ret\n"
		:
		: [off_usp]   "i"(__builtin_offsetof(struct M68KState, USP)),
		  [off_msp]   "i"(__builtin_offsetof(struct M68KState, MSP)),
		  [off_isp]   "i"(__builtin_offsetof(struct M68KState, ISP)),
		  [off_icnt]  "i"(__builtin_offsetof(struct M68KState, INSN_COUNT)),
		  [off_cacr]  "i"(__builtin_offsetof(struct M68KState, CACR)),
		  [off_fpsr]  "i"(__builtin_offsetof(struct M68KState, FPSR)),
		  [off_fpiar] "i"(__builtin_offsetof(struct M68KState, FPIAR)),
		  [off_fpcr]  "i"(__builtin_offsetof(struct M68KState, FPCR)),
		  [off_d0]    "i"(__builtin_offsetof(struct M68KState, D[0])),
		  [off_d2]    "i"(__builtin_offsetof(struct M68KState, D[2])),
		  [off_d4]    "i"(__builtin_offsetof(struct M68KState, D[4])),
		  [off_d6]    "i"(__builtin_offsetof(struct M68KState, D[6])),
		  [off_a0]    "i"(__builtin_offsetof(struct M68KState, A[0])),
		  [off_a2]    "i"(__builtin_offsetof(struct M68KState, A[2])),
		  [off_a4]    "i"(__builtin_offsetof(struct M68KState, A[4])),
		  [off_a6]    "i"(__builtin_offsetof(struct M68KState, A[6])),
		  [off_pc]    "i"(__builtin_offsetof(struct M68KState, PC)),
		  [off_fp0]   "i"(__builtin_offsetof(struct M68KState, FP[0])),
		  [off_fp1]   "i"(__builtin_offsetof(struct M68KState, FP[1])),
		  [off_fp2]   "i"(__builtin_offsetof(struct M68KState, FP[2])),
		  [off_fp3]   "i"(__builtin_offsetof(struct M68KState, FP[3])),
		  [off_fp4]   "i"(__builtin_offsetof(struct M68KState, FP[4])),
		  [off_fp5]   "i"(__builtin_offsetof(struct M68KState, FP[5])),
		  [off_fp6]   "i"(__builtin_offsetof(struct M68KState, FP[6])),
		  [off_fp7]   "i"(__builtin_offsetof(struct M68KState, FP[7])),
		  [off_sr]    "i"(__builtin_offsetof(struct M68KState, SR))
	);
}

/*
 * Naked JIT entry for 680x0 exceptions. SaveContext so emu68_exception_c can
 * use the host ABI, then LoadContext so A7/PC/SR are pinned again. Extra
 * format-2/4 words sit in the EMIT_Exception ARM frame below this trampoline.
 *
 * x29 is 680x0 A7. Restoring the host frame pointer after LoadContext would
 * throw away the ISP the C handler just wrote, and RTE would pop the trap
 * stub instead of the exception frame.
 *
 * AAPCS on entry: w0 = SR, w1 = type_and_format. Returns new SR in w0.
 */
extern "C" __attribute__((naked)) void M68K_Exception(void)
{
	__asm__ volatile(
		"stp x0, x1, [sp, #-16]!\n"
		"stp x29, x30, [sp, #-16]!\n"
		"mov x0, " CTX_POINTER_ASM "\n"
		"bl _M68K_SaveContext\n"
		"mov x29, sp\n"
		"ldp x0, x1, [sp, #16]\n"
		"ldr w2, [sp, #32]\n"
		"ldr w3, [sp, #40]\n"
		"bl _emu68_exception_c\n"
		"mov x0, " CTX_POINTER_ASM "\n"
		"bl _M68K_LoadContext\n"
		"ldr x30, [sp, #8]\n"
		"add sp, sp, #32\n"
		"umov w0, " REG_SR_ASM "\n"
		"ret\n"
	);
}

/*
 * True when a SIGILL PC is a leftover EL1 encoding inside the MAP_JIT pool.
 *
 * Parameters:
 *   pc   - Faulting host program counter from the signal ucontext.
 *   insn - AArch64 word at that PC.
 *
 * Returns:
 *   1 if the handler should skip 4 bytes and resume, else 0.
 */
extern "C" int emu68_hosted_try_skip_el1(uintptr_t pc, uint32_t insn)
{
	if (!s_jit_code_buffer)
		return 0;
	uintptr_t base = (uintptr_t)s_jit_code_buffer;
	if (pc < base || pc >= base + EMU68_CODE_CACHE_SIZE)
		return 0;
	return emu68_hosted_is_skippable_el1(pc, insn);
}

/*
 * C dispatcher for Basilisk EmulOp. Reads/writes only __m68k_state memory;
 * pinned JIT registers are saved/restored by the naked trampoline around this.
 *
 * Parameters:
 *   opcode - 0x7100 (EXEC_RETURN) or 0x7101..M68K_EMUL_OP_MAX.
 */
extern "C" void emu68_emulop_dispatch(uint32_t opcode)
{
	emu68_sync_to_m68k();

	if (opcode == M68K_EXEC_RETURN) {
		TriggerExecutionReturn();
		return;
	}

	if (opcode > M68K_EXEC_RETURN && opcode < M68K_EMUL_OP_MAX) {
		struct M68kRegisters r;

		for (int i = 0; i < 8; i++) {
			r.d[i] = __m68k_state->D[i].u32;
			r.a[i] = __m68k_state->A[i].u32;
		}
		r.sr = __m68k_state->SR;

#if defined(EMULOP_DEBUG) && EMULOP_DEBUG
		if (opcode != M68K_EMUL_OP_IRQ) {
			printf("[Emu68] EmulOp 0x%04X at PC=0x%08X\n", (unsigned)opcode, __m68k_state->PC);
			fflush(stdout);
		}
#endif

		EmulOp((uint16)opcode, &r);

		for (int i = 0; i < 8; i++)
			__m68k_state->D[i].u32 = r.d[i];
		for (int i = 0; i < 7; i++)
			__m68k_state->A[i].u32 = r.a[i];
		/*
		 * RESET writes A7 = 0x10000. Keep ISP (and MSP) in lockstep so
		 * LoadContext does not reload the 0x2000 boot stub into REG_ISP
		 * while x29 already has the BootGlobs stack.
		 */
		if (cpu_engine_should_commit_a7(__m68k_state->A[7].u32, r.a[7])) {
			__m68k_state->A[7].u32 = r.a[7];
			if ((r.sr & SR_S) && !(r.sr & SR_M))
				__m68k_state->ISP.u32 = r.a[7];
			else if (r.sr & SR_M)
				__m68k_state->MSP.u32 = r.a[7];
			else
				__m68k_state->USP.u32 = r.a[7];
		}
		__m68k_state->SR = r.sr;
		emu68_sync_to_m68k();
		return;
	}

	printf("[Emu68] Unhandled EmulOp 0x%04X at PC=0x%08X\n", (unsigned)opcode, __m68k_state->PC);
	fflush(stdout);
}

/*
 * JIT call-in for EmulOp. Saves pinned guest GPRs into __m68k_state, runs the
 * C dispatcher (which may clobber x19-x29), then reloads pinned registers.
 *
 * Parameters:
 *   opcode - Trap word in w0, already advanced past by the translator.
 */
extern "C" __attribute__((naked)) void emu68_hosted_emulop(uint32_t opcode)
{
	__asm__ volatile(
		"stp x0, x30, [sp, #-16]!\n"
		"mov x0, " CTX_POINTER_ASM "\n"
		"bl _M68K_SaveContext\n"
		"ldr w0, [sp]\n"
		"bl _emu68_emulop_dispatch\n"
		"mov x0, " CTX_POINTER_ASM "\n"
		"bl _M68K_LoadContext\n"
		"ldp x0, x30, [sp], #16\n"
		"ret\n"
	);
}

/*
 * === JIT AArch64 crash diagnostics ===
 *
 * Emu68 emits native AArch64 into a Darwin MAP_JIT buffer and then branches to
 * it. A SIGBUS/SIGSEGV/SIGILL inside that buffer has no C++ backtrace frame, so
 * the only way to identify the faulting operation is to dump the actual 32-bit
 * instruction words with host addresses. These helpers:
 *   1. Print every emitted insn after translation (before execution).
 *   2. On crash, locate the faulting PC in the last unit and mark it.
 * Privileged 680x0 ops and hosted memory accesses are the usual crash sources
 * (MSR DAIF, SYS/DC, WFI, or a load/store through HOST_MEM_BASE / a NULL GPR).
 */

/*
 * Formats a signed PC-relative immediate as a host target address string.
 *
 * Parameters:
 *   buf     - Output buffer receiving the formatted target.
 *   buflen  - Capacity of buf in bytes.
 *   insn_pc - Host address of the instruction containing the immediate.
 *   imm     - Byte displacement from insn_pc to the branch target.
 */
static void emu68_fmt_target(char *buf, size_t buflen, uintptr_t insn_pc, int64_t imm)
{
	snprintf(buf, buflen, "0x%llx", (unsigned long long)(insn_pc + (uintptr_t)imm));
}

/*
 * Produces a short AArch64 disassembly of one 32-bit instruction word.
 *
 * The decoder covers the encodings Emu68 actually emits (data processing,
 * load/store with host-memory indexing, branches, and system/privileged ops
 * such as MSR DAIF, SYS/DC, WFI) so a crash dump can name the faulting insn.
 *
 * Parameters:
 *   insn    - Little-endian AArch64 instruction word as fetched from JIT memory.
 *   insn_pc - Host address of this instruction, used to resolve PC-relative targets.
 *   buf     - Output buffer receiving a NUL-terminated mnemonic string.
 *   buflen  - Capacity of buf in bytes.
 */
static void emu68_format_a64(uint32_t insn, uintptr_t insn_pc, char *buf, size_t buflen)
{
	unsigned rt = insn & 31;
	unsigned rn = (insn >> 5) & 31;
	unsigned rd = rt;
	char tgt[32];

	if (insn == 0xD503201F) { snprintf(buf, buflen, "nop"); return; }
	if (insn == 0xD503203F) { snprintf(buf, buflen, "yield"); return; }
	if (insn == 0xD503205F) { snprintf(buf, buflen, "wfe"); return; }
	if (insn == 0xD503207F) { snprintf(buf, buflen, "wfi  ; PRIVILEGED/trappable on Darwin"); return; }
	if (insn == 0xD503209F) { snprintf(buf, buflen, "sev"); return; }
	if (insn == 0xD5033F9F) { snprintf(buf, buflen, "dsb sy"); return; }
	if (insn == 0xD5033FBF) { snprintf(buf, buflen, "dmb sy"); return; }
	if (insn == 0xD5033FDF) { snprintf(buf, buflen, "isb"); return; }
	if (insn == 0xFFFFFFFF) { snprintf(buf, buflen, "INVALID (0xffffffff block marker leftover)"); return; }
	if (insn == 0xFFFFFFFE) { snprintf(buf, buflen, "INVALID (0xfffffffe branch-fixup marker leftover)"); return; }

	// Unconditional B / BL (imm26, 4-byte units)
	if ((insn & 0xFC000000) == 0x14000000 || (insn & 0xFC000000) == 0x94000000) {
		int32_t imm26 = ((int32_t)(insn << 6)) >> 6;
		emu68_fmt_target(tgt, sizeof(tgt), insn_pc, (int64_t)imm26 * 4);
		snprintf(buf, buflen, "%s %s", (insn & 0x80000000) ? "bl" : "b", tgt);
		return;
	}

	// Conditional B.cond
	if ((insn & 0xFF000010) == 0x54000000) {
		static const char *cc[] = {
			"eq","ne","cs","cc","mi","pl","vs","vc",
			"hi","ls","ge","lt","gt","le","al","nv"
		};
		int32_t imm19 = ((int32_t)(insn << 8)) >> 13;
		emu68_fmt_target(tgt, sizeof(tgt), insn_pc, (int64_t)imm19 * 4);
		snprintf(buf, buflen, "b.%s %s", cc[insn & 15], tgt);
		return;
	}

	// CBZ / CBNZ
	if ((insn & 0x7E000000) == 0x34000000) {
		int is64 = (insn >> 31) & 1;
		int32_t imm19 = ((int32_t)(insn << 8)) >> 13;
		emu68_fmt_target(tgt, sizeof(tgt), insn_pc, (int64_t)imm19 * 4);
		snprintf(buf, buflen, "%s %c%u, %s",
		         (insn & 0x01000000) ? "cbnz" : "cbz",
		         is64 ? 'x' : 'w', rt, tgt);
		return;
	}

	// TBZ / TBNZ
	if ((insn & 0x7E000000) == 0x36000000) {
		unsigned bit = ((insn >> 19) & 0x1f) | ((insn >> 26) & 0x20);
		int32_t imm14 = ((int32_t)(insn << 13)) >> 18;
		emu68_fmt_target(tgt, sizeof(tgt), insn_pc, (int64_t)imm14 * 4);
		snprintf(buf, buflen, "%s %c%u, #%u, %s",
		         (insn & 0x01000000) ? "tbnz" : "tbz",
		         (bit >= 32) ? 'x' : 'w', rt, bit, tgt);
		return;
	}

	// BR / BLR / RET
	if ((insn & 0xFFFFFC1F) == 0xD61F0000) { snprintf(buf, buflen, "br x%u", rn); return; }
	if ((insn & 0xFFFFFC1F) == 0xD63F0000) { snprintf(buf, buflen, "blr x%u", rn); return; }
	if ((insn & 0xFFFFFC1F) == 0xD65F0000) { snprintf(buf, buflen, "ret x%u", rn); return; }

	// SVC / HVC / SMC / BRK
	if ((insn & 0xFFE0001F) == 0xD4000001) {
		snprintf(buf, buflen, "svc #0x%x", (insn >> 5) & 0xffff);
		return;
	}
	if ((insn & 0xFFE0001F) == 0xD4200000) {
		snprintf(buf, buflen, "brk #0x%x", (insn >> 5) & 0xffff);
		return;
	}

	// MSR (immediate) — PSTATE / DAIF. These are EL1-privileged on Darwin.
	if ((insn & 0xFFF8F01F) == 0xD500401F) {
		unsigned op1 = (insn >> 16) & 7;
		unsigned crm = (insn >> 8) & 15;
		unsigned op2 = (insn >> 5) & 7;
		if (op1 == 3 && op2 == 6)
			snprintf(buf, buflen, "msr daifset, #0x%x  ; PRIVILEGED", crm);
		else if (op1 == 3 && op2 == 7)
			snprintf(buf, buflen, "msr daifclr, #0x%x  ; PRIVILEGED", crm);
		else
			snprintf(buf, buflen, "msr pstate op1=%u CRm=%u op2=%u  ; PRIVILEGED", op1, crm, op2);
		return;
	}

	// MRS / MSR (system register)
	if ((insn & 0xFFF00000) == 0xD5300000 || (insn & 0xFFF00000) == 0xD5100000) {
		unsigned op0 = ((insn >> 19) & 1) + 2;
		unsigned op1 = (insn >> 16) & 7;
		unsigned crn = (insn >> 12) & 15;
		unsigned crm = (insn >> 8) & 15;
		unsigned op2 = (insn >> 5) & 7;
		snprintf(buf, buflen, "%s s%u_%u_c%u_c%u_%u, x%u  ; SYSREG",
		         ((insn & 0xFFF00000) == 0xD5300000) ? "mrs" : "msr",
		         op0, op1, crn, crm, op2, rt);
		return;
	}

	// SYS (DC/IC/AT/TLBI). DC CIVAC and friends are typically EL1 on Darwin.
	if ((insn & 0xFFF80000) == 0xD5080000) {
		unsigned op1 = (insn >> 16) & 7;
		unsigned crn = (insn >> 12) & 15;
		unsigned crm = (insn >> 8) & 15;
		unsigned op2 = (insn >> 5) & 7;
		if (op1 == 3 && crn == 7 && crm == 14 && op2 == 1)
			snprintf(buf, buflen, "dc civac, x%u  ; SYS", rt);
		else if (op1 == 3 && crn == 7 && crm == 10 && op2 == 1)
			snprintf(buf, buflen, "dc cvac, x%u  ; SYS", rt);
		else if (op1 == 3 && crn == 7 && crm == 5 && op2 == 1)
			snprintf(buf, buflen, "ic ivau, x%u  ; SYS", rt);
		else
			snprintf(buf, buflen, "sys op1=%u CRn=%u CRm=%u op2=%u, x%u  ; SYS", op1, crn, crm, op2, rt);
		return;
	}

	// Load/store unscaled / post-index / pre-index (GPR): bits[27:24]=1000, bit21=0, V=0
	if ((insn & 0x3F200000) == 0x38000000) {
		unsigned size = (insn >> 30) & 3;
		unsigned opc = (insn >> 22) & 3;
		unsigned mode = (insn >> 10) & 3; // 0=unscaled, 1=post, 3=pre
		int32_t imm9 = ((int32_t)(insn << 11)) >> 23;
		const char *mn = "?";
		char r = (size == 3) ? 'x' : 'w';
		if (opc == 0)
			mn = (size == 0) ? "strb" : (size == 1) ? "strh" : "str";
		else if (opc == 1)
			mn = (size == 0) ? "ldrb" : (size == 1) ? "ldrh" : "ldr";
		else if (opc == 2) {
			mn = (size == 0) ? "ldrsb" : (size == 1) ? "ldrsh" : "ldrsw";
			r = 'x';
		}
		char rnbuf[8];
		const char *base;
		if (rn == 31) {
			base = "sp";
		} else {
			snprintf(rnbuf, sizeof(rnbuf), "x%u", rn);
			base = rnbuf;
		}
		if (mode == 1)
			snprintf(buf, buflen, "%s %c%u, [%s], #%d", mn, r, rt, base, imm9);
		else if (mode == 3)
			snprintf(buf, buflen, "%s %c%u, [%s, #%d]!", mn, r, rt, base, imm9);
		else
			snprintf(buf, buflen, "%s %c%u, [%s, #%d]", mn, r, rt, base, imm9);
		return;
	}

	// Load/store unsigned immediate (GPR): size:31-30, bits[27:24]=1001, V=0
	if ((insn & 0x3F000000) == 0x39000000) {
		unsigned size = (insn >> 30) & 3;
		unsigned opc = (insn >> 22) & 3;
		unsigned imm12 = (insn >> 10) & 0xfff;
		unsigned scale = size;
		const char *mn = "?";
		char r = (size == 3) ? 'x' : 'w';
		if (opc == 0) {
			mn = (size == 0) ? "strb" : (size == 1) ? "strh" : "str";
		} else if (opc == 1) {
			mn = (size == 0) ? "ldrb" : (size == 1) ? "ldrh" : "ldr";
		} else if (opc == 2) {
			mn = (size == 0) ? "ldrsb" : (size == 1) ? "ldrsh" : "ldrsw";
			r = 'x';
		}
		snprintf(buf, buflen, "%s %c%u, [x%u, #%u]", mn, r, rt, rn, imm12 << scale);
		return;
	}

	// Load/store register offset (GPR): bits[27:24]=1000, bit21=1, bits[11:10]=10, V=0
	if ((insn & 0x3F200C00) == 0x38200800) {
		unsigned size = (insn >> 30) & 3;
		unsigned opc = (insn >> 22) & 3;
		unsigned rm = (insn >> 16) & 31;
		unsigned option = (insn >> 13) & 7;
		unsigned sbit = (insn >> 12) & 1;
		const char *mn = "?";
		char r = (size == 3) ? 'x' : 'w';
		if (opc == 0)
			mn = (size == 0) ? "strb" : (size == 1) ? "strh" : "str";
		else if (opc == 1)
			mn = (size == 0) ? "ldrb" : (size == 1) ? "ldrh" : "ldr";
		else if (opc == 2) {
			mn = (size == 0) ? "ldrsb" : (size == 1) ? "ldrsh" : "ldrsw";
			r = 'x';
		}
		const char *ext = (option == 2) ? "uxtw" : (option == 3) ? "lsl" : (option == 6) ? "sxtw" : (option == 7) ? "sxtx" : "ext";
		snprintf(buf, buflen, "%s %c%u, [x%u, %c%u, %s #%u]",
		         mn, r, rt, rn, (option & 1) ? 'x' : 'w', rm, ext, sbit ? size : 0);
		return;
	}

	// Load/store pair (STP/LDP): V=0 and bits[27:25] match the pair encoding class
	if ((insn & 0x3A000000) == 0x28000000) {
		unsigned opc = (insn >> 30) & 3;
		unsigned lbit = (insn >> 22) & 1;
		unsigned mode = (insn >> 23) & 3; // 1=post, 2=signed, 3=pre
		unsigned rt2 = (insn >> 10) & 31;
		int32_t imm7 = ((int32_t)(insn << 10)) >> 25;
		int scale = (opc == 2) ? 8 : 4;
		char r = (opc == 2) ? 'x' : 'w';
		int32_t off = imm7 * scale;
		const char *mn = lbit ? "ldp" : "stp";
		if (mode == 1)
			snprintf(buf, buflen, "%s %c%u, %c%u, [x%u], #%d", mn, r, rt, r, rt2, rn, off);
		else if (mode == 3)
			snprintf(buf, buflen, "%s %c%u, %c%u, [x%u, #%d]!", mn, r, rt, r, rt2, rn, off);
		else
			snprintf(buf, buflen, "%s %c%u, %c%u, [x%u, #%d]", mn, r, rt, r, rt2, rn, off);
		return;
	}

	// ADD/SUB immediate
	if ((insn & 0x1F000000) == 0x11000000) {
		int is64 = (insn >> 31) & 1;
		int is_sub = (insn >> 30) & 1;
		int setflags = (insn >> 29) & 1;
		unsigned shift = (insn >> 22) & 3;
		unsigned imm12 = (insn >> 10) & 0xfff;
		unsigned imm = imm12 << (shift ? 12 : 0);
		const char *mn = is_sub ? (setflags ? "subs" : "sub") : (setflags ? "adds" : "add");
		if (rn == 31 && !is_sub && !setflags)
			snprintf(buf, buflen, "mov %c%u, #0x%x", is64 ? 'x' : 'w', rd, imm);
		else
			snprintf(buf, buflen, "%s %c%u, %c%u, #0x%x", mn,
			         is64 ? 'x' : 'w', rd, is64 ? 'x' : 'w', rn, imm);
		return;
	}

	// MOVZ / MOVK / MOVN
	if ((insn & 0x1F800000) == 0x12800000) {
		int is64 = (insn >> 31) & 1;
		unsigned opc = (insn >> 29) & 3; // 0=MOVN, 2=MOVZ, 3=MOVK
		unsigned hw = (insn >> 21) & 3;
		unsigned imm16 = (insn >> 5) & 0xffff;
		const char *mn = (opc == 0) ? "movn" : (opc == 3) ? "movk" : "movz";
		snprintf(buf, buflen, "%s %c%u, #0x%x, lsl #%u", mn, is64 ? 'x' : 'w', rd, imm16, hw * 16);
		return;
	}

	// ADD/SUB shifted register
	if ((insn & 0x1F000000) == 0x0B000000) {
		int is64 = (insn >> 31) & 1;
		int is_sub = (insn >> 30) & 1;
		int setflags = (insn >> 29) & 1;
		unsigned rm = (insn >> 16) & 31;
		unsigned imm6 = (insn >> 10) & 63;
		unsigned shift = (insn >> 22) & 3;
		static const char *sh[] = { "lsl", "lsr", "asr", "ror" };
		const char *mn = is_sub ? (setflags ? "subs" : "sub") : (setflags ? "adds" : "add");
		snprintf(buf, buflen, "%s %c%u, %c%u, %c%u, %s #%u", mn,
		         is64 ? 'x' : 'w', rd, is64 ? 'x' : 'w', rn, is64 ? 'x' : 'w', rm, sh[shift], imm6);
		return;
	}

	// Logical shifted register: ORR with Rn=31 is MOV
	if ((insn & 0x1F000000) == 0x0A000000) {
		int is64 = (insn >> 31) & 1;
		unsigned opc = (insn >> 29) & 3;
		unsigned rm = (insn >> 16) & 31;
		unsigned imm6 = (insn >> 10) & 63;
		unsigned shift = (insn >> 22) & 3;
		static const char *sh[] = { "lsl", "lsr", "asr", "ror" };
		const char *mn = (opc == 0) ? "and" : (opc == 1) ? "orr" : (opc == 2) ? "eor" : "ands";
		if (opc == 1 && rn == 31 && imm6 == 0)
			snprintf(buf, buflen, "mov %c%u, %c%u", is64 ? 'x' : 'w', rd, is64 ? 'x' : 'w', rm);
		else
			snprintf(buf, buflen, "%s %c%u, %c%u, %c%u, %s #%u", mn,
			         is64 ? 'x' : 'w', rd, is64 ? 'x' : 'w', rn, is64 ? 'x' : 'w', rm, sh[shift], imm6);
		return;
	}

	// UMOV Wd, Vn.H[index] / UMOV Xd, Vn.D[index] (used for SR / HOST_MEM_BASE)
	if ((insn & 0xBFE0FC00) == 0x0E003C00) {
		unsigned imm5 = (insn >> 16) & 31;
		unsigned q = (insn >> 30) & 1;
		if (imm5 & 1)
			snprintf(buf, buflen, "umov w%u, v%u.b[%u]", rd, rn, imm5 >> 1);
		else if (imm5 & 2)
			snprintf(buf, buflen, "umov w%u, v%u.h[%u]", rd, rn, imm5 >> 2);
		else if (imm5 & 4)
			snprintf(buf, buflen, "umov %c%u, v%u.s[%u]", q ? 'x' : 'w', rd, rn, imm5 >> 3);
		else
			snprintf(buf, buflen, "umov x%u, v%u.d[%u]", rd, rn, imm5 >> 4);
		return;
	}

	// INS Vd.H[index], Wn (mov_reg_to_simd)
	if ((insn & 0xFFE0FC00) == 0x4E001C00) {
		unsigned imm5 = (insn >> 16) & 31;
		if (imm5 & 1)
			snprintf(buf, buflen, "ins v%u.b[%u], w%u", rd, imm5 >> 1, rn);
		else if (imm5 & 2)
			snprintf(buf, buflen, "ins v%u.h[%u], w%u", rd, imm5 >> 2, rn);
		else if (imm5 & 4)
			snprintf(buf, buflen, "ins v%u.s[%u], w%u", rd, imm5 >> 3, rn);
		else
			snprintf(buf, buflen, "ins v%u.d[%u], x%u", rd, imm5 >> 4, rn);
		return;
	}

	// ADR / ADRP
	if ((insn & 0x1F000000) == 0x10000000) {
		unsigned lo = (insn >> 29) & 3;
		int32_t immhi = ((int32_t)(insn << 8)) >> 13;
		int64_t imm = ((int64_t)immhi << 2) | lo;
		if (insn & 0x80000000) {
			imm <<= 12;
			emu68_fmt_target(tgt, sizeof(tgt), insn_pc & ~0xfffULL, imm);
			snprintf(buf, buflen, "adrp x%u, %s", rd, tgt);
		} else {
			emu68_fmt_target(tgt, sizeof(tgt), insn_pc, imm);
			snprintf(buf, buflen, "adr x%u, %s", rd, tgt);
		}
		return;
	}

	snprintf(buf, buflen, "???  (group bits[28:25]=0x%x)", (insn >> 25) & 0xf);
}

/*
 * Prints one AArch64 instruction as host address, LE bytes, word, and mnemonic.
 *
 * Parameters:
 *   addr     - Host address of the 4-byte instruction.
 *   insn     - Instruction word loaded from that address.
 *   fault_pc - Crash PC to mark, or 0 if not highlighting a fault.
 *   lr       - Crash LR to mark, or 0 if not highlighting a return address.
 */
static void emu68_print_a64_line(uintptr_t addr, uint32_t insn, uintptr_t fault_pc, uintptr_t lr)
{
	char desc[160];
	emu68_format_a64(insn, addr, desc, sizeof(desc));
	const char *mark = "";
	if (fault_pc && addr == (fault_pc & ~3ULL))
		mark = "  <<< FAULT";
	else if (lr && addr == (lr & ~3ULL))
		mark = "  <<< LR";
	printf("  [%+5lld] 0x%llx: %02x %02x %02x %02x  (0x%08x)  %s%s\n",
	       (long long)(int64_t)(addr - (uintptr_t)s_last_jit_entry),
	       (unsigned long long)addr,
	       insn & 0xff, (insn >> 8) & 0xff, (insn >> 16) & 0xff, (insn >> 24) & 0xff,
	       insn, desc, mark);
}


/*
 * Records the unit that is about to run so a crash handler can dump it, and
 * optionally prints every AArch64 instruction when emu68_jit_dump is set.
 *
 * Parameters:
 *   unit    - Translation unit whose mt_ARMCode[] is about to run.
 *   m68k_pc - 680x0 PC of the block (Macintosh address).
 *   opcode  - First 680x0 opcode word at m68k_pc.
 */
static void emu68_dump_jit_unit(struct M68KTranslationUnit *unit, uint32_t m68k_pc, uint16_t opcode)
{
	if (!unit || !unit->mt_ARMEntryPoint)
		return;

	uint32_t count = unit->mt_ARMInsnCnt;
	const uint32_t *code = (const uint32_t *)unit->mt_ARMEntryPoint;

	// Always keep last-unit metadata so SIGSEGV/SIGBUS dumps remain useful
	s_last_jit_entry = unit->mt_ARMEntryPoint;
	s_last_jit_insn_count = count;
	s_last_m68k_pc = m68k_pc;
	s_last_m68k_op = opcode;

	if (!s_emu68_jit_dump)
		return;

	printf("[Emu68 JIT dump] m68k PC=0x%08X opcode=0x%04X  ARM entry=%p  m68k_insns=%u arm_insns=%u\n",
	       m68k_pc, opcode, unit->mt_ARMEntryPoint, unit->mt_M68kInsnCnt, count);
	printf("[Emu68 JIT dump] ctx SR=0x%04X HOST_MEM_BASE=%p\n",
	       __m68k_state->SR, (void *)emu68_host_mem_base);
	printf("[Emu68 JIT dump] m68k words:");
	for (int i = 0; i < 6; i++)
		printf(" %04X", cache_read_16(ICACHE, m68k_pc + (uint32_t)(i * 2)));
	printf("\n");
	for (uint32_t i = 0; i < count; i++) {
		uintptr_t addr = (uintptr_t)&code[i];
		emu68_print_a64_line(addr, code[i], 0, 0);
	}
	fflush(stdout);
}

/*
 * Enables writes to the MAP_JIT pool. Called from C helpers that the JIT
 * branches to (e.g. check_cacr) while execute-only is in force.
 */
extern "C" void emu68_hosted_jit_write_enable(void)
{
	jit_write_enable();
}

/*
 * Restores execute-only on the MAP_JIT pool before returning to translated
 * AArch64 so the next instruction fetch is legal on Darwin W^X.
 */
extern "C" void emu68_hosted_jit_write_disable(void)
{
	jit_write_disable();
}

/*
 * Dumps AArch64 bytes around a crash PC and reports whether it sits in the
 * last translated JIT unit. Called from the SIGBUS/SIGSEGV/SIGILL/SIGSYS handler.
 *
 * Parameters:
 *   pc - Faulting host program counter.
 *   lr - Host link register at the time of the crash. Used as the dump
 *        centre when pc is not in the MAP_JIT pool (blr to a bad helper).
 */
extern "C" void emu68_jit_on_crash(uintptr_t pc, uintptr_t lr)
{
	uintptr_t aligned = pc & ~3ULL;
	printf("[Emu68 JIT crash] last m68k PC=0x%08X opcode=0x%04X  last ARM entry=%p arm_insns=%u\n",
	       s_last_m68k_pc, s_last_m68k_op, s_last_jit_entry, s_last_jit_insn_count);

	if (!s_jit_code_buffer) {
		printf("[Emu68 JIT crash] no JIT cache allocated\n");
		fflush(stdout);
		return;
	}

	uintptr_t lo = (uintptr_t)s_jit_code_buffer;
	uintptr_t hi = lo + EMU68_CODE_CACHE_SIZE;
	printf("[Emu68 JIT crash] JIT cache=[0x%llx, 0x%llx)  PC %s cache\n",
	       (unsigned long long)lo, (unsigned long long)hi,
	       (aligned >= lo && aligned < hi) ? "IS IN" : "is NOT in");

	if (s_last_jit_entry && s_last_jit_insn_count) {
		uintptr_t entry = (uintptr_t)s_last_jit_entry;
		uintptr_t end = entry + (uintptr_t)s_last_jit_insn_count * 4u;
		if (aligned >= entry && aligned < end) {
			// Report offset only — never dump a 2000-insn unit (it buries the fault and can stall the handler).
			printf("[Emu68 JIT crash] PC is +0x%llx (%u ARM insns) into last unit\n",
			       (unsigned long long)(aligned - entry),
			       (unsigned)((aligned - entry) / 4));
		} else {
			printf("[Emu68 JIT crash] PC is outside last translated unit\n");
		}
	} else {
		printf("[Emu68 JIT crash] no last unit recorded\n");
	}

	// Dump around PC when it is in the MAP_JIT pool. A blr to a bad helper
	// pointer leaves PC outside the cache; LR still names the call site.
	const int radius = 16;
	uintptr_t dump_at = aligned;
	if (!(dump_at >= lo && dump_at < hi) && (lr >= lo && lr < hi)) {
		dump_at = lr & ~3ULL;
		printf("[Emu68 JIT crash] dumping around LR (blr/ret site) instead of PC:\n");
	}
	if (dump_at >= lo + (uintptr_t)(radius * 4) && dump_at + (uintptr_t)(radius * 4) < hi) {
		printf("[Emu68 JIT crash] dumping ±%d insns at 0x%llx:\n",
		       radius, (unsigned long long)dump_at);
		for (int i = -radius; i <= radius; i++) {
			uintptr_t addr = dump_at + (uintptr_t)(i * 4);
			uint32_t insn = *(const uint32_t *)addr;
			emu68_print_a64_line(addr, insn, aligned, lr);
		}
	} else {
		printf("[Emu68 JIT crash] refusing ±%d dump; neither PC nor LR is safely inside the JIT cache\n", radius);
	}
	fflush(stdout);
}

/*
 * Executes a JIT compiled code block while preserving host AAPCS ABI conventions.
 *
 * Saves host AAPCS registers (x19-x29, x30, d8-d15), loads the 680x0 context
 * into pinned registers, executes the compiled AArch64 code block, writes back
 * the updated 680x0 context, and restores host registers.
 *
 * The branch to the unit must use an unpinned temp (x0-x12). x16 is 680x0 A3;
 * loading the host entry there left A3 equal to the ARM pointer after the
 * first unit, so ROM boot (4EFA / RESET 0x7103 / JMP) crashed on (A3).
 *
 * Parameters:
 *   entry_point - Pointer to executable AArch64 machine code in JIT buffer.
 */
static void emu68_call_jit_block(void *entry_point)
{
	__asm__ volatile(
		// 1. Save host AAPCS callee-saved registers
		"stp x19, x20, [sp, #-16]!\n"
		"stp x21, x22, [sp, #-16]!\n"
		"stp x23, x24, [sp, #-16]!\n"
		"stp x25, x26, [sp, #-16]!\n"
		"stp x27, x28, [sp, #-16]!\n"
		"stp x29, x30, [sp, #-16]!\n"
		"stp d8,  d9,  [sp, #-16]!\n"
		"stp d10, d11, [sp, #-16]!\n"
		"stp d12, d13, [sp, #-16]!\n"
		"stp d14, d15, [sp, #-16]!\n"
		"stp %0, %1, [sp, #-16]!\n"

		// 2. Load 680x0 context into pinned registers
		"mov x0, %1\n"
		"bl _M68K_LoadContext\n"

		// 3. Entry is on the stack; x8 is a translator temp, x16 is A3
		"ldr x8, [sp]\n"

		// 4. Branch and link to JIT code block
		"blr x8\n"

		// 5. Load valid ctx pointer from stack and save updated context
		"ldr x0, [sp, #8]\n"
		"bl _M68K_SaveContext\n"

		// 6. Clean up parameter frame
		"add sp, sp, #16\n"

		// 7. Restore host AAPCS callee-saved registers
		"ldp d14, d15, [sp], #16\n"
		"ldp d12, d13, [sp], #16\n"
		"ldp d10, d11, [sp], #16\n"
		"ldp d8,  d9,  [sp], #16\n"
		"ldp x29, x30, [sp], #16\n"
		"ldp x27, x28, [sp], #16\n"
		"ldp x25, x26, [sp], #16\n"
		"ldp x23, x24, [sp], #16\n"
		"ldp x21, x22, [sp], #16\n"
		"ldp x19, x20, [sp], #16\n"
		:
		: "r"(entry_point), "r"(__m68k_state)
		: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory"
	);
}

/*
 * Takes a pending Emu68 interrupt between translation units.
 *
 * ARM_err is NMI (level 7). VIA/SCC level is cpu_engine_intlev() (live
 * InterruptFlags), not a sticky INTF.IPL latch. Compared against SR IPL
 * the same way ExecutionLoop.c does on bare metal.
 */
static void emu68_hosted_poll_irq(void)
{
	/* Live intlev() like UAE — a latched IPL stays 1 after EmulOp IRQ
	 * clears InterruptFlags and storms vector 25 on every later block. */
	int via_scc = cpu_engine_intlev();
	__m68k_state->INTF.IPL = (uint8_t)via_scc;

	if (__m68k_state->INT64 == 0)
		return;

	int level = 0;
	if (__m68k_state->INTF.ARM_err) {
		level = 7;
		__m68k_state->INTF.ARM_err = 0;
	} else if (__m68k_state->INTF.ARM) {
		level = 6;
	} else if (via_scc) {
		level = via_scc;
	}

	int sr_ipl = (__m68k_state->SR >> SRB_IPL) & 7;
	if (level == 0 || (level <= sr_ipl && level != 7))
		return;

	uint32_t vector = VECTOR_INT_LEVEL1 + (uint32_t)(level - 1) * 4;
	emu68_exception_c(__m68k_state->SR, vector, 0, 0);
	uint16_t sr = __m68k_state->SR;
	sr = (uint16_t)((sr & ~SR_IPL) | ((level & 7) << SRB_IPL));
	__m68k_state->SR = sr;
	/* Musashi auto-clears CPU_INT_LEVEL on ack; drop the VIA/SCC latch. */
	__m68k_state->INTF.IPL = 0;
}

/*
 * Runs a short 680x0 slice for Execute68k / Execute68kTrap using Musashi.
 *
 * Musashi's illegal-instruction callback handles M68K_EXEC_RETURN (0x7100) and
 * EmulOp traps reliably. The JIT translator may include return stubs in a
 * larger block without emitting the hosted EmulOp hook, so test harness and
 * toolbox bridges stay on the interpreter.
 *
 * Parameters:
 *   timeslice_cycles - Maximum cycles to execute in this slice.
 */
static void emu68_run_execute_slice(int timeslice_cycles)
{
	emu68_sync_to_m68k();
	int executed = m68k_execute(timeslice_cycles);
	emu68_sync_from_m68k();
	(void)executed;
}

/*
 * Executes a slice of 680x0 instructions.
 *
 * When JIT is enabled (UseJIT is true), translates 680x0 blocks into native AArch64
 * code via M68K_GetTranslationUnit() and executes them in the Darwin JIT buffer.
 * When JIT is disabled (UseJIT is false), runs via the Musashi interpreter.
 *
 * Parameters:
 *   timeslice_cycles - Target number of cycles to execute in this time slice.
 */
static void emu68_run_jit_slice(int timeslice_cycles)
{
	int cycles = 0;
	while (cycles < timeslice_cycles && !s_emu68_quit_requested) {
		// Stop slice immediately if a nested execution return hook was triggered
		if (IsExecutionReturnTriggered())
			break;

		if (UseJIT) {
			uint32 pc = __m68k_state->PC;
			uint16 op = cache_read_16(ICACHE, pc);
			cpu_engine_note_pc(pc);

			/*
			 * Basilisk plants illegal MOVEQ encodings 0x7100..M68K_EMUL_OP_MAX
			 * as host calls (ROM patches, Execute68k return stubs). Handle them
			 * here so the translator never sees them as MOVEQ.
			 */
			if ((op & 0xff00) == 0x7100) {
				__m68k_state->PC = pc + 2;
				emu68_emulop_dispatch(op);
				cycles += 4;
				continue;
			}

			if (s_emu68_jit_dump) {
				printf("[Emu68 JIT] PC=0x%08X (opcode 0x%04X)\n", pc, op);
				fflush(stdout);
			}

			jit_write_enable();
			struct M68KTranslationUnit *unit = M68K_GetTranslationUnit((uint16_t *)(uintptr_t)pc);
			jit_write_disable();

			if (unit && unit->mt_ARMEntryPoint) {
				// Dump emitted AArch64 bytes before execution so a crash log names the faulting insn
				emu68_dump_jit_unit(unit, pc, op);
				emu68_call_jit_block(unit->mt_ARMEntryPoint);
				emu68_hosted_poll_irq();

				/*
				 * A cached block may have been translated before EmulOp hooks
				 * were emitted; re-check PC after every unit (RTS into 0x7100).
				 */
				pc = __m68k_state->PC;
				op = cache_read_16(ICACHE, pc);
				if ((op & 0xff00) == 0x7100) {
					__m68k_state->PC = pc + 2;
					emu68_emulop_dispatch(op);
					cycles += 4;
					if (IsExecutionReturnTriggered())
						break;
					continue;
				}

				int block_cycles = (unit->mt_M68kInsnCnt > 0) ? (unit->mt_M68kInsnCnt * 4) : 16;
				cycles += block_cycles;
			} else {
				// Fallback to single interpreter step if block translation not available
				emu68_sync_to_m68k();
				int executed = m68k_execute(100);
				emu68_sync_from_m68k();
				cycles += executed;
			}
		} else {
			// Non-JIT interpreter fallback execution
			emu68_sync_to_m68k();
			int executed = m68k_execute(5000);
			emu68_sync_from_m68k();
			cycles += executed;
		}

		if (IsExecutionReturnTriggered())
			break;
	}
}

/*
 * Injects 680x0 vector 2 after a PROT_NONE hole access in JIT or interpreter.
 *
 * Darwin W^X may have been left write-enabled if the fault hit during
 * translation; restore execute-only before resuming a translated block.
 */
static void emu68_on_guest_hole(void)
{
	emu68_hosted_jit_write_disable();
	uint32 fault = memory_guest_fault_addr();
	printf("[MEM] emu68 guest hole at 0x%08X (PC=0x%08X)\n", fault, __m68k_state->PC);
	fflush(stdout);
	emu68_exception_c(__m68k_state->SR, (2u << 12) | VECTOR_ACCESS_FAULT, fault, 0);
}

/*
 * Main execution entry point for the Emu68 engine.
 * Sets up the longjmp reset point and enters the execution loop.
 */
static void emu68_start(void)
{
	s_emu68_quit_requested = false;
	for (;;) {
		if (setjmp(s_emu68_reset_jmp) == 0) {
			// Normal boot sequence
			s_emu68_reset_valid = true;
			printf("[Emu68] Starting AArch64 JIT execution at 0x%08X...\n", ROMBaseMac + CPU_ENGINE_BOOT_PC_OFF);
			printf("[Emu68] First opcode at entry: 0x%04X\n", cache_read_16(ICACHE, ROMBaseMac + CPU_ENGINE_BOOT_PC_OFF));
			fflush(stdout);

			// Pulse CPU reset line to initialize internal hardware state
			m68k_pulse_reset();

			// Initialize boot registers: default SP, ROM entry point, interrupts masked
			m68k_set_reg(M68K_REG_A7, CPU_ENGINE_BOOT_SP);
			m68k_set_reg(M68K_REG_PC, ROMBaseMac + CPU_ENGINE_BOOT_PC_OFF);
			m68k_set_reg(M68K_REG_SR, CPU_ENGINE_BOOT_SR);

			emu68_sync_from_m68k();

			memory_fault_jmp_buf *guard = memory_guard_enter();
			while (!s_emu68_quit_requested) {
				if (MEMORY_FAULT_SETJMP(*guard) != 0) {
					emu68_on_guest_hole();
					continue;
				}
				emu68_run_jit_slice(50000);
			}
			memory_guard_leave();
			break;
		} else {
			memory_guard_clear();
			// Subsystem warm reset triggered via Reset680x0()
			printf("Reset680x0 (Emu68): Resetting machine subsystems...\n");
			fflush(stdout);

			// Reset all peripheral subsystems to initial clean states
			cpu_engine_reset_peripherals();

			s_emu68_quit_requested = false;
			MenuBar_UpdateAll();
		}
	}
	s_emu68_reset_valid = false;
}

/*
 * Performs a software reset of the 680x0 CPU.
 * Uses longjmp to jump back to the s_emu68_reset_jmp reset handler.
 */
static void emu68_reset(void)
{
	if (s_emu68_reset_valid) {
		longjmp(s_emu68_reset_jmp, 1);
	}
}

/*
 * Calculates the current highest pending interrupt request level.
 *
 * Returns:
 *   Interrupt level (0 = none, 1 = 60Hz VIA timer, 2/4 = SCC serial, 7 = NMI).
 */
static int emu68_intlev(void)
{
	return cpu_engine_intlev();
}

/*
 * Asserts the calculated pending interrupt level on the CPU core.
 */
static void emu68_trigger_interrupt(void)
{
	int level = cpu_engine_intlev();
	__m68k_state->INTF.IPL = (uint8_t)level;
	/* Musashi remains the Execute68k / trap-subroutine bridge. */
	m68k_set_irq(level);
}

/*
 * Triggers a non-maskable interrupt (NMI, level 7).
 */
static void emu68_trigger_nmi(void)
{
	__m68k_state->INTF.ARM_err = 1;
	m68k_set_irq(7);
}

/*
 * Soft-flushes the Emu68 JIT cache so forked test children do not inherit
 * stale translation units compiled before hosted glue fixes.
 */
static void emu68_invalidate_code(uint32 addr, uint32 size)
{
	(void)addr;
	(void)size;
	cache_invalidate_all(ICACHE);
}

/*
 * Executes a Mac OS Line-A or Toolbox trap subroutine and returns control to C++.
 *
 * Parameters:
 *   trap - 16-bit Line-A/Toolbox trap word (e.g., 0xA000 - 0xAFFF).
 *   r    - Pointer to 680x0 register structure containing in/out parameters.
 *
 * Domain Explanation:
 *   Constructs a synthetic 4-byte execution frame on the Mac stack (SP):
 *     [SP - 4]: Trap word (0xAxxx)
 *     [SP - 2]: M68K_EXEC_RETURN (0x7100 exit hook)
 *   Sets PC to this stub and executes until the EmulOp subsystem detects 0x7100,
 *   which signals the return condition and restores the caller context.
 */
static void emu68_execute_68k_trap(uint16 trap, struct M68kRegisters *r)
{
	Emu68CpuSnapshot outer;
	emu68_snapshot_cpu(&outer);

	// Load caller register arguments into 680x0 CPU core
	for (int i = 0; i < 8; i++)
		m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), r->d[i]);
	for (int i = 0; i < 7; i++)
		m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), r->a[i]);

	// Validate stack pointer against Mac RAM bounds to prevent corrupt pointer crashes
	uint32 sp = cpu_engine_clamp_sp(m68k_get_reg(NULL, M68K_REG_A7));
	uint32 stub = cpu_engine_write_trap_stub(sp, trap);
	m68k_set_reg(M68K_REG_A7, stub);
	m68k_set_reg(M68K_REG_PC, stub);

	bool return_seen = false;
	PushReturnStack(&return_seen);
	emu68_sync_from_m68k();
	memory_fault_jmp_buf *guard = memory_guard_enter();
	while (!return_seen && !s_emu68_quit_requested) {
		if (MEMORY_FAULT_SETJMP(*guard) != 0) {
			emu68_on_guest_hole();
			continue;
		}
		emu68_run_execute_slice(5000);
	}
	memory_guard_leave();
	emu68_sync_to_m68k();
	PopReturnStack();

	// Copy nested subroutine results to the host caller, then restore outer guest state
	for (int i = 0; i < 8; i++)
		r->d[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i));
	for (int i = 0; i < 7; i++)
		r->a[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i));
	r->a[7] = m68k_get_reg(NULL, M68K_REG_A7);
	r->sr = (uint16)m68k_get_reg(NULL, M68K_REG_SR);
	emu68_restore_cpu(&outer);
}

/*
 * Executes a 680x0 subroutine at `addr` and returns control to C++.
 *
 * Parameters:
 *   addr - 32-bit Macintosh address of the 680x0 subroutine to execute.
 *   r    - Pointer to 680x0 register structure containing in/out parameters.
 *
 * Domain Explanation:
 *   Constructs a synthetic 6-byte return frame on the Mac stack (SP):
 *     [SP - 2]: M68K_EXEC_RETURN (0x7100 exit hook)
 *     [SP - 6]: Return address pointing to [SP - 2]
 *   When the 680x0 subroutine executes RTS, it pops the return address, jumps
 *   to the M68K_EXEC_RETURN hook, and returns control cleanly back to host C++.
 */
static void emu68_execute_68k(uint32 addr, struct M68kRegisters *r)
{
	Emu68CpuSnapshot outer;
	emu68_snapshot_cpu(&outer);

	// Load caller register arguments into 680x0 CPU core
	for (int i = 0; i < 8; i++)
		m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), r->d[i]);
	for (int i = 0; i < 7; i++)
		m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), r->a[i]);

	// Validate stack pointer against Mac RAM bounds to prevent corrupt pointer crashes
	uint32 sp = cpu_engine_clamp_sp(m68k_get_reg(NULL, M68K_REG_A7));
	uint32 ret_addr = 0;
	sp = cpu_engine_write_exec_return_frame(sp, &ret_addr);
	m68k_set_reg(M68K_REG_A7, sp);
	m68k_set_reg(M68K_REG_PC, addr);

	bool return_seen = false;
	PushReturnStack(&return_seen);
	emu68_sync_from_m68k();
	memory_fault_jmp_buf *guard = memory_guard_enter();
	while (!return_seen && !s_emu68_quit_requested) {
		if (MEMORY_FAULT_SETJMP(*guard) != 0) {
			emu68_on_guest_hole();
			continue;
		}
		emu68_run_execute_slice(5000);
	}
	memory_guard_leave();
	emu68_sync_to_m68k();
	PopReturnStack();

	// Copy nested subroutine results to the host caller, then restore outer guest state
	for (int i = 0; i < 8; i++)
		r->d[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i));
	for (int i = 0; i < 7; i++)
		r->a[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i));
	r->a[7] = m68k_get_reg(NULL, M68K_REG_A7);
	r->sr = (uint16)m68k_get_reg(NULL, M68K_REG_SR);
	emu68_restore_cpu(&outer);
}

// Emu68 CPUEngine dispatch table
extern const CPUEngine emu68_cpu_engine = {
	"emu68",
	"Emu68 AArch64 Dynamic Binary Translation / JIT",
	true, // JIT enabled
	emu68_init,
	emu68_exit,
	emu68_start,
	emu68_reset,
	emu68_execute_68k,
	emu68_execute_68k_trap,
	emu68_trigger_interrupt,
	emu68_trigger_nmi,
	emu68_intlev,
	emu68_invalidate_code
};
