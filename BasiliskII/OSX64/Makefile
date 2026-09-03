# Makefile for CockatriceIII macOS 64-bit (ARM64 & AMD64) with Musashi CPU Core

SHELL = /bin/sh

prefix = /usr/local
exec_prefix = ${prefix}
bindir = ${exec_prefix}/bin
datadir = ${prefix}/share
mandir = ${prefix}/man

CC = clang
CXX = clang++
# `make optimised` overrides this to -Os; JIT-heavy code is not cycle/size
# sensitive enough for -Os to matter, this is about smaller release binaries.
OPT_LEVEL ?= -O2
CFLAGS = $(OPT_LEVEL) -Wall -Wno-unused -Wno-deprecated-non-prototype
CXXFLAGS = $(OPT_LEVEL) -Wall -Wno-unused -Wno-writable-strings -Wno-multichar -Wno-deprecated-declarations -Wno-format -Wno-c++11-narrowing

APP = CockatriceIII

ARCH ?= $(shell uname -m)
ifeq ($(ARCH),x86_64)
  TARGET_ARCH = -arch x86_64
  SDL_PREFIX ?= /usr/local
else ifeq ($(ARCH),amd64)
  TARGET_ARCH = -arch x86_64
  SDL_PREFIX ?= /usr/local
else
  TARGET_ARCH = -arch arm64
  SDL_PREFIX ?= /opt/homebrew
endif

CFLAGS += $(TARGET_ARCH)
CXXFLAGS += $(TARGET_ARCH)
LDFLAGS += $(TARGET_ARCH)
# Guest RAM uses Host_Mem_Base + offset (memory_init mmap), not identity-mapped
# low host addresses. Default 4GB PAGEZERO keeps the Mach-O load layout valid
# on modern arm64 macOS; the old -pagezero_size,0x4000 trick caused instant
# SIGKILL before main (lldb: "Malformed Mach-o file").

# Use the SDL matching TARGET_ARCH, not whatever sdl-config is first on PATH.
SDL_CFLAGS := $(shell $(SDL_PREFIX)/bin/sdl-config --cflags 2>/dev/null || echo "-I$(SDL_PREFIX)/include/SDL -D_THREAD_SAFE")
SDL_LIBS := $(shell $(SDL_PREFIX)/bin/sdl-config --libs 2>/dev/null || echo "-L$(SDL_PREFIX)/lib -lSDL -Wl,-framework,Cocoa")

CPPFLAGS = -I../include -I. -I../Musashi -I../Musashi/softfloat -I../Unix -I../SDL -I../bridge/darwin \
	-I../m68k_rs/include \
	-I/opt/homebrew/include -I/usr/local/include $(SDL_CFLAGS)

HOST_CPPFLAGS = -I../include -I. -I../Musashi -I../Musashi/softfloat -I../Unix -I../SDL -I../bridge/darwin
HOST_CFLAGS = $(OPT_LEVEL) -Wall -Wno-unused -Wno-deprecated-non-prototype
HOST_CXXFLAGS = $(OPT_LEVEL) -Wall -Wno-unused -Wno-writable-strings -Wno-multichar -Wno-deprecated-declarations -Wno-format -Wno-c++11-narrowing
DEFS = -DHAVE_CONFIG_H -D_REENTRANT
ifeq ($(COCKATRICE_DEBUG),1)
  DEFS += -DCOCKATRICE_DEBUG=1 -DSCC_DEBUG=1 -DEMULOP_DEBUG=1 -DHEARTBEAT_DEBUG=1
endif

LIBS = ../slirp/libslirp.a -lpthread -ldl $(SDL_LIBS) -framework Cocoa -framework IOKit

M68K_RS_DIR = ../m68k_rs
M68K_RS_TARGET = $(OBJ_DIR)/m68k_rs_target
M68K_RS_LIB = $(M68K_RS_TARGET)/release/libcockatrice_m68k_rs.a

SYSSRCS = sdlmain.m ../bridge/darwin/macos_menu_bridge.mm ../SDL/sdl_pcap.cpp ../SDL/audio_sdl.cpp ../dummy/scsi_dummy.cpp ../dummy/prefs_editor_dummy.cpp

CPUSRCS = ../cpu_engine.cpp ../cpu_glue.cpp ../memory.cpp ../Musashi/musashi_glue.cpp ../Musashi/memory_musashi.cpp ../amiberry/amiberry_glue.cpp ../m68k_rs/m68k_rs_glue.cpp ../Musashi/m68kcpu.c ../Musashi/m68kops.c ../Musashi/m68kdasm.c ../Musashi/softfloat/softfloat.c

# Amiberry 680x0 CPU + JIT (ARM64 and x86-64 backends selected by the compiler).
AMIBERRY_DIR = ../amiberry
AMIBERRY_CPPFLAGS = -I$(AMIBERRY_DIR)/hosted -I$(AMIBERRY_DIR) -I$(AMIBERRY_DIR)/src/include \
	-I$(AMIBERRY_DIR)/src -I$(AMIBERRY_DIR)/src/jit -I$(AMIBERRY_DIR)/src/threaddep \
	-I$(AMIBERRY_DIR)/src/softfloat -I. \
	-DHAVE_CONFIG_H -DJIT -DUSE_JIT_FPU -DFPUEMU -DWITH_SOFTFLOAT
AMIBERRY_CXXFLAGS = $(CXXFLAGS) -std=c++17 -Wno-unused -Wno-deprecated-declarations \
	-Wno-format -Wno-c++11-narrowing -fno-strict-aliasing -ferror-limit=0

AMIBERRY_OBJS = \
	$(OBJ_DIR)/amiberry_newcpu.o \
	$(OBJ_DIR)/amiberry_newcpu_common.o \
	$(OBJ_DIR)/amiberry_readcpu.o \
	$(OBJ_DIR)/amiberry_cpudefs.o \
	$(OBJ_DIR)/amiberry_cpustbl.o \
	$(OBJ_DIR)/amiberry_fpp.o \
	$(OBJ_DIR)/amiberry_fpp_softfloat.o \
	$(OBJ_DIR)/amiberry_fpp_native.o \
	$(OBJ_DIR)/amiberry_cpuemu_0.o \
	$(OBJ_DIR)/amiberry_cpuemu_11.o \
	$(OBJ_DIR)/amiberry_cpuemu_13.o \
	$(OBJ_DIR)/amiberry_cpuemu_20.o \
	$(OBJ_DIR)/amiberry_cpuemu_21.o \
	$(OBJ_DIR)/amiberry_cpuemu_22.o \
	$(OBJ_DIR)/amiberry_cpuemu_23.o \
	$(OBJ_DIR)/amiberry_cpuemu_24.o \
	$(OBJ_DIR)/amiberry_cpuemu_31.o \
	$(OBJ_DIR)/amiberry_cpuemu_32.o \
	$(OBJ_DIR)/amiberry_cpuemu_33.o \
	$(OBJ_DIR)/amiberry_cpuemu_34.o \
	$(OBJ_DIR)/amiberry_cpuemu_35.o \
	$(OBJ_DIR)/amiberry_cpuemu_40.o \
	$(OBJ_DIR)/amiberry_cpuemu_50.o \
	$(OBJ_DIR)/amiberry_cpummu.o \
	$(OBJ_DIR)/amiberry_cpummu30.o \
	$(OBJ_DIR)/amiberry_vm.o \
	$(OBJ_DIR)/amiberry_jit_compemu.o \
	$(OBJ_DIR)/amiberry_jit_compstbl.o \
	$(OBJ_DIR)/amiberry_jit_compemu_fpp.o \
	$(OBJ_DIR)/amiberry_jit_compemu_support.o \
	$(OBJ_DIR)/amiberry_sf_softfloat.o \
	$(OBJ_DIR)/amiberry_sf_softfloat_decimal.o \
	$(OBJ_DIR)/amiberry_sf_softfloat_fpsp.o \
	$(OBJ_DIR)/amiberry_hosted_host.o \
	$(OBJ_DIR)/amiberry_hosted_memory.o

## Files
SRCS = ../main.cpp ../SDL/main_sdl.cpp ../prefs.cpp ../dummy/prefs_dummy.cpp ../SDL/sys_unix_sdl.cpp \
    ../rom_patches.cpp ../slot_rom.cpp ../rsrc_patches.cpp ../emul_op.cpp ../toolbox_traps.cpp \
    ../macos_util.cpp ../xpram.cpp ../Unix/xpram_unix.cpp ../timer.cpp timer_unix.cpp \
    ../dummy/clip_dummy.cpp ../adb.cpp ../serial.cpp ../dummy/serial_dummy.cpp ../scc.cpp ../ether.cpp ../sony.cpp \
    ../disk.cpp ../cdrom.cpp ../scsi.cpp ../video.cpp ../SDL/video_sdl.cpp ../audio.cpp \
    ../extfs.cpp ../user_strings.cpp user_strings_sdl.cpp ../SDL/menu_bar.cpp \
    $(SYSSRCS) $(CPUSRCS)

## Rules
.PHONY: all clean mostlyclean clean-objects slirp_lib universal amiberry-objs debug optimised optimized
.SUFFIXES:
.SUFFIXES: .c .cpp .m .mm .s .o .h

all: $(APP)

# SCC, EmulOp, heartbeat, and D(bug(...)) from debug.h.
# clean-objects so TUs that already compiled with DEBUG 0 pick up the flags.
debug:
	$(MAKE) clean-objects
	$(MAKE) COCKATRICE_DEBUG=1 all

# Release build optimised for size (-Os) rather than the default -O2.
# clean-objects so TUs built at another OPT_LEVEL get rebuilt.
optimized: optimised
optimised:
	$(MAKE) clean-objects
	$(MAKE) OPT_LEVEL=-Os all

universal:
	$(MAKE) clean-objects
	$(MAKE) ARCH=arm64
	cp $(APP) $(APP)-arm64
	$(MAKE) clean-objects
	$(MAKE) ARCH=amd64
	cp $(APP) $(APP)-x86_64
	lipo -create $(APP)-arm64 $(APP)-x86_64 -output $(APP)
	@if [ -f entitlements.plist ]; then \
		echo "Codesigning $(APP) with entitlements.plist..."; \
		codesign -s - --force --entitlements entitlements.plist $(APP); \
	fi

OBJ_DIR = obj
$(OBJ_DIR)::
	@[ -d $(OBJ_DIR) ] || mkdir $(OBJ_DIR) > /dev/null 2>&1

define SRCS_LIST_TO_OBJS
	$(addprefix $(OBJ_DIR)/, $(addsuffix .o, $(foreach file, $(SRCS), \
	$(basename $(notdir $(file))))))
endef
OBJS = $(SRCS_LIST_TO_OBJS)

SRC_PATHS += $(sort $(foreach file, $(SRCS), $(dir $(file))))
VPATH :=
VPATH += $(addprefix :, $(subst  ,:, $(filter-out $($(subst, :, ,$(VPATH))), $(SRC_PATHS))))

amiberry-objs: $(OBJ_DIR) $(AMIBERRY_OBJS)

$(APP): $(OBJ_DIR) ../slirp/libslirp.a $(OBJS) $(AMIBERRY_OBJS) $(M68K_RS_LIB)
	$(CXX) -o $(APP) $(LDFLAGS) $(OBJS) $(AMIBERRY_OBJS) $(M68K_RS_LIB) $(LIBS)
	@if [ -f entitlements.plist ]; then \
		echo "Codesigning $(APP) with entitlements.plist..."; \
		codesign -s - --force --entitlements entitlements.plist $(APP); \
	fi

../slirp/libslirp.a:
	$(MAKE) -C ../slirp CC="$(CC)" CFLAGS="-I. -O2 -Wall -Wno-unused $(TARGET_ARCH)"

clean-objects:
	rm -rf $(APP) $(OBJ_DIR) core* *.core *~ *.bak
	mkdir -p $(OBJ_DIR)
	rm -rf $(M68K_RS_TARGET)
	$(MAKE) -C ../slirp clean

$(M68K_RS_LIB): $(M68K_RS_DIR)/Cargo.toml $(M68K_RS_DIR)/src/lib.rs ../vendor/m68k-rs/Cargo.toml ../vendor/m68k-rs/src/core/op_cache.rs
	cargo build --release --manifest-path $(M68K_RS_DIR)/Cargo.toml --target-dir $(M68K_RS_TARGET)

mostlyclean: clean-objects
	rm -f $(APP)-arm64 $(APP)-x86_64

clean: mostlyclean
	rm -f cpuemu.cpp cpudefs.cpp cpustbl.cpp cputbl.h

$(OBJ_DIR)/%.o : %.c
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o : %.m
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o : %.mm
	$(CXX) $(CPPFLAGS) $(DEFS) $(CXXFLAGS) -std=c++17 -c $< -o $@

$(OBJ_DIR)/%.o : %.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o : %.s
	$(CC) -c $< -o $@

$(OBJ_DIR)/m68kmake: ../Musashi/m68kmake.c
	$(CC) $(HOST_CPPFLAGS) $(HOST_CFLAGS) -o $@ $<

../Musashi/m68kops.c ../Musashi/m68kops.h: $(OBJ_DIR)/m68kmake
	cd ../Musashi && ../OSX64/$(OBJ_DIR)/m68kmake

$(OBJ_DIR)/m68kcpu.o: ../Musashi/m68kcpu.c ../Musashi/m68kops.h ../Musashi/m68kcpu.h ../Musashi/m68kfpu.c ../Musashi/m68kmmu.h
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/m68kops.o: ../Musashi/m68kops.c ../Musashi/m68kops.h
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/softfloat.o: ../Musashi/softfloat/softfloat.c
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/amiberry_%.o: $(AMIBERRY_DIR)/src/%.cpp
	$(CXX) $(AMIBERRY_CPPFLAGS) $(DEFS) $(AMIBERRY_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/amiberry_jit_%.o: $(AMIBERRY_DIR)/src/jit/%.cpp
	$(CXX) $(AMIBERRY_CPPFLAGS) $(DEFS) $(AMIBERRY_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/amiberry_sf_%.o: $(AMIBERRY_DIR)/src/softfloat/%.cpp
	$(CXX) $(AMIBERRY_CPPFLAGS) $(DEFS) $(AMIBERRY_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/amiberry_hosted_host.o: $(AMIBERRY_DIR)/hosted/amiberry_host.cpp
	$(CXX) $(AMIBERRY_CPPFLAGS) $(DEFS) $(AMIBERRY_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/amiberry_hosted_memory.o: $(AMIBERRY_DIR)/hosted/amiberry_memory.cpp
	$(CXX) $(AMIBERRY_CPPFLAGS) $(DEFS) $(AMIBERRY_CXXFLAGS) -c $< -o $@

