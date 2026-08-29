# Makefile for CockatriceIII macOS ARM64

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

SDL_CFLAGS := $(shell sdl-config --cflags 2>/dev/null || echo "-I/opt/homebrew/include/SDL -D_THREAD_SAFE")
SDL_LIBS := $(shell sdl-config --libs 2>/dev/null || echo "-L/opt/homebrew/lib -lSDL -Wl,-framework,Cocoa")

CPPFLAGS = -I../include -I. -I../uae_cpu -I../Unix -I../SDL -I/opt/homebrew/include $(SDL_CFLAGS)
DEFS = -DHAVE_CONFIG_H -D_REENTRANT -DFPU_IEEE

LDFLAGS = 
LIBS = ../slirp/libslirp.a -lpthread $(SDL_LIBS) -framework Cocoa -framework IOKit

SYSSRCS = sdlmain.m ../SDL/sdl_pcap.cpp ../SDL/audio_sdl.cpp ../dummy/scsi_dummy.cpp ../dummy/prefs_editor_dummy.cpp
CPUSRCS = ../uae_cpu/basilisk_glue.cpp ../uae_cpu/memory.cpp ../uae_cpu/newcpu.cpp ../uae_cpu/readcpu.cpp ../uae_cpu/fpu/fpu_ieee.cpp cpustbl.cpp cpudefs.cpp cpuemu.cpp

## Files
SRCS = ../main.cpp ../SDL/main_sdl.cpp ../prefs.cpp ../dummy/prefs_dummy.cpp ../SDL/sys_unix_sdl.cpp \
    ../rom_patches.cpp ../slot_rom.cpp ../rsrc_patches.cpp ../emul_op.cpp \
    ../macos_util.cpp ../xpram.cpp ../dummy/xpram_dummy.cpp ../timer.cpp timer_unix.cpp \
    ../dummy/clip_dummy.cpp ../adb.cpp ../serial.cpp ../dummy/serial_dummy.cpp ../ether.cpp ../sony.cpp \
    ../disk.cpp ../cdrom.cpp ../scsi.cpp ../video.cpp ../SDL/video_sdl.cpp ../audio.cpp \
    ../extfs.cpp ../user_strings.cpp user_strings_sdl.cpp \
    $(SYSSRCS) $(CPUSRCS)

APP = BasiliskII

## Rules
.PHONY: all clean mostlyclean slirp_lib
.SUFFIXES:
.SUFFIXES: .c .cpp .m .s .o .h

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

$(APP): $(OBJ_DIR) ../slirp/libslirp.a $(OBJS)
	$(CXX) -o $(APP) $(LDFLAGS) $(OBJS) $(LIBS)

../slirp/libslirp.a:
	$(MAKE) -C ../slirp CC="$(CC)" CFLAGS="-I. -O2 -Wall -Wno-unused"

mostlyclean:
	rm -f $(APP) $(OBJ_DIR)/* core* *.core *~ *.bak

clean: mostlyclean
	rm -f cpuemu.cpp cpudefs.cpp cputmp*.s cpufast*.s cpustbl.cpp cputbl.h
	$(MAKE) -C ../slirp clean

$(OBJ_DIR)/%.o : %.c
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o : %.m
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o : %.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o : %.s
	$(CC) -c $< -o $@

$(OBJ_DIR)/build68k: $(OBJ_DIR)/build68k.o
	$(CC) $(LDFLAGS) -o $(OBJ_DIR)/build68k $(OBJ_DIR)/build68k.o

$(OBJ_DIR)/gencpu: $(OBJ_DIR)/gencpu.o $(OBJ_DIR)/readcpu.o $(OBJ_DIR)/cpudefs.o
	$(CXX) $(LDFLAGS) -o $(OBJ_DIR)/gencpu $(OBJ_DIR)/gencpu.o $(OBJ_DIR)/readcpu.o $(OBJ_DIR)/cpudefs.o

cpudefs.cpp: $(OBJ_DIR)/build68k ../uae_cpu/table68k
	$(OBJ_DIR)/build68k <../uae_cpu/table68k >cpudefs.cpp

cpustbl.cpp: cpuemu.cpp
cputbl.h: cpuemu.cpp

cpuemu.cpp: $(OBJ_DIR)/gencpu
	$(OBJ_DIR)/gencpu

