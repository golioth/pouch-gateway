/*
 * Copyright (c) 2025 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>

#include <pouch/transport/gatt/common/packetizer.h>

#include <pouch_gateway/types.h>
#include <pouch_gateway/uplink.h>
#include <pouch_gateway/bt/connect.h>
#include <pouch_gateway/bt/downlink.h>
#include <pouch_gateway/bt/uplink.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(uplink_gatt);

static uint8_t handle_uplink_payload(struct bt_conn *conn, const void *data, uint16_t length)
{
    bool is_first = false;
    bool is_last = false;
    const void *payload = NULL;
    ssize_t payload_len = pouch_gatt_packetizer_decode(data, length, &payload, &is_first, &is_last);
    if (payload_len < 0)
    {
        LOG_ERR("Failed to decode BLE GATT %s (err %d)", "Uplink", (int) payload_len);
        pouch_gateway_bt_finished(conn);
        return BT_GATT_ITER_STOP;
    }

    if (data)
    {
        LOG_HEXDUMP_INF(data, length, "[READ] BLE GATT Uplink");
    }

    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    int ret = pouch_gateway_uplink_write(node->uplink, payload, payload_len, is_last);
    if (ret)
    {
        LOG_ERR("Failed to write to pouch (err %d)", ret);
        pouch_gateway_bt_finished(conn);
        return BT_GATT_ITER_STOP;
    }

    if (is_last)
    {
        pouch_gateway_uplink_close(node->uplink);
        node->uplink = NULL;

        if (!IS_ENABLED(CONFIG_POUCH_GATEWAY_CLOUD))
        {
            pouch_gateway_bt_finished(conn);
        }

        return BT_GATT_ITER_STOP;
    }

    return BT_GATT_ITER_CONTINUE;
}

/* Callback for handling BLE GATT Uplink Response */
static uint8_t tf_uplink_read_cb(struct bt_conn *conn,
                                 uint8_t err,
                                 struct bt_gatt_read_params *params,
                                 const void *data,
                                 uint16_t length)
{
    if (err)
    {
        LOG_ERR("Failed to read BLE GATT %s (err %d)", "Uplink", err);
        return BT_GATT_ITER_STOP;
    }

    err = handle_uplink_payload(conn, data, length);

    if (BT_GATT_ITER_STOP == err)
    {
        return err;
    }

    err = bt_gatt_read(conn, params);
    if (err)
    {
        LOG_ERR("BT (re)read request failed: %d", err);
        return BT_GATT_ITER_STOP;
    }

    return BT_GATT_ITER_STOP;
}

static uint8_t tf_uplink_indicate_cb(struct bt_conn *conn,
                                     struct bt_gatt_subscribe_params *params,
                                     const void *data,
                                     uint16_t length)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (NULL == data)
    {
        LOG_DBG("Subscription terminated");

        if (node->uplink)
        {
            LOG_WRN("Subscription terminated while uplink is open");
            pouch_gateway_uplink_close(node->uplink);
            node->uplink = NULL;

            pouch_gateway_bt_finished(conn);
        }

        return BT_GATT_ITER_STOP;
    }

    return handle_uplink_payload(conn, data, length);
}

static void uplink_end_cb(void *conn, enum pouch_gateway_uplink_result res)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);
    node->uplink = NULL;

    bt_gatt_unsubscribe(conn, &node->subscribe_params);

    if (POUCH_GATEWAY_UPLINK_SUCCESS != res)
    {
        pouch_gateway_bt_finished(conn);
    }
}

void pouch_gateway_uplink_start(struct bt_conn *conn)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    struct pouch_gateway_downlink_context *downlink = pouch_gateway_downlink_start(conn);

    node->uplink = pouch_gateway_uplink_open(downlink, uplink_end_cb, conn);
    if (node->uplink == NULL)
    {
        LOG_ERR("Failed to open pouch uplink");
        pouch_gateway_bt_finished(conn);
        return;
    }

    if (node->attr_handles[POUCH_GATEWAY_GATT_ATTR_UPLINK].ccc)
    {
        struct bt_gatt_subscribe_params *subscribe_params = &node->subscribe_params;
        memset(subscribe_params, 0, sizeof(*subscribe_params));

        subscribe_params->notify = tf_uplink_indicate_cb;
        subscribe_params->value = BT_GATT_CCC_INDICATE;
        subscribe_params->value_handle = node->attr_handles[POUCH_GATEWAY_GATT_ATTR_UPLINK].value;
        subscribe_params->ccc_handle = node->attr_handles[POUCH_GATEWAY_GATT_ATTR_UPLINK].ccc;
        int err = bt_gatt_subscribe(conn, subscribe_params);
        if (err)
        {
            LOG_ERR("BT subscribe request failed: %d", err);
            pouch_gateway_bt_finished(conn);
        }
    }
    else
    {
        struct bt_gatt_read_params *read_params = &node->read_params;
        memset(read_params, 0, sizeof(*read_params));

        read_params->func = tf_uplink_read_cb;
        read_params->handle_count = 1;
        read_params->single.handle = node->attr_handles[POUCH_GATEWAY_GATT_ATTR_UPLINK].value;
        int err = bt_gatt_read(conn, read_params);
        if (err)
        {
            LOG_ERR("BT read request failed: %d", err);
            pouch_gateway_bt_finished(conn);
        }
    }
}

void pouch_gateway_uplink_cleanup(struct bt_conn *conn)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (node->uplink)
    {
        pouch_gateway_uplink_close(node->uplink);
        node->uplink = NULL;
    }
}
