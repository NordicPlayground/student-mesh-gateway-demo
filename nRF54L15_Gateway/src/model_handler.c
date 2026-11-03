/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <bluetooth/mesh/models.h>
#include <bluetooth/mesh/gen_onoff_cli.h>
#include <errno.h>
#include <stdio.h>
#include <zephyr/sys/printk.h>
#include "uart_cmd.h"

/*ADDED INCLUDES*/
#include <zephyr/shell/shell.h>
#include <zephyr/bluetooth/mesh/shell.h>

static struct bt_mesh_cfg_cli cfg_cli;

#if defined(CONFIG_BT_MESH_RPR_CLI)
/* Remote Provisioning Client instance */
static struct bt_mesh_rpr_cli rpr_cli;

struct bt_mesh_rpr_cli *model_handler_rpr_cli(void)
{
	return &rpr_cli;
}
#endif

/* callback for receiving on/off status messages from the mesh network, sends status to cloud */
static void OnOff_status_cb(struct bt_mesh_onoff_cli *cli,
			    struct bt_mesh_msg_ctx *ctx,
			    const struct bt_mesh_onoff_status *status)
{
	(void)cli;

	if (!ctx || !status) {
		printk("OnOff status callback received invalid data\n");
		return;
	}

	printk("OnOff status from 0x%04x: status=%u",
	       ctx->addr,
	       status->present_on_off ? 1U : 0U);

	if (status->remaining_time >= 0) {
		printk(", target=%u, remaining=%ld ms",
		       status->target_on_off ? 1U : 0U,
		       (long)status->remaining_time);
	}

	printk("\n");

	char status_msg[96];

	snprintf(status_msg, sizeof(status_msg),
		 "src=0x%04x status=%s",
		 ctx->addr,
		 status->present_on_off ? "ON" : "OFF");
	uart30_send(status_msg, "OnOff_status");
}

static struct bt_mesh_onoff_cli onoff_cli = BT_MESH_ONOFF_CLI_INIT(OnOff_status_cb);

/* callback for Light Lightness status, forwards the brightness level to the cloud */
static void lightness_status_cb(struct bt_mesh_lightness_cli *cli,
				struct bt_mesh_msg_ctx *ctx,
				const struct bt_mesh_lightness_status *status)
{
	(void)cli;

	if (!ctx || !status) {
		return;
	}

	printk("Lightness status from 0x%04x: lvl=%u\n", ctx->addr, status->current);

	char msg[96];

	snprintf(msg, sizeof(msg), "src=0x%04x lvl=%u", ctx->addr, status->current);
	/* Subject must be <=15 chars: the nRF9151 forwarder parses the appId with
	 * sscanf("%15s") into a 16-byte buffer, so "Lightness_status" (16) would be
	 * truncated to "Lightness_statu" and never match on the cloud side.
	 */
	uart30_send(msg, "Light_status");
}

static const struct bt_mesh_lightness_cli_handlers lightness_cli_handlers = {
	.light_status = lightness_status_cb,
};

/* Light Lightness client, used by the gateway to set absolute brightness on fixtures */
static struct bt_mesh_lightness_cli lightness_cli =
	BT_MESH_LIGHTNESS_CLI_INIT(&lightness_cli_handlers);

/* callback for Scene status, forwards the current scene to the cloud */
static void scene_status_cb(struct bt_mesh_scene_cli *cli,
			    struct bt_mesh_msg_ctx *ctx,
			    const struct bt_mesh_scene_state *state)
{
	(void)cli;

	if (!ctx || !state) {
		return;
	}

	printk("Scene status from 0x%04x: current=%u\n", ctx->addr, state->current);

	char msg[96];

	snprintf(msg, sizeof(msg), "src=0x%04x scene=%u", ctx->addr, state->current);
	uart30_send(msg, "Scene_status");
}

/* Scene client, used by the gateway to store/recall scenes on fixtures */
static struct bt_mesh_scene_cli scene_cli = {
	.status = scene_status_cb,
};

/* callback for Sensor Status: decode the Precise Total Device Energy Use value
 * and forward it to the cloud. The Sensor Client already decodes the on-air
 * wire format into a bt_mesh_sensor_value; we convert that to a number using
 * the property's format (energy32: unit kWh, resolution 0.001 kWh).
 */
static void sensor_data_cb(struct bt_mesh_sensor_cli *cli,
			   struct bt_mesh_msg_ctx *ctx,
			   const struct bt_mesh_sensor_type *sensor,
			   const struct bt_mesh_sensor_value *value)
{
	(void)cli;

	if (!ctx || !sensor || !value) {
		return;
	}

	/* The gateway only queries the energy-use sensor; ignore anything else */
	if (sensor->id != BT_MESH_PROP_ID_PRECISE_TOT_DEV_ENERGY_USE) {
		printk("Sensor status from 0x%04x: unhandled property 0x%04x\n",
		       ctx->addr, sensor->id);
		return;
	}

	int64_t micro_kwh = 0;
	enum bt_mesh_sensor_value_status st =
		bt_mesh_sensor_value_to_micro(&value[0], &micro_kwh);
	char msg[64];

	if (!bt_mesh_sensor_value_status_is_numeric(st)) {
		printk("Energy status from 0x%04x: non-numeric value (status %d)\n",
		       ctx->addr, (int)st);
		snprintf(msg, sizeof(msg), "src=0x%04x ukwh=unknown", ctx->addr);
		uart30_send(msg, "Energy_status");
		return;
	}
	printk("Energy status from 0x%04x: %lld micro-kWh\n", ctx->addr, micro_kwh);

	snprintf(msg, sizeof(msg), "src=0x%04x ukwh=%lld", ctx->addr, micro_kwh);

	uart30_send(msg, "Energy_status");
}

static const struct bt_mesh_sensor_cli_handlers sensor_cli_handlers = {
	.data = sensor_data_cb,
};

static struct bt_mesh_sensor_cli sensor_cli =
	BT_MESH_SENSOR_CLI_INIT(&sensor_cli_handlers);

#if defined(CONFIG_BT_MESH_DFD_SRV)
static struct bt_mesh_dfd_srv dfd_srv;
#endif

#if defined(CONFIG_BT_MESH_SAR_CFG_CLI)
static struct bt_mesh_sar_cfg_cli sar_cfg_cli;
#endif

#if defined(CONFIG_BT_MESH_PRIV_BEACON_CLI)
static struct bt_mesh_priv_beacon_cli priv_beacon_cli;
#endif

#if defined(CONFIG_BT_MESH_SOL_PDU_RPL_CLI)
static struct bt_mesh_sol_pdu_rpl_cli srpl_cli;
#endif


#if defined(CONFIG_BT_MESH_OD_PRIV_PROXY_CLI)
static struct bt_mesh_od_priv_proxy_cli od_priv_proxy_cli;
#endif

#if defined(CONFIG_BT_MESH_LARGE_COMP_DATA_CLI)
struct bt_mesh_large_comp_data_cli large_comp_data_cli;
#endif

#if defined(CONFIG_BT_MESH_BRG_CFG_CLI)
static struct bt_mesh_brg_cfg_cli brg_cfg_cli;
#endif

BT_MESH_SHELL_HEALTH_PUB_DEFINE(health_pub);

static const struct bt_mesh_model root_models[] = {
	BT_MESH_MODEL_CFG_SRV,
	BT_MESH_MODEL_CFG_CLI(&cfg_cli),
	BT_MESH_MODEL_ONOFF_CLI(&onoff_cli),
	BT_MESH_MODEL_LIGHTNESS_CLI(&lightness_cli),
	BT_MESH_MODEL_SCENE_CLI(&scene_cli),
	BT_MESH_MODEL_HEALTH_SRV(&bt_mesh_shell_health_srv, &health_pub, health_srv_meta),
	BT_MESH_MODEL_HEALTH_CLI(&bt_mesh_shell_health_cli),
#if defined(CONFIG_BT_MESH_DFD_SRV)
	BT_MESH_MODEL_DFD_SRV(&dfd_srv),
#else
#if defined(CONFIG_BT_MESH_SHELL_DFU_SRV)
	BT_MESH_MODEL_DFU_SRV(&bt_mesh_shell_dfu_srv),
#elif defined(CONFIG_BT_MESH_SHELL_BLOB_SRV)
	BT_MESH_MODEL_BLOB_SRV(&bt_mesh_shell_blob_srv),
#endif
#if defined(CONFIG_BT_MESH_SHELL_DFU_CLI)
	BT_MESH_MODEL_DFU_CLI(&bt_mesh_shell_dfu_cli),
#elif defined(CONFIG_BT_MESH_SHELL_BLOB_CLI)
	BT_MESH_MODEL_BLOB_CLI(&bt_mesh_shell_blob_cli),
#endif
#endif /* CONFIG_BT_MESH_DFD_SRV */
#if defined(CONFIG_BT_MESH_RPR_CLI)
	BT_MESH_MODEL_RPR_CLI(&rpr_cli),
#endif
#if defined(CONFIG_BT_MESH_RPR_SRV)
	BT_MESH_MODEL_RPR_SRV,
#endif

#if defined(CONFIG_BT_MESH_SAR_CFG_SRV)
	BT_MESH_MODEL_SAR_CFG_SRV,
#endif
#if defined(CONFIG_BT_MESH_SAR_CFG_CLI)
	BT_MESH_MODEL_SAR_CFG_CLI(&sar_cfg_cli),
#endif

#if defined(CONFIG_BT_MESH_OP_AGG_SRV)
	BT_MESH_MODEL_OP_AGG_SRV,
#endif
#if defined(CONFIG_BT_MESH_OP_AGG_CLI)
	BT_MESH_MODEL_OP_AGG_CLI,
#endif

#if defined(CONFIG_BT_MESH_LARGE_COMP_DATA_SRV)
	BT_MESH_MODEL_LARGE_COMP_DATA_SRV,
#endif
#if defined(CONFIG_BT_MESH_LARGE_COMP_DATA_CLI)
	BT_MESH_MODEL_LARGE_COMP_DATA_CLI(&large_comp_data_cli),
#endif

#if defined(CONFIG_BT_MESH_PRIV_BEACON_SRV)
	BT_MESH_MODEL_PRIV_BEACON_SRV,
#endif
#if defined(CONFIG_BT_MESH_PRIV_BEACON_CLI)
	BT_MESH_MODEL_PRIV_BEACON_CLI(&priv_beacon_cli),
#endif
#if defined(CONFIG_BT_MESH_OD_PRIV_PROXY_CLI)
	BT_MESH_MODEL_OD_PRIV_PROXY_CLI(&od_priv_proxy_cli),
#endif
#if defined(CONFIG_BT_MESH_SOL_PDU_RPL_CLI)
	BT_MESH_MODEL_SOL_PDU_RPL_CLI(&srpl_cli),
#endif
#if defined(CONFIG_BT_MESH_OD_PRIV_PROXY_SRV)
	BT_MESH_MODEL_OD_PRIV_PROXY_SRV,
#endif
#if defined(CONFIG_BT_MESH_BRG_CFG_SRV)
	BT_MESH_MODEL_BRG_CFG_SRV,
#endif
#if defined(CONFIG_BT_MESH_BRG_CFG_CLI)
	BT_MESH_MODEL_BRG_CFG_CLI(&brg_cfg_cli),
#endif
	/* Appended last on purpose: keeps the composition-data model indices of
	 * every model above unchanged, so a firmware update that adds this model
	 * doesn't misalign bindings/subscriptions restored from flash.
	 */
	BT_MESH_MODEL_SENSOR_CLI(&sensor_cli),
};

static const struct bt_mesh_elem elements[] = {
	BT_MESH_ELEM(0, root_models, BT_MESH_MODEL_NONE),
};


static const struct bt_mesh_comp comp = {
	.cid = CONFIG_BT_COMPANY_ID,
	.elem = elements,
	.elem_count = ARRAY_SIZE(elements),
};

const struct bt_mesh_comp *model_handler_init(void)
{
	return &comp;
}

/* creates and send a mesh message setting the on/off state of a light */
int model_handler_onoff_set(uint16_t net_idx, uint16_t app_idx, uint16_t dst_addr, bool on_off)
{
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net_idx,
		.app_idx = app_idx,
		.addr = dst_addr,
		.send_ttl = BT_MESH_TTL_DEFAULT,
	};
	struct bt_mesh_onoff_set set = {
		.on_off = on_off,
		.reuse_transaction = false,
		.transition = NULL,
	};

	if (!bt_mesh_is_provisioned()) {
		return -EAGAIN;
	}

	return bt_mesh_onoff_cli_set_unack(&onoff_cli, &ctx, &set);
}

/* sets the absolute brightness (0-100%) of a Light Lightness Server fixture */
int model_handler_lightness_set(uint16_t net_idx, uint16_t app_idx, uint16_t dst_addr,
				uint8_t percent)
{
	if (percent > 100U) {
		percent = 100U;
	}

	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net_idx,
		.app_idx = app_idx,
		.addr = dst_addr,
		.send_ttl = BT_MESH_TTL_DEFAULT,
	};
	struct bt_mesh_lightness_set set = {
		/* Scale 0-100% to the 0-65535 Lightness Actual range. */
		.lvl = (uint16_t)(((uint32_t)percent * 65535U) / 100U),
		.transition = NULL,
	};

	if (!bt_mesh_is_provisioned()) {
		return -EAGAIN;
	}

	return bt_mesh_lightness_cli_light_set_unack(&lightness_cli, &ctx, &set);
}

/* recalls a stored scene on a Scene Server */
int model_handler_scene_recall(uint16_t net_idx, uint16_t app_idx, uint16_t dst_addr,
			       uint16_t scene)
{
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net_idx,
		.app_idx = app_idx,
		.addr = dst_addr,
		.send_ttl = BT_MESH_TTL_DEFAULT,
	};

	if (!bt_mesh_is_provisioned()) {
		return -EAGAIN;
	}

	return bt_mesh_scene_cli_recall_unack(&scene_cli, &ctx, scene, NULL);
}

/* stores the current fixture state as a scene on a Scene Server */
int model_handler_scene_store(uint16_t net_idx, uint16_t app_idx, uint16_t dst_addr,
			      uint16_t scene)
{
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net_idx,
		.app_idx = app_idx,
		.addr = dst_addr,
		.send_ttl = BT_MESH_TTL_DEFAULT,
	};

	if (!bt_mesh_is_provisioned()) {
		return -EAGAIN;
	}

	return bt_mesh_scene_cli_store_unack(&scene_cli, &ctx, scene);
}

/* Read Precise Total Device Energy Use from a fixture's Sensor Server.
 * Non-blocking (rsp = NULL): the decoded value arrives in sensor_data_cb.
 */
int model_handler_sensor_get(uint16_t net_idx, uint16_t app_idx, uint16_t dst_addr)
{
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net_idx,
		.app_idx = app_idx,
		.addr = dst_addr,
		.send_ttl = BT_MESH_TTL_DEFAULT,
	};

	if (!bt_mesh_is_provisioned()) {
		return -EAGAIN;
	}

	return bt_mesh_sensor_cli_get(&sensor_cli, &ctx,
				      &bt_mesh_sensor_precise_tot_dev_energy_use, NULL);
}
