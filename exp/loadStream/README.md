# loadStream
A load test for UART1, run against the [echo](../echo/README.md) server on a second Pico.

[loadStreamAux](../loadStreamAux/README.md) and [loadStreamPio](../loadStreamPio/README.md) run the same test on different pins and hardware.

## Wiring
| Function | Pins | Notes |
|---|---|---|
| Test UART | GP20 (TX), GP21 (RX) | UART1, 8N1, no flow control |
| OLED display | GP12 (SDA), GP13 (SCL) | SSD1306 128x64 on I2C0, address 0x3C |
| stdio console | GP16 (TX), GP17 (RX) | UART0, 115200 baud |

Connect to the echo server, TX to RX both ways, with a common ground:

| loadStream | echo |
|---|---|
| GP20 (TX) | GP21 (RX) |
| GP21 (RX) | GP20 (TX) |
| GND | GND |

Set both Picos to the same baud rate.

## The test
A test runs every 10 seconds, starting 1 second after power-up. Each test:
1. Clears all metrics and results, and anything left in the UART.
2. Builds 1KB of random printable characters (ASCII 32 to 126), new each time.
3. Sends it out of UART1 as fast as the UART will take it. The main loop never waits on the UART.
4. Receives the echo by interrupt into a 1KB buffer plus 256 bytes of contingency. Bytes with errors are kept, and anything beyond the buffer is counted as dropped.
5. Stops at whichever comes first:
   - all 1024 bytes are back
   - nothing has arrived for 50ms plus 20 byte times since sending finished
   - an overall timeout of twice the send time plus 500ms
6. Compares what came back with what was sent.

## Analysis
The comparison walks the sent and received data side by side. Where they differ, it works out which of these explains the data that follows:

| Result | Meaning |
|---|---|
| Valid | Received correctly |
| Corrupt | A byte was wrong, but the data after it lines up |
| Missing | Sent bytes that never arrived. The data lines up again further on |
| Extra | A byte arrived that was never sent. The next byte lines up |

The two are taken as lined up again once 4 bytes in a row agree. If nothing explains a difference, the byte counts as corrupt.

The echo server doesn't send back characters with a framing or break error, so errors on its side show up here as missing bytes.

## UART metrics
As in echo, the interrupt reads the UART data register directly to see the error flags for each character:

| Metric | Meaning |
|---|---|
| Received | Bytes received |
| Overrun | The receive FIFO was full and data was lost |
| Framing | The stop bit was missing |
| Break | The RX line was held low |
| Parity | Never seen, as parity checking is not turned on |

## Display
```
V1024 M0        valid       missing
C0    R1024     corrupt     received
O0    F0        overrun     framing
B0    P0        break       parity
```
Counts over 9999 show as `>9k`. The display refreshes every 0.5 seconds, but not while data is being sent, as the I2C update would pause the send.

## Console
After each test the console prints:
```
Test 3 @ 115200 baud: complete
  Sent 1024, received 1024 (dropped 0), 178456 us, 45906 bit/s
  Valid 1024, missing 0, corrupt 0, extra 0
  Overrun 0, framing 0, break 0, parity 0
```
The end reason is `complete`, `idle timeout` or `overall timeout`. The time runs from the start of sending to the last byte received.

The menu uses the same keys as the [echo](../echo/README.md#baud-rate-menu) baud rate menu, from 9600 up to 9.375 Mbps, plus `t` to run a test straight away. Changing rate cancels any test in progress and starts a new one about 1 second later.

## Building
The build target is `LoadStream`. See the [top-level README](../../README.md) for cloning and building.
