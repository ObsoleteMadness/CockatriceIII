# Makefile for Cockatrice III (MinGW / MSYS2 - Win32 x86, x64, ARM64)

SHELL = /bin/sh

prefix = /usr/local
exec_prefix = ${prefix}
bindir = ${exec_prefix}/bin
datadir = ${prefix}/share
mandir = ${prefix}/man

# GNU Make predefines CC=cc and CXX=g++. `?=` therefore cannot replace CXX,
# which breaks CLANGARM64 (clang++ / c++ only — no g++ binary). Override
# Make's defaults; command-line and environment CC/CXX still win because
# their origin is not "default".
ifeq ($(origin CC),default)
CC = cc
endif
ifeq ($(origin CXX),default)
CXX = c++
endif
WINDRES ?= windres
CC_FOR_BUILD ?= $(CC)
CXX_FOR_BUILD ?= $(CXX)

SDL_CFLAGS := $(shell sdl-config --cflags 2>/dev/null || echo "-ISDL -Ipcap")
SDL_LIBS := $(shell sdl-config --libs 2>/dev/null || echo "-Llibs -lSDLmain -lSDL")

CFLAGS ?= -O2 -Wall -Wno-unused
CXXFLAGS ?= -O2 -Wall -Wno-unused -Wno-deprecated-declarations -Wno-c++11-narrowing
CPPFLAGS = -I../include -I. -I../uae_cpu -I../SDL -ISDL -Ipcap $(SDL_CFLAGS)
DEFS = -DHAVE_CONFIG_H -DENABLE_MON=0 -D_REENTRANT -DFPU_IEEE -D_FILE_OFFSET_BITS=64

LDFLAGS = 
LIBS = ../slirp/libslirp.a $(SDL_LIBS) -lws2_32 -liphlpapi -lcomdlg32 -luser32 -static-libgcc -static-libstdc++

# Conditionally link DrMinGW for 32-bit x86 if available
ifeq ($(ARCH),x86)
  ifneq ($(wildcard ../drmingw-0.7.7-win32/lib/libexchndl.a),)
    LIBS += ../drmingw-0.7.7-win32/lib/libexchndl.a
  endif
endif

SYSSRCS = ../SDL/sdl_pcap.cpp ../dummy/scsi_dummy.cpp ../SDL/audio_sdl.cpp ../dummy/prefs_editor_dummy.cpp 
CPUSRCS = ../uae_cpu/basilisk_glue.cpp ../uae_cpu/memory.cpp ../uae_cpu/newcpu.cpp ../uae_cpu/readcpu.cpp cpustbl.cpp cpudefs.cpp cpuemu.cpp
FPUSRCS = ../uae_cpu/fpu/fpu_ieee.cpp

## Files
SRCS = ../main.cpp ../SDL/main_sdl.cpp ../prefs.cpp ../dummy/prefs_dummy.cpp ../SDL/sys_unix_sdl.cpp \
    ../rom_patches.cpp ../slot_rom.cpp ../rsrc_patches.cpp ../emul_op.cpp \
    ../macos_util.cpp ../xpram.cpp ../dummy/xpram_dummy.cpp ../timer.cpp ../SDL/timer_common.cpp \
    ../dummy/clip_dummy.cpp ../adb.cpp ../serial.cpp ../dummy/serial_dummy.cpp ../scc.cpp ../ether.cpp ../sony.cpp \
    ../disk.cpp ../cdrom.cpp ../scsi.cpp ../video.cpp ../SDL/video_sdl.cpp ../audio.cpp \
    ../extfs.cpp ../user_strings.cpp user_strings_unix.cpp ../SDL/menu_bar.cpp ../windows/menu_bar_win32.cpp \
    $(SYSSRCS) $(CPUSRCS) $(FPUSRCS)

APP = CockatriceIII.exe

## Rules
.PHONY: all clean mostlyclean slirp_lib
.SUFFIXES:
.SUFFIXES: .c .cpp .s .o .h .rc

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

$(APP): $(OBJ_DIR) ../slirp/libslirp.a $(OBJS) cockatricerc.o
	$(CXX) -o $(APP) $(LDFLAGS) $(OBJS) cockatricerc.o $(LIBS)

../slirp/libslirp.a:
	$(MAKE) -C ../slirp CC="$(CC)" CFLAGS="-I. -O2 -Wall -Wno-unused"

mostlyclean:
	rm -f $(APP) $(OBJ_DIR)/*.o $(OBJ_DIR)/*.exe $(OBJ_DIR)/build68k $(OBJ_DIR)/gencpu core* *.core *~ *.bak cockatricerc.o

clean: mostlyclean
	rm -f cpuemu.cpp cpudefs.cpp cputmp*.s cpufast*.s cpustbl.cpp cputbl.h
	$(MAKE) -C ../slirp clean

cockatricerc.o: cockatrice.rc
	$(WINDRES) cockatrice.rc -O coff -o cockatricerc.o

$(OBJ_DIR)/%.o : %.c
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o : %.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o : %.s
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/build68k: $(OBJ_DIR)/build68k.o
	$(CC_FOR_BUILD) $(LDFLAGS) -o $(OBJ_DIR)/build68k $(OBJ_DIR)/build68k.o

$(OBJ_DIR)/gencpu: $(OBJ_DIR)/gencpu.o $(OBJ_DIR)/readcpu.o $(OBJ_DIR)/cpudefs.o
	$(CXX_FOR_BUILD) $(LDFLAGS) -o $(OBJ_DIR)/gencpu $(OBJ_DIR)/gencpu.o $(OBJ_DIR)/readcpu.o $(OBJ_DIR)/cpudefs.o

cpudefs.cpp: $(OBJ_DIR)/build68k ../uae_cpu/table68k
	$(OBJ_DIR)/build68k < ../uae_cpu/table68k > cpudefs.cpp

cpustbl.cpp: cpuemu.cpp
cputbl.h: cpuemu.cpp

cpuemu.cpp: $(OBJ_DIR)/gencpu
	$(OBJ_DIR)/gencpu

