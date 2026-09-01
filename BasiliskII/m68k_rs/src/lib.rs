//! C ABI shim between CockatriceIII and the m68k-rs interpreter core.

use m68k::{
    AddressBus, CpuCore, CpuType, CycleBatchControl, CycleBatchExit, CycleBoundaryEvent,
};
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

struct BasiliskBus {
    callbacks: *const M68kRsHostCallbacks,
}

impl BasiliskBus {
    fn callbacks(&self) -> &M68kRsHostCallbacks {
        unsafe { &*self.callbacks }
    }
}

impl AddressBus for BasiliskBus {
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
        },
        stop_requested: false,
    });
    boxed.bus.callbacks = &boxed.callbacks;
    Box::into_raw(boxed)
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
