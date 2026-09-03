# BasiliskII/tests — CPU suite + Basilisk subsystem tests
#
# Shared objects go in obj/libcockatrice_test.a so the three 680x0 engines
# (Musashi, UAE/Amiberry, m68k-rs) compile once. Run `make test` (Basilisk is
# the gate; CPU is reported). `make test-strict` fails on CPU failures too.

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

DEFS = -DHAVE_CONFIG_H -D_REENTRANT \
	-DTEST_REPO_ROOT=\"$(REPO_ROOT)\" \
	-DMUSASHI_TEST_DIR=\"$(abspath $(MUSASHI)/test)\" \
	-DUAE_CPUTEST_BIN_PATH=\"$(abspath $(OBJ_DIR)/uae_cputest)\"

INCLUDES = -Iinclude -I$(OSX64) -I../include -I$(MUSASHI) -I$(MUSASHI)/softfloat \
	-I../Unix -I../dummy -I../amiberry -I../SDL -I../m68k_rs/include -I../jit_support

CPPFLAGS = $(DEFS) $(INCLUDES)

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

.PHONY: all test test-strict test-basilisk test-cpu clean amiberry-objs cpu_tests basilisk_tests \
	vendor-cputest uae_cputest

all: cpu_tests basilisk_tests

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

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
$(OBJ_DIR)/m68k_rs_glue.o: ../m68k_rs/m68k_rs_glue.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
$(OBJ_DIR)/jit_host_darwin.o: ../jit_support/jit_host_darwin.cpp | $(OBJ_DIR)
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
	$(OBJ_DIR)/amiberry_glue.o $(OBJ_DIR)/m68k_rs_glue.o $(OBJ_DIR)/jit_host_darwin.o \
	$(OBJ_DIR)/m68kcpu.o $(OBJ_DIR)/m68kops.o $(OBJ_DIR)/m68kdasm.o $(OBJ_DIR)/softfloat.o

LIB_OBJS = $(CORE_OBJS)

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
	cpu/cpu_uae_cputest.cpp cpu/cpu_interrupt_stress.cpp
CPU_OBJS = $(patsubst cpu/%.cpp,$(OBJ_DIR)/c_%.o,$(CPU_SRCS))

$(OBJ_DIR)/c_%.o: cpu/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

cpu_tests: $(CPU_OBJS) $(OBJ_DIR)/libcockatrice_test.a $(OBJ_DIR)/uae_cputest $(M68K_RS_LIB)
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
	@mkdir -p $(OBJ_DIR)/data

uae_cputest: $(OBJ_DIR)/uae_cputest

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

basilisk_memory_test: $(OBJ_DIR)/basilisk_memory_test.o $(OBJ_DIR)/libcockatrice_test.a $(M68K_RS_LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)
	@if [ -f $(ENTITLEMENTS) ]; then codesign -s - --force --entitlements $(ENTITLEMENTS) $@; fi
basilisk_engine_test: $(OBJ_DIR)/basilisk_engine_test.o $(OBJ_DIR)/libcockatrice_test.a $(M68K_RS_LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)
	@if [ -f $(ENTITLEMENTS) ]; then codesign -s - --force --entitlements $(ENTITLEMENTS) $@; fi
basilisk_emulop_test: $(OBJ_DIR)/basilisk_emulop_test.o $(OBJ_DIR)/libcockatrice_test.a $(M68K_RS_LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)
	@if [ -f $(ENTITLEMENTS) ]; then codesign -s - --force --entitlements $(ENTITLEMENTS) $@; fi
basilisk_patches_test: $(OBJ_DIR)/basilisk_patches_test.o $(OBJ_DIR)/libcockatrice_test.a $(M68K_RS_LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)
	@if [ -f $(ENTITLEMENTS) ]; then codesign -s - --force --entitlements $(ENTITLEMENTS) $@; fi
basilisk_scsi_test: $(OBJ_DIR)/basilisk_scsi_test.o $(OBJ_DIR)/libcockatrice_test.a $(M68K_RS_LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)
	@if [ -f $(ENTITLEMENTS) ]; then codesign -s - --force --entitlements $(ENTITLEMENTS) $@; fi
basilisk_scc_test: $(OBJ_DIR)/basilisk_scc_test.o $(OBJ_DIR)/libcockatrice_test.a $(M68K_RS_LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)
	@if [ -f $(ENTITLEMENTS) ]; then codesign -s - --force --entitlements $(ENTITLEMENTS) $@; fi
basilisk_disk_test: $(OBJ_DIR)/basilisk_disk_test.o $(OBJ_DIR)/libcockatrice_test.a $(M68K_RS_LIB)
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
