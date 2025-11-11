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

#include <pouch_gateway/bt/cert.h>
#include <pouch_gateway/bt/connect.h>
#include <pouch_gateway/bt/uplink.h>

#include <pouch_gateway/cert.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(cert_gatt);

static void gateway_server_cert_write_start(struct bt_conn *conn);
static void gateway_device_cert_read_start(struct bt_conn *conn);

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
            gateway_device_cert_read_start(conn);
        }
        else
        {
            // There was certificate update in the meantime, so send it once again.
            LOG_INF("Noticed certificate update, sending once again");
            node->server_cert_provisioned = false;
            gateway_server_cert_write_start(conn);
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

static void gateway_server_cert_write_start(struct bt_conn *conn)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (node->server_cert_provisioned)
    {
        gateway_device_cert_read_start(conn);
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

static void gateway_device_cert_read_start(struct bt_conn *conn)
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

void pouch_gateway_cert_exchange_start(struct bt_conn *conn)
{
    gateway_server_cert_write_start(conn);
}
