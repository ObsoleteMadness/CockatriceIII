/*
 *  memory.cpp - Unified 4GB Macintosh address window (all CPU engines)
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *  CockatriceIII Multi-Engine Architecture (C) 2026
 *
 *  Musashi, UAE, and m68k-rs share one 4GB virtual window
 *  (Host_Mem_Base) so Mac2HostAddr(addr) stays Host_Mem_Base + addr.
 *  Historic UAE kept the framebuffer and dummy NuBus slots out of the
 *  RAM translate; a RW-zero mmap of the whole 4GB made holes look like
 *  RAM (opcode 0x0000).
 *
 *  The window is reserved PROT_NONE / PAGE_NOACCESS (Wine/QEMU style)
 *  and only RAM, ROM, and the real framebuffer bytes are committed.
 *  Host SIGSEGV/SIGBUS or EXCEPTION_ACCESS_VIOLATION inside a guarded
 *  CPU run becomes 680x0 vector 2 (access fault). 68k instruction fetch
 *  is a data load, so committed pages are PROT_READ|PROT_WRITE without
 *  PROT_EXEC — host W^X still applies; NX is not a 68k execute/data split.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "scc.h"

// Global 4GB Flat Host Memory Window Base Pointer
uint8 *Host_Mem_Base = NULL;

/* Host page size; Apple Silicon is 16KB, Windows/x86 typically 4KB. */
static size_t s_page_size = 0;

/* Committed Macintosh ranges (page-aligned). Holes stay PROT_NONE. */
#define MEMORY_MAX_RANGES 16
static struct {
	uint32 start;
	uint32 end;
} s_mapped[MEMORY_MAX_RANGES];
static int s_nmapped = 0;

/* Nested sigsetjmp checkpoints for CPU run loops (start / Execute68k). */
#define MEMORY_GUARD_MAX 8
static memory_fault_jmp_buf s_guard_jmp[MEMORY_GUARD_MAX];
static int s_guard_depth = 0;
static uint32 s_guest_fault_addr = 0;
/* True: map the full 4GB window RW (legacy/JIT-safe mode). False: reserve the
 * full window PROT_NONE and only commit RAM/ROM/framebuffer ranges. */
static bool s_flat_dummy_window = true;

/* Local helpers used by memory_apply_window_policy(). */
static size_t memory_page_size(void);
static void memory_note_range(uint32 start, uint32 end);
static int memory_host_prot(int prot);

/*
 * Applies the selected host window policy to the live 4GB mapping and rebuilds
 * committed-range bookkeeping.
 */
static void memory_apply_window_policy(void)
{
	if (!Host_Mem_Base)
		return;

	const uint64 window_size = 0x100000000ULL;
	const int full_prot = s_flat_dummy_window
		? (MEMORY_PROT_READ | MEMORY_PROT_WRITE)
		: 0;

#ifdef _WIN32
	DWORD old_prot = 0;
	if (!VirtualProtect(Host_Mem_Base, (SIZE_T)window_size, (DWORD)memory_host_prot(full_prot), &old_prot)) {
		printf("[MEM] FATAL: VirtualProtect policy switch failed (err=%lu)\n",
		       (unsigned long)GetLastError());
		fflush(stdout);
		return;
	}
#else
	if (mprotect(Host_Mem_Base, (size_t)window_size, memory_host_prot(full_prot)) != 0) {
		printf("[MEM] FATAL: mprotect policy switch failed (%s)\n", strerror(errno));
		fflush(stdout);
		return;
	}
#endif

	s_nmapped = 0;
	if (s_flat_dummy_window) {
		memory_note_range(0, 0xffffffffU);
		printf("[MEM] flat 4GB RW dummy window at %p (page %zu)\n",
		       (void *)Host_Mem_Base, memory_page_size());
		fflush(stdout);
		return;
	}

	if (RAMSize > 0)
		memory_commit_range(RAMBaseMac, RAMSize, MEMORY_PROT_READ | MEMORY_PROT_WRITE);

	/*
	 * Keep the same ROM commit policy as memory_init(): a full 8MB Quadra
	 * window in 32-bit mode, minimum 1MB in 24-bit mode.
	 */
	uint32 rom_commit = TwentyFourBitAddressing ? (ROMSize > 0x100000 ? ROMSize : 0x100000)
	                                            : (ROMSize > 0x00800000 ? ROMSize : 0x00800000);
	memory_commit_range(ROMBaseMac, rom_commit, MEMORY_PROT_READ | MEMORY_PROT_WRITE);

	if (MacFrameLayout != FLAYOUT_NONE && MacFrameSize > 0)
		memory_commit_range(MacFrameBaseMac, MacFrameSize, MEMORY_PROT_READ | MEMORY_PROT_WRITE);

	printf("[MEM] strict-hole 4GB PROT_NONE window at %p (page %zu)\n",
	       (void *)Host_Mem_Base, memory_page_size());
	fflush(stdout);
}

#ifndef _WIN32
static struct sigaction s_old_sigsegv;
static struct sigaction s_old_sigbus;
static bool s_fault_signals_installed = false;
#endif

#ifdef _WIN32
static PVOID s_veh = NULL;
#endif

/*
 * Reads a single byte from the Z8530 Serial Communications Controller (SCC).
 *
 * Arguments:
 *   addr: 32-bit Macintosh address in the SCC MMIO range.
 *
 * Returns:
 *   8-bit register value returned by SCC_Access.
 */
uint32 scc_bget(uint32 addr)
{
	uint32 a24 = addr & 0x00ffffff;
	// In 24-bit mode, SCC registers are at 0x00900000; in 32-bit mode at 0x50000000
	if (TwentyFourBitAddressing) {
		return SCC_Access(0, false, (a24 >> 1) & 3);
	} else {
		return SCC_Access(0, false, (addr >> 1) & 3);
	}
}

/*
 * Writes a single byte to the Z8530 Serial Communications Controller (SCC).
 *
 * Arguments:
 *   addr: 32-bit Macintosh address in the SCC MMIO range.
 *   b: 8-bit value to write to the SCC register.
 */
void scc_bput(uint32 addr, uint32 b)
{
	uint32 a24 = addr & 0x00ffffff;
	// Dispatch write to SCC register based on current addressing mode
	if (TwentyFourBitAddressing) {
		SCC_Access((uint8)b, true, (a24 >> 1) & 3);
	} else {
		SCC_Access((uint8)b, true, (addr >> 1) & 3);
	}
}

/*
 * Returns the host page size used to align mprotect / VirtualAlloc commits.
 *
 * Returns:
 *   Page size in bytes (16KB on Apple Silicon, typically 4KB elsewhere).
 */
static size_t memory_page_size(void)
{
	if (s_page_size)
		return s_page_size;
#ifdef _WIN32
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	s_page_size = si.dwPageSize;
#else
	long n = sysconf(_SC_PAGESIZE);
	s_page_size = (n > 0) ? (size_t)n : 4096;
#endif
	return s_page_size;
}

/*
 * Records a committed Macintosh range so memory_is_mapped() can distinguish
 * holes from RAM/ROM/framebuffer without probing the OS.
 *
 * Arguments:
 *   start: Inclusive 32-bit Mac start (already page-aligned).
 *   end: Exclusive 32-bit Mac end (already page-aligned).
 */
static void memory_note_range(uint32 start, uint32 end)
{
	for (int i = 0; i < s_nmapped; i++) {
		if (start <= s_mapped[i].end && end >= s_mapped[i].start) {
			if (start < s_mapped[i].start)
				s_mapped[i].start = start;
			if (end > s_mapped[i].end)
				s_mapped[i].end = end;
			return;
		}
	}
	if (s_nmapped >= MEMORY_MAX_RANGES)
		return;
	s_mapped[s_nmapped].start = start;
	s_mapped[s_nmapped].end = end;
	s_nmapped++;
}

/*
 * Translates Cockatrice MEMORY_PROT_* bits to a host protection value.
 *
 * Arguments:
 *   prot: Bitmask of MEMORY_PROT_READ / WRITE / EXEC.
 *
 * Returns:
 *   POSIX PROT_* or Windows PAGE_* constant. 68k fetch is a data load, so
 *   callers normally pass READ|WRITE and leave EXEC off (host W^X).
 */
static int memory_host_prot(int prot)
{
#ifdef _WIN32
	int exec = (prot & MEMORY_PROT_EXEC) != 0;
	int wr = (prot & MEMORY_PROT_WRITE) != 0;
	if (exec && wr)
		return PAGE_EXECUTE_READWRITE;
	if (exec)
		return PAGE_EXECUTE_READ;
	if (wr)
		return PAGE_READWRITE;
	return PAGE_READONLY;
#else
	int hp = PROT_NONE;
	if (prot & MEMORY_PROT_READ)
		hp |= PROT_READ;
	if (prot & MEMORY_PROT_WRITE)
		hp |= PROT_WRITE;
	if (prot & MEMORY_PROT_EXEC)
		hp |= PROT_EXEC;
	return hp;
#endif
}

/*
 * Commits a Macintosh range inside the reserved 4GB window.
 *
 * Rounds to the host page size so a 16KB Apple Silicon page is never left
 * half-PROT_NONE. Overlapping commits are merged in the range table.
 *
 * Arguments:
 *   mac_addr: 32-bit Macintosh start address.
 *   size: Length in bytes (0 is a no-op).
 *   prot: MEMORY_PROT_READ / WRITE / EXEC bits.
 */
void memory_commit_range(uint32 mac_addr, uint32 size, int prot)
{
	if (!Host_Mem_Base || size == 0)
		return;

	size_t page = memory_page_size();
	uint64 start = (uint64)mac_addr & ~((uint64)page - 1);
	uint64 end = ((uint64)mac_addr + size + page - 1) & ~((uint64)page - 1);
	if (end > 0x100000000ULL)
		end = 0x100000000ULL;
	if (end <= start)
		return;

	size_t bytes = (size_t)(end - start);
	uint8 *host = Host_Mem_Base + start;
	int hp = memory_host_prot(prot);

#ifdef _WIN32
	if (!VirtualAlloc(host, bytes, MEM_COMMIT, (DWORD)hp)) {
		printf("[MEM] FATAL: VirtualAlloc COMMIT failed at Mac 0x%08X size 0x%zx (err=%lu)\n",
		       (uint32)start, bytes, (unsigned long)GetLastError());
		fflush(stdout);
		return;
	}
#else
	if (mprotect(host, bytes, hp) != 0) {
		printf("[MEM] FATAL: mprotect failed at Mac 0x%08X size 0x%zx (%s)\n",
		       (uint32)start, bytes, strerror(errno));
		fflush(stdout);
		return;
	}
#endif
	memory_note_range((uint32)start, (uint32)end);
}

/*
 * Returns whether [addr, addr+size) lies entirely in a committed range.
 *
 * Arguments:
 *   addr: 32-bit Macintosh address.
 *   size: Access length in bytes (0 is treated as mapped).
 *
 * Returns:
 *   true if every byte is in RAM, ROM, framebuffer, or another commit.
 */
bool memory_is_mapped(uint32 addr, uint32 size)
{
	if (size == 0)
		return true;
	uint64 last = (uint64)addr + (uint64)size - 1;
	if (last > 0xffffffffULL)
		return false;
	bool start_ok = false, last_ok = false;
	for (int i = 0; i < s_nmapped; i++) {
		if (addr >= s_mapped[i].start && addr < s_mapped[i].end)
			start_ok = true;
		if ((uint32)last >= s_mapped[i].start && (uint32)last < s_mapped[i].end)
			last_ok = true;
	}
	return start_ok && last_ok;
}

/*
 * Copies the current committed-range table so a caller can check
 * memory_is_mapped() locally instead of paying a per-access callback (the
 * m68k-rs FFI bridge's checked memory path does this: the table only
 * changes at boot, but every guest memory access outside FastMem used to
 * cross the host callback for this check).
 *
 * Arguments:
 *   out_start / out_end: Parallel arrays, at least max_ranges entries.
 *   max_ranges: Capacity of out_start/out_end.
 *
 * Returns:
 *   Number of ranges copied (<= max_ranges).
 */
int memory_get_mapped_ranges(uint32 *out_start, uint32 *out_end, int max_ranges)
{
	int n = s_nmapped < max_ranges ? s_nmapped : max_ranges;
	for (int i = 0; i < n; i++) {
		out_start[i] = s_mapped[i].start;
		out_end[i] = s_mapped[i].end;
	}
	return n;
}

/*
 * Commits the NuBus framebuffer bytes at MacFrameBaseMac.
 *
 * Classic video keeps the screen in RAM (FLAYOUT_NONE) so 0xA0000000 stays
 * a hole, matching historic dummy banks. Mac II / Quadra only map MacFrameSize,
 * not the whole 256MB slot — a jump to 0xA0000000 + RAM offset then faults
 * instead of fetching zeros.
 */
void memory_map_framebuffer(void)
{
	if (!Host_Mem_Base)
		return;
	MacFrameBaseHost = Host_Mem_Base + MacFrameBaseMac;
	if (MacFrameLayout == FLAYOUT_NONE || MacFrameSize == 0)
		return;
	memory_commit_range(MacFrameBaseMac, MacFrameSize, MEMORY_PROT_READ | MEMORY_PROT_WRITE);
	printf("[MEM] framebuffer committed at 0x%08X + %u bytes (layout %d)\n",
	       MacFrameBaseMac, MacFrameSize, MacFrameLayout);
	fflush(stdout);
}

/*
 * Pushes a guest-fault checkpoint. Pair with memory_guard_leave().
 *
 * The caller must MEMORY_FAULT_SETJMP(*returned_buf) in the same function;
 * hiding sigsetjmp in a helper would invalidate the jmp_buf on return.
 *
 * Returns:
 *   Pointer to the jmp_buf for this nesting level.
 */
memory_fault_jmp_buf *memory_guard_enter(void)
{
	if (s_guard_depth >= MEMORY_GUARD_MAX) {
		printf("[MEM] FATAL: guest-fault guard nesting overflow\n");
		fflush(stdout);
		abort();
	}
	return &s_guard_jmp[s_guard_depth++];
}

/*
 * Pops the guest-fault checkpoint pushed by memory_guard_enter().
 */
void memory_guard_leave(void)
{
	if (s_guard_depth > 0)
		s_guard_depth--;
}

/*
 * Drops every nested guest-fault checkpoint.
 *
 * Reset680x0 longjmps out of the CPU loop and skips memory_guard_leave(),
 * so the reset path must clear the depth or the next start() would overflow.
 */
void memory_guard_clear(void)
{
	s_guard_depth = 0;
}

/*
 * Returns the Macintosh address of the last guarded hole access.
 *
 * Returns:
 *   32-bit Mac address that raised SIGSEGV / ACCESS_VIOLATION.
 */
uint32 memory_guest_fault_addr(void)
{
	return s_guest_fault_addr;
}

/*
 * Raises a guest access fault when software detects an unmapped address before
 * dereferencing Host_Mem_Base (avoids host SIGSEGV outside the 4GB window).
 *
 * Arguments:
 *   addr: 32-bit Macintosh address that would fault.
 *
 * Does not return when a memory_guard_enter() checkpoint is active.
 */
void memory_raise_guest_fault(uint32 addr)
{
	if (s_guard_depth <= 0)
		return;

	s_guest_fault_addr = addr;
#ifdef _WIN32
	longjmp(s_guard_jmp[s_guard_depth - 1], 1);
#else
	siglongjmp(s_guard_jmp[s_guard_depth - 1], 1);
#endif
}

/*
 * Selects the host window policy used by the next memory_init() call.
 *
 * Arguments:
 *   flat_dummy: true keeps the whole 4GB RW dummy-backed (legacy behavior);
 *               false enables PROT_NONE holes with range commits only.
 */
void memory_set_flat_dummy_window(bool flat_dummy)
{
	s_flat_dummy_window = flat_dummy;
}

/*
 * Reapplies the selected host window policy to an already-created mapping.
 * Safe to call after RAM/ROM/framebuffer metadata changes.
 */
void memory_reconfigure_window(void)
{
	memory_apply_window_policy();
}

/*
 * Converts a host fault inside the 4GB window into a longjmp to the CPU loop.
 *
 * Arguments:
 *   si_addr: Faulting host address from siginfo or ExceptionInformation[1].
 *
 * Returns:
 *   0 if the fault is not a guarded guest hole (caller should crash-dump).
 *   Does not return when a CPU run loop is armed and si_addr is an unmapped
 *   Macintosh address — siglongjmp resumes that loop to inject vector 2.
 */
int memory_try_handle_guest_fault(const void *si_addr)
{
	if (s_guard_depth <= 0 || !Host_Mem_Base || !si_addr)
		return 0;

	const uint8 *p = (const uint8 *)si_addr;
	if (p < Host_Mem_Base || p >= Host_Mem_Base + 0x100000000ULL)
		return 0;

	uint32 guest = (uint32)(p - Host_Mem_Base);
	if (memory_is_mapped(guest, 1))
		return 0;

	s_guest_fault_addr = guest;
#ifdef _WIN32
	longjmp(s_guard_jmp[s_guard_depth - 1], 1);
#else
	siglongjmp(s_guard_jmp[s_guard_depth - 1], 1);
#endif
	return 0;
}

#ifndef _WIN32
/*
 * SIGSEGV/SIGBUS trampoline: guest holes longjmp, anything else chains.
 *
 * Arguments:
 *   sig: SIGSEGV or SIGBUS.
 *   info: Faulting address in si_addr.
 *   ucontext: Passed through to a previously installed crash handler.
 */
static void memory_unix_fault(int sig, siginfo_t *info, void *ucontext)
{
	if (info)
		memory_try_handle_guest_fault(info->si_addr);

	struct sigaction *old = (sig == SIGBUS) ? &s_old_sigbus : &s_old_sigsegv;
	if ((old->sa_flags & SA_SIGINFO) && old->sa_sigaction) {
		old->sa_sigaction(sig, info, ucontext);
		return;
	}
	if (old->sa_handler == SIG_IGN)
		return;
	signal(sig, SIG_DFL);
	raise(sig);
}

/*
 * Installs the guest-hole trampoline in front of any existing SIGSEGV/SIGBUS
 * handler (main_sdl crash_handler is typically already installed).
 */
static void memory_install_unix_fault(void)
{
	if (s_fault_signals_installed)
		return;
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = memory_unix_fault;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, &s_old_sigsegv);
	sigaction(SIGBUS, &sa, &s_old_sigbus);
	s_fault_signals_installed = true;
}
#endif

#ifdef _WIN32
/*
 * First-chance VEH: ACCESS_VIOLATION inside a guarded hole longjmps to
 * the CPU loop. Other exceptions continue the search (DrMinGW, SEH, etc.).
 *
 * Arguments:
 *   info: Windows exception pointers; ExceptionInformation[1] is the address.
 *
 * Returns:
 *   EXCEPTION_CONTINUE_SEARCH when the fault is not a guarded guest hole.
 */
static LONG CALLBACK memory_veh(PEXCEPTION_POINTERS info)
{
	if (!info || !info->ExceptionRecord)
		return EXCEPTION_CONTINUE_SEARCH;
	if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
		return EXCEPTION_CONTINUE_SEARCH;
	const void *addr = (const void *)info->ExceptionRecord->ExceptionInformation[1];
	memory_try_handle_guest_fault(addr);
	return EXCEPTION_CONTINUE_SEARCH;
}
#endif

/*
 * Initializes the unified 4GB memory window.
 *
 * Mode A (flat dummy, default): commit/map the whole 4GB window RW. This keeps
 * all hole reads/writes non-faulting and avoids longjmp exits from JIT compile
 * paths that dereference guest pointers directly.
 *
 * Mode B (strict holes): reserve/map the full window PROT_NONE and commit only
 * RAM/ROM/framebuffer ranges. This restores real unmapped-hole behavior (bus/
 * address fault semantics via memory_try_handle_guest_fault()) for engines that
 * can tolerate it.
 */
void memory_init(void)
{
	if (!Host_Mem_Base) {
#ifdef _WIN32
		if (s_flat_dummy_window) {
			Host_Mem_Base = (uint8 *)VirtualAlloc(NULL, 0x100000000ULL, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		} else {
			Host_Mem_Base = (uint8 *)VirtualAlloc(NULL, 0x100000000ULL, MEM_RESERVE, PAGE_NOACCESS);
		}
		if (Host_Mem_Base && !s_veh)
			s_veh = AddVectoredExceptionHandler(1, memory_veh);
#else
		Host_Mem_Base = (uint8 *)mmap(NULL, 0x100000000ULL,
					      s_flat_dummy_window ? (PROT_READ | PROT_WRITE) : PROT_NONE,
					      MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
		if (Host_Mem_Base == MAP_FAILED)
			Host_Mem_Base = NULL;
		memory_install_unix_fault();
#endif
		if (Host_Mem_Base) {
			memory_apply_window_policy();
		}
	}

	if (!Host_Mem_Base) {
		printf("[MEM] FATAL: Failed to reserve 4GB host memory window!\n");
		fflush(stdout);
		return;
	}

	uint8 *old_rom_host = ROMBaseHost;

	RAMBaseHost = Host_Mem_Base + RAMBaseMac;
	ROMBaseHost = Host_Mem_Base + ROMBaseMac;
	MacFrameBaseHost = Host_Mem_Base + MacFrameBaseMac;

	/* Refresh the mapping policy and committed ranges on repeated init calls. */
	memory_apply_window_policy();

	if (old_rom_host && old_rom_host != ROMBaseHost && ROMSize > 0)
		memmove(ROMBaseHost, old_rom_host, ROMSize);
}

/*
 * Releases the unified 4GB host window allocated by memory_init().
 *
 * RAMBaseHost / ROMBaseHost / MacFrameBaseHost are aliases into that window
 * and must not be delete[]'d. Safe to call when the window was never allocated.
 */
void memory_exit(void)
{
	if (!Host_Mem_Base)
		return;
#ifdef _WIN32
	if (s_veh) {
		RemoveVectoredExceptionHandler(s_veh);
		s_veh = NULL;
	}
	VirtualFree(Host_Mem_Base, 0, MEM_RELEASE);
#else
	munmap(Host_Mem_Base, 0x100000000ULL);
#endif
	Host_Mem_Base = NULL;
	RAMBaseHost = NULL;
	ROMBaseHost = NULL;
	MacFrameBaseHost = NULL;
	s_nmapped = 0;
	s_guard_depth = 0;
}

/*
 * Translates a 32-bit Macintosh guest address to a flat host pointer.
 *
 * Arguments:
 *   addr: 32-bit Macintosh address.
 *
 * Returns:
 *   Direct host pointer into the 4GB memory window.
 */
uint8 *get_real_address(uint32 addr)
{
	return Mac2HostAddr(addr);
}

/*
 * Translates a host pointer within the 4GB window back to a 32-bit Macintosh address.
 *
 * Arguments:
 *   addr: Host pointer inside Host_Mem_Base window.
 *
 * Returns:
 *   32-bit Macintosh guest address.
 */
uint32 get_virtual_address(uint8 *addr)
{
	return Host2MacAddr(addr);
}

