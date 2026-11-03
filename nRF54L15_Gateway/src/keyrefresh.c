/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "keyrefresh.h"
#include "uart_cmd.h"   /* uart30_send */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zephyr/bluetooth/mesh.h>
#include <zephyr/bluetooth/mesh/cdb.h>
#include <zephyr/bluetooth/mesh/cfg_cli.h>
#include <zephyr/bluetooth/crypto.h>

/* bt_mesh_primary_addr() (the gateway's own primary unicast) */
uint16_t bt_mesh_primary_addr(void);

/* Bluetooth Mesh Key Refresh phase/transition values */
#define KR_PHASE_NORMAL   0x00
#define KR_PHASE_1	0x01
#define KR_PHASE_2	0x02
#define KR_TRANS_PHASE_2  0x02
#define KR_TRANS_PHASE_3  0x03

/* All provisioned nodes plus the local gateway (not stored in the CDB). */
#define KR_MAX_TARGETS    (CONFIG_BT_MESH_CDB_NODE_COUNT + 1)

/* Scratch buffer for the target list. Only ever touched by the single
 * serialized worker that runs key_refresh_run().
 */
static uint16_t kr_targets[KR_MAX_TARGETS];

/* Is app_idx one of the `count` selected AppKeys? */
static bool kr_app_selected(uint16_t app_idx, const uint16_t *sel, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (sel[i] == app_idx) {
			return true;
		}
	}
	return false;
}

/* Runs the full three-phase Key Refresh for one subnet, rolling its NetKey and */
/* every selected AppKey (`sel`/`sel_count`) that is bound to this subnet. */
static void key_refresh_subnet(const struct kr_subnet *target,
				   const uint16_t *sel, size_t sel_count,
				   uint16_t evict_addr, bool del_evicted)
{
	uint16_t refresh_net_idx = target->net_idx;
	struct bt_mesh_cdb_subnet *sub = bt_mesh_cdb_subnet_get(refresh_net_idx);
	uint16_t local_addr = bt_mesh_primary_addr();
	uint8_t new_key[16];
	/* One fresh value per CDB AppKey slot (only meaningful where ak_done[] is */
	/* set), indexed by the slot's position in bt_mesh_cdb.app_keys[]. */
	uint8_t new_app_keys[ARRAY_SIZE(bt_mesh_cdb.app_keys)][16];
	bool ak_done[ARRAY_SIZE(bt_mesh_cdb.app_keys)] = { 0 };
	size_t ak_count = 0;
	char resp[192];
	size_t n;
	int p1_fail = 0, p2_fail = 0, p3_fail = 0, ak_fail = 0;

	/* All key_refresh_run failures report under the "KeyRefresh" subject (not */
	/* "Error") so the website — which set its status to "running" the moment the */
	/* command was dispatched — routes them to its key-refresh handler and clears */
	/* that pending state instead of leaving the badge stuck. */
	if (!sub) {
		snprintf(resp, sizeof(resp), "key refresh: no subnet 0x%03x in database",
			refresh_net_idx);
		printk("%s\n", resp);
		uart30_send(resp, "KeyRefresh");
		return;
	}
	if (sub->kr_phase != KR_PHASE_NORMAL) {
		snprintf(resp, sizeof(resp),
			"key refresh: subnet 0x%03x already refreshing (phase 0x%02x)",
			refresh_net_idx, sub->kr_phase);
		printk("%s\n", resp);
		uart30_send(resp, "KeyRefresh");
		return;
	}
	/* Evicting the gateway's own node would leave every remote on the new key */
	/* while the gateway keeps the old one, partitioning it from the network it */
	/* provisions. Never allow it. */
	if (evict_addr && evict_addr == local_addr) {
		snprintf(resp, sizeof(resp),
			"key refresh: refusing to evict gateway 0x%04x", local_addr);
		printk("%s\n", resp);
		uart30_send(resp, "KeyRefresh");
		return;
	}

	/* One fresh random NetKey shared by every node. The same value MUST reach */
	/* all nodes, so we generate it once here rather than letting each config */
	/* command mint its own (the shell would fall back to the default key). */
	if (bt_rand(new_key, sizeof(new_key))) {
		uart30_send("key refresh: RNG failed", "KeyRefresh");
		return;
	}

	/* Target list: the nodes the caller says hold this subnet (the gateway */
	/* does not track which subnets each node holds), then the local gateway, */
	/* which holds every subnet and goes last so it keeps the old key until */
	/* every remote has switched. */
	n = 0;
	for (size_t i = 0; i < target->addr_count && n < KR_MAX_TARGETS - 1; i++) {
		uint16_t addr = target->addrs[i];

		if (addr == evict_addr || addr == local_addr) {
			continue;
		}
		if (!bt_mesh_cdb_node_get(addr)) {
			printk("  0x%04x not in database, skipping\n", addr);
			continue;
		}
		kr_targets[n++] = addr;
	}
	if (local_addr) {
		kr_targets[n++] = local_addr;
	}
	if (n == 0) {
		uart30_send("key refresh: no target nodes", "KeyRefresh");
		return;
	}

	printk("Key refresh on subnet 0x%03x: %u node(s)%s\n", refresh_net_idx,
	   (unsigned int)n, evict_addr ? " (evicting one)" : "");
	snprintf(resp, sizeof(resp), "started subnet 0x%03x nodes=%u evict=0x%04x",
		 refresh_net_idx, (unsigned int)n, evict_addr);
	uart30_send(resp, "KeyRefresh");

	/* ---- Phase 1: distribute the new key (§3.11.4.1) ---- */
	/* Record the new key in the database as the "new" key (index 1) first, then */
	/* push it to each node. A NetKey Update moves a node from Normal to Phase 1; */
	/* it then keeps TX on the old key and RX on both keys. */
	bt_mesh_cdb_subnet_key_import(sub, 1, new_key);
	sub->kr_phase = KR_PHASE_1;
	bt_mesh_cdb_subnet_store(sub);

	for (size_t i = 0; i < n; i++) {
		uint8_t status = 0;
		int err = bt_mesh_cfg_cli_net_key_update(refresh_net_idx, kr_targets[i],
							refresh_net_idx, new_key, &status);
		if (err || status) {
			p1_fail++;
			printk("  P1 net-key-update 0x%04x FAILED (err %d, status 0x%02x)\n",
			kr_targets[i], err, status);
		}
	}

	/* The selected AppKeys on this subnet, refreshed while still in Phase 1. */
	for (size_t ak = 0; ak < ARRAY_SIZE(bt_mesh_cdb.app_keys); ak++) {
		struct bt_mesh_cdb_app_key *key = &bt_mesh_cdb.app_keys[ak];

		/* Only refresh selected AppKeys bound to this NetKey. */
		if (key->net_idx != refresh_net_idx ||
			!kr_app_selected(key->app_idx, sel, sel_count)) {
			continue;
		}
		if (bt_rand(new_app_keys[ak], sizeof(new_app_keys[ak]))) {
			printk("  AppKey 0x%04x: RNG failed, skipping\n", key->app_idx);
			continue;
		}

		bt_mesh_cdb_app_key_import(key, 1, new_app_keys[ak]);
		ak_done[ak] = true;
		ak_count++;

		for (size_t i = 0; i < n; i++) {
			uint8_t status = 0;
			int err = bt_mesh_cfg_cli_app_key_update(refresh_net_idx, kr_targets[i],
								refresh_net_idx, key->app_idx,
								new_app_keys[ak], &status);
			if (err) {
				ak_fail++;
				printk("  P1 app-key-update 0x%04x appidx=0x%04x FAILED (err %d)\n",
					kr_targets[i], key->app_idx, err);
			} else if (status) {
				printk(
			"  P1 app-key-update 0x%04x appidx=0x%04x not applicable (status 0x%02x)\n",
					kr_targets[i], key->app_idx, status);
			}
		}
	}

	/* ---- Phase 2: switch every node's TX to the new key (§3.11.4.2) ---- */
	for (size_t i = 0; i < n; i++) {
		uint8_t status = 0, phase = 0;
		int err = bt_mesh_cfg_cli_krp_set(refresh_net_idx, kr_targets[i],
						refresh_net_idx, KR_TRANS_PHASE_2,
						&status, &phase);
		if (err || status || phase != KR_PHASE_2) {
			p2_fail++;
			printk("  P2 krp-set 0x%04x FAILED (err %d, status 0x%02x, phase 0x%02x)\n",
				kr_targets[i], err, status, phase);
		}
	}
	sub->kr_phase = KR_PHASE_2;
	bt_mesh_cdb_subnet_store(sub);

	/* ---- Phase 3: revoke the old key (§3.11.4.3) ---- */
	/* Iterating in build order revokes the local gateway last, so it keeps a */
	/* working old key until every remote has switched. Each node settles back */
	/* to Normal (0x00) once the old key is revoked. */
	for (size_t i = 0; i < n; i++) {
		uint8_t status = 0, phase = 0;
		int err = bt_mesh_cfg_cli_krp_set(refresh_net_idx, kr_targets[i],
						refresh_net_idx, KR_TRANS_PHASE_3,
						&status, &phase);
		if (err || status || phase != KR_PHASE_NORMAL) {
			p3_fail++;
			printk("  P3 krp-set 0x%04x FAILED (err %d, status 0x%02x, phase 0x%02x)\n",
				kr_targets[i], err, status, phase);
		}
	}

	/* The new key is now the sole key on the network; mirror that in the CDB. */
	bt_mesh_cdb_subnet_key_import(sub, 0, new_key);
	sub->kr_phase = KR_PHASE_NORMAL;
	bt_mesh_cdb_subnet_store(sub);

	/* Same promotion for every AppKey that was refreshed above. */
	for (size_t ak = 0; ak < ARRAY_SIZE(bt_mesh_cdb.app_keys); ak++) {
		if (!ak_done[ak]) {
			continue;
		}
		struct bt_mesh_cdb_app_key *key = &bt_mesh_cdb.app_keys[ak];

		bt_mesh_cdb_app_key_import(key, 0, new_app_keys[ak]);
		bt_mesh_cdb_app_key_store(key);
	}

	/* An evicted node never received the new key and is now off the network, so */
	/* drop it from the database as well. Skipped when several subnets are */
	/* refreshed: key_refresh_run removes the node once, after the last subnet. */
	if (del_evicted && evict_addr) {
		struct bt_mesh_cdb_node *gone = bt_mesh_cdb_node_get(evict_addr);

		if (gone) {
			bt_mesh_cdb_node_del(gone, true);
			printk("Evicted node 0x%04x removed from database\n", evict_addr);
		}
	}

	snprintf(resp, sizeof(resp),
		 "complete subnet 0x%03x nodes=%u appkeys=%u fails p1=%d p2=%d p3=%d ak=%d%s",
		 refresh_net_idx, (unsigned int)n, (unsigned int)ak_count,
		 p1_fail, p2_fail, p3_fail, ak_fail,
		 evict_addr ? " evicted" : "");
	printk("Key refresh %s\n", resp);
	uart30_send(resp, "KeyRefresh");
}

/* Refresh the selected subnets' NetKeys (`subnets`/`subnet_count`), each across
 * the nodes listed with it, optionally rolling the selected AppKeys
 * (`app_idxs`/`app_count`) alongside their own subnet's NetKey.
 */
void key_refresh_run(const struct kr_subnet *subnets, size_t subnet_count,
		     const uint16_t *app_idxs, size_t app_count,
		     uint16_t evict_addr)
{
	char resp[96];

	if (subnet_count == 0) {
		uart30_send("key refresh: no subnet selected", "KeyRefresh");
		return;
	}

	/* An AppKey is only rolled with its own subnet, whose node list must be */
	/* in the command - flag any whose subnet was not selected. */
	for (size_t i = 0; i < app_count; i++) {
		struct bt_mesh_cdb_app_key *key = bt_mesh_cdb_app_key_get(app_idxs[i]);
		bool selected = false;

		if (!key) {
			snprintf(resp, sizeof(resp),
				"key refresh: app_idx 0x%03x not in database", app_idxs[i]);
			printk("%s\n", resp);
			uart30_send(resp, "KeyRefresh");
			continue;
		}
		for (size_t j = 0; j < subnet_count; j++) {
			if (subnets[j].net_idx == key->net_idx) {
				selected = true;
				break;
			}
		}
		if (!selected) {
			snprintf(resp, sizeof(resp),
				"key refresh: app_idx 0x%03x skipped, its subnet 0x%03x not selected",
				app_idxs[i], key->net_idx);
			printk("%s\n", resp);
			uart30_send(resp, "KeyRefresh");
		}
	}

	/* Refresh each subnet once, passing the full AppKey selection; the subnet
	 * routine rolls its NetKey plus the selected AppKeys bound to it.
	 * Defer the evicted-node removal to after the last subnet.
	 */
	for (size_t j = 0; j < subnet_count; j++) {
		key_refresh_subnet(&subnets[j], app_idxs, app_count, evict_addr,
				/* del_evicted = */ false);
	}

	/* Each subnet refused to evict the gateway itself (it must stay on the new */
	/* key), so never drop the local node here either - only a genuine remote. */
	if (evict_addr && evict_addr != bt_mesh_primary_addr()) {
		struct bt_mesh_cdb_node *gone = bt_mesh_cdb_node_get(evict_addr);

		if (gone) {
			bt_mesh_cdb_node_del(gone, true);
			printk("Evicted node 0x%04x removed from database\n", evict_addr);
		}
	}
}
