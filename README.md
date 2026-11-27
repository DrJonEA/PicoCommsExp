# PicoCommsExp
Experiments in Pico 2 Communication

## Cloning and Building
This is a Pico SDK based project in C++. Where libraries are included they are normally included as submodules. When cloning please recurse submodules.

I have a blog that explains how to clone repos and build my projects on
[drjonea.co.uk](https://drjonea.co.uk/2025/12/15/building-my-projects-from-repo/).

Each experiment in the `exp` folder is a stand-alone project and is built from its own folder.

## Hardware
All of the experiments use the same setup on a Pico 2:

| Function | Pins | Notes |
|---|---|---|
| OLED display | GP12 (SDA), GP13 (SCL) | SSD1306 128x64 on I2C0, address 0x3C |
| stdio console | GP16 (TX), GP17 (RX) | UART0, 115200 baud |
| Test UART | GP20 (TX), GP21 (RX) | UART1, used by echo and loadStream |
| LED | GP2 | oledTest only |

The OLED uses the [pico-ssd1306](https://github.com/daschr/pico-ssd1306) library, included as a submodule in `lib`.

## Folders
- `exp`: experiments (see below)
- `lib`: libraries (submodules)

## Experiments
The `exp` folder holds small stand-alone projects. echo and loadStream are used as a pair, on two Picos.

### oledTest
Bring-up of the OLED display. It shows "Testing" with a line underneath, then blinks the LED on GP2 and shows "Blink" on the display in time with it, every 0.5 seconds. Use it to check the display wiring before moving on to the UART experiments.

### echo
An echo server on UART1 (TX GP20, RX GP21). Every character received is sent straight back out. Receiving is interrupt driven, and the interrupt sends the echo.

The interrupt reads the UART data register directly so it can count the error flags that come with each character. Characters with a framing, break or parity error are counted but not echoed.

The display refreshes every 0.5 seconds:
```
hello worl      last 10 characters received
E:1234567       bytes echoed
O:0   F:0       overrun     framing
B:0   P:0       break       parity
```
Parity is never seen, as parity checking is not turned on. Counts over 999 in the error columns show as `1k+`.

A menu on the stdio console selects the baud rate, from 9600 up to 9.375 Mbps (the UART maximum with the Pico 2's 150 MHz peripheral clock). Changing rate resets the UART, confirms the actual baud rate achieved, and clears the metrics and history.

### loadStream
A load test for UART1, run against the echo server. Wire the two Picos together, TX to RX both ways, with a common ground:

| loadStream | echo |
|---|---|
| GP20 (TX) | GP21 (RX) |
| GP21 (RX) | GP20 (TX) |
| GND | GND |

Set both to the same baud rate. A test runs every 10 seconds, and `t` on the console runs one straight away. Each test:
1. Clears all metrics and results.
2. Builds 1KB of random printable characters.
3. Streams it out of UART1 as fast as the UART will take it.
4. Receives the echo by interrupt into a 1KB buffer plus 256 bytes of contingency.
5. Stops once all the data is back, or no data has arrived for a short time, or an overall timeout expires.
6. Compares what came back with what was sent.

The comparison walks both buffers together. When they differ it works out whether a byte is corrupt (the data after it lines up), missing (the data lines up further on) or extra (an inserted byte).

The display shows the results of the last test:
```
V1024 M0        valid       missing
C0    R1024     corrupt     received
O0    F0        overrun     framing
B0    P0        break       parity
```
Counts over 9999 show as `>9k`.

The console prints a full report for each test: the end reason, bytes sent and received, time, throughput in bit/s, the valid, missing, corrupt and extra counts, and the UART errors. It has the same baud rate menu as echo.

The echo server does not return characters with a framing or break error, so those show up here as missing bytes.
