# Makefile for CockatriceIII Windows MinGW (x86_64, i686, aarch64) with Musashi CPU Core

SHELL = /bin/sh

prefix = /usr/local
exec_prefix = ${prefix}
bindir = ${exec_prefix}/bin
datadir = ${prefix}/share
mandir = ${prefix}/man

CC ?= gcc
CXX ?= g++
WINDRES ?= windres
AR ?= ar
RANLIB ?= ranlib

CC_FOR_BUILD ?= $(CC)
CXX_FOR_BUILD ?= $(CXX)

MUSASHIDIR = ../Musashi
AMIBERRY_DIR = ../amiberry
M68K_RS_DIR = ../m68k_rs

# ARCH selects the target triple: x64 (default), x86, or arm64 -- matches the
# MAKE_ARCH values cross-from-macos.sh passes for CROSS=1 builds.
ARCH ?= x64

# i686-pc-windows-gnu is an unmaintained Rust target upstream (MSYS2 dropped
# its mingw32 rust package entirely -- msys2/MINGW-packages#23346) and this
# project's own i686 cross toolchain note (see cross-from-macos.sh) predates
# that; a Homebrew i686 mingw-w64 build is SJLJ-exception-based and cannot
# link Rust's DWARF2-based i686-pc-windows-gnu std at all. So x86 has no
# m68k-rs; the Amiberry/UAE interpreter+JIT engine builds fine, though (its
# x86 JIT backend, amiberry/src/jit/x86/, already handles CPU_i386 alongside
# CPU_x86_64 throughout -- only the top-level dispatcher files needed an
# #ifdef widened to route i686 there instead of compiling to nothing).
ifeq ($(ARCH),x86)
  CARGO_TRIPLE = i686-pc-windows-gnu
  CARGO_LINKER_VAR = CARGO_TARGET_I686_PC_WINDOWS_GNU_LINKER
  ENABLE_AMIBERRY_CPU ?= 1
  ENABLE_M68K_RS_CPU ?= 0
else ifeq ($(ARCH),arm64)
  CARGO_TRIPLE = aarch64-pc-windows-gnullvm
  CARGO_LINKER_VAR = CARGO_TARGET_AARCH64_PC_WINDOWS_GNULLVM_LINKER
  ENABLE_AMIBERRY_CPU ?= 1
  ENABLE_M68K_RS_CPU ?= 1
else
  CARGO_TRIPLE = x86_64-pc-windows-gnu
  CARGO_LINKER_VAR = CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER
  ENABLE_AMIBERRY_CPU ?= 1
  ENABLE_M68K_RS_CPU ?= 1
endif

# Cross-compilation settings
ifeq ($(CROSS),1)
  CC = $(TARGET)-gcc
  CXX = $(TARGET)-g++
  WINDRES = $(TARGET)-windres
  AR = $(TARGET)-ar
  RANLIB = $(TARGET)-ranlib
  CC_FOR_BUILD = cc
  CXX_FOR_BUILD = c++

  # Prefer local per-target SDL prefix over any system SDL
  SDL_PREFIX ?= $(CURDIR)/.cross-prefix
  SDL_CFLAGS = -ISDL -Ipcap -I$(SDL_PREFIX)/include/SDL -I$(SDL_PREFIX)/include
  SDL_LIBS = -L$(SDL_PREFIX)/lib -L$(SDL_PREFIX)/bin -lmingw32 -lSDLmain -lSDL -mwindows
  HOST_CPPFLAGS = -I../OSX64 -I../include -I$(MUSASHIDIR) -I$(MUSASHIDIR)/softfloat
  HOST_CFLAGS = -O2 -Wall -Wno-unused
  HOST_CXXFLAGS = -O2 -Wall -Wno-unused
else
  SDL_CFLAGS := $(shell sdl-config --cflags 2>/dev/null || echo "-ISDL -Ipcap")
  SDL_LIBS := $(shell sdl-config --libs 2>/dev/null || echo "-Llibs -lSDLmain -lSDL")
  HOST_CPPFLAGS = $(CPPFLAGS)
  HOST_CFLAGS = $(CFLAGS)
  HOST_CXXFLAGS = $(CXXFLAGS)
endif

CFLAGS ?= -O2 -Wall -Wno-unused
CXXFLAGS ?= -O2 -Wall -Wno-unused -Wno-deprecated-declarations -Wno-c++11-narrowing
CPPFLAGS = -I../include -I. -I$(MUSASHIDIR) -I$(MUSASHIDIR)/softfloat -I../m68k_rs/include -I../jit_support \
	-I../SDL -ISDL -Ipcap $(SDL_CFLAGS)
DEFS = -DHAVE_CONFIG_H -DENABLE_MON=0 -D_REENTRANT -D_FILE_OFFSET_BITS=64 \
	-DENABLE_AMIBERRY_CPU=$(ENABLE_AMIBERRY_CPU) -DENABLE_M68K_RS_CPU=$(ENABLE_M68K_RS_CPU)

LDFLAGS =
LIBS = ../slirp/libslirp.a $(SDL_LIBS) -lws2_32 -liphlpapi -lcomdlg32 -luser32 \
	-static-libgcc -static-libstdc++ -l:libwinpthread.a \
	-lntdll -luserenv -lbcrypt

# Conditionally link DrMinGW for 32-bit x86 if available
ifeq ($(ARCH),x86)
  ifneq ($(wildcard ../drmingw-0.7.7-win32/lib/libexchndl.a),)
    LIBS += ../drmingw-0.7.7-win32/lib/libexchndl.a
  endif
endif

# Amiberry 680x0 CPU + JIT (x86-64/ARM64 backend selected by the compiler,
# see the ENABLE_AMIBERRY_CPU note above). Mirrors OSX64/Makefile.
AMIBERRY_CPPFLAGS = -I$(AMIBERRY_DIR)/hosted -I$(AMIBERRY_DIR) -I$(AMIBERRY_DIR)/src/include \
	-I$(AMIBERRY_DIR)/src -I$(AMIBERRY_DIR)/src/jit -I$(AMIBERRY_DIR)/src/threaddep \
	-I$(AMIBERRY_DIR)/src/softfloat -I../jit_support -I. \
	-DHAVE_CONFIG_H -DJIT -DUSE_JIT_FPU -DFPUEMU -DWITH_SOFTFLOAT
AMIBERRY_CXXFLAGS = $(CXXFLAGS) -std=c++17 -Wno-unused -Wno-deprecated-declarations \
	-Wno-format -Wno-c++11-narrowing -fno-strict-aliasing

ifeq ($(ENABLE_AMIBERRY_CPU),1)
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
AMIBERRY_GLUE_SRCS = ../amiberry/amiberry_glue.cpp
JIT_HOST_SRCS = ../jit_support/jit_host_win32.cpp
else
AMIBERRY_OBJS =
AMIBERRY_GLUE_SRCS =
JIT_HOST_SRCS =
endif

# m68k-rs Rust staticlib -- cross-built for CARGO_TRIPLE alongside the C++
# sources, on the arches where ENABLE_M68K_RS_CPU is on (see above).
M68K_RS_TARGET = $(OBJ_DIR)/m68k_rs_target
ifeq ($(ENABLE_M68K_RS_CPU),1)
M68K_RS_LIB = $(M68K_RS_TARGET)/$(CARGO_TRIPLE)/release/libcockatrice_m68k_rs.a
M68K_RS_GLUE_SRCS = ../m68k_rs/m68k_rs_glue.cpp
else
M68K_RS_LIB =
M68K_RS_GLUE_SRCS =
endif

SYSSRCS = ../SDL/sdl_pcap.cpp ../dummy/scsi_dummy.cpp ../SDL/audio_sdl.cpp ../dummy/prefs_editor_dummy.cpp
# Musashi is always present. Amiberry (interpreter + compemu JIT) and m68k-rs
# are cross-built for x86_64/arm64 targets only (see ENABLE_*_CPU above; i686
# keeps Musashi only). cpu_engine.cpp reads the same ENABLE_*_CPU switches
# from DEFS to know which engines it can register.
CPUSRCS = ../cpu_engine.cpp ../cpu_glue.cpp ../memory.cpp $(MUSASHIDIR)/musashi_glue.cpp $(MUSASHIDIR)/memory_musashi.cpp \
	$(AMIBERRY_GLUE_SRCS) $(JIT_HOST_SRCS) $(M68K_RS_GLUE_SRCS) \
	$(MUSASHIDIR)/m68kcpu.c $(MUSASHIDIR)/m68kops.c $(MUSASHIDIR)/m68kdasm.c $(MUSASHIDIR)/softfloat/softfloat.c

## Files
SRCS = ../main.cpp ../SDL/main_sdl.cpp ../prefs.cpp ../dummy/prefs_dummy.cpp ../SDL/sys_unix_sdl.cpp \
    ../rom_patches.cpp ../slot_rom.cpp ../rsrc_patches.cpp ../emul_op.cpp ../toolbox_traps.cpp ../toolbox_menu.cpp ../toolbox_window.cpp \
    ../macos_util.cpp ../xpram.cpp ../dummy/xpram_dummy.cpp ../timer.cpp ../SDL/timer_common.cpp \
    ../dummy/clip_dummy.cpp ../adb.cpp ../serial.cpp ../dummy/serial_dummy.cpp ../scc.cpp ../ether.cpp ../sony.cpp \
    ../disk.cpp ../cdrom.cpp ../scsi.cpp ../video.cpp ../SDL/video_sdl.cpp ../audio.cpp \
    ../extfs.cpp ../user_strings.cpp user_strings_unix.cpp ../SDL/menu_bar.cpp ../windows/menu_bar_win32.cpp \
    $(SYSSRCS) $(CPUSRCS)

APP = CockatriceIII.exe

## Rules
.PHONY: all clean mostlyclean slirp_lib
.SUFFIXES:
.SUFFIXES: .c .cpp .s .o .h

all: $(APP)

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

$(APP): $(OBJ_DIR) slirp_lib cockatricerc.o $(OBJS) $(AMIBERRY_OBJS) $(M68K_RS_LIB)
	$(CXX) -o $(APP) $(LDFLAGS) $(OBJS) $(AMIBERRY_OBJS) cockatricerc.o $(M68K_RS_LIB) $(LIBS)

slirp_lib:
ifeq ($(CROSS),1)
	$(MAKE) -C ../slirp clean
endif
	$(MAKE) -C ../slirp CC="$(CC)" AR="$(AR)" RANLIB="$(RANLIB)" CFLAGS="-I. -O2 -Wall -Wno-unused"

ifeq ($(ENABLE_M68K_RS_CPU),1)
# rustup target add is a no-op when the triple is already installed, and is
# skipped entirely when rustup itself isn't on PATH -- MSYS2's native
# mingw-w64-*-rust packages (used in CI) ship a distro-built cargo/rustc
# whose only target already is this environment's own host triple, with no
# rustup involved at all. The linker env var only matters if a dependency's
# build script needs to link a host probe; the staticlib itself needs no
# final link step.
$(M68K_RS_LIB): $(M68K_RS_DIR)/Cargo.toml $(M68K_RS_DIR)/src/lib.rs ../vendor/m68k-rs/Cargo.toml ../vendor/m68k-rs/src/core/op_cache.rs
	command -v rustup >/dev/null 2>&1 && rustup target add $(CARGO_TRIPLE) || true
	$(CARGO_LINKER_VAR)="$(CC)" cargo build --release --target $(CARGO_TRIPLE) --manifest-path $(M68K_RS_DIR)/Cargo.toml --target-dir $(M68K_RS_TARGET)
endif

mostlyclean:
	rm -f $(APP) $(OBJ_DIR)/*.o $(OBJ_DIR)/*.exe $(OBJ_DIR)/m68kmake core* *.core *~ *.bak cockatricerc.o
	rm -rf $(M68K_RS_TARGET)

clean: mostlyclean
	rm -f $(MUSASHIDIR)/m68kops.c $(MUSASHIDIR)/m68kops.h
	$(MAKE) -C ../slirp clean

cockatricerc.o: cockatrice.rc
	$(WINDRES) cockatrice.rc -O coff -o cockatricerc.o

$(OBJ_DIR)/%.o : %.c
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o : %.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/menu_bar_win32.o: ../windows/menu_bar_win32.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o : %.s
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/m68kmake: $(MUSASHIDIR)/m68kmake.c
	$(CC_FOR_BUILD) $(HOST_CPPFLAGS) $(HOST_CFLAGS) -o $@ $<

$(MUSASHIDIR)/m68kops.c $(MUSASHIDIR)/m68kops.h: $(OBJ_DIR)/m68kmake
	cd $(MUSASHIDIR) && ../mingw/$(OBJ_DIR)/m68kmake

$(OBJ_DIR)/m68kcpu.o: $(MUSASHIDIR)/m68kcpu.c $(MUSASHIDIR)/m68kops.h $(MUSASHIDIR)/m68kcpu.h $(MUSASHIDIR)/m68kfpu.c $(MUSASHIDIR)/m68kmmu.h
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/m68kops.o: $(MUSASHIDIR)/m68kops.c $(MUSASHIDIR)/m68kops.h
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/softfloat.o: $(MUSASHIDIR)/softfloat/softfloat.c
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
