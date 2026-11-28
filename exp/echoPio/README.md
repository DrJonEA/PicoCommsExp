# echoPio
The [echo](../echo/README.md) server, using a PIO UART in place of the UART hardware. It keeps echo's pins, GP20 and GP21, and adds 12.5 and 18.75 Mbps, so it can serve the [loadStreamPio](../loadStreamPio/README.md) load test at its top rates.

## PIO UART
The PIO programs are taken unchanged from [pico-examples](https://github.com/raspberrypi/pico-examples/tree/master/pio):
- [uart_tx.pio](src/uart_tx.pio): an 8N1 transmitter
- [uart_rx.pio](src/uart_rx.pio): the full `uart_rx` receiver, which detects framing errors

Each runs on its own state machine in the same PIO. The setup follows the pico-examples `uart_rx_intr` example, with a shared handler on a free PIO interrupt. That interrupt fires when the receive FIFO is not empty, or when `uart_rx` raises its framing error flag. Routing that flag to an interrupt needs the RP2350, so this is Pico 2 only.

The interrupt reads each character from the top byte of the receive FIFO and puts it straight into the transmit FIFO. Transmit runs at the same rate as receive, so this only waits if the transmit FIFO is full.

### Interrupt timing at 18.75 Mbps
At 18.75 Mbps a character arrives every 533 ns, so the 8-entry receive FIFO fills in about 4.3 µs. From flash, the first version of the handler had a few overruns and framing errors at this rate (12.5 Mbps was fine). The likely cause is the main loop's OLED refresh and console code pushing the handler out of the flash cache. Reloading it from flash can then take longer than the FIFO lasts.

The framing errors are most likely knock-on effects of the overruns. While the receive state machine is stalled it misses start bits. When it restarts it can lock onto a data bit in the middle of a character, and the stop bit it then checks is wrong.

To keep the handler fast:
- it runs from RAM (`__not_in_flash_func`), and makes no calls back into flash
- it has its own interrupt slot (`irq_set_exclusive_handler`), so there is no shared handler chain to walk
- it runs at the highest interrupt priority

## Wiring
| Function | Pins | Notes |
|---|---|---|
| Echo UART | GP20 (TX), GP21 (RX) | PIO, 8N1. RX has the internal pull-up on |
| OLED display | GP12 (SDA), GP13 (SCL) | SSD1306 128x64 on I2C0, address 0x3C |
| stdio console | GP16 (TX), GP17 (RX) | UART0, 115200 baud |

The pins are the same as echo, so the load tests wire up the same way. See the README of each load test for its pins.

At 18.75 Mbps, keep the wires between the boards short and make the ground connection between them solid. Long jumper wires and a weak ground gave blocks of corrupt bytes, with no framing errors or overruns, that went away once the wiring was improved.

## Metrics
There is no UART hardware to report errors, so the metrics come from the PIO, as in loadStreamPio:

| Metric | Source |
|---|---|
| Echoed | Bytes sent back out |
| Overrun | The receive state machine stalled on a full FIFO (the `RXSTALL` debug flag). It is counted each time the interrupt finds the flag set, not for each byte lost |
| Framing | `uart_rx` saw a bad stop bit. This includes breaks, as the program can't tell them apart |
| Break | Always 0 |
| Parity | Always 0 |

`uart_rx` drops characters with a bad stop bit, so they are never echoed or added to the history.

## Display
The same as [echo](../echo/README.md#display):
```
hello worl      last 10 characters received
E:1234567       bytes echoed
O:0   F:0       overrun     framing
B:0   P:0       break       parity
```

## Baud rate menu
The same keys as the [echo](../echo/README.md#baud-rate-menu) menu, plus two faster rates:

| Key | Baud | PIO clock divider |
|---|---|---|
| g | 9.375M | 2 |
| h | 12.5M | 1.5 |
| i | 18.75M | 1, the PIO maximum at 150 MHz |

A rate with a fractional divider, such as 12.5M, makes some bits one PIO clock (1/8 bit) longer than others. Only whole-number dividers give even bit lengths.

The hardware UART load tests, loadStream and loadStreamAux, stop at 9.375 Mbps. Only loadStreamPio can use the two faster rates.

Changing rate restarts both state machines, prints the requested and actual baud rate, and clears the metrics and the history. The actual rate is worked out from the PIO clock divider, which is set in steps of 1/256.

## Building
The build target is `EchoPio`. The two `.pio` files are turned into headers at build time with `pico_generate_pio_header`. See the [top-level README](../../README.md) for cloning and building.
