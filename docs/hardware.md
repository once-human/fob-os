# Hardware facts (confirmed on the device)

| Part | Detail |
| --- | --- |
| SoC | Realtek RTL8762D, Cortex-M4F (FPU), ~90 MHz, BLE 5 |
| Panel | 368 × 448 AMOLED, RGB565, quad-SPI on SPIC2: CLK P4_0, CS P4_3, D0 P4_2, D1 P4_1, D2 P0_6, D3 P0_7. Tearing-effect line on P3_0. |
| Panel power | P0_1 logic (soft start), P2_0 reset, P2_5 OLED high voltage (TIM5 PWM ramp only) |
| Pixel writes | window commands 2A/2B, then header `32 00 2C 00` and quad data by GDMA channel 1; each 12-row strip is its own window |
| Touch | Hynitron CHSC5816 at I2C 0x2E, SCL P0_5, SDA P1_2 (bit-banged), interrupt P2_4 (low while touched) |
| Crown | rotation on P1_0 / P2_6 decoded in software at 1 kHz (the QDEC block reads nothing on this board); the push switch is P3_1 (low = pressed) |
| Haptics | motor on P0_0 (low = on), 25 kHz PWM on TIM2, 1 kHz pattern tick on TIM3 |
| Battery | ADC reading of VBAT; charger-only supply reads ~3.29 V and sags to ~3.1 V during the P2_5 ramp |

## Power-up order

1. SPIC2 pins and clocks (all four data lines), 2. P0_1 soft start, 3. P2_0 reset pulse, 4. the 22-command init table, 5. display on, brightness 0,
6. clear the panel memory black, 7. P2_5 PWM ramp in slow steps (500 ms steps on a weak charger), 8. fade brightness up.

## Lessons that cost time

- Greys came out green and edges blue until D2/D3 were claimed.
- A hard switch-on of P2_5 resets the chip. On a weak charger even the ramp needs slow steps.
- Never read a peripheral whose clock is off (the QDEC read froze the chip and caused a boot loop).
- Stream mode that renders ahead of the DMA can run the FIFO dry and corrupt the frame; keep one window per strip.
