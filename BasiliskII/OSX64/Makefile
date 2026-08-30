# Makefile for CockatriceIII macOS 64-bit (ARM64 & AMD64)

SHELL = /bin/sh

prefix = /usr/local
exec_prefix = ${prefix}
bindir = ${exec_prefix}/bin
datadir = ${prefix}/share
mandir = ${prefix}/man

CC = clang
CXX = clang++
CFLAGS = -O2 -Wall -Wno-unused -Wno-deprecated-non-prototype
CXXFLAGS = -O2 -Wall -Wno-unused -Wno-writable-strings -Wno-multichar -Wno-deprecated-declarations -Wno-format -Wno-c++11-narrowing

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

# Use the SDL matching TARGET_ARCH, not whatever sdl-config is first on PATH.
# Native Homebrew on Apple Silicon would otherwise poison an x86_64 link.
SDL_CFLAGS := $(shell $(SDL_PREFIX)/bin/sdl-config --cflags 2>/dev/null || echo "-I$(SDL_PREFIX)/include/SDL -D_THREAD_SAFE")
SDL_LIBS := $(shell $(SDL_PREFIX)/bin/sdl-config --libs 2>/dev/null || echo "-L$(SDL_PREFIX)/lib -lSDL -Wl,-framework,Cocoa")

CPPFLAGS = -I../include -I. -I../uae_cpu -I../Unix -I../SDL -I/opt/homebrew/include -I/usr/local/include $(SDL_CFLAGS)

# Code generators must run on the build machine. Compile them without
# TARGET_ARCH so ARCH=x86_64/amd64 works on Apple Silicon without Rosetta.
HOST_CPPFLAGS = -I../include -I. -I../uae_cpu -I../Unix -I../SDL
HOST_CFLAGS = -O2 -Wall -Wno-unused -Wno-deprecated-non-prototype
HOST_CXXFLAGS = -O2 -Wall -Wno-unused -Wno-writable-strings -Wno-multichar -Wno-deprecated-declarations -Wno-format -Wno-c++11-narrowing
DEFS = -DHAVE_CONFIG_H -D_REENTRANT -DFPU_IEEE

LIBS = ../slirp/libslirp.a -lpthread $(SDL_LIBS) -framework Cocoa -framework IOKit

SYSSRCS = sdlmain.m ../SDL/sdl_pcap.cpp ../SDL/audio_sdl.cpp ../dummy/scsi_dummy.cpp ../dummy/prefs_editor_dummy.cpp
CPUSRCS = ../uae_cpu/basilisk_glue.cpp ../uae_cpu/memory.cpp ../uae_cpu/newcpu.cpp ../uae_cpu/readcpu.cpp ../uae_cpu/fpu/fpu_ieee.cpp cpustbl.cpp cpudefs.cpp cpuemu.cpp

## Files
SRCS = ../main.cpp ../SDL/main_sdl.cpp ../prefs.cpp ../dummy/prefs_dummy.cpp ../SDL/sys_unix_sdl.cpp \
    ../rom_patches.cpp ../slot_rom.cpp ../rsrc_patches.cpp ../emul_op.cpp \
    ../macos_util.cpp ../xpram.cpp ../Unix/xpram_unix.cpp ../timer.cpp timer_unix.cpp \
    ../dummy/clip_dummy.cpp ../adb.cpp ../serial.cpp ../dummy/serial_dummy.cpp ../scc.cpp ../ether.cpp ../sony.cpp \
    ../disk.cpp ../cdrom.cpp ../scsi.cpp ../video.cpp ../SDL/video_sdl.cpp ../audio.cpp \
    ../extfs.cpp ../user_strings.cpp user_strings_sdl.cpp ../SDL/menu_bar.cpp \
    $(SYSSRCS) $(CPUSRCS)

## Rules
.PHONY: all clean mostlyclean clean-objects slirp_lib universal
.SUFFIXES:
.SUFFIXES: .c .cpp .m .s .o .h

all: $(APP)

# Thin arm64 + x86_64 slices, then lipo into a universal Mach-O.
# Host tools stay native; only the final executable is fat. Requires both
# Homebrew SDLs (/opt/homebrew for arm64, /usr/local for x86_64).
# clean-objects must not delete the saved slices between the two builds.
universal:
	$(MAKE) clean-objects
	$(MAKE) ARCH=arm64
	cp $(APP) $(APP)-arm64
	$(MAKE) clean-objects
	$(MAKE) ARCH=amd64
	cp $(APP) $(APP)-x86_64
	lipo -create $(APP)-arm64 $(APP)-x86_64 -output $(APP)

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

$(APP): $(OBJ_DIR) ../slirp/libslirp.a $(OBJS)
	$(CXX) -o $(APP) $(LDFLAGS) $(OBJS) $(LIBS)

../slirp/libslirp.a:
	$(MAKE) -C ../slirp CC="$(CC)" CFLAGS="-I. -O2 -Wall -Wno-unused $(TARGET_ARCH)"

clean-objects:
	rm -f $(APP) $(OBJ_DIR)/* core* *.core *~ *.bak
	$(MAKE) -C ../slirp clean

mostlyclean: clean-objects
	rm -f $(APP)-arm64 $(APP)-x86_64

clean: mostlyclean
	rm -f cpuemu.cpp cpudefs.cpp cputmp*.s cpufast*.s cpustbl.cpp cputbl.h

$(OBJ_DIR)/%.o : %.c
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o : %.m
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o : %.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o : %.s
	$(CC) -c $< -o $@

$(OBJ_DIR)/build68k.o: ../uae_cpu/build68k.c
	$(CC) $(HOST_CPPFLAGS) $(DEFS) $(HOST_CFLAGS) -c $< -o $@

$(OBJ_DIR)/gencpu.o: ../uae_cpu/gencpu.c
	$(CC) $(HOST_CPPFLAGS) $(DEFS) $(HOST_CFLAGS) -c $< -o $@

$(OBJ_DIR)/readcpu_host.o: ../uae_cpu/readcpu.cpp
	$(CXX) $(HOST_CPPFLAGS) $(DEFS) $(HOST_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/cpudefs_host.o: cpudefs.cpp
	$(CXX) $(HOST_CPPFLAGS) $(DEFS) $(HOST_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/build68k: $(OBJ_DIR)/build68k.o
	$(CC) -o $(OBJ_DIR)/build68k $(OBJ_DIR)/build68k.o

$(OBJ_DIR)/gencpu: $(OBJ_DIR)/gencpu.o $(OBJ_DIR)/readcpu_host.o $(OBJ_DIR)/cpudefs_host.o
	$(CXX) -o $(OBJ_DIR)/gencpu $(OBJ_DIR)/gencpu.o $(OBJ_DIR)/readcpu_host.o $(OBJ_DIR)/cpudefs_host.o

cpudefs.cpp: $(OBJ_DIR)/build68k ../uae_cpu/table68k
	$(OBJ_DIR)/build68k <../uae_cpu/table68k >cpudefs.cpp

cpustbl.cpp: cpuemu.cpp
cputbl.h: cpuemu.cpp

cpuemu.cpp: $(OBJ_DIR)/gencpu
	$(OBJ_DIR)/gencpu

