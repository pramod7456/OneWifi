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

#ifndef LINKQ_H
#define LINKQ_H

#include "vector.h"
#include "sequence.h"
#include <cjson/cJSON.h>
#include "wifi_hal.h"
#include "run_qmgr.h"
#include <vector>

#define MAX_LINE_SIZE   1024
#define MAX_LINKQ_PARAMS    3

typedef struct {
    const char *name;
    bool booster;
} linkq_params_t;

typedef float linkq_data_t[MAX_LINKQ_PARAMS];

class linkq_t {
    mac_addr_str_t m_mac; 
    sequence_t m_seq[MAX_LINKQ_PARAMS];
    pthread_mutex_t m_vec_lock; 
    // test parameters
    //cJSON *m_test_obj;
    unsigned int m_recs;
    unsigned int m_current;
    unsigned int m_max_phy;
    double m_threshold;
    unsigned int m_reporting_mult;
    unsigned int m_threshold_cross_counter;
    unsigned int m_sampled;
    std::vector<stats_arg_t> m_stats_arr; 
    static linkq_params_t m_linkq_params[MAX_LINKQ_PARAMS];
public:
    vector_t run_test(bool &alarm,bool update_alarm);
    vector_t run_algorithm(linkq_data_t data, bool &alarm, bool update_alarm);
    int init(double threshold, unsigned int reporting_mult,stats_arg_t *stats);//const char *test_file_name = NULL);
    static linkq_params_t *get_linkq_params();
    const char * get_mac_addr() const{ return m_mac; }
    linkq_t(mac_addr_str_t mac);
    ~linkq_t();
};

#endif
