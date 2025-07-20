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
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/filter.h>
#include <netinet/ether.h>
#include <netpacket/packet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <sys/select.h>
#include "wifi_ctrl.h"
#include "wifi_mgr.h"
#include "wifi_stubs.h"
#include "wifi_util.h"
#include "multiap.h"
#define BUF_SIZE 2048
#define MAX_IFACES 6
volatile multiap_state_t state = multiap_state_none;

bool get_al_mac_address(unsigned char *buff,unsigned int len,unsigned char *mac)
 {
    multiap_tlv_t    *tlv;
	unsigned int start =(sizeof(multiap_raw_hdr_t) + sizeof(multiap_cmdu_t));
	unsigned int len1 = len - (sizeof(multiap_raw_hdr_t) + sizeof(multiap_cmdu_t));
    wifi_util_info_print(WIFI_CTRL,"%s:%d\n",__func__,__LINE__); 
    tlv = (multiap_tlv_t *) &buff[start];
	while ((tlv->type != multiap_tlv_type_eom) && (len1 > 0)) {
         if (tlv->type == multiap_tlv_type_al_mac_address) {
		 wifi_util_info_print(WIFI_CTRL,"%s:%d len==%d :%d \n",__func__,__LINE__,tlv->len,htons(tlv->len));
               memcpy(mac, tlv->value, htons(tlv->len));
               return true;
         }
         len1 -= (unsigned int )(sizeof(multiap_tlv_t) + htons(tlv->len));
		 wifi_util_info_print(WIFI_CTRL,"%s:%d len1==%d %d:%d \n",__func__,__LINE__,len1,tlv->len,htons(tlv->len));
         tlv = (multiap_tlv_t *) ((unsigned char *) tlv + sizeof(multiap_tlv_t) + htons(tlv->len));
    }

    return false;
 }


int mac_address_from_name(const char *ifname, mac_address_t mac)
 {
    int sock;
    struct ifreq ifr;

    if ((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP)) < 0) {
         wifi_util_info_print(WIFI_CTRL,"%s:%d: Failed to create socket\n", __func__, __LINE__);
         return -1;
    } 
    memset(&ifr, 0, sizeof(struct ifreq));
    ifr.ifr_addr.sa_family = AF_INET;
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) != 0) {
        close(sock);
		wifi_util_info_print(WIFI_CTRL,"%s:%d: ioctl failed to get hardware address for interface:%s\n", __func__, __LINE__, ifname);
       return -1;
    }

    memcpy(mac, (unsigned char *) ifr.ifr_hwaddr.sa_data, sizeof(mac_address_t));
 
    close(sock);
 
    return 0;
}

int get_service_type()
{
    wifi_util_error_print(WIFI_CTRL,"Enter %s:%d\n",__func__,__LINE__);
	wifi_ctrl_t *ctrl = NULL;
    ctrl = (wifi_ctrl_t *)get_wifictrl_obj();

    if (ctrl->network_mode == rdk_dev_mode_type_gw) {
        wifi_util_error_print(WIFI_CTRL,"Gateway mode  %s:%d\n",__func__,__LINE__);
		return multiap_service_type_gateway;
    } else {
        wifi_util_error_print(WIFI_CTRL,"Extender mode  %s:%d\n",__func__,__LINE__);
		return multiap_service_type_extender;
	}
	
}
int handle_autoconf_search (unsigned char *data, unsigned int len)
{
    unsigned char msg[MAX_EM_BUFF_SZ];
    wifi_util_error_print(WIFI_CTRL,"Enter %s:%d\n",__func__,__LINE__);
    mac_address_t dst;
    char st[64];
   char *ifaces[MAX_IFACES] = { "wl1.1" , "wl1" ,"wl0.1", "wl0", "wl0.7", "wl1.7"};
    get_al_mac_address(data,len, dst); 
   uint8_mac_to_string_mac(dst,st);
     wifi_util_error_print(WIFI_CTRL,"Enter %s:%d sender mac=%s len got =%ld\n",__func__,__LINE__,st,len);
     for (int i = 0; i < MAX_IFACES; ++i) {
        len = create_autoconfig_resp_msg(msg,(unsigned char*)dst,ifaces[i]);
        wifi_util_error_print(WIFI_CTRL,"After create_autoconfig_resp_msg got len = %s:%d :%d\n",__func__,__LINE__,len);
        send_frame(msg, len, false, ifaces[i]);
        wifi_util_error_print(WIFI_CTRL,"Enter %s:%d\n",__func__,__LINE__);
    }
    return 0;
}
int handle_autoconf_search_resp (unsigned char *data, unsigned int len)
{
    wifi_util_error_print(WIFI_CTRL,"Enter %s:%d\n",__func__,__LINE__);
    mac_address_t dst;
	char st[64];
	get_al_mac_address(data,len, dst); 
   uint8_mac_to_string_mac(dst,st);
     wifi_util_error_print(WIFI_CTRL,"Enter %s:%d received al_mac_addr to be whitelisted=%s\n",__func__,__LINE__,st);
     return 0;
}
int create_multiap_brodcast_frame(unsigned char *buff,char *interface_name)
{
    unsigned short  msg_id = multiap_msg_type_autoconf_search;
	static unsigned msg_num = 0;
    int len = 0;
    multiap_cmdu_t *cmdu;
    multiap_tlv_t *tlv;
	multiap_enum_type_t searched, profile;
    unsigned char *tmp = buff;
    unsigned short type = htons(ETH_P_1905);
	char st[64];
    mac_address_t   multi_addr = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    mac_address_t   src_addr;// = {0xE0, 0xDB, 0xD1, 0xDD, 0x08, 0x79};
    multiap_service_type_t service_type = multiap_service_type_gateway;
    unsigned char registrar = 0;
    multiap_freq_band_t freq_band;
    mac_address_from_name(interface_name,src_addr);

   uint8_mac_to_string_mac(src_addr,st);
   wifi_util_info_print(WIFI_CTRL,"st from mac_address_from_name =%s \n",st);
    
	memcpy(tmp, (unsigned char *) multi_addr, sizeof(mac_address_t));
    tmp += sizeof(mac_address_t);
    len += (int) (sizeof(mac_address_t));
 
    memcpy(tmp, (unsigned char *) src_addr, sizeof(mac_address_t));
    tmp += sizeof(mac_address_t);
    len += (int) (sizeof(mac_address_t));

    memcpy(tmp, (unsigned char *) &type, sizeof(unsigned short));
    tmp += sizeof(unsigned short);
    len += (int) (sizeof(unsigned short));
    cmdu = (multiap_cmdu_t *) tmp;
 
    memset(tmp, 0, sizeof(multiap_cmdu_t));
    cmdu->type = htons(msg_id);
    cmdu->id = msg_num;
    msg_num++;
    cmdu->last_frag_ind = 1;
    cmdu->relay_ind = 1;
 
    tmp += sizeof(multiap_cmdu_t);
    len += (int) (sizeof(multiap_cmdu_t));
 
    // AL MAC Address type TLV
    tlv = (multiap_tlv_t *) tmp; 
	tlv->type = multiap_tlv_type_al_mac_address;
    tlv->len = htons(sizeof(mac_address_t));
    memcpy(tlv->value, (unsigned char *) src_addr, sizeof(mac_address_t));

    tmp += (sizeof (multiap_tlv_t) + sizeof(mac_address_t));
    len += (int) (sizeof (multiap_tlv_t) + sizeof(mac_address_t));

    //6-22—SearchedRole TLV
    tlv = (multiap_tlv_t *) (tmp);
    tlv->type = multiap_tlv_type_searched_role;
    tlv->len = htons(sizeof(unsigned char));
    memcpy(&tlv->value, &registrar, sizeof(unsigned char));
    
    tmp += (sizeof (multiap_tlv_t) + 1);
    len += (int) (sizeof (multiap_tlv_t) + 1);

    //6-23—autoconf_freq_band TLV
    freq_band = multiap_freq_band_5;
    tlv = (multiap_tlv_t *) (tmp);
    tlv->type = multiap_tlv_type_autoconf_freq_band;
    tlv->len = htons(sizeof(unsigned char));
    memcpy(&tlv->value, &freq_band, sizeof(unsigned char));

    tmp += (sizeof (multiap_tlv_t) + 1);
    len += (int) (sizeof (multiap_tlv_t) + 1);

    // supported service 17.2.1
	tlv = (multiap_tlv_t *) (tmp);
    tlv->type = multiap_tlv_type_supported_service;
    tlv->len = htons(sizeof(multiap_enum_type_t) + 1);
    tlv->value[0] = 1;
    memcpy(&tlv->value[1], &service_type, sizeof(multiap_enum_type_t));

    tmp += (sizeof(multiap_tlv_t) + sizeof(multiap_enum_type_t) + 1);
    len += (int) (sizeof(multiap_tlv_t) + sizeof(multiap_enum_type_t) + 1);

     // searched service 17.2.2
    tlv = (multiap_tlv_t *) (tmp);
    tlv->type = multiap_tlv_type_searched_service;
    tlv->len = htons(sizeof(multiap_enum_type_t) + 1);
    tlv->value[0] = 1;
    searched = multiap_service_type_gateway;
    memcpy(&tlv->value[1], &searched, sizeof(multiap_enum_type_t));

    tmp += (sizeof(multiap_tlv_t) + sizeof(multiap_enum_type_t) + 1);
    len += (int) (sizeof(multiap_tlv_t) + sizeof(multiap_enum_type_t) + 1);

    // One multiAP profile tlv 17.2.47
    tlv = (multiap_tlv_t *) (tmp);
    tlv->type = multiap_tlv_type_profile;
    tlv->len = htons(sizeof(multiap_enum_type_t));
    profile = 3;
    memcpy(tlv->value, &profile, sizeof(multiap_enum_type_t));

    tmp += (sizeof(multiap_tlv_t) + sizeof(multiap_enum_type_t));
    len += (int) (sizeof(multiap_tlv_t) + sizeof(multiap_enum_type_t));

     // End of message
    tlv = (multiap_tlv_t *) (tmp);
	tlv->type = multiap_tlv_type_eom;
    tlv->len = 0;

    tmp += (sizeof (multiap_tlv_t));
    len += (int) (sizeof (multiap_tlv_t));
     
    return len;

}
int send_frame(unsigned char *buff, unsigned int len, bool multicast,  char *ifname)
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

 void send_multiap_broadcast_message(char *ifname)
{
    unsigned char buff[MAX_BUFF_SZ];
    unsigned int sz;
    wifi_util_info_print(WIFI_CTRL,"%s:%d: ifname = %s\n",__func__, __LINE__,ifname);
	//state = multiap_state_none;
    state = multiap_state_search_rsp_pending;
    sz = create_multiap_brodcast_frame(buff,ifname);
    while(state != multiap_state_completed) {
        if (send_frame(buff, sz, true,ifname)  < 0) {
            wifi_util_info_print(WIFI_CTRL,"%s:%d: failed, err:%d\n", __func__, __LINE__);
            return;
        }
        wifi_util_info_print(WIFI_CTRL,"%s:%d: state in while loop = %d\n",__func__, __LINE__,state);
        sleep(5);
	 }	
        wifi_util_info_print(WIFI_CTRL,"autoconfig_search send successful and state =%d \n",state);
}
int set_bp_filter(int sockfd,const char *iface_name)
 {
      struct packet_mreq mreq;
 #define OP_LDH (BPF_LD  | BPF_H   | BPF_ABS)
  #define OP_LDB (BPF_LD  | BPF_B   | BPF_ABS)
  #define OP_JEQ (BPF_JMP | BPF_JEQ | BPF_K)
  #define OP_RET (BPF_RET | BPF_K)
      static struct sock_filter bpfcode[4] = {
          { OP_LDH, 0, 0, 12          },  // ldh [12]
         { OP_JEQ, 0, 1, ETH_P_1905  },  // jeq #0x893a, L2, L3
         { OP_RET, 0, 0, 0xffffffff,         },  // ret #0xffffffff
          { OP_RET, 0, 0, 0           },  // ret #0x0
      };
     struct sock_fprog bpf = { 4, bpfcode };
 wifi_util_info_print(WIFI_CTRL,"%s:%d: \n", __func__, __LINE__);
      if (setsockopt(sockfd, SOL_SOCKET, SO_ATTACH_FILTER, &bpf, sizeof(bpf))) {
          wifi_util_info_print(WIFI_CTRL,"%s:%d: Error in attaching filter, err:%d\n", __func__, __LINE__, errno);
         close(sockfd);
          return -1;
     }
 
      memset(&mreq, 0, sizeof(mreq));
      mreq.mr_type = PACKET_MR_PROMISC;
      mreq.mr_ifindex = (int)(if_nametoindex(iface_name));
      if (setsockopt(sockfd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, (char *)&mreq, sizeof(mreq))) {
          wifi_util_info_print(WIFI_CTRL,"%s:%d: Error setting promisuous for interface:%s, err:%d\n", __func__, __LINE__,iface_name, errno);
          close(sockfd);
          return -1;
     }
 wifi_util_info_print(WIFI_CTRL,"%s:%d: \n", __func__, __LINE__);

     return 0;
  }

int create_raw_socket(const char *iface_name) {
    int sockfd;
    struct sockaddr_ll sll;
    // Create raw socket
    sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sockfd < 0) {
        wifi_util_info_print(WIFI_CTRL,"socket\n");
        return -1;
    }

    // Bind to interface
    memset(&sll, 0, sizeof(sll));
    sll.sll_family   = AF_PACKET;
    sll.sll_ifindex  = (int)(if_nametoindex(iface_name));
    sll.sll_protocol = htons(ETH_P_ALL);
    if (bind(sockfd, (struct sockaddr *)&sll, sizeof(sll)) == -1) {
        wifi_util_info_print(WIFI_CTRL,"bind error\n");
        close(sockfd);
        return -1;
    }
	set_bp_filter(sockfd,iface_name);

    return sockfd;
}

int create_autoconfig_resp_msg(unsigned char *buff, unsigned char *dst, char *interface_name)
 {
    unsigned short  msg_id = multiap_msg_type_autoconf_resp;
    int len = 0;
    multiap_cmdu_t *cmdu;
    multiap_tlv_t *tlv;
    multiap_enum_type_t profile;
    multiap_ctrl_cap_t   ctrl_cap;
    multiap_ieee_1905_security_cap_t sec_info_cap;
    unsigned char *tmp = buff;
	unsigned char src_addr[64],sta_addr[64];
	char *sta_name = "wl1";
	char st[64];
    unsigned short type = htons(ETH_P_1905);
    multiap_service_type_t   service_type = get_service_type();
    unsigned char registrar = 0;
	multiap_freq_band_t band = multiap_freq_band_5; 
    mac_address_from_name(interface_name,src_addr);

   uint8_mac_to_string_mac(src_addr,st);
   wifi_util_info_print(WIFI_CTRL,"string from mac_address_from_name %s =%s \n",interface_name,st);

    mac_address_from_name(sta_name,sta_addr);

   uint8_mac_to_string_mac(sta_addr,st);
   wifi_util_info_print(WIFI_CTRL,"string from mac_address_from_name of wl1=%s \n",st);
   uint8_mac_to_string_mac(dst,st);
   wifi_util_info_print(WIFI_CTRL,"string from mac_address_from_name of dest==%s \n",st);
    memcpy(tmp, (unsigned char *)dst, sizeof(mac_address_t));
	tmp += sizeof(mac_address_t);

    len += (int) (sizeof(mac_address_t));

    memcpy(tmp, (unsigned char *) src_addr, sizeof(mac_address_t));
    tmp += sizeof(mac_address_t);
    len += (int) (sizeof(mac_address_t));

    memcpy(tmp, (unsigned char *) (&type), sizeof(unsigned short));
    tmp += sizeof(unsigned short);
    len += (int) (sizeof(unsigned short));
    cmdu = (multiap_cmdu_t *) (tmp);

    memset(tmp, 0, sizeof(multiap_cmdu_t));
    cmdu->type = htons(msg_id);
	cmdu->id = msg_id;
	msg_id++;
	cmdu->last_frag_ind = 1;
    cmdu->relay_ind = 1;
	
	tmp += sizeof(multiap_cmdu_t);
    len += (int) (sizeof(multiap_cmdu_t));

    //6-24—SupportedRole TLV
    tlv = (multiap_tlv_t *) (tmp);
    tlv->type = multiap_tlv_type_supported_role;
    tlv->len = htons(sizeof(unsigned char));
    memcpy(&tlv->value, &registrar, sizeof(unsigned char));

     tmp += (sizeof (multiap_tlv_t) + 1);
     len += (int) (sizeof (multiap_tlv_t) + 1);

     //6-25—supported freq_band TLV
     tlv = (multiap_tlv_t *) (tmp);
     tlv->type = multiap_tlv_type_supported_freq_band;
     tlv->len = htons(sizeof(unsigned char));
     memcpy(&tlv->value, &band, sizeof(unsigned char));

     tmp += (sizeof (multiap_tlv_t) + 1);
     len += (int) (sizeof (multiap_tlv_t) + 1);

     // supported service tlv 17.2.1
    tlv = (multiap_tlv_t *) (tmp);
    tlv->type = multiap_tlv_type_supported_service;
    tlv->len = htons(sizeof(multiap_enum_type_t) + 1);
    tlv->value[0] = 1;
    memcpy(&tlv->value[1], &service_type, sizeof(multiap_enum_type_t));

    tmp += (sizeof(multiap_tlv_t) + sizeof(multiap_enum_type_t) + 1);
    len += (int) (sizeof(multiap_tlv_t) + sizeof(multiap_enum_type_t) + 1);

    // 1905 layer security capability tlv 17.2.67
	sec_info_cap.onboarding_proto = 0;
	sec_info_cap.integrity_algo  = 1;
	sec_info_cap.encryption_algo = 0;
    tlv = (multiap_tlv_t *) (tmp);
    tlv->type = multiap_tlv_type_1905_layer_security_cap;
     tlv->len = htons(sizeof(multiap_ieee_1905_security_cap_t));
     memcpy(tlv->value, &sec_info_cap, sizeof(multiap_ieee_1905_security_cap_t));

     tmp += (sizeof(multiap_tlv_t) + sizeof(multiap_ieee_1905_security_cap_t));
     len += (int) (sizeof(multiap_tlv_t) + sizeof(multiap_ieee_1905_security_cap_t));

     // One multiAP profile tlv 17.2.47
    tlv = (multiap_tlv_t *) (tmp);
    tlv->type = multiap_tlv_type_profile;
    tlv->len = htons(sizeof(multiap_enum_type_t));
    profile = multiap_profile_type_3;
    memcpy(tlv->value, &profile, sizeof(multiap_enum_type_t));

    tmp += (sizeof(multiap_tlv_t) + sizeof(multiap_enum_type_t));
    len += (int) (sizeof(multiap_tlv_t) + sizeof(multiap_enum_type_t));

     // One controller capability tlv 17.2.94
	 tlv = (multiap_tlv_t *) (tmp);
     tlv->type = multiap_tlv_type_ctrl_cap;
     tlv->len = htons(sizeof(multiap_ctrl_cap_t));
     memset(&ctrl_cap, 0, sizeof(multiap_ctrl_cap_t));;
     memcpy(tlv->value, &ctrl_cap, sizeof(multiap_ctrl_cap_t));

     tmp += (sizeof(multiap_tlv_t) + sizeof(multiap_ctrl_cap_t));
     len += (int) (sizeof(multiap_tlv_t) + sizeof(multiap_ctrl_cap_t));

 // AL MAC Address type TLV
    tlv = (multiap_tlv_t *) tmp;
    tlv->type = multiap_tlv_type_al_mac_address;
    tlv->len = htons(sizeof(mac_address_t));
    memcpy(tlv->value, (unsigned char *) sta_addr, sizeof(mac_address_t));

    tmp += (sizeof (multiap_tlv_t) + sizeof(mac_address_t));
    len += (int) (sizeof (multiap_tlv_t) + sizeof(mac_address_t));
// End of message
    tlv = (multiap_tlv_t *) (tmp);
    tlv->type = multiap_tlv_type_eom;
    tlv->len = 0;

    tmp += (sizeof (multiap_tlv_t));
    len += (int) (sizeof (multiap_tlv_t));
    wifi_util_info_print(WIFI_CTRL,"len = %d in fun=%s\n",len,__func__); 
    return len;

}

void proto_process(unsigned char *data, unsigned int len)
{
    multiap_cmdu_t *cmdu;
    multiap_raw_hdr_t *hdr = (multiap_raw_hdr_t *)(data);
    cmdu = (multiap_cmdu_t *)(data + sizeof(multiap_raw_hdr_t));

    if (memcmp(hdr->src, hdr->dst, sizeof(mac_address_t)) == 0){
  
        wifi_util_info_print(WIFI_CTRL, "%s:%d :Failed to initialize socket on\n", __func__,__LINE__);
        // This is a message that was sent to the same address it was sent fro
		return;
    }
    wifi_util_info_print(WIFI_CTRL, "%s:%d :Got a valid packet of type =%d\n", __func__,__LINE__,htons(cmdu->type));
    switch (htons(cmdu->type)) {
          case multiap_msg_type_autoconf_search:
		   if (state == multiap_state_none) {
              wifi_util_info_print(WIFI_CTRL, "%s:%d :Got a valid packet of type =%d\n", __func__,__LINE__,htons(cmdu->type));
              wifi_util_info_print(WIFI_CTRL, "Got autoconfig search\n");
			  state = multiap_state_completed;
		      handle_autoconf_search(data,len);
			}
		   break;
		   case multiap_msg_type_autoconf_resp:
           if (state == multiap_state_search_rsp_pending) {
               wifi_util_info_print(WIFI_CTRL, "%s:%d :Got a valid packet of type =%d\n", __func__,__LINE__,htons(cmdu->type));
              state =  multiap_state_completed;
              wifi_util_info_print(WIFI_CTRL, "Got autoconfig search response\n");
              handle_autoconf_search_resp(data,len);
            }
            break;
		   default:
           wifi_util_info_print(WIFI_CTRL, "Got different  package\n");
		   break;
   }
}
 static void *receive_multicast_message(void *ctx)
 {
    const char *ifaces[MAX_IFACES] = { "wl1.1" , "wl1" ,"wl0.1", "wl0", "wl0.7", "wl1.7"};
    int sockets[MAX_IFACES];
    char buffer[BUF_SIZE];
    state = multiap_state_none;
     // struct timeval tv;
    // Create and bind sockets for both interfaces
    for (int i = 0; i < MAX_IFACES; ++i) {
        sockets[i] = create_raw_socket(ifaces[i]);
        if (sockets[i] < 0) {
             wifi_util_info_print(WIFI_CTRL, "Failed to initialize socket on %s\n", ifaces[i]);
            return NULL;
        }
		 wifi_util_info_print(WIFI_CTRL,"%s:%d sockets[i]= %d\n", __func__, __LINE__,sockets[i]);
    }

    wifi_util_info_print(WIFI_CTRL,"Listening for 1905 packets on %s and %s using select()...\n",
           ifaces[0], ifaces[1]);

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
		  //tv.tv_sec = 0;
        //tv.tv_usec = 100;
		 if(state == multiap_state_completed)
		 {
		     wifi_util_info_print(WIFI_CTRL,"%s:%d multiap_state_completed\n", __func__, __LINE__);
			 return NULL;
		 }

        int maxfd = -1;
        for (int i = 0; i < MAX_IFACES; i++) {
            FD_SET(sockets[i], &readfds);
            if (sockets[i] > maxfd) maxfd = sockets[i];
		 wifi_util_info_print(WIFI_CTRL,"%s:%d sockets[i]= %d maxfd = %d\n", __func__, __LINE__,sockets[i],maxfd);
        }

		 wifi_util_info_print(WIFI_CTRL,"%s:%d maxfd = %d\n", __func__, __LINE__,maxfd);
        int ret = select(maxfd + 1, &readfds, NULL, NULL,NULL);
        if (ret < 0) {
            wifi_util_info_print(WIFI_CTRL,"select error");
            break;
        }

        for (int i = 0; i < MAX_IFACES; ++i) {
            if (FD_ISSET(sockets[i], &readfds)) {
                ssize_t len = recvfrom(sockets[i], buffer, BUF_SIZE, 0, NULL, NULL);
                if (len < 0) {
                    wifi_util_info_print(WIFI_CTRL,"recvfrom \n");
                    continue;
                }

                wifi_util_info_print(WIFI_CTRL,"Received 1905 packet of %d bytes on interface %s\n",
                       len, ifaces[i]);
					   proto_process((unsigned char *)buffer,len);

            }
        }
		 //FD_ZERO(&readfds);
    }
    return NULL;
}

void get_1905_status( int status)
{
    wifi_util_info_print(WIFI_CTRL,"%s:%d \n",__func__,__LINE__);
    status = (int)state; 
    wifi_util_info_print(WIFI_CTRL,"%s:%d status=%d and state=%d\n",__func__,__LINE__,status,state);
}
void set_1905_status(int  status)
{
    wifi_util_info_print(WIFI_CTRL,"%s:%d \n",__func__,__LINE__);
    state = (multiap_state_t)status; 
    wifi_util_info_print(WIFI_CTRL,"%s:%d status=%d and state=%d\n",__func__,__LINE__,status,state);
}
 void receive_multiap_message()
{
    wifi_util_info_print(WIFI_CTRL,"receive message \n");
	int ret;
    pthread_attr_t attr;
     pthread_t  thread_id;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    ret = pthread_create(&thread_id, &attr,receive_multicast_message, NULL);
	if (ret != 0){
    wifi_util_info_print(WIFI_CTRL,"thread was not created successfully \n");
	}
	else 
      wifi_util_info_print(WIFI_CTRL,"thread was created successfully \n");


}
