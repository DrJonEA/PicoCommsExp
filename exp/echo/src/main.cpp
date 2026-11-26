/**
 * Jon Durrant.
 *
 * UART Echo on Raspberry PICO
 * Echo characters received on UART1 (RX GP21) back out on UART1 (TX GP20).
 * Receive is interrupt driven. The last 10 characters received are
 * displayed on the OLED screen, refreshed every 0.5 seconds.
 */

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
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

// Circular buffer of the last HISTORY_LEN characters received
static volatile char     history[HISTORY_LEN];
static volatile uint32_t historyHead  = 0;	// Next write position
static volatile uint32_t historyCount = 0;	// Number of valid chars

/***
 * UART RX interrupt handler.
 * Echo each received character to TX and store it in the history buffer
 */
void onUartRx() {
	while (uart_is_readable(ECHO_UART)) {
		char c = uart_getc(ECHO_UART);

		// Echo back out
		uart_putc_raw(ECHO_UART, c);

		// Record in history
		history[historyHead] = c;
		historyHead = (historyHead + 1) % HISTORY_LEN;
		if (historyCount < HISTORY_LEN) {
			historyCount++;
		}
	}
}

/***
 * Setup UART1 for echo with receive interrupt
 */
void setupUart() {
	uart_init(ECHO_UART, ECHO_BAUD);
	gpio_set_function(ECHO_TX_PIN, GPIO_FUNC_UART);
	gpio_set_function(ECHO_RX_PIN, GPIO_FUNC_UART);

	uart_set_hw_flow(ECHO_UART, false, false);
	uart_set_format(ECHO_UART, 8, 1, UART_PARITY_NONE);
	uart_set_fifo_enabled(ECHO_UART, true);

	irq_set_exclusive_handler(ECHO_UART_IRQ, onUartRx);
	irq_set_enabled(ECHO_UART_IRQ, true);

	// Enable RX interrupt only (RX FIFO level and RX timeout)
	uart_set_irq_enables(ECHO_UART, true, false);
}

/***
 * Copy the history, oldest first, into buf as a null terminated string.
 * Non printable characters are shown as '.'
 */
void getHistory(char *buf) {
	uint32_t irqState = save_and_disable_interrupts();
	uint32_t count = historyCount;
	uint32_t start = (historyHead + HISTORY_LEN - count) % HISTORY_LEN;
	for (uint32_t i = 0; i < count; i++) {
		buf[i] = history[(start + i) % HISTORY_LEN];
	}
	restore_interrupts(irqState);

	for (uint32_t i = 0; i < count; i++) {
		if ((buf[i] < 32) || (buf[i] > 126)) {
			buf[i] = '.';
		}
	}
	buf[count] = 0;
}

/***
 * Draw the received history on the Oled Screen
 */
void drawHistory(ssd1306_t *pOled){
	char buf[HISTORY_LEN + 1];
	getHistory(buf);

	ssd1306_draw_string(
			pOled,
			0,
			2,
			2,
			"UART1 Echo");

	ssd1306_draw_line(
			pOled,
			2, 25,
			120, 25);

	ssd1306_draw_string(
			pOled,
			0,
			35,
			2,
			buf);
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
	setupUart();

	while (true) { // Loop forever
		if (res){
			ssd1306_clear(&oled);
			drawHistory(&oled);
			ssd1306_show(&oled);
		}
		sleep_ms(DELAY);
	}

}
