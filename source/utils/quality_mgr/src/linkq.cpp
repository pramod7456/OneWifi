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
#include "wifi_util.h"

linkq_params_t linkq_t::m_linkq_params[MAX_LINKQ_PARAMS] = {{"SNR", true}, {"PER", false}, {"PHY", true}};

vector_t linkq_t::run_algorithm(linkq_data_t data, bool &alarm, bool update_alarm)
{
    vector_t v;
    unsigned int i = 0;
    double x, y;
    char mac_addr[1024] = {0};
    alarm = false; 
    v.m_num = MAX_LINKQ_PARAMS + 1;
    v.m_val[v.m_num - 1].m_re = 0.0; 
    strncpy(mac_addr, m_mac, sizeof(mac_addr) - 1);
    mac_addr[sizeof(mac_addr) - 1] = '\0'; 
    for (i = 0; i < MAX_LINKQ_PARAMS; i++) {
        m_seq[i] = m_seq[i] + number_t((float)data[i], 0);
        y = m_seq[i].get_max().m_re - m_seq[i].get_min().m_re;
        //wifi_util_dbg_print(WIFI_APPS,"max=%f min=%f\n", m_seq[i].get_max().m_re,m_seq[i].get_min().m_re,m_seq[i].get_min().m_re);
        if (y != 0) {
            x = data[i] - m_seq[i].get_min().m_re;
            v.m_val[i].m_re = x/y;
	    if (v.m_val[i].m_re > 1)
                v.m_val[i].m_re = 1;
        } else {
            v.m_val[i].m_re = 0;
        }
        //wifi_util_dbg_print(WIFI_APPS,"%s:%d v.m_val[i].m_re =%f\n",__func__,__LINE__,v.m_val[i].m_re); 
 
        if (m_linkq_params[i].booster == false) {
            v.m_val[v.m_num - 1].m_re -= pow(v.m_val[i].m_re, 2);
        } else {
            v.m_val[v.m_num - 1].m_re += pow(v.m_val[i].m_re, 2);
        }
    wifi_util_dbg_print(WIFI_APPS,"%s:%d v.m_val[v.m_val[i].m_re =%f: i=%d mac=%s\n",__func__,__LINE__,v.m_val[v.m_num -1].m_re,i,mac_addr); 
    }
    if (v.m_val[v.m_num - 1].m_re < 0){
        v.m_val[v.m_num - 1].m_re = 0;
    } else {
        v.m_val[v.m_num - 1].m_re = sqrt(v.m_val[v.m_num - 1].m_re/MAX_LINKQ_PARAMS);
    }
    m_sampled++;
    wifi_util_dbg_print(WIFI_APPS,"%s:%d score =%f m_sampled =%d mac=%s\n",__func__,__LINE__,v.m_val[v.m_num -1].m_re,m_sampled,mac_addr); 
    
    // has threshold crossed 80% in the last reporting multiplicity
    if (v.m_val[v.m_num - 1].m_re < m_threshold) {
        m_threshold_cross_counter++;
    }
    
    if (update_alarm) {
        alarm = (m_threshold_cross_counter >= ceil(0.8 * m_sampled)) ? true:false;
        m_sampled = 0;
        m_threshold_cross_counter = 0;
    }
    return v;
}

vector_t linkq_t::run_test(bool &alarm, bool update_alarm)
{
    vector_t v;
    linkq_data_t data;
    pthread_mutex_lock(&m_vec_lock);
    alarm = false;
    // Bounds check
    if (m_stats_arr.empty() || m_current >= m_recs) {
        pthread_mutex_unlock(&m_vec_lock);
        wifi_util_error_print(
            WIFI_APPS,
            "%s:%d: Failed to load record \n",
            __func__, __LINE__
        );
        return vector_t(0);
    }
    const stats_arg_t stat = m_stats_arr[0];   // COPY, not reference
    pthread_mutex_unlock(&m_vec_lock);


    for (unsigned int i = 0; i < MAX_LINKQ_PARAMS; i++) {
        if (strcmp(m_linkq_params[i].name, "SNR") == 0) {
            data[i] = stat.snr;
        } else if (strcmp(m_linkq_params[i].name, "PER") == 0) {
            data[i] = stat.per;
        } else if (strcmp(m_linkq_params[i].name, "PHY") == 0) {
            data[i] = stat.phy;
        }
    }

    v = run_algorithm(data, alarm, update_alarm);
    m_current++;
    return v;
}

int linkq_t::init(double threshold, unsigned int reporting_mult, stats_arg_t *stats )//, const char *test_file_name)
{
    char *buff, tmp[MAX_LINE_SIZE];
    unsigned int i;
    
    m_threshold = threshold;
    m_reporting_mult = reporting_mult;
    m_max_phy = stats->max_phy;
    mac_address_t sta_mac;
    
    for (i = 0; i < MAX_LINKQ_PARAMS; i++) {
        if (strncmp(m_linkq_params[i].name, "SNR", strlen("SNR")) == 0) {
            m_seq[i].set_max(number_t(70, 0));
            m_seq[i].set_min(number_t(0, 0));
        } else if (strncmp(m_linkq_params[i].name, "PER", strlen("PER")) == 0) {
            m_seq[i].set_max(number_t(25, 0));
            m_seq[i].set_min(number_t(0, 0));
        } else if (strncmp(m_linkq_params[i].name, "PHY", strlen("PHY")) == 0) {
            m_seq[i].set_max(number_t(m_max_phy, 0));
            m_seq[i].set_min(number_t(0, 0));
        }
    }
    pthread_mutex_lock(&m_vec_lock);
    if (m_stats_arr.empty()) {
        m_stats_arr.push_back(*stats);
    } else {
        m_stats_arr[0] = *stats;   // overwrite atomically
    }
    m_recs = 1;
    m_current = 0;
    pthread_mutex_unlock(&m_vec_lock);
    wifi_util_error_print(WIFI_APPS," %s:%d m_recs =%d m_current=%d m_max_phy=%d\n",__func__,__LINE__,m_recs,m_current,m_max_phy); 
    return 0;
}

linkq_params_t *linkq_t::get_linkq_params()
{
    return m_linkq_params;
}

linkq_t::linkq_t(mac_addr_str_t mac)
{
    strncpy(m_mac, mac, sizeof(m_mac) - 1);
    m_mac[sizeof(m_mac) - 1] = '\0';
    pthread_mutex_init(&m_vec_lock, NULL);
    m_recs = 0;
    m_current = 0;
    m_max_phy = 0;
    m_threshold_cross_counter = 0;
    m_sampled = 0;
}

linkq_t::~linkq_t()
{
     m_stats_arr.clear();

}

