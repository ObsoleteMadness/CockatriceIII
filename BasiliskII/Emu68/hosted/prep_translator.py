#!/usr/bin/env python3
"""Mechanical hosted TARGET substitutions on unmodified upstream translator .c files.

Does not fork LINE*.c. Applies only:
  - Mach-O has no GNU function aliases (Clang on Darwin)
  - C-level WFI/WFE hangs become emu68_hosted_hang()
  - Dual-map JIT high bit is cleared (no EL1 MMU alias)
  - Host pointer movz/movk halfwords are little-endian (hw0 = bits 15:0)
  - md5 / GetSRMask guest fetches always use cache_read_* (A0 is not a host pointer)
  - LINEF EL1 dcache maintenance and GNU .globl trampolines become EL0-safe
"""
import re
import sys


def rewrite_aliases(src: str) -> str:
    pattern = re.compile(
        r"((?:static\s+)?)(uint32_t)\s+(\w+)\s*\(([^)]*)\)\s*"
        r"__attribute__\s*\(\s*\(\s*alias\s*\(\s*\"(\w+)\"\s*\)\s*\)\s*\)\s*;",
        re.MULTILINE,
    )
    trampolines = []

    def repl(m: re.Match) -> str:
        static, ret, name, args, target = m.groups()
        names = []
        for part in args.split(","):
            part = part.strip()
            if not part:
                continue
            tok = part.split()[-1].lstrip("*")
            names.append(tok)
        call = ", ".join(names) if names else args
        trampolines.append(
            f"{static}{ret} {name}({args})\n"
            f"{{\n"
            f"    return {target}({call});\n"
            f"}}\n"
        )
        return f"{static}{ret} {name}({args});\n"

    src = pattern.sub(repl, src)
    if trampolines:
        src += "\n/* Mach-O has no GNU function aliases; trampolines appended. */\n"
        src += "\n".join(trampolines)
    return src


def rewrite_linef_el1(src: str) -> str:
    """Replace EL1 cache-maintenance asm with Darwin-safe ret stubs.

    Upstream LINEF emits MRS CLIDR_EL1 / DC CISW sequences and GNU .globl
    labels. Those encodings SIGILL in a userspace TARGET, and Mach-O needs
    the leading underscore on the exported labels.
    """
    src = src.replace(
        '__asm__ volatile(".globl trampoline_icache_invalidate\\n'
        'trampoline_icache_invalidate: bl invalidate_instruction_cache\\n\\tbr x0");',
        '__asm__ volatile(".globl _trampoline_icache_invalidate\\n'
        '_trampoline_icache_invalidate: bl _invalidate_instruction_cache\\n\\tbr x0");',
    )

    dcache_stub = (
        "void __attribute__((used)) {cname}(void)\n"
        "{{\n"
        "    __asm__ volatile(\n"
        '        ".globl {label}\\n"\n'
        '        "{label}:\\n"\n'
        '        "ret\\n"\n'
        "    );\n"
        "}}\n"
    )

    src = re.sub(
        r"void\s+__attribute__\(\(used\)\)\s+__clear_entire_dcache\(void\)\s*\{.*?\n\}\n",
        lambda _m: dcache_stub.format(
            cname="__clear_entire_dcache", label="_clear_entire_dcache"
        ),
        src,
        count=1,
        flags=re.S,
    )
    src = re.sub(
        r"void\s+__attribute__\(\(used\)\)\s+__invalidate_entire_dcache\(void\)\s*\{.*?\n\}\n",
        lambda _m: dcache_stub.format(
            cname="__invalidate_entire_dcache", label="_invalidate_entire_dcache"
        ),
        src,
        count=1,
        flags=re.S,
    )
    return src


def rewrite(src: str, path: str) -> str:
    src = rewrite_aliases(src)
    src = src.replace(
        'while(1) asm volatile("wfi");',
        "emu68_hosted_hang();",
    )
    src = src.replace(
        'while (1) asm volatile("wfe");',
        "emu68_hosted_hang();",
    )
    src = src.replace(
        " | 0x0000001000000000ULL",
        "",
    )
    src = src.replace(
        " | 0x0000001000000000ull",
        "",
    )
    # Upstream overlays a uint64 pointer on uint16[4] and emits u16[3] first.
    # On little-endian AArch64 that swaps the halves, so blr lands on
    # 0x....00010000 (Darwin code ptr 0x00000001xxxxxxxx reversed). Mac ROM
    # MOVEC to CACR calls check_cacr this way.
    src = src.replace("u.u16[3], 0)", "u.u16[__LE0], 0)")
    src = src.replace("u.u16[2], 1)", "u.u16[__LE1], 1)")
    src = src.replace("u.u16[1], 2)", "u.u16[__LE2], 2)")
    src = src.replace("u.u16[0], 3)", "u.u16[__LE3], 3)")
    src = src.replace("u.u16[__LE0], 0)", "u.u16[0], 0)")
    src = src.replace("u.u16[__LE1], 1)", "u.u16[1], 1)")
    src = src.replace("u.u16[__LE2], 2)", "u.u16[2], 2)")
    src = src.replace("u.u16[__LE3], 3)", "u.u16[3], 3)")
    if path.endswith("md5.c"):
        # Always take the cache_read path; guest addresses are not host pointers.
        src = src.replace("if (s > 0x01000000)", "if (0 && s > 0x01000000)")
    if path.endswith("M68k_SR.c"):
        # DBcc displacement is a guest word; insn_stream is a 32-bit Mac address.
        src = src.replace(
            "int32_t branch_offset = (int16_t)(insn_stream[1]);",
            "int32_t branch_offset = (int16_t)cache_read_16(ICACHE, (uint32_t)(uintptr_t)&insn_stream[1]);",
        )
    if path.endswith("M68k_LINEF.c"):
        src = rewrite_linef_el1(src)
    if path.endswith("M68k_Translator.c"):
        if '#include "emu68_hosted.h"' not in src:
            src = '#include "emu68_hosted.h"\n' + src
        # Basilisk Execute68k/EmulOp hooks (0x7100..0x71FF) must not be translated
        # as 680x0 opcodes. RTS into a synthetic 0x7100 return stub is inside JIT blocks.
        needle = "    host_flags = 0;\n"
        insert = (
            "    /* Hosted Basilisk EmulOp / EXEC_RETURN (illegal MOVEQ 0x71xx). */\n"
            "    if ((opcode & 0xff00) == 0x7100) {\n"
            "        EMIT_LoadHostPointer(ctx, 2, (uintptr_t)(void *)emu68_hosted_emulop);\n"
            "        EMIT(ctx, mov_immed_u16(0, opcode, 0), blr(2));\n"
            "        EMIT(ctx, INSN_TO_LE(MARKER_STOP));\n"
            "        ctx->tc_M68kCodePtr++;\n"
            "        return 1;\n"
            "    }\n\n"
        )
        if needle in src and insert not in src:
            src = src.replace(needle, insert + needle, 1)
    if "emu68_hosted_hang" in src and "emu68_hosted.h" not in src:
        src = '#include "emu68_hosted.h"\n' + src
    return src


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: prep_translator.py IN.c OUT.c", file=sys.stderr)
        return 2
    src = open(sys.argv[1], encoding="utf-8", errors="replace").read()
    open(sys.argv[2], "w", encoding="utf-8").write(rewrite(src, sys.argv[1]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
