/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef GTW_SHELL_H
#define GTW_SHELL_H

#define STATUS_GROUP_ADDR 0xC000

bool queue_shell_cmd(const char *cmd);

#endif /* GTW_SHELL_H */
