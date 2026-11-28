# loadStreamAux
The [loadStream](../loadStream/README.md) load test, moved to GP26 and GP27 using the RP2350's UART_AUX pin function.

## UART_AUX
GP26 and GP27 are normally UART1's CTS and RTS pins. On the RP2350, pin function 11 (UART_AUX) turns a UART's CTS and RTS pins into TX and RX, so UART1 can be used on these pins:

| Pin | UART (F2) | UART_AUX (F11) |
|---|---|---|
| GP26 | UART1 CTS | UART1 TX |
| GP27 | UART1 RTS | UART1 RX |

In code the only change is the pin function:
```c
gpio_set_function(26, GPIO_FUNC_UART_AUX);
gpio_set_function(27, GPIO_FUNC_UART_AUX);
```
It is still UART1, so the interrupt, metrics, test and analysis are the same as loadStream.

This is Pico 2 only, as the RP2040 has no UART_AUX function.

## Wiring
| Function | Pins | Notes |
|---|---|---|
| Test UART | GP26 (TX), GP27 (RX) | UART1 via UART_AUX, 8N1, no flow control |
| OLED display | GP12 (SDA), GP13 (SCL) | SSD1306 128x64 on I2C0, address 0x3C |
| stdio console | GP16 (TX), GP17 (RX) | UART0, 115200 baud |

Connect to the [echo](../echo/README.md) server, TX to RX both ways, with a common ground:

| loadStreamAux | echo |
|---|---|
| GP26 (TX) | GP21 (RX) |
| GP27 (RX) | GP20 (TX) |
| GND | GND |

## Test, display and console
These are the same as [loadStream](../loadStream/README.md#the-test).

## Building
The build target is `LoadStreamAux`. See the [top-level README](../../README.md) for cloning and building.
