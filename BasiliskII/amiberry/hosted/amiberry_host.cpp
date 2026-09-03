/*
 *  amiberry_host.cpp - Amiga-chipset stubs and Amiberry CPU lifecycle for Cockatrice
 *
 *  Macintosh emulation has no Agnus/Paula; these symbols exist so newcpu.cpp
 *  and the JIT link. Cycle counting still advances so m68k_run() can timeslice
 *  back to the SDL event loop via SPCFLAG_MODE_CHANGE.
 */

#include "sysconfig.h"
#include "sysdeps.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "options.h"
#include "events.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "gui.h"
#include "savestate.h"
#include "debug.h"
#include "devices.h"
#include "audio.h"
#include "fpp.h"
#include "compemu.h"
#include "inputrecord.h"
#include "inputdevice.h"
#include "autoconf.h"
#include "cpuboard.h"
#include "blitter.h"
#include "statusline.h"

#include "amiberry_cpu_api.h"

struct uae_prefs currprefs;
struct uae_prefs changed_prefs;

int quit_program;
static int s_m68k_execute_depth;
static volatile int s_nested_quit_program;
int vpos;
bool lof_store;
uae_u16 beamcon0;
uae_u16 dmacon;
uae_u16 intena, intreq, intreqr;
uae_u8 agnus_hpos;
int hpos;
volatile uae_atomic uae_int_requested;
int inputrecord_debug;
uae_u32 hsync_counter;
uae_u32 vsync_counter;

evt_t currcycle;
evt_t currcycle_cck;
evt_t nextevent;
evt_t vsync_cycles;
evt_t start_cycles;
int pissoff_value;
int pissoff_nojit_value;
int maxhpos = 227;
int custom_fastmode;
int is_syncline;
evt_t is_syncline_end;
bool event_wait;
frame_time_t vsyncmintime, vsyncmintimepre;
frame_time_t vsyncmaxtime, vsyncwaittime;
frame_time_t vsynctimebase, cputimebase, syncbase;
struct ev eventtab[ev_max];
struct ev2 eventtab2[ev2_max];

int savestate_state;
int debugging;
int debug_illegal;
int debug_illegal_mask;
int input_play;
int input_record;
struct gui_info gui_data;

bool cloanto_rom;
bool kickstart_rom;
uae_u16 kickstart_version;
int uae_boot_rom_type;
int uae_boot_rom_size;
uaecptr rtarea_base;
bool rom_write_enabled;
bool canbang;
bool jit_direct_compatible_memory;
uaecptr highest_ram;
uae_u8 *natmem_offset;
uae_u8 *natmem_reserved;
size_t natmem_reserved_size;

int special_mem;
int special_mem_default;
int jit_n_addr_unsafe;
int jit_n_addr_bank_unsafe;

static evt_t s_slice_cycles;
static const evt_t kSliceLimit = (evt_t)40000 * CYCLE_UNIT;

/*
 * Writes a UAE-style log line to stdout so JIT/CPU diagnostics are visible
 * in the Cockatrice console without Amiberry's GUI logger.
 */
void write_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

void uae_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

uae_time_t uae_time(void)
{
	return (uae_time_t)currcycle;
}

void uae_time_init(void) {}
void uae_time_calibrate(void) {}

/*
 * Advances the hosted cycle clock. After enough cycles, raise MODE_CHANGE so
 * m68k_run() returns to Basilisk's event/interrupt loop (there is no Agnus hsync).
 */
void do_cycles_slow(int cycles_to_add)
{
	currcycle += cycles_to_add;
	s_slice_cycles += cycles_to_add;
	if (s_slice_cycles > kSliceLimit) {
		s_slice_cycles = 0;
		set_special(SPCFLAG_MODE_CHANGE);
	}
}

void do_cycles_normal(int cycles_to_add)
{
	do_cycles_slow(cycles_to_add);
}

void do_cycles_ce(int cycles_to_add)
{
	do_cycles_slow(cycles_to_add);
}

void do_cycles_ce020(int cycles_to_add)
{
	do_cycles_slow(cycles_to_add);
}

void do_cycles_ce020_internal(int clocks)
{
	do_cycles_slow(clocks);
}

int do_cycles_cck(int cycles)
{
	do_cycles_slow(cycles);
	return cycles;
}

void events_schedule(void) {}
void events_reset_syncline(void) {}
void events_reset_synchandler(void) {}
void clear_events(void) {}
void reset_frame_rate_hack(void) {}
void event_init(void) {}
void compute_vsynctime(void) {}
void init_eventtab(void) {}
bool is_cycle_ce(uaecptr) { return false; }
void MISC_handler(void) {}
void event2_newevent_xx(int, evt_t, uae_u32, evfunc2) {}
void event2_newevent_x_replace(evt_t, uae_u32, evfunc2) {}
void event2_newevent_x_replace_exists(evt_t, uae_u32, evfunc2) {}
void event2_newevent_x_add_not_exists(evt_t, uae_u32, evfunc2) {}
void event2_newevent_x_remove(evfunc2) {}
void event2_newevent_xx_ce(evt_t, uae_u32, evfunc2) {}
bool event2_newevent_x_exists(evfunc2) { return false; }
void event_audxdat_func(uae_u32) {}
void event_setdsr(uae_u32) {}
void event_CIA_synced_interrupt(uae_u32) {}
void event_CIA_tod_inc_event(uae_u32) {}
void event_DISK_handler(uae_u32) {}

void custom_reset(bool, bool) {}
void customreset(int, int) { custom_reset(false, false); }
void INTREQ(uae_u16) {}
bool INTREQ_0(uae_u16) { return false; }
void INTREQ_f(uae_u16) {}
void INTREQ_INT(int, int) {}
uae_u16 INTREQR(void) { return intreq; }
void rethink_uae_int(void) {}
void custom_prepare(void) {}
void send_interrupt(int, int) {}
bool intlev_is_not_on(int) { return true; }
extern "C" void intlev_ack(int) {}
void m68k_reset_delay_check(void) {}
void notice_screen_contents_lost(int) {}
void notice_resolution_seen(int, bool) {}
void check_prefs_changed_custom(void) {}
void set_config_changed(int) {}
uaecptr need_uae_boot_rom(struct uae_prefs *) { return 0; }

void devices_reset(int) {}
void devices_reset_ext(int) {}
void devices_vsync_pre(void) {}
void devices_vsync_post(void) {}
void devices_hsync(void) {}
void devices_rethink(void) {}
void devices_rethink_all(void (*)(void)) {}
void check_uae_int_request(void) {}
void device_call_main_thread_callbacks(void) {}

void gui_led(int, int, int) {}
void gui_flicker_led(int, int, int) {}
void gui_update(void) {}
void gui_handle_events(void) {}
void gui_filename(int, const TCHAR *) {}
void gui_fps(int, int, bool, int, int) {}
void gui_changesettings(void) {}
void gui_lock(void) {}
void gui_unlock(void) {}
void gui_disk_image_change(int, const TCHAR *, bool) {}
void notify_user(int) {}
void notify_user_parms(int, const TCHAR *, ...) {}
int gui_init(void) { return 1; }
void gui_exit(void) {}
void gui_display(int) {}
void gui_message(const TCHAR *, ...) {}
unsigned int gui_ledstate;
bool no_gui, quit_to_gui;

void debug(void) {}
void debug_exception(int) {}
void debug_init(void) {}
bool debugmem_illg(uae_u32) { return false; }
void deactivate_debugger(void) {}
void mmu_disasm(uaecptr, int) {}

/*
 * Savestate byte-stream helpers used by newcpu.cpp restore_cpu/save_cpu.
 * Cockatrice never snapshots Amiga chipset state; these just walk a buffer.
 */
void save_u8_func(uae_u8 **dstp, uae_u8 v)
{
	if (dstp && *dstp)
		*(*dstp)++ = v;
}
void save_u16_func(uae_u8 **dstp, uae_u16 v)
{
	save_u8_func(dstp, (uae_u8)(v >> 8));
	save_u8_func(dstp, (uae_u8)v);
}
void save_u32_func(uae_u8 **dstp, uae_u32 v)
{
	save_u16_func(dstp, (uae_u16)(v >> 16));
	save_u16_func(dstp, (uae_u16)v);
}
void save_u32t_func(uae_u8 **dstp, size_t v)
{
	save_u32_func(dstp, (uae_u32)v);
}
void save_u64_func(uae_u8 **dstp, uae_u64 v)
{
	save_u32_func(dstp, (uae_u32)(v >> 32));
	save_u32_func(dstp, (uae_u32)v);
}
uae_u8 restore_u8_func(uae_u8 **srcp)
{
	return (srcp && *srcp) ? *(*srcp)++ : 0;
}
uae_u16 restore_u16_func(uae_u8 **srcp)
{
	uae_u16 v = (uae_u16)restore_u8_func(srcp) << 8;
	return v | restore_u8_func(srcp);
}
uae_u32 restore_u32_func(uae_u8 **srcp)
{
	uae_u32 v = (uae_u32)restore_u16_func(srcp) << 16;
	return v | restore_u16_func(srcp);
}
uae_u64 restore_u64_func(uae_u8 **srcp)
{
	uae_u64 v = (uae_u64)restore_u32_func(srcp) << 32;
	return v | restore_u32_func(srcp);
}
bool restore_data_valid_func(uae_u8 **) { return true; }
void restore_ram(size_t, uae_u8 *) {}
TCHAR savestate_fname[MAX_DPATH];
bool savestate_check(void) { return false; }
void savestate_init(void) {}
uae_u32 get_statefile_version(void) { return 0; }

void statusline_clear(void) {}
void statusline_updated(int) {}
void warpmode(int) {}
void send_internalevent(int) {}
void custom_end_drawing(void) {}
void protect_roms(bool) {}
void check_prefs_changed_audio(void) {}
bool blitter_cycle_exact;
bool inprec_prepare_record(const TCHAR *) { return false; }

void rtarea_reset(void) {}
void audio_deactivate(void) {}
void maybe_blit(int) {}
int blitnasty(void) { return 0; }
void blitter_done_notify(void) {}
int inprec_open(const TCHAR *, const TCHAR *) { return 0; }
void inprec_startup(void) {}
void inprec_close(bool) {}
void inprec_recorddebug_cpu(int, uae_u16) {}
void inprec_playdebug_cpu(int, uae_u16) {}
void inprec_recorddebug(uae_u32) {}
void inprec_playdebug(uae_u32) {}

void uae_reset(int, int) {}
void uae_quit(void) { quit_program = UAE_QUIT; }
void target_shutdown(void) {}
void uae_restart(struct uae_prefs *, int, const TCHAR *) {}
void target_reset(void) {}
void target_run(void) {}
int sleep_millis(int) { return 0; }
int sleep_millis_main(int) { return 0; }
int sleep_millis_amiga(int) { return 0; }
void sleep_cpu_wakeup(void) {}
int sleep_resolution;

void memory_reset(void) {}
void memory_restore(void) {}
void a1000_reset(void) {}
void memory_cleanup(void) {}
void restore_banks(void) {}
void map_banks(addrbank *, int, int, int) {}
void map_banks_quick(addrbank *, int, int, int) {}
void map_banks_nojitdirect(addrbank *, int, int, int) {}
void map_banks_cond(addrbank *, int, int, int) {}
void map_overlay(int) {}
void memory_hardreset(int) {}
void memory_clear(void) {}
void free_fastmemory(int) {}
void mman_set_barriers(bool) {}
bool init_shm(void) { return true; }
void free_shm(void) {}
bool preinit_shm(void) { return true; }

void cpuboard_reset(int) {}
void cpuboard_rethink(void) {}
void cpuboard_map(void) {}
bool is_ppc_cpu(struct uae_prefs *) { return false; }
bool ppc_interrupt(int) { return false; }
bool cpuboard_forced_hardreset(void) { return false; }
bool cpuboard_fc_check(uaecptr, uae_u32 *, int, bool) { return false; }
uaecptr cpuboard_get_reset_pc(uaecptr *stack)
{
	if (stack)
		*stack = 0;
	return 0;
}

/* Defined in BasiliskII/memory.cpp: base of the flat 4GB Host_Mem_Base
 * mmap that backs every Mac guest address (Mac2HostAddr(addr) == this + addr). */
extern unsigned char *Host_Mem_Base;

addrbank dummy_bank;
addrbank kickmem_bank;
addrbank rtarea_bank;
static addrbank mac_bank;         /* SCC MMIO window only: must stay indirect. */
static addrbank mac_direct_bank;  /* Everything else: flat Host_Mem_Base, JIT can go direct. */
addrbank *mem_banks[MEMORY_BANKS];
uae_u8 *baseaddr[MEMORY_BANKS];
uae_u8 ce_banktype[65536];
uae_u8 ce_cachable[65536];
int config_changed;
int config_changed_flags;

/*
 * Forwards Amiberry banked fetches to Basilisk Macintosh memory. JIT
 * get_long_jit() reads jit_read_flag from mem_banks[]; those slots must
 * point at an Amiberry addrbank, not Musashi's smaller struct.
 */
static uae_u32 REGPARAM2 mac_lget(uaecptr addr)
{
	return memory_get_long(addr);
}
static uae_u32 REGPARAM2 mac_wget(uaecptr addr)
{
	return memory_get_word(addr);
}
static uae_u32 REGPARAM2 mac_bget(uaecptr addr)
{
	return memory_get_byte(addr);
}
static void REGPARAM2 mac_lput(uaecptr addr, uae_u32 v)
{
	memory_put_long(addr, v);
}
static void REGPARAM2 mac_wput(uaecptr addr, uae_u32 v)
{
	memory_put_word(addr, v);
}
static void REGPARAM2 mac_bput(uaecptr addr, uae_u32 v)
{
	memory_put_byte(addr, v);
}
static uae_u8 *REGPARAM2 mac_xlate(uaecptr addr)
{
	return memory_get_real_address(addr);
}
static int REGPARAM2 mac_check(uaecptr addr, uae_u32 size)
{
	return memory_valid_address(addr, size);
}

/*
 * Fills mac_direct_bank/mac_bank with Macintosh accessors. Every 64K slot
 * defaults to mac_direct_bank (flat Host_Mem_Base, JIT compiles direct
 * loads/stores); only the SCC MMIO windows are pointed at mac_bank, whose
 * jit_read_flag/jit_write_flag force the JIT back to indirect (checked)
 * access for any block that touches them. Ranges mirror is_scc_addr() in
 * BasiliskII/include/cpu_emulation.h: both 24-bit mirrors are marked
 * regardless of current addressing mode, since a 32-bit-clean ROM can still
 * briefly run 24-bit during early boot.
 *   0x00900000-0x009FFFFF, 0x00B00000-0x00BFFFFF : 24-bit SCC mirrors
 *   0x50000000-0x50FFFFFF                        : 32-bit SCC window
 * dummy_bank stays a true unmapped sentinel: UAE Exception() treats
 * SSP in dummy_bank as CPU_HALT_SSP_IN_NON_EXISTING_ADDRESS, so the Mac
 * RAM bank must be a different addrbank object.
 *
 * mem_banks[] alone is NOT enough for the JIT's direct path: computed-jump
 * targets (JMP/JSR (An), see jnf_MEM_GETADR_JMP_OFF in
 * compemu_midfunc_arm64_2.cpp) translate guest->host addresses via the
 * *separate* per-bank baseaddr[] array, not via R_MEMSTART/natmem_offset.
 * Skipping put_mem_bank() left baseaddr[] all-zero, so any computed jump
 * stored the raw (untranslated) guest address into regs.pc_p, which then
 * segfaulted on the next fetch. put_mem_bank() populates both arrays
 * consistently; mac_direct_bank.baseaddr = Host_Mem_Base with realstart=0
 * makes baseaddr[i] + addr == Host_Mem_Base + addr for every slot, matching
 * mac_xlate()/Mac2HostAddr() exactly.
 */
static void amiberry_init_mac_banks(void)
{
	memset(&dummy_bank, 0, sizeof(dummy_bank));
	memset(&mac_bank, 0, sizeof(mac_bank));
	memset(&mac_direct_bank, 0, sizeof(mac_direct_bank));

	for (addrbank *b : { &mac_bank, &mac_direct_bank }) {
		b->lget = mac_lget;
		b->wget = mac_wget;
		b->bget = mac_bget;
		b->lput = mac_lput;
		b->wput = mac_wput;
		b->bput = mac_bput;
		b->xlateaddr = mac_xlate;
		b->check = mac_check;
		b->lgeti = mac_lget;
		b->wgeti = mac_wget;
	}

	mac_bank.jit_read_flag = S_READ;
	mac_bank.jit_write_flag = S_WRITE;
	mac_bank.flags = ABFLAG_IO | ABFLAG_INDIRECT;
	mac_bank.label = _T("scc");
	mac_bank.name = _T("Macintosh SCC");

	mac_direct_bank.jit_read_flag = 0;
	mac_direct_bank.jit_write_flag = 0;
	mac_direct_bank.flags = ABFLAG_RAM;
	mac_direct_bank.label = _T("mac");
	mac_direct_bank.name = _T("Macintosh");
	mac_direct_bank.baseaddr = Host_Mem_Base;

	kickmem_bank = mac_direct_bank;
	rtarea_bank = mac_direct_bank;

	for (int i = 0; i < MEMORY_BANKS; i++)
		put_mem_bank((uaecptr)i << 16, &mac_direct_bank, 0);
	for (int i = 0x90; i < 0xA0; i++)
		put_mem_bank((uaecptr)i << 16, &mac_bank, 0);
	for (int i = 0xB0; i < 0xC0; i++)
		put_mem_bank((uaecptr)i << 16, &mac_bank, 0);
	for (int i = 0x5000; i < 0x5100; i++)
		put_mem_bank((uaecptr)i << 16, &mac_bank, 0);
}

addrbank *get_mem_bank_real(uaecptr)
{
	return &mac_bank;
}

void expansion_cpu_fallback(void) {}
void a3000_fakekick(int) {}
void custom_cpuchange(void) {}
void init_custom(void) {}
void fixup_cpu(struct uae_prefs *) {}
void target_cpu_speed(void) {}
bool is_mainthread(void) { return true; }

/*
 * Cycle-exact 68000/020 wait-state accessors. Macintosh hosting has no
 * Agnus DMA contention, so these just perform the Mac memory operation.
 */
uae_u32 wait_cpu_cycle_read(uaecptr addr, int mode)
{
	switch (mode) {
	case -1:
		return memory_get_long(addr);
	case -2:
		return memory_get_longi(addr);
	case 1:
		return memory_get_word(addr);
	case 2:
		return memory_get_wordi(addr);
	default:
		return memory_get_byte(addr);
	}
}

void wait_cpu_cycle_write(uaecptr addr, int mode, uae_u32 v)
{
	if (mode < 0)
		memory_put_long(addr, v);
	else if (mode > 0)
		memory_put_word(addr, v);
	else
		memory_put_byte(addr, v);
}

uae_u32 wait_cpu_cycle_read_ce020(uaecptr addr, int mode)
{
	return wait_cpu_cycle_read(addr, mode);
}

void wait_cpu_cycle_write_ce020(uaecptr addr, int mode, uae_u32 v)
{
	wait_cpu_cycle_write(addr, mode, v);
}

void console_out_f(const TCHAR *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

char *ua(const TCHAR *s)
{
	return s ? strdup(s) : nullptr;
}

static uae_u32 s_uaerand_seed = 1;

uae_u32 uaerand(void)
{
	s_uaerand_seed = s_uaerand_seed * 1103515245u + 12345u;
	return s_uaerand_seed;
}

uae_u32 uaesetrandseed(uae_u32 seed)
{
	uae_u32 old = s_uaerand_seed;
	s_uaerand_seed = seed ? seed : 1;
	return old;
}

uae_u32 uaerandgetseed(void)
{
	return s_uaerand_seed;
}

void uaerandomizeseed(void)
{
	s_uaerand_seed = 1;
}

void jit_abort(const TCHAR *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);
	printf("\n");
	abort();
}

/*
 * Maps Basilisk CPUType (0-4) onto Amiberry cpu_model / fpu_model and optional JIT.
 */
int amiberry_cpu_init(int cpu_type, int fpu_type, int jit, uint32_t cache_kb, int jitfpu)
{
	int models[] = { 68000, 68010, 68020, 68030, 68040 };

	if (cpu_type < 0 || cpu_type > 4)
		cpu_type = 4;

	currprefs.cpu_model = models[cpu_type];
	currprefs.mmu_model = 0;
	if (cpu_type >= 4)
		currprefs.fpu_model = 68040;
	else if (fpu_type > 0)
		currprefs.fpu_model = (fpu_type >= 2) ? 68882 : 68881;
	else
		currprefs.fpu_model = 0;

	/* Direct/trusted JIT memory access. Mac memory is a flat 4GB
	 * Host_Mem_Base mmap (see memory.cpp) and both address-translation bugs
	 * that direct mode originally exposed are fixed: comp_hardflush above,
	 * and jnf_MEM_GETADR_JMP_OFF (compemu_midfunc_arm64_2.cpp) now routes
	 * computed-jump targets through R_MEMSTART/natmem_offset instead of the
	 * separate baseaddr[] table, matching ARAnyM's WINUAE_ARANYM
	 * get_n_addr_jmp(). Verified: no host crashes, no regs.pc_p desyncs,
	 * across repeated boots. 0 = direct (trust the flat mmap and skip the
	 * cockatrice_mac_valid_addr() call per compiled access); canbang below
	 * must also be true or check_prefs_changed_comp() (compemu_prefs.cpp)
	 * forces these back to 1 (indirect) on the next prefs-apply.
	 *
	 * Only comptrustnaddr (address computation: get_n_addr/get_n_addr_jmp)
	 * is direct here. That is the part the "verified" note above actually
	 * covers. Flipping comptrustbyte/word/long to 0 as well exercises the
	 * direct byte/word/long data load/store codegen (jnf_MEM_READ_OFF_x and
	 * jnf_MEM_WRITE_OFF_x in compemu_midfunc_arm64_2.cpp) for the first
	 * time ever in this port - doing so produced an immediate guest illegal
	 * instruction on every boot, so it stays indirect until that path is
	 * debugged on its own. */
	currprefs.comptrustbyte = 1;
	currprefs.comptrustword = 1;
	currprefs.comptrustlong = 1;
	currprefs.comptrustnaddr = 0;
	currprefs.compnf = true;
	/* Lazy flush: 68040 guests issue CINVA/CPUSHx routinely for DMA cache
	 * coherency (every disk/network transfer), not because code changed.
	 * Hard-flushing the whole JIT cache on each one forces constant
	 * recompilation; lazy flush just checksums affected blocks before
	 * reuse, which is what real UAE JIT builds use for this reason. */
	currprefs.comp_hardflush = false;
	currprefs.cpu_compatible = false;
	currprefs.address_space_24 = false;
	currprefs.fpu_no_unimplemented = false;
	canbang = true;
	jit_direct_compatible_memory = false;
	/* memory_init() (amiberry_glue.cpp, before this call) has already
	 * allocated Host_Mem_Base; the JIT direct-access bugs above are fixed,
	 * so canbang is on and comptrust* above is 0 (direct). */
	natmem_offset = Host_Mem_Base;

	/* Mac memory reaches the JIT through indirect addrbanks, so multi-access
	 * opcodes (MOVEM in both directions, MOVE16) must fall back to the
	 * per-access helpers instead of being compiled as native burst accesses:
	 * a runtime data address can land in an indirect bank even when the
	 * instruction history carries no special-memory marker.
	 * jit_opcode_needs_compile_fallback() (compemu_support_arm.cpp) already
	 * implements exactly that, but it is gated on jit_n_addr_bank_unsafe,
	 * which nothing ever assigned - so the guard was dead code and MOVEM
	 * compiled to a burst that corrupted guest memory (a MOVEM.L (A6)+
	 * register restore then RTS'd through a clobbered slot). */
	jit_n_addr_bank_unsafe = 1;

	if (jit && cache_kb > 0) {
		currprefs.cachesize = (int)cache_kb;
		currprefs.compfpu = (jitfpu != 0 && currprefs.fpu_model != 0);
	} else {
		currprefs.cachesize = 0;
		currprefs.compfpu = false;
	}
	changed_prefs = currprefs;
	quit_program = 0;

	amiberry_init_mac_banks();
	init_m68k();
	if (currprefs.fpu_model)
		fp_init_softfloat(currprefs.fpu_model);

#ifdef JIT
	/* Allocate the translation cache before build_cpufunctbl() runs build_comp(). */
	compiler_init();
#endif
	/* Wire cpufunctbl[] and x_get_iword; Amiberry does this in m68k_go() only. */
	m68k_prepare();
	m68k_reset();
	write_log("[UAE] Amiberry %d init (fpu=%d mmu_model=%d jit=%s compfpu=%s cache=%d KB)\n",
		currprefs.cpu_model, currprefs.fpu_model, currprefs.mmu_model,
		currprefs.cachesize ? "yes" : "no",
		currprefs.compfpu ? "yes" : "no", currprefs.cachesize);
	return 1;
}

/*
 * Discards stale JIT blocks after Basilisk writes guest code (CheckLoad,
 * BlockMove, ROM patches). Always hard-flushes the whole cache; addr/size are
 * for logging only on the ARM backend.
 *
 * Must raise SPCFLAG_MODE_CHANGE after flush: flush_icache_hard() ends with
 * set_special(0), which clears flags without leaving m68k_run_jit, so stale
 * native code could otherwise keep executing fill-pattern heap (0x65A00).
 */
void amiberry_cpu_invalidate_code(uint32_t addr, uint32_t size)
{
#ifdef JIT
	if (size == 0 || currprefs.cachesize == 0)
		return;
	/* macemu Basilisk: range flush + exit compiled code; full flush if ~0. */
	if (size >= 0xffffffu || addr == 0)
		flush_icache(3);
	else
		flush_icache_range((uaecptr)addr, size);
	fill_prefetch();
	static unsigned inv_log_count;
	if (inv_log_count < 20 && (addr >= 0x65000 && addr < 0x68000)) {
		inv_log_count++;
		printf("[UAE-JIT] invalidate 0x%08X len 0x%X PC=0x%08X\n",
			addr, size, m68k_getpc());
		fflush(stdout);
	}
#else
	(void)addr;
	(void)size;
#endif
}

extern "C" void cockatrice_uae_fline_trap(uint32_t opcode, uint32_t pc, int from_mmu_op)
{
	static unsigned count;
	static int rom_header_halted;

	if (count >= 100)
		return;
	count++;
	printf("[F-LINE] opcode=0x%04X PC=0x%08X SP=0x%08X mmu_model=%d cpu=%d jit=%s compfpu=%s from_mmu_op=%d\n",
		(unsigned)(opcode & 0xffff), pc, m68k_areg(regs, 7),
		currprefs.mmu_model, currprefs.cpu_model,
		currprefs.cachesize ? "yes" : "no",
		currprefs.compfpu ? "yes" : "no",
		from_mmu_op);
	printf("[F-LINE]   guest[%08X..] = %04X %04X %04X %04X\n",
		pc,
		(unsigned)(cockatrice_mac_get_word(pc) & 0xffff),
		(unsigned)(cockatrice_mac_get_word(pc + 2) & 0xffff),
		(unsigned)(cockatrice_mac_get_word(pc + 4) & 0xffff),
		(unsigned)(cockatrice_mac_get_word(pc + 6) & 0xffff));
	if ((opcode & 0xFF00) == 0xF000 && !currprefs.mmu_model)
		printf("[F-LINE]   (68040 MMU coprocessor 0xF0xx — NOP without MMU)\n");
	else if ((opcode & 0xF000) == 0xF200 && !currprefs.compfpu && currprefs.cachesize)
		printf("[F-LINE] hint: FPU F-line under JIT without compfpu — try pref jitfpu true\n");
	if (pc >= 0x65000 && pc < 0x67000) {
		printf("[F-LINE]   low heap fill — return chain:\n");
		for (int i = 0; i < 8; i++)
			printf("    D%d=0x%08X", i, (unsigned)m68k_dreg(regs, i));
		printf("\n");
		for (int i = 0; i < 8; i++)
			printf("    A%d=0x%08X", i, (unsigned)m68k_areg(regs, i));
		printf("\n");
	}
	extern uint32_t ROMBaseMac;
	extern uint32_t cpu_engine_last_pc;
	if (pc >= ROMBaseMac && pc < ROMBaseMac + 0x100) {
		uint32_t sp = m68k_areg(regs, 7);
		printf("[F-LINE]   WARNING: PC in Mac ROM header (corrupt CPU state?)\n");
		printf("[F-LINE]   last_note_pc=0x%08X VBR=0x%08X vector_B=0x%08X\n",
			cpu_engine_last_pc, (unsigned)regs.vbr,
			(unsigned)cockatrice_mac_get_long(regs.vbr + 0x2c));
		printf("[F-LINE]   stack @A7:");
		for (int i = 0; i < 8; i++)
			printf(" %08X", (unsigned)cockatrice_mac_get_long(sp + i * 4));
		printf("\n");
		if (!rom_header_halted) {
			rom_header_halted = 1;
			regs.halted = CPU_HALT_DOUBLE_FAULT;
			set_special(SPCFLAG_MODE_CHANGE);
		}
		return;
	}
	fflush(stdout);
}

void amiberry_cpu_exit(void)
{
#ifdef JIT
	compiler_exit();
#endif
	quit_program = UAE_QUIT;
	set_special(SPCFLAG_MODE_CHANGE);
}

void amiberry_cpu_reset(void)
{
	m68k_reset();
}

/*
 * Raises a 680x0 access fault (vector 2) for a Macintosh hole address.
 *
 * Called after SIGSEGV/ACCESS_VIOLATION longjmps out of m68k_run(). Not
 * invoked from the signal handler itself, so Exception() runs in C++.
 *
 * Arguments:
 *   addr: 32-bit Macintosh address that was not committed in the 4GB window.
 */
void amiberry_cpu_bus_error(uint32_t addr)
{
	printf("[MEM] uae guest hole at 0x%08X (PC=0x%08X)\n", addr, m68k_getpc());
	fflush(stdout);
	Exception(2);
}

/*
 * Marks the start of a nested m68k_execute slice (Execute68k / Execute68kTrap).
 * macemu increments m68k_execute_depth around m68k_execute() so EmulOp return
 * and JIT cache flush semantics stay scoped to the nested call.
 */
void amiberry_cpu_nested_execute_begin(void)
{
	++s_m68k_execute_depth;
	s_nested_quit_program = 0;
}

/*
 * Ends a nested m68k_execute slice started by amiberry_cpu_nested_execute_begin().
 */
void amiberry_cpu_nested_execute_end(void)
{
	if (s_m68k_execute_depth > 0)
		--s_m68k_execute_depth;
	s_nested_quit_program = 0;
}

extern "C" int amiberry_cpu_nested_execute_depth(void)
{
	return s_m68k_execute_depth;
}

void amiberry_cpu_nested_request_quit(void)
{
	s_nested_quit_program = 1;
	/* BRK ends the nested interpreter slice only (see do_specialties). Do not
	 * set MODE_CHANGE here — it would escape the outer m68k_run_jit after the
	 * enclosing EmulOp returns (EtherIRQ / TimerInterrupt Execute68k). */
	amiberry_cpu_request_brk();
}

int amiberry_cpu_nested_quit_requested(void)
{
	return s_nested_quit_program;
}

void amiberry_cpu_execute_slice(void)
{
	unset_special(SPCFLAG_MODE_CHANGE);
	m68k_run();
}

void amiberry_cpu_execute_interpreter_slice(void)
{
	m68k_run_interpreter_slice();
}

void amiberry_cpu_clear_mode_change(void)
{
	unset_special(SPCFLAG_MODE_CHANGE);
}

void amiberry_cpu_set_mode_change(void)
{
	set_special(SPCFLAG_MODE_CHANGE);
}

void amiberry_cpu_fill_prefetch(void)
{
	fill_prefetch();
}

void amiberry_cpu_request_brk(void)
{
	set_special(SPCFLAG_BRK);
}

void amiberry_cpu_request_irq(void)
{
	set_special(SPCFLAG_INT);
}

uint32_t amiberry_cpu_get_pc(void)
{
	return m68k_getpc();
}

void amiberry_cpu_set_pc(uint32_t pc)
{
	m68k_setpc(pc);
}

void amiberry_cpu_inc_pc(int bytes)
{
	m68k_incpc_normal(bytes);
}

uint32_t amiberry_cpu_get_reg(int n)
{
	if (n < 0 || n > 15)
		return 0;
	return regs.regs[n];
}

void amiberry_cpu_set_reg(int n, uint32_t v)
{
	if (n < 0 || n > 15)
		return;
	regs.regs[n] = v;
}

uint16_t amiberry_cpu_get_sr(void)
{
	MakeSR();
	return (uint16_t)regs.sr;
}

void amiberry_cpu_set_sr(uint16_t sr)
{
	regs.sr = sr;
	MakeFromSR();
}
