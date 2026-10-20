# IBM PC (platform independent; host build in host/)
idf_component_register(SRCS "pc.c"
                            "pic.c"
                            "pit.c"
                            "ppi.c"
                            "keyboard.c"
                            "speaker.c"
                            "kbc.c"
                            "rtc.c"
                            "video.c"
                            "cga.c"
                            "herc.c"
                            "vga.c"
                            "bios.c"
                            "bios_video.c"
                            "bios_vga.c"
                            "bios_disk.c"
                            "bios_kbd.c"
                            "disk.c"
                            "folder.c"
                            "dma.c"
                            "fdc.c"
                            "hdc.c"
                            "uart.c"
                            "lpt.c"
                            "sound.c"
                            "font.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES x86 upd765 emu8950
                       PRIV_REQUIRES emudbg esp_psram)

# Runs together with the CPU core in the emulation loop
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
