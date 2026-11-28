# loadStreamPioDMA
The [loadStreamPio](../loadStreamPio/README.md) load test, with the PIO UART fed by DMA so characters go out back to back with no gaps.

## Why DMA
In loadStreamPio the main loop feeds the PIO transmit FIFO. At 18.75 Mbps a character goes out every 533 ns, so the 8-entry FIFO empties in about 4.3 µs. The main loop can't keep it full, so there are idle gaps between characters. The data still arrives intact, but at about 11 Mbit/s rather than the 15 Mbit/s the line can carry.

Here a DMA channel does the feeding. It reads one byte at a time from the test data and writes it to the PIO transmit FIFO. The FIFO's DREQ paces it, so it writes whenever there is space. The FIFO never runs dry and the CPU is free during the send.

The DMA writes single bytes. A byte write to a PIO FIFO is copied across all four byte lanes, and `uart_tx` shifts out the bottom 8 bits, so each character goes out intact.

## Receive
Receive is the same as loadStreamPio, by interrupt from the PIO receive FIFO. With a gap-free stream coming back, the handler has no slack. As in [echoPio](../echoPio/README.md#interrupt-timing-at-1875-mbps):
- it runs from RAM (`__not_in_flash_func`), and makes no calls back into flash
- it has its own interrupt slot (`irq_set_exclusive_handler`)
- it runs at the highest interrupt priority

## Wiring
| Function | Pins | Notes |
|---|---|---|
| Test UART | GP18 (TX), GP19 (RX) | PIO, 8N1. RX has the internal pull-up on |
| OLED display | GP12 (SDA), GP13 (SCL) | SSD1306 128x64 on I2C0, address 0x3C |
| stdio console | GP16 (TX), GP17 (RX) | UART0, 115200 baud |

Wire it the same way as loadStreamPio. Use [echoPio](../echoPio/README.md) as the echo server, or a loop jumper from GP18 to GP19, for rates above 9.375 Mbps. At 18.75 Mbps keep the wires short and the ground connection between boards solid.

## Console
The report adds one line to loadStreamPio's. A run at 18.75 Mbps against echoPio:
```
Test 6 @ 18750000 baud: complete
  Sent 1024, received 1024 (dropped 0), 548 us, 14948905 bit/s
  Line use 99%, DMA send done at 549 us
  Valid 1024, missing 0, corrupt 0, extra 0
  Overrun 0, framing 0, break 0, parity 0
```
- `bit/s` counts data bits, 8 per character. Each character takes 10 bits on the wire (start, 8 data, stop), so the most it can show is 80% of the baud rate: 15 Mbit/s at 18.75 Mbps.
- `Line use` compares the wire bits (10 per character) with the baud rate. 100% means no gaps. 1024 characters need at least 546 µs at 18.75 Mbps, so 548 µs is 99.6%, as the time includes the trip through the echo server.
- `DMA send done` is when the main loop saw the DMA finish. At 18.75 Mbps the receive interrupt takes almost all of the CPU, so the main loop notices late. That is why it can show a time after the last byte came back.

For comparison, loadStreamPio, with the main loop feeding the FIFO, took about 718 µs (11.4 Mbit/s) for the same test.

The display, test, analysis and baud rate menu are the same as [loadStreamPio](../loadStreamPio/README.md).

## Building
The build target is `LoadStreamPioDMA`. See the [top-level README](../../README.md) for cloning and building.
