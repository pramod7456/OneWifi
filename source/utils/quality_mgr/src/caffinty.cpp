
/**
 * Copyright 2023 Comcast Cable Communications Management, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "linkq.h"
#include <sys/time.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include "wifi_util.h"
#include "wifi_events.h"
#include "caffinity.h"

int caffinity_t::init(stats_arg_t *stats)
{
    if (!stats) {
        wifi_util_error_print(WIFI_CTRL, "caffinity %s:%d NULL stats pointer\n", __func__, __LINE__);
        return -1;
    }

    wifi_util_info_print(WIFI_CTRL, "caffinity %s:%d Updating SNR for MAC %s, cli_SNR=%d, channel_utilization=%d\n",
        __func__, __LINE__, stats->mac_str, stats->dev.cli_SNR, stats->channel_utilization);

    pthread_mutex_lock(&m_lock);
    m_cli_snr = stats->dev.cli_SNR;
    m_channel_utilization = stats->channel_utilization;
    
    // Update m_connected status based on cli_Active and cli_AuthenticationState
    bool client_active = (stats->dev.cli_Active && stats->dev.cli_AuthenticationState);
    
    if (client_active != m_connected) {
        m_connected = client_active;
        wifi_util_info_print(WIFI_CTRL, "caffinity %s:%d Connection status changed for MAC %s: was=%d now=%d (cli_Active=%d cli_AuthenticationState=%d)\n",
            __func__, __LINE__, stats->mac_str, !m_connected, m_connected,
            stats->dev.cli_Active, stats->dev.cli_AuthenticationState);
    }
    
    pthread_mutex_unlock(&m_lock);

    wifi_util_info_print(WIFI_CTRL, "caffinity %s:%d Updated stats for MAC %s with SNR=%d, channel_util=%d, m_connected=%d\n",
        __func__, __LINE__, stats->mac_str, m_cli_snr, m_channel_utilization, m_connected);

    return 0;  // Success
}

int caffinity_t::periodic_stats_update(stats_arg_t *stats)
{
    if (!stats) {
        wifi_util_error_print(WIFI_CTRL, "caffinity %s:%d NULL stats pointer\n", __func__, __LINE__);
        return -1;
    }

    wifi_util_info_print(WIFI_CTRL, "caffinity %s:%d Periodic stats update for MAC %s\n",
        __func__, __LINE__, stats->mac_str);

    pthread_mutex_lock(&m_lock);

    // Update m_connected_time from total_connected_time
    m_connected_time = stats->total_connected_time;

    // Update m_disconnected_time from total_disconnected_time
    m_disconnected_time = stats->total_disconnected_time;

    // Update cli_SNR
    m_cli_snr = stats->dev.cli_SNR;

    pthread_mutex_unlock(&m_lock);

    wifi_util_error_print(WIFI_CTRL, "timestats caffinity %s:%d Updated periodic stats for MAC %s: "
        "connected_time=%ld.%09ld disconnected_time=%ld.%09ld cli_SNR=%d\n",
        __func__, __LINE__, stats->mac_str,
        (long)m_connected_time.tv_sec, m_connected_time.tv_nsec,
        (long)m_disconnected_time.tv_sec, m_disconnected_time.tv_nsec,
        m_cli_snr);

    return 0;  // Success
}

int caffinity_t::update_affinity_stats(affinity_arg_t *arg)
{
    wifi_util_info_print(WIFI_CTRL, "caffinity CAFF %s:%d event=%d dhcp_event=%d\n", __func__, __LINE__, arg->event, arg->dhcp_event);
    
    pthread_mutex_lock(&m_lock);
    
    // Store RSSI from frame for use with unconnected clients
    m_rssi = arg->sig_dbm;
    
    // Handle DHCP event - just update attempts and failures directly
    if (arg->dhcp_event == DHCP_EVENT_UPDATE) {
        m_dhcp_attempts = arg->dhcp_attempts;
        m_dhcp_failures = arg->dhcp_failures;
        wifi_util_info_print(WIFI_CTRL, "caffinity CAFF %s:%d DHCP stats updated: attempts=%u failures=%u\n",
            __func__, __LINE__, m_dhcp_attempts, m_dhcp_failures);
        pthread_mutex_unlock(&m_lock);
        return 0;
    }
    
    // Handle WiFi events (auth, assoc, etc.)
    switch(arg->event)
    {
        case wifi_event_hal_auth_frame:
            m_auth_attempts++;
            wifi_util_info_print(WIFI_CTRL, "caffinity CAFF %s:%d AUTH attempt, total=%u\n", 
                __func__, __LINE__, m_auth_attempts);
            break;

        case wifi_event_hal_deauth_frame:
            m_auth_failures++;
            m_disconnected_time = arg->disconnected_time;
            wifi_util_info_print(WIFI_CTRL, "caffinity CAFF %s:%d DEAUTH failure, total=%u, disconnected_time=%ld.%09ld\n",
                __func__, __LINE__, m_auth_failures,
                (long)m_disconnected_time.tv_sec, m_disconnected_time.tv_nsec);
            break;

        case wifi_event_hal_assoc_req_frame:
        case wifi_event_hal_reassoc_req_frame:
            m_assoc_attempts++;
            wifi_util_info_print(WIFI_CTRL, "caffinity CAFF %s:%d ASSOC/REASSOC request, attempts=%u\n",
                __func__, __LINE__, m_assoc_attempts);
            break;

        case wifi_event_hal_assoc_rsp_frame:
        case wifi_event_hal_reassoc_rsp_frame:
            // Only increment failure if status_code is non-zero
            if (arg->status_code != 0) {
                m_assoc_failures++;
                m_connected = false;
                wifi_util_info_print(WIFI_CTRL, "caffinity CAFF %s:%d ASSOC/REASSOC response FAILED (status=%u), failures=%u, m_connected=false\n",
                    __func__, __LINE__, arg->status_code, m_assoc_failures);
            } else {
                m_connected = true;
                m_connected_time = arg->connected_time;
                wifi_util_info_print(WIFI_CTRL, "caffinity CAFF %s:%d ASSOC/REASSOC response SUCCESS (status=%u), m_connected=true, connected_time=%ld.%09ld\n",
                    __func__, __LINE__, arg->status_code,
                    (long)m_connected_time.tv_sec, m_connected_time.tv_nsec);
            }
            break;

        case wifi_event_hal_sta_conn_status:
            {
                wifi_util_info_print(WIFI_CTRL, "caffinity CAFF %s:%d wifi_event_hal_sta_conn_status for MAC %s\n",
                    __func__, __LINE__, arg->mac_str);
                // DHCP stats are now tracked via dhcp_msg_type field in update_affinity_stats
            }
            break;

        case wifi_event_hal_disassoc_device:
            m_connected = false;
            m_disconnected_time = arg->disconnected_time;
            wifi_util_info_print(WIFI_CTRL, "caffinity CAFF %s:%d DISASSOC device, m_connected=false, disconnected_time=%ld.%09ld\n",
                __func__, __LINE__, (long)m_disconnected_time.tv_sec, m_disconnected_time.tv_nsec);
            break;

        default:
            wifi_util_info_print(WIFI_CTRL, "caffinity CAFF %s:%d Unhandled event=%d\n",
                __func__, __LINE__, arg->event);
            break;
    }
    
    pthread_mutex_unlock(&m_lock);
    
    wifi_util_info_print(WIFI_CTRL, "caffinity CAFF %s:%d Updated stats for event=%d: auth_attempts=%u auth_failures=%u assoc_attempts=%u assoc_failures=%u\n",
        __func__, __LINE__, arg->event, m_auth_attempts, m_auth_failures, m_assoc_attempts, m_assoc_failures);
    
    return 0;
}

caffinity_result_t caffinity_t::run_algorithm_caffinity()
{
    caffinity_result_t result;
    double score = 0.0;
    
    // Initialize result
    strncpy(result.mac, m_mac, sizeof(result.mac) - 1);
    result.mac[sizeof(result.mac) - 1] = '\0';
    result.score = 0.0;
    result.connected = false;

    wifi_util_error_print(WIFI_CTRL, "caffinity %s:%d Computing caffinity score for MAC %s\n",
        __func__, __LINE__, m_mac);

    pthread_mutex_lock(&m_lock);
    
    result.connected = m_connected;
    
    // Debug dump of all stats for this MAC (one call per line to avoid logging truncation on embedded \n)
    wifi_util_error_print(WIFI_CTRL, "caffinity %s:%d [MAC=%s] Stats Dump:\n", __func__, __LINE__, m_mac);
    wifi_util_error_print(WIFI_CTRL, "caffinity   auth_attempts=%u auth_failures=%u assoc_attempts=%u\n",
        m_auth_attempts, m_auth_failures, m_assoc_attempts);
    wifi_util_error_print(WIFI_CTRL, "caffinity   assoc_failures=%u dhcp_attempts=%u dhcp_failures=%u\n",
        m_assoc_failures, m_dhcp_attempts, m_dhcp_failures);
    wifi_util_error_print(WIFI_CTRL, "caffinity   connected_time=%ld.%09ld disconnected_time=%ld.%09ld sleep_time=%ld.%09ld\n",
        (long)m_connected_time.tv_sec, (long)m_connected_time.tv_nsec,
        (long)m_disconnected_time.tv_sec, (long)m_disconnected_time.tv_nsec,
        (long)m_sleep_time.tv_sec, (long)m_sleep_time.tv_nsec);
    wifi_util_error_print(WIFI_CTRL, "caffinity   total_time=%ld.%09ld connected=%d\n",
        (long)m_total_time.tv_sec, (long)m_total_time.tv_nsec, m_connected);
    
    bool connected = m_connected;

    /* ------------------------------------------------------------------ */
    /* PATH A: Connected client                                             */
    /* score = connected_time / (connected_time + disconnected_time + sleep_time) */
    /* ------------------------------------------------------------------ */
    if (connected) {
        double connected_sec    = (double)m_connected_time.tv_sec
                                  + (double)m_connected_time.tv_nsec / 1e9;
        double disconnected_sec = (double)m_disconnected_time.tv_sec
                                  + (double)m_disconnected_time.tv_nsec / 1e9;
        double sleep_sec        = 0.0;  // reserved for future use, currently 0

        pthread_mutex_unlock(&m_lock);
        
        wifi_util_info_print(WIFI_CTRL,
            "SCORE params  %s:%d  sleep=%.4f"
            " (connected=%.3fs disconnected=%.3fs sleep=%.3fs)\n",
            __func__, __LINE__, 0.0, connected_sec, disconnected_sec, sleep_sec);
        double total = connected_sec + disconnected_sec + sleep_sec;

        if (total <= 0.0) {
            wifi_util_info_print(WIFI_CTRL,
                "caffinity %s:%d Connected client, total time is zero, returning score=0\n",
                __func__, __LINE__);
            return result;
        }

        score = connected_sec / total;

        // Clamp to [0, 1]
        if (score < 0.0) score = 0.0;
        if (score > 1.0) score = 1.0;

        result.score = score;
        return result;
    }

    /* ------------------------------------------------------------------ */
    /* PATH B: Unconnected client sigmoid logic                           */
    /* ------------------------------------------------------------------ */
    double failure_ratio    = 0.0;
    double auth_failure_rate  = 0.0;
    double assoc_failure_rate = 0.0;
    double dhcp_failure_rate  = 0.0;
    double snr_normalized   = 0.0;
    double snr_squared      = 0.0;
    double sigmoid_factor   = 0.0;
    double exponent         = 0.0;
    int    channel_utilization = 0;
    int    cli_snr = 0;

    // For unconnected clients, use RSSI instead of SNR (SNR is 0 for unassociated)
    // RSSI range: typically -90 dBm (bad) to -30 dBm (excellent)
    // Map to approximate SNR range 0-70: snr_equivalent = rssi + 90
    /*if (m_rssi != 0) {
        cli_snr = m_rssi + 90;  // e.g., -30 dBm -> 60, -90 dBm -> 0
        if (cli_snr < 0) cli_snr = 0;
        if (cli_snr > 70) cli_snr = 70;
        wifi_util_info_print(WIFI_CTRL,
            "caffinity %s:%d Unconnected client, using RSSI=%d dBm -> equivalent SNR=%d\n",
            __func__, __LINE__, m_rssi, cli_snr);
    }*/
    cli_snr = m_cli_snr;
    channel_utilization = m_channel_utilization;

    // Calculate failure rates with division-by-zero protection
    if (m_auth_attempts > 0) {
        auth_failure_rate = (double)m_auth_failures / (double)m_auth_attempts;
    }
    if (m_assoc_attempts > 0) {
        assoc_failure_rate = (double)m_assoc_failures / (double)m_assoc_attempts;
    }
    // DHCP failure rate is now included for unconnected clients as well
    if (m_dhcp_attempts > 0) {
        dhcp_failure_rate = (double)m_dhcp_failures / (double)m_dhcp_attempts;
    }

    pthread_mutex_unlock(&m_lock);

    wifi_util_info_print(WIFI_CTRL, "caffinity %s:%d cli_SNR=%d, channel_utilization=%d\n",
        __func__, __LINE__, cli_snr, channel_utilization);

    // Sum failure rates
    failure_ratio = auth_failure_rate + assoc_failure_rate + dhcp_failure_rate;

    // Clamp failure_ratio to [0, 1]
    if (failure_ratio < 0.0) failure_ratio = 0.0;
    if (failure_ratio > 1.0) failure_ratio = 1.0;

    wifi_util_info_print(WIFI_CTRL,
        "SCORE caffinity %s:%d failure_ratio=%.4f (auth=%.4f, assoc=%.4f, dhcp=%.4f)\n",
        __func__, __LINE__, failure_ratio, auth_failure_rate, assoc_failure_rate, dhcp_failure_rate);

    // Normalize SNR to [0, 1] range (max SNR assumed 70)
    if (cli_snr > 0) {
        snr_normalized = (double)cli_snr / 70.0;
    }

    // Clamp snr_normalized to [0, 1]
    if (snr_normalized < 0.0) snr_normalized = 0.0;
    if (snr_normalized > 1.0) snr_normalized = 1.0;

    // Square the normalized SNR
    snr_squared = snr_normalized * snr_normalized;

    wifi_util_info_print(WIFI_CTRL, "caffinity %s:%d snr_normalized=%.4f, snr_squared=%.4f\n",
        __func__, __LINE__, snr_normalized, snr_squared);

    // Compute sigmoid factor: 1 / (1 + exp(-(b0 + b1 * channel_utilization)))
    exponent = -(LINK_QTY_B0 + LINK_QTY_B1 * channel_utilization);

    // Clamp exponent to safe range for numerical stability
    if (exponent < -50.0) exponent = -50.0;
    if (exponent > 50.0) exponent = 50.0;

    sigmoid_factor = 1.0 / (1.0 + exp(exponent));

    wifi_util_info_print(WIFI_CTRL, "caffinity %s:%d exponent=%.4f, sigmoid_factor=%.4f\n",
        __func__, __LINE__, exponent, sigmoid_factor);

    // Calculate final score: (1 - failure_ratio) * snr_squared * sigmoid_factor
    score = (1.0 - failure_ratio) * snr_squared * sigmoid_factor;

    // Clamp final score to [0, 1]
    if (score < 0.0) score = 0.0;
    if (score > 1.0) score = 1.0;

    wifi_util_info_print(WIFI_CTRL, "caffinity %s:%d FINAL SCORE=%.4f for MAC %s connected %d\n",
        __func__, __LINE__, score, m_mac, m_connected);
    
    result.score = score + 0.1;
    return result;
}



caffinity_t::caffinity_t(mac_addr_str_t *mac)
{
    strncpy(m_mac, *mac, sizeof(m_mac) - 1);
    m_mac[sizeof(m_mac) - 1] = '\0';
    pthread_mutex_init(&m_lock, NULL);
    m_auth_failures = 0;
    m_auth_attempts = 0;
    m_assoc_failures = 0;
    m_assoc_attempts = 0;
    m_dhcp_failures = 0;
    m_dhcp_attempts = 0;
    m_snr_assoc = 0;
    m_cli_snr = 0;
    m_rssi = 0;
    m_channel_utilization = 0;
    memset(&m_disconnected_time, 0, sizeof(m_disconnected_time));
    memset(&m_connected_time, 0, sizeof(m_connected_time));
    memset(&m_sleep_time, 0, sizeof(m_sleep_time));
    memset(&m_total_time, 0, sizeof(m_total_time));
    m_connected =  false;

}

caffinity_t::~caffinity_t()
{
   pthread_mutex_destroy(&m_lock);
}
