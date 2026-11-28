# oledTest
Bring-up of the OLED display, and the starting point for the other experiments.

## What it does
On start it shows "Testing" with a line underneath. It then loops, every 0.5 seconds:
- LED on, "Testing" and "Blink" on the display
- LED off, "Testing" only

Use it to check the display wiring before moving on to the UART experiments.

## Wiring
| Function | Pins | Notes |
|---|---|---|
| OLED display | GP12 (SDA), GP13 (SCL) | SSD1306 128x64 on I2C0, address 0x3C |
| LED | GP2 | With a suitable resistor to GND |
| stdio console | GP16 (TX), GP17 (RX) | UART0, 115200 baud. Prints "GO" on start, and "Oled Init failed" if the display isn't found |

## Building
The build target is `Oled`. See the [top-level README](../../README.md) for cloning and building.
