# Makefile for Musashi 680x0 CPU and Test Suites

MUSASHIFILES     = m68kcpu.c m68kdasm.c softfloat/softfloat.c
MUSASHIGENCFILES = m68kops.c
MUSASHIGENHFILES = m68kops.h
MUSASHIGENERATOR = m68kmake

EXE =
EXEPATH = ./

.CFILES   = $(MAINFILES) $(OSDFILES) $(MUSASHIFILES) $(MUSASHIGENCFILES)
.OFILES   = $(.CFILES:%.c=%.o)

CC        = clang
CXX       = clang++
WARNINGS  = -Wall -Wextra -Wno-unused -g
CFLAGS    = $(WARNINGS)
CXXFLAGS  = $(WARNINGS) -Wno-deprecated-declarations -Wno-format -Wno-bitwise-op-parentheses
LFLAGS    = $(WARNINGS)

DELETEFILES = $(MUSASHIGENCFILES) $(MUSASHIGENHFILES) $(.OFILES) $(TARGET) $(MUSASHIGENERATOR)$(EXE) test_driver$(EXE) test_fpu$(EXE)

all: $(.OFILES)

clean:
	rm -f $(DELETEFILES)
	rm -rf $(EMU68_GEN)
	@$(MAKE) -C test clean

m68kcpu.o: $(MUSASHIGENHFILES) m68kfpu.c m68kmmu.h softfloat/softfloat.c softfloat/softfloat.h

$(MUSASHIGENCFILES) $(MUSASHIGENHFILES): $(MUSASHIGENERATOR)$(EXE)
	$(EXEPATH)$(MUSASHIGENERATOR)$(EXE)

$(MUSASHIGENERATOR)$(EXE):  $(MUSASHIGENERATOR).c
	$(CC) -o  $(MUSASHIGENERATOR)$(EXE)  $(MUSASHIGENERATOR).c

test_driver$(EXE): test/test_driver.c $(.OFILES)
	$(CC) $(CFLAGS) -o test_driver$(EXE) test/test_driver.c $(.OFILES) -I. -lm

test_fpu$(EXE): test_fpu.c softfloat/softfloat.c $(MUSASHIGENHFILES)
	$(CC) $(CFLAGS) -I. -Isoftfloat test_fpu.c softfloat/softfloat.c -o test_fpu$(EXE) -lm

EMU68_UPSTREAM = ../Emu68/upstream
EMU68_GEN = emu68_gen
EMU68_PREP = ../Emu68/hosted/prep_translator.py
EMU68_INCLUDES = -I../Emu68/hosted -I../Emu68/include \
    -I$(EMU68_UPSTREAM)/include -I$(EMU68_UPSTREAM)/include/aarch64 -I$(EMU68_UPSTREAM)/src
EMU68_MATH_SRCS = $(EMU68_UPSTREAM)/src/math/96bit.c $(EMU68_UPSTREAM)/src/math/exp10.c $(EMU68_UPSTREAM)/src/math/sin.c \
    $(EMU68_UPSTREAM)/src/math/cos.c $(EMU68_UPSTREAM)/src/math/tan.c $(EMU68_UPSTREAM)/src/math/__sin.c $(EMU68_UPSTREAM)/src/math/__cos.c \
    $(EMU68_UPSTREAM)/src/math/__tan.c $(EMU68_UPSTREAM)/src/math/__rem_pio2.c $(EMU68_UPSTREAM)/src/math/__rem_pio2_large.c \
    $(EMU68_UPSTREAM)/src/math/__expo2.c $(EMU68_UPSTREAM)/src/math/acos.c $(EMU68_UPSTREAM)/src/math/asin.c $(EMU68_UPSTREAM)/src/math/atan.c \
    $(EMU68_UPSTREAM)/src/math/atanh.c $(EMU68_UPSTREAM)/src/math/cosh.c $(EMU68_UPSTREAM)/src/math/exp.c $(EMU68_UPSTREAM)/src/math/expm1.c \
    $(EMU68_UPSTREAM)/src/math/floor.c $(EMU68_UPSTREAM)/src/math/log.c $(EMU68_UPSTREAM)/src/math/log10.c $(EMU68_UPSTREAM)/src/math/log1p.c \
    $(EMU68_UPSTREAM)/src/math/log2.c $(EMU68_UPSTREAM)/src/math/modf.c $(EMU68_UPSTREAM)/src/math/pow.c $(EMU68_UPSTREAM)/src/math/remquo.c \
    $(EMU68_UPSTREAM)/src/math/sincos.c $(EMU68_UPSTREAM)/src/math/sinh.c $(EMU68_UPSTREAM)/src/math/tanh.c
EMU68_PREP_SRCS = $(EMU68_GEN)/M68k_Translator.c $(EMU68_GEN)/M68k_MOVE.c \
    $(EMU68_GEN)/M68k_LINE0.c $(EMU68_GEN)/M68k_LINE4.c $(EMU68_GEN)/M68k_LINE5.c \
    $(EMU68_GEN)/M68k_LINE6.c $(EMU68_GEN)/M68k_LINE8.c $(EMU68_GEN)/M68k_LINE9.c \
    $(EMU68_GEN)/M68k_LINEB.c $(EMU68_GEN)/M68k_LINEC.c $(EMU68_GEN)/M68k_LINED.c \
    $(EMU68_GEN)/M68k_LINEE.c $(EMU68_GEN)/M68k_LINEF.c $(EMU68_GEN)/M68k_EA.c \
    $(EMU68_GEN)/M68k_MULDIV.c $(EMU68_GEN)/M68k_SR.c $(EMU68_GEN)/aarch64/RegisterAllocator64.c \
    $(EMU68_GEN)/M68k_CC.c $(EMU68_GEN)/md5.c $(EMU68_GEN)/tlsf.c
EMU68_C_SRCS = ../Emu68/hosted/emu68_hosted.c ../Emu68/hosted/emu68_exception.c ../Emu68/hosted/emu68_lru.c \
    ../Emu68/hosted/emu68_support.c ../Emu68/hosted/emu68_emit_exception.c \
    ../Emu68/hosted/emu68_mem.c \
    $(EMU68_PREP_SRCS) $(EMU68_MATH_SRCS)

EMU68_OBJS = $(EMU68_C_SRCS:.c=.o)

# Force Darwin RegLock/A64 wraps so upstream headers do not pin q19-q21 (Clang rejects those).
EMU68_WRAP = -include ../Emu68/include/RegLock.h -include ../Emu68/hosted/A64.h
EMU68_CC = $(CC) $(CFLAGS) -DHAVE_CONFIG_H -I../OSX64 $(EMU68_INCLUDES) -I../include -I. -Isoftfloat -I../Unix -I../dummy $(EMU68_WRAP)

$(EMU68_GEN)/%.c: $(EMU68_UPSTREAM)/src/%.c $(EMU68_PREP)
	@mkdir -p $(dir $@)
	python3 $(EMU68_PREP) $< $@

.PRECIOUS: $(EMU68_GEN)/%.c $(EMU68_GEN)/aarch64/%.c

../Emu68/hosted/%.o: ../Emu68/hosted/%.c
	$(EMU68_CC) -c $< -o $@

$(EMU68_GEN)/%.o: $(EMU68_GEN)/%.c
	$(EMU68_CC) -c $< -o $@

$(EMU68_GEN)/aarch64/%.o: $(EMU68_GEN)/aarch64/%.c
	$(EMU68_CC) -c $< -o $@

# Math objects must use the hosted libm.h wrap (Clang fsqrt). This rule must
# precede $(EMU68_UPSTREAM)/src/%.o so BSD make does not steal math/ files.
# exp10/log/log10/log2 include system math.h; the wrap's remquo/sincos macros conflict.
$(EMU68_UPSTREAM)/src/math/exp10.o: $(EMU68_UPSTREAM)/src/math/exp10.c
	$(EMU68_CC) -c $< -o $@
$(EMU68_UPSTREAM)/src/math/log.o: $(EMU68_UPSTREAM)/src/math/log.c
	$(EMU68_CC) -c $< -o $@
$(EMU68_UPSTREAM)/src/math/log10.o: $(EMU68_UPSTREAM)/src/math/log10.c
	$(EMU68_CC) -c $< -o $@
$(EMU68_UPSTREAM)/src/math/log2.o: $(EMU68_UPSTREAM)/src/math/log2.c
	$(EMU68_CC) -c $< -o $@

$(EMU68_UPSTREAM)/src/math/%.o: $(EMU68_UPSTREAM)/src/math/%.c
	$(EMU68_CC) -include ../Emu68/hosted/math/libm.h -c $< -o $@

$(EMU68_UPSTREAM)/src/%.o: $(EMU68_UPSTREAM)/src/%.c
	$(EMU68_CC) -c $< -o $@

# Amiberry 680x0 + JIT objects are built by the OSX64 Makefile (same flags as the app).
OSX64_OBJ = ../OSX64/obj
AMIBERRY_TEST_OBJS = \
	$(OSX64_OBJ)/amiberry_newcpu.o \
	$(OSX64_OBJ)/amiberry_newcpu_common.o \
	$(OSX64_OBJ)/amiberry_readcpu.o \
	$(OSX64_OBJ)/amiberry_cpudefs.o \
	$(OSX64_OBJ)/amiberry_cpustbl.o \
	$(OSX64_OBJ)/amiberry_fpp.o \
	$(OSX64_OBJ)/amiberry_fpp_softfloat.o \
	$(OSX64_OBJ)/amiberry_fpp_native.o \
	$(OSX64_OBJ)/amiberry_cpuemu_0.o \
	$(OSX64_OBJ)/amiberry_cpuemu_11.o \
	$(OSX64_OBJ)/amiberry_cpuemu_13.o \
	$(OSX64_OBJ)/amiberry_cpuemu_20.o \
	$(OSX64_OBJ)/amiberry_cpuemu_21.o \
	$(OSX64_OBJ)/amiberry_cpuemu_22.o \
	$(OSX64_OBJ)/amiberry_cpuemu_23.o \
	$(OSX64_OBJ)/amiberry_cpuemu_24.o \
	$(OSX64_OBJ)/amiberry_cpuemu_31.o \
	$(OSX64_OBJ)/amiberry_cpuemu_32.o \
	$(OSX64_OBJ)/amiberry_cpuemu_33.o \
	$(OSX64_OBJ)/amiberry_cpuemu_34.o \
	$(OSX64_OBJ)/amiberry_cpuemu_35.o \
	$(OSX64_OBJ)/amiberry_cpuemu_40.o \
	$(OSX64_OBJ)/amiberry_cpuemu_50.o \
	$(OSX64_OBJ)/amiberry_cpummu.o \
	$(OSX64_OBJ)/amiberry_cpummu30.o \
	$(OSX64_OBJ)/amiberry_vm.o \
	$(OSX64_OBJ)/amiberry_jit_compemu.o \
	$(OSX64_OBJ)/amiberry_jit_compstbl.o \
	$(OSX64_OBJ)/amiberry_jit_compemu_fpp.o \
	$(OSX64_OBJ)/amiberry_jit_compemu_support.o \
	$(OSX64_OBJ)/amiberry_sf_softfloat.o \
	$(OSX64_OBJ)/amiberry_sf_softfloat_decimal.o \
	$(OSX64_OBJ)/amiberry_sf_softfloat_fpsp.o \
	$(OSX64_OBJ)/amiberry_hosted_host.o \
	$(OSX64_OBJ)/amiberry_hosted_memory.o

.PHONY: amiberry-test-objs
amiberry-test-objs:
	$(MAKE) -C ../OSX64 amiberry-objs

SYN68K_DIR = ../syn68k
SYN68K_RUNTIME = $(SYN68K_DIR)/runtime
SYN68K_SYNGEN = $(SYN68K_DIR)/syngen
SYN68K_INCLUDES = -I$(SYN68K_DIR)/include -I$(SYN68K_RUNTIME)/include -I$(SYN68K_RUNTIME)
SYN68K_DEFS = -DRUNTIME -DNONNATIVE -DNONDIRECT
SYN68K_CC = $(CC) $(CFLAGS) -DHAVE_CONFIG_H $(SYN68K_DEFS) -I../OSX64 $(SYN68K_INCLUDES) -I../include -I. -Isoftfloat -I../Unix -I../dummy -Wno-parentheses-equality -Wno-unused-value -Wno-constant-logical-operand

SYN68K_C_SRCS = $(SYN68K_RUNTIME)/alloc.c $(SYN68K_RUNTIME)/backpatch.c $(SYN68K_RUNTIME)/block.c \
    $(SYN68K_RUNTIME)/blockinfo.c $(SYN68K_RUNTIME)/callback.c $(SYN68K_RUNTIME)/checksum.c \
    $(SYN68K_RUNTIME)/deathqueue.c $(SYN68K_RUNTIME)/destroyblock.c $(SYN68K_RUNTIME)/diagnostics.c \
    $(SYN68K_RUNTIME)/hash.c $(SYN68K_RUNTIME)/init.c $(SYN68K_RUNTIME)/interrupt.c \
    $(SYN68K_RUNTIME)/mapindex.c $(SYN68K_RUNTIME)/mapinfo.c $(SYN68K_RUNTIME)/native.c \
    $(SYN68K_RUNTIME)/opcode_dummy.c $(SYN68K_RUNTIME)/profile.c $(SYN68K_RUNTIME)/rangetree.c \
    $(SYN68K_RUNTIME)/recompile.c $(SYN68K_RUNTIME)/syn68k.c $(SYN68K_RUNTIME)/translate.c \
    $(SYN68K_RUNTIME)/trap.c

SYN68K_OBJS = $(SYN68K_C_SRCS:.c=.o)

$(SYN68K_SYNGEN)/syngen: $(wildcard $(SYN68K_SYNGEN)/*.c)
	$(CC) -O2 -DSYNGEN -DNONNATIVE -DNONDIRECT -I$(SYN68K_SYNGEN)/include -I$(SYN68K_DIR)/include $(wildcard $(SYN68K_SYNGEN)/*.c) -o $@

$(SYN68K_RUNTIME)/syn68k.c $(SYN68K_RUNTIME)/mapindex.c $(SYN68K_RUNTIME)/mapinfo.c: $(SYN68K_SYNGEN)/syngen $(SYN68K_RUNTIME)/68k.scm $(SYN68K_RUNTIME)/syn68k_header.h $(SYN68K_RUNTIME)/68k.defines.scm
	cd $(SYN68K_RUNTIME) && ../syngen/syngen 68k.scm

$(SYN68K_RUNTIME)/%.o: $(SYN68K_RUNTIME)/%.c
	$(SYN68K_CC) -c $< -o $@

TESTS_68000 = abcd adda add_i addq add addx andi_to_ccr andi_to_sr and \
               bcc bchg bclr bool_i bset bsr btst \
               chk cmpa cmpm cmp dbcc divs divu eori_to_ccr eori_to_sr eor exg ext \
               lea_pea lea_tas lea_tst links \
               movem movep moveq move move_usp move_xxx_flags muls mulu \
               nbcd negs op_cmp_i ori_to_ccr ori_to_sr or \
               rox roxx rtr sbcd scc shifts2 shifts suba sub_i subq sub subx swap trapv

TESTS_68040 = bfchg bfclr bfext bfffo bfins bfset bftst cas chk2 cmp2 \
	divs_long divu_long interrupt jmp mul_long rtd shifts3 trapcc

TESTS_68000_RUN = $(TESTS_68000:%=%.bin)
$(TESTS_68000_RUN): test_driver$(EXE)
	./test_driver$(EXE) test/mc68000/$@

TESTS_68040_RUN = $(TESTS_68040:%=%.bin)
$(TESTS_68040_RUN): test_driver$(EXE)
	./test_driver$(EXE) test/mc68040/$@

build_tests:
	@$(MAKE) -C test all

test: $(TESTS_68000_RUN) $(TESTS_68040_RUN) test_fpu$(EXE)
	./test_fpu$(EXE)
	$(MAKE) -C ../tests test
