# Building

The sources here are an overlay for the Realtek RTL8762D HMI SDK (not vendored here).

1. Copy the SDK tree somewhere (the build writes into it).
2. Copy `firmware/overlay/*` over it (`src/app/silent_ota/*` and `board/evb_stack_img/silent_ota_gcc/Makefile`).
3. `make` in `board/evb_stack_img/silent_ota_gcc` (needs `arm-none-eabi-gcc`). Output: `bin/app_MP.bin`. The app slot is `0x25000` (151552 B).
4. Flash wirelessly: `bash tools/run.sh ota_flash --go path/to/app_MP.bin`.

Compiler flags in the Makefile: `-O1`, hard float, nano.specs, `-std=c99`. `APP_GLOBAL_SIZE` is 48 KB of RAM.
