/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include "provisioning.h"
#include "uart_cmd.h"
#include "model_handler.h"
#include "gtw_shell.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zephyr/bluetooth/mesh.h>
#include <zephyr/bluetooth/mesh/cdb.h>
#include <zephyr/bluetooth/mesh/cfg_cli.h>
#include <zephyr/bluetooth/crypto.h>
#if defined(CONFIG_BT_MESH_SHELL)
#include <zephyr/bluetooth/mesh/shell.h>
#endif

/* bt_mesh_primary_addr() returns the gateway's own primary unicast address. It
 * lives in the mesh subsys, not a public include, so forward-declare it.
 */
uint16_t bt_mesh_primary_addr(void);

#define CMD_BUFFER_SIZE 128

#define RPR_SCAN_TIMEOUT_S   5       /* duration of each remote scan window */
#define RPR_RESCAN_PERIOD_S  30
#define RPR_POST_PROV_SETTLE_S  7
#define RPR_PROV_PAUSE_MAX_TICKS  75
/* Once there are no more unprovisioned devices to find, the periodic re-scan
 * would otherwise keep both relays in near-continuous BLE scan forever, which
 * starves their mesh RX/TX so the rpr_cli's Scan Status ack window times out
 * (err -116) round after round. Instead, suspend the re-scan after this many
 * rounds that turn up nothing new and resume on demand (a new beacon/relay
 * sighting, or a provisioning).
 */
#define RPR_MAX_EMPTY_ROUNDS  3
#define MAX_RPR_SERVERS      16
#define MAX_UNPROV    32

#define RPR_WORKQ_STACK_SIZE  4096
#define RPR_WORKQ_PRIO	5

#define MAX_NODE_MODELS 24

/* Subnet the gateway reaches an RPR Server relay on: the relay's own subnet,
 * looked up from its CDB node.
 */
static uint16_t relay_comms_net(uint16_t relay_addr)
{
	struct bt_mesh_cdb_node *node = bt_mesh_cdb_node_get(relay_addr);

	return node ? node->net_idx : 0;
}

/* Provisioning bookkeeping */
uint16_t cur_app_idx;
uint16_t cur_net_idx;
uint16_t cur_group;

/* Addresses of nodes that has the RPR server model */
static uint16_t rpr_servers[MAX_RPR_SERVERS];
static int num_rpr_servers;

/* True if the gateway should automatically provision unprovisioned devices
 * as soon as they are seen, without waiting for a user command.
 */
static bool auto_provision;

/* Local sightings carry no measured RSSI; treat them as the strongest possible */
/* link so direct provisioning is always preferred over tunneling via a relay. */
#define UNPROV_RSSI_LOCAL  INT8_MAX

/* Per-scan record of every unprovisioned UUID seen this scan window */
static struct {
	char uuid[33];
	uint16_t srv;
	int8_t rssi;
} unprov[MAX_UNPROV];
static int num_unprov;

/* Registers a node address as a known RPR Server (dedup-insert). */
void prov_rpr_server_add(uint16_t addr)
{
	for (int i = 0; i < num_rpr_servers; i++) {
		if (rpr_servers[i] == addr) {
			return;
		}
	}
	if (num_rpr_servers < MAX_RPR_SERVERS) {
		rpr_servers[num_rpr_servers++] = addr;
		printk("Registered RPR server 0x%04x (%d total)\n", addr, num_rpr_servers);
	} else {
		printk("RPR server list full, dropping 0x%04x\n", addr);
	}
}

/* Returns the RPR Server that discovered a UUID, or 0 if it was found locally. */
static uint16_t unprov_lookup(const char *uuid_str)
{
	uint16_t srv = 0;

	for (int i = 0; i < num_unprov; i++) {
		if (strcmp(unprov[i].uuid, uuid_str) == 0) {
			srv = unprov[i].srv;
			break;
		}
	}
	return srv;
}

/* Records the best source that reported an unprovisioned UUID */
static bool unprov_record(const char *uuid_str, uint16_t srv_addr, int8_t rssi)
{
	for (int i = 0; i < num_unprov; i++) {
		if (strcmp(unprov[i].uuid, uuid_str) == 0) {
			if (rssi > unprov[i].rssi) {
				unprov[i].srv = srv_addr;
				unprov[i].rssi = rssi;
			}
			return false;
		}
	}
	if (num_unprov < MAX_UNPROV) {
		snprintf(unprov[num_unprov].uuid,
				 sizeof(unprov[num_unprov].uuid), "%s", uuid_str);
		unprov[num_unprov].srv = srv_addr;
		unprov[num_unprov].rssi = rssi;
		num_unprov++;
	}
	/* Forward even if the list is full: better a duplicate (the website
	 * dedups) than a dropped device.
	 */
	return true;
}

/* True while a scan is active; gates the periodic remote re-scan. */
static bool scanning;
static bool rpr_scanning;

/* Set while a provisioning link is open / a node is being configured. */
static bool prov_active;
/* UUID of device currently being provisioned */
static char prov_cur_uuid[33];
static int  rpr_prov_pause_ticks;
static bool prov_node_added;

/* Clean up after failed provisioning */
static void prov_failed(const char *reason);

/* Set true (via rpr_note_new_device) whenever a new unprovisioned device is seen */
/* during the current scan window; reset at the start of each round. */
static bool rpr_round_found;
/* Consecutive rounds that found nothing new. */
static int  rpr_empty_rounds;
/* True while the re-scan is suspended after too many empty rounds. The user still */
/* wants scanning (rpr_scanning stays true); we just stopped re-arming the work. */
static bool rpr_scan_idle;
/* Skip the empty-round check on the first round after a (re)start/resume. */
static bool rpr_first_round;

/* Kicks off one remote-scan round on every known relay. */
static void rpr_scan_all_servers(void)
{
	for (int i = 0; i < num_rpr_servers; i++) {
		/* Each scan-start below blocks for up to the rpr_cli ack timeout, so a
		 * whole round spans several seconds. A provisioning may start (setting
		 * prov_active) while we are mid-round; bail immediately so we never
		 * issue a scan-start to a relay that just began hosting a provisioning
		 * link. The handler re-arms the next round normally.
		 */
		if (prov_active) {
			return;
		}
		struct bt_mesh_rpr_node srv = {
			.addr = rpr_servers[i],
			.net_idx = relay_comms_net(rpr_servers[i]),
			.ttl = BT_MESH_TTL_DEFAULT,
		};
		struct bt_mesh_rpr_scan_status status;
		int err = bt_mesh_rpr_scan_start(model_handler_rpr_cli(), &srv, NULL,
				  (uint8_t)RPR_SCAN_TIMEOUT_S,
				  BT_MESH_RPR_SCAN_MAX_DEVS_ANY, &status);
		if (err || status.status != BT_MESH_RPR_SUCCESS) {
			printk("RPR scan start on 0x%04x failed (err %d, status %u)\n",
				   rpr_servers[i], err, status.status);
		} else {
			printk("RPR scan started on 0x%04x (%us)\n",
				   rpr_servers[i], RPR_SCAN_TIMEOUT_S);
		}
	}
}

/* Tells every known relay to stop scanning, returning them to idle. */
static void rpr_scan_stop_all_servers(void)
{
	for (int i = 0; i < num_rpr_servers; i++) {
		struct bt_mesh_rpr_node srv = {
			.addr = rpr_servers[i],
			.net_idx = relay_comms_net(rpr_servers[i]),
			.ttl = BT_MESH_TTL_DEFAULT,
		};
		struct bt_mesh_rpr_scan_status status;
		(void)bt_mesh_rpr_scan_stop(model_handler_rpr_cli(), &srv, &status);
	}
}

static K_THREAD_STACK_DEFINE(rpr_workq_stack, RPR_WORKQ_STACK_SIZE);
static struct k_work_q rpr_workq;

static void rpr_rescan_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!rpr_scanning) {
		return;
	}
	/* A remote provisioning link is open through a relay */
	if (prov_active) {
		if (++rpr_prov_pause_ticks < RPR_PROV_PAUSE_MAX_TICKS) {
			k_work_reschedule_for_queue(&rpr_workq, k_work_delayable_from_work(work),
					 K_SECONDS(2));
			return;
		}
		/* Watchdog tripped. prov_failed() clears the pause, puts the target
		 * device back in play and re-arms this work itself.
		 */
		printk("RPR re-scan: provisioning didn't complete in time\n");
		prov_failed("timed out before completion");
		return;
	}
	rpr_prov_pause_ticks = 0;

	/* Evaluate the window that just finished */
	if (!rpr_first_round) {
		if (rpr_round_found) {
			rpr_empty_rounds = 0;
		} else if (++rpr_empty_rounds >= RPR_MAX_EMPTY_ROUNDS) {
			printk("RPR re-scan: nothing new in %d rounds, suspending remote scan (resumes on new activity)\n",
				   RPR_MAX_EMPTY_ROUNDS);
			rpr_scan_idle = true;
			rpr_scan_stop_all_servers();
			return;
		}
	}
	rpr_first_round = false;
	rpr_round_found = false;

	rpr_scan_all_servers();
	/* Re-arm for the next round. The period leaves an idle gap after the scan
	 * window so the relays return to being mesh-responsive between scans.
	 */
	k_work_reschedule_for_queue(&rpr_workq, k_work_delayable_from_work(work),
			 K_SECONDS(RPR_RESCAN_PERIOD_S));
}

static K_WORK_DELAYABLE_DEFINE(rpr_rescan_work, rpr_rescan_work_handler);

/* Resume the periodic re-scan if it was suspended after going idle. Safe to call */
/* from any context that observes fresh provisioning activity. */
static void rpr_scan_resume(void)
{
	if (rpr_scanning && rpr_scan_idle) {
		rpr_scan_idle = false;
		rpr_empty_rounds = 0;
		rpr_first_round = true;
		printk("RPR re-scan: new activity, resuming remote scan\n");
		k_work_reschedule_for_queue(&rpr_workq, &rpr_rescan_work, K_SECONDS(0));
	}
}

/* A new unprovisioned device was seen this window: keep the current scan loop */
/* alive (counts the round as productive) and wake it if it had gone idle. */
static void rpr_note_new_device(void)
{
	rpr_round_found = true;
	rpr_scan_resume();
}

/* Called when a node's provisioning AND configuration have both finished (or were */
/* abandoned). Lifts the pause held across the link + config burst and restarts the */
/* periodic re-scan (with a small gap) so it can pick up the next device. A node was */
/* just added, so treat the next window as fresh (reset the empty-round counter). */
static void rpr_prov_done(void)
{
	prov_active = false;
	rpr_prov_pause_ticks = 0;
	rpr_empty_rounds = 0;
	rpr_first_round = true;
	if (rpr_scanning) {
		rpr_scan_idle = false;
		k_work_reschedule_for_queue(&rpr_workq, &rpr_rescan_work,
				 K_SECONDS(RPR_POST_PROV_SETTLE_S));
	}
}

/* A provisioning attempt ended without the node joining the network. Name the
 * device in the failure report, so the website can put it back in its
 * unprovisioned list, then lift the provisioning pause and restart the re-scan,
 * which prov_provision() cancelled outright for the RPR path.
 */
static void prov_failed(const char *reason)
{
	char resp[160];

	if (prov_cur_uuid[0] != '\0') {
		snprintf(resp, sizeof(resp), "provisioning failed: %s uuid=%s",
				 reason, prov_cur_uuid);
		prov_cur_uuid[0] = '\0';
	} else {
		snprintf(resp, sizeof(resp), "provisioning failed: %s", reason);
	}
	printk("PROV-FAIL: %s\n", resp);
	uart30_send(resp, "Error");

	rpr_prov_done();
}

struct node_models {
	uint16_t ids[MAX_NODE_MODELS];
	size_t count;
};

/* converts 128-bit uuid received from unprovisioned beacon to readable string format */
static void uuid_to_str(const uint8_t uuid[16], char *out, size_t out_len)
{
	static const char hex[] = "0123456789abcdef";
	size_t p = 0;

	for (size_t i = 0; i < 16 && (p + 2) < out_len; i++) {
		out[p++] = hex[(uuid[i] >> 4) & 0x0F];
		out[p++] = hex[uuid[i] & 0x0F];
	}
	if (p < out_len) {
		out[p] = '\0';
	} else if (out_len > 0) {
		out[out_len - 1] = '\0';
	}
}

void prov_auto_set(bool enable)
{
	auto_provision = enable;
	printk("Auto-provisioning %s\n", enable ? "enabled" : "disabled");
}
/* Callback for unprovisioned beacons: forwards the uuid to the cloud once per
 * scan session.
 */
static void uart_unprov_beacon_cb(uint8_t uuid[16],
								  bt_mesh_prov_oob_info_t oob_info,
								  uint32_t *uri_hash)
{
	char uuid_str[33];

	char cmd[CMD_BUFFER_SIZE];

	uuid_to_str(uuid, uuid_str, sizeof(uuid_str));

	printk("uuid=%s\r\n", uuid_str);

	/* Forward to the cloud only the first time this device is seen */
	if (unprov_record(uuid_str, 0, UNPROV_RSSI_LOCAL)) { /* 0 srv = local, strongest link */
		rpr_note_new_device();
		uart30_send(uuid_str, "UUID");
	}

	if (auto_provision && !prov_active) {
		char response[CMD_BUFFER_SIZE];

		prov_active = true;
		snprintf(response, sizeof(response), "Auto-provisioning %s", uuid_str);
		uart30_send(response, "auto_provision");
		snprintf(cmd, sizeof(cmd), "gtw provandconfig %s %u %u %x",
			 uuid_str, cur_net_idx, cur_app_idx, cur_group);
		queue_shell_cmd(cmd);
	}
}

/* Callback for unprovisioned devices discovered remotely by an RPR Server. */
static void rpr_scan_report_cb(struct bt_mesh_rpr_cli *cli,
							   const struct bt_mesh_rpr_node *srv,
							   struct bt_mesh_rpr_unprov *unprov,
							   struct net_buf_simple *adv_data)
{
	ARG_UNUSED(cli);
	ARG_UNUSED(adv_data);

	char uuid_str[33];

	if (!srv || !unprov) {
		return;
	}

	uuid_to_str(unprov->uuid, uuid_str, sizeof(uuid_str));
	printk("RPR scan: server 0x%04x found uuid=%s\r\n", srv->addr, uuid_str);
	printk("rssi=%d, oob=0x%04x, hash=0x%08x\n",
		   unprov->rssi, unprov->oob, unprov->hash);

	/* Record the discovering relay together with the RSSI it measured */
	if (unprov_record(uuid_str, srv->addr, unprov->rssi)) {
		rpr_note_new_device();
		uart30_send(uuid_str, "UUID");
	}

	if (auto_provision && !prov_active) {
		char cmd[CMD_BUFFER_SIZE];

		char response[CMD_BUFFER_SIZE];

		prov_active = true;
		snprintf(response, sizeof(response), "Auto-provisioning %s", uuid_str);
		uart30_send(response, "auto_provision");
		snprintf(cmd, sizeof(cmd), "gtw provandconfig %s %u %u %x",
			 uuid_str, cur_net_idx, cur_app_idx, cur_group);
		queue_shell_cmd(cmd);
	}
}

/* Callback for when a node is provisioned. */
static void uart_node_added_cb(uint16_t net_idx,
							   uint8_t uuid[16],
							   uint16_t addr,
							   uint8_t num_elem)
{
	char cmd[CMD_BUFFER_SIZE];
	char uuid_str[33];

	ARG_UNUSED(net_idx);

	prov_node_added = true;
	prov_active = true;
	rpr_prov_pause_ticks = 0;
	prov_cur_uuid[0] = '\0';

	/* Configure over the subnet the node was just provisioned onto*/
	snprintf(cmd, sizeof(cmd), "gtw cfg %u 0x%04x %u 0x%04x", cur_net_idx, addr,
		 cur_app_idx, cur_group);
	if (!queue_shell_cmd(cmd)) {
		printk("Node 0x%04x: cfg enqueue failed; resuming re-scan without config\n", addr);
		rpr_prov_done();
	}

	uuid_to_str(uuid, uuid_str, sizeof(uuid_str));

	char response[160];

	snprintf(response, sizeof(response),
			 "Provisioned node 0x%04x (%u elements) uuid=%s, classifying via composition data",
			 addr, num_elem, uuid_str);
	uart30_send(response, "Provisioning");
}

/* reads the composition data of a node to get a list of models on it */
static int read_node_models(uint16_t net_idx, uint16_t addr,
							struct node_models *out)
{
	NET_BUF_SIMPLE_DEFINE(buf, BT_MESH_RX_SDU_MAX);
	struct bt_mesh_comp_p0 comp;
	struct bt_mesh_comp_p0_elem elem;
	uint8_t page = 0;
	int err;
	bool has_rpr_srv = false;

	out->count = 0;

	err = bt_mesh_cfg_cli_comp_data_get(net_idx, addr, 0, &page, &buf);
	if (err) {
		printk("comp-data get failed for 0x%04x (err %d)\n", addr, err);
		return err;
	}

	if (page != 0) {
		printk("Unexpected comp-data page %u for 0x%04x\n", page, addr);
		return -EINVAL;
	}

	err = bt_mesh_comp_p0_get(&comp, &buf);
	if (err) {
		printk("Parsing comp-data failed for 0x%04x (err %d)\n", addr, err);
		return err;
	}

	/* Collect the SIG models across all elements, de-duplicating so a model that */
	/* appears on more than one element is reported (and configured) once. */
	while (bt_mesh_comp_p0_elem_pull(&comp, &elem)) {
		for (size_t i = 0; i < elem.nsig; i++) {
			uint16_t id = bt_mesh_comp_p0_elem_mod(&elem, (int)i);
			bool seen = false;

			if (id == BT_MESH_MODEL_ID_REMOTE_PROV_SRV) { /* 0x0004 */
				has_rpr_srv = true;
			}

			for (size_t j = 0; j < out->count; j++) {
				if (out->ids[j] == id) {
					seen = true;
					break;
				}
			}
			if (seen) {
				continue;
			}

			if (out->count < MAX_NODE_MODELS) {
				out->ids[out->count++] = id;
			} else {
				printk("model cap (%u) reached for 0x%04x, dropping 0x%04x\n",
					   (unsigned int)MAX_NODE_MODELS, addr, id);
			}
		}
	}
	if (has_rpr_srv) {
		prov_rpr_server_add(addr);
	}

	return 0;
}


/* Per-model configuration rule. Applied to each model a node has */
struct model_cfg_rule {
	uint16_t id;
	bool sub_group;   /* subscribe to the control group */
	bool pub_status;  /* publish state to the gateway status group */
	bool pub_group;   /* publish to the control group */
};
static const struct model_cfg_rule model_rules[] = {
	{ BT_MESH_MODEL_ID_GEN_ONOFF_SRV,       true,  true,  false }, /* 0x1000 */
	{ BT_MESH_MODEL_ID_GEN_LEVEL_SRV,       true,  false, false }, /* 0x1002 */
	{ BT_MESH_MODEL_ID_LIGHT_LIGHTNESS_SRV, true,  true,  false }, /* 0x1300 */
	{ BT_MESH_MODEL_ID_SCENE_SRV,	   true,  true,  false }, /* 0x1203 */
	{ BT_MESH_MODEL_ID_SCENE_SETUP_SRV,     true,  false, false }, /* 0x1204 */
	{ BT_MESH_MODEL_ID_SENSOR_SRV,	   false, false, false }, /* 0x1100 */
	{ BT_MESH_MODEL_ID_GEN_ONOFF_CLI,       false, false, true  }, /* 0x1001 */
	{ BT_MESH_MODEL_ID_GEN_LEVEL_CLI,       false, false, true  }, /* 0x1003 */
	{ BT_MESH_MODEL_ID_SCENE_CLI,	   false, false, true  }, /* 0x1205 */
};

static const struct model_cfg_rule *find_model_rule(uint16_t id)
{
	for (size_t i = 0; i < ARRAY_SIZE(model_rules); i++) {
		if (model_rules[i].id == id) {
			return &model_rules[i];
		}
	}
	return NULL;
}

/* Queue configuration commands for a freshly provisioned node */
static void configure_by_models(uint16_t net_idx,
				 uint16_t addr, uint16_t app_idx, uint16_t group,
				 const struct node_models *models)
{
	char cmd[CMD_BUFFER_SIZE];

	const struct bt_mesh_cdb_app_key *app_key = bt_mesh_cdb_app_key_get(app_idx);
	uint16_t key_net_idx = app_key ? app_key->net_idx : net_idx;

	snprintf(cmd, sizeof(cmd), "mesh target net %u", net_idx);
	queue_shell_cmd(cmd);
	snprintf(cmd, sizeof(cmd), "mesh target dst 0x%04x", addr);
	queue_shell_cmd(cmd);
	snprintf(cmd, sizeof(cmd), "mesh models cfg appkey add %u %u", key_net_idx, app_idx);
	queue_shell_cmd(cmd);

	for (size_t i = 0; i < models->count; i++) {
		const struct model_cfg_rule *rule = find_model_rule(models->ids[i]);

		if (!rule) {
			continue;
		}

		snprintf(cmd, sizeof(cmd), "mesh models cfg model app-bind 0x%04x %u 0x%04x",
				 addr, app_idx, rule->id);
		queue_shell_cmd(cmd);

		if (rule->sub_group) {
			snprintf(cmd, sizeof(cmd),
				 "mesh models cfg model sub-add 0x%04x 0x%04x 0x%04x",
				 addr, group, rule->id);
			queue_shell_cmd(cmd);
		}
		if (rule->pub_status) {
			snprintf(cmd, sizeof(cmd),
				 "mesh models cfg model pub 0x%04x 0x%04x 0x%04x %u 0 7 0 0 0 0",
				 addr, rule->id, STATUS_GROUP_ADDR, app_idx);
			queue_shell_cmd(cmd);
		}
		if (rule->pub_group) {
			snprintf(cmd, sizeof(cmd),
				 "mesh models cfg model pub 0x%04x 0x%04x 0x%04x %u 0 7 0 0 0 0",
				 addr, rule->id, group, app_idx);
			queue_shell_cmd(cmd);
		}
	}
}

/* Read a freshly provisioned node's model composition, queue configuration commands, */
/* and report the model list to the cloud */
void configure_and_report(uint16_t addr, uint16_t net_idx, uint16_t app_idx,
						  uint16_t group)
{
	struct node_models models;
	char models_csv[160];
	char response[256];
	size_t pos = 0;
	int err;

	err = read_node_models(net_idx, addr, &models);
	if (err || models.count == 0) {
		printk("Node 0x%04x: no models read (err %d); no auto-config applied\n", addr, err);
		snprintf(response, sizeof(response), "0x%04x models= cfg=skipped", addr);
		uart30_send(response, "Classification");
		rpr_prov_done();
		return;
	}

	configure_by_models(net_idx, addr, app_idx, group, &models);

	for (size_t i = 0; i < models.count; i++) {
		int n = snprintf(models_csv + pos, sizeof(models_csv) - pos, "%s%x",
						 i ? "," : "", models.ids[i]);
		if (n < 0 || (size_t)n >= sizeof(models_csv) - pos) {
			printk("model list truncated for 0x%04x\n", addr);
			break;
		}
		pos += n;
	}

	snprintf(response, sizeof(response), "0x%04x models=%s group=0x%04x cfg=queued",
			 addr, models_csv, group);

	printk("Node 0x%04x configuration queued: %u models\n",
		   addr, (unsigned int)models.count);
	uart30_send(response, "Classification");
	rpr_prov_done();
}

/* Toggle combined scanning (local beacons + remote RPR) for unprovisioned devices */
void prov_scan_toggle(void)
{
	scanning = !scanning;
	if (scanning) {
		printk("Scanning for devices (local + remote)...\n");
		uart30_send("scan started", "response");
		num_unprov = 0;; /* clear unprovisioned UUID list */
		/* Activate callback for unprovisioned beacons */
		bt_mesh_shell_prov.unprovisioned_beacon = uart_unprov_beacon_cb;

		/* Temporary for testing, will be combined with the "scanning" later */
		rpr_scanning = true;
		rpr_scan_idle = false;
		rpr_empty_rounds = 0;
		rpr_round_found = false;
		rpr_first_round = true;
		/* Kick off the first remote scan immediately */
		k_work_reschedule_for_queue(&rpr_workq, &rpr_rescan_work, K_SECONDS(0));

	} else {
		printk("Stopping scan...\n");
		uart30_send("scan stopped", "response");
		/* Deactivate callback to stop receiving unprovisioned beacons */
		bt_mesh_shell_prov.unprovisioned_beacon = NULL;

		rpr_scanning = false;
		rpr_scan_idle = false;
		k_work_cancel_delayable(&rpr_rescan_work);
		rpr_scan_stop_all_servers();
	}
}

/* Toggle REMOTE-ONLY scanning: RPR relays only, local beacons OFF (for isolating/testing RPR) */
void prov_scan_rpr_toggle(void)
{
	rpr_scanning = !rpr_scanning;
	if (rpr_scanning) {
		printk("Scanning REMOTE-ONLY via %d RPR server(s); local beacons disabled\n",
			   num_rpr_servers);
		/* Force local scanning off so a device can ONLY be discovered through a */
		/* relay. Keep the "scan" toggle in sync so a later "scan" press behaves. */
		scanning = false;
		bt_mesh_shell_prov.unprovisioned_beacon = NULL;
		num_unprov = 0;; /* clear unprovisioned UUID list */
		if (num_rpr_servers == 0) {
			uart30_send("rpr-only scan started but NO RPR servers registered (use 'rpr-server list' / 'rpr-server add 0x<addr>')",
				    "response");
		} else {
			uart30_send("rpr-only scan started", "response");
		}
		/* Temporary for testing, will be combined with the "scanning" later */
		rpr_scanning = true;
		rpr_scan_idle = false;
		rpr_empty_rounds = 0;
		rpr_round_found = false;
		rpr_first_round = true;
		/* Kick off the first remote scan immediately */
		k_work_reschedule_for_queue(&rpr_workq, &rpr_rescan_work, K_SECONDS(0));

	} else {
		printk("Stopping rpr-only scan...\n");
		uart30_send("rpr-only scan stopped", "response");

		rpr_scanning = false;
		rpr_scan_idle = false;
		k_work_cancel_delayable(&rpr_rescan_work);
		rpr_scan_stop_all_servers();
	}
}

/* Report the relays the gateway will scan/provision through */
void prov_rpr_server_list(void)
{
	char list[256];
	int pos = snprintf(list, sizeof(list), "count=%d", num_rpr_servers);

	for (int i = 0; i < num_rpr_servers && pos < (int)sizeof(list); i++) {
		pos += snprintf(list + pos, sizeof(list) - pos, " 0x%04x", rpr_servers[i]);
	}
	printk("RPR servers: %s\n", list);
	uart30_send(list, "rpr_servers");
}

/* Check if subnet `net_idx` exists in the CDB and is held by the gateway itself
 * If not, create it. 
 */
int prov_ensure_subnet(uint16_t net_idx, const char *netkey_hex)
{
	if (bt_mesh_cdb_subnet_get(net_idx)) {
		return 0; /* already present */
	}

	uint8_t key[16];

	if (netkey_hex && netkey_hex[0] != '\0') {
		size_t len = hex2bin(netkey_hex, strlen(netkey_hex), key, sizeof(key));

		if (len != sizeof(key)) {
			printk("ensure_subnet 0x%03x: bad netkey hex\n", net_idx);
			return -EINVAL;
		}
	} else if (bt_rand(key, sizeof(key))) {
		printk("ensure_subnet 0x%03x: RNG failed\n", net_idx);
		return -EIO;
	}

	/* Record the subnet in the provisioner database (the source of the NetKey */
	/* handed to devices provisioned onto this subnet). */
	struct bt_mesh_cdb_subnet *sub = bt_mesh_cdb_subnet_alloc(net_idx);

	if (!sub) {
		printk("ensure_subnet 0x%03x: CDB alloc failed\n", net_idx);
		return -ENOMEM;
	}
	if (bt_mesh_cdb_subnet_key_import(sub, 0, key)) {
		printk("ensure_subnet 0x%03x: key import failed\n", net_idx);
		bt_mesh_cdb_subnet_del(sub, false);
		return -EIO;
	}
	bt_mesh_cdb_subnet_store(sub);

	/* Add the subnet to the gateway's own node */
	uint8_t status = 0;
	int err = bt_mesh_cfg_cli_net_key_add(0, bt_mesh_primary_addr(), net_idx,
										  key, &status);
	if (err || status) {
		printk("ensure_subnet 0x%03x: net-key-add to gateway failed (err %d, status 0x%02x)\n",
			   net_idx, err, status);
		bt_mesh_cdb_subnet_del(sub, true);
		return err ? err : -EIO;
	}

	printk("ensure_subnet: created subnet 0x%03x\n", net_idx);
	return 0;
}

/* Add another subnet to an already-provisioned node. */
int prov_node_add_subnet(uint16_t comms_net, uint16_t addr, uint16_t net_idx)
{
	struct bt_mesh_cdb_node *node = bt_mesh_cdb_node_get(addr);
	struct bt_mesh_cdb_subnet *sub;
	uint8_t key[16];
	uint8_t status = 0;
	char resp[192];
	int err;

	if (!node) {
		snprintf(resp, sizeof(resp), "add-subnet: node 0x%04x not in database", addr);
		printk("%s\n", resp);
		uart30_send(resp, "Error");
		return -ENODEV;
	}

	err = prov_ensure_subnet(net_idx, NULL);
	if (err) {
		snprintf(resp, sizeof(resp),
			 "add-subnet: failed to create subnet 0x%03x (err %d)", net_idx, err);
		printk("%s\n", resp);
		uart30_send(resp, "Error");
		return err;
	}

	sub = bt_mesh_cdb_subnet_get(net_idx);
	if (!sub) {
		snprintf(resp, sizeof(resp), "add-subnet: no subnet 0x%03x in database", net_idx);
		printk("%s\n", resp);
		uart30_send(resp, "Error");
		return -ENOENT;
	}
	/* Mid key-refresh the subnet holds two keys and index 0 is the OLD one,
	 * which is about to be revoked - handing that out now would strand the node
	 * on a dead key.
	 */
	if (sub->kr_phase != BT_MESH_KR_NORMAL) {
		snprintf(resp, sizeof(resp),
			 "add-subnet: subnet 0x%03x is refreshing (phase 0x%02x), retry after",
			 net_idx, sub->kr_phase);
		printk("%s\n", resp);
		uart30_send(resp, "Error");
		return -EBUSY;
	}
	if (bt_mesh_cdb_subnet_key_export(sub, 0, key)) {
		snprintf(resp, sizeof(resp),
			 "add-subnet: key export failed for subnet 0x%03x", net_idx);
		printk("%s\n", resp);
		uart30_send(resp, "Error");
		return -EIO;
	}

	err = bt_mesh_cfg_cli_net_key_add(comms_net, addr, net_idx, key, &status);
	if (err || status) {
		snprintf(resp, sizeof(resp),
			 "add-subnet: net-key-add 0x%04x subnet 0x%03x failed (err %d, status 0x%02x)",
			 addr, net_idx, err, status);
		printk("%s\n", resp);
		uart30_send(resp, "Error");
		return err ? err : -EIO;
	}

	printk("Node 0x%04x: subnet 0x%03x added (sent over subnet 0x%03x)\n",
	       addr, net_idx, comms_net);
	snprintf(resp, sizeof(resp), "0x%04x subnet=0x%03x netkey=added", addr, net_idx);
	uart30_send(resp, "SubnetAdded");
	return 0;
}

/* Remove a subnet from a node */
int prov_node_del_subnet(uint16_t comms_net, uint16_t addr, uint16_t net_idx)
{
	struct bt_mesh_cdb_node *node = bt_mesh_cdb_node_get(addr);
	uint8_t status = 0;
	char resp[192];
	int err;

	if (!node) {
		snprintf(resp, sizeof(resp), "del-subnet: node 0x%04x not in database", addr);
		printk("%s\n", resp);
		uart30_send(resp, "Error");
		return -ENODEV;
	}

	/* A node rejects a NetKey Delete for the subnet the message arrived
	 * on, so send this one over another subnet it holds.
	 */
	if (net_idx == comms_net) {
		snprintf(resp, sizeof(resp),
			 "node_del-subnet: 0x%04x is addressed over subnet 0x%03x, cannot remove it",
			 addr, net_idx);
		printk("%s\n", resp);
		uart30_send(resp, "Error");
		return -EINVAL;
	}

	err = bt_mesh_cfg_cli_net_key_del(comms_net, addr, net_idx, &status);
	if (err || status) {
		snprintf(resp, sizeof(resp),
		"node_del-subnet: net-key-del 0x%04x subnet 0x%03x failed (err %d, status 0x%02x)",
			 addr, net_idx, err, status);
		printk("%s\n", resp);
		uart30_send(resp, "Error");
		return err ? err : -EIO;
	}

	printk("Node 0x%04x: subnet 0x%03x removed (sent over subnet 0x%03x)\n",
	       addr, net_idx, comms_net);
	snprintf(resp, sizeof(resp), "0x%04x subnet=0x%03x netkey=removed", addr, net_idx);
	uart30_send(resp, "SubnetRemoved");
	return 0;
}

/* Remove an AppKey from a node */
int prov_node_del_appkey(uint16_t comms_net, uint16_t addr, uint16_t app_idx)
{
	struct bt_mesh_cdb_node *node = bt_mesh_cdb_node_get(addr);
	struct bt_mesh_cdb_app_key *key;
	uint8_t status = 0;
	char resp[192];
	int err;

	if (!node) {
		snprintf(resp, sizeof(resp), "del-appkey: node 0x%04x not in database", addr);
		printk("%s\n", resp);
		uart30_send(resp, "Error");
		return -ENODEV;
	}

	key = bt_mesh_cdb_app_key_get(app_idx);
	if (!key) {
		snprintf(resp, sizeof(resp),
			 "del-appkey: app-key 0x%03x not in database, cannot name its subnet",
			 app_idx);
		printk("%s\n", resp);
		uart30_send(resp, "Error");
		return -ENOENT;
	}

	uint16_t key_net_idx = key->net_idx;

	err = bt_mesh_cfg_cli_app_key_del(comms_net, addr, key_net_idx, app_idx, &status);
	if (err || status) {
		snprintf(resp, sizeof(resp),
			 "del-appkey: app-key-del 0x%04x app 0x%03x net 0x%03x failed "
			 "(err %d, status 0x%02x)",
			 addr, app_idx, key_net_idx, err, status);
		printk("%s\n", resp);
		uart30_send(resp, "Error");
		return err ? err : -EIO;
	}

	printk("Node 0x%04x: app-key 0x%03x (subnet 0x%03x) removed\n",
	       addr, app_idx, key_net_idx);
	snprintf(resp, sizeof(resp), "0x%04x appkey=0x%03x net=0x%03x appkey=removed",
		 addr, app_idx, key_net_idx);
	uart30_send(resp, "AppKeyRemoved");
	return 0;
}

/* Remove a group address from a node */
int prov_node_del_group(uint16_t comms_net, uint16_t addr, uint16_t group)
{
	struct bt_mesh_cdb_node *node = bt_mesh_cdb_node_get(addr);
	struct node_models models;
	unsigned int unsub = 0, unpub = 0, failed = 0;
	char resp[192];
	int err;

	if (!node) {
		snprintf(resp, sizeof(resp), "del-group: node 0x%04x not in database", addr);
		printk("%s\n", resp);
		uart30_send(resp, "Error");
		return -ENODEV;
	}

	if (!BT_MESH_ADDR_IS_GROUP(group)) {
		snprintf(resp, sizeof(resp),
			 "del-group: 0x%04x is not a group address (0xC000-0xFEFF)", group);
		printk("%s\n", resp);
		uart30_send(resp, "Error");
		return -EINVAL;
	}

	/* The status group is how fixture state reaches the cloud at all, and
	 * every node publishes to it. Removing it would silently stop reporting.
	 */
	if (group == STATUS_GROUP_ADDR) {
		snprintf(resp, sizeof(resp),
			 "del-group: 0x%04x is the status group, cannot remove it from 0x%04x",
			 group, addr);
		printk("%s\n", resp);
		uart30_send(resp, "Error");
		return -EINVAL;
	}

	err = read_node_models(comms_net, addr, &models);
	if (err || models.count == 0) {
		snprintf(resp, sizeof(resp),
			 "del-group: no models read from 0x%04x (err %d), group 0x%04x left as is",
			 addr, err, group);
		printk("%s\n", resp);
		uart30_send(resp, "Error");
		return err ? err : -EIO;
	}

	for (size_t i = 0; i < models.count; i++) {
		const struct model_cfg_rule *rule = find_model_rule(models.ids[i]);
		struct bt_mesh_cfg_cli_mod_pub pub;
		uint8_t status = 0;

		if (!rule) {
			continue;
		}

		err = bt_mesh_cfg_cli_mod_sub_del(comms_net, addr, addr, group, rule->id,
						  &status);
		if (err || status) {
			printk("del-group 0x%04x: sub-del 0x%04x on model 0x%04x failed (err %d, status 0x%02x)\n",
			       addr, group, rule->id, err, status);
			failed++;
		} else {
			unsub++;
		}

		/* Only clear publication that actually points at this group */
		status = 0;
		err = bt_mesh_cfg_cli_mod_pub_get(comms_net, addr, addr, rule->id, &pub,
						  &status);
		if (err || status) {
			printk("del-group 0x%04x: pub-get on model 0x%04x failed (err %d, status 0x%02x)\n",
			       addr, rule->id, err, status);
			failed++;
			continue;
		}
		if (pub.addr != group) {
			continue;
		}

		pub.addr = BT_MESH_ADDR_UNASSIGNED;
		pub.uuid = NULL;
		status = 0;
		err = bt_mesh_cfg_cli_mod_pub_set(comms_net, addr, addr, rule->id, &pub,
						  &status);
		if (err || status) {
			printk("del-group 0x%04x: pub-clear on model 0x%04x failed (err %d, status 0x%02x)\n",
			       addr, rule->id, err, status);
			failed++;
		} else {
			unpub++;
		}
	}

	printk("Node 0x%04x: group 0x%04x removed from %u model(s), %u publication(s) cleared, %u failure(s)\n",
	       addr, group, unsub, unpub, failed);

	if (unsub == 0 && unpub == 0) {
		snprintf(resp, sizeof(resp),
			 "del-group: 0x%04x held no configurable models for group 0x%04x (%u failure(s))",
			 addr, group, failed);
		uart30_send(resp, "Error");
		return failed ? -EIO : -ENOENT;
	}

	snprintf(resp, sizeof(resp), "0x%04x group=0x%04x unsub=%u unpub=%u failed=%u",
		 addr, group, unsub, unpub, failed);
	uart30_send(resp, "GroupRemoved");
	return failed ? -EIO : 0;
}

/* Provision a discovered device, tunneling through its RPR relay if it was seen */
/* remotely, else locally over PB-ADV. Completion fires uart_node_added_cb. */
void prov_provision(const char *uuid)
{
	char cmd[CMD_BUFFER_SIZE];
	char response[128];
	uint16_t rpr_srv = unprov_lookup(uuid);
	uint16_t prov_addr = bt_mesh_cdb_free_addr_get(1);

	prov_node_added = false;
	snprintf(prov_cur_uuid, sizeof(prov_cur_uuid), "%s", uuid);

	if (rpr_srv) {
		/* Device was discovered by a relay: tunnel provisioning through it.
		 * srv.net_idx is the subnet we reach the RELAY on
		 */
		struct bt_mesh_rpr_node srv = {
			.addr = rpr_srv,
			.net_idx = relay_comms_net(rpr_srv),
			.ttl = BT_MESH_TTL_DEFAULT,
		};
		uint8_t uuid_bin[16];
		size_t len = hex2bin(uuid, strlen(uuid), uuid_bin, sizeof(uuid_bin));
		(void)memset(uuid_bin + len, 0, sizeof(uuid_bin) - len);

		/* Pause the periodic re-scan BEFORE opening the link: a scan-start to */
		/* this relay while its provisioning link is opening collides with the */
		/* link and stalls it. Setting the flag first closes the window where a */
		/* re-scan round could sample it as still-clear during the open call. */
		prov_active = true;
		rpr_prov_pause_ticks = 0;
		k_work_cancel_delayable(&rpr_rescan_work);
		rpr_scan_stop_all_servers();

		int err = bt_mesh_provision_remote(model_handler_rpr_cli(), &srv,
				    uuid_bin, cur_net_idx, prov_addr);
		if (err) {
			/* Link never opened clear the pause so the
			 */
			char reason[96];

			printk("RPR provision via 0x%04x failed (err %d)\n", rpr_srv, err);
			snprintf(reason, sizeof(reason), "relay 0x%04x rejected the link (err %d)",
					 rpr_srv, err);
			prov_failed(reason);
			return;
		}
		/* RPR (remote) path: provisioning is tunneled through an already-
		 * provisioned relay.
		 */
		printk("PROV-PATH: RPR (remote) %s -> 0x%04x via relay 0x%04x\n",
			   uuid, prov_addr, rpr_srv);
		snprintf(response, sizeof(response),
				 "Provisioning started via RPR relay 0x%04x -> 0x%04x",
				 rpr_srv, prov_addr);
	} else {
		/* LOCAL path: device is in the gateway's own radio range; provision
		 * directly over PB-ADV.
		 */
		printk("PROV-PATH: LOCAL (PB-ADV) %s -> 0x%04x via gateway radio\n",
			   uuid, prov_addr);
		prov_active = true;
		snprintf(cmd, sizeof(cmd), "mesh prov remote-adv %s %u 0x%04x 5",
			 uuid, cur_net_idx, prov_addr);
		if (!queue_shell_cmd(cmd)) {
			prov_failed("command queue full, provisioning not started");
			return;
		}
		snprintf(response, sizeof(response),
				 "Provisioning started locally (PB-ADV) -> 0x%04x", prov_addr);
	}

	uart30_send(response, "response");
}

/* The mesh shell's own link_close handler, chained from ours. */
static void (*shell_link_close_cb)(bt_mesh_prov_bearer_t bearer);

/* When provisioning link closes check if node was added; if not, report a failure to the cloud.
 */
static void prov_link_close_cb(bt_mesh_prov_bearer_t bearer)
{
	if (shell_link_close_cb) {
		shell_link_close_cb(bearer);
	}

	if (!prov_active || prov_node_added) {
		return;
	}

	char reason[64];

	snprintf(reason, sizeof(reason), "%s link closed before completion",
			 (bearer == BT_MESH_PROV_REMOTE) ? "PB-REMOTE" :
			 (bearer == BT_MESH_PROV_ADV) ? "PB-ADV" : "PB-GATT");
	prov_failed(reason);
}

/* One-time provisioning subsystem setup. */
void provisioning_init(void)
{
	bt_mesh_shell_prov.node_added = uart_node_added_cb;

	/* Without this the gateway only ever hears about SUCCESS (node_added) and a
	 * failed provisioning is silent.
	 */
	shell_link_close_cb = bt_mesh_shell_prov.link_close;
	bt_mesh_shell_prov.link_close = prov_link_close_cb;

	/* Dedicated workqueue for the blocking RPR scan-start/scan-stop round */
	static const struct k_work_queue_config rpr_workq_cfg = { .name = "rpr_workq" };

	k_work_queue_start(&rpr_workq, rpr_workq_stack,
					   K_THREAD_STACK_SIZEOF(rpr_workq_stack),
					   RPR_WORKQ_PRIO, &rpr_workq_cfg);

	/* Receive remotely-discovered unprovisioned devices from RPR Servers. */
	model_handler_rpr_cli()->scan_report = rpr_scan_report_cb;

	/* The rpr_cli's default 2s Scan Status ack window is too tight when a relay is busy */
	bt_mesh_rpr_cli_timeout_set(8000);
}
