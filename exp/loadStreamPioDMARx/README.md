# loadStreamPioDMARx
The [loadStreamPioDMA](../loadStreamPioDMA/README.md) load test, with receive moved to DMA as well, so the core is free while data streams in both directions.

## Why DMA receive
In loadStreamPioDMA the transmit side is DMA, but every received character raises an interrupt. At 18.75 Mbps that is one interrupt every 533 ns. The handler runs at the highest priority, so the core spends almost all of the test inside it and the main loop barely runs. One sign of this is that its "DMA send done" time came out later than the last byte received.

Here a second DMA channel drains the PIO receive FIFO into the receive buffer, so no interrupt runs per character.

## How it works
- **TX DMA:** as in loadStreamPioDMA, one byte at a time from the test data into the PIO transmit FIFO, paced by the transmit FIFO DREQ.
- **RX DMA:** one byte at a time from the PIO receive FIFO into `rxBuf`, paced by the receive FIFO DREQ. `uart_rx` leaves each character in the top byte of the FIFO word, so the DMA reads the byte at offset 3, as the interrupt did before. Any read takes the entry off the FIFO.
- **Started each test:** the RX DMA starts first, then the TX DMA. The RX DMA is set for the full 1280-byte buffer and stops when the buffer is full.
- **Bytes received:** the buffer size minus the DMA's remaining transfer count. The top 4 bits of that register are the mode, so they are masked off.
- **Main loop polling:** while a test runs, the main loop reads the count, notes the time whenever it changes, and checks the overrun flag. That gives the time of the last byte and spots an idle line for the timeout.
- **No display refresh during a test:** a refresh takes about 25 ms, which would delay the polling and make the timing late.

## Metrics
| Metric | Source |
|---|---|
| Received | Bytes the RX DMA has written |
| Dropped | Bytes left in the RX FIFO after the buffer was full |
| Overrun | The receive state machine stalled on a full FIFO (the `RXSTALL` flag), checked by the main loop |
| Framing | `uart_rx` saw a bad stop bit. This still raises an interrupt, but only when there is an error |
| Break, Parity | Always 0 |

## Wiring
| Function | Pins | Notes |
|---|---|---|
| Test UART | GP18 (TX), GP19 (RX) | PIO, 8N1. RX has the internal pull-up on |
| OLED display | GP12 (SDA), GP13 (SCL) | SSD1306 128x64 on I2C0, address 0x3C |
| stdio console | GP16 (TX), GP17 (RX) | UART0, 115200 baud |

Wire it the same way as loadStreamPio. Use [echoPio](../echoPio/README.md) as the echo server, or a loop jumper from GP18 to GP19, for rates above 9.375 Mbps. At 18.75 Mbps keep the wires short and the ground connection between boards solid.

## Console
A run at 18.75 Mbps against echoPio:
```
Test 8 @ 18750000 baud: complete
  Sent 1024, received 1024 (dropped 0), 548 us, 14948905 bit/s
  Line use 99%, DMA send done at 542 us
  Core busy 0%, free 100%
  Valid 1024, missing 0, corrupt 0, extra 0
  Overrun 0, framing 0, break 0, parity 0
```
The throughput is the same as loadStreamPioDMA's, but the core is free. "DMA send done" now comes before the last byte, as up to 8 characters are still in the transmit FIFO when the DMA finishes.

The report is the same as loadStreamPioDMA's, including the `Core busy N%, free M%` line, measured as in [loadStreamPio](../loadStreamPio/README.md#core-load). Comparing it with loadStreamPioDMA shows what moving receive to DMA frees up.

The time of the last byte comes from the main loop polling, so it is accurate to one run of the loop, a few µs at most.

The display, test, analysis and baud rate menu are the same as [loadStreamPio](../loadStreamPio/README.md).

## Building
The build target is `LoadStreamPioDMARx`. See the [top-level README](../../README.md) for cloning and building.
