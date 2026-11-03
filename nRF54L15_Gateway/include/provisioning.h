/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef PROVISIONING_H
#define PROVISIONING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Control/status group the gateway subscribes its own client models to, and the
 * group fixtures publish their status to, so the cloud sees state changes from any
 * source. Shared by the local-model setup in uart_cmd.c and the remote-node
 * auto-configuration here.
 */
#define STATUS_GROUP_ADDR 0xC000

/* One-time provisioning subsystem setup */
void provisioning_init(void);

/* Ensure subnet `net_idx` exists in the CDB and is added to the gateway's own
 * node
 */
int prov_ensure_subnet(uint16_t net_idx, const char *netkey_hex);

/* Add subnet to an already-provisioned node
 */
int prov_node_add_subnet(uint16_t net_idx, uint16_t addr, uint16_t add_net_idx);

/* Remove subnet from a node */
int prov_node_del_subnet(uint16_t net_idx, uint16_t addr, uint16_t del_net_idx);

/* Remove one AppKey from a node */
int prov_node_del_appkey(uint16_t net_idx, uint16_t addr, uint16_t app_idx);

/* Remove a group address from a node: unsubscribe its models from the group and
 * clear any publication aimed at it.
 */
int prov_node_del_group(uint16_t net_idx, uint16_t addr, uint16_t group);

/* Toggle combined scanning: local unprovisioned beacons + remote RPR relays. */
void prov_scan_toggle(void);

/* Toggle REMOTE-ONLY scanning: RPR relays only, local beacons stay off. */
void prov_scan_rpr_toggle(void);

/* Enable/disable auto-provisioning of newly discovered unprovisioned devices. */
void prov_auto_set(bool enable);

/* net_idx/app_idx/group to use for provisioning. */
extern uint16_t cur_net_idx;
extern uint16_t cur_app_idx;
extern uint16_t cur_group;

/* Provision a discovered device (by UUID). Tunnels through the discovering RPR
 * relay if the UUID was seen remotely, otherwise provisions locally over PB-ADV.
 */
void prov_provision(const char *uuid);

/* Register a node address as a known RPR Server relay (dedup-insert). */
void prov_rpr_server_add(uint16_t addr);

/* Report the registered RPR Server relays to the cloud. */
void prov_rpr_server_list(void);

/* Read a freshly provisioned node's composition data and queue configuration
 * commands for the models the gateway recognizes.
 */
void configure_and_report(uint16_t addr, uint16_t net_idx, uint16_t app_idx,
			      uint16_t group);

#ifdef __cplusplus
}
#endif

#endif /* PROVISIONING_H */
