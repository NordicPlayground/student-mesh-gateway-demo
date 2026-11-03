/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef UART_CMD_H
#define UART_CMD_H

#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

void uart30_send(const char *data, const char *subject);


#ifdef __cplusplus
}
#endif

#endif /* UART_CMD_H */
