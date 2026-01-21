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

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(server_cert_gatt, CONFIG_POUCH_GATEWAY_GATT_LOG_LEVEL);

static void write_response_cb(struct bt_conn *conn,
                              uint8_t err,
                              struct bt_gatt_write_params *params);

static void server_cert_cleanup(struct bt_conn *conn)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (node->server_cert_scratch)
    {
        free(node->server_cert_scratch);
        node->server_cert_scratch = NULL;
    }

    if (node->server_cert_ctx)
    {
        pouch_gateway_server_cert_abort(node->server_cert_ctx);
        node->server_cert_ctx = NULL;
    }

    if (node->packetizer)
    {
        pouch_gatt_packetizer_finish(node->packetizer);
        node->packetizer = NULL;
    }
}

static int write_server_cert_characteristic(struct bt_conn *conn)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);
    struct bt_gatt_write_params *params = &node->write_params;
    uint16_t server_cert_handle = node->attr_handles[POUCH_GATEWAY_GATT_ATTR_SERVER_CERT].value;

    size_t mtu = bt_gatt_get_mtu(conn);
    if (mtu < POUCH_GATEWAY_BT_ATT_OVERHEAD)
    {
        LOG_ERR("MTU too small: %d", mtu);
        return -EIO;
    }

    size_t len = mtu - POUCH_GATEWAY_BT_ATT_OVERHEAD;
    enum pouch_gatt_packetizer_result ret =
        pouch_gatt_packetizer_get(node->packetizer, node->server_cert_scratch, &len);

    if (POUCH_GATT_PACKETIZER_ERROR == ret)
    {
        ret = pouch_gatt_packetizer_error(node->packetizer);
        LOG_ERR("Error getting %s data %d", "server cert", ret);
        return ret;
    }

    params->func = write_response_cb;
    params->handle = server_cert_handle;
    params->offset = 0;
    params->data = node->server_cert_scratch;
    params->length = len;

    LOG_HEXDUMP_DBG(node->server_cert_scratch, params->length, "server_cert write");
    LOG_DBG("Writing %d bytes to handle %d", params->length, params->handle);

    int res = bt_gatt_write(conn, params);
    if (0 > res)
    {
        server_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
    }

    return 0;
}

static void write_response_cb(struct bt_conn *conn,
                              uint8_t err,
                              struct bt_gatt_write_params *params)
{
    LOG_DBG("Received write response: %d", err);

    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (pouch_gateway_server_cert_is_complete(node->server_cert_ctx))
    {
        bool is_newest = pouch_gateway_server_cert_is_newest(node->server_cert_ctx);

        server_cert_cleanup(conn);

        if (is_newest)
        {
            pouch_gateway_device_cert_read(conn);
        }
        else
        {
            // There was certificate update in the meantime, so send it once again.
            LOG_INF("Noticed certificate update, sending once again");
            node->server_cert_provisioned = false;
            pouch_gateway_server_cert_write(conn);
        }
    }
    else
    {
        int ret = write_server_cert_characteristic(conn);
        if (0 != ret)
        {
            server_cert_cleanup(conn);
            pouch_gateway_bt_finished(conn);
        }
    }
}

static enum pouch_gatt_packetizer_result server_cert_fill_cb(void *dst,
                                                             size_t *dst_len,
                                                             void *user_arg)
{
    bool last = false;

    int ret = pouch_gateway_server_cert_get_data(user_arg, dst, dst_len, &last);
    if (-EAGAIN == ret)
    {
        LOG_DBG("Awaiting additional %s data from cloud", "server cert");
        return POUCH_GATT_PACKETIZER_MORE_DATA;
    }
    if (ret < 0)
    {
        *dst_len = 0;
        return POUCH_GATT_PACKETIZER_ERROR;
    }

    return last ? POUCH_GATT_PACKETIZER_NO_MORE_DATA : POUCH_GATT_PACKETIZER_MORE_DATA;
}

void pouch_gateway_server_cert_write(struct bt_conn *conn)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (node->server_cert_provisioned)
    {
        pouch_gateway_device_cert_read(conn);
        return;
    }

    if (0 == node->attr_handles[POUCH_GATEWAY_GATT_ATTR_SERVER_CERT].value)
    {
        LOG_ERR("%s characteristic undiscovered", "server cert");
        server_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return;
    }

    node->server_cert_scratch = pouch_gateway_bt_gatt_mtu_malloc(conn);
    if (NULL == node->server_cert_scratch)
    {
        LOG_ERR("Could not allocate space for %s scratch buffer", "server cert");
        server_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return;
    }

    node->server_cert_ctx = pouch_gateway_server_cert_start();
    if (node->server_cert_ctx == NULL)
    {
        LOG_ERR("Failed to allocate server cert context");
        server_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return;
    }
    node->packetizer =
        pouch_gatt_packetizer_start_callback(server_cert_fill_cb, node->server_cert_ctx);
    if (node->packetizer == NULL)
    {
        LOG_ERR("Failed to start packetizer");
        server_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return;
    }

    write_server_cert_characteristic(conn);
}
