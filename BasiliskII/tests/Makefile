# BasiliskII/tests — CPU suite + Basilisk subsystem tests
#
# Shared objects go in obj/libcockatrice_test.a so the four 680x0 engines
# compile once. Run `make test` (Basilisk is the gate; CPU is reported).
# `make test-strict` fails on CPU failures too.

SHELL = /bin/sh
CC = clang
CXX = clang++
WARNINGS = -Wall -Wextra -Wno-unused -g
CFLAGS = $(WARNINGS) -Wno-deprecated-non-prototype
CXXFLAGS = $(WARNINGS) -Wno-deprecated-declarations -Wno-format -Wno-bitwise-op-parentheses -std=c++17

ARCH ?= $(shell uname -m)
ifeq ($(ARCH),x86_64)
  TARGET_ARCH = -arch x86_64
else ifeq ($(ARCH),amd64)
  TARGET_ARCH = -arch x86_64
else
  TARGET_ARCH = -arch arm64
endif
CFLAGS += $(TARGET_ARCH)
CXXFLAGS += $(TARGET_ARCH)
LDFLAGS += $(TARGET_ARCH)

REPO_ROOT = $(abspath ../..)
MUSASHI = ../Musashi
OSX64 = ../OSX64
OBJ_DIR = obj
M68K_RS_DIR = ../m68k_rs
M68K_RS_TARGET = $(OBJ_DIR)/m68k_rs_target
M68K_RS_LIB = $(M68K_RS_TARGET)/release/libcockatrice_m68k_rs.a
EMU68_GEN = $(OBJ_DIR)/emu68_gen
EMU68_UPSTREAM = ../Emu68/upstream
EMU68_PREP = ../Emu68/hosted/prep_translator.py

DEFS = -DHAVE_CONFIG_H -D_REENTRANT \
	-DTEST_REPO_ROOT=\"$(REPO_ROOT)\" \
	-DMUSASHI_TEST_DIR=\"$(abspath $(MUSASHI)/test)\" \
	-DUAE_CPUTEST_BIN_PATH=\"$(abspath $(OBJ_DIR)/uae_cputest)\" \
	-DSYN68K_BATTERY_BIN_PATH=\"$(abspath $(OBJ_DIR)/syn68k_battery)\" \
	-DSYN68K_BATTERY_GOLDEN_PATH=\"$(abspath $(SYN68K_DIR)/test/output/10000-notnative)\"

INCLUDES = -Iinclude -I$(OSX64) -I../include -I$(MUSASHI) -I$(MUSASHI)/softfloat \
	-I../Unix -I../dummy -I../amiberry -I../SDL -I../m68k_rs/include

EMU68_INCLUDES = -I../Emu68/hosted -I../Emu68/include \
	-I$(EMU68_UPSTREAM)/include -I$(EMU68_UPSTREAM)/include/aarch64 \
	-I$(EMU68_UPSTREAM)/src
EMU68_WRAP = -include ../Emu68/include/RegLock.h -include ../Emu68/hosted/A64.h

SYN68K_DIR = ../syn68k
SYN68K_RUNTIME = $(SYN68K_DIR)/runtime
SYN68K_SYNGEN = $(SYN68K_DIR)/syngen
SYN68K_INCLUDES = -I$(SYN68K_DIR)/include -I$(SYN68K_RUNTIME)/include -I$(SYN68K_RUNTIME)
SYN68K_DEFS = -DRUNTIME -DNONNATIVE -DNONDIRECT

CPPFLAGS = $(DEFS) $(INCLUDES) $(EMU68_INCLUDES) $(SYN68K_INCLUDES)

ENTITLEMENTS = $(OSX64)/entitlements.plist

CPUTEST_VENDOR = ../amiberry/cputest/vendor
CPUTEST_VENDOR_STAMP = $(CPUTEST_VENDOR)/.vendored
CPUTEST_DEFS = -DCPU_TESTER -DCOMPILER=2 -DAMIBERRY -DUAE -DHAVE_CONFIG_H \
	-DJIT -DUSE_JIT_FPU -DFPUEMU -DWITH_SOFTFLOAT -DDEBUGGER \
	-DCPUEMU_90 -DCPUEMU_91 -DCPUEMU_92 -DCPUEMU_93 -DCPUEMU_94 -DCPUEMU_95
CPUTEST_INCLUDES = -I../amiberry/hosted -I../amiberry -I../amiberry/src/include \
	-I../amiberry/src -I../amiberry/src/threaddep -I../amiberry/src/softfloat \
	-I$(CPUTEST_VENDOR) -I$(CPUTEST_VENDOR)/cputest \
	-I../Musashi/softfloat -I../include -I$(OSX64)
CPUTEST_CXXFLAGS = $(CXXFLAGS) $(CPUTEST_DEFS) $(CPUTEST_INCLUDES) \
	-include ../amiberry/hosted/tchar.h -fno-strict-aliasing
CPUTEST_SRCS = \
	$(CPUTEST_VENDOR)/cputest.cpp \
	$(CPUTEST_VENDOR)/cputest_support.cpp \
	../amiberry/cputest/hosted/cputest_support_overrides.cpp \
	$(CPUTEST_VENDOR)/cpustbl_test.cpp \
	$(CPUTEST_VENDOR)/cpuemu_90_test.cpp \
	$(CPUTEST_VENDOR)/cpuemu_91_test.cpp \
	$(CPUTEST_VENDOR)/cpuemu_92_test.cpp \
	$(CPUTEST_VENDOR)/cpuemu_93_test.cpp \
	$(CPUTEST_VENDOR)/cpuemu_94_test.cpp \
	$(CPUTEST_VENDOR)/cpuemu_95_test.cpp \
	$(CPUTEST_VENDOR)/disasm.cpp \
	$(CPUTEST_VENDOR)/ini.cpp \
	../amiberry/src/readcpu.cpp \
	../amiberry/src/cpudefs.cpp \
	../amiberry/src/newcpu_common.cpp \
	../amiberry/src/fpp.cpp \
	../amiberry/src/fpp_softfloat.cpp \
	../amiberry/src/softfloat/softfloat.cpp \
	../amiberry/src/softfloat/softfloat_fpsp.cpp \
	../amiberry/src/softfloat/softfloat_decimal.cpp
CPUTEST_OBJS = $(addprefix $(OBJ_DIR)/cputest_,$(notdir $(CPUTEST_SRCS:.cpp=.o)))

SYN68K_TEST_DIR = $(SYN68K_DIR)/test
SYN68K_BATTERY_SRCS = \
	$(SYN68K_TEST_DIR)/main.c \
	$(SYN68K_TEST_DIR)/driver.c \
	$(SYN68K_TEST_DIR)/tests.c \
	$(SYN68K_TEST_DIR)/testbattery.c \
	$(SYN68K_TEST_DIR)/setup.c \
	$(SYN68K_TEST_DIR)/testtrap.c \
	$(SYN68K_TEST_DIR)/crc.c \
	$(SYN68K_TEST_DIR)/testrt.c \
	$(SYN68K_TEST_DIR)/testqsort.c
SYN68K_BATTERY_OBJS = $(patsubst $(SYN68K_TEST_DIR)/%.c,$(OBJ_DIR)/syn68k_batt_%.o,$(SYN68K_BATTERY_SRCS))

.PHONY: all test test-strict test-basilisk test-cpu clean amiberry-objs cpu_tests basilisk_tests \
	vendor-cputest uae_cputest syn68k_battery

all: cpu_tests basilisk_tests

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR) $(EMU68_GEN) $(EMU68_GEN)/aarch64

$(MUSASHI)/m68kops.c $(MUSASHI)/m68kops.h: $(OBJ_DIR)/m68kmake
	cd $(MUSASHI) && ../tests/$(OBJ_DIR)/m68kmake

$(OBJ_DIR)/m68kmake: $(MUSASHI)/m68kmake.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -o $@ $<

OSX64_OBJ = $(OSX64)/obj
AMIBERRY_OBJS = \
	$(OSX64_OBJ)/amiberry_newcpu.o $(OSX64_OBJ)/amiberry_newcpu_common.o \
	$(OSX64_OBJ)/amiberry_readcpu.o $(OSX64_OBJ)/amiberry_cpudefs.o \
	$(OSX64_OBJ)/amiberry_cpustbl.o $(OSX64_OBJ)/amiberry_fpp.o \
	$(OSX64_OBJ)/amiberry_fpp_softfloat.o $(OSX64_OBJ)/amiberry_fpp_native.o \
	$(OSX64_OBJ)/amiberry_cpuemu_0.o $(OSX64_OBJ)/amiberry_cpuemu_11.o \
	$(OSX64_OBJ)/amiberry_cpuemu_13.o $(OSX64_OBJ)/amiberry_cpuemu_20.o \
	$(OSX64_OBJ)/amiberry_cpuemu_21.o $(OSX64_OBJ)/amiberry_cpuemu_22.o \
	$(OSX64_OBJ)/amiberry_cpuemu_23.o $(OSX64_OBJ)/amiberry_cpuemu_24.o \
	$(OSX64_OBJ)/amiberry_cpuemu_31.o $(OSX64_OBJ)/amiberry_cpuemu_32.o \
	$(OSX64_OBJ)/amiberry_cpuemu_33.o $(OSX64_OBJ)/amiberry_cpuemu_34.o \
	$(OSX64_OBJ)/amiberry_cpuemu_35.o $(OSX64_OBJ)/amiberry_cpuemu_40.o \
	$(OSX64_OBJ)/amiberry_cpuemu_50.o $(OSX64_OBJ)/amiberry_cpummu.o \
	$(OSX64_OBJ)/amiberry_cpummu30.o $(OSX64_OBJ)/amiberry_vm.o \
	$(OSX64_OBJ)/amiberry_jit_compemu.o $(OSX64_OBJ)/amiberry_jit_compstbl.o \
	$(OSX64_OBJ)/amiberry_jit_compemu_fpp.o $(OSX64_OBJ)/amiberry_jit_compemu_support.o \
	$(OSX64_OBJ)/amiberry_sf_softfloat.o $(OSX64_OBJ)/amiberry_sf_softfloat_decimal.o \
	$(OSX64_OBJ)/amiberry_sf_softfloat_fpsp.o \
	$(OSX64_OBJ)/amiberry_hosted_host.o $(OSX64_OBJ)/amiberry_hosted_memory.o

amiberry-objs:
	$(MAKE) -C $(OSX64) amiberry-objs

EMU68_HOSTED_SRCS = ../Emu68/hosted/emu68_hosted.c ../Emu68/hosted/emu68_exception.c \
	../Emu68/hosted/emu68_lru.c ../Emu68/hosted/emu68_support.c \
	../Emu68/hosted/emu68_emit_exception.c ../Emu68/hosted/emu68_mem.c

$(EMU68_GEN)/%.c: $(EMU68_UPSTREAM)/src/%.c $(EMU68_PREP) | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	python3 $(EMU68_PREP) $< $@

.PRECIOUS: $(EMU68_GEN)/%.c $(EMU68_GEN)/aarch64/%.c

EMU68_CC = $(CC) $(CFLAGS) $(DEFS) -I$(OSX64) $(EMU68_INCLUDES) -I../include \
	-I$(MUSASHI) -I$(MUSASHI)/softfloat -I../Unix -I../dummy $(EMU68_WRAP)

$(OBJ_DIR)/emu68_hosted_%.o: ../Emu68/hosted/%.c | $(OBJ_DIR)
	$(EMU68_CC) -c $< -o $@

$(OBJ_DIR)/emu68_gen_%.o: $(EMU68_GEN)/%.c | $(OBJ_DIR)
	$(EMU68_CC) -c $< -o $@

$(OBJ_DIR)/emu68_ra64.o: $(EMU68_GEN)/aarch64/RegisterAllocator64.c | $(OBJ_DIR)
	$(EMU68_CC) -c $< -o $@

$(OBJ_DIR)/emu68_math_exp10.o: $(EMU68_UPSTREAM)/src/math/exp10.c | $(OBJ_DIR)
	$(EMU68_CC) -c $< -o $@
$(OBJ_DIR)/emu68_math_log.o: $(EMU68_UPSTREAM)/src/math/log.c | $(OBJ_DIR)
	$(EMU68_CC) -c $< -o $@
$(OBJ_DIR)/emu68_math_log10.o: $(EMU68_UPSTREAM)/src/math/log10.c | $(OBJ_DIR)
	$(EMU68_CC) -c $< -o $@
$(OBJ_DIR)/emu68_math_log2.o: $(EMU68_UPSTREAM)/src/math/log2.c | $(OBJ_DIR)
	$(EMU68_CC) -c $< -o $@

$(OBJ_DIR)/emu68_math_%.o: $(EMU68_UPSTREAM)/src/math/%.c | $(OBJ_DIR)
	$(EMU68_CC) -include ../Emu68/hosted/math/libm.h -c $< -o $@

EMU68_HOSTED_OBJS = $(patsubst ../Emu68/hosted/%.c,$(OBJ_DIR)/emu68_hosted_%.o,$(EMU68_HOSTED_SRCS))
EMU68_GEN_BASENAME = M68k_Translator M68k_MOVE M68k_LINE0 M68k_LINE4 M68k_LINE5 \
	M68k_LINE6 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED \
	M68k_LINEE M68k_LINEF M68k_EA M68k_MULDIV M68k_SR M68k_CC md5 tlsf
EMU68_GEN_OBJS = $(patsubst %,$(OBJ_DIR)/emu68_gen_%.o,$(EMU68_GEN_BASENAME)) $(OBJ_DIR)/emu68_ra64.o
EMU68_MATH_SPECIAL = exp10 log log10 log2
EMU68_MATH_REST = 96bit sin cos tan __sin __cos __tan __rem_pio2 __rem_pio2_large \
	__expo2 acos asin atan atanh cosh exp expm1 floor log1p modf pow remquo sincos sinh tanh
EMU68_MATH_OBJS = $(patsubst %,$(OBJ_DIR)/emu68_math_%.o,$(EMU68_MATH_SPECIAL) $(EMU68_MATH_REST))
EMU68_OBJS = $(EMU68_HOSTED_OBJS) $(EMU68_GEN_OBJS) $(EMU68_MATH_OBJS) $(OBJ_DIR)/emu68_glue.o

SYN68K_C_SRCS = $(SYN68K_RUNTIME)/alloc.c $(SYN68K_RUNTIME)/backpatch.c $(SYN68K_RUNTIME)/block.c \
	$(SYN68K_RUNTIME)/blockinfo.c $(SYN68K_RUNTIME)/callback.c $(SYN68K_RUNTIME)/checksum.c \
	$(SYN68K_RUNTIME)/deathqueue.c $(SYN68K_RUNTIME)/destroyblock.c $(SYN68K_RUNTIME)/diagnostics.c \
	$(SYN68K_RUNTIME)/hash.c $(SYN68K_RUNTIME)/init.c $(SYN68K_RUNTIME)/interrupt.c \
	$(SYN68K_RUNTIME)/mapindex.c $(SYN68K_RUNTIME)/mapinfo.c $(SYN68K_RUNTIME)/native.c \
	$(SYN68K_RUNTIME)/opcode_dummy.c $(SYN68K_RUNTIME)/profile.c $(SYN68K_RUNTIME)/rangetree.c \
	$(SYN68K_RUNTIME)/recompile.c $(SYN68K_RUNTIME)/syn68k.c $(SYN68K_RUNTIME)/translate.c \
	$(SYN68K_RUNTIME)/trap.c
SYN68K_CC = $(CC) $(CFLAGS) $(DEFS) $(SYN68K_DEFS) -I$(OSX64) $(SYN68K_INCLUDES) \
	-I../include -I$(MUSASHI) -I$(MUSASHI)/softfloat -I../Unix -I../dummy \
	-Wno-parentheses-equality -Wno-unused-value -Wno-constant-logical-operand

$(SYN68K_SYNGEN)/syngen: $(wildcard $(SYN68K_SYNGEN)/*.c)
	$(CC) -O2 -DSYNGEN -DNONNATIVE -DNONDIRECT -I$(SYN68K_SYNGEN)/include -I$(SYN68K_DIR)/include $(wildcard $(SYN68K_SYNGEN)/*.c) -o $@

$(SYN68K_RUNTIME)/syn68k.c $(SYN68K_RUNTIME)/mapindex.c $(SYN68K_RUNTIME)/mapinfo.c: $(SYN68K_SYNGEN)/syngen $(SYN68K_RUNTIME)/68k.scm
	cd $(SYN68K_RUNTIME) && ../syngen/syngen 68k.scm

$(OBJ_DIR)/syn68k_rt_%.o: $(SYN68K_RUNTIME)/%.c | $(OBJ_DIR)
	$(SYN68K_CC) -c $< -o $@

SYN68K_RT_OBJS = $(patsubst $(SYN68K_RUNTIME)/%.c,$(OBJ_DIR)/syn68k_rt_%.o,$(SYN68K_C_SRCS))
SYN68K_OBJS = $(SYN68K_RT_OBJS) $(OBJ_DIR)/syn68k_glue.o

$(OBJ_DIR)/%.o: include/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

$(OBJ_DIR)/memory.o: ../memory.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/cpu_glue.o: ../cpu_glue.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/cpu_engine.o: ../cpu_engine.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/emul_op.o: ../emul_op.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/toolbox_traps.o: ../toolbox_traps.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/scc.o: ../scc.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/scsi.o: ../scsi.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/scsi_dummy.o: ../dummy/scsi_dummy.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/disk.o: ../disk.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/macos_util.o: ../macos_util.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/rom_patches.o: ../rom_patches.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/musashi_glue.o: $(MUSASHI)/musashi_glue.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/memory_musashi.o: $(MUSASHI)/memory_musashi.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/amiberry_glue.o: ../amiberry/amiberry_glue.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/emu68_glue.o: ../Emu68/emu68_glue.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/syn68k_glue.o: ../syn68k/syn68k_glue.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(SYN68K_DEFS) -c $< -o $@
$(OBJ_DIR)/m68k_rs_glue.o: ../m68k_rs/m68k_rs_glue.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

$(OBJ_DIR)/m68kcpu.o: $(MUSASHI)/m68kcpu.c $(MUSASHI)/m68kops.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/m68kops.o: $(MUSASHI)/m68kops.c $(MUSASHI)/m68kops.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/m68kdasm.o: $(MUSASHI)/m68kdasm.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/softfloat.o: $(MUSASHI)/softfloat/softfloat.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

CORE_OBJS = \
	$(OBJ_DIR)/test_env.o $(OBJ_DIR)/test_stubs.o $(OBJ_DIR)/test_sys.o \
	$(OBJ_DIR)/memory.o $(OBJ_DIR)/cpu_glue.o $(OBJ_DIR)/cpu_engine.o \
	$(OBJ_DIR)/emul_op.o $(OBJ_DIR)/toolbox_traps.o $(OBJ_DIR)/scc.o $(OBJ_DIR)/scsi.o $(OBJ_DIR)/scsi_dummy.o \
	$(OBJ_DIR)/disk.o $(OBJ_DIR)/macos_util.o $(OBJ_DIR)/rom_patches.o \
	$(OBJ_DIR)/musashi_glue.o $(OBJ_DIR)/memory_musashi.o \
	$(OBJ_DIR)/amiberry_glue.o $(OBJ_DIR)/m68k_rs_glue.o \
	$(OBJ_DIR)/m68kcpu.o $(OBJ_DIR)/m68kops.o $(OBJ_DIR)/m68kdasm.o $(OBJ_DIR)/softfloat.o

LIB_OBJS = $(CORE_OBJS) $(EMU68_OBJS) $(SYN68K_OBJS)

$(OBJ_DIR)/libcockatrice_test.a: $(LIB_OBJS) $(AMIBERRY_OBJS) | $(OBJ_DIR)
	rm -f $@
	ar rcs $@ $(LIB_OBJS) $(AMIBERRY_OBJS)

$(AMIBERRY_OBJS):
	$(MAKE) -C $(OSX64) amiberry-objs

LIBS = -Wl,-force_load,$(OBJ_DIR)/libcockatrice_test.a $(M68K_RS_LIB) -lm -lpthread -ldl

$(M68K_RS_LIB): $(M68K_RS_DIR)/Cargo.toml $(M68K_RS_DIR)/src/lib.rs ../vendor/m68k-rs/Cargo.toml | $(OBJ_DIR)
	cargo build --release --manifest-path $(M68K_RS_DIR)/Cargo.toml --target-dir $(M68K_RS_TARGET)

CPU_SRCS = cpu/cpu_test_main.cpp cpu/cpu_emulop.cpp cpu/cpu_instructions.cpp \
	cpu/cpu_jsr.cpp cpu/cpu_exceptions.cpp cpu/cpu_fpu.cpp \
	cpu/cpu_opcode_battery.cpp cpu/cpu_regressions.cpp cpu/rom_snippets.cpp \
	cpu/cpu_uae_cputest.cpp cpu/cpu_syn68k_battery.cpp
CPU_OBJS = $(patsubst cpu/%.cpp,$(OBJ_DIR)/c_%.o,$(CPU_SRCS))

$(OBJ_DIR)/c_%.o: cpu/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

cpu_tests: $(CPU_OBJS) $(OBJ_DIR)/libcockatrice_test.a $(OBJ_DIR)/uae_cputest $(OBJ_DIR)/syn68k_battery $(M68K_RS_LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(CPU_OBJS) $(LIBS)
	@if [ -f $(ENTITLEMENTS) ]; then codesign -s - --force --entitlements $(ENTITLEMENTS) $@; fi

$(CPUTEST_VENDOR_STAMP):
	@chmod +x ../scripts/vendor-uae-cputest.sh
	../scripts/vendor-uae-cputest.sh
	@touch $@

vendor-cputest: $(CPUTEST_VENDOR_STAMP)

$(OBJ_DIR)/cputest_%.o: $(CPUTEST_VENDOR)/%.cpp $(CPUTEST_VENDOR_STAMP) | $(OBJ_DIR)
	$(CXX) $(CPUTEST_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/cputest_cputest_support_overrides.o: ../amiberry/cputest/hosted/cputest_support_overrides.cpp $(CPUTEST_VENDOR_STAMP) | $(OBJ_DIR)
	$(CXX) $(CPUTEST_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/cputest_readcpu.o: ../amiberry/src/readcpu.cpp $(CPUTEST_VENDOR_STAMP) | $(OBJ_DIR)
	$(CXX) $(CPUTEST_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/cputest_cpudefs.o: ../amiberry/src/cpudefs.cpp $(CPUTEST_VENDOR_STAMP) | $(OBJ_DIR)
	$(CXX) $(CPUTEST_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/cputest_newcpu_common.o: ../amiberry/src/newcpu_common.cpp $(CPUTEST_VENDOR_STAMP) | $(OBJ_DIR)
	$(CXX) $(CPUTEST_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/cputest_fpp.o: ../amiberry/src/fpp.cpp $(CPUTEST_VENDOR_STAMP) | $(OBJ_DIR)
	$(CXX) $(CPUTEST_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/cputest_fpp_softfloat.o: ../amiberry/src/fpp_softfloat.cpp $(CPUTEST_VENDOR_STAMP) | $(OBJ_DIR)
	$(CXX) $(CPUTEST_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/cputest_softfloat.o: ../amiberry/src/softfloat/softfloat.cpp $(CPUTEST_VENDOR_STAMP) | $(OBJ_DIR)
	$(CXX) $(CPUTEST_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/cputest_softfloat_fpsp.o: ../amiberry/src/softfloat/softfloat_fpsp.cpp $(CPUTEST_VENDOR_STAMP) | $(OBJ_DIR)
	$(CXX) $(CPUTEST_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/cputest_softfloat_decimal.o: ../amiberry/src/softfloat/softfloat_decimal.cpp $(CPUTEST_VENDOR_STAMP) | $(OBJ_DIR)
	$(CXX) $(CPUTEST_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/cputestgen.ini: ../amiberry/cputest/cockatrice_cputest.ini | $(OBJ_DIR)
	cp $< $@

$(OBJ_DIR)/uae_cputest: $(CPUTEST_OBJS) $(OBJ_DIR)/cputestgen.ini | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(CPUTEST_OBJS) -lz -lm
	@if [ -f $(ENTITLEMENTS) ]; then codesign -s - --force --entitlements $(ENTITLEMENTS) $@; fi

uae_cputest: $(OBJ_DIR)/uae_cputest

$(OBJ_DIR)/syn68k_batt_%.o: $(SYN68K_TEST_DIR)/%.c | $(OBJ_DIR) $(SYN68K_RUNTIME)/syn68k.c
	$(SYN68K_CC) -DMEMORY_OFFSET=8192 -I$(SYN68K_TEST_DIR)/include -c $< -o $@

$(OBJ_DIR)/syn68k_battery: $(SYN68K_BATTERY_OBJS) $(SYN68K_RT_OBJS) syn68k_battery_stubs.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SYN68K_BATTERY_OBJS) $(SYN68K_RT_OBJS) syn68k_battery_stubs.c -lm
	@if [ -f $(ENTITLEMENTS) ]; then codesign -s - --force --entitlements $(ENTITLEMENTS) $@; fi

syn68k_battery: $(OBJ_DIR)/syn68k_battery

BASILISK_TESTS = basilisk_memory_test basilisk_engine_test basilisk_emulop_test \
	basilisk_patches_test basilisk_scsi_test basilisk_scc_test basilisk_disk_test

$(OBJ_DIR)/basilisk_memory_test.o: basilisk/basilisk_memory_test.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/basilisk_engine_test.o: basilisk/basilisk_engine_test.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/basilisk_emulop_test.o: basilisk/basilisk_emulop_test.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/basilisk_patches_test.o: basilisk/basilisk_patches_test.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/basilisk_scsi_test.o: basilisk/basilisk_scsi_test.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/basilisk_scc_test.o: basilisk/basilisk_scc_test.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/basilisk_disk_test.o: basilisk/basilisk_disk_test.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

basilisk_memory_test: $(OBJ_DIR)/basilisk_memory_test.o $(OBJ_DIR)/libcockatrice_test.a
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)
	@if [ -f $(ENTITLEMENTS) ]; then codesign -s - --force --entitlements $(ENTITLEMENTS) $@; fi
basilisk_engine_test: $(OBJ_DIR)/basilisk_engine_test.o $(OBJ_DIR)/libcockatrice_test.a
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)
	@if [ -f $(ENTITLEMENTS) ]; then codesign -s - --force --entitlements $(ENTITLEMENTS) $@; fi
basilisk_emulop_test: $(OBJ_DIR)/basilisk_emulop_test.o $(OBJ_DIR)/libcockatrice_test.a
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)
	@if [ -f $(ENTITLEMENTS) ]; then codesign -s - --force --entitlements $(ENTITLEMENTS) $@; fi
basilisk_patches_test: $(OBJ_DIR)/basilisk_patches_test.o $(OBJ_DIR)/libcockatrice_test.a
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)
	@if [ -f $(ENTITLEMENTS) ]; then codesign -s - --force --entitlements $(ENTITLEMENTS) $@; fi
basilisk_scsi_test: $(OBJ_DIR)/basilisk_scsi_test.o $(OBJ_DIR)/libcockatrice_test.a
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)
	@if [ -f $(ENTITLEMENTS) ]; then codesign -s - --force --entitlements $(ENTITLEMENTS) $@; fi
basilisk_scc_test: $(OBJ_DIR)/basilisk_scc_test.o $(OBJ_DIR)/libcockatrice_test.a
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)
	@if [ -f $(ENTITLEMENTS) ]; then codesign -s - --force --entitlements $(ENTITLEMENTS) $@; fi
basilisk_disk_test: $(OBJ_DIR)/basilisk_disk_test.o $(OBJ_DIR)/libcockatrice_test.a
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)
	@if [ -f $(ENTITLEMENTS) ]; then codesign -s - --force --entitlements $(ENTITLEMENTS) $@; fi

basilisk_tests: $(BASILISK_TESTS)

test-basilisk: basilisk_tests
	./run_basilisk_tests.sh

test-cpu: cpu_tests
	./run_cpu_tests.sh

test: basilisk_tests cpu_tests
	./run_all.sh

test-strict: basilisk_tests cpu_tests
	STRICT_CPU=1 ./run_all.sh

clean:
	rm -rf $(OBJ_DIR) cpu_tests $(BASILISK_TESTS)
	rm -f $(CPUTEST_VENDOR_STAMP)
