/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @file
 * @brief Model handler
 */

#ifndef MODEL_HANDLER_H__
#define MODEL_HANDLER_H__

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/bluetooth/mesh.h>

#ifdef __cplusplus
extern "C" {
#endif

const struct bt_mesh_comp *model_handler_init(void);
int model_handler_onoff_set(uint16_t net_idx, uint16_t app_idx, uint16_t dst_addr, bool on_off);

/* Set absolute brightness (0-100%) on a Light Lightness Server (light_ctrl fixture). */
int model_handler_lightness_set(uint16_t net_idx, uint16_t app_idx, uint16_t dst_addr,
				uint8_t percent);

/* Recall/store a scene on a Scene Server. */
int model_handler_scene_recall(uint16_t net_idx, uint16_t app_idx, uint16_t dst_addr,
			       uint16_t scene);
int model_handler_scene_store(uint16_t net_idx, uint16_t app_idx, uint16_t dst_addr,
			      uint16_t scene);

/* Read bt_mesh_sensor_precise_tot_dev_energy_use
 */
int model_handler_sensor_get(uint16_t net_idx, uint16_t app_idx, uint16_t dst_addr);

#if defined(CONFIG_BT_MESH_RPR_CLI)
/* Remote Provisioning Client model instance */
struct bt_mesh_rpr_cli *model_handler_rpr_cli(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MODEL_HANDLER_H__ */
