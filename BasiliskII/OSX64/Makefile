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
APP_BUNDLE = $(APP).app
# Overridden by CI to the release tag (e.g. 0.6.0); keep it PEP440/semver-ish
# since it lands in both CFBundleVersion and CFBundleShortVersionString.
APP_VERSION ?= 0.0.0-dev

ARCH ?= $(shell uname -m)
HOST_ARCH := $(shell uname -m)
# Committed x86_64 SDL 1.2 prefix (see dist/dependencies/osx/README.md).
CROSS_SDL_X86 = $(CURDIR)/../../dist/dependencies/osx/intel
# CARGO_TRIPLE matches TARGET_ARCH so m68k-rs's staticlib links into a
# cross-compiled slice. cargo without --target always builds the host
# triple (aarch64 on Apple Silicon CI), which then fails at link.
# Intel slices use the committed prefix, not Intel Homebrew / Rosetta.
ifeq ($(ARCH),x86_64)
  TARGET_ARCH = -arch x86_64
  CARGO_TRIPLE = x86_64-apple-darwin
  ifneq ($(wildcard $(CROSS_SDL_X86)/bin/sdl-config),)
    SDL_PREFIX ?= $(CROSS_SDL_X86)
  else ifeq ($(HOST_ARCH),x86_64)
    SDL_PREFIX ?= /usr/local
  else
    SDL_PREFIX ?= $(CROSS_SDL_X86)
  endif
else ifeq ($(ARCH),amd64)
  TARGET_ARCH = -arch x86_64
  CARGO_TRIPLE = x86_64-apple-darwin
  ifneq ($(wildcard $(CROSS_SDL_X86)/bin/sdl-config),)
    SDL_PREFIX ?= $(CROSS_SDL_X86)
  else ifeq ($(HOST_ARCH),x86_64)
    SDL_PREFIX ?= /usr/local
  else
    SDL_PREFIX ?= $(CROSS_SDL_X86)
  endif
else
  TARGET_ARCH = -arch arm64
  SDL_PREFIX ?= /opt/homebrew
  CARGO_TRIPLE = aarch64-apple-darwin
endif

CFLAGS += $(TARGET_ARCH)
CXXFLAGS += $(TARGET_ARCH)
LDFLAGS += $(TARGET_ARCH)
# Committed Intel dylibs use @rpath; point the linker at this prefix.
ifeq ($(SDL_PREFIX),$(CROSS_SDL_X86))
LDFLAGS += -Wl,-rpath,$(SDL_PREFIX)/lib
endif
# Guest RAM uses Host_Mem_Base + offset (memory_init mmap), not identity-mapped
# low host addresses. Default 4GB PAGEZERO keeps the Mach-O load layout valid
# on modern arm64 macOS; the old -pagezero_size,0x4000 trick caused instant
# SIGKILL before main (lldb: "Malformed Mach-o file").

# Use the SDL matching TARGET_ARCH, not whatever sdl-config is first on PATH.
# Sources include <SDL/SDL.h>, so -I<prefix>/include is required. sdl-config
# --cflags also adds -I<prefix>/include/SDL for the <SDL.h> form.
SDL_CFLAGS := $(shell $(SDL_PREFIX)/bin/sdl-config --cflags 2>/dev/null || echo "-I$(SDL_PREFIX)/include -I$(SDL_PREFIX)/include/SDL -D_THREAD_SAFE")
SDL_LIBS := $(shell $(SDL_PREFIX)/bin/sdl-config --libs 2>/dev/null || echo "-L$(SDL_PREFIX)/lib -lSDL -Wl,-framework,Cocoa")

CPPFLAGS = -I../include -I. -I../Musashi -I../Musashi/softfloat -I../Unix -I../SDL -I../bridge/darwin \
	-I../m68k_rs/include -I../jit_support \
	-I$(SDL_PREFIX)/include -I/opt/homebrew/include -I/usr/local/include $(SDL_CFLAGS)

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
M68K_RS_LIB = $(M68K_RS_TARGET)/$(CARGO_TRIPLE)/release/libcockatrice_m68k_rs.a

SYSSRCS = sdlmain.m ../bridge/darwin/macos_menu_bridge.mm ../bridge/darwin/macos_window_bridge.mm ../SDL/sdl_pcap.cpp ../SDL/audio_sdl.cpp ../dummy/scsi_dummy.cpp ../dummy/prefs_editor_dummy.cpp

CPUSRCS = ../cpu_engine.cpp ../cpu_glue.cpp ../memory.cpp ../Musashi/musashi_glue.cpp ../Musashi/memory_musashi.cpp ../amiberry/amiberry_glue.cpp ../m68k_rs/m68k_rs_glue.cpp ../jit_support/jit_host_darwin.cpp ../Musashi/m68kcpu.c ../Musashi/m68kops.c ../Musashi/m68kdasm.c ../Musashi/softfloat/softfloat.c

# Amiberry 680x0 CPU + JIT (ARM64 and x86-64 backends selected by the compiler).
AMIBERRY_DIR = ../amiberry
AMIBERRY_CPPFLAGS = -I$(AMIBERRY_DIR)/hosted -I$(AMIBERRY_DIR) -I$(AMIBERRY_DIR)/src/include \
	-I$(AMIBERRY_DIR)/src -I$(AMIBERRY_DIR)/src/jit -I$(AMIBERRY_DIR)/src/threaddep \
	-I$(AMIBERRY_DIR)/src/softfloat -I../jit_support -I. \
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
    ../rom_patches.cpp ../slot_rom.cpp ../rsrc_patches.cpp ../emul_op.cpp ../toolbox_traps.cpp ../toolbox_menu.cpp ../toolbox_window.cpp \
    ../macos_util.cpp ../xpram.cpp ../Unix/xpram_unix.cpp ../timer.cpp timer_unix.cpp \
    ../dummy/clip_dummy.cpp ../adb.cpp ../serial.cpp ../dummy/serial_dummy.cpp ../scc.cpp ../ether.cpp ../sony.cpp \
    ../disk.cpp ../cdrom.cpp ../scsi.cpp ../video.cpp ../SDL/video_sdl.cpp ../audio.cpp \
    ../extfs.cpp ../user_strings.cpp user_strings_sdl.cpp ../SDL/menu_bar.cpp \
    $(SYSSRCS) $(CPUSRCS)

## Rules
.PHONY: all clean mostlyclean clean-objects slirp_lib universal amiberry-objs debug optimised optimized app app-universal bundle sdl-x86_64
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

# .app bundle wrapping the plain $(APP) binary already built for the current
# ARCH. `app-universal` wraps the lipo'd fat binary from `universal` instead
# of rebuilding it a second time.
app: $(APP)
	$(MAKE) bundle

app-universal: universal
	$(MAKE) bundle

# Prefs/xpram/archive are not part of this bundle: the app resolves them
# relative to its own working directory (chdir'd to the bundle root on a
# Finder/no-arg launch, see setupWorkingDirectory in sdlmain.m), so packaging
# copies those in alongside Contents afterwards. The ROM does ship inside the
# bundle (Contents/Resources) since main_sdl.cpp falls back to looking for it
# there when it isn't found relative to cwd.
DIST_DIR = ../../dist
ROM_FILE = Quadra800.rom
ICON_FILE = CockatriceIII.icns

bundle:
	@echo "Creating $(APP_BUNDLE) (version $(APP_VERSION))..."
	rm -rf $(APP_BUNDLE)
	mkdir -p $(APP_BUNDLE)/Contents/MacOS $(APP_BUNDLE)/Contents/Resources
	cp $(APP) $(APP_BUNDLE)/Contents/MacOS/$(APP)
	sed 's/@BUNDLE_VERSION@/$(APP_VERSION)/g' Info.plist.in > $(APP_BUNDLE)/Contents/Info.plist
	@[ -f $(DIST_DIR)/$(ROM_FILE) ] && cp $(DIST_DIR)/$(ROM_FILE) $(APP_BUNDLE)/Contents/Resources/ || true
	@[ -f $(ICON_FILE) ] && cp $(ICON_FILE) $(APP_BUNDLE)/Contents/Resources/ || true
	@if [ -f entitlements.plist ]; then \
		echo "Codesigning $(APP_BUNDLE) with entitlements.plist..."; \
		codesign -s - --force --deep --entitlements entitlements.plist $(APP_BUNDLE); \
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

# Intel / universal slices need the committed prefix. Rebuild it with
# `make sdl-x86_64` (or dist/dependencies/osx/rebuild-intel-sdl.sh) and
# commit dist/dependencies/osx/intel/; CI must not compile SDL from source.
ifneq ($(HOST_ARCH),x86_64)
ifeq ($(filter $(ARCH),amd64 x86_64),$(ARCH))
$(APP): $(CROSS_SDL_X86)/bin/sdl-config
endif
endif

sdl-x86_64:
	../../dist/dependencies/osx/rebuild-intel-sdl.sh

$(CROSS_SDL_X86)/bin/sdl-config:
	@echo "Missing committed Intel SDL prefix at $(CROSS_SDL_X86)"
	@echo "On Apple Silicon run: dist/dependencies/osx/rebuild-intel-sdl.sh"
	@echo "then git add dist/dependencies/osx/intel"
	@exit 1

$(APP): $(OBJ_DIR) slirp_lib $(OBJS) $(AMIBERRY_OBJS) $(M68K_RS_LIB)
	$(CXX) -o $(APP) $(LDFLAGS) $(OBJS) $(AMIBERRY_OBJS) $(M68K_RS_LIB) $(LIBS)
	@if [ -f entitlements.plist ]; then \
		echo "Codesigning $(APP) with entitlements.plist..."; \
		codesign -s - --force --entitlements entitlements.plist $(APP); \
	fi

# ../slirp is shared with the Windows cross build in BasiliskII/mingw, which
# leaves a PE archive at this exact path.  A plain file rule would treat that
# foreign archive as up to date and the link would fail with "archive member
# not a mach-o file", so probe it with lipo and rebuild from clean when it is
# not a Mach-O archive for this host.
slirp_lib:
	@if [ -f ../slirp/libslirp.a ] && ! lipo -info ../slirp/libslirp.a >/dev/null 2>&1; then \
		echo "slirp: libslirp.a is not a Mach-O archive (left by the Windows cross build); rebuilding"; \
		$(MAKE) -C ../slirp clean; \
	fi
	$(MAKE) -C ../slirp CC="$(CC)" CFLAGS="-I. -O2 -Wall -Wno-unused $(TARGET_ARCH)"

clean-objects:
	rm -rf $(APP) $(OBJ_DIR) core* *.core *~ *.bak
	mkdir -p $(OBJ_DIR)
	rm -rf $(M68K_RS_TARGET)
	$(MAKE) -C ../slirp clean

# rustup target add is a no-op when the triple is already installed, and is
# required when ARCH is x86_64 on an Apple Silicon host or CI runner. --target
# puts the archive under $(CARGO_TRIPLE)/ so a universal build can keep both
# slices in the same target dir.
$(M68K_RS_LIB): $(M68K_RS_DIR)/Cargo.toml $(M68K_RS_DIR)/src/lib.rs ../vendor/m68k-rs/Cargo.toml ../vendor/m68k-rs/src/core/op_cache.rs
	rustup target add $(CARGO_TRIPLE)
	cargo build --release --target $(CARGO_TRIPLE) --manifest-path $(M68K_RS_DIR)/Cargo.toml --target-dir $(M68K_RS_TARGET)

mostlyclean: clean-objects
	rm -f $(APP)-arm64 $(APP)-x86_64
	rm -rf $(APP_BUNDLE)

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

