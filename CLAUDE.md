# esp32pc: IBM PC emulator on the Makerfabs MaTouch ESP32-S3 7" and the M5Stack Tab5

An IBM PC/XT (an 8088 at 4.77 MHz, 256-640 KB) and PC/AT (an 80286 at 6
MHz, on the Tab5 also 8 MHz; 640 KB and extended memory, protected mode),
and a Custom machine (the 8086 at 8 MHz; on the Tab5 the 286 at 8 and 12
MHz too), with a CGA, Hercules, EGA or VGA card, an AdLib and a Sound
Blaster 2.0, an 8087/287 coprocessor (a choice on the XT and AT, always on
Custom), two floppy drives and two hard disks from images (or folders) on
the SD card, a Microsoft mouse on COM1, a printer on LPT1 (a file on the
card) (`docs/architecture.md`: the plan and the decisions). Built-in BIOS
(no ROM images), or a real BIOS ROM from the SD card. Bluetooth and USB
keyboards and mice, touch as a touchpad, an LVGL setup screen, a web page
for disk images (it packs dropped files and folders onto one). ESP-IDF
v5.5.4, C.

The board side (tasks, LCD, touch, SD, WiFi, BLE, setup screen, web page,
console) and the x86 core are the shared library in `../esp32emu`; see its
CLAUDE.md (changes there need all projects built, for both boards).

## Build, flash, debug

As the siblings (`../esp32ste/CLAUDE.md`): the MaTouch with `idf.py build`
and `-p /dev/cu.usbmodem1101 flash`, the Tab5 with `-B build-tab5
-DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig` and
`/dev/cu.usbmodem3101`, both through the IDF venv's environment. No ROM
partitions: `flash` writes the partition table and the app. The boards are
shared with the other projects: after another one, a full `flash`.

- Console (`../esp32emu/tools/emu_console.py`): the runtime's commands, and
  `k <hex>` tap a key (scan code set 1, `E0xx` extended), `K <hex> <0|1>`,
  `T <text>` type (`\n` Enter). Touches (`t x y [ms]`, the board's
  pixels): Start/Restart `t 850 532` (MaTouch), `t 1065 637` (Tab5); the
  setup screen from a running machine: the top-right corner held, `t 1015
  10 2600` / `t 1270 10 2600`. Paths given to `screenshot`/`lcd` must be
  absolute (the tool runs in `../esp32emu/tools`).
- The debugger: port 6502 on the board (`PC_DEBUG=<port>` on the host);
  the coprocessor's FCW, FSW, FTW and ST0-ST7 (floats) among the
  registers.
- Web: `http://ibm-pc.local` (`/api/status`, `PUT /disks/<name>`, `PUT
  /api/drive/<n>` with the name in the body; a folder is `NAME/`). The
  page's own logic can be driven from a headless Chrome
  (`--remote-debugging-port`, then `Runtime.evaluate` of e.g. `send([new
  File(...)])` through node's WebSocket): the Chrome DevTools connector
  didn't connect here.
- Flashing restarts the board and ends the user's session: say so first.

## Layout

| Path | What |
|---|---|
| `main/main.c` | The PC as an `emu_machine_t`: memory, settings, the HID key map, the mouse and touch, the drives, the BIOS ROM file, status, console commands |
| `main/settings.c` | NVS settings (namespace `pc`: `machine`, `cpu`, `khz`, `fpu`, `video`, `ram_kb`, `ext_kb`, `bios`); the processors (with or without the coprocessor) and memory each machine offers, per board |
| `main/web/pcfiles.js` | The web page's packer (`/machine.js`): dropped files, folders and ZIP contents onto a FAT floppy (the smallest that fits) or a FAT16 hard disk image, in the browser; runs under node too |
| `components/ibmpc/` | The PC, platform independent (`include/pc.h`); `host/` its host build |
| `components/ibmpc/pc.c` | Memory map, I/O ports (the XT decodes 10 bits, 32-port blocks below 100h), the frame loop and its events |
| `pic.c`, `pit.c`, `ppi.c`, `keyboard.c` | 8259 (the AT's two cascaded), 8253 (worked out from the time when read), the XT's 8255 and keyboard port, the keyboard itself (set 1, typematic, typing) |
| `kbc.c`, `rtc.c` | The AT's 8042 (command byte, A20 gate, CPU reset, the keyboard's answers) and port 61h (refresh toggle); the MC146818 clock and CMOS RAM (time from the board's clock, update-in-progress, periodic IRQ 8, the shutdown byte) |
| `dma.c`, `fdc.c` | The 8237s (the AT's second, page registers, the XT's refresh counting on channel 0) and the 765 through `../esp32emu/components/upd765` (DMA, IRQ 6, four units) |
| `hdc.c` | The AT's hard disk controller: the WD1003's task file as IDE drives answer it (1F0h-1F7h, 3F6h, IRQ 14; CHS and LBA, multiple, identify) |
| `uart.c` | COM1 (an 8250 at 3F8h, IRQ 4) and a Microsoft mouse on it (`pc_mouse()`) |
| `lpt.c` | LPT1 (378h-37Ah, the printer adapter's registers) when `pc_config_t.printer` takes the bytes |
| `sound.c` | The AdLib (an OPL2 at 388h through `../esp32emu/components/emu8950`) and the Sound Blaster 2.0's DSP (220h, IRQ 7, DMA 1: `BLASTER=A220 I7 D1 T3`) |
| `speaker.c` | The PC speaker: each 44.1 kHz sample the share of its time the cone was pushed (port 61h bit 1 and channel 2's output, the square wave worked out), caught up when the timer or port 61h change and at the frame's end; a high pass; the BIOS's bell |
| `video.c`, `video.h` | Timing from the card's totals, the picture copies (the part of the card's memory a picture shows), registers changed mid-picture (applied per line), drawing's lock |
| `cga.c`, `herc.c`, `vga.c` | The CGA; the Hercules card (MDA text, 720 x 348 graphics); the EGA and VGA (planes, their logic, register-driven drawing, the DAC) |
| `bios.c`, `bios_video.c`, `bios_vga.c`, `bios_disk.c`, `bios_kbd.c` | The built-in BIOS: the ROM (IBM's entry points, tables, the 8x8 font), POST, INT 10h (CGA/MDA, and the EGA's and VGA's with their video ROM at C0000h), 13h, 14h, 15h, 09h/16h and the rest; a real BIOS's option ROMs |
| `disk.c` | Floppy and hard disk images, blank formatted ones |
| `folder.c` | A folder on the SD card as a read-only FAT16 hard disk, its sectors made up as they're read |
| `font.c` | IBM's character sets from the Ultimate Oldschool PC Font Pack (CC BY-SA 4.0), made by `tools/fon2c.py` |
| `docs/architecture.md` | The architecture review: decisions, phases |
| `tools/bench.s`, `bench.sh` | The speed benchmark: a boot floppy running a mix of 8088 code forever, its rounds counted on screen (`bench.sh out.img`, needs LLVM) |
| `tools/mkboot.sh`, `fatmk.py` | A boot floppy from a test program (`host/tests/*.S`); a FreeDOS boot floppy with a program's files |

## How it works

- **The CPU** is `../esp32emu/components/x86` (8088, 8086, 80286 models,
  and `x87.c` the 8087/287;
  SingleStepTests 8088 and 8086 suites pass whole, undefined flags too; the
  286's real mode suite but for 17 of 1,478,000). The 286's protected mode
  (descriptors and limits folded into the segments' direct windows, gates,
  task switches, error codes, double fault and shutdown, LOADALL) is
  checked by `host/tests/pm.S` and by Minix 2.0.4 (its 16-bit kernel in
  protected mode, its own floppy and AT disk drivers).
- **The AT** (`pc.c`, `kbc.c`, `rtc.c`): extended memory above 1 MB, the
  ROM again at FF0000 for the 286's reset, one wait state on memory (the
  IBM AT's), the 8042 (A20 through its output port, the CPU's reset through
  it or its pulse command), the CMOS clock, the second 8259 (vectors
  70h-77h). The BIOS's INT 15h has 86h (wait), 87h (block move, A20 opened
  for it), 88h (extended memory size), 89h (into protected mode, IBM's
  GDT layout), C0h, and POST honours the shutdown byte's 05h and 0Ah (back
  to 40:67, which is how programs leave protected mode: a triple fault or
  the 8042's reset).
- **Video cards**: the CGA and Hercules from their 6845's registers; the
  EGA and VGA (`vga.c`) with 256 KB as four interleaved planes (`vram[o *
  4 + p]`, so chain-4 is linear: mode 13h maps A0000h straight into the
  page table when the logic is at its defaults, the rest goes through the
  memory callbacks), drawn from the CRTC/sequencer/attribute registers as
  the hardware generates the picture (split screen, panning, doubling,
  word/byte/doubleword modes). Their BIOS (`bios_vga.c`) programs modes
  from the cards' 64-byte parameter tables in its own video ROM.
- **Time** is CPU clocks. A frame is the video card's (its 6845's totals at
  its dot clock: CGA 59.92 Hz, 262 lines; MDA/Hercules 49.82 Hz, 370 lines).
  The CPU runs to the next event (the timer's IRQ 0 edge, the keyboard's
  next byte) or the frame's end; the PIT's ticks against CPU clocks are a
  0.32 fixed-point product rebased each frame (the XT's 4.77 MHz exactly
  four clocks a tick). Status registers (retrace) work out the beam from
  the time when read: no events for polling.
- **The BIOS** (`bios.c`): a 64 KB ROM made at each reset in the RAM block
  at F0000 (mapped read-only): IBM's entry points as jumps to stubs, each a
  few instructions around a trap (`FE 38 nn`, honoured by the core only
  inside F0000-FFFFF) whose service is C. The services drive the emulated
  chips through their ports, so programs read back what the BIOS set. INT
  16h's wait sleeps with HLT (the stub loops on CF); INT 19h without a
  disk waits and tries again (a disk can be put in meanwhile). INT 13h reads
  the images directly; programs that drive the floppy controller, DMA or
  the AT's disk controller get the chips. With a real BIOS
  (`pc_config_t.bios_rom`: `pcxt.rom`/`pcat.rom` beside the images) the
  traps are honoured only in C0000h-CFFFFh, where ours become option ROMs:
  the hard disks' INT 13h and INT 19h at C8000h (as an XT controller card's:
  the BIOS's own INT 13h kept for the floppies), the EGA/VGA BIOS at
  C0000h.
- **The mouse**: `uart.c` sends 'M' when DTR and RTS come on, then
  three-byte packets at the line's speed; `pc_mouse()` takes the BT mouse
  (`hid_mouse`) and touch (`main.c`: a drag moves, a tap is the left
  button, a long press the right; a press between two packets is kept for
  one).
- **Pictures** (`video.c`): esp32arc's picture copies: at each frame's end
  the registers are noted and the card's memory copied on core 0 (`picture`
  task) with a hash as the signature (copied and hashed in one pass);
  three copies, only the frame before the presenter's next picture.
- **The EGA's and VGA's planar memory** (`vga.c`): the CPU's bus callbacks
  are the card's own (`pc_reset()` puts `video_read_fn`/`video_write_fn`
  into `cpu.bus`), and `remap()` works out fast paths: write mode 0 with
  no rotation, no set/reset, the ALU passing and the bit mask all ones is
  one 32-bit read-modify-write of the four interleaved plane bytes; read
  mode 0 one load (the latches with it).
- **Sound** (`sound.c`, `speaker.c`): the AdLib's OPL2 is emu8950 at its
  own rate (3.58 MHz / 72 = 49,716 Hz: its rate converter uses doubles,
  which neither chip has in hardware) resampled to 44.1 kHz in 16.16 fixed
  point, on the speaker's task (core 0); the CPU's register writes reach it
  through a ring stamped with the sound's sample position, and its timers
  and status (how games find it: timer 1, then C0h) are worked out on the
  emulation's time. It rests after a second of silence. The Sound
  Blaster's DSP takes DMA bytes as the 44.1 kHz samples are made (its
  rate's share of each), single-cycle and auto-init blocks with their IRQ
  7, the speaker on/off, version 2.01, the forced interrupt.
  `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` and the component's linker
  fragment put emu8950's tables in PSRAM (its allocations too, through
  wrappers): 9.7 KB of internal RAM back.
- **The coprocessor** (`../esp32emu/components/x86/x87.c`, see x86.h):
  80-bit registers kept exactly (moves, integer and BCD conversions,
  compares, FRNDINT, FSCALE, FXTRACT, FPREM, FSAVE/FRSTOR), FADD, FSUB,
  FMUL, FDIV and FSQRT exact (128-bit intermediates, the precision and
  rounding controls, PE; checked against Python's Fraction on 120,000
  random and edge operands), the functions through libm;
  projective infinity (the 287's test against the 387). The XT: SW1-2 off
  in the 8255's switches, an unmasked exception an NMI (port A0h); the AT:
  IRQ 13 until a write to F0h (the BIOS's INT 75h, then INT 2), CMOS 14h
  bit 1. The built-in BIOS's equipment word says so.
- **Folders as hard disks** (`folder.c`, the drives' list shows the disk
  folder's subfolders as `NAME/`: esp32emu's `disks_config_t.folders`):
  read once at insert (names, sizes, dates into PSRAM), each folder and
  file a contiguous run of clusters in the order found, the MBR, boot
  sector, FATs and directories worked out per sector, file sectors read
  from the card. 8.3 names as Windows makes them (`~1`); at least 4,200
  clusters (FAT16), 2 KB and up to 65,000; type 04 up to 32 MB (DOS 3.3),
  06 above; 1,024 cylinders at most (504 MB; what doesn't fit is left
  out). Read only: DOS reports a write-protect error.
- **The web page's packer** (`main/web/pcfiles.js`, esp32emu's
  `registerPacker()`): dropped files (loose, from dropped folders with
  their paths, or a ZIP's) go onto one FAT image built in the browser:
  the smallest floppy that holds them (360 KB to 2.88 MB), else a FAT16
  `.hdd` with a quarter more room and 1 MB besides. The same 8.3 rules as
  the folders.
- **The printer** (`lpt.c`, `main.c`): the strobe's rising edge prints the
  data latch; INT 17h (90h ready), INT 05h prints the text screen (50:00
  as IBM's). On the board the bytes go into a 16 KB ring and once a second
  (core 0) onto `PRINTER.TXT` in the disk folder (busy while it's full);
  the web page's status counts them.
- **Memory** (`main.c`): one PSRAM block (4 KB aligned) laid out as the
  PC's memory (the first megabyte with the cards' memory and the ROM, then
  the AT's extended memory), allocated first: the Tab5 reserves 16 MB at
  power-on, the MaTouch what the settings need and restarts into the
  machine for more (`emu_reboot_into_machine()`). The CPU's page table
  (`PC_PAGES`: 1 KB for the XT, 16 KB for the AT): internal RAM on the
  Tab5, PSRAM on the MaTouch (the core only looks there when a segment
  changes or an access goes to the bus; the 16 KB went to the S3's
  instruction cache, 32 KB).
- **Processors per board** (`settings.c`): only what runs at full speed
  there. Tab5: XT 8088 4.77; AT 286 6, 8 MHz; Custom 8088 4.77, 8086 8, 286
  8, 12 MHz. MaTouch: XT 8088 4.77; AT 286 6 MHz; Custom 8088 4.77, 8086 8
  (its 286 at 8 MHz ran the benchmark at 97-99% with core 1 at 100%).
  Settings a board doesn't offer fall back to its first. The XT and AT
  also offer each with the coprocessor (the buttons' second line: "6 MHz
  + 287"); Custom always has it.
- **Disks**: floppies and hard disks read and written in place (geometry
  from a VHD footer, the partition table, or 16 x 63), or a folder. Blank hard
  disks (10-128 MB) are partitioned and formatted with a master boot record
  that starts the active partition: `SYS C:` (FreeDOS, DOS 4+) or `FORMAT C:
  /S` makes them bootable.

## Testing

- **Host build**: `make -C components/ibmpc/host`, then `build/pc_host
  [frames] [out.ppm]` with `PC_FLOPPY_A/B`, `PC_HD`, `PC_HD2`,
  `PC_VIDEO=cga|herc|ega|vga`, `PC_MACHINE=at` (`PC_MHZ`, `PC_EXT`),
  `PC_CPU=8086`, `PC_RAM`, `PC_BIOS=<rom>`, `PC_TYPE="<frame>:<text>;..."`
  (`;` separates entries; `$'\x1b'` types ESC), `PC_MOUSE="<frame>:dx,dy,b;..."`,
  `PC_SHOTS="<frame>,..."`, `PC_NEW_HD=<path>:<MB>`,
  `PC_NEW_FLOPPY=<path>:<KB>`, `PC_TRACE=<seg>:<off>[:n]`,
  `PC_TRACE_ALL=<frame>`, `PC_LOG13` (INT 13h calls, the disk controller's
  commands), `PC_WAV=<file>`, `PC_DEBUG=<port>`, `PC_KEYS="<frame>:<hex>[:<frames>];..."`
  (a key held), `PC_FPU` (the coprocessor), `PC_PRINTER=<file>`,
  `PC_DUMP_HD=<file>` (C:'s sectors, e.g. a folder's, then stop: check them
  with `fsck_msdos -n` on the partition). `PC_HD` takes a folder too. Runs
  ~200x real time.
- **Test programs** (`components/ibmpc/host/tests/*.S`, `tools/mkboot.sh
  IN.S OUT.img` makes a boot floppy; each line prints OK or BAD): `fdc`,
  `queue`, `herc`, `cgasplit`, `vga` and `ega` (a key between stages),
  `pm` (protected mode, ends back in real mode through a triple fault),
  `pm89` (INT 15h AH=89h), `hdc` (needs a hard disk), `mouse` (with
  `PC_MOUSE`), `sound` (AdLib timer and note, the DSP's reset, version,
  DMA blocks and IRQs; with `PC_WAV`), `fpu` (with `PC_FPU`: 27 checks on
  the AT's 287 or the XT's 8087, ending with the control word and the BAD
  count). They run on the boards too (upload, insert, `r`).
- **Disk images of files**: `node pcfiles.js` through a small wrapper
  (walk a folder into `{path, data, date}`, `pack()`) makes the web page's
  images on the host; `fsck_msdos -n` checks them (the partition, from
  sector 63, for hard disks).
- **Games**: `tools/fatmk.py` makes a boot floppy of FreeDOS's kernel and
  the game's files (FreeDOS without a CONFIG.SYS leaves ~506 KB: Keen 1
  needs more, so `SHELL=A:\KEEN1.EXE` starts it with no FreeCOM resident;
  `--no-shell` leaves COMMAND.COM out, `--2880` makes a 2.88 MB floppy for
  Wolfenstein 3D's 1.4 MB). Shareware from archive.org: Commander Keen 1
  and 4 (EGA scrolling; Keen 4's AdLib music plays in the game, its title
  and menus are silent), Wolfenstein 3D 1.4 (VGA, AdLib music, Sound
  Blaster effects: its menus need the arrows after the intro, and five Ups
  reach "New Game" because the greyed "Save Game" is skipped), Duke Nukem
  1 v2.0 (EGA). OpenGEM (FreeDOS's repository) with CTMOUSE on a packed
  hard disk image (it wants C:\ as its root): its desktop in VGA 640 x
  480, menus under the mouse.
- **XMS on the AT**: FreeDOS's FDXMS286 (repository `base/fdxms286.zip`,
  GPL) with `DOS=HIGH`: the kernel in the HMA, its XMSTEST allocating,
  filling, verifying and moving blocks through INT 15h AH=87h, MEM showing
  1,024 KB of XMS and 623 KB free below 640 KB.
- **The coprocessor's arithmetic**: a host harness that `#include`s x87.c
  runs add/sub/mul/div/sqrt on random and edge operands (all precision and
  rounding controls) against Python's `Fraction`, bit for bit with PE.
- **Real software**: Minix 2.0.4's i86 ROOT.MNX (padded to 1.44 MB) in A:
  and USR.MNX in B:, `=` at its monitor, `fd1` for /usr, `root`; ESC, `c0=at`,
  `boot` for its AT disk driver. GLaBIOS's 8X ROM with `PC_BIOS` (XT): its
  POST passes, it boots A: and C:.
- **FreeDOS 1.3**'s floppy edition (`144m/x86BOOT.img`, GPL, from
  ibiblio.org) boots to its installer on the 8088 in ~100 s of emulated time
  (its 39 KB SETUP.BAT read line by line from the floppy: a real XT is that
  slow); `n` Enter leaves it for `A:\>`.
- **CPU tests**: `make -C ../esp32emu/components/x86/test`, `make data`
  fetches the suites, `test` runs the 8088's, 8086's and 286's (and the
  8088's and 286's again with `-w`: writable pages, the core's direct
  writes).
- **Speed**: `tools/bench.sh` makes the benchmark floppy; upload it, insert
  it, reset, and read speed and core 1's load from `/api/status`. FreeDOS's
  floppy boot is the real-world check: its FreeCOM start-up (an INT 16h
  polling loop through the BIOS traps) and its loading (a memory scan of
  short instructions, ~1.3 million a second at 6 MHz) are the heaviest
  parts seen. The core's own changes are checked on the host: the suites,
  then the benchmark and FreeDOS boots compared frame for frame (screens,
  CS:IP, instruction counts) against the core before the change.

## Measured

- Both boards (2026-10-20), the AT at 6 MHz: Wolfenstein 3D (VGA, AdLib
  and Sound Blaster): the Tab5 100% in its menus and game; the MaTouch
  100% in the menus, 93% standing in the game and 83-88% moving (core 1
  100%: 70-84% before the VGA's fast paths, 77-87% before the drawing went
  into internal RAM). Experiments: with the presenter not drawing (the
  copies still made) 86-90%; without its hash pass (every row drawn)
  slower; neither the OPL's synthesis nor the framebuffer writes made a
  measurable difference. Keen 4
  on the world map: MaTouch 97-102% (core 1 92-96%), Tab5 99-100% (45%).
  Core 0 with AdLib music: 30-55% on the MaTouch, 41-58% on the Tab5.
  Internal RAM on the MaTouch: ~45 KB free, 27 KB the least.
- Both boards (2026-10-20), the AT at 6 MHz: the VGA costs no speed in
  text (the benchmark: MaTouch 100%, core 1 69-71% against 68% on the CGA;
  Tab5 37%); FreeDOS's boot with the VGA on the MaTouch 99-100%, core 1
  72-79%. Internal RAM on the MaTouch: ~40 KB free running, 22.5 KB the
  least. The protected mode, VGA/EGA, Hercules, CGA split, FDC, mouse
  (touch), debugger and GLaBIOS checks passed on the boards. Commander
  Keen 1 (shareware, the EGA) at 98-102% on both: core 1 68-97% on the
  MaTouch in its scrolling intro, 38-55% on the Tab5.
- Tab5 (2026-10-20), all at 100% of real time, core 1's load: the AT at 6
  MHz the benchmark 37-39% (1.14 million instructions a second), at 8 MHz
  47% (1.51), FreeDOS's boot 52-57%; the 286 at 12 MHz the benchmark 70%
  (2.27), FreeDOS's FreeCOM start-up 84%, its loading 75-81%. ~135 KB
  internal RAM free. (Before the core's speed work the AT at 6 MHz took 83%
  for the benchmark: ~260 P4 cycles an instruction, now ~120.)
- MaTouch (2026-10-20), at 100%: the AT at 6 MHz the benchmark 68%,
  FreeDOS's FreeCOM start-up 69%, its loading 72-82%; the 8086 at 8 MHz
  the benchmark 65% (0.70 million a second). 46.7 KB internal RAM free
  (29.5 KB the least seen). (Before: 60-73% of real time, at core 1's
  limit.)
- Earlier, the XT (2026-10-19/20): FreeDOS at full speed on both, core 1
  22-25% busy on the Tab5 and 46-48% on the MaTouch (before the core's speed
  work). Checked then: the setup screen's rows, upload and insert through
  the web API, a blank 32 MB hard disk made on the web page and seen by DOS,
  typing through the console, both cards' pictures on the panel.

## Lessons (each one cost a debugging session)

- **Memory the page table maps must be 4 KB aligned**: its entries are
  host addresses with flags in the low bits. The VGA's planes from a plain
  PSRAM calloc (aligned on the host by chance) corrupted the S3's heap the
  moment mode 13h mapped them; `pc_calloc_pages()`.
- **The S3's internal RAM: no big statics**: the EGA/VGA registers in five
  picture copies, an 8 KB disk buffer and an 8 KB font buffer (~25 KB of
  .bss) left 16 KB and broke the start; they're PSRAM allocations now.
- **Drawing runs on more than one task** (the presenter, the console's
  screenshots, the debugger): its state between lines (the VGA's line
  buffer, the per-picture caches) needs `render_lock`.
- **Ports 70h/71h are in the 60h-7Fh block**: the decode sent them to the
  keyboard controller, unnoticed while only the BIOS (in C) read the CMOS.
- **Real BIOSes test the chips**: GLaBIOS wanted DMA channel 0 to count
  (the XT's refresh), the keyboard reset with its clock low from power-on,
  port A 0 after a clear, the 765's interrupt to drop at each SENSE
  INTERRUPT (IBM's XT BIOS asks once after a reset), empty drives ready,
  and the video ROM to set the equipment word's display bits.
- **Clocks for programs that time themselves against the RTC** (Minix's
  readclock): the host build runs ~200x real time, so the RTC's seconds
  are the board's or the machine's own, whichever is later.
- **What made the x86 core fast** (both boards; profile first: the
  console's `profile`, and by source line through addr2line): one run loop
  per model with everything inlined (`always_inline`, the slow paths
  `noinline` and out of the loop); the instruction's bytes through a host
  pointer held in registers, IP stored once per instruction; segment
  windows instead of a page table walk per access; the 286's near jumps
  inside the code window just moving that pointer.
- **Keep the code pointer's struct from escaping**: passing its address to
  a function that isn't inlined (`fetch_slow()`) puts it in memory for the
  whole loop; passing a copy at each of the ~80 inlined fetch sites made a
  2 KB stack frame and 10 KB more code. It hands its results back through
  `cpu->fetch[]`.
- **Specialised copies beat shared code here**: decoding the ModR/M once
  before the switch made the loop 45% smaller and both boards ~10% slower;
  keeping the run's clocks in a register gave nothing on the P4 and lost on
  the S3 (register pressure).
- **Segment windows must be lazy and cached**: worked out at every segment
  load, FreeCOM's INT 16h polling (CS changes twice per BIOS call) spent
  19% of the S3 in `seg_window()`.
- **The S3's cores share their caches**: core 0's work slows core 1's
  emulation, mostly through the 32 KB instruction cache: the presenter's
  drawing (its loop, the scaling, the card's line renderer) running from
  flash/PSRAM evicted the x86 interpreter's code. In internal RAM
  (`PRESENT_HOT` in esp32emu's emu.c, `PC_DRAW_HOT` here; ~3.6 KB) the
  game gained 3-6%. A bigger data cache (64 KB) would need 32 KB of
  internal RAM the S3 hasn't got.
- **emu8950's rate converter uses doubles per sample**: run it at the
  chip's own rate and resample in fixed point (neither the S3 nor the P4
  has a double FPU).
- **A partition's end in the MBR must match its size**: FreeDOS warned
  "suspect partition" when the folder disk's partition ended at its last
  cluster but the CHS end at the cylinder's; both are worked out from the
  last sector now.
- **The S3's instruction cache is its limit for BIOS-heavy code**: with
  16 KB, FreeCOM's loop ran at 85-100% depending on the build (where the
  functions landed); with 32 KB at 69% of core 1. Measure with real
  software too: the synthetic benchmark hardly showed it.

## Open items (the phases in docs/architecture.md)

- Of the architecture's "checked by" list: Commander Keen 1 and 4,
  Wolfenstein 3D, Duke Nukem 1 and OpenGEM (FreeGEM) run; not Windows
  3.0's standard mode (not available), nor a real AT BIOS ROM (none is
  freely available; the option is in).
- Not checked by eye, ear or hand: the panel's picture by eye (only LCD
  captures), keys from a real keyboard, a real Bluetooth mouse (touch and
  the host's scripted mouse were), the speaker's, AdLib's and Sound
  Blaster's sound on the boards (the host's WAV files were analysed), the
  setup screen's coprocessor buttons by touch, printing from a real
  program beyond DOS's PRN.
- The MaTouch runs Wolfenstein 3D at 83-93% (core 1 at its limit).
- The folder drives are read only; the coprocessor's transcendental
  functions have the doubles' 53 bits; no Sound Blaster Pro or 16, no MPU-401; the 765's
  track-level copy protection.

## Working with the user

- Expects changes built (both boards), flashed and checked on the board,
  then a short report of what was verified and what wasn't.
- Keep their private data (WiFi name, IP, SD card contents) out of anything
  meant for publishing.
