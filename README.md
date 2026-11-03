# fob-os

Firmware for **fob**, a carry device built from scratch on the board of a Fire-Boltt BSW051 smartwatch
(Realtek RTL8762D, 368 × 448 AMOLED, crown, touch, haptic motor, Bluetooth LE).

Not a watch: a crown-driven dial of modes with an ambient resting screen, meant to hang from a belt loop, a bag or a set of keys.
The design book (sketches, architecture, interaction, motion, every screen) lives with the portfolio:
**onkaryaglewad.in/fob/book.html**.

## Status

This repository starts from the hardware work that is already proven and replaces the interface on top of it.

| | |
| --- | --- |
| Carried over, proven on the device | panel power-up and init, quad-SPI + DMA streaming, vector renderer (≈25 ms full redraw), touch, crown (software decoder), haptic pattern engine, BLE + wireless updates, watchdog and boot back-off |
| Built here (fob 0.1) | the shell: boot, Carry with four scenes, the dial of nine modes, per-mode memory, double-press, settings, Archivo type, one signal colour |
| Still to build | the modes themselves (cards, music, inbox, remote, claude, play, tools), pairing, packs, the pet, the beluga boot |

See [`docs/roadmap.md`](docs/roadmap.md) for the order.

## Layout

```
docs/                 hardware facts, architecture, roadmap
firmware/
  overlay/            sources that go on top of the Realtek RTL8762D HMI SDK (see firmware/BUILD.md)
  baseline/app_MP.bin last known-good image (v19), flashable for recovery
sim/                  laptop simulator: the same renderer + UI sources, PPM screenshots
tools/                wireless flashing and control over Bluetooth (run.sh, ota_flash.py, os_ctl.py, os_rescue.py, tool5.py)
```

## Hard rules

- Never touch pins P1_3 and P1_4 (they freeze the chip).
- Never switch the OLED supply (P2_5) on hard, and never unconditionally at boot: a boot loop would end wireless flashing. Use the PWM soft start and the boot back-off.
- Bluetooth and the update path must work in every build. It is the only way back in.
- The screen's four data lines (D0..D3) must all be claimed; two floating lines caused the early colour corruption.

## Flashing

```
bash tools/run.sh ota_flash --go $PWD/firmware/images/fob_0.1.bin      # recovery: firmware/baseline/app_MP.bin (the v19 watch UI)
bash tools/run.sh os_ctl
```

Needs Python with `bleak` and `pycryptodome` in `~/watchenv`, and the watch on its charger or battery.
