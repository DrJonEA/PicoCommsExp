# echo
An echo server on UART1. Every character received is sent straight back out. It is the server end for the [loadStream](../loadStream/README.md), [loadStreamAux](../loadStreamAux/README.md) and [loadStreamPio](../loadStreamPio/README.md) load tests.

## Wiring
| Function | Pins | Notes |
|---|---|---|
| Echo UART | GP20 (TX), GP21 (RX) | UART1, 8N1, no flow control |
| OLED display | GP12 (SDA), GP13 (SCL) | SSD1306 128x64 on I2C0, address 0x3C |
| stdio console | GP16 (TX), GP17 (RX) | UART0, 115200 baud |

To pair it with a load test, cross TX and RX and connect the grounds. See the README of each load test for its pins.

## How it works
Receiving is interrupt driven. The interrupt reads every character waiting in the UART, sends each one straight back out and adds it to a 10-character history.

The interrupt reads the UART data register directly, not through `uart_getc`, so it can see the error flags that come with each character:

| Metric | Meaning |
|---|---|
| Echoed | Bytes sent back out |
| Overrun | The receive FIFO was full and data was lost before this character. The character itself is good, so it is echoed |
| Framing | The stop bit was missing |
| Break | The RX line was held low |
| Parity | Never seen, as parity checking is not turned on |

Characters with a framing, break or parity error are counted but not echoed, and not added to the history. A break, for example, arrives as a bogus `0x00`.

The main loop never turns interrupts off, so no data is lost while it reads the history and metrics. The values on the display can be from slightly different moments, but each one is a 32-bit value that is read in one go.

## Display
The display refreshes every 0.5 seconds:
```
hello worl      last 10 characters received
E:1234567       bytes echoed
O:0   F:0       overrun     framing
B:0   P:0       break       parity
```
Characters that can't be printed, such as CR and LF, show as `.`. Counts over 999 in the error columns show as `1k+`.

## Baud rate menu
The menu on the stdio console selects the baud rate with a single key:

| Key | Baud | Key | Baud |
|---|---|---|---|
| 0 | 9600 | 9 | 2M |
| 1 | 19200 | a | 3M |
| 2 | 38400 | b | 4M |
| 3 | 57600 | c | 5M |
| 4 | 115200 (default) | d | 6M |
| 5 | 230400 | e | 7M |
| 6 | 460800 | f | 8M |
| 7 | 921600 | g | 9.375M |
| 8 | 1M | | |

9.375 Mbps is the UART maximum, the Pico 2's 150 MHz peripheral clock divided by 16. Any other key prints the menu again.

Changing rate resets the UART, prints the requested and actual baud rate, and clears the metrics and the history.

## Building
The build target is `Oled`. See the [top-level README](../../README.md) for cloning and building.
