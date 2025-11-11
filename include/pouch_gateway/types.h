/*
 * Copyright (c) 2025 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <zephyr/bluetooth/gatt.h>

#define POUCH_GATEWAY_BT_ATT_OVERHEAD 3 /* opcode (1) + handle (2) */

enum pouch_gateway_gatt_attr
{
    POUCH_GATEWAY_GATT_ATTR_INFO,
    POUCH_GATEWAY_GATT_ATTR_DOWNLINK,
    POUCH_GATEWAY_GATT_ATTR_UPLINK,
    POUCH_GATEWAY_GATT_ATTR_SERVER_CERT,
    POUCH_GATEWAY_GATT_ATTR_DEVICE_CERT,

    POUCH_GATEWAY_GATT_ATTRS,
};

struct pouch_gateway_attr_handle
{
    uint16_t value;
    uint16_t ccc;
};

struct pouch_gateway_node_info
{
    struct pouch_gateway_attr_handle attr_handles[POUCH_GATEWAY_GATT_ATTRS];
    union
    {
        struct bt_gatt_discover_params discover_params;
        struct bt_gatt_read_params read_params;
        struct bt_gatt_subscribe_params subscribe_params;
        struct bt_gatt_write_params write_params;
    };
    struct pouch_gateway_downlink_context *downlink_ctx;
    void *downlink_scratch;
    void *server_cert_scratch;
    struct pouch_gatt_packetizer *packetizer;
    struct pouch_gateway_uplink *uplink;
    struct pouch_gateway_info_context *info_ctx;
    struct pouch_gateway_device_cert_context *device_cert_ctx;
    struct pouch_gateway_server_cert_context *server_cert_ctx;
    bool server_cert_provisioned;
    bool device_cert_provisioned;
};

/**
 * Allocate a buffer the size of the current MTU for GATT operations.
 * @param conn Bluetooth connection
 * @return Pointer to allocated buffer or NULL on failure
 */
static inline void *pouch_gateway_bt_gatt_mtu_malloc(struct bt_conn *conn)
{
    size_t mtu = bt_gatt_get_mtu(conn);
    if (mtu < POUCH_GATEWAY_BT_ATT_OVERHEAD)
    {
        return NULL;
    }

    return malloc(mtu - POUCH_GATEWAY_BT_ATT_OVERHEAD);
}
