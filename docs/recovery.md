# Recovering the watch when Bluetooth is dead

Written 2026-11-03 after a build with changed Bluetooth start-up (two links, an oversized advertising reply, a second GATT client)
left the watch not advertising. Wireless update is the only route in while the app runs, so this is the route that does not depend on the app.

## What the board offers (from photos of the MOY.M80182.03 board)
- Top right, in a bracket next to the heart-rate flex: pads `TX`, `RX` and a large `G` (ground).
- Middle right: pads `TXD`, `G`, `G`, and a column of four small holes marked `G`, `V` and two unmarked (a jig header).
- A 5-stripe gold strip numbered 1 to 5 near the middle (a test-jig contact).
- Not identified: which pad, if any, is the chip's boot-select pin (P0_3, the "LOG" pin).

## The mechanism (documented by Realtek for the RTL87x2 family; not yet confirmed on this exact board)
If the boot-select pin is held low when the chip powers up, it skips the flash and runs its ROM loader, which accepts images over the UART.
My firmware cannot affect this. Tools: Realtek's Windows MP Tool (certain to know the chip), or the open-source
`rtkprog` (RTL87x2x/87x2G) and `rtltool` (RTL8762C), which may or may not speak the RTL8762D variant.

## What to write
Only the app region was ever overwritten, so the secure boot, patch and upper-stack images are still in flash.
Write `firmware/images/fob_0.4.bin` (or `firmware/baseline/app_MP.bin`) back at the app address (0x82C000 on this layout; confirm against a read-back of the flash before writing).

## Needs
- 3.3 V USB-to-UART adapter (CP2102 type), jumper wires, a way to touch the pads (spring probes, or fine wires soldered by a repair shop).
- Power: the battery or charger connected during the download.
- A multimeter, or a shop, to find which pad reaches the boot-select pin.

## Rules for next time
Never flash a Bluetooth start-up change without a boot-time fallback (see README).
