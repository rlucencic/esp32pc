# Host build of the IBM PC: make -C components/ibmpc/host
#   build/pc_host [frames] [out.ppm]   (see host_main.c)
X86       := ../../../../esp32emu/components/x86
EMUDBG    := ../../../../esp32emu/components/emudbg
UPD765    := ../../../../esp32emu/components/upd765
EMU8950   := ../../../../esp32emu/components/emu8950
PC        := ..
BUILD     := build
CFLAGS    := -O2 -g -Wall -Wextra -Wno-unused-parameter -DX86_HOST_HOOK \
             -I$(PC)/include -I$(PC) -I$(X86)/include -I$(EMUDBG)/include -I$(UPD765)/include -I$(EMU8950)/include

PC_SRCS   := $(wildcard $(PC)/*.c)
X86_SRCS  := $(X86)/x86.c $(X86)/x87.c $(X86)/x86_dis.c $(X86)/x86_dbg.c
DBG_SRCS  := $(EMUDBG)/emudbg.c $(EMUDBG)/emudbg_posix.c
UPD_SRCS  := $(UPD765)/upd765.c
OPL_SRCS  := $(EMU8950)/emu8950.c $(EMU8950)/emuadpcm.c
PC_OBJS   := $(patsubst $(PC)/%.c,$(BUILD)/pc/%.o,$(PC_SRCS))
X86_OBJS  := $(patsubst $(X86)/%.c,$(BUILD)/x86/%.o,$(X86_SRCS))
UPD_OBJS  := $(patsubst $(UPD765)/%.c,$(BUILD)/upd765/%.o,$(UPD_SRCS))
OPL_OBJS  := $(patsubst $(EMU8950)/%.c,$(BUILD)/emu8950/%.o,$(OPL_SRCS))
DBG_OBJS  := $(patsubst $(EMUDBG)/%.c,$(BUILD)/emudbg/%.o,$(DBG_SRCS))

$(BUILD)/pc_host: host_main.c $(PC_OBJS) $(X86_OBJS) $(UPD_OBJS) $(OPL_OBJS) $(DBG_OBJS)
	$(CC) $(CFLAGS) $^ -lm -lpthread -o $@

$(BUILD)/pc/%.o: $(PC)/%.c $(wildcard $(PC)/*.h $(PC)/include/*.h) $(X86)/include/x86.h $(UPD765)/include/upd765.h $(EMU8950)/include/emu8950.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/x86/%.o: $(X86)/%.c $(X86)/include/x86.h $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/upd765/%.o: $(UPD765)/%.c $(UPD765)/include/upd765.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/emu8950/%.o: $(EMU8950)/%.c $(EMU8950)/include/emu8950.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-sign-compare -c $< -o $@

$(BUILD)/emudbg/%.o: $(EMUDBG)/%.c $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: clean
