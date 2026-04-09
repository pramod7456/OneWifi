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
#include "wifi_ctrl.h"
#include "wifi_linkquality_libs.h"
#include "run_qmgr.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
int g_sock;

int connect_to_gateway(const char *ip) {
    struct sockaddr_in server;
    
    wifi_util_dbg_print(WIFI_APPS,"%s:%d %s\n",__func__,__LINE__,ip);
    memset(&server, 0, sizeof(server));

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) return -1;

    server.sin_family = AF_INET;
    server.sin_port = htons(5000);
    inet_pton(AF_INET, ip, &server.sin_addr);

    if (connect(g_sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
        close(g_sock);
        return -1;
    }
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);

    return g_sock;
}

void* run_gateway_thread(void *arg) {
    int server_fd, client_fd;
    char buffer[1024];
    struct sockaddr_in addr;
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
     int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(5000);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);

    wifi_util_dbg_print(WIFI_APPS,"Gateway listening on port 5000...\n");

    client_fd = accept(server_fd, NULL, NULL);

    while (1) {
        uint32_t net_len;
        int n = recv(client_fd, &net_len, sizeof(net_len), MSG_WAITALL);
        if (n <= 0) break;
        uint32_t data_len = ntohl(net_len);
        wifi_util_dbg_print(WIFI_APPS,"Expecting %u bytes of stats\n", data_len);
        stats_arg_t stats;
        wifi_util_dbg_print(WIFI_APPS,"%s:%d \n", __func__,__LINE__);
        n = recv(client_fd, &stats, sizeof(stats), MSG_WAITALL);
        wifi_util_dbg_print(WIFI_APPS,"%s:%d n=%d\n", __func__,__LINE__,n);
        if (n <= 0) break;

        wifi_util_dbg_print(WIFI_APPS,"Received stats:\n");
        wifi_util_dbg_print(WIFI_APPS,"MAC: %s\n", stats.mac_str);
        wifi_util_dbg_print(WIFI_APPS,"AP MAC: %s\n", stats.ap_mac_str);
        wifi_util_dbg_print(WIFI_APPS,"VAP index: %u, Radio index: %u\n",
                            stats.vap_index, stats.radio_index);
        wifi_util_dbg_print(WIFI_APPS,"Channel Utilization: %d%%\n",
                            stats.channel_utilization);
        wifi_util_dbg_print(WIFI_APPS,"Event: %d, Status code: %u, DHCP Event: %d\n",
                            stats.event, stats.status_code, stats.dhcp_event);
    }
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);

    close(client_fd);
    close(server_fd);
    return NULL;
}
//Here the stats has to be sent to GW using 1905.1 frame
static int periodic_caffinity_stats_update_ext(stats_arg_t *stats,int len) 
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}
//This function is not needed in extender this is specific to project Ignite
static void register_station_mac_ext(const char *str) 
{ 
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
}
//This function is not needed in extender this is specific to the Gateway
static void unregister_station_mac_ext(const char *str)
{ 
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
}
static int start_link_metrics_ext()
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}
static int stop_link_metrics_ext()
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}
//Here the stats has to be sent to GW using 1905.1 frame
static int disconnect_link_stats_ext(stats_arg_t *stats)
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}

//This function is not needed in extender this is specific to the Gateway
static int reinit_link_metrics_ext(server_arg_t *arg)
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}
//Here the stats has to be sent to GW using 1905.1 frame
static int remove_link_stats_ext(stats_arg_t *stats)
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}
//Here the stats has to be sent to GW using 1905.1 frame
static int add_stats_metrics_ext(stats_arg_t *stats, int len)
{
    if (g_sock < 0 || stats == NULL) {
        return -1;
    }
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);

    uint32_t net_len = htonl(len *sizeof(stats_arg_t));

    if (send(g_sock, &net_len, sizeof(net_len), 0) != sizeof(net_len)) {
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
        return -1;
    }
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);

    if (send(g_sock, stats, sizeof(stats_arg_t), 0) != sizeof(stats_arg_t)) {
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
        return -1;
    }
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);

    return 0;
}
//This function is not needed in extender this is specific to the Gateway
static char* get_link_metrics_ext() 
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return NULL;
}
//This function is not needed in extender this is specific to the Gateway
static int set_quality_flags_ext(quality_flags_t *flag)
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}
//This function is not needed in extender this is specific to the Gateway
static int get_quality_flags_ext(quality_flags_t *flag)
{
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    return 0;
}

static void read_config(char *role, char *ip) {
    FILE *fp = fopen("/nvram/config.txt", "r");
    if (!fp) return;

    char line[100];
    if (fgets(line, sizeof(line), fp)) {
        sscanf(line, "%[^,],%s", role, ip);
        ip[strcspn(ip, "\n")] = 0;
    }
    wifi_util_error_print(WIFI_APPS, "%s:%d role=%s and ip=%s\n",
        __func__, __LINE__,role,ip);

    fclose(fp);
}
wifi_lq_descriptor_t* get_lq_descriptor()
{
    static bool initialized = false;
    static wifi_lq_descriptor_t desc;
    wifi_ctrl_t *ctrl = NULL ;
    char role[50] = {0};
    char ip[50] = {0};
    pthread_t tid;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (!initialized) { 
        //read_config,role will be only for testing between 2 GW
        read_config(role, ip);

        ctrl = (wifi_ctrl_t *)get_wifictrl_obj();
        if (ctrl->network_mode == rdk_dev_mode_type_ext || strcmp(role, "Extender") == 0) {
            // Use Socket and send it to the GW
            wifi_util_error_print(WIFI_CTRL, "%s:%d\n", __func__, __LINE__);
            desc.periodic_caffinity_stats_update_fn = periodic_caffinity_stats_update_ext;
            desc.register_station_mac_fn = register_station_mac_ext;
            desc.unregister_station_mac_fn = unregister_station_mac_ext;
            desc.start_link_metrics_fn = start_link_metrics_ext;
            desc.stop_link_metrics_fn = stop_link_metrics_ext;
            desc.disconnect_link_stats_fn = disconnect_link_stats_ext;
            desc.reinit_link_metrics_fn = reinit_link_metrics_ext;
            desc.remove_link_stats_fn = remove_link_stats_ext;
            desc.add_stats_metrics_fn = add_stats_metrics_ext;
            desc.get_link_metrics_fn = get_link_metrics_ext;
            desc.set_quality_flags_fn = set_quality_flags_ext;
            desc.get_quality_flags_fn = get_quality_flags_ext;
	    connect_to_gateway(ip);
        } else {
            // Use Library calls in EasyMesh,Ignite and GW mode
            wifi_util_error_print(WIFI_CTRL, "%s:%d\n", __func__, __LINE__);
            extern int periodic_caffinity_stats_update(stats_arg_t *stats,int len);
            extern void register_station_mac(const char *str);
            extern void unregister_station_mac(const char *str);
            extern int start_link_metrics();
            extern int stop_link_metrics();
            extern int disconnect_link_stats(stats_arg_t *stats);
            extern int reinit_link_metrics(server_arg_t *arg);
            extern int remove_link_stats(stats_arg_t *stats);
            extern int add_stats_metrics(stats_arg_t *stats,int len);
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
            pthread_create(&tid, &attr, run_gateway_thread, NULL);
        }

        initialized = true;
    }

    return &desc;
}
