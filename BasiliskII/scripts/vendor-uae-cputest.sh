#!/bin/sh
# Vendor Toni Wilen WinUAE cputest into amiberry/cputest/vendor/.
#
# Upstream layout:
#   WinUAE/cputest/          — native m68k replay harness + cputestgen.ini
#   WinUAE/cputest.cpp       — hosted test generator (CPU_TESTER self-check)
#
# gencpu CPU_TEST outputs (cpuemu_90_test.cpp …) are not committed to WinUAE;
# fetch pre-generated copies from Amiberry (same gencpu toolchain).
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/amiberry/cputest/vendor"
WINUAE="https://raw.githubusercontent.com/tonioni/WinUAE/master"
AMIBERRY="https://raw.githubusercontent.com/BlitterStudio/amiberry/master/src"

mkdir -p "$DEST/cputest/adis"

fetch() {
	local url="$1"
	local out="$2"
	mkdir -p "$(dirname "$out")"
	echo "fetch $(basename "$out")"
	curl -fsSL "$url" -o "$out"
}

# Hosted cputestgen (cputest_support/disasm from Amiberry: matches hosted debugmem API)
for f in cputest.cpp ini.cpp; do
	fetch "$WINUAE/$f" "$DEST/$f"
done
fetch "$AMIBERRY/cputest_support.cpp" "$DEST/cputest_support.cpp"
fetch "$AMIBERRY/disasm.cpp" "$DEST/disasm.cpp"

# WinUAE cputest/ — native harness, generator ini, disassembler tables
for f in \
	cputest/cputest_defines.h \
	cputest/cputestgen.ini \
	cputest/readme.txt \
	cputest/main.c \
	cputest/dir.c \
	cputest/msc_dirent.h \
	cputest/amiga.S \
	cputest/asm.S \
	cputest/asm040.S \
	cputest/asm060.S \
	cputest/atari.S \
	cputest/inflate.S \
	cputest/makefile \
	cputest/makefile.st \
	cputest/adis/decode_ea.c \
	cputest/adis/defs.h \
	cputest/adis/globals.c \
	cputest/adis/opcode_handler_cpu.c \
	cputest/adis/opcode_handler_fpu.c \
	cputest/adis/opcode_handler_mmu.c \
	cputest/adis/opcodes_cpu.c \
	cputest/adis/opcodes_fpu.c \
	cputest/adis/opcodes_mmu.c \
	cputest/adis/string_recog.h \
	cputest/adis/util.c
do
	fetch "$WINUAE/$f" "$DEST/$f"
done

# gencpu CPU_TEST=1 outputs (not in WinUAE git; Amiberry ships compatible copies)
echo "fetch gencpu CPU_TEST outputs (Amiberry pre-built; WinUAE requires local gencpu)"
for f in \
	cputbl_test.h \
	cpustbl_test.cpp \
	cpuemu_90_test.cpp \
	cpuemu_91_test.cpp \
	cpuemu_92_test.cpp \
	cpuemu_93_test.cpp \
	cpuemu_94_test.cpp \
	cpuemu_95_test.cpp
do
	fetch "$AMIBERRY/$f" "$DEST/$f"
done

cat >"$DEST/UPSTREAM.txt" <<EOF
WinUAE cputest vendored from tonioni/WinUAE master ($(date -u +%Y-%m-%d)).
Generated cpuemu_*_test.cpp / cpustbl_test.cpp from Amiberry master (gencpu CPU_TEST output).
Hosted patches applied to cputest.cpp (nzcv flags, printf).
EOF

# Hosted Cockatrice: Amiberry m68k.h uses regflags.nzcv and supplies cctrue().
sed -i '' 's/regflags\.cznv/regflags.nzcv/g' "$DEST/cputest.cpp"
sed -i '' 's/\.cznv/.nzcv/g' "$DEST"/cpuemu_*_test.cpp "$DEST/cpustbl_test.cpp" 2>/dev/null || true
sed -i '' 's/wprintf/printf/g' "$DEST/cputest.cpp"
sed -i '' 's/wprintf/printf/g' "$DEST/cputest_support.cpp"
sed -i '' 's/fp_init_native(void)/fp_init_native(void)/; s/printf(_T("fp_init_native called!"));//; s/exit(0);//g' "$DEST/cputest_support.cpp" 2>/dev/null || true
perl -0pi -e 's/void fp_init_native\(void\)\s*\{\s*printf\(_T\("fp_init_native called!"\)\);\s*exit\(0\);\s*\}/void fp_init_native(void)\n{\n}/s; s/bool fp_init_native_80\(void\)\s*\{\s*printf\(_T\("fp_init_native_80 called!"\)\);\s*exit\(0\);\s*return false;\s*\}/bool fp_init_native_80(void)\n{\n\treturn false;\n}/s' "$DEST/cputest_support.cpp"
sed -i '' 's/fgetws/fgets/g; s/fputws/fputs/g; s/fwprintf/fprintf/g' "$DEST/ini.cpp"
sed -i '' 's/void f_out(void \*f/void f_out(FILE *f/g' "$DEST/cputest_support.cpp"
perl -0pi -e 's/\nint cctrue\(int cc\)\n\{.*?\n\}//s' "$DEST/cputest.cpp"
perl -0pi -e 's/\nstatic void my_trim\(TCHAR \*s\)\n\{.*?\n\}//s' "$DEST/cputest.cpp"

echo "Vendored WinUAE cputest into $DEST"
