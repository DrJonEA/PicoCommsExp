# PicoCommsExp
Experiments in Pico 2 Communication

## Cloning and Building
This is a Pico SDK based project in C++. Where libraries are included they are normally included as submodules. When cloning please recurse submodules.

I have a blog that explains how to clone repos and build my projects on
[drjonea.co.uk](https://drjonea.co.uk/2025/12/15/building-my-projects-from-repo/).

Each experiment in the `exp` folder is a stand-alone project and is built from its own folder.

## Hardware
All of the experiments run on a Pico 2 and share this setup:

| Function | Pins | Notes |
|---|---|---|
| OLED display | GP12 (SDA), GP13 (SCL) | SSD1306 128x64 on I2C0, address 0x3C |
| stdio console | GP16 (TX), GP17 (RX) | UART0, 115200 baud |

The UART under test changes from one experiment to the next, see the table below.

The OLED uses the [pico-ssd1306](https://github.com/daschr/pico-ssd1306) library, included as a submodule in `lib`.

## Folders
- `exp`: experiments (see below)
- `lib`: libraries (submodules)

## Experiments
The `exp` folder holds small stand-alone projects, each with its own README. echo and echoPio are the server end. The loadStream experiments run the same load test against one of them, on a second Pico, using a different way of driving the UART each time.

| Experiment | What it does | UART under test | TX | RX |
|---|---|---|---|---|
| [oledTest](exp/oledTest/README.md) | OLED display and LED bring-up | none | | |
| [echo](exp/echo/README.md) | Echo server with error metrics | UART1 | GP20 | GP21 |
| [echoPio](exp/echoPio/README.md) | echo with a PIO UART, up to 18.75 Mbps | PIO | GP20 | GP21 |
| [loadStream](exp/loadStream/README.md) | 1KB load test against echo | UART1 | GP20 | GP21 |
| [loadStreamAux](exp/loadStreamAux/README.md) | loadStream on the UART_AUX pins | UART1 via UART_AUX | GP26 | GP27 |
| [loadStreamPio](exp/loadStreamPio/README.md) | loadStream with a PIO UART | PIO | GP18 | GP19 |
| [loadStreamPioDMA](exp/loadStreamPioDMA/README.md) | loadStreamPio with DMA transmit, no gaps | PIO + DMA | GP18 | GP19 |
| [loadStreamPioDMARx](exp/loadStreamPioDMARx/README.md) | DMA for transmit and receive, core left free | PIO + DMA | GP18 | GP19 |

### oledTest
Shows text on the OLED and blinks an LED on GP2. Use it to check the display wiring first.

### echo
Echoes everything received on UART1 back out, from the receive interrupt. Counts bytes echoed and overrun, framing, break and parity errors, and shows them on the OLED with the last 10 characters received. The baud rate is selected from a console menu, from 9600 up to 9.375 Mbps.

### echoPio
The echo server using the PIO UART programs from [pico-examples](https://github.com/raspberrypi/pico-examples/tree/master/pio), on the same pins as echo. It adds 12.5 and 18.75 Mbps, beyond the 9.375 Mbps limit of the UART hardware.

### loadStream
Sends 1KB of random printable characters to the echo server every 10 seconds and reads the echo back by interrupt. It then compares the two to count valid, missing, corrupt and extra bytes. Results and UART errors are shown on the OLED and reported on the console.

### loadStreamAux
The same load test on GP26 and GP27. These are normally UART1's CTS and RTS pins. The RP2350's UART_AUX pin function turns them into UART1 TX and RX.

### loadStreamPio
The same load test using the PIO UART programs from [pico-examples](https://github.com/raspberrypi/pico-examples/tree/master/pio), on GP18 and GP19. Errors come from the PIO itself, as there is no UART hardware to report them. It adds 12.5 and 18.75 Mbps, for use against echoPio or with a loop jumper.

### loadStreamPioDMA
loadStreamPio with the PIO transmit FIFO fed by DMA. At 18.75 Mbps the main loop can't keep the FIFO full, which leaves gaps between characters and limits loadStreamPio to about 11 Mbit/s. With DMA the characters go out back to back. At 18.75 Mbps it moves 1KB in 548 µs, 14.95 Mbit/s, using 99.6% of the line with no errors.

### loadStreamPioDMARx
loadStreamPioDMA with receive moved to DMA too. In loadStreamPioDMA every received character raises an interrupt, which at 18.75 Mbps takes almost all of the core. Here a second DMA channel drains the receive FIFO, and the main loop just polls the count. At 18.75 Mbps it keeps the same 14.95 Mbit/s with the core 100% free.
