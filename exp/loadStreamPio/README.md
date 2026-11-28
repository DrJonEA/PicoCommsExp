# loadStreamPio
The [loadStream](../loadStream/README.md) load test, using a PIO UART on GP18 and GP19 in place of the UART hardware.

## PIO UART
The PIO programs are taken unchanged from [pico-examples](https://github.com/raspberrypi/pico-examples/tree/master/pio):
- [uart_tx.pio](src/uart_tx.pio): an 8N1 transmitter
- [uart_rx.pio](src/uart_rx.pio): the full `uart_rx` receiver, which detects framing errors (rather than `uart_rx_mini`)

Each runs on its own state machine in the same PIO, with its FIFOs joined to give 8 entries in the direction it uses. Both programs take 8 PIO clocks per bit, so they can run up to clk_sys / 8, about 18.75 Mbps at 150 MHz.

The PIO and interrupt setup follows the pico-examples `uart_rx_intr` example. The program is loaded with `pio_claim_free_sm_and_add_program_for_gpio_range`, and a shared handler is attached to a free PIO interrupt. That interrupt fires when:
- the receive FIFO is not empty
- `uart_rx` raises its framing error flag (`irq 4 rel`). This flag can only trigger an interrupt on the RP2350, so this is Pico 2 only

Data from `uart_rx` is left justified, so the handler reads the character from the top byte of the receive FIFO.

## Metrics
There is no UART hardware to report errors, so the metrics come from the PIO:

| Metric | Source |
|---|---|
| Received | Characters read from the receive FIFO |
| Overrun | The receive state machine stalled on a full FIFO (the `RXSTALL` debug flag). It is counted each time the interrupt finds the flag set, not for each byte lost |
| Framing | `uart_rx` saw a bad stop bit. This includes breaks, as the program can't tell them apart |
| Break | Always 0 |
| Parity | Always 0 |

`uart_rx` drops characters with a bad stop bit, so each framing error also shows up as a missing byte.

The actual baud rate printed on a rate change is worked out from the PIO clock divider, which is set in steps of 1/256.

## Wiring
| Function | Pins | Notes |
|---|---|---|
| Test UART | GP18 (TX), GP19 (RX) | PIO, 8N1. RX has the internal pull-up on |
| OLED display | GP12 (SDA), GP13 (SCL) | SSD1306 128x64 on I2C0, address 0x3C |
| stdio console | GP16 (TX), GP17 (RX) | UART0, 115200 baud |

Connect to the [echo](../echo/README.md) server, TX to RX both ways, with a common ground:

| loadStreamPio | echo |
|---|---|
| GP18 (TX) | GP21 (RX) |
| GP19 (RX) | GP20 (TX) |
| GND | GND |

## Test, display and console
These are the same as [loadStream](../loadStream/README.md#the-test). The baud rate menu keeps the same list, 9600 up to 9.375 Mbps, so results can be compared with the hardware UART, and adds two faster rates:

| Key | Baud | PIO clock divider |
|---|---|---|
| g | 9.375M | 2 |
| h | 12.5M | 1.5 |
| i | 18.75M | 1, the PIO maximum at 150 MHz |

A rate with a fractional divider, such as 12.5M, makes some bits one PIO clock (1/8 bit) longer than others. Only whole-number dividers give even bit lengths.

The [echo](../echo/README.md) server is a hardware UART and stops at 9.375 Mbps. For the two faster rates, use one of these:
- [echoPio](../echoPio/README.md), the echo server on a PIO UART, wired the same way as echo
- a loop jumper from GP18 (TX) to GP19 (RX), to test the PIO UART on its own

The console reminds you of this when you select one of them.

With a loop jumper, the PIO UART has run with no loss at 18.75 Mbps.

Between two boards at 18.75 Mbps, wiring matters. Long jumper wires and a single weak ground gave blocks of corrupt bytes, with no framing errors or overruns. Short wires and a solid ground connection between the boards fixed it.

## Building
The build target is `LoadStreamPio`. The two `.pio` files are turned into headers at build time with `pico_generate_pio_header`. See the [top-level README](../../README.md) for cloning and building.
