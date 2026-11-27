/**
 * Jon Durrant.
 *
 * UART Load Test on Raspberry PICO 2, using the UART AUX pin function
 * Streams a 1KB block of random printable characters out of UART1
 * (TX GP26) to an echo server and reads the echo back on UART1 (RX GP27).
 *
 * GP26 and GP27 are normally UART1 CTS and RTS. On the RP2350 the UART_AUX
 * function (F11) swaps these to UART1 TX and RX.
 * Receive is interrupt driven into a 1KB + contingency buffer.
 *
 * Once all data is back, or a timeout expires, the received data is
 * compared to what was sent to count valid, missing, corrupt and extra
 * bytes. The test repeats every 10 seconds, with all metrics cleared first.
 *
 * Results and UART error metrics (Overrun, Framing, Break and Parity) are
 * shown on the OLED screen and printed on stdio. A stdio menu selects the
 * baud rate.
 */

#include "pico/stdlib.h"
#include "pico/rand.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
extern "C"{
#include <ssd1306.h>
}
#include <cstdio>

#define DELAY 500 // Screen refresh in milliseconds

#define ECHO_UART      uart1
#define ECHO_UART_IRQ  UART1_IRQ
#define ECHO_BAUD      115200
#define ECHO_TX_PIN    26	// UART1 TX via UART_AUX
#define ECHO_RX_PIN    27	// UART1 RX via UART_AUX

#define TEST_LEN        1024				// Bytes sent per test
#define RX_BUF_LEN      (TEST_LEN + 256)	// Receive buffer with contingency
#define TEST_PERIOD_MS  10000				// Start a test every 10 seconds
#define MATCH_LEN       4					// Bytes that must agree to resync

// Baud rates selectable from the stdio menu, keys 0-9 then a-g.
// 9375000 is the UART maximum: clk_peri (150MHz on Pico 2) / 16
static const uint32_t baudRates[] = {
	9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600,
	1000000, 2000000, 3000000, 4000000, 5000000, 6000000, 7000000, 8000000,
	9375000
};
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

typedef enum {
	TEST_IDLE,
	TEST_SENDING,
	TEST_WAITING
} TestState;

/***
 * UART RX interrupt handler.
 * Store each received character in rxBuf and count errors
 */
void onUartRx() {
	uart_hw_t *hw = uart_get_hw(ECHO_UART);

	while (uart_is_readable(ECHO_UART)) {
		// Read the data register directly, rather than uart_getc, so we
		// get the error flags that go with each character
		uint32_t dr = hw->dr;
		char c = (char)(dr & UART_UARTDR_DATA_BITS);

		if (dr & UART_UARTDR_OE_BITS) {
			// Character is valid, but data was lost before it
			metrics.overrun++;
		}
		if (dr & UART_UARTDR_BE_BITS) {
			metrics.breaks++;
		}
		if (dr & UART_UARTDR_FE_BITS) {
			metrics.framing++;
		}
		if (dr & UART_UARTDR_PE_BITS) {
			metrics.parity++;
		}

		// Keep corrupted characters too, the analysis will find them
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
 * Only call with the UART IRQ disabled so the ISR can't race us
 */
void clearAll() {
	// Discard anything left in the RX FIFO
	while (uart_is_readable(ECHO_UART)) {
		(void)uart_get_hw(ECHO_UART)->dr;
	}

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
 * Setup UART1 with receive interrupt.
 * Can be called again to change baud rate: the UART is reset first and
 * all metrics cleared.
 * Returns the actual baud rate achieved.
 */
uint32_t setupUart(uint32_t baud) {
	// Stop receive interrupts and reset the UART
	irq_set_enabled(ECHO_UART_IRQ, false);
	uart_deinit(ECHO_UART);

	uint32_t actual = uart_init(ECHO_UART, baud);
	// UART_AUX makes the CTS/RTS pins act as TX/RX
	gpio_set_function(ECHO_TX_PIN, GPIO_FUNC_UART_AUX);
	gpio_set_function(ECHO_RX_PIN, GPIO_FUNC_UART_AUX);

	uart_set_hw_flow(ECHO_UART, false, false);
	uart_set_format(ECHO_UART, 8, 1, UART_PARITY_NONE);
	uart_set_fifo_enabled(ECHO_UART, true);

	clearAll();

	irq_set_exclusive_handler(ECHO_UART_IRQ, onUartRx);
	irq_set_enabled(ECHO_UART_IRQ, true);

	// Enable RX interrupt only (RX FIFO level and RX timeout)
	uart_set_irq_enables(ECHO_UART, true, false);

	return actual;
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
	printf("\nUART1 Load Test - select baud rate (current %lu)\n",
			(unsigned long)current);
	for (uint32_t i = 0; i < NUM_BAUD_RATES; i++) {
		char key = (i < 10) ? ('0' + i) : ('a' + i - 10);
		printf("  %c : %lu\n", key, (unsigned long)baudRates[i]);
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
 *   Row 3: B break      P parity
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
	fmtShort(line, sizeof(line), 'P', metrics.parity);
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

	//Setup the UART
	uint32_t baud = setupUart(ECHO_BAUD);
	printMenu(baud);

	TestState state = TEST_IDLE;
	uint32_t testNum = 0;
	uint32_t txPos = 0;
	uint32_t startUs = 0;
	uint32_t sentUs = 0;
	uint32_t idleUs = 0;
	uint32_t deadlineUs = 0;
	absolute_time_t nextTest = make_timeout_time_ms(1000);
	absolute_time_t nextRefresh = get_absolute_time();

	while (true) { // Loop forever
		// Poll stdio for menu selection. Only wait when idle so the test
		// isn't slowed down
		int c = getchar_timeout_us((state == TEST_IDLE) ? 10000 : 0);
		if (c != PICO_ERROR_TIMEOUT) {
			if ((c == 't') || (c == 'T')) {
				nextTest = get_absolute_time();
			} else {
				int idx = menuKeyToIndex(c);
				if (idx < 0) {
					printMenu(baud);
				} else {
					// Abort any test in progress and change baud
					uint32_t requested = baudRates[idx];
					baud = setupUart(requested);
					state = TEST_IDLE;
					printf("%c\nRequested %lu baud, actual %lu baud. "
							"Metrics reset.\n> ",
							c, (unsigned long)requested,
							(unsigned long)baud);
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
				irq_set_enabled(ECHO_UART_IRQ, false);
				clearAll();
				irq_set_enabled(ECHO_UART_IRQ, true);

				fillTestData();

				// Time to send the block: 10 bits per byte
				uint32_t byteUs = 10000000 / baud + 1;
				uint32_t sendUs = byteUs * TEST_LEN;
				// Finish early when no data arrives for 50ms + 20 byte times
				idleUs = 50000 + (byteUs * 20);
				// Give up at twice the send time plus 500ms
				deadlineUs = (sendUs * 2) + 500000;

				txPos = 0;
				startUs = time_us_32();
				state = TEST_SENDING;
			}
			break;

		case TEST_SENDING:
			// Fill the TX FIFO without blocking
			while ((txPos < TEST_LEN) && uart_is_writable(ECHO_UART)) {
				uart_putc_raw(ECHO_UART, txBuf[txPos++]);
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

		// Refresh the screen every DELAY ms, but not while sending as the
		// I2C transfer would stall the TX stream
		if ((state != TEST_SENDING) && time_reached(nextRefresh)) {
			nextRefresh = make_timeout_time_ms(DELAY);
			if (res){
				ssd1306_clear(&oled);
				drawStatus(&oled);
				ssd1306_show(&oled);
			}
		}
	}

}
