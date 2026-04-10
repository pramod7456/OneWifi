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
#include <errno.h>
int g_sock = -1;
static char g_gw_ip[50];

int connect_to_gateway(const char *ip) {
    struct sockaddr_in server;
    

    wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d attempting connect to ip=%s port=5000\n", __func__, __LINE__, ip ? ip : "NULL");

    strncpy(g_gw_ip, ip, sizeof(g_gw_ip) - 1);
    g_gw_ip[sizeof(g_gw_ip) - 1] = '\0';

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) {
        wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d socket() failed: %s\n", __func__, __LINE__, strerror(errno));
        return -1;
    }

    server.sin_family = AF_INET;
    server.sin_port = htons(5000);
    if (inet_pton(AF_INET, ip, &server.sin_addr) <= 0) {
        wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d inet_pton failed for ip=%s: %s\n", __func__, __LINE__, ip, strerror(errno));
        close(g_sock);
        g_sock = -1;
        return -1;
    }

    if (connect(g_sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d connect() to %s:5000 failed: %s\n", __func__, __LINE__, ip, strerror(errno));
        close(g_sock);
        g_sock = -1;
        return -1;
    }

    wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d connected to GW %s:5000 fd=%d\n", __func__, __LINE__, ip, g_sock);
    return g_sock;
}

void* run_gateway_thread(void *arg) {
    int server_fd, client_fd;
    struct sockaddr_in addr;
    extern int add_stats_metrics(stats_arg_t *stats, int len);
    extern int periodic_caffinity_stats_update(stats_arg_t *stats, int len);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d server socket() failed: %s\n", __func__, __LINE__, strerror(errno));
        return NULL;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d setsockopt(SO_REUSEADDR) failed: %s\n", __func__, __LINE__, strerror(errno));
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(5000);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d bind() on port 5000 failed: %s\n", __func__, __LINE__, strerror(errno));
        close(server_fd);
        return NULL;
    }
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);

    if (listen(server_fd, 5) < 0) {
        wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d listen() failed: %s\n", __func__, __LINE__, strerror(errno));
        close(server_fd);
        return NULL;
    }

    wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d GW listening on port 5000 fd=%d\n", __func__, __LINE__, server_fd);

    client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d accept() failed: %s\n", __func__, __LINE__, strerror(errno));
        close(server_fd);
        return NULL;
    }

    wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d extender connected client_fd=%d\n", __func__, __LINE__, client_fd);

    while (1) {
        int count = 0;
        int n = recv(client_fd, &count, sizeof(int), MSG_WAITALL);
        if (n == 0) {
            wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d extender closed connection\n", __func__, __LINE__);
            break;
        }
        if (n < 0) {
            wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d recv count failed: %s\n", __func__, __LINE__, strerror(errno));
            break;
        }

        wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d recv count=%d (expected payload=%zu bytes)\n",
            __func__, __LINE__, count, count * sizeof(stats_arg_t));

        if (count <= 0 || count > 256) {
            wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d invalid count=%d, closing\n", __func__, __LINE__, count);
            break;
        }

        stats_arg_t *buf = (stats_arg_t *)calloc(count, sizeof(stats_arg_t));
        if (!buf) {
            wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d calloc failed count=%d size=%zu: %s\n",
                __func__, __LINE__, count, count * sizeof(stats_arg_t), strerror(errno));
            break;
        }

        n = recv(client_fd, buf, count * (int)sizeof(stats_arg_t), MSG_WAITALL);
        if (n == 0) {
            wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d extender closed during payload recv\n", __func__, __LINE__);
            free(buf);
            break;
        }
        if (n < 0) {
            wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d recv payload failed: %s\n", __func__, __LINE__, strerror(errno));
            free(buf);
            break;
        }
        if (n != count * (int)sizeof(stats_arg_t)) {
            wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d short read: got=%d expected=%zu\n",
                __func__, __LINE__, n, count * sizeof(stats_arg_t));
            free(buf);
            break;
        }

        wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d dispatching count=%d ext_event_type=%d mac[0]=%s\n",
            __func__, __LINE__, count, buf[0].ext_event_type, buf[0].mac_str);

        switch (buf[0].ext_event_type) {
            case EXT_EVENT_ADD_STATS:
                wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d -> add_stats_metrics count=%d\n", __func__, __LINE__, count);
                add_stats_metrics(buf, count);
                break;
            case EXT_EVENT_PERIODIC_CAFFINITY:
                wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d -> periodic_caffinity_stats_update count=%d\n", __func__, __LINE__, count);
                periodic_caffinity_stats_update(buf, count);
                break;
            default:
                wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d unknown ext_event_type=%d mac=%s\n",
                    __func__, __LINE__, buf[0].ext_event_type, buf[0].mac_str);
                break;
        }
        free(buf);
    }

    wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d closing client_fd=%d server_fd=%d\n", __func__, __LINE__, client_fd, server_fd);
    close(client_fd);
    close(server_fd);
    return NULL;
}
/* Check if g_sock is live; if not, attempt one reconnect to the stored GW IP.
 * Returns 0 on success, -1 if the socket is not usable after the attempt. */
static int ensure_connected(void)
{
    if (g_sock > 0) {
        return 0;
    }
    if (g_gw_ip[0] == '\0') {
        wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d no GW IP stored, cannot reconnect\n",
            __func__, __LINE__);
        return -1;
    }
    wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d g_sock=%d, reconnecting to %s\n",
        __func__, __LINE__, g_sock, g_gw_ip);
    int fd = connect_to_gateway(g_gw_ip);
    if (fd <= 0) {
        wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d reconnect to %s failed\n",
            __func__, __LINE__, g_gw_ip);
        return -1;
    }
    wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d reconnected g_sock=%d\n", __func__, __LINE__, g_sock);
    return 0;
}

//Here the stats has to be sent to GW using socket
static int periodic_caffinity_stats_update_ext(stats_arg_t *stats, int len)
{
    wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d len=%d g_sock=%d\n", __func__, __LINE__, len, g_sock);
    if (ensure_connected() < 0) {
        wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d socket not available, dropping caffinity update len=%d\n",
            __func__, __LINE__, len);
        return -1;
    }
    for (int i = 0; i < len; i++) {
        stats[i].ext_event_type = EXT_EVENT_PERIODIC_CAFFINITY;
        wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d [%d] mac=%s event=%d dhcp_event=%d dhcp_msg=%d\n",
            __func__, __LINE__, i, stats[i].mac_str, stats[i].event, stats[i].dhcp_event, stats[i].dhcp_msg_type);
    }
    if (send(g_sock, &len, sizeof(int), MSG_NOSIGNAL) < 0) {
        wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d send count failed g_sock=%d: %s\n", __func__, __LINE__, g_sock, strerror(errno));
        close(g_sock);
        g_sock = -1;
        return -1;
    }
    ssize_t sent = send(g_sock, stats, len * sizeof(stats_arg_t), MSG_NOSIGNAL);
    if (sent < 0) {
        wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d send payload failed g_sock=%d: %s\n", __func__, __LINE__, g_sock, strerror(errno));
        close(g_sock);
        g_sock = -1;
        return -1;
    }
    wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d sent len=%d payload_bytes=%zd\n", __func__, __LINE__, len, sent);
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
//Here the stats has to be sent to GW using socket
static int add_stats_metrics_ext(stats_arg_t *stats, int len)
{
    wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d len=%d g_sock=%d\n", __func__, __LINE__, len, g_sock);
    if (ensure_connected() < 0) {
        wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d socket not available, dropping add_stats len=%d\n",
            __func__, __LINE__, len);
        return -1;
    }
    for (int i = 0; i < len; i++) {
        stats[i].ext_event_type = EXT_EVENT_ADD_STATS;
        wifi_util_dbg_print(WIFI_CTRL, " SOCKET %s:%d [%d] mac=%s snr=%d phy=%d vap=%d\n",
            __func__, __LINE__, i, stats[i].mac_str, stats[i].dev.cli_SNR,
            stats[i].dev.cli_LastDataDownlinkRate, stats[i].vap_index);
    }
    if (send(g_sock, &len, sizeof(int), MSG_NOSIGNAL) < 0) {
        wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d send count failed g_sock=%d: %s\n", __func__, __LINE__, g_sock, strerror(errno));
        close(g_sock);
        g_sock = -1;
        return -1;
    }
    ssize_t sent = send(g_sock, stats, len * sizeof(stats_arg_t), MSG_NOSIGNAL);
    if (sent < 0) {
        wifi_util_error_print(WIFI_CTRL, " SOCKET %s:%d send payload failed g_sock=%d: %s\n", __func__, __LINE__, g_sock, strerror(errno));
        close(g_sock);
        g_sock = -1;
        return -1;
    }
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
        __func__, __LINE__, role, ip);

    fclose(fp);
}

/* Returns true if this device is an Extender — checks both ctrl->network_mode
 * (production) and /nvram/config.txt role (lab/test scenario with two GWs). */
bool lq_is_extender_mode(void)
{
    wifi_ctrl_t *ctrl = (wifi_ctrl_t *)get_wifictrl_obj();
    if (ctrl && ctrl->network_mode == rdk_dev_mode_type_ext) {
        return true;
    }
    char role[50] = {0}, ip[50] = {0};
    read_config(role, ip);
    return (strcmp(role, "Extender") == 0);
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
