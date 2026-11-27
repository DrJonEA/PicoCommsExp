/**
 * Jon Durrant.
 *
 * UART Echo on Raspberry PICO
 * Echo characters received on UART1 (RX GP21) back out on UART1 (TX GP20).
 * Receive is interrupt driven. The last 10 characters received are
 * displayed on the OLED screen, refreshed every 0.5 seconds, along with
 * metrics for bytes echoed and receive errors (Overrun, Framing, Break and
 * Parity).
 */

#include "pico/stdlib.h"
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
#define ECHO_TX_PIN    20
#define ECHO_RX_PIN    21

#define HISTORY_LEN    10

// Baud rates selectable from the stdio menu, keys 0-9 then a-g.
// 9375000 is the UART maximum: clk_peri (150MHz on Pico 2) / 16
static const uint32_t baudRates[] = {
	9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600,
	1000000, 2000000, 3000000, 4000000, 5000000, 6000000, 7000000, 8000000,
	9375000
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
 * UART RX interrupt handler.
 * Echo each received character to TX and store it in the history buffer
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

		// Don't echo characters which arrived corrupted
		if (dr & (UART_UARTDR_BE_BITS | UART_UARTDR_FE_BITS |
				UART_UARTDR_PE_BITS)) {
			continue;
		}

		// Echo back out
		uart_putc_raw(ECHO_UART, c);
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
 * Setup UART1 for echo with receive interrupt.
 * Can be called again to change baud rate: the UART is reset first.
 * Returns the actual baud rate achieved.
 */
uint32_t setupUart(uint32_t baud) {
	// Stop receive interrupts and reset the UART
	irq_set_enabled(ECHO_UART_IRQ, false);
	uart_deinit(ECHO_UART);

	uint32_t actual = uart_init(ECHO_UART, baud);
	gpio_set_function(ECHO_TX_PIN, GPIO_FUNC_UART);
	gpio_set_function(ECHO_RX_PIN, GPIO_FUNC_UART);

	uart_set_hw_flow(ECHO_UART, false, false);
	uart_set_format(ECHO_UART, 8, 1, UART_PARITY_NONE);
	uart_set_fifo_enabled(ECHO_UART, true);

	// Zero the metrics and clear the history.
	// The UART IRQ is off, so the ISR can't race us
	historyHead  = 0;
	historyCount = 0;
	metrics.echoed  = 0;
	metrics.overrun = 0;
	metrics.framing = 0;
	metrics.breaks  = 0;
	metrics.parity  = 0;

	irq_set_exclusive_handler(ECHO_UART_IRQ, onUartRx);
	irq_set_enabled(ECHO_UART_IRQ, true);

	// Enable RX interrupt only (RX FIFO level and RX timeout)
	uart_set_irq_enables(ECHO_UART, true, false);

	return actual;
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
	printf("\nUART1 Echo - select baud rate (current %lu)\n",
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

	//Setup the echo UART
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
