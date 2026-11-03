/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_string_conv.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/bluetooth/mesh.h>
#include "uart_cmd.h"
#include "gtw_shell.h"
#include "provisioning.h"
#include "model_handler.h"
#include "keyrefresh.h"

#define CMD_BUFFER_SIZE 256
#define CMD_QUEUE_LEN   32

/* bt_mesh_primary_addr() returns the gateway's own primary unicast address.
 * It lives in the mesh subsys (subsys/bluetooth/mesh/access.h), not a public
 * include, so forward-declare the one prototype we use.
 */
uint16_t bt_mesh_primary_addr(void);


K_MSGQ_DEFINE(shell_cmd_msgq, CMD_BUFFER_SIZE, CMD_QUEUE_LEN, 4);

/* Queue a shell command for shell_cmd_executor_thread to run.
 * Returns false if the command is empty or the queue is full.
 */
bool queue_shell_cmd(const char *cmd)
{
	if (!cmd || cmd[0] == '\0') {
		return false;
	}

	char tmp[CMD_BUFFER_SIZE];

	snprintf(tmp, sizeof(tmp), "%s", cmd);

	if (k_msgq_put(&shell_cmd_msgq, tmp, K_NO_WAIT) != 0) {
		printk("shell: command queue full, dropping: %s\n", tmp);
		return false;
	}
	return true;
}

bool queue_shell_cmd_front(const char *cmd)
{
	if (!cmd || cmd[0] == '\0') {
		return false;
	}

	char tmp[CMD_BUFFER_SIZE];

	snprintf(tmp, sizeof(tmp), "%s", cmd);

	if (k_msgq_put_front(&shell_cmd_msgq, &tmp) != 0) {
		printk("shell: command queue full, dropping: %s\n", tmp);
		return false;
	}
	return true;
}

/* Drains shell_cmd_msgq and executes each command on the dummy shell backend. */
static void shell_cmd_executor_thread(void)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();
	char cmd[CMD_BUFFER_SIZE];

	while (1) {
		k_msgq_get(&shell_cmd_msgq, cmd, K_FOREVER);

		if (!sh) {
			printk("shell backend not available\n");
			continue;
		}

		printk("running command: %s\n", cmd);
		shell_backend_dummy_clear_output(sh);
		if (strncmp(cmd, "-r ", 3) == 0) {
			/* "-r <appid> <command>": run shell command <command> and
			 * forward its output to cloud with appid <appid>
			 */
			char *appid = cmd + 3; /* first token after "-r " */
			char *raw_cmd = strchr(appid, ' ');

			if (!raw_cmd || raw_cmd[1] == '\0') {
				printk("malformed -r command (no command after appid): %s\n", cmd);
				continue;
			}
			*raw_cmd++ = '\0'; /* terminate appid, advance to the command */

			int ret = shell_execute_cmd(sh, raw_cmd);

			printk("command returned: %d\n", ret);

			size_t out_len;
			const char *out = shell_backend_dummy_get_output(sh, &out_len);

			printk("command output: %.*s", (int)out_len, out);
			uart30_send(out, appid);
		} else {
			int ret = shell_execute_cmd(sh, cmd);

			printk("command returned: %d\n", ret);

			size_t out_len;
			const char *out = shell_backend_dummy_get_output(sh, &out_len);

			printk("command output: %.*s", (int)out_len, out);
		}
	}
}

K_THREAD_DEFINE(gtw_shell_cmd_executor_thread, 6144, shell_cmd_executor_thread,
		NULL, NULL, NULL, 5, 0, 0);

/* Initiates the gateway */
static int cmd_gtw_init(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	uint16_t addr = (argc > 1) ? (uint16_t)shell_strtoul(argv[1], 0, &err) : 0x0001;

	if (err) {
		shell_error(sh, "invalid address: %s", argv[1]);
		return -EINVAL;
	}
	char cmd[CMD_BUFFER_SIZE];

	shell_print(sh, "Initializing device at 0x%04x...", addr);
	queue_shell_cmd("mesh init");
	queue_shell_cmd("mesh cdb create");
	snprintf(cmd, sizeof(cmd), "mesh prov local 0 0x%04x", addr);
	queue_shell_cmd(cmd);
	queue_shell_cmd("gtw appkey 0 0"); /* add default appkey on the primary subnet */
	queue_shell_cmd("mesh target net 0");
	queue_shell_cmd("mesh target dst local");
	/* Subscribe the local client models to status publications from fixtures */
	/* so the cloud sees on/off, brightness and scene changes from any source. */
	snprintf(cmd, sizeof(cmd), "mesh models cfg model sub-add 0x%04x 0x%04x 0x%04x",
		 addr, STATUS_GROUP_ADDR, BT_MESH_MODEL_ID_GEN_ONOFF_CLI);
	queue_shell_cmd(cmd);
	snprintf(cmd, sizeof(cmd), "mesh models cfg model sub-add 0x%04x 0x%04x 0x%04x",
		 addr, STATUS_GROUP_ADDR, BT_MESH_MODEL_ID_LIGHT_LIGHTNESS_CLI);
	queue_shell_cmd(cmd);
	snprintf(cmd, sizeof(cmd), "mesh models cfg model sub-add 0x%04x 0x%04x 0x%04x",
		 addr, STATUS_GROUP_ADDR, BT_MESH_MODEL_ID_SCENE_CLI);
	queue_shell_cmd(cmd);


	shell_print(sh, "gtw init: gateway initialised");
	const char *response = "initialization complete";

	uart30_send(response, "init_complete");
	return 0;
}

/* Add an AppKey (bound to a subnet) and bind the gateway's local client models
 * to it
 * Usage: appkey <net_idx> <app_idx> [app_key]
 */
static int cmd_gtw_appkey(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	uint16_t net_idx = (uint16_t)shell_strtoul(argv[1], 0, &err);

	if (err) {
		shell_error(sh, "invalid net_idx: %s", argv[1]);
		return -EINVAL;
	}
	uint16_t app_idx = (uint16_t)shell_strtoul(argv[2], 0, &err);

	if (err) {
		shell_error(sh, "invalid app_idx: %s", argv[2]);
		return -EINVAL;
	}
	const char *app_key = (argc > 3) ? argv[3] : NULL;

	/* ensure the subnet exists */
	err = prov_ensure_subnet(net_idx, NULL);
	if (err) {
		shell_error(sh, "gtw appkey: failed to ensure subnet 0x%03x (err %d)",
				net_idx, err);
		return err;
	}

	char cmd[CMD_BUFFER_SIZE];
	/* Front-queue so this appkey setup jumps ahead of already-queued commands.
	 * The front queue is LIFO, so push in REVERSE of the required run order;
	 * they then dequeue forwards:
	 *   target net -> target dst -> cdb key-add -> config key-add -> model binds
	 * (a bind issued before the key exists returns status 0x03).
	 */

	/* Model binds run last -> pushed first (reverse of onoff, lightness, scene,
	 * sensor).
	 */
	snprintf(cmd, sizeof(cmd), "mesh models cfg model app-bind 0x%04x %u 0x%04x",
		 bt_mesh_primary_addr(), app_idx, BT_MESH_MODEL_ID_SENSOR_CLI);
	queue_shell_cmd_front(cmd);
	snprintf(cmd, sizeof(cmd), "mesh models cfg model app-bind 0x%04x %u 0x%04x",
		 bt_mesh_primary_addr(), app_idx, BT_MESH_MODEL_ID_SCENE_CLI);
	queue_shell_cmd_front(cmd);
	snprintf(cmd, sizeof(cmd), "mesh models cfg model app-bind 0x%04x %u 0x%04x",
		 bt_mesh_primary_addr(), app_idx, BT_MESH_MODEL_ID_LIGHT_LIGHTNESS_CLI);
	queue_shell_cmd_front(cmd);
	snprintf(cmd, sizeof(cmd), "mesh models cfg model app-bind 0x%04x %u 0x%04x",
		 bt_mesh_primary_addr(), app_idx, BT_MESH_MODEL_ID_GEN_ONOFF_CLI);
	queue_shell_cmd_front(cmd);

	snprintf(cmd, sizeof(cmd), "mesh models cfg appkey add %u %u", net_idx, app_idx);
	queue_shell_cmd_front(cmd);

	if (app_key && app_key[0] != '\0') {
		snprintf(cmd, sizeof(cmd),
		 "mesh cdb app-key-add %u %u %s", net_idx, app_idx, app_key);
	} else {
		snprintf(cmd, sizeof(cmd), "mesh cdb app-key-add %u %u", net_idx, app_idx);
	}
	queue_shell_cmd_front(cmd);

	queue_shell_cmd_front("mesh target dst local");
	queue_shell_cmd_front("mesh target net 0");

	shell_print(sh, "gtw appkey: added app_idx %u on subnet 0x%03x", app_idx, net_idx);
	return 0;
}

/* Create (or confirm) a subnet and add it to the gateway.
 * Usage: subnet <net_idx> [netkey_hex]
 */
static int cmd_gtw_subnet(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	uint16_t net_idx = (uint16_t)shell_strtoul(argv[1], 0, &err);

	if (err) {
		shell_error(sh, "invalid net_idx: %s", argv[1]);
		return -EINVAL;
	}
	const char *netkey = (argc > 2) ? argv[2] : NULL;

	err = prov_ensure_subnet(net_idx, netkey);
	if (err) {
		shell_error(sh, "gtw subnet: failed to create subnet 0x%03x (err %d)",
				net_idx, err);
		return err;
	}

	shell_print(sh, "gtw subnet: subnet 0x%03x ready", net_idx);
	return 0;
}

/* Add another subnet to an already-provisioned node.
 * Usage: addsubnet <net_idx> <addr_hex> <add_net_idx>
 */
static int cmd_gtw_addsubnet(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);

	uint16_t net_idx = (uint16_t)shell_strtoul(argv[1], 0, &err);

	if (err) {
		shell_error(sh, "invalid net_idx: %s", argv[1]);
		return -EINVAL;
	}
	uint16_t addr = (uint16_t)shell_strtoul(argv[2], 16, &err);

	if (err) {
		shell_error(sh, "invalid address: %s", argv[2]);
		return -EINVAL;
	}
	uint16_t add_net_idx = (uint16_t)shell_strtoul(argv[3], 0, &err);

	if (err) {
		shell_error(sh, "invalid add_net_idx: %s", argv[3]);
		return -EINVAL;
	}

	shell_print(sh, "Adding subnet 0x%03x to node 0x%04x over subnet 0x%03x...",
		    add_net_idx, addr, net_idx);
	return prov_node_add_subnet(net_idx, addr, add_net_idx);
}

/* Remove a subnet from a node. Takes every AppKey bound to that subnet with it.
 * Usage: node_delsubnet <net_idx> <addr_hex> <del_net_idx>
 */
static int cmd_gtw_node_delsubnet(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);

	uint16_t net_idx = (uint16_t)shell_strtoul(argv[1], 0, &err);

	if (err) {
		shell_error(sh, "invalid net_idx: %s", argv[1]);
		return -EINVAL;
	}
	uint16_t addr = (uint16_t)shell_strtoul(argv[2], 16, &err);

	if (err) {
		shell_error(sh, "invalid address: %s", argv[2]);
		return -EINVAL;
	}
	uint16_t del_net_idx = (uint16_t)shell_strtoul(argv[3], 0, &err);

	if (err) {
		shell_error(sh, "invalid del_net_idx: %s", argv[3]);
		return -EINVAL;
	}

	shell_print(sh, "Removing subnet 0x%03x from node 0x%04x over subnet 0x%03x...",
		    del_net_idx, addr, net_idx);
	return prov_node_del_subnet(net_idx, addr, del_net_idx);
}

/* Remove one AppKey from a node, leaving its subnets alone.
 * Usage: node_delappkey <net_idx> <addr_hex> <app_idx>
 */
static int cmd_gtw_node_delappkey(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);

	uint16_t net_idx = (uint16_t)shell_strtoul(argv[1], 0, &err);

	if (err) {
		shell_error(sh, "invalid net_idx: %s", argv[1]);
		return -EINVAL;
	}
	uint16_t addr = (uint16_t)shell_strtoul(argv[2], 16, &err);

	if (err) {
		shell_error(sh, "invalid address: %s", argv[2]);
		return -EINVAL;
	}
	uint16_t app_idx = (uint16_t)shell_strtoul(argv[3], 0, &err);

	if (err) {
		shell_error(sh, "invalid app_idx: %s", argv[3]);
		return -EINVAL;
	}

	shell_print(sh, "Removing app-key 0x%03x from node 0x%04x over subnet 0x%03x...",
		    app_idx, addr, net_idx);
	return prov_node_del_appkey(net_idx, addr, app_idx);
}

/* Remove a group from a node: unsubscribe its models from the group address and
 * clear any publication aimed at it.
 * Usage: node_delgroup <net_idx> <addr_hex> <group_hex>
 */
static int cmd_gtw_node_delgroup(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);

	uint16_t net_idx = (uint16_t)shell_strtoul(argv[1], 0, &err);

	if (err) {
		shell_error(sh, "invalid net_idx: %s", argv[1]);
		return -EINVAL;
	}
	uint16_t addr = (uint16_t)shell_strtoul(argv[2], 16, &err);

	if (err) {
		shell_error(sh, "invalid address: %s", argv[2]);
		return -EINVAL;
	}
	uint16_t group = (uint16_t)shell_strtoul(argv[3], 16, &err);

	if (err) {
		shell_error(sh, "invalid group: %s", argv[3]);
		return -EINVAL;
	}

	shell_print(sh, "Removing group 0x%04x from node 0x%04x over subnet 0x%03x...",
		    group, addr, net_idx);
	return prov_node_del_group(net_idx, addr, group);
}

/* One "cdb" line: the widest is a node line at 92 characters. */
#define CDB_LINE_SIZE 128

/* Sends one CDB line to the cloud as its own d2c message. The website folds the
 * dump in line by line, so every line has to be its own message.
 */
static void cdb_send_line(const char *line)
{
	uart30_send(line, "cdb");
}

static void cdb_send_nodes(void)
{
	char line[CDB_LINE_SIZE];
	char uuid_hex[32 + 1], key_hex[32 + 1];
	uint8_t dev_key[16];
	int total = 0;

	for (int i = 0; i < ARRAY_SIZE(bt_mesh_cdb.nodes); i++) {
		struct bt_mesh_cdb_node *node = &bt_mesh_cdb.nodes[i];

		if (node->addr == BT_MESH_ADDR_UNASSIGNED) {
			continue;
		}
		if (bt_mesh_cdb_node_key_export(node, dev_key)) {
			printk("cdb: cannot export dev key for 0x%04x\n", node->addr);
			continue;
		}
		total++;
		bin2hex(node->uuid, 16, uuid_hex, sizeof(uuid_hex));
		bin2hex(dev_key, 16, key_hex, sizeof(key_hex));
		snprintf(line, sizeof(line), "0x%04x   %-8d  %-5s  %s  %s", node->addr,
			 node->num_elem,
			 atomic_test_bit(node->flags, BT_MESH_CDB_NODE_CONFIGURED) ? "C" : "-",
			 uuid_hex, key_hex);
		cdb_send_line(line);
	}

	snprintf(line, sizeof(line), "> Total nodes: %d", total);
	cdb_send_line(line);
}

static void cdb_send_subnets(void)
{
	char line[CDB_LINE_SIZE];
	char key_hex[32 + 1];
	uint8_t net_key[16];
	int total = 0;

	for (int i = 0; i < ARRAY_SIZE(bt_mesh_cdb.subnets); i++) {
		struct bt_mesh_cdb_subnet *sub = &bt_mesh_cdb.subnets[i];

		if (sub->net_idx == BT_MESH_KEY_UNUSED) {
			continue;
		}
		if (bt_mesh_cdb_subnet_key_export(sub, 0, net_key)) {
			printk("cdb: cannot export net key for 0x%03x\n", sub->net_idx);
			continue;
		}
		total++;
		bin2hex(net_key, 16, key_hex, sizeof(key_hex));
		snprintf(line, sizeof(line), "0x%03x   %s", sub->net_idx, key_hex);
		cdb_send_line(line);
	}

	snprintf(line, sizeof(line), "> Total subnets: %d", total);
	cdb_send_line(line);
}

static void cdb_send_app_keys(void)
{
	char line[CDB_LINE_SIZE];
	char key_hex[32 + 1];
	uint8_t app_key[16];
	int total = 0;

	for (int i = 0; i < ARRAY_SIZE(bt_mesh_cdb.app_keys); i++) {
		struct bt_mesh_cdb_app_key *key = &bt_mesh_cdb.app_keys[i];

		if (key->net_idx == BT_MESH_KEY_UNUSED) {
			continue;
		}
		if (bt_mesh_cdb_app_key_export(key, 0, app_key)) {
			printk("cdb: cannot export app key 0x%03x\n", key->app_idx);
			continue;
		}
		total++;
		bin2hex(app_key, 16, key_hex, sizeof(key_hex));
		snprintf(line, sizeof(line), "0x%03x   0x%03x   %s", key->net_idx, key->app_idx,
			 key_hex);
		cdb_send_line(line);
	}

	/* The website treats this as the end of the dump: it prunes whatever the
	 * gateway did not list, so it has to be the last line sent.
	 */
	snprintf(line, sizeof(line), "> Total app-keys: %d", total);
	cdb_send_line(line);
}

/* Sends the Config DB to the cloud, one d2c message per line.
 *
 * This walks bt_mesh_cdb itself instead of relaying "mesh cdb show". That
 * command's output lands in the dummy shell backend's fixed output buffer,
 * whose write() clamps to the buffer size and still reports success, so a dump
 * that outgrew the buffer was cut off mid-line with nothing reporting it. With
 * CDB_NODE_COUNT=128 a full dump is ~12 kB, which no fixed buffer here should
 * have to hold, and losing the tail also lost the "> Total app-keys" line the
 * website needs to finish the dump and prune stale nodes.
 *
 * The line formats match "mesh cdb show" so the website's parser is unchanged.
 * The banner and separator lines are left out: nothing reads them.
 */
static int cmd_gtw_cdb(const struct shell *sh, size_t argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!atomic_test_bit(bt_mesh_cdb.flags, BT_MESH_CDB_VALID)) {
		shell_print(sh, "No valid networks");
		cdb_send_line("No valid networks");
		return 0;
	}

	shell_print(sh, "Sending CDB to cloud...");

	/* The website starts a fresh dump on this line. */
	cdb_send_line("Mesh Network Information");
	cdb_send_nodes();
	cdb_send_subnets();
	cdb_send_app_keys();
	return 0;
}

/* Scan for unprovisioned devices */
static int cmd_gtw_scan(const struct shell *sh, size_t argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Toggling scan for unprovisioned devices...");
	prov_scan_toggle();
	return 0;
}

/* Provision a device by UUID and auto-configure it on completion.
 * Usage: provandconfig <uuid> <net_idx> <app_idx> <group_hex>
 */
static int cmd_gtw_provandconfig(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);

	const char *uuid = argv[1];

	uint16_t net_idx = (uint16_t)shell_strtoul(argv[2], 0, &err);

	if (err) {
		shell_error(sh, "invalid net_idx: %s", argv[2]);
		return -EINVAL;
	}
	uint16_t app_idx = (uint16_t)shell_strtoul(argv[3], 0, &err);

	if (err) {
		shell_error(sh, "invalid app_idx: %s", argv[3]);
		return -EINVAL;
	}
	uint16_t group = (uint16_t)shell_strtoul(argv[4], 16, &err);

	if (err) {
		shell_error(sh, "invalid group: %s", argv[4]);
		return -EINVAL;
	}

	/* An AppKey binds to exactly one subnet. If this app_idx already exists on
	 * a different subnet, reusing it here would be inconsistent - reject it.
	 */
	struct bt_mesh_cdb_app_key *app_key = bt_mesh_cdb_app_key_get(app_idx);

	if (app_key && app_key->net_idx != net_idx) {
		char resp[192];

		snprintf(resp, sizeof(resp),
			 "provisioning failed: app_idx %u already bound to subnet 0x%03x, cannot reuse on 0x%03x uuid=%s",
			 app_idx, app_key->net_idx, net_idx, uuid);
		shell_error(sh, "%s", resp);
		uart30_send(resp, "Error");
		return -EINVAL;
	}

	/* ensure the subnet exists */
	err = prov_ensure_subnet(net_idx, NULL);
	if (err) {
		char resp[192];

		snprintf(resp, sizeof(resp),
			 "provisioning failed: could not create subnet 0x%03x (err %d) uuid=%s",
			 net_idx, err, uuid);
		shell_error(sh, "%s", resp);
		uart30_send(resp, "Error");
		return err;
	}

	/* ensure appkey exists */
	if (!app_key) {
		char cmd[CMD_BUFFER_SIZE];

		shell_print(sh, "AppKey index %u does not exist, creating it on subnet 0x%03x",
				app_idx, net_idx);
		snprintf(cmd, sizeof(cmd), "gtw appkey %u %u", net_idx, app_idx);
		queue_shell_cmd(cmd);
	}

	cur_net_idx = net_idx;
	cur_app_idx = app_idx;
	cur_group = group;

	shell_print(sh, "Provisioning node %s onto subnet 0x%03x...", uuid, net_idx);
	prov_provision(uuid);
	return 0;
}

/* Configure a provisioned node.
 * Usage: cfg <net_idx> <addr_hex> <app_idx> <group_hex>
 */
static int cmd_gtw_cfg(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);

	uint16_t net_idx = (uint16_t)shell_strtoul(argv[1], 0, &err);

	if (err) {
		shell_error(sh, "invalid net_idx: %s", argv[1]);
		return -EINVAL;
	}

	uint16_t addr = (uint16_t)shell_strtoul(argv[2], 16, &err);

	if (err) {
		shell_error(sh, "invalid address: %s", argv[2]);
		return -EINVAL;
	}

	uint16_t app_idx = (uint16_t)shell_strtoul(argv[3], 0, &err);

	if (err) {
		shell_error(sh, "invalid app_idx: %s", argv[3]);
		return -EINVAL;
	}

	uint16_t group = (uint16_t)shell_strtoul(argv[4], 16, &err);

	if (err) {
		shell_error(sh, "invalid group: %s", argv[4]);
		return -EINVAL;
	}

	/* check that the AppKey exists and is bound to a subnet */
	const struct bt_mesh_cdb_app_key *app_key = bt_mesh_cdb_app_key_get(app_idx);

	if (!app_key) {
		char resp[128];

		snprintf(resp, sizeof(resp),
			 "config failed: app_idx %u does not exist, 0x%04x not configured",
			 app_idx, addr);
		shell_error(sh, "%s", resp);
		uart30_send(resp, "Error");
		return -ENOENT;
	}

	shell_print(sh, "Configuring node 0x%04x over subnet 0x%03x (app 0x%03x on subnet 0x%03x)...",
		    addr, net_idx, app_idx, app_key->net_idx);
	configure_and_report(addr, net_idx, app_idx, group);
	return 0;
}

/* Set a fixture's on/off state via Generic OnOff.
 * Usage: light <net_idx> <app_idx> <dst_addr> <on/off>
 */
static int cmd_gtw_light(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);

	uint16_t net_idx = (uint16_t)shell_strtoul(argv[1], 0, &err);

	if (err) {
		shell_error(sh, "invalid net_idx: %s", argv[1]);
		return -EINVAL;
	}
	uint16_t app_idx = (uint16_t)shell_strtoul(argv[2], 0, &err);

	if (err) {
		shell_error(sh, "invalid app_idx: %s", argv[2]);
		return -EINVAL;
	}
	uint16_t dst_addr = (uint16_t)shell_strtoul(argv[3], 0, &err);

	if (err) {
		shell_error(sh, "invalid dst_addr: %s", argv[3]);
		return -EINVAL;
	}
	unsigned long on_off = shell_strtoul(argv[4], 0, &err);

	if (err) {
		shell_error(sh, "invalid on/off: %s", argv[4]);
		return -EINVAL;
	}

	err = model_handler_onoff_set(net_idx, app_idx, dst_addr, on_off != 0UL);
	if (err) {
		/* Report to the website too - it dispatched this and needs the failure. */
		char response[128];

		snprintf(response, sizeof(response),
			 "Light command failed to 0x%04x (err %d)", dst_addr, err);
		shell_error(sh, "%s", response);
		uart30_send(response, "Error");
		return err;
	}

	shell_print(sh, "Light 0x%04x -> %s", dst_addr, on_off ? "on" : "off");
	return 0;
}

/* Set a fixture's absolute brightness (0-100%) via Light Lightness.
 * Usage: bri <net_idx> <app_idx> <dst_addr> <0-100>
 */
static int cmd_gtw_bri(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);

	uint16_t net_idx = (uint16_t)shell_strtoul(argv[1], 0, &err);

	if (err) {
		shell_error(sh, "invalid net_idx: %s", argv[1]);
		return -EINVAL;
	}
	uint16_t app_idx = (uint16_t)shell_strtoul(argv[2], 0, &err);

	if (err) {
		shell_error(sh, "invalid app_idx: %s", argv[2]);
		return -EINVAL;
	}
	uint16_t dst_addr = (uint16_t)shell_strtoul(argv[3], 0, &err);

	if (err) {
		shell_error(sh, "invalid dst_addr: %s", argv[3]);
		return -EINVAL;
	}
	uint8_t pct = (uint8_t)shell_strtoul(argv[4], 0, &err);

	if (err) {
		shell_error(sh, "invalid brightness: %s", argv[4]);
		return -EINVAL;
	}

	err = model_handler_lightness_set(net_idx, app_idx, dst_addr, pct);
	if (err) {
		/* Report to the website too - it dispatched this and needs the failure. */
		char response[128];

		snprintf(response, sizeof(response),
			 "Brightness command failed to 0x%04x (err %d)", dst_addr, err);
		shell_error(sh, "%s", response);
		uart30_send(response, "Error");
		return err;
	}

	shell_print(sh, "Brightness 0x%04x -> %u%%", dst_addr, pct);
	return 0;
}

/* Recall or store a scene on a fixture's Scene Server.
 * Usage: scene <recall|store> <net_idx> <app_idx> <dst_addr> <scene>
 */
static int cmd_gtw_scene(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);

	const char *action = argv[1];

	uint16_t net_idx = (uint16_t)shell_strtoul(argv[2], 0, &err);

	if (err) {
		shell_error(sh, "invalid net_idx: %s", argv[2]);
		return -EINVAL;
	}
	uint16_t app_idx = (uint16_t)shell_strtoul(argv[3], 0, &err);

	if (err) {
		shell_error(sh, "invalid app_idx: %s", argv[3]);
		return -EINVAL;
	}
	uint16_t dst_addr = (uint16_t)shell_strtoul(argv[4], 0, &err);

	if (err) {
		shell_error(sh, "invalid dst_addr: %s", argv[4]);
		return -EINVAL;
	}
	uint16_t scene = (uint16_t)shell_strtoul(argv[5], 0, &err);

	if (err) {
		shell_error(sh, "invalid scene: %s", argv[5]);
		return -EINVAL;
	}

	if (strcmp(action, "store") == 0) {
		err = model_handler_scene_store(net_idx, app_idx, dst_addr, scene);
	} else if (strcmp(action, "recall") == 0) {
		err = model_handler_scene_recall(net_idx, app_idx, dst_addr, scene);
	} else {
		shell_error(sh, "invalid action '%s' (use recall|store)", action);
		return -EINVAL;
	}

	if (err) {
		/* Report to the website too - it dispatched this and needs the failure. */
		char response[128];

		snprintf(response, sizeof(response),
			 "Scene command failed to 0x%04x (err %d)", dst_addr, err);
		shell_error(sh, "%s", response);
		uart30_send(response, "Error");
		return err;
	}

	shell_print(sh, "Scene %s %u -> 0x%04x", action, scene, dst_addr);
	return 0;
}

/* Read a fixture's energy use via the Sensor Client.
 * Usage: energy <net_idx> <app_idx> <dst_addr>
 */
static int cmd_gtw_energy(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);

	uint16_t net_idx = (uint16_t)shell_strtoul(argv[1], 0, &err);

	if (err) {
		shell_error(sh, "invalid net_idx: %s", argv[1]);
		return -EINVAL;
	}
	uint16_t app_idx = (uint16_t)shell_strtoul(argv[2], 0, &err);

	if (err) {
		shell_error(sh, "invalid app_idx: %s", argv[2]);
		return -EINVAL;
	}
	uint16_t dst_addr = (uint16_t)shell_strtoul(argv[3], 0, &err);

	if (err) {
		shell_error(sh, "invalid dst_addr: %s", argv[3]);
		return -EINVAL;
	}

	err = model_handler_sensor_get(net_idx, app_idx, dst_addr);
	if (err) {
		char response[128];

		snprintf(response, sizeof(response),
			 "Energy read failed to 0x%04x (err %d)", dst_addr, err);
		shell_error(sh, "%s", response);
		uart30_send(response, "Error");
		return err;
	}

	shell_print(sh, "Energy get -> 0x%04x", dst_addr);
	return 0;
}

/* Re-create the mesh with an existing NetKey and provision the gateway at a given address
 * Usage: replace <netkey_hex> <address>
 */
static int cmd_gtw_replace(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);

	const char *netkey = argv[1];
	uint16_t addr = (uint16_t)shell_strtoul(argv[2], 0, &err);

	if (err) {
		shell_error(sh, "invalid address: %s", argv[2]);
		return -EINVAL;
	}

	char cmd[CMD_BUFFER_SIZE];

	shell_print(sh, "Reprovisioning gateway at 0x%04x with supplied netkey...", addr);
	queue_shell_cmd("mesh init");
	snprintf(cmd, sizeof(cmd), "mesh cdb create %s", netkey);
	queue_shell_cmd(cmd);
	snprintf(cmd, sizeof(cmd), "mesh prov local 0 0x%04x", addr);
	queue_shell_cmd(cmd);

	/* Subscribe the local client models to fixture status publications. */
	snprintf(cmd, sizeof(cmd), "mesh models cfg model sub-add 0x%04x 0x%04x 0x%04x",
		 addr, STATUS_GROUP_ADDR, BT_MESH_MODEL_ID_GEN_ONOFF_CLI);
	queue_shell_cmd(cmd);
	snprintf(cmd, sizeof(cmd), "mesh models cfg model sub-add 0x%04x 0x%04x 0x%04x",
		 addr, STATUS_GROUP_ADDR, BT_MESH_MODEL_ID_LIGHT_LIGHTNESS_CLI);
	queue_shell_cmd(cmd);
	snprintf(cmd, sizeof(cmd), "mesh models cfg model sub-add 0x%04x 0x%04x 0x%04x",
		 addr, STATUS_GROUP_ADDR, BT_MESH_MODEL_ID_SCENE_CLI);
	queue_shell_cmd(cmd);

	uart30_send("reProvisioned", "reProvisioned");
	return 0;
}

/* Manually add a node to the CDB
 * Usage: node <uuid_hex> <devkey_hex> <address> <elements> <net_idx>
 */
static int cmd_gtw_node(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);

	const char *uuid = argv[1];
	const char *devkey = argv[2];
	uint16_t addr = (uint16_t)shell_strtoul(argv[3], 0, &err);

	if (err) {
		shell_error(sh, "invalid address: %s", argv[3]);
		return -EINVAL;
	}
	uint8_t elements = (uint8_t)shell_strtoul(argv[4], 0, &err);

	if (err) {
		shell_error(sh, "invalid elements: %s", argv[4]);
		return -EINVAL;
	}
	uint16_t net_idx = (uint16_t)shell_strtoul(argv[5], 0, &err);

	if (err) {
		shell_error(sh, "invalid net_idx: %s", argv[5]);
		return -EINVAL;
	}

	/* ensure the subnet exists */
	err = prov_ensure_subnet(net_idx, NULL);
	if (err) {
		shell_error(sh, "gtw node: failed to ensure subnet 0x%03x (err %d)",
				net_idx, err);
		return err;
	}

	char cmd[CMD_BUFFER_SIZE];
	/* mesh cdb node-add <UUID> <addr> <elem count> <NetKeyIdx> <DevKey> */
	snprintf(cmd, sizeof(cmd), "mesh cdb node-add %s 0x%04x %u %u %s",
		 uuid, addr, elements, net_idx, devkey);
	queue_shell_cmd(cmd);

	/* Ack subject "node" so the website's waitForAck("node") resolves. */
	char response[64];

	snprintf(response, sizeof(response), "provisioned:%s", uuid);
	uart30_send(response, "node");
	return 0;
}
/* Toggle auto-provisioning: automatically provision unprovisioned devices as soon as they are seen,
 * onto the given subnet / appkey / control group.
 * usage: auto-provision <on/off> <net_idx> <app_idx> <group_hex>
 */
static int cmd_gtw_auto_provision(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	char response[CMD_BUFFER_SIZE];

	ARG_UNUSED(argc);

	uint16_t enable = (uint16_t)shell_strtoul(argv[1], 0, &err);

	if (err) {
		shell_error(sh, "invalid on/off value: %s", argv[1]);
		return -EINVAL;
	}

	uint16_t net_idx = (uint16_t)shell_strtoul(argv[2], 0, &err);

	if (err) {
		shell_error(sh, "invalid net_idx: %s", argv[2]);
		return -EINVAL;
	}

	uint16_t app_idx = (uint16_t)shell_strtoul(argv[3], 0, &err);

	if (err) {
		shell_error(sh, "invalid app_idx: %s", argv[3]);
		return -EINVAL;
	}

	uint16_t group = (uint16_t)shell_strtoul(argv[4], 16, &err);

	if (err) {
		shell_error(sh, "invalid group: %s", argv[4]);
		return -EINVAL;
	}

	cur_net_idx = net_idx;
	cur_app_idx = app_idx;
	cur_group = group;

	shell_print(sh, "Auto-provisioning %s", enable ? "on" : "off");
	prov_auto_set(enable);
	snprintf(response, sizeof(response), "autoProvisioning %s", enable ? "on" : "off");
	uart30_send(response, "autoProvisioningToggled");
	return 0;
}

/* Toggle REMOTE-ONLY scanning: discover unprovisioned devices only through RPR
 * relays, with the gateway's own local beacon scan off.
 */
static int cmd_gtw_scan_rpr(const struct shell *sh, size_t argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Toggling RPR-only scan...");
	prov_scan_rpr_toggle();
	return 0;
}

/* Report the registered RPR Server relays to the cloud. */
static int cmd_gtw_rpr_list(const struct shell *sh, size_t argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Listing RPR servers...");
	prov_rpr_server_list();
	return 0;
}

/* Manually register a node as an RPR Server relay. Usage: rpr-add <addr> */
static int cmd_gtw_rpr_add(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);

	uint16_t addr = (uint16_t)shell_strtoul(argv[1], 0, &err);

	if (err) {
		shell_error(sh, "invalid address: %s", argv[1]);
		return -EINVAL;
	}

	prov_rpr_server_add(addr);
	shell_print(sh, "Registered RPR server 0x%04x", addr);
	return 0;
}

static int kr_parse_idx_list(const struct shell *sh, const char *what,
			     const char *list, uint16_t *out, size_t cap,
			     size_t *out_count)
{
	size_t count = 0;
	const char *p = list;

	while (*p) {
		char *end = NULL;
		unsigned long v = strtoul(p, &end, 0);

		if (end == p) {
			shell_error(sh, "invalid %s list: %s", what, list);
			return -EINVAL;
		}
		if (count >= cap) {
			shell_error(sh, "too many %s (max %u)", what,
					(unsigned int)cap);
			return -EINVAL;
		}
		out[count++] = (uint16_t)v;

		p = end;
		if (*p == ',') {
			p++;
		} else if (*p != '\0') {
			shell_error(sh, "invalid %s list: %s", what, list);
			return -EINVAL;
		}
	}
	*out_count = count;
	return 0;
}

/* Node addresses named by the subnet list. Each takes at least two characters
 * ("2,"), so the command buffer bounds how many there can be.
 */
static uint16_t kr_addr_buf[CONFIG_SHELL_CMD_BUFF_SIZE / 2];

/* Parse "<net_idx>[:<addr>,...][/<net_idx>[:<addr>,...]]..." - each subnet with
 * the remote nodes that hold it, addresses in hex - e.g. "0:2,4/1:4,6".
 */
static int kr_parse_subnets(const struct shell *sh, const char *spec,
			    struct kr_subnet *out, size_t cap, size_t *out_count)
{
	size_t count = 0;
	size_t used = 0;
	const char *p = spec;

	while (*p) {
		char *end = NULL;
		unsigned long net = strtoul(p, &end, 0);

		if (end == p) {
			goto invalid;
		}
		if (count >= cap) {
			shell_error(sh, "too many subnets (max %u)", (unsigned int)cap);
			return -EINVAL;
		}
		out[count].net_idx = (uint16_t)net;
		out[count].addrs = &kr_addr_buf[used];
		out[count].addr_count = 0;
		p = end;

		if (*p == ':') {
			p++;
			while (*p && *p != '/') {
				unsigned long addr = strtoul(p, &end, 16);

				if (end == p || used >= ARRAY_SIZE(kr_addr_buf)) {
					goto invalid;
				}
				kr_addr_buf[used++] = (uint16_t)addr;
				out[count].addr_count++;
				p = end;
				if (*p == ',') {
					p++;
				} else if (*p && *p != '/') {
					goto invalid;
				}
			}
		}
		count++;

		if (*p == '/') {
			p++;
		} else if (*p) {
			goto invalid;
		}
	}
	*out_count = count;
	return 0;

invalid:
	shell_error(sh, "invalid subnet list: %s", spec);
	return -EINVAL;
}

/* A trailing token is the evict address when written as a hex unicast
 * (0x-prefixed); anything else in that position is an AppKey index list.
 */
static bool kr_is_evict_token(const char *s)
{
	return s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
}

/* Roll one or more NetKeys, optionally together with selected AppKeys, across
 * the remote nodes listed with each subnet (hex addresses; the gateway is
 * always included)
 * Usage: keyrefresh <net_idx>[:<addr>,...][/<net_idx>[:<addr>,...]]... [<app_idx>,...] [evict_addr]
 * e.g.   keyrefresh 0:2,4	    keyrefresh 0:2,4/1:4,6 2,5 0x0007	 keyrefresh 1
 */
static int cmd_gtw_keyrefresh(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	uint16_t evict = 0;
	/* Bounded by the CDB subnet/AppKey tables - you cannot select more than
	 * exist.
	 */
	struct kr_subnet nets[CONFIG_BT_MESH_CDB_SUBNET_COUNT];
	uint16_t apps[CONFIG_BT_MESH_CDB_APP_KEY_COUNT];
	size_t net_count = 0;
	size_t app_count = 0;
	size_t idx = 2;

	/* argv[1]: required subnet list with each subnet's nodes, e.g. "0:2,4/1:4". */
	err = kr_parse_subnets(sh, argv[1], nets, ARRAY_SIZE(nets), &net_count);
	if (err) {
		return err;
	}
	if (net_count == 0) {
		shell_error(sh, "no net_idx given");
		return -EINVAL;
	}

	/* Optional AppKey index list: present when the next token is not the hex
	 * evict address.
	 */
	if (idx < argc && !kr_is_evict_token(argv[idx])) {
		err = kr_parse_idx_list(sh, "app_idx", argv[idx], apps,
					ARRAY_SIZE(apps), &app_count);
		if (err) {
			return err;
		}
		idx++;
	}

	/* Optional evict address (a hex unicast like 0x0007). */
	if (idx < argc) {
		evict = (uint16_t)shell_strtoul(argv[idx], 0, &err);
		if (err) {
			shell_error(sh, "invalid address: %s", argv[idx]);
			return -EINVAL;
		}
		idx++;
	}
	if (idx < argc) {
		shell_error(sh, "too many arguments");
		return -EINVAL;
	}

	shell_print(sh,
		    "Starting key refresh for %u NetKey(s) + %u AppKey(s)%s...",
		    (unsigned int)net_count, (unsigned int)app_count,
		    evict ? " (evicting a node)" : "");
	key_refresh_run(nets, net_count, apps, app_count, evict);
	return 0;
}

/* Latency probe echo */
static int cmd_latency(const struct shell *sh, size_t argc, char *argv[])
{
	ARG_UNUSED(sh);
	uart30_send(argc > 1 ? argv[1] : "", "latency");
	return 0;
}

/* Subcommand table under `gtw` */
SHELL_STATIC_SUBCMD_SET_CREATE(gtw_cmds,
	SHELL_CMD_ARG(init, NULL,
		      "Initialise the gateway: init [address]",
		      cmd_gtw_init, 1, 1),
	SHELL_CMD_ARG(appkey, NULL,
		      "Add appkey & bind local models: appkey <net_idx> <app_idx> [app_key]",
		      cmd_gtw_appkey, 3, 1),
	SHELL_CMD_ARG(subnet, NULL,
		      "Create/ensure subnet: subnet <net_idx> [netkey_hex]",
		      cmd_gtw_subnet, 2, 1),
	SHELL_CMD_ARG(addsubnet, NULL,
		      "Add subnet to a node: addsubnet <net_idx> <addr_hex> <add_net_idx>",
		      cmd_gtw_addsubnet, 4, 0),
	SHELL_CMD_ARG(node_delsubnet, NULL,
		      "Remove subnet from a node (and its AppKeys): "
		      "node_delsubnet <net_idx> <addr_hex> <del_net_idx>",
		      cmd_gtw_node_delsubnet, 4, 0),
	SHELL_CMD_ARG(node_delappkey, NULL,
		      "Remove one AppKey from a node: "
		      "node_delappkey <net_idx> <addr_hex> <app_idx>",
		      cmd_gtw_node_delappkey, 4, 0),
	SHELL_CMD_ARG(node_delgroup, NULL,
		      "Remove group from a node: "
		      "node_delgroup <net_idx> <addr_hex> <group_hex>",
		      cmd_gtw_node_delgroup, 4, 0),
	SHELL_CMD(cdb, NULL, "Send CDB to cloud", cmd_gtw_cdb),
	SHELL_CMD(scan, NULL,
		  "Toggle scanning for unprovisioned devices",
		  cmd_gtw_scan),
	SHELL_CMD_ARG(provandconfig, NULL,
		      "Provision & configure: provandconfig <uuid> <net_idx> <app_idx> <group_hex>",
		      cmd_gtw_provandconfig, 5, 0),
	SHELL_CMD_ARG(cfg, NULL,
		      "Configure node: cfg <net_idx> <addr_hex> <app_idx> <group_hex>",
		      cmd_gtw_cfg, 5, 0),
	SHELL_CMD_ARG(light, NULL,
		      "Set on/off: light <net_idx> <app_idx> <dst_addr> <on/off>",
		      cmd_gtw_light, 5, 0),
	SHELL_CMD_ARG(bri, NULL,
		      "Set brightness: bri <net_idx> <app_idx> <dst_addr> <0-100>",
		      cmd_gtw_bri, 5, 0),
	SHELL_CMD_ARG(scene, NULL,
		"Recall/store scene: scene <recall|store> <net_idx> <app_idx> <dst_addr> <scene>",
		      cmd_gtw_scene, 6, 0),
	SHELL_CMD_ARG(energy, NULL,
		      "Read energy use: energy <net_idx> <app_idx> <dst_addr>",
		      cmd_gtw_energy, 4, 0),
	SHELL_CMD_ARG(replace, NULL,
		      "Rejoin mesh: replace <netkey_hex> <address>",
		      cmd_gtw_replace, 3, 0),
	SHELL_CMD_ARG(node, NULL,
		      "Add node to CDB: node <uuid_hex> <devkey_hex> <address> <elements> <net_idx>",
		      cmd_gtw_node, 6, 0),
	SHELL_CMD_ARG(auto-provision, NULL,
		"Toggle auto-provisioning: auto-provision <on/off> <net_idx> <app_idx> <group_hex>",
		      cmd_gtw_auto_provision, 5, 0),
	SHELL_CMD(scan-rpr, NULL,
		  "Toggle RPR-only scanning",
		  cmd_gtw_scan_rpr),
	SHELL_CMD(rpr-list, NULL,
		  "List registered RPR server relays",
		  cmd_gtw_rpr_list),
	SHELL_CMD_ARG(rpr-add, NULL,
		      "Register RPR server relay: rpr-add <addr>",
		      cmd_gtw_rpr_add, 2, 0),
	SHELL_CMD_ARG(keyrefresh, NULL,
	"Roll NetKeys (+ optional AppKeys): "
	"keyrefresh <net_idx>[:<addr>,...][/...] [<app_idx>,...] [evict_addr]",
		      cmd_gtw_keyrefresh, 2, 2),
	SHELL_SUBCMD_SET_END
);

/* Register `gtw` as a root command */
SHELL_CMD_REGISTER(gtw, &gtw_cmds, "Gateway commands", NULL);

/* Register `latency` as a root command: the gateway's round-trip probe echo. */
SHELL_CMD_REGISTER(latency, NULL,
		   "Echo a latency probe id back to the gateway: latency <id>",
		   cmd_latency);
