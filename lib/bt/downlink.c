/*
 * Copyright (c) 2025 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>

#include <pouch/transport/gatt/common/packetizer.h>
#include <pouch/transport/gatt/common/sender.h>

#include <pouch_gateway/bt/connect.h>
#include <pouch_gateway/types.h>
#include <pouch_gateway/downlink.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(downlink_gatt, CONFIG_POUCH_GATEWAY_GATT_LOG_LEVEL);

static bool subscribed = false;

static enum pouch_gatt_packetizer_result downlink_packet_fill_cb(void *dst,
                                                                 size_t *dst_len,
                                                                 void *user_arg)
{
    bool last = false;

    int ret = pouch_gateway_downlink_get_data(user_arg, dst, dst_len, &last);
    if (-EAGAIN == ret)
    {
        LOG_DBG("Awaiting additional downlink data from cloud");
        return POUCH_GATT_PACKETIZER_MORE_DATA;
    }
    if (0 > ret)
    {
        *dst_len = 0;
        return POUCH_GATT_PACKETIZER_ERROR;
    }

    return last ? POUCH_GATT_PACKETIZER_NO_MORE_DATA : POUCH_GATT_PACKETIZER_MORE_DATA;
}

static int write_downlink_characteristic(void *conn, const void *data, size_t length)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);
    uint16_t downlink_handle = node->attr_handles[POUCH_GATEWAY_GATT_ATTR_DOWNLINK].value;

    int err = bt_gatt_write_without_response(conn, downlink_handle, data, length, false);
    if (err)
    {
        LOG_ERR("GATT write error: %d", err);
    }

    return err;
}

static uint8_t downlink_notify_cb(struct bt_conn *conn,
                                  struct bt_gatt_subscribe_params *params,
                                  const void *data,
                                  uint16_t length)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (NULL == data)
    {
        LOG_DBG("Subscription terminated");
        return BT_GATT_ITER_STOP;
    }

    bool complete = false;
    int err = pouch_gatt_sender_receive_ack(node->downlink_sender, data, length, &complete);
    if (err)
    {
        if (err > 0)
        {
            LOG_ERR("Received NACK %d", err);
        }
        else
        {
            LOG_ERR("Error handling ack: %d", err);
        }

        pouch_gateway_downlink_abort(node->downlink_ctx);
        node->downlink_ctx = NULL;
        pouch_gatt_packetizer_finish(node->packetizer);
        node->packetizer = NULL;

        pouch_gateway_bt_finished(conn);

        return BT_GATT_ITER_STOP;
    }

    if (complete)
    {
        pouch_gateway_downlink_close(node->downlink_ctx);
        node->downlink_ctx = NULL;
        pouch_gatt_packetizer_finish(node->packetizer);
        node->packetizer = NULL;

        pouch_gateway_bt_finished(conn);

        return BT_GATT_ITER_STOP;
    }

    return BT_GATT_ITER_CONTINUE;
}

static void downlink_data_available(void *arg)
{
    struct bt_conn *conn = arg;

    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (!subscribed)
    {
        struct bt_gatt_subscribe_params *subscribe_params = &node->downlink_subscribe_params;
        memset(subscribe_params, 0, sizeof(*subscribe_params));

        subscribe_params->notify = downlink_notify_cb;
        subscribe_params->value = BT_GATT_CCC_NOTIFY;
        subscribe_params->value_handle = node->attr_handles[POUCH_GATEWAY_GATT_ATTR_DOWNLINK].value;
        subscribe_params->ccc_handle = node->attr_handles[POUCH_GATEWAY_GATT_ATTR_DOWNLINK].ccc;
        atomic_set_bit(subscribe_params->flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
        int err = bt_gatt_subscribe(conn, subscribe_params);
        if (err)
        {
            LOG_ERR("BT subscribe request failed: %d", err);
            /* TODO: cleanup */
        }

        subscribed = true;

        return;
    }
    else
    {
        pouch_gatt_sender_data_available(node->downlink_sender);
    }
}

struct pouch_gateway_downlink_context *pouch_gateway_downlink_start(struct bt_conn *conn)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (0 == node->attr_handles[POUCH_GATEWAY_GATT_ATTR_DOWNLINK].value)
    {
        LOG_ERR("Downlink characteristic undiscovered");
        return NULL;
    }

    if (!IS_ENABLED(CONFIG_POUCH_GATEWAY_CLOUD))
    {
        return NULL;
    }

    size_t mtu = bt_gatt_get_mtu(conn) - POUCH_GATEWAY_BT_ATT_OVERHEAD;

    node->downlink_ctx = pouch_gateway_downlink_open(downlink_data_available, conn);
    if (node->downlink_ctx == NULL)
    {
        LOG_ERR("Failed to open downlink");
        return NULL;
    }

    node->packetizer =
        pouch_gatt_packetizer_start_callback(downlink_packet_fill_cb, node->downlink_ctx);
    if (node->packetizer == NULL)
    {
        LOG_ERR("Failed to start packetizer");
        pouch_gateway_downlink_close(node->downlink_ctx);
        node->downlink_ctx = NULL;
        return NULL;
    }

    node->downlink_sender =
        pouch_gatt_sender_create(node->packetizer, write_downlink_characteristic, conn, mtu);

    subscribed = false;

    return node->downlink_ctx;
}

void pouch_gateway_downlink_cleanup(struct bt_conn *conn)
{
    LOG_DBG("Downlink cleanup");

    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (node->downlink_sender)
    {
        pouch_gatt_sender_destroy(node->downlink_sender);
        node->downlink_sender = NULL;
    }
}
