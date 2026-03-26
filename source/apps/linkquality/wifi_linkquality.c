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
#include "stdlib.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <linux/if_packet.h>
#include <linux/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "wifi_hal.h"
#include "wifi_base.h"
#include "wifi_ctrl.h"
#include "wifi_mgr.h"
#include "wifi_stubs.h"
#include "wifi_util.h"
#include "wifi_apps_mgr.h"
#include "wifi_linkquality.h"
#include "wifi_hal_rdk_framework.h"
#include "wifi_monitor.h"
#include "scheduler.h"
#include "common/ieee802_11_defs.h"
#include "secure_wrapper.h"
#include "dml_onewifi_api.h"

#define MAX_STR_LEN 128
#define MAX_BUFF_LEN 1048
#define IGNITE_SCORE_LOG_INTERVAL_MS 900000 // 15 mins
#define IGNITE_INITIAL_PUBLISH_ITERATIONS 5

#define BUFFER_SIZE 65536
#define DHCP_BOOTP 1
#define DHCP_OP_MSG_TYPE 53
#define DHCP_OPTION_HOSTNAME 12
#define DHCP_OPTION_VENDOR_CLASS_ID 60
#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPDECLINE  4
#define DHCPACK      5
#define DHCPNAK      6

/* Local DHCP per-client stats structure */
typedef struct {
    unsigned char mac[6];
    uint32_t discover;
    uint32_t offer;
    uint32_t request;
    uint32_t decline;
    uint32_t ack;
    uint32_t nak;
} dhcp_local_stats_t;

#define MAX_DHCP_CLIENTS 64
static dhcp_local_stats_t dhcp_local_clients[MAX_DHCP_CLIENTS];
static int dhcp_local_count = 0;
static pthread_mutex_t dhcp_local_lock = PTHREAD_MUTEX_INITIALIZER;

struct dhcp_data
{
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;

    uint32_t xid;

    uint16_t secs;
    uint16_t flags;

    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;

    uint8_t chaddr[16];
};


static int dhcp_sniffer_fd = -1;
static int dhcp_sniffer_running = 0;
static pthread_t dhcp_sniffer_thread;
static volatile int dhcp_sniffer_exit = 0;


static char *wifi_health_log = "/rdklogs/logs/wifihealth.txt";

/* Find or create local DHCP stats entry for a MAC address */
static dhcp_local_stats_t* dhcp_local_find_or_create(const unsigned char *mac)
{
    int i;
    
    pthread_mutex_lock(&dhcp_local_lock);
    
    // Search for existing entry
    for (i = 0; i < dhcp_local_count; i++) {
        if (memcmp(dhcp_local_clients[i].mac, mac, 6) == 0) {
            pthread_mutex_unlock(&dhcp_local_lock);
            return &dhcp_local_clients[i];
        }
    }
    
    // Create new entry if space available
    if (dhcp_local_count < MAX_DHCP_CLIENTS) {
        dhcp_local_stats_t *entry = &dhcp_local_clients[dhcp_local_count];
        memset(entry, 0, sizeof(dhcp_local_stats_t));
        memcpy(entry->mac, mac, 6);
        dhcp_local_count++;
        pthread_mutex_unlock(&dhcp_local_lock);
        return entry;
    }
    
    pthread_mutex_unlock(&dhcp_local_lock);
    return NULL;
}

/* Update local DHCP stats and return computed attempts/failures */
static void dhcp_local_update_and_compute(dhcp_local_stats_t *stats, int msg_type,
                                          uint32_t *out_attempts, uint32_t *out_failures)
{
    pthread_mutex_lock(&dhcp_local_lock);
    
    switch(msg_type) {
        case DHCPDISCOVER: stats->discover++; break;
        case DHCPOFFER:    stats->offer++; break;
        case DHCPREQUEST:  stats->request++; break;
        case DHCPDECLINE:  stats->decline++; break;
        case DHCPACK:      stats->ack++; break;
        case DHCPNAK:      stats->nak++; break;
    }
    
    wifi_util_info_print(WIFI_CTRL,
        "DHCP_STATS %s:%d [msg_type=%d] raw counters:"
        " discover=%u offer=%u request=%u\n"
        "  ack=%u nak=%u decline=%u\n",
        __func__, __LINE__, msg_type,
        stats->discover, stats->offer, stats->request,
        stats->ack, stats->nak, stats->decline);

    // Compute attempts as max(discover, request).
    // - Initial lease: DISCOVER → OFFER → REQUEST → ACK (discover=1, request=1)
    // - Renewal:       REQUEST → ACK                    (discover=0, request=1)
    // - Failed discover (no offer): discover=1, request=0
    // max() correctly counts each transaction once in all cases.
    *out_attempts = (stats->discover > stats->request) ? stats->discover : stats->request;
    
    // Compute failures:
    // 1) nak + decline = explicit server-side failures
    // 2) (discover - offer) when positive = unanswered DISCOVERs (server
    //    unreachable / no offer received)
    // Do NOT use (request - ack): ACK always arrives in a later packet than
    // its REQUEST, so the difference is transiently non-zero and would be
    // reported as a false failure.
    uint32_t unanswered_discovers = (stats->discover > stats->offer)
                                    ? (stats->discover - stats->offer) : 0;
    *out_failures = stats->nak + stats->decline + unanswered_discovers;

    wifi_util_info_print(WIFI_CTRL,
        "DHCP_STATS %s:%d computed:"
        " unanswered_discovers=%u out_attempts=%u out_failures=%u\n",
        __func__, __LINE__,
        unanswered_discovers, *out_attempts, *out_failures);
    
    pthread_mutex_unlock(&dhcp_local_lock);
}

static int dhcp_get_msg_type(uint8_t *options, ssize_t options_len)
{
    while (options_len > 0)
    {
        uint8_t type = options[0];

        if (type == 255)
            break;

        if (type == 0)
        {
            options++;
            options_len--;
            continue;
        }

        uint8_t len = options[1];

        if (type == 53)
            return options[2];

        options += len + 2;
        options_len -= len + 2;
    }

    return -1;
}

static void mac_to_key(const unsigned char *mac, char *key)
{
    snprintf(key, 18,
        "%02x:%02x:%02x:%02x:%02x:%02x",
        mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

/* Register callback BEFORE starting qmgr */
void publish_qmgr_subdoc(const report_batch_t* report)
{
    webconfig_subdoc_type_t subdoc_type;
    webconfig_subdoc_data_t *data;
    bus_error_t status;
    raw_data_t rdata;
    wifi_app_t *wifi_app = NULL;
    wifi_util_dbg_print(WIFI_WEBCONFIG," %s:%d link_count=%d\n",__func__,__LINE__,report->link_count);
    wifi_util_error_print(WIFI_CTRL," SANJI %s:%d  \n", __func__, __LINE__);
    wifi_ctrl_t *ctrl = (wifi_ctrl_t *)get_wifictrl_obj();
    data = (webconfig_subdoc_data_t *)malloc(sizeof(webconfig_subdoc_data_t));
    if (data == NULL) {
        wifi_util_error_print(WIFI_WEBCONFIG, "%s:%d Error in allocation memory\n", __func__, __LINE__);
        return ;
    }
 
    memset(data, '\0', sizeof(webconfig_subdoc_data_t));
    data->u.decoded.hal_cap = get_wifimgr_obj()->hal_cap;
    for (unsigned int i = 0; i < getNumberRadios(); i++){
        data->u.decoded.radios[i] = get_wifimgr_obj()->radio_config[i];
    }
    data->u.decoded.qmgr_report =  (report_batch_t *)report;
    subdoc_type = webconfig_subdoc_type_link_report;
    if (webconfig_encode(&ctrl->webconfig, data, subdoc_type) != webconfig_error_none) {
        wifi_util_error_print(WIFI_WEBCONFIG, "%s:%d Error in encoding link report\n", __func__,
              __LINE__);
        free(data);
        return;
    }
    memset(&rdata, 0, sizeof(raw_data_t));
    rdata.data_type = bus_data_type_string;
    rdata.raw_data.bytes = (void *)data->u.encoded.raw;
    wifi_util_dbg_print(WIFI_WEBCONFIG,"raw data=%s\n",(char*)rdata.raw_data.bytes);
    rdata.raw_data_len = strlen(data->u.encoded.raw) + 1;


    wifi_app = get_app_by_inst(&ctrl->apps_mgr, wifi_app_inst_link_quality);
    if (wifi_app == NULL) {
        wifi_util_error_print(WIFI_APPS, "%s:%d NULL Pointer \n", __func__, __LINE__);
        return;
    }
    status = get_bus_descriptor()->bus_event_publish_fn(&wifi_app->ctrl->handle, WIFI_QUALITY_LINKREPORT, &rdata);
    if (status != bus_error_success) {
        wifi_util_error_print(WIFI_WEBCONFIG, "%s:%d: bus: bus_event_publish_fn Event failed %d\n",
            __func__, __LINE__, status);
        free(data);
        return ;
    }
    if(data)
        free(data);
    return;
}


static void dhcp_process_packet(const uint8_t *buffer, ssize_t len)
{
    struct iphdr *ip_header;
    struct udphdr *udp_header;
    struct dhcp_data *dhcp;
    char mac_key[18];
    int msg_type;
    uint16_t dest_port;
    const char *msg_type_str = "UNKNOWN";
    const char *direction = "";
    int vap_index = -1;
    int radio_index = -1;
    
    // DHCP packet structure offsets:
    // Ethernet (14) + IP (variable, usually 20) + UDP (8) + DHCP header (236) + Magic Cookie (4)
    // DHCP header = op(1) + htype(1) + hlen(1) + hops(1) + xid(4) + secs(2) + flags(2) +
    //               ciaddr(4) + yiaddr(4) + siaddr(4) + giaddr(4) + chaddr(16) + 
    //               sname(64) + file(128) = 236 bytes
    int eth_hdr_len = sizeof(struct ethhdr);  // 14 bytes
    int ip_hdr_len;   // Variable, calculated from IP header
    int udp_hdr_len = sizeof(struct udphdr);  // 8 bytes
    int dhcp_fixed_len = 236;  // Fixed DHCP header up to magic cookie
    int magic_cookie_len = 4;
    int options_offset;

    //wifi_util_dbg_print(WIFI_CTRL," DHCP %s:%d ENTERING dhcp_process_packet, len=%zd\n", __func__, __LINE__, len);

    // ============================================================================
    // STEP 1: Basic packet validation
    // ============================================================================
    if (len < eth_hdr_len + 20 + udp_hdr_len) {  // Minimum: Eth + min IP + UDP
        return;
    }

    // ============================================================================
    // STEP 2: Verify it's a UDP packet and get IP header length
    // ============================================================================
    ip_header = (struct iphdr *)(buffer + eth_hdr_len);
    ip_hdr_len = ip_header->ihl * 4;  // ihl is in 4-byte words
    
    if (ip_header->protocol != IPPROTO_UDP) {
        return;
    }

    // ============================================================================
    // STEP 3: Verify it's on DHCP ports (67=server, 68=client)
    // ============================================================================
    udp_header = (struct udphdr *)(buffer + eth_hdr_len + ip_hdr_len);
    dest_port = ntohs(udp_header->dest);
    
    if (!(dest_port == 67 || dest_port == 68)) {
        return;
    }

    // Determine direction based on destination port
    if (dest_port == 67) {
        direction = "CLIENT->SERVER";
    } else {
        direction = "SERVER->CLIENT";
    }

    // ============================================================================
    // STEP 4: Extract DHCP header and client MAC address
    // ============================================================================
    int dhcp_start = eth_hdr_len + ip_hdr_len + udp_hdr_len;
    dhcp = (struct dhcp_data *)(buffer + dhcp_start);
    mac_to_key(dhcp->chaddr, mac_key);

    // ============================================================================
    // STEP 5: Verify MAC is connected (using caffinity)
    // ============================================================================
    if (!is_client_connected(mac_key)) {
        wifi_util_dbg_print(WIFI_CTRL," DHCP %s:%d Client MAC %s NOT connected, REJECTING packet\n", 
            __func__, __LINE__, mac_key);
        return;
    }
    
    wifi_util_info_print(WIFI_CTRL," DHCP %s:%d Client MAC %s is connected\n", 
        __func__, __LINE__, mac_key);

    // ============================================================================
    // STEP 6: Calculate options offset and verify magic cookie
    // ============================================================================
    options_offset = dhcp_start + dhcp_fixed_len;
    
    if (len < options_offset + magic_cookie_len) {
        return;
    }
    
    // Check for DHCP magic cookie (0x63825363)
    uint8_t *magic_cookie_ptr = (uint8_t *)(buffer + options_offset);
    uint32_t magic_cookie = (magic_cookie_ptr[0] << 24) | (magic_cookie_ptr[1] << 16) | 
                            (magic_cookie_ptr[2] << 8) | magic_cookie_ptr[3];
    
    if (magic_cookie != 0x63825363) {
        wifi_util_dbg_print(WIFI_CTRL," DHCP %s:%d Invalid magic cookie, REJECTING\n", __func__, __LINE__);
        return;
    }

    // Options start right after magic cookie
    uint8_t *options = (uint8_t *)(buffer + options_offset + magic_cookie_len);
    ssize_t options_len = len - (options_offset + magic_cookie_len);
    
    msg_type = dhcp_get_msg_type(options, options_len);
    
    // Convert msg_type to string for logging
    switch(msg_type) {
        case DHCPDISCOVER: msg_type_str = "DISCOVER"; break;
        case DHCPOFFER:    msg_type_str = "OFFER"; break;
        case DHCPREQUEST:  msg_type_str = "REQUEST"; break;
        case DHCPDECLINE:  msg_type_str = "DECLINE"; break;
        case DHCPACK:      msg_type_str = "ACK"; break;
        case DHCPNAK:      msg_type_str = "NAK"; break;
        default:           msg_type_str = "UNKNOWN"; break;
    }

    // ============================================================================
    // STEP 7: Validate message type
    // ============================================================================
    if (msg_type < 0) {
        return;
    }

    // ============================================================================
    // STEP 8: Check if this is a BOOTP packet (op=1 for client->server, op=2 for server->client)
    // ============================================================================
    if (dhcp->op != DHCP_BOOTP && dhcp->op != 2) {
        return;
    }

    // ============================================================================
    // STEP 9: Update local DHCP stats and pass to caffinity
    // ============================================================================
    wifi_util_info_print(WIFI_CTRL," DHCP %s:%d Processing %s packet for MAC=%s (%s)\n",
        __func__, __LINE__, msg_type_str, mac_key, direction);
    
    // Find or create local stats entry for this client
    dhcp_local_stats_t *local_stats = dhcp_local_find_or_create(dhcp->chaddr);
    if (!local_stats) {
        wifi_util_dbg_print(WIFI_CTRL," DHCP %s:%d No space for new DHCP client\n", __func__, __LINE__);
        return;
    }
    
    // Update local stats and compute attempts/failures
    uint32_t dhcp_attempts = 0;
    uint32_t dhcp_failures = 0;
    dhcp_local_update_and_compute(local_stats, msg_type, &dhcp_attempts, &dhcp_failures);
    
    wifi_util_info_print(WIFI_CTRL," DHCP_main %s:%d MAC=%s: attempts=%u failures=%u\n",
        __func__, __LINE__, mac_key, dhcp_attempts, dhcp_failures);
    
    // Prepare affinity_arg_t and call update_affinity_stats
    affinity_arg_t affinity_arg;
    memset(&affinity_arg, 0, sizeof(affinity_arg_t));
    strncpy(affinity_arg.mac_str, mac_key, sizeof(affinity_arg.mac_str) - 1);
    affinity_arg.vap_index = (unsigned int)vap_index;
    affinity_arg.radio_index = (unsigned int)radio_index;
    affinity_arg.event = 0;  // Not a WiFi management frame event
    affinity_arg.dhcp_event = DHCP_EVENT_UPDATE;
    affinity_arg.dhcp_attempts = dhcp_attempts;
    affinity_arg.dhcp_failures = dhcp_failures;
    
    update_affinity_stats(&affinity_arg, true);
    
    wifi_util_info_print(WIFI_CTRL," DHCP %s:%d Successfully processed %s packet for MAC=%s\n",
        __func__, __LINE__, msg_type_str, mac_key);
}

static int ignite_score_log_timer(void *args)
{
    wifi_ctrl_t *ctrl = (wifi_ctrl_t *)get_wifictrl_obj();
    wifi_app_t *wifi_app = get_app_by_inst(&ctrl->apps_mgr, wifi_app_inst_link_quality);
    if (wifi_app == NULL) {
        wifi_util_error_print(WIFI_APPS, "%s:%d NULL Pointer\n", __func__, __LINE__);
        return RETURN_ERR;
    }
    ignite_lq_state_t *ignite = &wifi_app->data.u.linkquality.ignite;

    char tmp[128] = { 0 };
    char buff[MAX_BUFF_LEN] = { 0 };

    get_formatted_time(tmp);
    snprintf(buff, sizeof(buff), "%s WIFI_IGNITE_LINKQUALITY:%f %f\n", tmp, ignite->last_score,
        ignite->last_threshold);
    wifi_util_info_print(WIFI_APPS, "%s:%d: %s\n", __func__, __LINE__, buff);
    write_to_file(wifi_health_log, buff);
    return RETURN_OK;
}

static void *dhcp_sniffer_thread_func(void *arg)
{
    uint8_t buffer[2048];
    ssize_t len;
    fd_set read_fds;
    struct timeval timeout;
    int ret;

    prctl(PR_SET_NAME, "dhcp_sniffer", 0, 0, 0);
    wifi_util_info_print(WIFI_APPS, "%s:%d DHCP sniffer thread started\n", __func__, __LINE__);

    while (!dhcp_sniffer_exit) {
        FD_ZERO(&read_fds);
        FD_SET(dhcp_sniffer_fd, &read_fds);
        
        // Use select with timeout to allow checking exit flag
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        ret = select(dhcp_sniffer_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (ret < 0) {
            if (errno == EINTR) {
                continue;  // Interrupted, check exit flag and retry
            }
            wifi_util_error_print(WIFI_CTRL, "%s:%d select() failed: %s\n", __func__, __LINE__, strerror(errno));
            break;
        } else if (ret == 0) {
            // Timeout, check exit flag and continue
            continue;
        }
        
        if (FD_ISSET(dhcp_sniffer_fd, &read_fds)) {
            len = recvfrom(dhcp_sniffer_fd, buffer, sizeof(buffer), 0, NULL, NULL);
            
            if (len <= 0) {
                if (len < 0 && errno == EINTR) {
                    continue;
                }
                wifi_util_error_print(WIFI_CTRL, "%s:%d recvfrom() failed or connection closed\n", __func__, __LINE__);
                break;
            }
            
            // ============================================================================
            // EARLY MAC FILTERING: Check if client MAC is associated via wifi_associated_dev_t
            // Parse just enough to extract the client MAC, then filter before full processing
            // ============================================================================
            int dhcp_hdr_sz = sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr);
            
            // Basic packet validation
            if (len >= dhcp_hdr_sz + 28 + 16) {  // Need at least DHCP header with chaddr field
                struct iphdr *ip_header = (struct iphdr *)(buffer + sizeof(struct ethhdr));
                struct udphdr *udp_header = (struct udphdr *)(buffer + sizeof(struct ethhdr) + sizeof(struct iphdr));
                
                // Check if this is a DHCP packet (UDP ports 67 or 68)
                if (ip_header->protocol == IPPROTO_UDP && 
                    (ntohs(udp_header->dest) == 67 || ntohs(udp_header->dest) == 68)) {
                    
                    // Extract client MAC from DHCP chaddr field (offset 28 in DHCP header)
                    struct dhcp_data *dhcp = (struct dhcp_data *)(buffer + dhcp_hdr_sz);
                    char mac_key[18];
                    mac_to_key(dhcp->chaddr, mac_key);
                    
                    // Check if this client is connected using caffinity
                    if (!is_client_connected(mac_key)) {
                        // Client not connected - skip processing entirely
                        wifi_util_dbg_print(WIFI_CTRL, " DHCP %s:%d Client MAC %s NOT connected, SKIPPING packet (early filter)\n", 
                            __func__, __LINE__, mac_key);
                        continue;  // Skip to next packet
                    }
                    
                    wifi_util_dbg_print(WIFI_CTRL, " DHCP %s:%d Client MAC %s is connected, processing packet len=%zd\n", 
                        __func__, __LINE__, mac_key, len);
                }
            }
            
            // If we reach here, either it's a DHCP packet from an associated client,
            // or it's not a DHCP packet (will be filtered out in dhcp_process_packet)
            dhcp_process_packet(buffer, len);
        }
    }

    wifi_util_info_print(WIFI_APPS, "%s:%d DHCP sniffer thread exiting\n", __func__, __LINE__);
    return NULL;
}

static void dhcp_sniffer_start()
{
    struct sockaddr_ll sll;
    struct ifreq ifr;
    pthread_attr_t attr;
    int ret;

    
    wifi_util_error_print(WIFI_CTRL, " SANJI %s:%d DHCP sniffer start\n", __func__, __LINE__);
    
    if (dhcp_sniffer_running) {
        wifi_util_dbg_print(WIFI_CTRL, "%s:%d DHCP sniffer already running\n", __func__, __LINE__);
        return;
    }

    // Create raw socket
    dhcp_sniffer_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (dhcp_sniffer_fd < 0) {
        wifi_util_error_print(WIFI_CTRL, " SANJI %s:%d Failed to create socket: %s\n", __func__, __LINE__, strerror(errno));
        return;
    }

    // Bind to brlan0 interface
    memset(&sll, 0, sizeof(sll));
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "brlan0", IFNAMSIZ - 1);
    
    if (ioctl(dhcp_sniffer_fd, SIOCGIFINDEX, &ifr) < 0) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d Failed to get interface index: %s\n", __func__, __LINE__, strerror(errno));
        close(dhcp_sniffer_fd);
        dhcp_sniffer_fd = -1;
        return;
    }
    
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = ifr.ifr_ifindex;
    sll.sll_protocol = htons(ETH_P_ALL);

    if (bind(dhcp_sniffer_fd, (struct sockaddr*)&sll, sizeof(sll)) < 0) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d Failed to bind socket: %s\n", __func__, __LINE__, strerror(errno));
        close(dhcp_sniffer_fd);
        dhcp_sniffer_fd = -1;
        return;
    }

    // Reset exit flag and create thread
    dhcp_sniffer_exit = 0;
    
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    
    ret = pthread_create(&dhcp_sniffer_thread, &attr, dhcp_sniffer_thread_func, NULL);
    pthread_attr_destroy(&attr);
    
    if (ret != 0) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d Failed to create DHCP sniffer thread: %s\n", __func__, __LINE__, strerror(ret));
        close(dhcp_sniffer_fd);
        dhcp_sniffer_fd = -1;
        return;
    }

    dhcp_sniffer_running = 1;
    wifi_util_info_print(WIFI_CTRL, "%s:%d DHCP sniffer started successfully\n", __func__, __LINE__);
}

static void dhcp_sniffer_stop()
{
    if (!dhcp_sniffer_running) {
        wifi_util_dbg_print(WIFI_CTRL, "%s:%d DHCP sniffer not running\n", __func__, __LINE__);
        return;
    }

    wifi_util_info_print(WIFI_CTRL, "%s:%d Stopping DHCP sniffer\n", __func__, __LINE__);
    
    // Signal thread to exit
    dhcp_sniffer_exit = 1;
    
    // Close the socket to unblock select/recvfrom
    if (dhcp_sniffer_fd >= 0) {
        close(dhcp_sniffer_fd);
        dhcp_sniffer_fd = -1;
    }

    // Wait for thread to finish (joinable thread)
    wifi_util_error_print(WIFI_CTRL, " SANJI_DHCP %s:%d Waiting for sniffer thread to join\n", __func__, __LINE__);
    pthread_join(dhcp_sniffer_thread, NULL);
    wifi_util_error_print(WIFI_CTRL, " SANJI_DHCP %s:%d Sniffer thread joined\n", __func__, __LINE__);

    dhcp_sniffer_running = 0;
    wifi_util_info_print(WIFI_CTRL, "%s:%d DHCP sniffer stopped\n", __func__, __LINE__);
}

void publish_station_score(const char *input_str, double score, double threshold)
{
    char str[MAX_STR_LEN] = { '\0' };
    int current_state = -1;
    bus_error_t status;
    raw_data_t rdata;
    wifi_ctrl_t *ctrl = (wifi_ctrl_t *)get_wifictrl_obj();

    wifi_app_t *wifi_app = get_app_by_inst(&ctrl->apps_mgr, wifi_app_inst_link_quality);
    if (wifi_app == NULL) {
        wifi_util_error_print(WIFI_APPS, "%s:%d NULL Pointer\n", __func__, __LINE__);
        return;
    }
    ignite_lq_state_t *ignite = &wifi_app->data.u.linkquality.ignite;

    wifi_util_info_print(WIFI_APPS, "%s:%d str =%s score =%f threshold =%f\n", __func__, __LINE__,
        input_str, score, threshold);

    ignite->last_score = score;
    ignite->last_threshold = threshold;

    if (threshold != 0.0 && ignite->score_log_timer_id == 0) {
        scheduler_add_timer_task(ctrl->sched, FALSE, &ignite->score_log_timer_id,
            ignite_score_log_timer, NULL, IGNITE_SCORE_LOG_INTERVAL_MS, 0, 0);
        wifi_util_info_print(WIFI_APPS, "%s:%d: Started ignite score log timer (15 min)\n",
            __func__, __LINE__);
    }

    if (ignite->last_service_state == -1) {
        ignite->iteration_count++;
        if (ignite->iteration_count < IGNITE_INITIAL_PUBLISH_ITERATIONS) {
            wifi_util_info_print(WIFI_APPS,
                "%s:%d: Waiting for %dth iteration before first publish, current=%d\n",
                __func__, __LINE__, IGNITE_INITIAL_PUBLISH_ITERATIONS,
                ignite->iteration_count);
            return;
        }
    }

    if (score < threshold) {
        current_state = 0;
        snprintf(str, MAX_STR_LEN, "Non-Serviceable");
    } else if (score >= threshold) {
        current_state = 1;
        snprintf(str, MAX_STR_LEN, "Serviceable");
    }

    if (current_state != -1 && current_state != ignite->last_service_state) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d: ignite status toggled to %s\n", __func__, __LINE__,
            str);
        memset(&rdata, 0, sizeof(raw_data_t));
        rdata.data_type = bus_data_type_string;
        rdata.raw_data.bytes = (void *)str;
        rdata.raw_data_len = (strlen(str) + 1);

        status = get_bus_descriptor()->bus_event_publish_fn(&wifi_app->ctrl->handle,
            WIFI_IGNITE_STATUS, &rdata);
        if (status != bus_error_success) {
            wifi_util_error_print(WIFI_CTRL, "%s:%d: bus: bus_event_publish_fn Event failed %d\n",
                __func__, __LINE__, status);
        }
        if (ignite->last_service_state == -1) {
            char tmp[128] = { 0 };
            char buff[MAX_BUFF_LEN] = { 0 };
            get_formatted_time(tmp);
            snprintf(buff, sizeof(buff), "%s WIFI_IGNITE_LINKQUALITY:%f %f\n", tmp,
                ignite->last_score, ignite->last_threshold);
            wifi_util_info_print(WIFI_APPS, "%s:%d: Score at first RBUS publish after connection: %s\n", __func__,
                __LINE__, buff);
            write_to_file(wifi_health_log, buff);
        }
        ignite->last_service_state = current_state;
    }

    return;
}

int link_quality_register_station(wifi_app_t *apps, wifi_event_t *arg)
{
    wifi_util_info_print(WIFI_APPS, "%s:%d\n", __func__, __LINE__);
    if (!arg) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d NULL arg\n", __func__, __LINE__);
        return RETURN_ERR;
    }

    char *str = (char *)arg;

    wifi_ctrl_t *ctrl = (wifi_ctrl_t *)get_wifictrl_obj();
    if ( ctrl->rf_status_down) {
        register_station_mac(str);
        qmgr_register_score_callback(publish_station_score);
    }
    return RETURN_OK;
}

int link_quality_unregister_station(wifi_app_t *apps, wifi_event_t *arg)
{
    wifi_util_info_print(WIFI_APPS, "%s:%d\n", __func__, __LINE__);
    if (!arg) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d NULL arg\n", __func__, __LINE__);
        return RETURN_ERR;
    }
    char *str = (char *)arg;

    wifi_ctrl_t *ctrl = (wifi_ctrl_t *)get_wifictrl_obj();
    if ( ctrl->rf_status_down) {
        unregister_station_mac(str);
    }

    ignite_lq_state_t *ignite = &apps->data.u.linkquality.ignite;
    if (ignite->score_log_timer_id != 0) {
        scheduler_cancel_timer_task(ctrl->sched, ignite->score_log_timer_id);
        ignite->score_log_timer_id = 0;
        wifi_util_info_print(WIFI_APPS, "%s:%d: Cancelled ignite score log timer\n", __func__,
            __LINE__);
    }
    ignite->last_service_state = -1;
    ignite->iteration_count = 0;

    return RETURN_OK;
}
int update_radio_max_snr_observance(int radio, int max_snr)
{
    wifi_util_info_print(WIFI_APPS, "%s:%d radio=%d and max_snr=%d\n", __func__, __LINE__,radio,max_snr);
    wifi_rfc_dml_parameters_t *rfc_param = (wifi_rfc_dml_parameters_t *)get_ctrl_rfc_parameters();
    if (rfc_param == NULL) {
        wifi_util_error_print(WIFI_CTRL, "Unable to fetch CTRL RFC %s:%d\n", __func__, __LINE__);
        return RETURN_OK;
    }
    switch(radio) {
        case 0:
            if ( max_snr > rfc_param->radio_2g_observed_max_snr) {
                rfc_param->radio_2g_observed_max_snr = max_snr ;
            }
            break;
        case 1:
            if ( max_snr > rfc_param->radio_5g_observed_max_snr) {
                rfc_param->radio_5g_observed_max_snr = max_snr;
            }
            break;
        case 2:
            if ( max_snr > rfc_param->radio_6g_observed_max_snr) {
                rfc_param->radio_6g_observed_max_snr = max_snr;
            }
            break;
        default:
            wifi_util_info_print(WIFI_CTRL,"Not a valid radio\n");

    }
    get_wifidb_obj()->desc.update_rfc_config_fn(0, rfc_param);
    return RETURN_OK;
}
int link_quality_event_exec_start(wifi_app_t *apps, void *arg)
{
      
    wifi_util_info_print(WIFI_APPS, "%s:%d\n", __func__, __LINE__);
    wifi_ctrl_t *ctrl = (wifi_ctrl_t *)get_wifictrl_obj();
    
    // Start DHCP sniffer (no local hashmaps needed - using wifi_associated_dev_t)
    wifi_util_info_print(WIFI_CTRL," %s:%d Starting DHCP sniffer\n", __func__, __LINE__);
    dhcp_sniffer_start();
    
    if ( ctrl->network_mode == rdk_dev_mode_type_em_node
      || ctrl->network_mode == rdk_dev_mode_type_em_colocated_node) {
        qmgr_register_batch_callback(publish_qmgr_subdoc);
        wifi_util_info_print(WIFI_APPS, "%s:%d ctrl->network_mode=%d\n", __func__, __LINE__,ctrl->network_mode);
    } 
    radio_max_snr_t max_snr = {0};
    //qmgr_register_callback(publish_qmgr_subdoc);
    start_link_metrics();
    wifi_rfc_dml_parameters_t *rfc_param = (wifi_rfc_dml_parameters_t *)get_ctrl_rfc_parameters();
    if (rfc_param->link_quality_rfc) {
          wifi_util_error_print(WIFI_CTRL,"%s:%d start link_event \n", __func__, __LINE__);
    }
    if (rfc_param->radio_2g_observed_max_snr == 0 || rfc_param->radio_5g_observed_max_snr == 0|| 
        rfc_param->radio_6g_observed_max_snr == 0) {
        if (rfc_param->radio_2g_observed_max_snr == 0) {
            max_snr.radio_2g_max_snr = 25;
            rfc_param->radio_2g_observed_max_snr = 25;
	} else {
            max_snr.radio_2g_max_snr = rfc_param->radio_2g_observed_max_snr;
	}
        if (rfc_param->radio_5g_observed_max_snr == 0) {
            max_snr.radio_5g_max_snr = 25;
            rfc_param->radio_5g_observed_max_snr = 25;
	} else {
            max_snr.radio_5g_max_snr = rfc_param->radio_5g_observed_max_snr;
	}
        if (rfc_param->radio_6g_observed_max_snr == 0) {
            max_snr.radio_6g_max_snr = 25;
            rfc_param->radio_6g_observed_max_snr = 25;
	} else {
            max_snr.radio_6g_max_snr = rfc_param->radio_6g_observed_max_snr;
	}
        get_wifidb_obj()->desc.update_rfc_config_fn(0, rfc_param);

          wifi_util_error_print(WIFI_CTRL,"%s:%d setting max_snr \n", __func__, __LINE__);
    } else {
	max_snr.radio_2g_max_snr = rfc_param->radio_2g_observed_max_snr;
	max_snr.radio_5g_max_snr = rfc_param->radio_5g_observed_max_snr;
        max_snr.radio_6g_max_snr = rfc_param->radio_6g_observed_max_snr;
        wifi_util_error_print(WIFI_CTRL,"%s:%d setting max_snr \n", __func__, __LINE__);
    }
    
    wifi_util_info_print(WIFI_APPS, "%s:%d %d:%d:%d \n", __func__, __LINE__,
    max_snr.radio_2g_max_snr,max_snr.radio_5g_max_snr,max_snr.radio_6g_max_snr);
    set_max_snr_radios(&max_snr);
    wifi_util_error_print(WIFI_CTRL," SANJI %s:%d calling update_radio_max_snr_observance \n", __func__, __LINE__);
    qmgr_register_max_snr_callback(update_radio_max_snr_observance);
    return RETURN_OK;
}

int link_quality_event_exec_stop(wifi_app_t *apps, void *arg)
{
    wifi_util_info_print(WIFI_APPS, "%s:%d\n", __func__, __LINE__);
    
    // Stop DHCP sniffer
    wifi_util_info_print(WIFI_CTRL," %s:%d Stopping DHCP sniffer\n", __func__, __LINE__);
    dhcp_sniffer_stop();
    
    stop_link_metrics();

    ignite_lq_state_t *ignite = &apps->data.u.linkquality.ignite;
    if (ignite->score_log_timer_id != 0) {
        scheduler_cancel_timer_task(apps->ctrl->sched, ignite->score_log_timer_id);
        ignite->score_log_timer_id = 0;
        wifi_util_info_print(WIFI_APPS, "%s:%d: Cancelled ignite score log timer\n", __func__,
            __LINE__);
    }
    ignite->last_service_state = -1;
    ignite->iteration_count = 0;

    return RETURN_OK;
}

int link_quality_hal_rapid_connect(wifi_app_t *apps, void *arg)
{
    if (!arg) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d NULL arg\n", __func__, __LINE__);
        return RETURN_ERR;
    }

    linkquality_data_t *data = (linkquality_data_t *)arg;
    stats_arg_t *stats = &data->stats;
    wifi_util_error_print(
        WIFI_APPS,
        "%s:%d  mac=%s  snr=%d phy=%d\n",
        __func__, __LINE__,
        stats->mac_str,
        stats->dev.cli_SNR,
        stats->dev.cli_LastDataDownlinkRate
    );

    disconnect_link_stats(stats);
    return RETURN_OK;

}
int link_quality_ignite_reinit_param(wifi_app_t *apps, wifi_event_t *arg)
{
    if (!arg) {
        wifi_util_error_print(WIFI_APPS, "%s:%d NULL arg\n", __func__, __LINE__);
        return RETURN_ERR;
    }
    linkquality_data_t *data = (linkquality_data_t *)arg;
    server_arg_t *args = &data->server_arg;
    reinit_link_metrics(args);
    wifi_util_info_print(WIFI_APPS, "%s:%d sampling = %d reportingl as %d and threshold as %f\n",
        __func__, __LINE__,args->sampling, args->reporting, args->threshold);
    return RETURN_OK;

}
int link_quality_param_reinit(wifi_app_t *apps, wifi_event_t *arg)
{

#ifdef EM_APP
    if (!arg) {
        wifi_util_error_print(WIFI_APPS, "%s:%d NULL arg\n", __func__, __LINE__);
        return RETURN_ERR;
    }

    //linkquality_data_t *data = (linkquality_data_t *)arg;

    em_config_t *em_config;
    wifi_event_t *event = NULL;
    webconfig_subdoc_decoded_data_t *decoded_params = NULL;
    webconfig_subdoc_data_t *doc;

    if (!arg) {
        wifi_util_error_print(WIFI_APPS, "%s:%d NULL Pointer\n", __func__, __LINE__);
        return -1;
    }

    event = arg;
    doc = (webconfig_subdoc_data_t *)event->u.webconfig_data;
    decoded_params = &doc->u.decoded;
    if (decoded_params == NULL) {
        wifi_util_error_print(WIFI_APPS, "%s:%d Decoded data is NULL\n", __func__, __LINE__);
        return RETURN_ERR;
    }

    server_arg_t *server_arg = (server_arg_t *)malloc(sizeof(server_arg_t));
    memset(server_arg,0,sizeof(server_arg_t));
    switch (doc->type) {
        case webconfig_subdoc_type_em_config:
            em_config = &decoded_params->em_config;
            if (em_config == NULL) {
                wifi_util_error_print(WIFI_APPS, "%s:%d NULL pointer \n", __func__, __LINE__);
                return RETURN_ERR;
            }

            wifi_util_info_print(WIFI_APPS, "%s:%d Received config Interval as %d and threshold as %f\n",
                __func__, __LINE__, em_config->alarm_report_policy.reporting_interval,
                em_config->alarm_report_policy.link_quality_threshold);
            
            server_arg->reporting = em_config->alarm_report_policy.reporting_interval;
            server_arg->threshold = em_config->alarm_report_policy.link_quality_threshold;

            wifi_util_info_print(WIFI_APPS, "%s:%d reportingl as %d and threshold as %f\n",
                __func__, __LINE__, server_arg->reporting, server_arg->threshold);

            reinit_link_metrics(server_arg);
            free(server_arg);
            break;

        default:
  
            break;
    }
#endif
    return RETURN_OK;
}

int link_quality_hal_disconnect(wifi_app_t *apps, void *arg)
 {           
    if (!arg) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d NULL arg\n", __func__, __LINE__);
        return RETURN_ERR;
    }

    linkquality_data_t *data = (linkquality_data_t *)arg;
    stats_arg_t *stats = &data->stats;
    wifi_util_error_print( WIFI_CTRL,
         "%s:%d  mac=%s  snr=%d phy=%d\n",
         __func__, __LINE__,
         stats->mac_str,
         stats->dev.cli_SNR,
         stats->dev.cli_LastDataDownlinkRate
    );      
 
    remove_link_stats(stats);
    return RETURN_OK;
             
 } 

int link_quality_ignite_param_reinit(wifi_app_t *apps, wifi_event_t *arg)
{
    if (!arg) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d NULL arg\n", __func__, __LINE__);
        return RETURN_ERR;
    }

    linkquality_data_t *data = (linkquality_data_t *)arg;

     server_arg_t *server_arg = &data->server_arg;
        wifi_util_dbg_print(
            WIFI_APPS,
            "%s:%d  threshold=%f reporting=%d\n",
            __func__, __LINE__,
            server_arg->threshold,
            server_arg->reporting
        );
        reinit_link_metrics(server_arg);

    return RETURN_OK;
}

int link_quality_event_exec_timeout(wifi_app_t *apps, void *arg, int len)
{
    if (!arg) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d NULL arg\n", __func__, __LINE__);
        return RETURN_ERR;
    }

    linkquality_data_t *data = (linkquality_data_t *)arg;

    /* The number of devices is stored in the first element */
    int num_devs = len;

    for (int i = 0; i < num_devs; i++) {

        stats_arg_t *stats = &data[i].stats;
        wifi_util_dbg_print(
            WIFI_APPS,
            "%s:%d idx=%d mac=%s  snr=%d phy=%d\n",
            __func__, __LINE__,
            i,
            stats->mac_str,
            stats->dev.cli_SNR,
            stats->dev.cli_LastDataDownlinkRate,
            stats->vap_index
        );

        add_stats_metrics(stats);
    }
    //dhcp_cleanup_old_entries();
    return RETURN_OK;
}

int link_quality_periodic_stats_update(wifi_app_t *apps, void *arg)
{
    if (!arg) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d NULL arg\n", __func__, __LINE__);
        return RETURN_ERR;
    }

    linkquality_data_t *data = (linkquality_data_t *)arg;
    int num_devs = data[0].size;

    wifi_util_info_print(WIFI_APPS, "%s:%d periodic_stats_update for %d devices\n", 
        __func__, __LINE__, num_devs);

    for (int i = 0; i < num_devs; i++) {
        stats_arg_t *stats = &data[i].stats;
        wifi_util_dbg_print(WIFI_CTRL,
            "%s:%d timestats idx=%d mac=%s snr=%d connected_time=%ld.%09ld disconnected_time=%ld.%09ld\n",
            __func__, __LINE__, i, stats->mac_str, stats->dev.cli_SNR,
            (long)stats->total_connected_time.tv_sec, stats->total_connected_time.tv_nsec,
            (long)stats->total_disconnected_time.tv_sec, stats->total_disconnected_time.tv_nsec);

        periodic_caffinity_stats_update(stats);
    }
    return RETURN_OK;
}

int exec_event_link_quality(wifi_app_t *apps, wifi_event_subtype_t sub_type, void *arg, int len)
{
    switch (sub_type) {
        case wifi_event_exec_start:
            link_quality_event_exec_start(apps, arg);
            break;

        case wifi_event_exec_stop:
            link_quality_event_exec_stop(apps, arg);
            break;

        case wifi_event_exec_timeout:
            link_quality_event_exec_timeout(apps, arg,len);
            link_quality_periodic_stats_update(apps, arg);
            break;
        
        case wifi_event_exec_register_station:
            link_quality_register_station(apps, arg);
            break;
        
        case wifi_event_exec_unregister_station:
            link_quality_unregister_station(apps, arg);
            break;
        
	case wifi_event_exec_link_param_reinit:
            link_quality_ignite_reinit_param(apps, arg);
            break;
        
        
        default:
            wifi_util_error_print(WIFI_APPS, "%s:%d: event not handle %s\r\n", __func__, __LINE__,
            wifi_event_subtype_to_string(sub_type));
            break;
    }
    return RETURN_OK;
}

int exec_event_webconfig_event(wifi_app_t *apps, wifi_event_t *event)
{
    wifi_util_info_print(WIFI_APPS,"Enter %s:%d\n",__func__,__LINE__);
    switch (event->sub_type) {
        case wifi_event_exec_start:
            break;

        case wifi_event_exec_stop:
            break;

        case wifi_event_webconfig_set_data_ovsm:
            link_quality_param_reinit(apps, event);
            break;
        case wifi_event_exec_timeout:
            link_quality_ignite_param_reinit(apps, event);
            break;
        default:
            wifi_util_error_print(WIFI_APPS, "%s:%d: event not handle %s\r\n", __func__, __LINE__,
            wifi_event_subtype_to_string(event->sub_type));
            break;
    }
    return RETURN_OK;
}

int link_quality_apps_auth_event(wifi_app_t *app, bool req,int sub_event,void *arg)
{
    wifi_util_info_print(WIFI_APPS,"Enter %s:%d\n",__func__,__LINE__);
    if (!arg) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d NULL arg\n", __func__, __LINE__);
        return RETURN_ERR;
    }
   //Fill the affinity_arg with frame data 
    affinity_arg_t *affinity_arg = ( affinity_arg_t *) malloc(sizeof( affinity_arg_t));
    if (affinity_arg == NULL) {
        wifi_util_info_print(WIFI_APPS," %s:%d unable to alloc memry\n",__func__,__LINE__);
       return RETURN_ERR;
    }

    memset(affinity_arg, 0, sizeof(affinity_arg_t));
    frame_data_t *msg = (frame_data_t *)arg;
    to_mac_str(msg->frame.sta_mac, affinity_arg->mac_str);
    affinity_arg->vap_index = msg->frame.ap_index;
    affinity_arg->radio_index = getRadioIndexFromAp(msg->frame.ap_index);
    get_radio_channel_utilization(affinity_arg->radio_index,&affinity_arg->channel_utilization);
    affinity_arg->status_code = 0;
    affinity_arg->sig_dbm = msg->frame.sig_dbm;
    // dhcp_event = 0 (not a DHCP update) from memset
    
    if (req)   {
        affinity_arg->event = sub_event;
        // For deauth, fetch total_disconnected_time from sta_data_t
        if (sub_event == wifi_event_hal_deauth_frame) {
            sta_data_t *sta = get_stats_for_sta(affinity_arg->vap_index, msg->frame.sta_mac);
            if (sta) {
                affinity_arg->disconnected_time = sta->total_disconnected_time;
                wifi_util_info_print(WIFI_CTRL, " %s:%d DEAUTH: MAC=%s total_disconnected_time=%ld.%09ld\n",
                    __func__, __LINE__, affinity_arg->mac_str,
                    (long)sta->total_disconnected_time.tv_sec, sta->total_disconnected_time.tv_nsec);
            }
        }
        update_affinity_stats(affinity_arg,true);
    }

    free(affinity_arg);
    return RETURN_OK;
}

int link_quality_apps_assoc_event(wifi_app_t *app, bool req,int sub_event,void *arg)
{
    wifi_util_info_print(WIFI_APPS,"Enter %s:%d sub_event=%d req=%d\n",__func__,__LINE__, sub_event, req);
    if (!arg) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d NULL arg\n", __func__, __LINE__);
        return RETURN_ERR;
    }
   //Fill the affinity_arg with frame data 
    affinity_arg_t *affinity_arg = ( affinity_arg_t *) malloc(sizeof( affinity_arg_t));
    if (affinity_arg == NULL) {
        wifi_util_info_print(WIFI_APPS," %s:%d unable to alloc memry\n",__func__,__LINE__);
       return RETURN_ERR;
    }
    memset(affinity_arg, 0, sizeof(affinity_arg_t));
    frame_data_t *msg = (frame_data_t *)arg;
    to_mac_str(msg->frame.sta_mac, affinity_arg->mac_str);
    affinity_arg->vap_index = msg->frame.ap_index;
    affinity_arg->radio_index = getRadioIndexFromAp(msg->frame.ap_index);
    get_radio_channel_utilization(affinity_arg->radio_index,&affinity_arg->channel_utilization);
    affinity_arg->status_code = 0;
    affinity_arg->sig_dbm = msg->frame.sig_dbm;
    // dhcp_event = 0 (not a DHCP update) from memset

    if (req)   {
        affinity_arg->event = sub_event;
        update_affinity_stats(affinity_arg,true);
    } else {
        
        // Check for wifi_event_hal_assoc_rsp_frame sub_event
        if (sub_event == wifi_event_hal_assoc_rsp_frame) {
            struct ieee80211_mgmt *frame = (struct ieee80211_mgmt *)&msg->data;
            uint16_t status = le_to_host16(frame->u.assoc_resp.status_code);
            wifi_util_info_print(WIFI_CTRL," %s:%d wifi_event_hal_assoc_rsp_frame status_code=%d\n", __func__, __LINE__, status);
            
            // Update caffinity stats via update_affinity_stats with status_code
            affinity_arg->event = sub_event;
            affinity_arg->status_code = status;
            // For connected state (status 0), fetch total_connected_time from sta_data_t
            if (status == 0) {
                sta_data_t *sta = get_stats_for_sta(affinity_arg->vap_index, msg->frame.sta_mac);
                if (sta) {
                    affinity_arg->connected_time = sta->total_connected_time;
                    wifi_util_info_print(WIFI_CTRL, " %s:%d ASSOC RSP success: MAC=%s total_connected_time=%ld.%09ld\n",
                        __func__, __LINE__, affinity_arg->mac_str,
                        (long)sta->total_connected_time.tv_sec, sta->total_connected_time.tv_nsec);
                }
            }
            wifi_util_info_print(WIFI_CTRL, " %s:%d Calling update_affinity_stats for MAC %s, event=%d, status=%d\n",
                __func__, __LINE__, affinity_arg->mac_str, sub_event, status);
            update_affinity_stats(affinity_arg, true);
        }
        
        if (sub_event == wifi_event_hal_reassoc_rsp_frame) {
            struct ieee80211_mgmt *frame = (struct ieee80211_mgmt *)&msg->data;
            uint16_t status = le_to_host16(frame->u.assoc_resp.status_code);
            affinity_arg->event = sub_event;
            affinity_arg->status_code = status;
            // For connected state (status 0), fetch total_connected_time from sta_data_t
            if (status == 0) {
                sta_data_t *sta = get_stats_for_sta(affinity_arg->vap_index, msg->frame.sta_mac);
                if (sta) {
                    affinity_arg->connected_time = sta->total_connected_time;
                    wifi_util_info_print(WIFI_CTRL, " %s:%d REASSOC RSP success: MAC=%s total_connected_time=%ld.%09ld\n",
                        __func__, __LINE__, affinity_arg->mac_str,
                        (long)sta->total_connected_time.tv_sec, sta->total_connected_time.tv_nsec);
                }
            }
            update_affinity_stats(affinity_arg, true);
        }	    
    }
    free(affinity_arg);
    return RETURN_OK;
}
int link_quality_apps_disassoc_event(wifi_app_t *app, bool req,int sub_event,void *arg)
{
    wifi_util_info_print(WIFI_APPS,"Enter %s:%d\n",__func__,__LINE__);
    
    if (!arg) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d NULL arg\n", __func__, __LINE__);
        return RETURN_ERR;
    }
    
    // Get frame data
    frame_data_t *msg = (frame_data_t *)arg;
    
    // Fill the affinity_arg with frame data 
    affinity_arg_t *affinity_arg = (affinity_arg_t *) malloc(sizeof(affinity_arg_t));
    if (affinity_arg == NULL) {
        wifi_util_info_print(WIFI_APPS," %s:%d unable to alloc memory\n",__func__,__LINE__);
        return RETURN_ERR;
    }
    
    memset(affinity_arg, 0, sizeof(affinity_arg_t));
    to_mac_str(msg->frame.sta_mac, affinity_arg->mac_str);
    affinity_arg->vap_index = msg->frame.ap_index;
    affinity_arg->radio_index = getRadioIndexFromAp(msg->frame.ap_index);
    get_radio_channel_utilization(affinity_arg->radio_index, &affinity_arg->channel_utilization);
    // dhcp_event = 0 (not a DHCP update) from memset
    
    if (req) {
        affinity_arg->event = sub_event;
        // For disassoc, fetch total_disconnected_time from sta_data_t
        sta_data_t *sta = get_stats_for_sta(affinity_arg->vap_index, msg->frame.sta_mac);
        if (sta) {
            affinity_arg->disconnected_time = sta->total_disconnected_time;
            wifi_util_info_print(WIFI_CTRL, " %s:%d DISASSOC: MAC=%s total_disconnected_time=%ld.%09ld\n",
                __func__, __LINE__, affinity_arg->mac_str,
                (long)sta->total_disconnected_time.tv_sec, sta->total_disconnected_time.tv_nsec);
        }
        update_affinity_stats(affinity_arg, true);
    }
    
    free(affinity_arg);
    return RETURN_OK;
}

int exec_event_hal_ind(wifi_app_t *apps, wifi_event_subtype_t sub_type, void *arg)
{
    wifi_util_info_print(WIFI_APPS," %s:%d\n",__func__,__LINE__);
    if (!arg) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d NULL arg\n", __func__, __LINE__);
         return RETURN_ERR;
    }
    switch (sub_type) {
        case wifi_event_exec_start:
            break;

        case wifi_event_exec_stop:
            link_quality_hal_disconnect(apps, arg);
            break;

        case wifi_event_exec_timeout:
            link_quality_hal_rapid_connect(apps, arg);
            break;

        case wifi_event_hal_auth_frame:
            wifi_util_info_print(WIFI_APPS," %s:%d event = %d\n",__func__,__LINE__,sub_type);
            link_quality_apps_auth_event(apps,true,sub_type,arg);
            break;
        
        case wifi_event_hal_deauth_frame:
            link_quality_apps_auth_event(apps,true,sub_type,arg);
            wifi_util_info_print(WIFI_APPS," %s:%d event = %d\n",__func__,__LINE__,sub_type);
            break;
     
        case wifi_event_hal_assoc_req_frame:
            wifi_util_info_print(WIFI_APPS," %s:%d event = %d\n",__func__,__LINE__,sub_type);
            link_quality_apps_assoc_event(apps,true,sub_type,arg);
            break;
 
        case wifi_event_hal_assoc_rsp_frame:
            wifi_util_info_print(WIFI_APPS," %s:%d event = %d\n",__func__,__LINE__,sub_type);
            link_quality_apps_assoc_event(apps,false,sub_type,arg);
            break;

        case wifi_event_hal_reassoc_req_frame:
            wifi_util_info_print(WIFI_APPS," %s:%d event = %d\n",__func__,__LINE__,sub_type);
            link_quality_apps_assoc_event(apps,true,sub_type,arg);
            break;
        case wifi_event_hal_reassoc_rsp_frame:
            wifi_util_info_print(WIFI_APPS," %s:%d event = %d\n",__func__,__LINE__,sub_type);
            link_quality_apps_assoc_event(apps,true,sub_type,arg);
            break;
     
        case wifi_event_hal_sta_conn_status:
            //move the func call to here
            wifi_util_info_print(WIFI_APPS," %s:%d event = %d\n",__func__,__LINE__,sub_type);
            //may be here new function has to be used in this case the station has to be moved to connected 
	        link_quality_apps_assoc_event(apps,false,sub_type,arg);
            break;
        case wifi_event_hal_disassoc_device:
            //may be here new function has to be used in this case the station has to be moved to disconnect/removed. 
            wifi_util_info_print(WIFI_APPS," %s:%d event = %d\n",__func__,__LINE__,sub_type);
            link_quality_apps_disassoc_event(apps,true,sub_type,arg);
            break;
        
        default:
            wifi_util_error_print(WIFI_APPS, "%s:%d: event not handle %s\r\n", __func__, __LINE__,
            wifi_event_subtype_to_string(sub_type));
            break;
    }
    return RETURN_OK;
}

int link_quality_event(wifi_app_t *app, wifi_event_t *event)
{
    switch (event->event_type) {
        case wifi_event_type_webconfig:
            exec_event_webconfig_event(app, event);
            break;

        case wifi_event_type_exec:
            exec_event_link_quality(app, event->sub_type, event->u.core_data.msg, event->u.core_data.len);
            break;

        case wifi_event_type_hal_ind:
            exec_event_hal_ind(app, event->sub_type, event->u.core_data.msg);
            break;

        default:
            break;
    }

    return RETURN_OK;
}


int link_quality_init(wifi_app_t *app, unsigned int create_flag)
{
    char *component_name = "WifiLinkReport";
    int num_elements = 0;
    int rc = bus_error_success;

    bus_data_element_t dataElements[] = {
        { WIFI_QUALITY_LINKREPORT, bus_element_type_method,
            { NULL, NULL, NULL, NULL, NULL, NULL }, slow_speed, ZERO_TABLE,
            { bus_data_type_string, false, 0, 0, 0, NULL } } ,
    };

    if (app_init(app, create_flag) != 0) {
        return RETURN_ERR;
    }

    ignite_lq_state_t *ignite = &app->data.u.linkquality.ignite;
    ignite->last_score = 0.0;
    ignite->last_threshold = 0.0;
    ignite->score_log_timer_id = 0;
    ignite->last_service_state = -1;
    ignite->iteration_count = 0;

    rc = get_bus_descriptor()->bus_open_fn(&app->handle, component_name);
    if (rc != bus_error_success) {
        wifi_util_error_print(WIFI_APPS, "%s:%d bus: bus_open_fn open failed for component:%s, rc:%d\n",
            __func__, __LINE__, component_name, rc);
        return RETURN_ERR;
    }
    num_elements = (sizeof(dataElements)/sizeof(bus_data_element_t));
    if (get_bus_descriptor()->bus_reg_data_element_fn(&app->ctrl->handle, dataElements,
        num_elements) != bus_error_success) {
        wifi_util_error_print(WIFI_APPS, "%s:%d: failed to register Linkstats app data elements\n", __func__,
        __LINE__);
        return RETURN_ERR;
    }
    wifi_util_info_print(WIFI_APPS, "%s:%d: Linkstats app data elems registered\n", __func__,__LINE__);
    return RETURN_OK;
}

int link_quality_deinit(wifi_app_t *app)
{
    ignite_lq_state_t *ignite = &app->data.u.linkquality.ignite;
    if (ignite->score_log_timer_id != 0) {
        scheduler_cancel_timer_task(app->ctrl->sched, ignite->score_log_timer_id);
        ignite->score_log_timer_id = 0;
    }
    ignite->last_service_state = -1;
    ignite->iteration_count = 0;
    return RETURN_OK;
}
