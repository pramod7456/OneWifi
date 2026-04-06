/************************************************************************************
  If not stated otherwise in this file or this component's LICENSE file the
  following copyright and licenses apply:

  Copyright 2018 RDK Management

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 **************************************************************************/
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/time.h>
#include <string.h>
#include "wifi_hal.h"
#include "wifi_util.h"
#include "wifi_linkquality_libs.h"
#include "run_qmgr.h"

//Here the stats has to be sent to GW using 1905.1 frame
static int periodic_caffinity_stats_update_default(stats_arg_t *stats) 
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}
//This function is not needed in extender this is specific to project Ignite
static void register_station_mac_default(const char *str) 
{ 
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
}
//This function is not needed in extender this is specific to the Gateway
static void unregister_station_mac_default(const char *str)
{ 
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
}
static int start_link_metrics_default()
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}
static int stop_link_metrics_default()
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}
//Here the stats has to be sent to GW using 1905.1 frame
static char* disconnect_link_stats_default(stats_arg_t *stats)
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}

//This function is not needed in extender this is specific to the Gateway
static int reinit_link_metrics_default(server_arg_t *arg)
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}
//Here the stats has to be sent to GW using 1905.1 frame
static int remove_link_stats_default(stats_arg_t *stats)
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}
//Here the stats has to be sent to GW using 1905.1 frame
static int add_stats_metrics_default(stats_arg_t *stats)
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}
//This function is not needed in extender this is specific to the Gateway
static char* get_link_metrics_default() 
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return NULL;
}
//This function is not needed in extender this is specific to the Gateway
static int set_quality_flags_default(quality_flags_t *flag)
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}
//This function is not needed in extender this is specific to the Gateway
static int get_quality_flags_default(quality_flags_t *flag)
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}

wifi_lq_descriptor_t* get_lq_descriptor()
{
    static bool initialized = false;
    static wifi_lq_descriptor_t desc;
    wifi_ctrl_t *ctrl = NULL ;

    if (!initialized) {
        ctrl = (wifi_ctrl_t *)get_wifictrl_obj();
        if (ctrl->network_mode == rdk_dev_mode_type_ext) {
            // Use Socket and send it to the GW
            wifi_util_error_print(WIFI_CTRL, "%s:%d\n", __func__, __LINE__);
            desc.periodic_caffinity_stats_update_fn = periodic_caffinity_stats_update_default;
            desc.register_station_mac_fn = register_station_mac_default;
            desc.unregister_station_mac_fn = unregister_station_mac_default;
            desc.start_link_metrics_fn = start_link_metrics_default;
            desc.stop_link_metrics_fn = stop_link_metrics_default;
            desc.disconnect_link_stats_fn = disconnect_link_stats_default;
            desc.reinit_link_metrics_fn = reinit_link_metrics_default;
            desc.remove_link_stats_fn = remove_link_stats_default;
            desc.add_stats_metrics_fn = add_stats_metrics_default;
            desc.get_link_metrics_fn = get_link_metrics_default;
            desc.set_quality_flags_fn = set_quality_flags_default;
            desc.get_quality_flags_fn = get_quality_flags_default;
        } else {
            // Use Library calls in EasyMesh,Ignite and GW mode
            wifi_util_error_print(WIFI_CTRL, "%s:%d\n", __func__, __LINE__);
            extern int periodic_caffinity_stats_update(stats_arg_t *stats);
            extern void register_station_mac(const char *str);
            extern void unregister_station_mac(const char *str);
            extern int start_link_metrics();
            extern int stop_link_metrics();
            extern int disconnect_link_stats(stats_arg_t *stats);
            extern int reinit_link_metrics(server_arg_t *arg);
            extern int remove_link_stats(stats_arg_t *stats);
            extern int add_stats_metrics(stats_arg_t *stats);
            extern char* get_link_metrics();
            extern int set_quality_flags(quality_flags_t *flag);
            extern int get_quality_flags(quality_flags_t *flag);

            desc.periodic_caffinity_stats_update_fn = periodic_caffinity_stats_update;
            desc.register_station_mac_fn = register_station_mac;
            desc.unregister_station_mac_fn = unregister_station_mac;
            desc.start_link_metrics_fn = start_link_metrics;
            desc.stop_link_metrics_fn = stop_link_metrics;
            desc.disconnect_link_stats_fn = disconnect_link_stats;
            desc.reinit_link_metrics_fn = reinit_link_metrics;
            desc.remove_link_stats_fn = remove_link_stats;
            desc.add_stats_metrics_fn = add_stats_metrics;
            desc.get_link_metrics_fn = get_link_metrics;
            desc.set_quality_flags_fn = set_quality_flags;
            desc.get_quality_flags_fn = get_quality_flags;
        }

        initialized = true;
    }

    return &desc;
}
