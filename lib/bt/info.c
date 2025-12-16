/*
 * Copyright (c) 2025 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pouch/transport/gatt/common/packetizer.h>

#include <pouch_gateway/info.h>
#include <pouch_gateway/bt/cert.h>
#include <pouch_gateway/bt/connect.h>
#include <pouch_gateway/bt/info.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(info_gatt, CONFIG_POUCH_GATEWAY_GATT_LOG_LEVEL);

static void info_cleanup(struct bt_conn *conn)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (node->info_ctx)
    {
        pouch_gateway_info_abort(node->info_ctx);
        node->info_ctx = NULL;
    }
}

static uint8_t info_read_cb(struct bt_conn *conn,
                            uint8_t err,
                            struct bt_gatt_read_params *params,
                            const void *data,
                            uint16_t length)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (err)
    {
        LOG_ERR("Failed to read BLE GATT %s (err %d)", "info", err);
        info_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return BT_GATT_ITER_STOP;
    }

    if (length == 0)
    {
        info_cleanup(conn);
        pouch_gateway_cert_exchange_start(conn);
        return BT_GATT_ITER_STOP;
    }

    bool is_first = false;
    bool is_last = false;
    const void *payload = NULL;
    ssize_t payload_len = pouch_gatt_packetizer_decode(data, length, &payload, &is_first, &is_last);
    if (payload_len < 0)
    {
        LOG_ERR("Failed to decode BLE GATT %s (err %d)", "info", (int) payload_len);
        info_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return BT_GATT_ITER_STOP;
    }

    if (data)
    {
        LOG_HEXDUMP_DBG(data, length, "[READ] BLE GATT info");
    }

    pouch_gateway_info_push(node->info_ctx, payload, payload_len);

    if (is_last)
    {
        int err = pouch_gateway_info_finish(node->info_ctx,
                                            &node->server_cert_provisioned,
                                            &node->device_cert_provisioned);
        node->info_ctx = NULL;
        if (err)
        {
            LOG_ERR("Failed to parse info: %d", err);
            /* Continue anyway, as nothing contained in info is critical */
        }

        info_cleanup(conn);
        pouch_gateway_cert_exchange_start(conn);
        return BT_GATT_ITER_STOP;
    }

    err = bt_gatt_read(conn, params);
    if (err)
    {
        LOG_ERR("BT (re)read request failed: %d", err);
        info_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return BT_GATT_ITER_STOP;
    }

    return BT_GATT_ITER_STOP;
}

static void gateway_info_read_start(struct bt_conn *conn)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    struct bt_gatt_read_params *read_params = &node->read_params;
    memset(read_params, 0, sizeof(*read_params));

    node->server_cert_provisioned = false;
    node->device_cert_provisioned = false;

    node->info_ctx = pouch_gateway_info_start();
    if (node->info_ctx == NULL)
    {
        LOG_ERR("Failed to start info read");
        pouch_gateway_bt_finished(conn);
        return;
    }

    read_params->func = info_read_cb;
    read_params->handle_count = 1;
    read_params->single.handle = node->attr_handles[POUCH_GATEWAY_GATT_ATTR_INFO].value;
    int err = bt_gatt_read(conn, read_params);
    if (err)
    {
        LOG_ERR("BT read request failed: %d", err);
        info_cleanup(conn);
        pouch_gateway_bt_finished(conn);
    }
}

void pouch_gateway_info_read_start(struct bt_conn *conn)
{
    gateway_info_read_start(conn);
}
