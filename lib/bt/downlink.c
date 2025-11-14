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

#include <pouch_gateway/bt/connect.h>
#include <pouch_gateway/types.h>
#include <pouch_gateway/downlink.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(downlink_gatt);

static void write_response_cb(struct bt_conn *conn,
                              uint8_t err,
                              struct bt_gatt_write_params *params);

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

static int write_downlink_characteristic(struct bt_conn *conn)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);
    struct bt_gatt_write_params *params = &node->write_params;
    uint16_t downlink_handle = node->attr_handles[POUCH_GATEWAY_GATT_ATTR_DOWNLINK].value;

    size_t mtu = bt_gatt_get_mtu(conn);
    if (mtu < POUCH_GATEWAY_BT_ATT_OVERHEAD)
    {
        LOG_ERR("MTU too small: %d", mtu);
        return -EIO;
    }

    size_t len = mtu - POUCH_GATEWAY_BT_ATT_OVERHEAD;
    enum pouch_gatt_packetizer_result ret =
        pouch_gatt_packetizer_get(node->packetizer, node->downlink_scratch, &len);

    if (POUCH_GATT_PACKETIZER_ERROR == ret)
    {
        ret = pouch_gatt_packetizer_error(node->packetizer);
        LOG_ERR("Error getting downlink data %d", ret);
        return ret;
    }

    if (POUCH_GATT_PACKETIZER_EMPTY_PAYLOAD == ret)
    {
        LOG_DBG("No downlink data available");
        return -ENODATA;
    }

    params->func = write_response_cb;
    params->handle = downlink_handle;
    params->offset = 0;
    params->data = node->downlink_scratch;
    params->length = len;

    LOG_DBG("Writing %d bytes to handle %d", params->length, params->handle);

    int res = bt_gatt_write(conn, params);
    if (0 > res)
    {
        LOG_ERR("GATT write error: %d", res);
    }

    return res;
}

static void write_response_cb(struct bt_conn *conn,
                              uint8_t err,
                              struct bt_gatt_write_params *params)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    LOG_DBG("Received write response: %d", err);
    if (err)
    {
        pouch_gateway_downlink_abort(node->downlink_ctx);
        pouch_gatt_packetizer_finish(node->packetizer);

        pouch_gateway_bt_finished(conn);
        return;
    }

    if (pouch_gateway_downlink_is_complete(node->downlink_ctx))
    {
        pouch_gateway_downlink_close(node->downlink_ctx);
        pouch_gatt_packetizer_finish(node->packetizer);

        pouch_gateway_bt_finished(conn);
    }
    else
    {
        int ret = write_downlink_characteristic(conn);
        if (0 != ret && -ENODATA != ret)
        {
            pouch_gateway_downlink_abort(node->downlink_ctx);
            pouch_gatt_packetizer_finish(node->packetizer);

            pouch_gateway_bt_finished(conn);
        }
    }
}

static void downlink_data_available(void *arg)
{
    struct bt_conn *conn = arg;

    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    int ret = write_downlink_characteristic(conn);
    if (0 != ret)
    {
        pouch_gateway_downlink_abort(node->downlink_ctx);
        pouch_gatt_packetizer_finish(node->packetizer);

        pouch_gateway_bt_finished(conn);
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

    node->downlink_scratch = pouch_gateway_bt_gatt_mtu_malloc(conn);
    if (NULL == node->downlink_scratch)
    {
        LOG_ERR("Could not allocate space for downlink scratch buffer");
        return NULL;
    }

    node->downlink_ctx = pouch_gateway_downlink_open(downlink_data_available, conn);
    node->packetizer =
        pouch_gatt_packetizer_start_callback(downlink_packet_fill_cb, node->downlink_ctx);

    return node->downlink_ctx;
}

void pouch_gateway_downlink_cleanup(struct bt_conn *conn)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (node->downlink_scratch)
    {
        free(node->downlink_scratch);
        node->downlink_scratch = NULL;
    }
}
