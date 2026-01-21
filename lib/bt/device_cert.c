/*
 * Copyright (c) 2026 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>

#include <pouch/transport/gatt/common/packetizer.h>

#include <pouch_gateway/bt/connect.h>

#include <pouch_gateway/cert.h>

#include "cert.h"
#include "uplink.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(device_cert_gatt, CONFIG_POUCH_GATEWAY_GATT_LOG_LEVEL);


static void device_cert_cleanup(struct bt_conn *conn)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (node->device_cert_ctx)
    {
        pouch_gateway_device_cert_abort(node->device_cert_ctx);
        node->device_cert_ctx = NULL;
    }
}

static uint8_t device_cert_read_cb(struct bt_conn *conn,
                                   uint8_t err,
                                   struct bt_gatt_read_params *params,
                                   const void *data,
                                   uint16_t length)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (err)
    {
        LOG_ERR("Failed to read BLE GATT %s (err %d)", "device cert", err);
        device_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return BT_GATT_ITER_STOP;
    }

    if (length == 0)
    {
        LOG_ERR("No device cert");
        device_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return BT_GATT_ITER_STOP;
    }

    bool is_first = false;
    bool is_last = false;
    const void *payload = NULL;
    ssize_t payload_len = pouch_gatt_packetizer_decode(data, length, &payload, &is_first, &is_last);
    if (payload_len < 0)
    {
        LOG_ERR("Failed to decode BLE GATT %s (err %d)", "device cert", (int) payload_len);
        device_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return BT_GATT_ITER_STOP;
    }

    if (data)
    {
        LOG_HEXDUMP_DBG(data, length, "[READ] BLE GATT device cert");
    }

    pouch_gateway_device_cert_push(node->device_cert_ctx, payload, payload_len);

    if (is_last)
    {
        int err = pouch_gateway_device_cert_finish(node->device_cert_ctx);
        if (err)
        {
            LOG_ERR("Failed to finish device cert: %d", err);
            device_cert_cleanup(conn);
            pouch_gateway_bt_finished(conn);
            return BT_GATT_ITER_STOP;
        }

        pouch_gateway_uplink_start(conn);
        return BT_GATT_ITER_STOP;
    }

    err = bt_gatt_read(conn, params);
    if (err)
    {
        LOG_ERR("BT (re)read request failed: %d", err);
        device_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return BT_GATT_ITER_STOP;
    }

    return BT_GATT_ITER_STOP;
}

void pouch_gateway_device_cert_read(struct bt_conn *conn)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (node->device_cert_provisioned)
    {
        pouch_gateway_uplink_start(conn);
        return;
    }

    struct bt_gatt_read_params *read_params = &node->read_params;
    memset(read_params, 0, sizeof(*read_params));

    node->device_cert_ctx = pouch_gateway_device_cert_start();
    if (node->device_cert_ctx == NULL)
    {
        LOG_ERR("Failed to allocate device cert context");
        device_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return;
    }

    read_params->func = device_cert_read_cb;
    read_params->handle_count = 1;
    read_params->single.handle = node->attr_handles[POUCH_GATEWAY_GATT_ATTR_DEVICE_CERT].value;
    int err = bt_gatt_read(conn, read_params);
    if (err)
    {
        LOG_ERR("BT read request failed: %d", err);
        device_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
    }
}
