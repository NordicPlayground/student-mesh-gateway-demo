/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "uart_cmd.h"
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdio.h>
#include "gtw_shell.h"

#define CMD_BUFFER_SIZE 256

static const struct device *uart_dev;

/* mutex to serialize UART TX so multiple threads can send messages without interleaving */
static K_MUTEX_DEFINE(tx_mutex);

/* removes ANSI escape codes and other unwanted characters from the input string */
size_t strip_ansi_escapes(const char *src, size_t src_len, char *dst, size_t dst_size)
{
	size_t dst_pos = 0;
	size_t i = 0;

	while (i < src_len && dst_pos < dst_size - 1) {
		if (src[i] == 0x1b) {
			i++;
			if (i < src_len && src[i] == '[') {
				i++;
				while (i < src_len && ((src[i] < '@' || src[i] > '~'))) {
					i++;
				}
				if (i < src_len) {
					i++;
				}
			}
		} else if (src[i] == '~' && i + 2 < src_len && src[i+1] == '$' && src[i+2] == ' ') {
			i += 3;
		} else if (src[i] == '\r') {
			i++;
		} else {
			dst[dst_pos++] = src[i++];
		}
	}
	dst[dst_pos] = '\0';
	return dst_pos;
}

/* sends string over uart to nRF9151 */
void uart30_send(const char *data, const char *subject)
{
	if (!uart_dev || !device_is_ready(uart_dev)) {
		return;
	}
	static char json_buf[CONFIG_SHELL_BACKEND_DUMMY_BUF_SIZE];

	snprintf(json_buf, sizeof(json_buf), "%s %s", subject, data);
	size_t json_len = strip_ansi_escapes(json_buf, strlen(json_buf), json_buf,
					      sizeof(json_buf));

	/* Lock down the UART send so that multiple threads don't send at the same time */
	k_mutex_lock(&tx_mutex, K_FOREVER);
	for (size_t i = 0; i < json_len; i++) {
		uart_poll_out(uart_dev, json_buf[i]);

		/* for each new line, send the subject first again */
		/* nrf9151 expects the first word of a line to be the subject */
		if (json_buf[i] == '\n') {
			for (int i = 0; i < strlen(subject); i++) {
				uart_poll_out(uart_dev, subject[i]);

			}
			uart_poll_out(uart_dev, ' ');
		}
	}
	uart_poll_out(uart_dev, '\n');
	k_mutex_unlock(&tx_mutex);
}

/* receives uart commands from uart and queues them for execution */
static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	static char uart_buffer[CMD_BUFFER_SIZE];
	static size_t uart_buffer_pos;
	uint8_t c;

	if (!uart_irq_update(dev)) {
		return;
	}
	if (!uart_irq_rx_ready(dev)) {
		return;
	}
	while (uart_fifo_read(dev, &c, 1) == 1) {
		if (c == '\r' || c == '\n') {
			if (uart_buffer_pos > 0) {
				/* queue command and reset buffer */
				queue_shell_cmd(uart_buffer);
				uart_buffer_pos = 0;
				memset(uart_buffer, 0, sizeof(uart_buffer));
			} else {
				printk("Received empty command, ignoring\n");
			}

		} else if (uart_buffer_pos < CMD_BUFFER_SIZE - 1) {
			/* add character to buffer */
			uart_buffer[uart_buffer_pos++] = (char)c;
		} else {
			printk("Command buffer overflow, resetting\n");
			uart_buffer_pos = 0;
			memset(uart_buffer, 0, sizeof(uart_buffer));
		}
	}
}

int uart_cmd_init(void)
{
	int err;

	uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart30));
	if (!device_is_ready(uart_dev)) {
		printk("UART command device not ready\n");
		return -ENODEV;
	}
	err = uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);
	if (err < 0) {
		if (err == -ENOTSUP) {
			printk("Interrupt-driven UART API not enabled\n");
		}
		return err;
	}
	uart_irq_rx_enable(uart_dev);

	printk("UART command reception initialized on UART30\n");
	printk("TX: P0.0, RX: P0.1, 115200 baud\n");
	return 0;
}
