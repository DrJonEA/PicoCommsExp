/**
 * Jon Durrant.
 *
 * PIO UART Load Test on Raspberry PICO 2
 * Streams a 1KB block of random printable characters out of a PIO UART
 * (TX GP18) to an echo server and reads the echo back on a PIO UART
 * (RX GP19). The PIO programs are uart_tx and uart_rx from pico-examples.
 * Receive is interrupt driven into a 1KB + contingency buffer.
 *
 * Once all data is back, or a timeout expires, the received data is
 * compared to what was sent to count valid, missing, corrupt and extra
 * bytes. The test repeats every 10 seconds, with all metrics cleared first.
 *
 * PIO UART errors are mapped to the metrics:
 *   Overrun: RX state machine stalled on a full RX FIFO (FDEBUG RXSTALL)
 *   Framing: uart_rx raised its framing error IRQ flag. This also covers
 *            breaks, as the program can't tell them apart
 *   Break and Parity: never seen, kept so the display matches loadStream
 *
 * Above the 9.375 Mbps limit of the hardware UART echo server, test the
 * PIO UART on its own with a loop jumper from GP18 (TX) to GP19 (RX).
 *
 * Results and UART error metrics (Overrun, Framing, Break and Parity) are
 * shown on the OLED screen and printed on stdio. A stdio menu selects the
 * baud rate.
 */

#include "pico/stdlib.h"
#include "pico/rand.h"
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
#define ECHO_TX_PIN    18
#define ECHO_RX_PIN    19

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

#define TEST_LEN        1024				// Bytes sent per test
#define RX_BUF_LEN      (TEST_LEN + 256)	// Receive buffer with contingency
#define TEST_PERIOD_MS  10000				// Start a test every 10 seconds
#define MATCH_LEN       4					// Bytes that must agree to resync

// Core load. During a test, whenever the main loop has nothing to do it
// spins for IDLE_SPIN_CYCLES and counts one idle spin. At startup a
// calibration run with no traffic measures the idle spin rate of a free
// core. During a test, free = idle spin rate / calibrated rate. Anything
// that takes the core, interrupts or the main loop moving data, lowers the
// rate. The main loop's polling is in both, so it cancels out
#define IDLE_SPIN_CYCLES  64
#define IDLE_CAL_US       20000

// Baud rates selectable from the stdio menu, keys 0-9 then a-i.
// The PIO programs take 8 clocks per bit, so the limit is clk_sys / 8
// (18.75 Mbps at 150MHz). Up to 9375000 matches loadStream for comparison.
// 12500000 and 18750000 are beyond the hardware UART echo server, so use
// a loop jumper from TX to RX.
// 12500000 needs a clock divider of 1.5, so bit lengths jitter by one
// PIO clock (1/8 bit)
static const uint32_t baudRates[] = {
	9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600,
	1000000, 2000000, 3000000, 4000000, 5000000, 6000000, 7000000, 8000000,
	9375000, 12500000, 18750000
};
#define HW_UART_MAX_BAUD 9375000
#define NUM_BAUD_RATES (sizeof(baudRates) / sizeof(baudRates[0]))

// Test data
static char txBuf[TEST_LEN];
static volatile char     rxBuf[RX_BUF_LEN];
static volatile uint32_t rxCount  = 0;	// Bytes stored in rxBuf
static volatile uint32_t lastRxUs = 0;	// Time of last received byte

// UART metrics, updated by the ISR
typedef struct {
	uint32_t received;	// Bytes received on RX
	uint32_t dropped;	// Bytes received after rxBuf was full
	uint32_t overrun;	// RX FIFO overrun, data lost
	uint32_t framing;	// Missing stop bit
	uint32_t breaks;	// Break condition on RX line
	uint32_t parity;	// Parity error (parity checking is off, so always 0)
} UartMetrics;

static volatile UartMetrics metrics = {0, 0, 0, 0, 0, 0};

// Results of comparing received data to sent data
typedef struct {
	uint32_t valid;		// Bytes received correctly
	uint32_t missing;	// Bytes sent but never received
	uint32_t corrupt;	// Bytes received in place of a sent byte, but wrong
	uint32_t extra;		// Bytes received that do not line up with any sent
} TestResults;

static TestResults results = {0, 0, 0, 0};

// Core free % from the last test, for the display. -1 before the first
static int32_t lastFreePct = -1;

typedef enum {
	TEST_IDLE,
	TEST_SENDING,
	TEST_WAITING
} TestState;

/***
 * PIO interrupt handler.
 * Fires when the RX FIFO is not empty or uart_rx flags a framing error.
 * Store each received character in rxBuf and count errors
 */
void onPioRx() {
	// Framing error or break, raised by uart_rx. The bad character is
	// not pushed to the FIFO
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

		metrics.received++;
		lastRxUs = time_us_32();
		if (rxCount < RX_BUF_LEN) {
			rxBuf[rxCount] = c;
			rxCount = rxCount + 1;
		} else {
			metrics.dropped++;
		}
	}
}

/***
 * Clear the receive buffer, UART metrics and test results.
 * Only call with the PIO IRQ disabled so the ISR can't race us
 */
void clearAll() {
	// Discard anything left in the RX FIFO, and clear error flags
	pio_sm_clear_fifos(pio, smRx);
	pio_interrupt_clear(pio, RX_ERR_IRQ_FLAG(smRx));
	pio->fdebug = 1u << (PIO_FDEBUG_RXSTALL_LSB + smRx);

	rxCount = 0;
	metrics.received = 0;
	metrics.dropped  = 0;
	metrics.overrun  = 0;
	metrics.framing  = 0;
	metrics.breaks   = 0;
	metrics.parity   = 0;

	results.valid   = 0;
	results.missing = 0;
	results.corrupt = 0;
	results.extra   = 0;
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

	irq_add_shared_handler(pioIrq, onPioRx,
			PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);

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
 * Setup the PIO UART state machines.
 * Can be called again to change baud rate: the state machines are reset
 * and all metrics cleared.
 * Returns the actual baud rate achieved.
 */
uint32_t setupUart(uint32_t baud) {
	// Stop receive interrupts and the state machines
	irq_set_enabled(pioIrq, false);
	pio_sm_set_enabled(pio, smTx, false);
	pio_sm_set_enabled(pio, smRx, false);

	// The init functions from the .pio files restart each state machine
	uart_tx_program_init(pio, smTx, offsetTx, ECHO_TX_PIN, baud);
	uart_rx_program_init(pio, smRx, offsetRx, ECHO_RX_PIN, baud);

	clearAll();

	irq_set_enabled(pioIrq, true);

	return actualBaud(baud);
}

/***
 * Fill the transmit buffer with random printable characters (32 to 126)
 */
void fillTestData() {
	for (uint32_t i = 0; i < TEST_LEN; i++) {
		txBuf[i] = (char)(32 + (get_rand_32() % 95));
	}
}

/***
 * Check whether tx from txPos and rx from rxPos agree for MATCH_LEN bytes,
 * or for fewer if either buffer ends first.
 * At least minLen bytes must be compared for a match.
 */
bool runMatches(const char *rx, int rxLen, int txPos, int rxPos, int minLen) {
	int len = MATCH_LEN;
	if (TEST_LEN - txPos < len) {
		len = TEST_LEN - txPos;
	}
	if (rxLen - rxPos < len) {
		len = rxLen - rxPos;
	}
	if (len < minLen) {
		return false;
	}
	for (int k = 0; k < len; k++) {
		if (txBuf[txPos + k] != rx[rxPos + k]) {
			return false;
		}
	}
	return true;
}

/***
 * Compare received data to the sent data.
 * Walks both buffers together. On a mismatch it works out which of these
 * explains the data that follows:
 *   - Corrupt: one byte is wrong but the bytes after it line up
 *   - Missing: some sent bytes never arrived, rx lines up further on in tx
 *   - Extra:   a byte arrived that was never sent, the next rx byte lines up
 * If none fit, the byte is counted as corrupt.
 */
void analyse(const char *rx, int rxLen, TestResults *pRes) {
	int i = 0;	// Position in tx
	int j = 0;	// Position in rx

	pRes->valid   = 0;
	pRes->missing = 0;
	pRes->corrupt = 0;
	pRes->extra   = 0;

	while ((i < TEST_LEN) && (j < rxLen)) {
		if (txBuf[i] == rx[j]) {
			pRes->valid++;
			i++;
			j++;
			continue;
		}

		// Corrupt byte: the data after it lines up
		if (runMatches(rx, rxLen, i + 1, j + 1, 0)) {
			pRes->corrupt++;
			i++;
			j++;
			continue;
		}

		// Missing bytes: rx lines up with tx further on
		bool found = false;
		for (int k = 1; (i + k) < TEST_LEN; k++) {
			if (runMatches(rx, rxLen, i + k, j, 1)) {
				pRes->missing += k;
				i += k;
				found = true;
				break;
			}
		}
		if (found) {
			continue;
		}

		// Extra byte: the next rx byte lines up with this tx byte
		if (runMatches(rx, rxLen, i, j + 1, 1)) {
			pRes->extra++;
			j++;
			continue;
		}

		// Can't explain it, treat as a corrupt byte
		pRes->corrupt++;
		i++;
		j++;
	}

	pRes->missing += TEST_LEN - i;
	pRes->extra   += rxLen - j;
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
 * Print the menu on stdio
 */
void printMenu(uint32_t current) {
	printf("\nPIO UART Load Test - select baud rate (current %lu)\n",
			(unsigned long)current);
	for (uint32_t i = 0; i < NUM_BAUD_RATES; i++) {
		char key = (i < 10) ? ('0' + i) : ('a' + i - 10);
		printf("  %c : %lu%s\n", key, (unsigned long)baudRates[i],
				(baudRates[i] > HW_UART_MAX_BAUD) ? " (loop jumper only)" : "");
	}
	printf("  t : run test now\n");
	printf("> ");
}

/***
 * Format a short metric "Lnnnn" for a half width column at scale 2.
 * A column is 64px = 5 chars, so values over 9999 are shown as ">9k"
 */
void fmtShort(char *buf, size_t len, char label, uint32_t value) {
	if (value > 9999) {
		snprintf(buf, len, "%c>9k", label);
	} else {
		snprintf(buf, len, "%c%lu", label, (unsigned long)value);
	}
}

/***
 * Draw results and metrics on the Oled Screen.
 * Scale 2 text (12x16 px per char), 4 rows of two 5 char columns:
 *   Row 0: V valid      M missing
 *   Row 1: C corrupt    R received
 *   Row 2: O overrun    F framing
 *   Row 3: B break      core free % from the last test
 * Parity is always 0, so it is only reported on the console
 * Metrics are read without disabling interrupts, so they may be from
 * slightly different moments, but each 32 bit value is read atomically.
 */
void drawStatus(ssd1306_t *pOled){
	const uint32_t col2 = 64;
	char line[16];

	fmtShort(line, sizeof(line), 'V', results.valid);
	ssd1306_draw_string(pOled, 0, 0, 2, line);
	fmtShort(line, sizeof(line), 'M', results.missing);
	ssd1306_draw_string(pOled, col2, 0, 2, line);

	fmtShort(line, sizeof(line), 'C', results.corrupt);
	ssd1306_draw_string(pOled, 0, 16, 2, line);
	fmtShort(line, sizeof(line), 'R', metrics.received);
	ssd1306_draw_string(pOled, col2, 16, 2, line);

	fmtShort(line, sizeof(line), 'O', metrics.overrun);
	ssd1306_draw_string(pOled, 0, 32, 2, line);
	fmtShort(line, sizeof(line), 'F', metrics.framing);
	ssd1306_draw_string(pOled, col2, 32, 2, line);

	fmtShort(line, sizeof(line), 'B', metrics.breaks);
	ssd1306_draw_string(pOled, 0, 48, 2, line);
	if (lastFreePct < 0) {
		snprintf(line, sizeof(line), "--%%");
	} else {
		snprintf(line, sizeof(line), "%ld%%", (long)lastFreePct);
	}
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

	//Setup the PIO UART
	claimPio();
	uint32_t requestedBaud = ECHO_BAUD;
	uint32_t baud = setupUart(requestedBaud);
	printMenu(baud);

	TestState state = TEST_IDLE;
	uint32_t testNum = 0;
	uint32_t txPos = 0;
	uint32_t startUs = 0;
	uint32_t sentUs = 0;
	uint32_t idleUs = 0;
	uint32_t deadlineUs = 0;
	uint32_t idleSpins = 0;		// Idle spins during the test
	float idleRate = 0.0f;		// Idle spins per us with a free core
	absolute_time_t nextTest = make_timeout_time_ms(1000);
	absolute_time_t nextRefresh = get_absolute_time();

	// Calibrate the idle spin rate. Run the waiting state with nothing
	// sent, so it ends on the idle timeout after IDLE_CAL_US
	bool calibrating = true;
	idleUs = IDLE_CAL_US;
	deadlineUs = IDLE_CAL_US * 2;
	startUs = time_us_32();
	sentUs = startUs;
	state = TEST_WAITING;

	while (true) { // Loop forever
		bool didWork = false;	// Main loop moved data this time round

		// Poll stdio for menu selection, only when idle so the console
		// doesn't take core time during a test
		int c = PICO_ERROR_TIMEOUT;
		if (state == TEST_IDLE) {
			c = getchar_timeout_us(10000);
		}
		if (c != PICO_ERROR_TIMEOUT) {
			if ((c == 't') || (c == 'T')) {
				nextTest = get_absolute_time();
			} else {
				int idx = menuKeyToIndex(c);
				if (idx < 0) {
					printMenu(baud);
				} else {
					// Abort any test in progress and change baud
					requestedBaud = baudRates[idx];
					baud = setupUart(requestedBaud);
					state = TEST_IDLE;
					printf("%c\nRequested %lu baud, actual %lu baud. "
							"Metrics reset.\n",
							c, (unsigned long)requestedBaud,
							(unsigned long)baud);
					if (requestedBaud > HW_UART_MAX_BAUD) {
						printf("Note: above the hardware UART echo server's "
								"limit, use echoPio or a loop jumper "
								"GP%d to GP%d\n",
								ECHO_TX_PIN, ECHO_RX_PIN);
					}
					printf("> ");
					nextTest = make_timeout_time_ms(1000);
				}
			}
		}

		switch (state) {
		case TEST_IDLE:
			if (time_reached(nextTest)) {
				nextTest = make_timeout_time_ms(TEST_PERIOD_MS);
				testNum++;

				// Clear everything before the test
				irq_set_enabled(pioIrq, false);
				clearAll();
				irq_set_enabled(pioIrq, true);

				fillTestData();

				// Time to send the block: 10 bits per byte
				uint32_t byteUs = 10000000 / baud + 1;
				uint32_t sendUs = byteUs * TEST_LEN;
				// Finish early when no data arrives for 50ms + 20 byte times
				idleUs = 50000 + (byteUs * 20);
				// Give up at twice the send time plus 500ms
				deadlineUs = (sendUs * 2) + 500000;

				txPos = 0;
				idleSpins = 0;
				startUs = time_us_32();
				state = TEST_SENDING;
			}
			break;

		case TEST_SENDING:
			// Fill the PIO TX FIFO without blocking
			while ((txPos < TEST_LEN) && !pio_sm_is_tx_fifo_full(pio, smTx)) {
				pio_sm_put(pio, smTx, (uint32_t)txBuf[txPos++]);
				didWork = true;
			}
			if (txPos >= TEST_LEN) {
				sentUs = time_us_32();
				state = TEST_WAITING;
			}
			break;

		case TEST_WAITING: {
			uint32_t now = time_us_32();
			uint32_t count = rxCount;
			uint32_t last = lastRxUs;
			const char *reason = NULL;

			if (count >= TEST_LEN) {
				reason = "complete";
			} else if (((now - sentUs) > idleUs) &&
					((count == 0) || ((now - last) > idleUs))) {
				reason = "idle timeout";
			} else if ((now - startUs) > deadlineUs) {
				reason = "overall timeout";
			}

			if (reason != NULL) {
				// Test window for the core load, before any wait for
				// stragglers
				uint32_t windowUs = now - startUs;

				if (calibrating) {
					calibrating = false;
					idleRate = (windowUs > 0) ?
							((float)idleSpins / (float)windowUs) : 0.0f;
					printf("\nIdle calibration: %lu spins in %lu us\n> ",
							(unsigned long)idleSpins, (unsigned long)windowUs);
					state = TEST_IDLE;
					break;
				}

				uint32_t freePct = 0;
				if ((idleRate > 0.0f) && (windowUs > 0)) {
					float f = (100.0f * (float)idleSpins) /
							((float)windowUs * idleRate);
					freePct = (f > 100.0f) ? 100 : (uint32_t)(f + 0.5f);
				}
				lastFreePct = (int32_t)freePct;
				// Wait a little for any stragglers past TEST_LEN
				if (count >= TEST_LEN) {
					sleep_us(idleUs);
				}
				count = rxCount;
				uint32_t endUs = (count > 0) ? lastRxUs : now;

				analyse((const char *)rxBuf, (int)count, &results);

				uint32_t elapsedUs = endUs - startUs;
				uint32_t bps = (elapsedUs > 0) ?
						(uint32_t)(((uint64_t)count * 8 * 1000000) / elapsedUs) : 0;

				printf("\nTest %lu @ %lu baud: %s\n",
						(unsigned long)testNum, (unsigned long)baud, reason);
				printf("  Sent %u, received %lu (dropped %lu), %lu us, %lu bit/s\n",
						TEST_LEN, (unsigned long)metrics.received,
						(unsigned long)metrics.dropped,
						(unsigned long)elapsedUs, (unsigned long)bps);
				printf("  Core busy %lu%%, free %lu%%\n",
						(unsigned long)(100 - freePct), (unsigned long)freePct);
				printf("  Valid %lu, missing %lu, corrupt %lu, extra %lu\n",
						(unsigned long)results.valid,
						(unsigned long)results.missing,
						(unsigned long)results.corrupt,
						(unsigned long)results.extra);
				printf("  Overrun %lu, framing %lu, break %lu, parity %lu\n> ",
						(unsigned long)metrics.overrun,
						(unsigned long)metrics.framing,
						(unsigned long)metrics.breaks,
						(unsigned long)metrics.parity);

				state = TEST_IDLE;
			}
			break;
		}
		}

		// Nothing to do this time round, so spin and count it as idle
		if ((state != TEST_IDLE) && !didWork) {
			busy_wait_at_least_cycles(IDLE_SPIN_CYCLES);
			idleSpins++;
		}

		// Refresh the screen every DELAY ms, but not during a test. A
		// refresh takes about 25ms of core time, which would distort the
		// core load figure and the timing
		if ((state == TEST_IDLE) && time_reached(nextRefresh)) {
			nextRefresh = make_timeout_time_ms(DELAY);
			if (res){
				ssd1306_clear(&oled);
				drawStatus(&oled);
				ssd1306_show(&oled);
			}
		}
	}

}
