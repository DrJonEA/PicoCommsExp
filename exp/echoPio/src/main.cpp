/**
 * Jon Durrant.
 *
 * PIO UART Echo on Raspberry PICO 2
 * Echo characters received on a PIO UART (RX GP21) back out on a PIO UART
 * (TX GP20). The PIO programs are uart_tx and uart_rx from pico-examples.
 * Receive is interrupt driven. The last 10 characters received are
 * displayed on the OLED screen, refreshed every 0.5 seconds, along with
 * metrics for bytes echoed and receive errors (Overrun, Framing, Break and
 * Parity).
 *
 * PIO UART errors are mapped to the metrics:
 *   Overrun: RX state machine stalled on a full RX FIFO (FDEBUG RXSTALL)
 *   Framing: uart_rx raised its framing error IRQ flag. This also covers
 *            breaks, as the program can't tell them apart
 *   Break and Parity: never seen, kept so the display matches echo
 */

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "uart_tx.pio.h"
#include "uart_rx.pio.h"
extern "C"{
#include <ssd1306.h>
}
#include <cstdio>

#define DELAY 500 // Screen refresh in milliseconds

#define ECHO_BAUD      115200
#define ECHO_TX_PIN    20
#define ECHO_RX_PIN    21

#define HISTORY_LEN    10

// uart_rx raises "irq 4 rel": flag 4 + state machine number
#define RX_ERR_IRQ_FLAG(sm)  (4 + (sm))

// PIO resources, claimed at startup
static PIO  pio;
static uint smTx;
static uint smRx;
static uint offsetTx;
static uint offsetRx;
static uint pioIrq;		// System IRQ number used for the PIO
static uint pioIrqIndex;	// PIO IRQ line, 0 or 1

// Baud rates selectable from the stdio menu, keys 0-9 then a-i.
// The PIO programs take 8 clocks per bit, so the limit is clk_sys / 8
// (18.75 Mbps at 150MHz). Up to 9375000 matches the hardware UART echo.
// 12500000 needs a clock divider of 1.5, so bit lengths jitter by one
// PIO clock (1/8 bit)
static const uint32_t baudRates[] = {
	9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600,
	1000000, 2000000, 3000000, 4000000, 5000000, 6000000, 7000000, 8000000,
	9375000, 12500000, 18750000
};
#define NUM_BAUD_RATES (sizeof(baudRates) / sizeof(baudRates[0]))

// Circular buffer of the last HISTORY_LEN characters received
static volatile char     history[HISTORY_LEN];
static volatile uint32_t historyHead  = 0;	// Next write position
static volatile uint32_t historyCount = 0;	// Number of valid chars

// Metrics
typedef struct {
	uint32_t echoed;	// Bytes echoed back out on TX
	uint32_t overrun;	// RX FIFO overrun, data lost
	uint32_t framing;	// Missing stop bit
	uint32_t breaks;	// Break condition on RX line
	uint32_t parity;	// Parity error (parity checking is off, so always 0)
} EchoMetrics;

static volatile EchoMetrics metrics = {0, 0, 0, 0, 0};

/***
 * PIO interrupt handler.
 * Fires when the RX FIFO is not empty or uart_rx flags a framing error.
 * Echo each received character to TX and store it in the history buffer
 *
 * Runs from RAM. From flash, the main loop (OLED refresh, stdio) can evict
 * it from the XIP cache, and reloading it can take longer than the 8 entry
 * RX FIFO lasts at 18.75 Mbps (about 4.3us), causing overruns
 */
void __not_in_flash_func(onPioRx)() {
	// Framing error or break, raised by uart_rx. The bad character is
	// not pushed to the FIFO, so it is never echoed
	if (pio_interrupt_get(pio, RX_ERR_IRQ_FLAG(smRx))) {
		pio_interrupt_clear(pio, RX_ERR_IRQ_FLAG(smRx));
		metrics.framing++;
	}

	// RX FIFO was full, so the state machine stalled and bits were lost.
	// Write 1 to clear the sticky flag
	uint32_t stallMask = 1u << (PIO_FDEBUG_RXSTALL_LSB + smRx);
	if (pio->fdebug & stallMask) {
		pio->fdebug = stallMask;
		metrics.overrun++;
	}

	// Data is left justified, so the character is in the top byte
	io_rw_8 *rxFifoByte = (io_rw_8 *)&pio->rxf[smRx] + 3;
	while (!pio_sm_is_rx_fifo_empty(pio, smRx)) {
		char c = (char)*rxFifoByte;

		// Echo back out. TX runs at the same rate as RX, so this only
		// waits if the TX FIFO is full
		pio_sm_put_blocking(pio, smTx, (uint32_t)c);
		metrics.echoed++;

		// Record in history
		history[historyHead] = c;
		historyHead = (historyHead + 1) % HISTORY_LEN;
		if (historyCount < HISTORY_LEN) {
			historyCount++;
		}
	}
}

/***
 * Claim PIO state machines, load the uart_tx and uart_rx programs and
 * attach the RX interrupt handler. Called once at startup
 */
void claimPio() {
	bool ok = pio_claim_free_sm_and_add_program_for_gpio_range(
			&uart_tx_program, &pio, &smTx, &offsetTx, ECHO_TX_PIN, 1, true);
	hard_assert(ok);

	// Second claim must land in the same PIO. It will, as the first PIO
	// still has free state machines and instruction memory
	PIO pioRx;
	ok = pio_claim_free_sm_and_add_program_for_gpio_range(
			&uart_rx_program, &pioRx, &smRx, &offsetRx, ECHO_RX_PIN, 1, true);
	hard_assert(ok && (pioRx == pio));

	// Find a free PIO IRQ line, as in the pico-examples uart_rx_intr
	pioIrq = pio_get_irq_num(pio, 0);
	if (irq_get_exclusive_handler(pioIrq)) {
		pioIrq++;
		if (irq_get_exclusive_handler(pioIrq)) {
			panic("All PIO IRQs are in use");
		}
	}
	pioIrqIndex = pioIrq - pio_get_irq_num(pio, 0);

	// Exclusive handler, so there is no shared handler chain to walk, at
	// the highest priority so no other interrupt delays it
	irq_set_exclusive_handler(pioIrq, onPioRx);
	irq_set_priority(pioIrq, PICO_HIGHEST_IRQ_PRIORITY);

	// Interrupt when RX FIFO is not empty, or on a framing error
	pio_set_irqn_source_enabled(pio, pioIrqIndex,
			pio_get_rx_fifo_not_empty_interrupt_source(smRx), true);
	pio_set_irqn_source_enabled(pio, pioIrqIndex,
			(pio_interrupt_source_t)(pis_interrupt0 + RX_ERR_IRQ_FLAG(smRx)),
			true);
}

/***
 * Actual baud rate the PIO will run at, after the clock divider is
 * rounded to 1/256ths. Matches the divider the init functions set
 */
uint32_t actualBaud(uint32_t baud) {
	uint32_t clk = clock_get_hz(clk_sys);
	float div = (float)clk / (8 * baud);
	uint16_t divInt;
	uint8_t  divFrac;
	pio_calculate_clkdiv_from_float(div, &divInt, &divFrac);
	uint32_t div256 = ((uint32_t)divInt << 8) + divFrac;
	return (uint32_t)(((uint64_t)clk * 256) / ((uint64_t)8 * div256));
}

/***
 * Setup the PIO UART state machines for echo.
 * Can be called again to change baud rate: the state machines are reset
 * and the metrics and history cleared.
 * Returns the actual baud rate achieved.
 */
uint32_t setupUart(uint32_t baud) {
	// Stop receive interrupts and the state machines
	irq_set_enabled(pioIrq, false);
	pio_sm_set_enabled(pio, smTx, false);
	pio_sm_set_enabled(pio, smRx, false);

	// The init functions from the .pio files restart each state machine
	// and clear its FIFOs
	uart_tx_program_init(pio, smTx, offsetTx, ECHO_TX_PIN, baud);
	uart_rx_program_init(pio, smRx, offsetRx, ECHO_RX_PIN, baud);

	// Clear error flags, metrics and history.
	// The PIO IRQ is off, so the ISR can't race us
	pio_interrupt_clear(pio, RX_ERR_IRQ_FLAG(smRx));
	pio->fdebug = 1u << (PIO_FDEBUG_RXSTALL_LSB + smRx);
	historyHead  = 0;
	historyCount = 0;
	metrics.echoed  = 0;
	metrics.overrun = 0;
	metrics.framing = 0;
	metrics.breaks  = 0;
	metrics.parity  = 0;

	irq_set_enabled(pioIrq, true);

	return actualBaud(baud);
}

/***
 * Map a menu key to an index into baudRates, or -1 if not valid
 */
int menuKeyToIndex(int c) {
	int idx = -1;
	if ((c >= '0') && (c <= '9')) {
		idx = c - '0';
	} else if ((c >= 'a') && (c <= 'z')) {
		idx = c - 'a' + 10;
	} else if ((c >= 'A') && (c <= 'Z')) {
		idx = c - 'A' + 10;
	}
	if ((idx < 0) || (idx >= (int)NUM_BAUD_RATES)) {
		return -1;
	}
	return idx;
}

/***
 * Print the baud rate menu on stdio
 */
void printMenu(uint32_t current) {
	printf("\nPIO UART Echo - select baud rate (current %lu)\n",
			(unsigned long)current);
	for (uint32_t i = 0; i < NUM_BAUD_RATES; i++) {
		char key = (i < 10) ? ('0' + i) : ('a' + i - 10);
		printf("  %c : %lu\n", key, (unsigned long)baudRates[i]);
	}
	printf("> ");
}

/***
 * Handle a key from stdio. Returns the new current baud rate
 */
uint32_t handleMenuKey(int c, uint32_t current) {
	int idx = menuKeyToIndex(c);
	if (idx < 0) {
		printMenu(current);
		return current;
	}

	uint32_t requested = baudRates[idx];
	uint32_t actual = setupUart(requested);
	printf("%c\nRequested %lu baud, actual %lu baud. Metrics and history reset.\n> ",
			c, (unsigned long)requested, (unsigned long)actual);
	return actual;
}

/***
 * Copy the history, oldest first, into buf as a null terminated string.
 * Non printable characters are shown as '.'
 */
void getHistory(char *buf) {
	// No interrupt masking: the ISR may add characters while we copy, so the
	// snapshot can be slightly torn. Each 32 bit read is atomic, so indexes
	// are always in range.
	uint32_t count = historyCount;
	uint32_t start = (historyHead + HISTORY_LEN - count) % HISTORY_LEN;
	for (uint32_t i = 0; i < count; i++) {
		buf[i] = history[(start + i) % HISTORY_LEN];
	}

	for (uint32_t i = 0; i < count; i++) {
		if ((buf[i] < 32) || (buf[i] > 126)) {
			buf[i] = '.';
		}
	}
	buf[count] = 0;
}

/***
 * Take a copy of the metrics.
 * Interrupts are left enabled, so the values may be from slightly
 * different moments, but each 32 bit counter is read atomically.
 */
void getMetrics(EchoMetrics *pMetrics) {
	pMetrics->echoed  = metrics.echoed;
	pMetrics->overrun = metrics.overrun;
	pMetrics->framing = metrics.framing;
	pMetrics->breaks  = metrics.breaks;
	pMetrics->parity  = metrics.parity;
}

/***
 * Format a short metric "L:nnn" for a half width column at scale 2.
 * A column is 64px = 5 chars, so values over 999 are shown as "1k+"
 */
void fmtShort(char *buf, size_t len, char label, uint32_t value) {
	if (value > 999) {
		snprintf(buf, len, "%c:1k+", label);
	} else {
		snprintf(buf, len, "%c:%lu", label, (unsigned long)value);
	}
}

/***
 * Draw the received history and metrics on the Oled Screen.
 * All text at scale 2 (12x16 px per char), 4 rows of 10 chars:
 *   Row 0: last 10 characters received
 *   Row 1: E: bytes echoed
 *   Row 2: O: overrun    F: framing
 *   Row 3: B: break      P: parity
 */
void drawHistory(ssd1306_t *pOled){
	const uint32_t col2 = 64;
	char buf[HISTORY_LEN + 1];
	char line[16];
	EchoMetrics m;

	getHistory(buf);
	getMetrics(&m);

	// Last characters received
	ssd1306_draw_string(pOled, 0, 0, 2, buf);

	// Bytes echoed, full width
	snprintf(line, sizeof(line), "E:%lu", (unsigned long)m.echoed);
	ssd1306_draw_string(pOled, 0, 16, 2, line);

	// Errors, two columns
	fmtShort(line, sizeof(line), 'O', m.overrun);
	ssd1306_draw_string(pOled, 0, 32, 2, line);
	fmtShort(line, sizeof(line), 'F', m.framing);
	ssd1306_draw_string(pOled, col2, 32, 2, line);

	fmtShort(line, sizeof(line), 'B', m.breaks);
	ssd1306_draw_string(pOled, 0, 48, 2, line);
	fmtShort(line, sizeof(line), 'P', m.parity);
	ssd1306_draw_string(pOled, col2, 48, 2, line);
}

int main() {
	stdio_init_all();
	sleep_ms(2000);
	printf("GO\n");

	//Setup the OLED Screen
	i2c_init(i2c0, 400000);
	gpio_pull_up(12);
	gpio_pull_up(13);
	gpio_set_function(12, GPIO_FUNC_I2C);
	gpio_set_function(13, GPIO_FUNC_I2C);
	ssd1306_t oled;
	oled.external_vcc=false;
	bool res = ssd1306_init(
			&oled,
			128, 	// Width
			64, 		// Height
			0x3c, // Address
			i2c0	// I2C Channel
			);

	if (!res){
		printf("Oled Init failed\n");
	}

	//Setup the echo PIO UART
	claimPio();
	uint32_t baud = setupUart(ECHO_BAUD);
	printMenu(baud);

	absolute_time_t nextRefresh = get_absolute_time();
	while (true) { // Loop forever
		// Poll stdio for menu selection, waiting up to 10ms
		int c = getchar_timeout_us(10000);
		if (c != PICO_ERROR_TIMEOUT) {
			baud = handleMenuKey(c, baud);
		}

		// Refresh the screen every DELAY ms
		if (time_reached(nextRefresh)) {
			nextRefresh = make_timeout_time_ms(DELAY);
			if (res){
				ssd1306_clear(&oled);
				drawHistory(&oled);
				ssd1306_show(&oled);
			}
		}
	}

}
