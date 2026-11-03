/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef KEYREFRESH_H
#define KEYREFRESH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One subnet to refresh and the remote nodes that hold it. The gateway is
 * always refreshed too, so it is not listed.
 */
struct kr_subnet {
	uint16_t net_idx;
	const uint16_t *addrs;
	size_t addr_count;
};

/* Run the Bluetooth Mesh Key Refresh on the selected
 * NetKeys and AppKeys
 */
void key_refresh_run(const struct kr_subnet *subnets, size_t subnet_count,
		     const uint16_t *app_idxs, size_t app_count,
		     uint16_t evict_addr);

#ifdef __cplusplus
}
#endif

#endif /* KEYREFRESH_H */
