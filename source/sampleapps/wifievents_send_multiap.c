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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if_packet.h>
#include <arpa/inet.h>
#include <netinet/ether.h>

#define ETH_P_1905 0x893a

// IEEE 1905.1 multicast MAC address
unsigned char mcast_addr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct ieee1905_header {
    uint8_t version;
    uint16_t message_type;
    uint16_t message_id;
    uint8_t fragment_id;
    uint8_t flags;
} __attribute__((packed));

int get_if_info(const char *ifname, int sock, int *ifindex, unsigned char *mac)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ-1);

    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        perror("SIOCGIFINDEX");
        return -1;
    }
    *ifindex = ifr.ifr_ifindex;

    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) {
        perror("SIOCGIFHWADDR");
        return -1;
    }
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);

    return 0;
}

void send_autoconfig_search(int sock, int ifindex, unsigned char *src_mac)
{
    unsigned char buffer[1500];
    struct ether_header *eth = (struct ether_header *)buffer;

    memcpy(eth->ether_dhost, mcast_addr, 6);
    memcpy(eth->ether_shost, src_mac, 6);
    eth->ether_type = htons(ETH_P_1905);

    struct ieee1905_header *hdr = (struct ieee1905_header *)(buffer + sizeof(struct ether_header));
    hdr->version = 0x00;
    hdr->message_type = htons(0x0000); // Autoconfig Search
    hdr->message_id = htons(rand() & 0xFFFF);
    hdr->fragment_id = 0;
    hdr->flags = 0;

    int frame_len = sizeof(struct ether_header) + sizeof(struct ieee1905_header);

    struct sockaddr_ll addr = {0};
    addr.sll_ifindex = ifindex;
    addr.sll_halen = ETH_ALEN;
    memcpy(addr.sll_addr, mcast_addr, 6);

    if (sendto(sock, buffer, frame_len, 0, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("sendto");
    } else {
        printf("Sent 1905.1 Autoconfig Search\n");
    }
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <interface> <count>\n", argv[0]);
        return 1;
    }

    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_1905));
    if (sock < 0) { perror("socket"); return 1; }

    int ifindex;
    unsigned char mac[6];
    if (get_if_info(argv[1], sock, &ifindex, mac) < 0) {
        close(sock);
        return 1;
    }
    for (int i = 0; i<atoi(argv[2]); i++) 
    {
        printf("Sending 1905.1 Autoconfig Search iter=%d\n",i);
     send_autoconfig_search(sock, ifindex, mac);
    }
        printf("closing socket\n");
    close(sock);
    return 0;
}
