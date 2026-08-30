# Makefile for Musashi 680x0 CPU and Test Suites

MUSASHIFILES     = m68kcpu.c m68kdasm.c softfloat/softfloat.c
MUSASHIGENCFILES = m68kops.c
MUSASHIGENHFILES = m68kops.h
MUSASHIGENERATOR = m68kmake

EXE =
EXEPATH = ./

.CFILES   = $(MAINFILES) $(OSDFILES) $(MUSASHIFILES) $(MUSASHIGENCFILES)
.OFILES   = $(.CFILES:%.c=%.o)

CC        = clang
CXX       = clang++
WARNINGS  = -Wall -Wextra -Wno-unused -g
CFLAGS    = $(WARNINGS)
CXXFLAGS  = $(WARNINGS) -Wno-deprecated-declarations -Wno-format -Wno-bitwise-op-parentheses
LFLAGS    = $(WARNINGS)

DELETEFILES = $(MUSASHIGENCFILES) $(MUSASHIGENHFILES) $(.OFILES) $(TARGET) $(MUSASHIGENERATOR)$(EXE) test_driver$(EXE) test_fpu$(EXE) test_integration$(EXE)

all: $(.OFILES)

clean:
	rm -f $(DELETEFILES)
	@$(MAKE) -C test clean

m68kcpu.o: $(MUSASHIGENHFILES) m68kfpu.c m68kmmu.h softfloat/softfloat.c softfloat/softfloat.h

$(MUSASHIGENCFILES) $(MUSASHIGENHFILES): $(MUSASHIGENERATOR)$(EXE)
	$(EXEPATH)$(MUSASHIGENERATOR)$(EXE)

$(MUSASHIGENERATOR)$(EXE):  $(MUSASHIGENERATOR).c
	$(CC) -o  $(MUSASHIGENERATOR)$(EXE)  $(MUSASHIGENERATOR).c

test_driver$(EXE): test/test_driver.c $(.OFILES)
	$(CC) $(CFLAGS) -o test_driver$(EXE) test/test_driver.c $(.OFILES) -I. -lm

test_fpu$(EXE): test_fpu.c softfloat/softfloat.c $(MUSASHIGENHFILES)
	$(CC) $(CFLAGS) -I. -Isoftfloat test_fpu.c softfloat/softfloat.c -o test_fpu$(EXE) -lm

test_integration$(EXE): test_integration.cpp memory_musashi.cpp basilisk_glue.cpp ../emul_op.cpp ../scsi.cpp ../dummy/scsi_dummy.cpp $(.OFILES)
	$(CXX) $(CXXFLAGS) -DHAVE_CONFIG_H -I../OSX64 -I../include -I. -Isoftfloat -I../Unix -I../dummy test_integration.cpp memory_musashi.cpp basilisk_glue.cpp ../emul_op.cpp ../scsi.cpp ../dummy/scsi_dummy.cpp $(.OFILES) -o test_integration$(EXE) -lm

TESTS_68000 = abcd adda add_i addq add addx andi_to_ccr andi_to_sr and \
               bcc bchg bclr bool_i bset bsr btst \
               chk cmpa cmpm cmp dbcc divs divu eori_to_ccr eori_to_sr eor exg ext \
               lea_pea lea_tas lea_tst links \
               movem movep moveq move move_usp move_xxx_flags muls mulu \
               nbcd negs op_cmp_i ori_to_ccr ori_to_sr or \
               rox roxx rtr sbcd scc shifts2 shifts suba sub_i subq sub subx swap trapv

TESTS_68040 = bfchg bfclr bfext bfffo bfins bfset bftst cas chk2 cmp2 \
	divs_long divu_long interrupt jmp mul_long rtd shifts3 trapcc

TESTS_68000_RUN = $(TESTS_68000:%=%.bin)
$(TESTS_68000_RUN): test_driver$(EXE)
	./test_driver$(EXE) test/mc68000/$@

TESTS_68040_RUN = $(TESTS_68040:%=%.bin)
$(TESTS_68040_RUN): test_driver$(EXE)
	./test_driver$(EXE) test/mc68040/$@

build_tests:
	@$(MAKE) -C test all

test: $(TESTS_68000_RUN) $(TESTS_68040_RUN) test_fpu$(EXE) test_integration$(EXE)
	./test_fpu$(EXE)
	./test_integration$(EXE)
