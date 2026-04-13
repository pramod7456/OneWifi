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
#include "wifi_linkquality.h"
#include "run_qmgr.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#define MAX_EM_BUFF_SZ  1024
#define QMGR_FILE "/tmp/qmgr_ready"

int g_sock = -1;
static char g_gw_ip[50];
static  int create_autoconfig_resp_msg(unsigned char *buff, unsigned char *dst, 
    char *interface_name,stats_arg_t *stats, int len,ext_qualitymgr_type_t event);

static int send_frame(unsigned char *buff, unsigned int len, bool multicast,  char *ifname);

int connect_to_gateway(const char *ip) {
    struct sockaddr_in server;
    

    wifi_util_dbg_print(WIFI_APPS, " SOCKET %s:%d attempting connect to ip=%s port=5000\n", __func__, __LINE__, ip ? ip : "NULL");

    strncpy(g_gw_ip, ip, sizeof(g_gw_ip) - 1);
    g_gw_ip[sizeof(g_gw_ip) - 1] = '\0';

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) {
        wifi_util_error_print(WIFI_APPS, " SOCKET %s:%d socket() failed: %s\n", __func__, __LINE__, strerror(errno));
        return -1;
    }

    server.sin_family = AF_INET;
    server.sin_port = htons(5000);
    if (inet_pton(AF_INET, ip, &server.sin_addr) <= 0) {
        wifi_util_error_print(WIFI_APPS, " SOCKET %s:%d inet_pton failed for ip=%s: %s\n", __func__, __LINE__, ip, strerror(errno));
        close(g_sock);
        g_sock = -1;
        return -1;
    }

    if (connect(g_sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        wifi_util_error_print(WIFI_APPS, " SOCKET %s:%d connect() to %s:5000 failed: %s\n", __func__, __LINE__, ip, strerror(errno));
        close(g_sock);
        g_sock = -1;
        return -1;
    }

    wifi_util_dbg_print(WIFI_APPS, " SOCKET %s:%d connected to GW %s:5000 fd=%d\n", __func__, __LINE__, ip, g_sock);
    return g_sock;
}

/* Check if g_sock is live; if not, attempt one reconnect to the stored GW IP.
 * Returns 0 on success, -1 if the socket is not usable after the attempt. */
static int ensure_connected(void)
{
    if (g_sock > 0) {
        return 0;
    }
    if (g_gw_ip[0] == '\0') {
        wifi_util_error_print(WIFI_APPS, " SOCKET %s:%d no GW IP stored, cannot reconnect\n",
            __func__, __LINE__);
        return -1;
    }
    wifi_util_dbg_print(WIFI_APPS, " SOCKET %s:%d g_sock=%d, reconnecting to %s\n",
        __func__, __LINE__, g_sock, g_gw_ip);
    int fd = connect_to_gateway(g_gw_ip);
    if (fd <= 0) {
        wifi_util_error_print(WIFI_APPS, " SOCKET %s:%d reconnect to %s failed\n",
            __func__, __LINE__, g_gw_ip);
        return -1;
    }
    wifi_util_dbg_print(WIFI_APPS, " SOCKET %s:%d reconnected g_sock=%d\n", __func__, __LINE__, g_sock);
    return 0;
}

static int send_qmgr_data_to_gateway(stats_arg_t *stats, int len,
                                     ext_qualitymgr_type_t qmgr_val)
{
    if (ensure_connected() < 0) {
        wifi_util_error_print(WIFI_APPS,
            " SOCKET %s:%d socket not available, dropping update len=%d\n",
            __func__, __LINE__, len);
        return -1;
    }

    size_t packet_size = sizeof(qmgr_packet_t) + len * sizeof(stats_arg_t);
    qmgr_packet_t *packet = malloc(packet_size);
    if (!packet) {
        return -1;
    }

    packet->len = len;
    packet->ext_event_type = qmgr_val;
    memcpy(packet->stats, stats, len * sizeof(stats_arg_t));

    // Send all packet in a single go
    ssize_t sent = send(g_sock, packet, packet_size, MSG_NOSIGNAL);
    if (sent < 0) {
        wifi_util_error_print(WIFI_APPS,
            " SOCKET %s:%d send failed g_sock=%d:\n",
            __func__, __LINE__, g_sock);
        close(g_sock);
        g_sock = -1;
        free(packet);
        return -1;
    }

    wifi_util_dbg_print(WIFI_APPS,
        " SOCKET %s:%d sent len=%d payload_bytes=%zd\n",
        __func__, __LINE__, len, sent);

    free(packet);
    return 0;
}
void* run_gateway_thread(void *arg) {
    int server_fd, client_fd;
    struct sockaddr_in addr;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        wifi_util_error_print(WIFI_APPS, " SOCKET %s:%d server socket() failed: %s\n", __func__, __LINE__, strerror(errno));
        return NULL;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        wifi_util_error_print(WIFI_APPS, " SOCKET %s:%d setsockopt(SO_REUSEADDR) failed: %s\n", __func__, __LINE__, strerror(errno));
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(5000);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        wifi_util_error_print(WIFI_APPS, " SOCKET %s:%d bind() on port 5000 failed: %s\n", __func__, __LINE__, strerror(errno));
        close(server_fd);
        return NULL;
    }
    wifi_util_dbg_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);

    if (listen(server_fd, 5) < 0) {
        wifi_util_error_print(WIFI_APPS, " SOCKET %s:%d listen() failed: %s\n", __func__, __LINE__, strerror(errno));
        close(server_fd);
        return NULL;
    }

    wifi_util_dbg_print(WIFI_APPS, " SOCKET %s:%d GW listening on port 5000 fd=%d\n", __func__, __LINE__, server_fd);

    client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        wifi_util_error_print(WIFI_APPS, " SOCKET %s:%d accept() failed: %s\n", __func__, __LINE__, strerror(errno));
        close(server_fd);
        return NULL;
    }

    wifi_util_dbg_print(WIFI_APPS, " SOCKET %s:%d extender connected client_fd=%d\n", __func__, __LINE__, client_fd);
    while (1) {
        qmgr_packet_t hdr;

        int n = recv(client_fd, &hdr, sizeof(hdr), MSG_WAITALL);
        if (n <=0 || n != sizeof(hdr)) {
            wifi_util_dbg_print(WIFI_APPS, "  %s:%d Received header not valid \n", __func__, __LINE__);
            break;
        }
        int count = hdr.len;
        ext_qualitymgr_type_t event = hdr.ext_event_type;

        wifi_util_dbg_print(WIFI_APPS,"%s:%d recv header count=%d event=%d\n",
           __func__, __LINE__, count, event);

    if (count <= 0 || count > 256) {
        wifi_util_error_print(WIFI_APPS," %s:%d invalid count=%d, closing\n",
            __func__, __LINE__, count);
        break;
    }

    stats_arg_t *buf = calloc(count, sizeof(stats_arg_t));
    if (!buf) {
        wifi_util_error_print(WIFI_APPS,"%s:%d calloc failed count=%d size=%zu:\n",
            __func__, __LINE__, count, count * sizeof(stats_arg_t));
        break;
    }

    n = recv(client_fd, buf, count * sizeof(stats_arg_t), MSG_WAITALL);
    if (n <= 0 || n != count * (int)sizeof(stats_arg_t)) {
        wifi_util_dbg_print(WIFI_APPS,
            " SOCKET %s:%d received payload is less\n",
            __func__, __LINE__);
        free(buf);
        break;
    }
    wifi_util_dbg_print(WIFI_APPS,"%s:%d:%d:%d\n",__func__, __LINE__, event,count);
    switch (event) {
        case ext_qualitymgr_add_stats:
            add_stats_metrics(buf, count);
            break;

        case ext_qualitymgr_periodic_caffinity:
            periodic_caffinity_stats_update(buf, count);
            break;

        case ext_qualitymgr_disconnect_link_stats:
            disconnect_link_stats(buf);
            break;

        case ext_qualitymgr_remove_link_stats:
            remove_link_stats(buf);
            break;

        case ext_qualitymgr_lq_affinity:
            add_stats_metrics(buf,count);
            periodic_caffinity_stats_update(buf, count);
            break;

        default:
            wifi_util_error_print(WIFI_APPS,
                " SOCKET %s:%d unknown ext_event_type=%d\n",
                __func__, __LINE__, event);
            break;
    }
    free(buf);
}
    wifi_util_dbg_print(WIFI_APPS, " SOCKET %s:%d closing client_fd=%d server_fd=%d\n", __func__, __LINE__, client_fd, server_fd);
    close(client_fd);
    close(server_fd);
    return NULL;
}

//Here the stats has to be sent to GW using socket
static int periodic_caffinity_stats_update_ext(stats_arg_t *stats, int len)
{
    unsigned char msg[MAX_EM_BUFF_SZ];
    //This should be ur sending interface of RPI(Extender)
    char *ifname = "eth0";
    int frame_len = 0;
    //This should be the MAC address of brlan0 of GW
    unsigned char mac[] = {0xe0, 0xdb, 0xd1, 0xdd,0x08, 0x74};
    wifi_util_dbg_print(WIFI_APPS, " SOCKET %s:%d len=%d g_sock=%d\n", __func__, __LINE__, len, g_sock);
    if (access(QMGR_FILE, F_OK) == 0) {
        send_qmgr_data_to_gateway(stats,len,ext_qualitymgr_periodic_caffinity);
    } else {
        frame_len = create_autoconfig_resp_msg(msg,(unsigned char*)mac,ifname,stats,len,ext_qualitymgr_periodic_caffinity);
        send_frame(msg, frame_len, false, ifname);
    }
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
    unsigned char msg[MAX_EM_BUFF_SZ];
    //This should be ur sending interface of RPI(Extender)
    char *ifname = "eth0";
    int frame_len = 0;
    //This should be the MAC address of brlan0 of GW
    unsigned char mac[] = {0xe0, 0xdb, 0xd1, 0xdd,0x08, 0x74};
    if (access(QMGR_FILE, F_OK) == 0) {
        send_qmgr_data_to_gateway(stats,1,ext_qualitymgr_disconnect_link_stats);
    } else {
        frame_len = create_autoconfig_resp_msg(msg,(unsigned char*)mac,ifname,stats,1,ext_qualitymgr_disconnect_link_stats);
        send_frame(msg, frame_len, false, ifname);
    }
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
    unsigned char msg[MAX_EM_BUFF_SZ];
    //This should be ur sending interface of RPI(Extender)
    char *ifname = "eth0";
    int frame_len = 0;
    //This should be the MAC address of brlan0 of GW
    unsigned char mac[] = {0xe0, 0xdb, 0xd1, 0xdd,0x08, 0x74};
    if (access(QMGR_FILE, F_OK) == 0) {
        send_qmgr_data_to_gateway(stats,1,ext_qualitymgr_disconnect_link_stats);
    } else {
        frame_len = create_autoconfig_resp_msg(msg,(unsigned char*)mac,ifname,stats,1,ext_qualitymgr_disconnect_link_stats);
        send_frame(msg, frame_len, false, ifname);
    }
    return 0;
}
//Unified extender dispatcher: fills ext_event_type via send_qmgr_data_to_gateway
static int process_lq_stats_ext(stats_arg_t *stats, int len)
{
    wifi_util_dbg_print(WIFI_APPS, " SOCKET %s:%d len=%d  g_sock=%d\n",
        __func__, __LINE__, len, g_sock);
    unsigned char msg[MAX_EM_BUFF_SZ];
    //This should be ur sending interface of RPI(Extender)
    char *ifname = "eth0";
    int frame_len = 0;
    //This should be the MAC address of brlan0 of GW
    unsigned char mac[] = {0xe0, 0xdb, 0xd1, 0xdd,0x08, 0x74};
    wifi_util_dbg_print(WIFI_APPS, " SOCKET %s:%d len=%d g_sock=%d\n", __func__, __LINE__, len, g_sock);
    if (access(QMGR_FILE, F_OK) == 0) {
        send_qmgr_data_to_gateway(stats,len,ext_qualitymgr_lq_affinity);
    } else {
        frame_len = create_autoconfig_resp_msg(msg,(unsigned char*)mac,ifname,stats,len,ext_qualitymgr_lq_affinity);
        send_frame(msg, frame_len, false, ifname);
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

//This function is for GW to start dhcp_sniffer
static int start_link_metrics_gw()
{
    wifi_util_info_print(WIFI_APPS, " %s:%d Stopping DHCP sniffer (GW mode)\n", __func__, __LINE__);
    dhcp_sniffer_start();
    start_link_metrics();    
    return 0;
}

//This function is for GW to stop  dhcp_sniffer
static int stop_link_metrics_gw()
{
    wifi_util_info_print(WIFI_APPS, " %s:%d Stopping DHCP sniffer (GW mode)\n", __func__, __LINE__);
    dhcp_sniffer_stop();
    stop_link_metrics();
    return 0;
}
//GW-only dispatcher: calls add_stats_metrics or periodic_caffinity_stats_update based on enum
static int process_lq_stats_gw(stats_arg_t *stats, int len)
{
    wifi_util_dbg_print(WIFI_APPS, "%s:%d len=%d \n", __func__, __LINE__, len);
    add_stats_metrics(stats, len);
    periodic_caffinity_stats_update(stats, len);
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
            desc.get_link_metrics_fn = get_link_metrics_ext;
            desc.set_quality_flags_fn = set_quality_flags_ext;
            desc.get_quality_flags_fn = get_quality_flags_ext;
            desc.process_lq_stats_fn = process_lq_stats_ext;
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
            extern char* get_link_metrics();
            extern int set_quality_flags(quality_flags_t *flag);
            extern int get_quality_flags(quality_flags_t *flag);

            desc.periodic_caffinity_stats_update_fn = periodic_caffinity_stats_update;
            desc.register_station_mac_fn = register_station_mac;
            desc.unregister_station_mac_fn = unregister_station_mac;
            desc.start_link_metrics_fn = start_link_metrics_gw;
            desc.stop_link_metrics_fn = stop_link_metrics_gw;
            desc.disconnect_link_stats_fn = disconnect_link_stats;
            desc.reinit_link_metrics_fn = reinit_link_metrics;
            desc.remove_link_stats_fn = remove_link_stats;
            desc.get_link_metrics_fn = get_link_metrics;
            desc.set_quality_flags_fn = set_quality_flags;
            desc.get_quality_flags_fn = get_quality_flags;
            desc.process_lq_stats_fn = process_lq_stats_gw;
            pthread_create(&tid, &attr, run_gateway_thread, NULL);
        }

        initialized = true;
    }

    return &desc;
}

static int send_frame(unsigned char *buff, unsigned int len, bool multicast,  char *ifname)
{
    int ret = 0;
    multiap_raw_hdr_t *hdr = (multiap_raw_hdr_t *)(buff);

    struct sockaddr_ll sadr_ll;
    int sock;
    mac_address_t   multi_addr = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    wifi_util_info_print(WIFI_CTRL,"Sending frame on %s\n",ifname);

    sock = socket(AF_PACKET, SOCK_RAW, IPPROTO_RAW);
    if (sock < 0) {
        return -1;
    }

    sadr_ll.sll_ifindex = (int)(if_nametoindex(ifname));
    sadr_ll.sll_halen = ETH_ALEN; // length of destination mac address
    sadr_ll.sll_protocol = htons(ETH_P_ALL);
    memcpy(sadr_ll.sll_addr, (multicast == true) ? multi_addr:hdr->dst, sizeof(mac_address_t));

    ret = (int)(sendto(sock, buff, len, 0, (const struct sockaddr*)&sadr_ll, sizeof(struct sockaddr_ll)));
    wifi_util_info_print(WIFI_CTRL,"Sent frame on %s ret val =%d\n",ifname,ret);
    close(sock);
    return ret;
 }

int create_autoconfig_resp_msg(unsigned char *buff, unsigned char *dst, char *interface_name,stats_arg_t *stats, int num_devs,ext_qualitymgr_type_t event)
{
    unsigned short msg_id = multiap_msg_type_autoconf_resp;
    multiap_cmdu_t *cmdu;
    multiap_tlv_t *tlv;
    int len = 0;
    unsigned char *tmp = buff;
    unsigned char src_addr[64];
    char st[64] = { 0 };
    
    unsigned short type = htons(ETH_P_1905);
    
    mac_address_from_name(interface_name, src_addr);

    uint8_mac_to_string_mac(src_addr, st);
    wifi_util_info_print(WIFI_APPS, "Source MAC from interface %s = %s\n", interface_name, st);

    uint8_mac_to_string_mac(dst, st);
    wifi_util_info_print(WIFI_APPS, "Destination MAC = %s\n", st);

    memcpy(tmp, (unsigned char *)dst, sizeof(mac_address_t));
    tmp += sizeof(mac_address_t);
    len += (int)(sizeof(mac_address_t));

    memcpy(tmp, (unsigned char *)src_addr, sizeof(mac_address_t));
    tmp += sizeof(mac_address_t);
    len += (int)(sizeof(mac_address_t));

    memcpy(tmp, (unsigned char *)(&type), sizeof(unsigned short));
    tmp += sizeof(unsigned short);
    len += (int)(sizeof(unsigned short));
    cmdu = (multiap_cmdu_t *)(tmp);

    memset(tmp, 0, sizeof(multiap_cmdu_t));
    cmdu->type = htons(msg_id);
    cmdu->id = msg_id;
    msg_id++;
    cmdu->last_frag_ind = 1;
    cmdu->relay_ind = 1;

    tmp += sizeof(multiap_cmdu_t);
    len += (int)(sizeof(multiap_cmdu_t));

  // supported service tlv 17.2.1
    tlv = (multiap_tlv_t *) (tmp);
    tlv->type = multiap_tlv_type_searched_role;
    tlv->len = htons(sizeof(unsigned char));
    
    memcpy(&tlv->value[1], &event, sizeof(multiap_enum_type_t));

    tmp += (sizeof(multiap_tlv_t) + sizeof(multiap_enum_type_t) + 1);
    len += (int) (sizeof(multiap_tlv_t) + sizeof(multiap_enum_type_t) + 1);

    /* LinkQualityData TLV */
    tlv = (multiap_tlv_t *)tmp;

    tlv->type =  multiap_tlv_type_lq;  // or correct type
    int payload_len = num_devs * sizeof(stats_arg_t);

     tlv->len = htons(payload_len);

    memcpy(tlv->value, stats, payload_len);

    tmp += sizeof(multiap_tlv_t) + payload_len;
    len += sizeof(multiap_tlv_t) + payload_len;
    
    /* End of message */
    tlv = (multiap_tlv_t *)(tmp);
    tlv->type = multiap_tlv_type_eom;
    tlv->len = 0;

    tmp += (sizeof(multiap_tlv_t));
    len += (int)(sizeof(multiap_tlv_t));
    wifi_util_info_print(WIFI_APPS, "%s:%d Autoconfig response message created successfully, total_length=%d bytes\n",
       __func__, __LINE__, len);

    return len;
}

