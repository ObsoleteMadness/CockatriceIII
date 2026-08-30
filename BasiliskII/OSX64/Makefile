# Makefile for CockatriceIII macOS 64-bit (ARM64 & AMD64) with Musashi CPU Core

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
SDL_CFLAGS := $(shell $(SDL_PREFIX)/bin/sdl-config --cflags 2>/dev/null || echo "-I$(SDL_PREFIX)/include/SDL -D_THREAD_SAFE")
SDL_LIBS := $(shell $(SDL_PREFIX)/bin/sdl-config --libs 2>/dev/null || echo "-L$(SDL_PREFIX)/lib -lSDL -Wl,-framework,Cocoa")

CPPFLAGS = -I../include -I. -I../Musashi -I../Musashi/softfloat -I../Unix -I../SDL -I/opt/homebrew/include -I/usr/local/include $(SDL_CFLAGS)

HOST_CPPFLAGS = -I../include -I. -I../Musashi -I../Musashi/softfloat -I../Unix -I../SDL
HOST_CFLAGS = -O2 -Wall -Wno-unused -Wno-deprecated-non-prototype
HOST_CXXFLAGS = -O2 -Wall -Wno-unused -Wno-writable-strings -Wno-multichar -Wno-deprecated-declarations -Wno-format -Wno-c++11-narrowing
DEFS = -DHAVE_CONFIG_H -D_REENTRANT

LIBS = ../slirp/libslirp.a -lpthread $(SDL_LIBS) -framework Cocoa -framework IOKit

SYSSRCS = sdlmain.m ../SDL/sdl_pcap.cpp ../SDL/audio_sdl.cpp ../dummy/scsi_dummy.cpp ../dummy/prefs_editor_dummy.cpp
CPUSRCS = ../cpu_engine.cpp ../Musashi/basilisk_glue.cpp ../Musashi/memory_musashi.cpp ../WinUAE/winuae_glue.cpp ../Emu68/emu68_glue.cpp ../Musashi/m68kcpu.c ../Musashi/m68kops.c ../Musashi/m68kdasm.c ../Musashi/softfloat/softfloat.c

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
	rm -f cpuemu.cpp cpudefs.cpp cpustbl.cpp cputbl.h

$(OBJ_DIR)/%.o : %.c
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o : %.m
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

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
