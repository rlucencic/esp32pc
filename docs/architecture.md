# esp32pc: architecture review

Phase 1 of the brief: what to build, from what, and in what order. Checked
against esp32emu at `16e1711` (2026-10-19) and the siblings that use it.
The decisions below were agreed; progress is at the end.

## Summary

| | Proposal |
|---|---|
| Framework | All of esp32emu's runtime, as the other machines use it; `upd765` extended for the PC; a new x86 core in esp32emu |
| CPU | Our own core (MIT): one core, three models (8088, 8086, 80286), checked against the SingleStepTests suites |
| BIOS | Our own, built in (MIT): a ROM made at start with the standard entry points, tables and fonts, its services in C behind a trap opcode honoured only inside the ROM (the ST's cartridge idea). Real BIOS ROMs from the SD card later, as an option |
| Machines | An XT board and an AT board, each its own set of chips; the profile picks the board |
| Graphics | Four adapter modules (CGA, Hercules, EGA, VGA); EGA and VGA share only the planar memory logic; pictures drawn on core 0 from copies taken at vertical retrace (esp32arc's picture copies) |
| Memory | One PSRAM block: the first megabyte laid out as in a PC, then the extended memory; 4 KB pages; EGA/VGA memory separate |
| Licence | MIT, like the rest: no GPL code |
| First milestone | A host build of an XT (8088, MDA/CGA text, built-in BIOS) booting FreeDOS from a floppy image to the prompt; then the same on both boards |

## 1. What esp32emu provides (Q1)

**Used as it is**, like the twelve other machines:

- `emu_run()`: boot order, the emulation task on core 1 (`run_frame()` per
  video frame, paced by the clock or the speaker), stats, pausing, restart,
  `emu_reboot_into_machine()` for memory that only fits at power-on.
- Presenter: `render_line()` in RGB565 at the machine's resolution (up to
  1024 wide), scaled to `view_width` (800 = 4:3, 960 x 720 on the Tab5)
  when `screen_signature()` changes; only changed rows are written.
- Setup screen: up to four choice rows (the CoCo's Model, CPU, Memory,
  Monitor is the template), rows whose buttons follow another choice
  (`choice_names()`), two-line buttons, the drive and image pages.
- `disks.c`: one folder of images, up to five drives, NVS memory of what
  is inserted, blank images, `accepts()`/`takes_blank()` for drives of
  different kinds; the web page's upload, download, delete and insert, and
  `machine.js` converters.
- Speaker (MAX98357A, ES8388) through `audio_render()`/`audio_pending()`;
  keys as HID usages from Bluetooth, USB (Tab5) and the Tab5 Keyboard;
  mice; touch.
- Console (screen dumps, touches, the profiler `p`), JSON status, SNTP and
  the Tab5's RTC (the PC's clock), the board drivers, exFAT.
- emudbg: protocol and server; it needs an x86 adapter (below).

**Extended:**

- `upd765`: the PC's floppy controller is the same chip (Intel 8272A = NEC
  uPD765A). Today it is non-DMA, without terminal count, two drives,
  DSK/EDSK images. The PC needs DMA (channel 2), IRQ 6, terminal count,
  the digital output register (3F2h, four drives, motors), the data rate
  register (3F7h on the AT) and raw sector images. Added as options; the
  CPC and the +3 keep their behaviour (both rebuilt and checked).
- Possibly the presenter's scaling: it is per machine (`emu_scale_t`);
  720 x 400 text onto 800 x 600 may look better filtered than nearest. A
  per-mode choice in `emu_screen_t` is a small change (all machines
  rebuilt).

**New:** `components/x86` in esp32emu (core, disassembler, debugger
adapter, host tests), and x86 in esp32emu-vscode (segment:offset
addresses, registers).

**Patterns from the siblings:** the ST's cartridge (native opcodes honoured
only in its ROM) for the BIOS; esp32arc's picture copies, line batching and
event cache, and its lesson that host MIPS don't predict the board; the
ST's memory reserved first and capped to what fits; the 65C816 and ARM2
cores' 4 KB page tables, cached code page, jump tables in internal RAM
(`linker.lf`) and small hot code for the S3's instruction cache.

## 2. The CPU core (Q2, Q3)

| Core | Licence | CPUs | Why not |
|---|---|---|---|
| 8086tiny, FabGL's `i8086.cpp` (based on it) | MIT | 8086/80186 | No 286, no 8088/8086 differences, approximate flags and undefined opcodes |
| Fake86, Faux86 | GPL-2.0 | 8086/80186 | GPL |
| PCE | GPL-2.0 | 8086 to a partial 80286 | GPL |
| 86Box, PCem, DOSBox | GPL-2.0 | 8088 and up | GPL, large |
| Bochs | LGPL-2.1 | 386 and up | No 286, C++, large |
| tiny386 | BSD-3-Clause | 386 to 686 | A 386: no 8086 quirks or 286 differences. A useful reference for AT peripherals |
| MartyPC | MIT | 8088, cycle exact | Rust; cycle exactness costs far more than these boards have. A reference for 8088 behaviour |

**Proposal: write the core for this project (MIT)**, as with the ARM2: no
candidate is both licence-compatible and right for all three CPUs, and the
hot loop has to be shaped for the S3 and the P4 anyway.

- **One core, the model chosen at reset.** The models differ in few
  places, all covered by the test suites:
  - `0F`: POP CS on the 8088/8086; the 286's two-byte opcodes (LGDT ...
    LOADALL).
  - On the 8086, `60-6F` alias `70-7F`, `C0/C1` alias `C2/C3`, `C8/C9`
    alias `CA/CB`; on the 286 they are PUSHA, POPA, BOUND, PUSH/IMUL
    immediate, INS/OUTS, shifts by an immediate, ENTER/LEAVE.
  - Shift counts: all 8 bits on the 8086, 5 bits on the 286. PUSH SP: the
    decremented value on the 8086, the old one on the 286. FLAGS bits
    12-15: always set on the 8086, clear in the 286's real mode. Programs
    tell the CPUs apart by exactly these.
  - Undefined opcodes: the 8086 does something (the suites record what),
    the 286 raises #UD (interrupt 6). Divide error: the 8086 returns after
    the DIV, the 286 to it. A word at offset FFFF wraps within the segment
    on the 8086, raises interrupt 13 on the 286.
  - 8088 against 8086: bus width (on the 8088 every word takes two bus
    cycles, on the 8086 only words at odd addresses do) and the prefetch
    queue (4 against 6 bytes). Programs detect the queue length with
    self-modifying code a few bytes ahead; that is modelled by keeping the
    bytes a write overwrote within the next 4 or 6 code bytes (a compare
    per write), not with a full queue.
- **Interpreter:** dispatch on the opcode byte (the 286's `0F` page
  second), one ModR/M decoder, handlers sharing ALU helpers, lazy flags
  (result, operands and operation kept; flags worked out only when read:
  Jcc, PUSHF, INT, LAHF). Memory through 4 KB pages (host pointer plus
  read-only/slow bits, the rest to bus callbacks), the code page cached,
  A20 applied to the address on the AT.
- **286 protected mode** after real mode: descriptor caches for every
  segment (in real mode base = selector x 16, limit FFFF, so one address
  path for both modes); GDT, LDT, IDT; selector and privilege checks;
  gates; TSS task switches; exceptions with error codes, double fault and
  shutdown (which on the AT resets the CPU); LMSW, SMSW, CLTS; LOADALL
  (`0F 05`).
- **Time in CPU clocks:** documented clocks per instruction, EA clocks,
  bus-width penalties, a fetch term for the 8088 (it spends much of its
  time waiting for its 8-bit bus), DRAM refresh, the AT's wait state;
  calibrated so the usual XT and AT speed tests read like the real
  machines. Not cycle exact: demos that depend on it (8088 MPH) won't run
  right.
- A trap opcode for the BIOS, honoured only with CS:IP inside the BIOS ROM;
  a host hook for tracing; the debugger's map test, as in the other cores.
- **Tests** (`make -C components/x86/test`): SingleStepTests 8088, 8086 and
  80286 (real mode). They are hardware-generated, check registers, memory
  and flags after single instructions, and are MIT; they are converted once
  from JSON into compact files, like the 65C816's `ssts`, and downloaded by
  a script rather than committed. Own cases cover interrupts, prefixes, the
  queue and protected mode (the 80286 suite has no protected-mode set yet).
  A speed test.

**Speed, to be measured and not promised:** an XT's 8088 runs roughly 0.3
million instructions a second, a 6-8 MHz AT roughly 1-1.5 million, 12 MHz
roughly twice that. For scale, Musashi runs the ST's 8 MHz 68000 at ~80%
of real time on the MaTouch and in ~40% of core 1 on the Tab5. Expected:
the XT at full speed on both boards; a 6-8 MHz AT on the Tab5, perhaps on
the MaTouch; 12 MHz on the Tab5 if anywhere. Each board offers only what
runs at full speed there (as with the ST's CPU row and the A5000).

## 3. Licences (Q4)

- esp32pc is MIT, like esp32emu and the siblings, so no GPL or LGPL code
  goes in.
- MIT/BSD projects (tiny386; TinyEMU and QEMU, the sources of tiny386's
  peripherals, licences to check per file; 8086tiny; MartyPC) are for
  reference. Any code taken keeps its notice.
- **BIOS: no free BIOS exists for a 286 AT.** GLaBIOS and Sergey Kiselev's
  8088 BIOS are GPL-3.0 and XT-only. SeaBIOS (LGPL-3.0) needs a 386. IBM's
  BIOS (including the source reconstructed from the Technical Reference
  listings), Phoenix, Award and AMI are copyrighted. So the BIOS is our own
  and built in. Real ROMs can come later as files the user supplies, never
  distributed (like the ST's TOS).
- **Fonts:** the text modes need 8x8, 8x14, 9x14 (MDA) and 8x16 character
  sets, and the BIOS needs its 8x8 font in memory. IBM's bitmaps are in
  most emulators, but their legal status varies by country. To settle
  before Phase 3: a font under a clear free licence, or the user's
  character ROM dumps.
- DOS for tests: FreeDOS (GPL, free to download, not bundled); the user's
  MS-DOS or PC DOS; Microsoft's MS-DOS 1.25 and 2.0 binaries and 4.0
  sources are MIT (`github.com/microsoft/MS-DOS`). Games and programs for
  tests are the user's, never committed.

## 4. XT and AT hardware (Q5)

Two boards made of separate chip modules, picked by the profile, not one
motherboard switched by the CPU:

| | XT board (8088, 8086) | AT board (80286) |
|---|---|---|
| Interrupts | one 8259, IRQ 0-7 | two 8259s cascaded on IRQ 2 (IRQ 8-15; 9 answers 2) |
| DMA | one 8237 (8-bit; channel 0 refresh, 2 floppy), page registers 81h-83h | two (channel 4 the cascade, 5-7 16-bit), page registers 80h-8Fh |
| Timer | 8253: tick, refresh, speaker | 8254 (read-back command) |
| Keyboard | 8255 PPI: scan codes on 60h, control on 61h, DIP switches on 60h/62h; the 83-key keyboard, set 1, no commands | 8042 (60h/64h): its commands, A20 gate, CPU reset, set 2 translated to set 1; keyboard commands (LEDs, typematic, ID) |
| Clock | none (DOS asks for the date) | MC146818 RTC and CMOS (70h/71h, IRQ 8), filled from the settings and the board's clock |
| Port 61h | PPI port B | speaker gate, refresh toggle (bit 4, used by delay loops), timer 2 output |
| NMI mask | A0h | 70h bit 7 |
| Address space | 1 MB, wraps | 16 MB, A20 gate |
| Floppy | 765, DMA 2, IRQ 6 | the same, plus the data rate register for 1.2/1.44 MB |
| Hard disk | through the BIOS | through the BIOS first; the AT's task-file controller (1F0h-1F7h, IRQ 14) later, for software that bypasses it |
| Reset | the CPU's | also the 8042's pulse and shutdown: the BIOS reads the CMOS shutdown byte (0Fh) and goes on through 40:67 (how 286 programs leave protected mode) |

Both have the PIT at 1.193182 MHz, the PC speaker, the video adapter, a
parallel port status (so printer checks pass), later a serial port.

**Code:** `components/ibmpc/` (platform independent; `host/` the host
build): `pc.c` (frame loop, events), `bus.c` (pages, a table of 1,024 I/O
ports), `pic.c`, `pit.c`, `dma.c`, `ppi.c`, `kbc.c`, `rtc.c`, `fdc.c`
(glue to `upd765`), `hdd.c`, `speaker.c`, `serial.c`, `bios*.c`,
`video/*.c`. `main/main.c` holds the `emu_machine_t`, `main/settings.c`
the NVS settings.

**Frame loop:** one frame of the active adapter (CGA 59.92 Hz, 262
lines; MDA/Hercules 49.8 Hz; EGA 60 Hz; VGA 70 or 60 Hz). The CPU runs to
the next event: timer outputs that raise IRQ 0, keyboard, floppy, RTC,
serial (esp32arc's event cache). Retrace and display-enable status bits
are worked out from the time when read, so polling them needs no events.
The speaker catches up whenever its timer or port 61h changes and at the
frame's end (esp32arc's `sound_run()`).

## 5. Graphics adapters (Q6)

One adapter at a time, each a module with its own ports, memory,
registers, timing and drawing, behind a small interface: reset, port
in/out, memory (mapped pages or callbacks), events at frame and line
batches, mode and signature, and `render_line()` from the picture copy.

- **CGA:** 6845 CRTC, 16 KB at B8000 (mapped directly), mode and colour
  registers (3D8h/3D9h), status (3DAh); 40- and 80-column text, 320 x 200
  in 4 colours (both palettes, intensity, mode 5's), 640 x 200 in 2; the
  even/odd line banks. Composite colour can be added later as another
  renderer of the same copy.
- **Hercules:** the MDA text part (6845 at 3B4h, mode 3B8h, status 3BAh,
  80 x 25 in 9 x 14 cells, underline, intensity, reverse, blink) and the
  Hercules part (720 x 348, four interleaved banks, two pages, the
  configuration switch 3BFh, status bit 7 changing with the vertical
  retrace, which is how software tells a Hercules from an MDA). 64 KB at
  B0000. Two files (`mda.c`, `herc.c`); MDA isn't a setup choice.
- **EGA and VGA:** each has its own registers, CRTC, attribute controller,
  palette and timing; they share only the planar memory logic (four
  planes, latches, write modes 0-2 (VGA 0-3), read modes, set/reset,
  rotate, bit mask, map mask) in `planar.c`.
  - EGA: 256 KB, 64-colour palette, modes 0Dh, 0Eh and 10h, 8 x 14 text,
    registers mostly write-only (as on the card).
  - VGA: the DAC (256 x 18-bit), chain 4 (mode 13h), unchained (mode X),
    mode 12h, 720 x 400 text (9-dot cells, 8 x 16), the mono mode at 3Bxh,
    readable registers, split screen and pel panning. The CGA and EGA
    modes run through its own registers, without switching adapter.
- **Memory** at A0000, B0000 or B8000 by the mode (VGA: the graphics
  controller's memory map). It is mapped directly where the layout is
  linear (CGA, Hercules, text, VGA chain 4) and through callbacks in the
  planar modes, where the write logic is needed anyway.
- **Pictures on core 0.** At vertical retrace a core 0 task copies the
  visible memory and the registers into one of three copies:
  - CGA 16 KB, Hercules 32 KB, mode 13h 64 KB, planar modes up to ~150 KB.
  - This is esp32arc's scheme; without it the Archimedes showed black
    stripes while a game cleared its screen.
  - Registers changed mid-frame (palette, start address, split line,
    panning) are recorded per batch of lines, so raster effects and split
    screens show. Writes racing the beam within a frame don't.
  - `render_line()` decodes the copy into RGB565.
  - Cursor and attribute blinking change the signature.
- **Video BIOS:** for EGA and VGA, a small option ROM at C0000 (55 AA
  header, the fonts at real addresses for INT 10h AX=1130h, the parameter
  tables). The services are in C with the rest of the BIOS.

## 6. Memory (Q7)

- **One PSRAM block**, allocated first in `reserve_memory()`: 1 MB plus the
  extended memory. The first megabyte is laid out as in the PC:
  conventional RAM, CGA/Hercules memory at B0000-BFFFF, option ROM at
  C0000, the BIOS at F0000. The extended memory follows from 1 MB. The
  384 KB above 640 KB costs nothing extra because it holds those.
- EGA/VGA's 256 KB in a block of its own (PSRAM; on the Tab5 internal RAM
  if that measures faster).
- **Pages of 4 KB:** 256 for an XT, 4,096 for a 16 MB AT (16 KB of internal
  RAM; the MaTouch's 4 MB needs 1,024).
- **A20 on the AT:** while it is off, addresses wrap at 1 MB (FFFF:0010 =
  0000:0000); on, the HMA reaches 10FFEFh.
- **Budget, MaTouch:** 8 MB of PSRAM, ~6.7 MB of heap after the code. The
  board side needs ~2 MB (LCD framebuffer 1.23 MB, WiFi, BLE, setup
  screen), leaving about 4 MB for the PC: 640 KB plus up to 3 MB extended,
  VGA memory and copies.
- **Budget, Tab5:** 32 MB, so 640 KB plus 15 MB.
- Floppy images in PSRAM (at most 1.44 MB, like `upd765`). Hard disk images
  go through a small sector cache from the SD card (on the S3, writes from
  an internal DMA buffer).
- **Sizes:**
  - XT: 256, 512 or 640 KB.
  - AT: 640 KB plus 1, 4 or 15 MB on the Tab5, less on the MaTouch (to be
    measured).
  - The brief's "16 MB extended" isn't possible: the 286 addresses 16 MB in
    all, so at most 15 MB above the first.

## 7. The minimum to boot DOS (Q8)

With the built-in BIOS:

- The 8088 core, RAM and ROM.
- The 8259 (IRQ 0 timer, IRQ 1 keyboard).
- 8253 channel 0: the 18.2 Hz tick DOS keeps time with.
- The 8255 and the keyboard.
- A text display: MDA or CGA text, meaning the 6845's start address and
  cursor and memory at B0000/B8000.
- The BIOS interrupts:
  - INT 08h and 09h.
  - INT 10h: modes 3 and 7, cursor, teletype, scroll, characters.
  - INT 11h and 12h.
  - INT 13h: reset, read, write, verify, parameters, change line.
  - INT 15h: minimal.
  - INT 16h.
  - INT 14h and 17h: "no device".
  - INT 19h: the boot sector to 0000:7C00.
  - INT 1Ah.

The floppy controller and DMA aren't needed for booting: INT 13h reads the
image itself, which is fast, as the ST's floppy timing is. They come in
Phase 3, for programs that program them directly.

With a real BIOS ROM (later) the 8237, 765, DIP switches, NMI mask and
refresh are needed too: IBM's POST checks them all, which makes it a good
hardware test.

## 8. Configuration (Q9)

Four rows, the CoCo's layout:

```
Machine     [ PC/XT ] [ PC/AT ] [ Custom ]
Processor   PC/XT:  [ 8088 4.77 MHz ]                       (shown, fixed)
            PC/AT:  [ 80286 6 MHz ] [ 80286 8 MHz ]
            Custom: [ 8088 4.77 MHz ] [ 8086 8 MHz ] [ 80286 8 MHz ] [ 80286 12 MHz ]
Video       [ CGA ] [ Hercules ] [ EGA ] [ VGA ]
Memory      8088/8086: [ 256 KB ] [ 512 KB ] [ 640 KB ]
            80286:     [ 640 KB ] [ 640 KB + 1 MB ] [ + 4 MB ] [ + 15 MB ]
```

- Choosing PC/XT or PC/AT sets the other rows to that machine's defaults:
  8088 4.77 MHz, CGA, 640 KB; or 80286 6 MHz, EGA, 640 KB + 1 MB. That is
  the way back to the defaults (also a console command).
- Video and memory stay free in every profile. Custom adds the 8086 and the
  faster clocks; its board follows the CPU (8088 and 8086 on the XT board,
  the 286 on the AT's).
- Clock lists and memory sizes per board are set by measurement: the
  MaTouch drops what it can't run at full speed.
- NVS namespace `pc`: `machine`, `cpu`, `khz`, `video`, `ram_kb`,
  `ext_kb`. Values are stored explicitly rather than as button indices, so
  the lists can change. Loading validates them: an unknown or impossible
  combination falls back to the profile's defaults and is logged.
- Changes restart the machine (paused meanwhile). More memory than was
  reserved at power-on restarts the board straight into the machine (the
  ST's way). Errors (no memory, no boot disk) go to `emu_set_message()`.
- Drives (`disks_config_t`, one folder):
  - A: and B:, floppies: `.img`, `.ima`, `.vfd`, the size giving the
    geometry (160 KB to 1.44 MB). "New" makes a formatted FAT12 disk.
  - C: (and perhaps D:), hard disks: `.img`, `.hdd`, or fixed `.vhd`
    (geometry from its footer), up to 504 MB, the original INT 13h's
    limit.
  - `accepts()` sends each image to the right drives.

## 9. The first milestone (Q10)

- **M1, host:** `components/x86` with the 8088 model passing SingleStepTests
  8088 (every opcode, documented or not, with flags). `components/ibmpc`
  with the XT board's minimum (8259, 8253, 8255, keyboard), MDA and CGA
  text, and the built-in BIOS. `pc_host` boots FreeDOS (and the user's
  MS-DOS, if available) from a floppy image to `A:\>`, runs DIR and TYPE,
  and writes the screen to a PPM.
- **M2, boards:** esp32pc builds for both targets; the machine fixed to
  PC/XT. A: and B: come from the SD card through the setup screen and the
  web page; the keyboard works; speed and core 1 load are measured on both
  boards.

## 10. Phases

The brief's order, with two changes: the debugger adapter early (it pays
for itself during bring-up), and the floppy controller and DMA together
with the speaker in Phase 3.

| Phase | Work | Checked by |
|---|---|---|
| 2 | x86 core (8088); XT minimum; MDA/CGA text; built-in BIOS; host build; then both boards (setup rows, drives, keyboard) | M1, M2 |
| 3 | CGA complete (modes 4-6, palettes, status timing, mid-frame changes); speaker; 8237 and 765 through `upd765`; disassembler, debugger adapter and extension | CGA games; Trixter's CGA Compatibility Tester; stepping through DOS |
| 4 | Hercules and MDA text | text programs, Hercules graphics, adapter detection |
| 5 | 8086 model; the Processor row and its persistence | SingleStepTests 8086; queue-length detection |
| 6 | AT board and 286 real mode; hard disk C:; then protected mode | SingleStepTests 80286; HIMEM.SYS; Windows 3.0 in standard mode |
| 7 | EGA | EGA games (Commander Keen episode 1, shareware); FreeGEM |
| 8 | VGA | mode 13h, mode X and mode 12h games (Wolfenstein 3D shareware, a 286 and VGA game) |
| 9 | Setup and compatibility; serial mouse on COM1 from the Bluetooth/USB mice and touch; real BIOS ROMs as an option; README, CLAUDE.md | the profiles, the saved settings, the documented limitations |
| 10 | Optimisation from the board profiler | measured on both boards |

Each phase ends built for both boards, flashed and checked, with the
measurements written down.

## 11. Risks and open questions

- The AT's speed on the MaTouch is unknown until Phase 6; that build may
  offer less.
- Planar EGA/VGA writes go through callbacks: games that draw a lot in
  modes 12h or X will show the cost.
- The S3's instruction cache against the size of an x86 interpreter (the
  65C816's lesson: forced inlining to 72 KB, ~850 cycles an instruction).
- 286 protected mode has no hardware test suite yet.
- The fonts' licence (before Phase 3).
- Copy-protected originals need the 765's track-level behaviour, outside
  the first scope.
- Speed-sensitive demos won't run right (not cycle exact).
- Not in the brief but worth having:
  - A serial mouse, so GEM and Windows work with the framework's mice.
  - A way to get files in besides disk images: the web page building a FAT
    image from dropped files (`machine.js`), or an SD card folder as a
    drive through DOS's redirector interface (like the ST's C:).

## Needs a decision

1. The built-in BIOS (ours), with real ROMs only as a later option.
2. Our own CPU core.
3. The setup rows and the Processor lists above.
4. MIT for esp32pc.

## Progress

- **Phase 2** (2026-10-19): the x86 core (`../esp32emu/components/x86`)
  passes the SingleStepTests 8088 suite (3,007,000 tests) and the 8086's
  (646,000), undefined flags included; the PC/XT (8259, 8253, 8255 and
  keyboard), CGA (text and both graphics modes) and the Hercules card's
  MDA text, the built-in BIOS, floppy and hard disk images (both on the XT,
  blank ones formatted, 10-128 MB hard disks). FreeDOS 1.3 boots to its
  installer and the DOS prompt on the host build and on the Tab5 (full
  speed, core 1 a quarter busy) and the MaTouch (full speed, core 1 about
  half busy). The speaker came forward from Phase 3 (it also paces the
  emulation by the sound's clock). Changes from the plan: hard disks on the
  XT as well (asked for);
  INT 1Ah's clock functions on the XT too, from the board's clock (as a
  clock card would), so DOS has the date.
- **Phase 6, real mode** (2026-10-20): the PC/AT: the 286 model (its real
  mode suite passes but for 17 of 1,478,000 tests), extended memory, the
  8042 with the A20 gate and CPU reset, the CMOS clock, the second 8259,
  one wait state, INT 15h's block move and memory size, the shutdown byte.
  FreeDOS boots on it and MEM sees the extended memory. (Protected mode
  followed: below.)
- **The core's speed** (2026-10-20): the first AT ran at full speed on the
  Tab5 only just (6 MHz, core 1 83%) and at 60-73% on the MaTouch. Now one
  run loop per model with everything inlined, the code read through a
  pointer in registers, direct segment windows instead of the page table
  per access, REP MOVS/STOS in bulk on the 286, and on the MaTouch the
  32 KB instruction cache (the page table moved to PSRAM for it). Clock
  lists as measured (section 8's rule): the Tab5 keeps them all (the 286
  at 12 MHz: core 1 70-84%); the MaTouch offers the AT at 6 MHz (core 1
  68-82%, its 8 MHz came to 97-99% of real time) and the 8086 at 8 MHz in
  Custom, not the 286s there.

- **Phases 3-5** (2026-10-20): CGA mode and palette changes in the middle
  of the picture; the 8237s and the 765 (through esp32emu's `upd765`, with
  DMA, IRQ 6 and four units) for programs that drive them; the debugger's
  x86 adapter and the VS Code extension's x86 support; Hercules graphics;
  the 8088/8086 prefetch queue against self-modifying code, so programs
  tell the two apart (queue-length detection: the 8088 shows 0 1 1, the
  8086 0 0 1).
- **Phase 6, protected mode** (2026-10-20): descriptor tables, limits and
  access rights folded into the segments' direct windows (real mode's speed
  unchanged), the four privilege levels, call/interrupt/trap/task gates,
  task switches, exceptions with error codes, the double fault and the
  shutdown, IOPL, LLDT/LTR/LAR/LSL/VERR/VERW/ARPL, LOADALL (real mode's
  "unreal" segments too); INT 15h AH=89h; the AT's hard disk controller
  (WD1003/IDE task file, IRQ 14). Checked by a test program (18 checks,
  ending back in real mode through a triple fault and the shutdown byte)
  and Minix 2.0.4: its 16-bit kernel in protected mode, its floppy and AT
  disk drivers, multiuser, a shell. The 8254's read-back was in already.
  Not checked: HIMEM.SYS's LOADALL and Windows 3.0 (not available).
- **Phases 7-8, EGA and VGA** (2026-10-20): one module for both: 256 KB of
  planes interleaved so that chain-4 is linear, the planar logic in the
  memory callbacks (mode 13h mapped directly), drawing from the registers
  as the hardware generates the picture (CRTC addressing, split screen,
  panning, double scanning, the attribute controller, the DAC; the EGA's
  6-bit colours). Their BIOS with a video ROM at C0000h (fonts, the
  cards' parameter tables, modes 0-13h, AH=10h-1Ch). Checked by test
  programs (modes 13h, 12h, X, 10h, 0Dh, 4, split and panning, 43/50-line
  text, the DAC) on the host and both boards; the VGA's text costs no speed
  on either board. Commander Keen 1 (shareware) runs on the EGA at full
  speed on both boards (the MaTouch's core 1 up to 97% in its intro);
  Wolfenstein 3D not tried.
- **Phase 9** (2026-10-20): COM1 (8250) with a Microsoft mouse fed by the
  Bluetooth/USB mice and touch (a drag moves, a tap and a long press
  click); INT 14h; the Video row with four cards and the AT's EGA default;
  a BIOS row: a real ROM (`pcxt.rom`/`pcat.rom` with the disk images)
  instead of ours, which then become option ROMs (the hard disks' at
  C8000h, the EGA/VGA's at C0000h). GLaBIOS passes its POST (after the XT's
  refresh on DMA channel 0, the keyboard's power-on reset, the 765's
  interrupt after a reset and the ready lines were put right) and boots
  floppies and hard disks on the host and the MaTouch. Not done then: the
  printer port (below).
- **Beyond the phases** (2026-10-20): sound cards: the AdLib (emu8950's
  OPL2 on the speaker's task, its timers on the emulation's time) and the
  Sound Blaster 2.0's DSP (DMA, IRQ 7); Wolfenstein 3D (shareware 1.4)
  plays with both, Commander Keen 4 with its music. Phase 10's
  optimisation, measured on the MaTouch in Wolfenstein's 3D view (70-84%
  of real time): fast paths for the VGA's plain write and read modes, the
  card's callbacks straight on the CPU's bus, the picture copy and its
  hash in one pass: 78-90%. The rest is the S3's shared cache (core 0's
  picture work; 89-90% with it stopped). Files without disk images: the
  web page packs dropped files and folders onto a FAT image (the risks'
  `machine.js` idea), and folders on the SD card go into C:/D: as
  read-only FAT16 disks made up per sector (instead of DOS's redirector:
  no DOS-side driver needed, any DOS sees them). The printer port (LPT1
  to a file on the card, INT 17h, Print Screen) and the 8087/287
  coprocessor (a choice on the XT and AT, always in Custom; GLaBIOS finds
  it; its FADD/FSUB/FMUL/FDIV/FSQRT exact to the last bit, its registers
  in the debugger). OpenGEM (FreeGEM) runs with CTMOUSE; Duke Nukem 1 too;
  FDXMS286 gives XMS and the HMA (the risks' HIMEM.SYS check, with
  FreeDOS's driver). Later the presenter's drawing went into the S3's
  internal RAM (its code had taken the shared instruction cache from the
  emulation): Wolfenstein's 3D view 83-88% on the MaTouch.
- **Setup screen, as asked** (2026-10-20): the Custom machine is gone (the
  8086 at 8 MHz went to the PC/XT, the Tab5's 12 MHz 286 to the PC/AT),
  the coprocessor is a row of its own (Off/On), and the BIOS row with it
  (the built-in BIOS stays; another would be built into the firmware):
  five rows, Machine, Processor, Coprocessor, Video, Memory. Section 8's
  table is the original plan.

## Sources

- [SingleStepTests 8088](https://github.com/SingleStepTests/8088), [8086](https://github.com/SingleStepTests/8086), [80286](https://github.com/SingleStepTests/80286) (MIT)
- [GLaBIOS](https://github.com/640-KB/GLaBIOS) (GPL-3.0), [Sergey Kiselev's 8088 BIOS](https://github.com/skiselev/8088_bios) (GPL-3.0)
- [Minix 2.0.4](https://www.minix3.org/previous-versions/Intel-2.0.4/) (BSD): its i86 floppies as the protected mode check
- [tiny386](https://github.com/hchunhui/tiny386) (BSD-3-Clause), [8086tiny](https://github.com/adriancable/8086tiny) (MIT), [FabGL](https://github.com/fdivitto/FabGL) (`src/emudevs/i8086.cpp`: MIT, based on 8086tiny), [MartyPC](https://github.com/dbalsom/martypc) (MIT)
- [Faux86](https://github.com/jhhoward/Faux86), [Faux86-remake](https://github.com/ArnoldUK/Faux86-remake), [86Box](https://github.com/86Box/86Box) (GPL-2.0)
- [Microsoft MS-DOS](https://github.com/microsoft/MS-DOS) (MIT)
- [Reconstructed IBM PC BIOS sources](https://github.com/Gawlas/IBM-PC-BIOS), [VOGONS on AT BIOS alternatives](https://www.vogons.org/viewtopic.php?p=1071938)
