# esp32pc roadmap

Proposed work in priority order. See [Building](README.md#building) and the [architecture notes](docs/architecture.md) for the current project.

1. **Run a Windows 3.0 Standard Mode smoke test.** With a local installer, boot
   the 286 AT profile and use the emulated VGA and mouse to reach Program
   Manager and run a stock app. Record the first protected-mode or BIOS
   discrepancy if it stalls. Preserve the existing `pm` and `pm89` boot tests
   as controls.

2. **Profile the MaTouch during Wolfenstein 3D gameplay.** Repeat a fixed
   shareware gameplay sequence with PC speaker, AdLib and Sound Blaster output
   in turn. Measure guest pace and PCM underruns, separate CPU from VGA and DMA
   costs, and compare screenshots and audio before keeping an optimization.

3. **Validate the external AT BIOS path.** Use `PC_BIOS` in
   `components/ibmpc/host` with a locally supplied compatible ROM. Check POST,
   keyboard controller and disk enumeration, and compare a floppy and hard-disk
   boot with the built-in BIOS path. Record BIOS model and checksum without
   adding the ROM to Git.
