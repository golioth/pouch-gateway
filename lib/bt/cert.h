/*
 * Copyright (c) 2025 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

struct bt_conn;

/**
 * Start certificate exchange for the given Bluetooth connection.
 *
 * @param conn The Bluetooth connection.
 */
void pouch_gateway_cert_exchange_start(struct bt_conn *conn);
