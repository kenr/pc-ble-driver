/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 *
 *   3. Neither the name of Nordic Semiconductor ASA nor the names of other
 *   contributors to this software may be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 *   4. This software must only be used in or with a processor manufactured by Nordic
 *   Semiconductor ASA, or in or with a processor manufactured by a third party that
 *   is used in combination with a processor manufactured by Nordic Semiconductor.
 *
 *   5. Any software provided in binary or object form under this license must not be
 *   reverse engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// Test framework
#include "catch2/catch.hpp"

// Logging support
#include <internal/log.h>

// Test support
#include <test_setup.h>
#include <test_util.h>

#include <ble.h>
#include <sd_rpc.h>

#include <sstream>
#include <string>
#include <thread>

TEST_CASE(CREATE_TEST_NAME_AND_TAGS(rssi, [PCA10028][PCA10031][PCA10040][PCA10056][PCA10059]))
{
    // Indicates if an error has occurred in a callback.
    // The test framework is not thread safe so this variable is used to communicate that an issues
    // has occurred in a callback.
    auto centralError    = false;
    auto peripheralError = false;

    // Set to true when the test is complete
    auto testCompleteCentral    = false;
    auto testCompletePeripheral = false;

    auto env = ::test::getEnvironment();
    INFO(::test::getEnvironmentAsText(env));
    REQUIRE(env.serialPorts.size() >= 2);
    const auto central    = env.serialPorts.at(0);
    const auto peripheral = env.serialPorts.at(1);

    auto centralRssiReportsCount    = 0;
    auto peripheralRssiReportsCount = 0;
    const auto maxRssiReportsWanted = 100;
    const auto rssiThreshold        = 1;
    const auto rssiSkipCount        = 1;

    auto periperalRssiStop = false;
    auto centralRssiStop   = false;

    const auto advertisementNameLength = 20;
    std::vector<uint8_t> peripheralAdvNameBuffer(advertisementNameLength);
    testutil::appendRandomAlphaNumeric(peripheralAdvNameBuffer, advertisementNameLength);
    const std::string peripheralAdvName(peripheralAdvNameBuffer.begin(),
                                        peripheralAdvNameBuffer.end());
    std::vector<uint8_t> advResponse;
    testutil::appendAdvertisingName(advResponse, peripheralAdvName);

    // Instantiate an adapter to use as BLE Central in the test
    auto c = std::make_shared<testutil::AdapterWrapper>(
        testutil::Central, central.port, env.baudRate, env.mtu, env.retransmissionInterval,
        env.responseTimeout);

    // Instantiated an adapter to use as BLE Peripheral in the test
    auto p = std::make_shared<testutil::AdapterWrapper>(
        testutil::Peripheral, peripheral.port, env.baudRate, env.mtu, env.retransmissionInterval,
        env.responseTimeout);

    REQUIRE(sd_rpc_log_handler_severity_filter_set(c->unwrap(), env.driverLogLevel) == NRF_SUCCESS);
    REQUIRE(sd_rpc_log_handler_severity_filter_set(p->unwrap(), env.driverLogLevel) == NRF_SUCCESS);

    c->setGapEventCallback([&](const uint16_t eventId, const ble_gap_evt_t *gapEvent) -> bool {
        switch (eventId)
        {
            case BLE_GAP_EVT_CONNECTED:
            {
                const auto err_code = sd_ble_gap_rssi_start(c->unwrap(), gapEvent->conn_handle,
                                                            rssiThreshold, rssiSkipCount);

                if (err_code != NRF_SUCCESS)
                {
                    NRF_LOG(c->role()
                            << "BLE_GAP_EVT_CONNECTED: error calling sd_ble_gap_rssi_start"
                            << ", " << testutil::errorToString(err_code));

                    centralError = true;
                }
            }
                return true;
            case BLE_GAP_EVT_DISCONNECTED:
                return true;
            case BLE_GAP_EVT_ADV_REPORT:
                if (testutil::findAdvName(gapEvent->params.adv_report, peripheralAdvName))
                {
                    if (!c->scratchpad.connection_in_progress)
                    {
                        const auto err_code = c->connect(&(gapEvent->params.adv_report.peer_addr));

                        if (err_code != NRF_SUCCESS)
                        {
                            NRF_LOG(c->role()
                                    << " Error connecting to "
                                    << testutil::asText(gapEvent->params.adv_report.peer_addr)
                                    << ", " << testutil::errorToString(err_code));
                            centralError = true;
                        }
                    }
                }
                else
                {
                    c->startScan(true);
                }
                return true;
            case BLE_GAP_EVT_TIMEOUT:
                if (gapEvent->params.timeout.src == BLE_GAP_TIMEOUT_SRC_SCAN)
                {
                    const auto err_code = c->startScan();

                    if (err_code != NRF_SUCCESS)
                    {
                        NRF_LOG(c->role() << " Scan start error, err_code " << err_code);
                        centralError = true;
                    }
                }
                return true;
            case BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST:
            {
                const auto err_code = sd_ble_gap_conn_param_update(
                    c->unwrap(), c->scratchpad.connection_handle,
                    &(gapEvent->params.conn_param_update_request.conn_params));

                if (err_code != NRF_SUCCESS)
                {
                    NRF_LOG(c->role() << " Conn params update failed, err_code " << err_code);
                    centralError = true;
                }
            }
                return true;

            case BLE_GAP_EVT_RSSI_CHANGED:
            {
                centralRssiReportsCount++;

                if (centralRssiReportsCount >= maxRssiReportsWanted && !centralRssiStop)
                {
                    centralRssiStop = true;

                    int8_t lastRssi;

#if NRF_SD_BLE_API >= 6
                    uint8_t channelIndex;
                    auto err_code = sd_ble_gap_rssi_get(c->unwrap(), gapEvent->conn_handle,
                                                        &lastRssi, &channelIndex);
#else
                    auto err_code =
                        sd_ble_gap_rssi_get(c->unwrap(), gapEvent->conn_handle, &lastRssi);
#endif

                    if (err_code != NRF_SUCCESS)
                    {
                        NRF_LOG(c->role() << "BLE_GAP_EVT_RSSI_CHANGED: sd_ble_gap_rssi_get"
                                          << ", " << testutil::errorToString(err_code));

                        centralError = true;
                        return true;
                    }

                    NRF_LOG(c->role()
                            << "BLE_GAP_EVT_RSSI_CHANGED: RSSI from sd_ble_api_rssi_get is "
                            << static_cast<int32_t>(lastRssi));

#if NRF_SD_BLE_API >= 6
                    NRF_LOG(
                        c->role()
                        << "BLE_GAP_EVT_RSSI_CHANGED: channel index from sd_ble_api_rssi_get is "
                        << static_cast<uint32_t>(channelIndex));
#endif

                    err_code = sd_ble_gap_rssi_stop(c->unwrap(), gapEvent->conn_handle);

                    if (err_code != NRF_SUCCESS)
                    {
                        NRF_LOG(c->role() << "BLE_GAP_EVT_RSSI_CHANGED: sd_ble_gap_rssi_stop"
                                          << ", " << testutil::errorToString(err_code));

                        centralError = true;
                        return true;
                    }

                    testCompleteCentral = true;
                }
            }

                return true;
            default:
                return false;
        }
    });

    p->setGapEventCallback([&](const uint16_t eventId, const ble_gap_evt_t *gapEvent) {
        switch (eventId)
        {
            case BLE_GAP_EVT_CONNECTED:
            {
                const auto err_code = sd_ble_gap_rssi_start(p->unwrap(), gapEvent->conn_handle,
                                                            0, // threshold_dbm
                                                            1  // skip_count
                );

                if (err_code != NRF_SUCCESS)
                {
                    NRF_LOG(p->role()
                            << "BLE_GAP_EVT_CONNECTED: error calling sd_ble_gap_rssi_start"
                            << ", " << testutil::errorToString(err_code));

                    peripheralError = true;
                    return true;
                }
            }
                return true;
            case BLE_GAP_EVT_DISCONNECTED:
            {
                // Use scratchpad defaults when advertising
                NRF_LOG(p->role() << " Starting advertising.");
                if (p->startAdvertising() != NRF_SUCCESS)
                {
                    peripheralError = true;
                    return true;
                }
            }
            case BLE_GAP_EVT_RSSI_CHANGED:
            {
                peripheralRssiReportsCount++;

                if (peripheralRssiReportsCount >= maxRssiReportsWanted && !periperalRssiStop)
                {
                    periperalRssiStop = true;

                    int8_t lastRssi;

#if NRF_SD_BLE_API >= 6
                    uint8_t channelIndex;
                    auto err_code = sd_ble_gap_rssi_get(p->unwrap(), gapEvent->conn_handle,
                                                        &lastRssi, &channelIndex);
#else
                    auto err_code =
                        sd_ble_gap_rssi_get(c->unwrap(), gapEvent->conn_handle, &lastRssi);
#endif

                    if (err_code != NRF_SUCCESS)
                    {
                        NRF_LOG(p->role() << "BLE_GAP_EVT_RSSI_CHANGED: sd_ble_gap_rssi_get"
                                          << ", " << testutil::errorToString(err_code));

                        peripheralError = true;
                        return true;
                    }

                    NRF_LOG(p->role()
                            << "BLE_GAP_EVT_RSSI_CHANGED: RSSI from sd_ble_api_rssi_get is "
                            << static_cast<int32_t>(lastRssi));

#if NRF_SD_BLE_API >= 6
                    NRF_LOG(
                        p->role()
                        << "BLE_GAP_EVT_RSSI_CHANGED: channel index from sd_ble_api_rssi_get is "
                        << static_cast<uint32_t>(channelIndex));
#endif

                    err_code = sd_ble_gap_rssi_stop(p->unwrap(), gapEvent->conn_handle);

                    if (err_code != NRF_SUCCESS)
                    {
                        NRF_LOG(p->role() << "BLE_GAP_EVT_RSSI_CHANGED: sd_ble_gap_rssi_stop"
                                          << ", " << testutil::errorToString(err_code));

                        peripheralError = true;
                        return true;
                    }

                    testCompletePeripheral = true;
                }
            }
                return true;
            default:
                return false;
        }
    });

    // Open the adapters
    REQUIRE(c->open() == NRF_SUCCESS);
    REQUIRE(p->open() == NRF_SUCCESS);

    REQUIRE(c->configure() == NRF_SUCCESS);
    REQUIRE(p->configure() == NRF_SUCCESS);

    REQUIRE(p->setupAdvertising(advResponse, // advertising data
                                {},          // scan response data
                                40,          // interval
                                0,           // duration
                                true         // connectable
                                ) == NRF_SUCCESS);
    REQUIRE(p->startAdvertising() == NRF_SUCCESS);

    REQUIRE(c->startScan() == NRF_SUCCESS);

    // Wait for the test to complete
    std::this_thread::sleep_for(std::chrono::seconds(5));

    REQUIRE(centralError == false);
    REQUIRE(peripheralError == false);

    REQUIRE(testCompletePeripheral == true);
    REQUIRE(testCompleteCentral == true);

    REQUIRE(c->close() == NRF_SUCCESS);
    sd_rpc_adapter_delete(c->unwrap());

    REQUIRE(p->close() == NRF_SUCCESS);
    sd_rpc_adapter_delete(p->unwrap());
}
