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
#include <pouch/transport/gatt/common/receiver.h>
#include <pouch/transport/gatt/common/sender.h>

#include <pouch_gateway/bt/cert.h>
#include <pouch_gateway/bt/connect.h>
#include <pouch_gateway/bt/uplink.h>

#include <pouch_gateway/cert.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(cert_gatt, CONFIG_POUCH_GATEWAY_GATT_LOG_LEVEL);

static void gateway_server_cert_write_start(struct bt_conn *conn);
static void gateway_device_cert_read_start(struct bt_conn *conn);

static void server_cert_cleanup(struct bt_conn *conn)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

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

    if (node->device_cert_receiver)
    {
        pouch_gatt_receiver_destroy(node->device_cert_receiver);
        node->device_cert_receiver = NULL;
    }
}

static int write_server_cert_characteristic(void *conn, const void *data, size_t length)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);
    uint16_t server_cert_handle = node->attr_handles[POUCH_GATEWAY_GATT_ATTR_SERVER_CERT].value;

    int err = bt_gatt_write_without_response(conn, server_cert_handle, data, length, false);
    if (err)
    {
        server_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
    }

    return err;
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

static uint8_t server_cert_notify_cb(struct bt_conn *conn,
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
    int err = pouch_gatt_sender_receive_ack(node->server_cert_sender, data, length, &complete);
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

        server_cert_cleanup(conn);
        bt_gatt_unsubscribe(conn, params);
        pouch_gateway_bt_finished(conn);

        return BT_GATT_ITER_STOP;
    }

    if (complete)
    {
        LOG_DBG("Server cert complete");

        bool is_newest = pouch_gateway_server_cert_is_newest(node->server_cert_ctx);

        server_cert_cleanup(conn);

        if (is_newest)
        {
            LOG_DBG("Ending server cert");
            gateway_device_cert_read_start(conn);
        }
        else
        {
            // There was certificate update in the meantime, so send it once again.
            LOG_INF("Noticed certificate update, sending once again");
            gateway_server_cert_write_start(conn);
        }

        return BT_GATT_ITER_STOP;
    }

    return BT_GATT_ITER_CONTINUE;
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

    size_t mtu = bt_gatt_get_mtu(conn) - POUCH_GATEWAY_BT_ATT_OVERHEAD;

    node->server_cert_sender =
        pouch_gatt_sender_create(node->packetizer, write_server_cert_characteristic, conn, mtu);
    if (NULL == node->server_cert_sender)
    {
        LOG_ERR("Failed to create sender");
        server_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return;
    }

    struct bt_gatt_subscribe_params *subscribe_params = &node->server_cert_subscribe_params;
    if (NULL == subscribe_params)
    {
        LOG_ERR("Could not subscribe to server cert characteristic");
        server_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return;
    }

    memset(subscribe_params, 0, sizeof(*subscribe_params));
    subscribe_params->notify = server_cert_notify_cb;
    subscribe_params->value = BT_GATT_CCC_NOTIFY;
    subscribe_params->value_handle = node->attr_handles[POUCH_GATEWAY_GATT_ATTR_SERVER_CERT].value;
    subscribe_params->ccc_handle = node->attr_handles[POUCH_GATEWAY_GATT_ATTR_SERVER_CERT].ccc;
    int err = bt_gatt_subscribe(conn, subscribe_params);
    if (err)
    {
        LOG_ERR("Could not subscribe to server cert characteristic");
        server_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
    }
}

static int send_ack_cb(void *conn, const void *data, size_t length)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);
    uint16_t handle = node->attr_handles[POUCH_GATEWAY_GATT_ATTR_DEVICE_CERT].value;

    return bt_gatt_write_without_response(conn, handle, data, length, false);
}

static int device_cert_data_received_cb(void *conn,
                                        const void *data,
                                        size_t length,
                                        bool is_first,
                                        bool is_last)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (data)
    {
        LOG_HEXDUMP_DBG(data, length, "[READ] BLE GATT device cert");
    }

    int err = pouch_gateway_device_cert_push(node->device_cert_ctx, data, length);
    if (err)
    {
        LOG_ERR("Failed to push device cert");
        goto finish;
    }

    if (is_last)
    {
        err = pouch_gateway_device_cert_finish(node->device_cert_ctx);
        if (err)
        {
            LOG_ERR("Failed to finish device cert: %d", err);
            goto finish;
        }
        node->device_cert_ctx = NULL;
    }

finish:
    if (err)
    {
        device_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        ;
    }

    return err;
}

static uint8_t device_cert_notify_cb(struct bt_conn *conn,
                                     struct bt_gatt_subscribe_params *params,
                                     const void *data,
                                     uint16_t length)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (NULL == data)
    {
        LOG_DBG("Subscription terminated");
        device_cert_cleanup(conn);

        return BT_GATT_ITER_STOP;
    }

    enum pouch_gatt_ack_code code;
    if (pouch_gatt_packetizer_is_fin(data, length, &code))
    {
        LOG_DBG("Received end from node (%d)", code);

        if (node->device_cert_ctx)
        {
            LOG_WRN("Node ended device cert prematurely");

            device_cert_cleanup(conn);

            pouch_gateway_bt_finished(conn);
        }
        else
        {
            device_cert_cleanup(conn);

            pouch_gateway_uplink_start(conn);
        }

        return BT_GATT_ITER_STOP;
    }

    int err = pouch_gatt_receiver_receive_data(node->device_cert_receiver, data, length);
    if (err)
    {
        LOG_ERR("Error receiving data: %d", err);
        device_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);

        return BT_GATT_ITER_STOP;
    }

    return BT_GATT_ITER_CONTINUE;
}

static void gateway_device_cert_read_start(struct bt_conn *conn)
{
    struct pouch_gateway_node_info *node = pouch_gateway_get_node_info(conn);

    if (node->device_cert_provisioned)
    {
        pouch_gateway_uplink_start(conn);
        return;
    }

    if (0 == node->attr_handles[POUCH_GATEWAY_GATT_ATTR_DEVICE_CERT].ccc)
    {
        LOG_ERR("Did not discover Device Cert CCC");
        pouch_gateway_bt_finished(conn);
        return;
    }

    node->device_cert_ctx = pouch_gateway_device_cert_start();
    if (node->device_cert_ctx == NULL)
    {
        LOG_ERR("Failed to allocate device cert context");
        device_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return;
    }

    node->device_cert_receiver =
        pouch_gatt_receiver_create(send_ack_cb,
                                   conn,
                                   device_cert_data_received_cb,
                                   conn,
                                   CONFIG_POUCH_GATT_DEVICE_CERT_WINDOW_SIZE);
    if (NULL == node->device_cert_receiver)
    {
        LOG_ERR("Failed to create receiver");
        device_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return;
    }

    struct bt_gatt_subscribe_params *subscribe_params = &node->device_cert_subscribe_params;
    if (NULL == subscribe_params)
    {
        LOG_ERR("Could not subscribe to device cert characteristic");
        device_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
        return;
    }
    memset(subscribe_params, 0, sizeof(*subscribe_params));

    subscribe_params->notify = device_cert_notify_cb;
    subscribe_params->value = BT_GATT_CCC_NOTIFY;
    subscribe_params->value_handle = node->attr_handles[POUCH_GATEWAY_GATT_ATTR_DEVICE_CERT].value;
    subscribe_params->ccc_handle = node->attr_handles[POUCH_GATEWAY_GATT_ATTR_DEVICE_CERT].ccc;
    atomic_set_bit(subscribe_params->flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
    int err = bt_gatt_subscribe(conn, subscribe_params);
    if (err)
    {
        LOG_ERR("BT subscribe request failed: %d", err);
        device_cert_cleanup(conn);
        pouch_gateway_bt_finished(conn);
    }
}

void pouch_gateway_cert_exchange_start(struct bt_conn *conn)
{
    gateway_server_cert_write_start(conn);
}
