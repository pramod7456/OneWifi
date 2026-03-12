
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

int caffinity_t::update_affinity_stats(affinity_arg_t *arg)
{
    switch(arg->event)
    {
        case wifi_event_hal_auth_frame:
            m_auth_attempts++;
            break;

        case wifi_event_hal_deauth_frame:
            m_auth_failures++;
            break;

        case wifi_event_hal_assoc_req_frame:
        case wifi_event_hal_reassoc_req_frame:
            m_assoc_attempts++;
            break;

        case wifi_event_hal_assoc_rsp_frame:
        case wifi_event_hal_reassoc_rsp_frame:
            m_assoc_failures++;
            break;

        case wifi_event_hal_sta_conn_status:
            break;

        case wifi_event_hal_disassoc_device:
            break;

        default:
            break;
    }
}

int caffinity_t::score()
{
    int score = 0;
    wifi_util_info_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return score;
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
