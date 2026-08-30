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
CPPFLAGS = -I../include -I. -I$(MUSASHIDIR) -I$(MUSASHIDIR)/softfloat -I../SDL -ISDL -Ipcap $(SDL_CFLAGS)
DEFS = -DHAVE_CONFIG_H -DENABLE_MON=0 -D_REENTRANT -D_FILE_OFFSET_BITS=64

LDFLAGS =
LIBS = ../slirp/libslirp.a $(SDL_LIBS) -lws2_32 -liphlpapi -lcomdlg32 -luser32 \
	-static-libgcc -static-libstdc++ -l:libwinpthread.a

# Conditionally link DrMinGW for 32-bit x86 if available
ifeq ($(ARCH),x86)
  ifneq ($(wildcard ../drmingw-0.7.7-win32/lib/libexchndl.a),)
    LIBS += ../drmingw-0.7.7-win32/lib/libexchndl.a
  endif
endif

SYSSRCS = ../SDL/sdl_pcap.cpp ../dummy/scsi_dummy.cpp ../SDL/audio_sdl.cpp ../dummy/prefs_editor_dummy.cpp 
CPUSRCS = $(MUSASHIDIR)/basilisk_glue.cpp $(MUSASHIDIR)/memory_musashi.cpp $(MUSASHIDIR)/m68kcpu.c $(MUSASHIDIR)/m68kops.c $(MUSASHIDIR)/m68kdasm.c $(MUSASHIDIR)/softfloat/softfloat.c

## Files
SRCS = ../main.cpp ../SDL/main_sdl.cpp ../prefs.cpp ../dummy/prefs_dummy.cpp ../SDL/sys_unix_sdl.cpp \
    ../rom_patches.cpp ../slot_rom.cpp ../rsrc_patches.cpp ../emul_op.cpp \
    ../macos_util.cpp ../xpram.cpp ../dummy/xpram_dummy.cpp ../timer.cpp ../SDL/timer_common.cpp \
    ../dummy/clip_dummy.cpp ../adb.cpp ../serial.cpp ../dummy/serial_dummy.cpp ../scc.cpp ../ether.cpp ../sony.cpp \
    ../disk.cpp ../cdrom.cpp ../scsi.cpp ../video.cpp ../SDL/video_sdl.cpp ../audio.cpp \
    ../extfs.cpp ../user_strings.cpp user_strings_unix.cpp ../SDL/menu_bar.cpp \
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

$(APP): $(OBJ_DIR) slirp_lib cockatricerc.o $(OBJS)
	$(CXX) -o $(APP) $(LDFLAGS) $(OBJS) cockatricerc.o $(LIBS)

slirp_lib:
ifeq ($(CROSS),1)
	$(MAKE) -C ../slirp clean
endif
	$(MAKE) -C ../slirp CC="$(CC)" AR="$(AR)" RANLIB="$(RANLIB)" CFLAGS="-I. -O2 -Wall -Wno-unused"

mostlyclean:
	rm -f $(APP) $(OBJ_DIR)/*.o $(OBJ_DIR)/*.exe $(OBJ_DIR)/m68kmake core* *.core *~ *.bak cockatricerc.o

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
