//! C ABI shim between CockatriceIII and the m68k-rs interpreter core.

use m68k::{
    AddressBus, BatchExit, CpuCore, CpuType, CycleBatchControl, CycleBatchExit, CycleBoundaryEvent,
    FastMem,
};
use m68k::core::memory::{BusFault, BusFaultKind};
use std::os::raw::{c_int, c_void};
use std::ptr;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct M68kRsHostCallbacks {
    pub read_byte: Option<unsafe extern "C" fn(*mut c_void, u32) -> u8>,
    pub read_word: Option<unsafe extern "C" fn(*mut c_void, u32) -> u16>,
    pub read_long: Option<unsafe extern "C" fn(*mut c_void, u32) -> u32>,
    pub write_byte: Option<unsafe extern "C" fn(*mut c_void, u32, u8)>,
    pub write_word: Option<unsafe extern "C" fn(*mut c_void, u32, u16)>,
    pub write_long: Option<unsafe extern "C" fn(*mut c_void, u32, u32)>,
    pub handle_illegal:
        Option<unsafe extern "C" fn(*mut c_void, u16, *mut M68kRsRegs) -> c_int>,
    pub handle_aline: Option<unsafe extern "C" fn(*mut c_void, u16, *mut M68kRsRegs) -> c_int>,
    pub boundary_hook: Option<unsafe extern "C" fn(*mut c_void, u32)>,
    pub get_irq: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    /// Reports a contiguous, side-effect-free guest RAM window to the batch
    /// executor. Writes `*base`/`*len` and returns the host pointer backing
    /// `*base`, or null when no direct window is available.
    pub fast_mem: Option<unsafe extern "C" fn(*mut c_void, *mut u32, *mut u32) -> *mut u8>,
    pub host_ctx: *mut c_void,
}

#[repr(C)]
pub struct M68kRsRegs {
    pub d: [u32; 8],
    pub a: [u32; 8],
    pub sr: u16,
}

#[repr(C)]
pub enum M68kRsReg {
    D0 = 0,
    D1,
    D2,
    D3,
    D4,
    D5,
    D6,
    D7,
    A0,
    A1,
    A2,
    A3,
    A4,
    A5,
    A6,
    A7,
    Pc,
    Sr,
    Ppc,
}

#[repr(C)]
pub enum M68kRsCpuType {
    M68000 = 0,
    M68010,
    M68020,
    M68030,
    M68040,
}

#[repr(C)]
pub enum M68kRsRunExit {
    Budget = 0,
    Stopped,
    Boundary,
    TrapUnhandled,
    Halted,
}

#[repr(C)]
pub struct M68kRsRunResult {
    pub exit: M68kRsRunExit,
    pub cycles: u32,
    pub instructions: u32,
    pub trap_opcode: u16,
}

pub struct M68kRsCpu {
    core: CpuCore,
    callbacks: M68kRsHostCallbacks,
    bus: BasiliskBus,
    stop_requested: bool,
}

/// Upper bound on committed ranges accepted from the host; must match
/// `M68K_RS_MAX_MAPPED_RANGES` in cockatrice_m68k_rs.h and memory.cpp's
/// `MEMORY_MAX_RANGES`.
const MAX_MAPPED_RANGES: usize = 16;

struct BasiliskBus {
    callbacks: *const M68kRsHostCallbacks,
    /// Local cache of the host's committed-range table (RAM/ROM/framebuffer),
    /// pushed once by `m68k_rs_set_mapped_ranges`. Checking membership here
    /// instead of calling back into the host on every access removes an FFI
    /// round trip from the hottest path in the interpreter: every checked
    /// (non-FastMem) memory access used to cross into C++ twice, once for
    /// `is_mapped` and once for the actual read/write.
    mapped_ranges: [(u32, u32); MAX_MAPPED_RANGES],
    mapped_range_count: usize,
    /// TwentyFourBitAddressing fallback: the Z8530 SCC mirrors across every
    /// 16MB slice of the address space (masked on the low 24 bits), which a
    /// fixed range table entry cannot express. Checked only when the range
    /// table misses, so the common 32-bit-addressing case (where the SCC's
    /// single window is just another table entry) never touches this.
    scc_24bit_mirror: bool,
}

impl BasiliskBus {
    fn callbacks(&self) -> &M68kRsHostCallbacks {
        unsafe { &*self.callbacks }
    }

    /// Mirrors memory.cpp's `memory_is_mapped()`: every byte in
    /// `[address, address+size)` must fall in some committed range, but the
    /// first and last byte are allowed to be covered by different ranges
    /// (matching the host's existing semantics exactly).
    #[inline]
    fn is_mapped_local(&self, address: u32, size: u32) -> bool {
        if size == 0 {
            return true;
        }
        let Some(last) = address.checked_add(size - 1) else {
            return false;
        };
        let mut start_ok = false;
        let mut last_ok = false;
        for &(start, end) in &self.mapped_ranges[..self.mapped_range_count] {
            if address >= start && address < end {
                start_ok = true;
            }
            if last >= start && last < end {
                last_ok = true;
            }
            if start_ok && last_ok {
                return true;
            }
        }
        // TwentyFourBitAddressing only: matches is_scc_addr()'s masked
        // mirror check, which the fixed range table above cannot express.
        self.scc_24bit_mirror && {
            let a24 = address & 0x00ff_ffff;
            (0x0090_0000..0x00a0_0000).contains(&a24) || (0x00b0_0000..0x00c0_0000).contains(&a24)
        }
    }
}

impl AddressBus for BasiliskBus {
    fn try_read_byte(&mut self, address: u32) -> Result<u8, BusFault> {
        if !self.is_mapped_local(address, 1) {
            return Err(BusFault {
                kind: BusFaultKind::BusError,
                address,
            });
        }
        Ok(self.read_byte(address))
    }

    fn try_read_word(&mut self, address: u32) -> Result<u16, BusFault> {
        if !self.is_mapped_local(address, 2) {
            return Err(BusFault {
                kind: BusFaultKind::BusError,
                address,
            });
        }
        Ok(self.read_word(address))
    }

    fn try_read_long(&mut self, address: u32) -> Result<u32, BusFault> {
        if !self.is_mapped_local(address, 4) {
            return Err(BusFault {
                kind: BusFaultKind::BusError,
                address,
            });
        }
        Ok(self.read_long(address))
    }

    fn try_write_byte(&mut self, address: u32, value: u8) -> Result<(), BusFault> {
        if !self.is_mapped_local(address, 1) {
            return Err(BusFault {
                kind: BusFaultKind::BusError,
                address,
            });
        }
        self.write_byte(address, value);
        Ok(())
    }

    fn try_write_word(&mut self, address: u32, value: u16) -> Result<(), BusFault> {
        if !self.is_mapped_local(address, 2) {
            return Err(BusFault {
                kind: BusFaultKind::BusError,
                address,
            });
        }
        self.write_word(address, value);
        Ok(())
    }

    fn try_write_long(&mut self, address: u32, value: u32) -> Result<(), BusFault> {
        if !self.is_mapped_local(address, 4) {
            return Err(BusFault {
                kind: BusFaultKind::BusError,
                address,
            });
        }
        self.write_long(address, value);
        Ok(())
    }

    fn try_read_immediate_word(&mut self, address: u32) -> Result<u16, BusFault> {
        self.try_read_word(address)
    }

    fn try_read_immediate_long(&mut self, address: u32) -> Result<u32, BusFault> {
        self.try_read_long(address)
    }

    fn read_byte(&mut self, address: u32) -> u8 {
        let cb = self.callbacks();
        if let Some(f) = cb.read_byte {
            unsafe { f(cb.host_ctx, address) }
        } else {
            0
        }
    }

    fn read_word(&mut self, address: u32) -> u16 {
        let cb = self.callbacks();
        if let Some(f) = cb.read_word {
            unsafe { f(cb.host_ctx, address) }
        } else {
            0
        }
    }

    fn read_long(&mut self, address: u32) -> u32 {
        let cb = self.callbacks();
        if let Some(f) = cb.read_long {
            unsafe { f(cb.host_ctx, address) }
        } else {
            0
        }
    }

    fn write_byte(&mut self, address: u32, value: u8) {
        let cb = self.callbacks();
        if let Some(f) = cb.write_byte {
            unsafe { f(cb.host_ctx, address, value) };
        }
    }

    fn write_word(&mut self, address: u32, value: u16) {
        let cb = self.callbacks();
        if let Some(f) = cb.write_word {
            unsafe { f(cb.host_ctx, address, value) };
        }
    }

    fn write_long(&mut self, address: u32, value: u32) {
        let cb = self.callbacks();
        if let Some(f) = cb.write_long {
            unsafe { f(cb.host_ctx, address, value) };
        }
    }

    /// Direct window over the host's flat Macintosh address space.
    ///
    /// `run_batch` captures this once per call, so the host is free to shrink
    /// or withdraw the window (24-bit addressing, MMIO apertures) between
    /// batches. Cycle-accurate entry points ignore it entirely.
    fn fast_mem(&mut self) -> Option<FastMem> {
        let cb = self.callbacks();
        let f = cb.fast_mem?;
        let mut base: u32 = 0;
        let mut len: u32 = 0;
        let ptr = unsafe { f(cb.host_ctx, &mut base, &mut len) };
        if ptr.is_null() || len < 4 {
            return None;
        }
        Some(FastMem { ptr, base, len })
    }
}

fn map_cpu_type(cpu_type: M68kRsCpuType) -> CpuType {
    match cpu_type {
        M68kRsCpuType::M68000 => CpuType::M68000,
        M68kRsCpuType::M68010 => CpuType::M68010,
        M68kRsCpuType::M68020 => CpuType::M68020,
        M68kRsCpuType::M68030 => CpuType::M68030,
        M68kRsCpuType::M68040 => CpuType::M68040,
    }
}

fn export_regs(cpu: &CpuCore) -> M68kRsRegs {
    let mut regs = M68kRsRegs {
        d: [0; 8],
        a: [0; 8],
        sr: cpu.get_sr(),
    };
    for i in 0..8 {
        regs.d[i] = cpu.d(i);
        regs.a[i] = cpu.a(i);
    }
    regs
}

fn import_regs(cpu: &mut CpuCore, regs: &M68kRsRegs) {
    for i in 0..8 {
        cpu.set_d(i, regs.d[i]);
        cpu.set_a(i, regs.a[i]);
    }
    cpu.set_sr(regs.sr);
}

fn dispatch_trap(
    cpu: &mut M68kRsCpu,
    handle: Option<unsafe extern "C" fn(*mut c_void, u16, *mut M68kRsRegs) -> c_int>,
    opcode: u16,
) -> bool {
    let Some(handler) = handle else {
        return false;
    };
    let mut regs = export_regs(&cpu.core);
    let handled = unsafe { handler(cpu.callbacks.host_ctx, opcode, &mut regs) != 0 };
    if handled {
        import_regs(&mut cpu.core, &regs);
    }
    handled
}

fn run_result(
    exit: M68kRsRunExit,
    cycles: u32,
    instructions: u32,
    trap_opcode: u16,
) -> M68kRsRunResult {
    M68kRsRunResult {
        exit,
        cycles,
        instructions,
        trap_opcode,
    }
}

#[no_mangle]
pub unsafe extern "C" fn m68k_rs_create(callbacks: *const M68kRsHostCallbacks) -> *mut M68kRsCpu {
    if callbacks.is_null() {
        return ptr::null_mut();
    }
    let cb = *callbacks;
    let mut boxed = Box::new(M68kRsCpu {
        core: CpuCore::new(),
        callbacks: cb,
        bus: BasiliskBus {
            callbacks: ptr::null(),
            mapped_ranges: [(0, 0); MAX_MAPPED_RANGES],
            mapped_range_count: 0,
            scc_24bit_mirror: false,
        },
        stop_requested: false,
    });
    boxed.bus.callbacks = &boxed.callbacks;
    Box::into_raw(boxed)
}

/// Pushes the host's committed-range table into the bus's local cache (see
/// [`BasiliskBus::is_mapped_local`]). `count` beyond `MAX_MAPPED_RANGES` is
/// clamped; `starts`/`ends` must each have at least `count` valid entries.
#[no_mangle]
pub unsafe extern "C" fn m68k_rs_set_mapped_ranges(
    cpu: *mut M68kRsCpu,
    starts: *const u32,
    ends: *const u32,
    count: u32,
    scc_24bit_mirror: c_int,
) {
    if cpu.is_null() || starts.is_null() || ends.is_null() {
        return;
    }
    let cpu = &mut *cpu;
    let n = (count as usize).min(MAX_MAPPED_RANGES);
    for i in 0..n {
        cpu.bus.mapped_ranges[i] = (*starts.add(i), *ends.add(i));
    }
    cpu.bus.mapped_range_count = n;
    cpu.bus.scc_24bit_mirror = scc_24bit_mirror != 0;
}

#[no_mangle]
pub unsafe extern "C" fn m68k_rs_destroy(cpu: *mut M68kRsCpu) {
    if !cpu.is_null() {
        drop(Box::from_raw(cpu));
    }
}

#[no_mangle]
pub unsafe extern "C" fn m68k_rs_init(cpu: *mut M68kRsCpu, cpu_type: M68kRsCpuType) -> c_int {
    if cpu.is_null() {
        return 0;
    }
    let cpu = &mut *cpu;
    cpu.core.set_cpu_type(map_cpu_type(cpu_type));
    cpu.stop_requested = false;
    1
}

#[no_mangle]
pub unsafe extern "C" fn m68k_rs_pulse_reset(cpu: *mut M68kRsCpu) {
    if cpu.is_null() {
        return;
    }
    (*cpu).core.pulse_reset();
}

#[no_mangle]
pub unsafe extern "C" fn m68k_rs_invalidate_prefetch(cpu: *mut M68kRsCpu) {
    if cpu.is_null() {
        return;
    }
    (*cpu).core.invalidate_prefetch();
}

#[no_mangle]
pub unsafe extern "C" fn m68k_rs_get_reg(cpu: *const M68kRsCpu, reg: M68kRsReg) -> u32 {
    if cpu.is_null() {
        return 0;
    }
    let core = &(*cpu).core;
    let reg_id = reg as u32;
    match reg_id {
        0..=7 => core.d(reg_id as usize),
        8..=15 => core.a((reg_id - 8) as usize),
        16 => core.pc,
        17 => core.get_sr() as u32,
        18 => core.ppc,
        _ => 0,
    }
}

/// Returns the most recent 68k exception vector taken since the last call
/// (any vector -- interrupts and A-line/Toolbox trap dispatch included,
/// which fire constantly and are not by themselves faults), clearing it so
/// each vector is reported at most once. Returns -1 if none was taken.
///
/// The host is expected to filter to the small set of vectors that indicate
/// a genuine fault (2-8, 11) before logging -- see
/// `cockatrice_report_cpu_exception()` in cpu_engine.cpp, which every CPU
/// engine in this tree funnels through for that purpose.
#[no_mangle]
pub unsafe extern "C" fn m68k_rs_take_last_exception_vector(cpu: *mut M68kRsCpu) -> i32 {
    if cpu.is_null() {
        return -1;
    }
    match (*cpu).core.last_exception_vector.take() {
        Some(v) => v as i32,
        None => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn m68k_rs_set_reg(cpu: *mut M68kRsCpu, reg: M68kRsReg, value: u32) {
    if cpu.is_null() {
        return;
    }
    let core = &mut (*cpu).core;
    let reg_id = reg as u32;
    match reg_id {
        0..=7 => core.set_d(reg_id as usize, value),
        8..=15 => core.set_a((reg_id - 8) as usize, value),
        16 => core.pc = value,
        17 => core.set_sr(value as u16),
        18 => core.ppc = value,
        _ => {}
    }
}

#[no_mangle]
pub unsafe extern "C" fn m68k_rs_set_irq(cpu: *mut M68kRsCpu, level: c_int) {
    if cpu.is_null() {
        return;
    }
    (*cpu).core.set_irq(level.clamp(0, 7) as u8);
}

#[no_mangle]
pub unsafe extern "C" fn m68k_rs_request_stop(cpu: *mut M68kRsCpu) {
    if cpu.is_null() {
        return;
    }
    (*cpu).stop_requested = true;
}

/// Reports whether this build compiled the Cranelift trace JIT in.
///
/// The batch executor works either way — without the feature the hot traces
/// run through the portable executor instead of native code — so this only
/// exists so the host can say which one it got.
#[no_mangle]
pub extern "C" fn m68k_rs_jit_available() -> c_int {
    if cfg!(feature = "jit") { 1 } else { 0 }
}

/// Reports whether Cranelift's native trace compiler actually initialized
/// on the calling thread, as opposed to merely being compiled in.
///
/// `m68k_rs_jit_available()` only reflects the `jit` Cargo feature; it stays
/// 1 even if `cranelift-jit`'s `JITBuilder::new()` fails at runtime (e.g. no
/// executable-memory permission) and the crate silently falls back to its
/// portable trace executor. This call forces that check now, on whichever
/// thread calls it, and reports the real outcome.
#[no_mangle]
pub extern "C" fn m68k_rs_jit_native_active() -> c_int {
    if m68k::jit_native_active() { 1 } else { 0 }
}

/// Throughput entry point: run up to `max_instructions` guest instructions
/// through the decoded-op cache, the fastmem window, and the trace JIT.
///
/// Unlike [`m68k_rs_run_cycles`] this keeps no cycle accounting and calls no
/// per-instruction boundary hook, so the host must poll interrupts between
/// batches. Traps are handled in-loop exactly as the cycle path handles them,
/// so a Toolbox-heavy guest does not pay a host round trip per A-line.
#[no_mangle]
pub unsafe extern "C" fn m68k_rs_run_batch(
    cpu: *mut M68kRsCpu,
    max_instructions: u32,
) -> M68kRsRunResult {
    if cpu.is_null() || max_instructions == 0 {
        return run_result(M68kRsRunExit::Budget, 0, 0, 0);
    }

    let cpu = &mut *cpu;
    if cpu.core.is_halted() {
        return run_result(M68kRsRunExit::Halted, 0, 0, 0);
    }

    let mut total_instructions = 0u32;
    let mut remaining = max_instructions;

    // `run_batch` services interrupts from the level latched on the CPU, but
    // unlike the cycle path it never calls back into the host mid-batch. Chunk
    // the outer budget and refresh IRQ from Basilisk between chunks so SCSI and
    // the 60 Hz tick are not starved across a 16K-instruction slice.
    const IRQ_POLL_CHUNK: u32 = 256;

    while remaining > 0 && !cpu.stop_requested {
        if let Some(get_irq) = cpu.callbacks.get_irq {
            let level = unsafe { get_irq(cpu.callbacks.host_ctx) };
            cpu.core.set_irq(level.clamp(0, 7) as u8);
        }
        let chunk = remaining.min(IRQ_POLL_CHUNK);
        let result = cpu.core.run_batch(&mut cpu.bus, chunk, &[]);

        total_instructions += result.instructions;
        remaining = remaining.saturating_sub(result.instructions);

        match result.exit {
            BatchExit::BudgetExhausted => {
                return run_result(M68kRsRunExit::Budget, 0, total_instructions, 0);
            }
            BatchExit::Stopped => {
                return run_result(M68kRsRunExit::Stopped, 0, total_instructions, 0);
            }
            BatchExit::WatchedPc { .. } => {
                return run_result(M68kRsRunExit::Boundary, 0, total_instructions, 0);
            }
            BatchExit::IllegalInstruction { opcode } => {
                let handled = dispatch_trap(cpu, cpu.callbacks.handle_illegal, opcode);
                if handled {
                    // M68K_EXEC_RETURN (0x7100): end the slice immediately so a
                    // patched STOP #0x2700 cannot fall through NOP into TEST_FAIL.
                    if opcode == 0x7100 {
                        return run_result(
                            M68kRsRunExit::Budget,
                            0,
                            total_instructions,
                            opcode,
                        );
                    }
                } else {
                    cpu.core.take_illegal_exception(&mut cpu.bus);
                }
            }
            BatchExit::AlineTrap { opcode } => {
                let handled = dispatch_trap(cpu, cpu.callbacks.handle_aline, opcode);
                if !handled {
                    cpu.core.take_aline_exception(&mut cpu.bus);
                }
            }
            BatchExit::FlineTrap { opcode: _ } => {
                cpu.core.take_fline_exception(&mut cpu.bus);
            }
            BatchExit::TrapInstruction { trap_num } => {
                cpu.core.take_trap_exception(&mut cpu.bus, trap_num);
            }
            BatchExit::Breakpoint { bp_num } => {
                cpu.core.take_bkpt_exception(&mut cpu.bus);
                let _ = bp_num;
            }
        }

        if cpu.core.is_halted() {
            return run_result(M68kRsRunExit::Halted, 0, total_instructions, 0);
        }
    }

    run_result(M68kRsRunExit::Budget, 0, total_instructions, 0)
}

#[no_mangle]
pub unsafe extern "C" fn m68k_rs_run_cycles(
    cpu: *mut M68kRsCpu,
    cycle_budget: i32,
) -> M68kRsRunResult {
    if cpu.is_null() || cycle_budget <= 0 {
        return run_result(M68kRsRunExit::Budget, 0, 0, 0);
    }

    let cpu = &mut *cpu;
    if cpu.core.is_halted() {
        return run_result(M68kRsRunExit::Halted, 0, 0, 0);
    }

    let mut total_cycles = 0u32;
    let mut total_instructions = 0u32;
    let mut remaining = cycle_budget;

    while remaining > 0 && !cpu.stop_requested {
        let callbacks = &cpu.callbacks;
        let stop_requested = &mut cpu.stop_requested;
        let result = cpu.core.run_for_cycles_with_boundary_hook(
            &mut cpu.bus,
            remaining,
            |core, _bus, event| {
                let cycles = match event {
                    CycleBoundaryEvent::Instruction { cycles } => cycles,
                    CycleBoundaryEvent::InterruptEntry { cycles } => cycles,
                };
                if let Some(hook) = callbacks.boundary_hook {
                    unsafe { hook(callbacks.host_ctx, cycles.max(0) as u32) };
                }
                if let Some(get_irq) = callbacks.get_irq {
                    let level = unsafe { get_irq(callbacks.host_ctx) };
                    core.set_irq(level.clamp(0, 7) as u8);
                }
                if *stop_requested {
                    CycleBatchControl::Return
                } else {
                    CycleBatchControl::Continue
                }
            },
        );

        total_cycles += result.cycles.max(0) as u32;
        total_instructions += result.instructions;
        remaining -= result.cycles;

        match result.exit {
            CycleBatchExit::BudgetExhausted => {
                return run_result(
                    M68kRsRunExit::Budget,
                    total_cycles,
                    total_instructions,
                    0,
                );
            }
            CycleBatchExit::BoundaryRequested => {
                return run_result(
                    M68kRsRunExit::Boundary,
                    total_cycles,
                    total_instructions,
                    0,
                );
            }
            CycleBatchExit::Stopped => {
                return run_result(
                    M68kRsRunExit::Stopped,
                    total_cycles,
                    total_instructions,
                    0,
                );
            }
            CycleBatchExit::IllegalInstruction { opcode } => {
                let handled = dispatch_trap(cpu, cpu.callbacks.handle_illegal, opcode);
                if handled {
                    // M68K_EXEC_RETURN (0x7100): end the slice immediately so patched
                    // STOP #0x2700 cannot fall through NOP into TEST_FAIL.
                    if opcode == 0x7100 {
                        return run_result(
                            M68kRsRunExit::Budget,
                            total_cycles,
                            total_instructions,
                            opcode,
                        );
                    }
                } else {
                    cpu.core.take_illegal_exception(&mut cpu.bus);
                }
            }
            CycleBatchExit::AlineTrap { opcode } => {
                let handled = dispatch_trap(cpu, cpu.callbacks.handle_aline, opcode);
                if !handled {
                    cpu.core.take_aline_exception(&mut cpu.bus);
                }
            }
            CycleBatchExit::FlineTrap { opcode: _ } => {
                cpu.core.take_fline_exception(&mut cpu.bus);
            }
            CycleBatchExit::TrapInstruction { trap_num } => {
                cpu.core.take_trap_exception(&mut cpu.bus, trap_num);
            }
            CycleBatchExit::Breakpoint { bp_num } => {
                cpu.core.take_bkpt_exception(&mut cpu.bus);
                let _ = bp_num;
            }
        }

        if cpu.core.is_halted() {
            return run_result(M68kRsRunExit::Halted, total_cycles, total_instructions, 0);
        }
    }

    run_result(M68kRsRunExit::Budget, total_cycles, total_instructions, 0)
}
