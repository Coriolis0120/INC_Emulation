#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>

uint32_t get_ip(const char *ip_str) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) <= 0) {
        fprintf(stderr, "Invalid IP address: %s\n", ip_str);
        return 0;
    }
    return addr.s_addr;
}

// 打印 MAC 地址
void print_mac(int id, const char *prefix, const uint8_t mac[6]) {
    printf("%s%02X:%02X:%02X:%02X:%02X:%02X\n", prefix,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// 打印 IP 地址
void print_ip(int id, const char *prefix, uint32_t ip) {
    struct in_addr addr;
    addr.s_addr = ip;
    printf("%s%s\n", prefix, inet_ntoa(addr));
}

// 打印以太网头
void print_eth_header(int id, const eth_header_t *eth) {
    printf("==== Ethernet Header ====\n");
    print_mac(id, "  Destination MAC: ", eth->dst_mac);
    print_mac(id, "  Source MAC:      ", eth->src_mac);
    printf("  EtherType:       0x%04X\n", ntohs(eth->ether_type));
}

// 打印 IPv4 头
void print_ipv4_header(int id, const ipv4_header_t *ip) {
    printf("==== IPv4 Header ====\n");
    printf("  Version:         %u\n", ip->version_ihl >> 4);
    printf("  IHL:             %u (words)\n", ip->version_ihl & 0x0F);
    printf("  Total Length:    %u bytes\n", ntohs(ip->total_length));
    printf("  TTL:             %u\n", ip->ttl);
    printf("  Protocol:        %u (UDP=17)\n", ip->protocol);
    print_ip(id, "  Source IP:       ", ip->src_ip);
    print_ip(id, "  Destination IP:  ", ip->dst_ip);
}

// 打印 UDP 头
void print_udp_header(int id, const udp_header_t *udp) {
    printf("==== UDP Header ====\n");
    printf("  Source Port:     %u\n", ntohs(udp->src_port));
    printf("  Destination Port:%u\n", ntohs(udp->dst_port));
    printf("  Length:          %u bytes\n", ntohs(udp->length));
}

// 打印 RRoCE BTH 头
void print_bth_header(int id, const bth_header_t *bth) {
    printf("==== RRoCE BTH Header ====\n");
    printf("  Opcode:          0x%02X\n", bth->opcode);
    printf("  QPN:             %u\n", ntohl(bth->qpn) & 0x00FFFFFF);
    printf("  PSN:             %u\n", ntohl(bth->apsn) & 0x00FFFFFF);
}

void print_connection(int id, const connection_t *conn) {
    printf("==== Connection Info ====\n");
    printf("  Device:           %s\n", conn->device);
    print_mac(id, "  My MAC:           ", conn->my_mac);
    print_mac(id, "  Peer MAC:         ", conn->peer_mac);
    print_ip(id, "  My IP:            ", conn->my_ip);
    print_ip(id, "  Peer IP:          ", conn->peer_ip);
    printf("  My Port:          %u\n", conn->my_port);
    printf("  Peer Port:        %u\n", conn->peer_port);
    printf("  My QP:            %u\n", conn->my_qp);
    printf("  Peer QP:          %u\n", conn->peer_qp);
    printf("  PSN:              %u\n", conn->psn);
}

uint16_t ipv4_checksum(const ipv4_header_t *ip) {
    uint32_t sum = 0;
    const uint16_t *ptr = (const uint16_t *)ip;
    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;
    uint16_t saved_checksum = ip->checksum;
    ((ipv4_header_t *)ip)->checksum = 0;

    for (size_t i = 0; i < ihl / 2; i++) {
        sum += ntohs(ptr[i]);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    ((ipv4_header_t *)ip)->checksum = saved_checksum;
    return htons((uint16_t)(~sum));
}

int is_ipv4_checksum_valid(const ipv4_header_t *ip) {
    uint16_t computed = ipv4_checksum(ip);
    return (computed == ntohs(ip->checksum));
}

#define POLY 0xEDB88320
uint32_t crc32_table[8][256];

void init_crc32_table() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (crc & 1 ? POLY : 0);
        }
        crc32_table[0][i] = crc;
    }

    for (int t = 1; t < 8; t++) {
        for (int i = 0; i < 256; i++) {
            crc32_table[t][i] = (crc32_table[t - 1][i] >> 8) ^ crc32_table[0][crc32_table[t - 1][i] & 0xFF];
        }
    }
}

uint32_t crc32(const void *data, size_t length) {
    const uint8_t *buf = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;

    while (length >= 8) {
        uint8_t b0 = buf[0] ^ (crc & 0xFF);
        uint8_t b1 = buf[1] ^ ((crc >> 8) & 0xFF);
        uint8_t b2 = buf[2] ^ ((crc >> 16) & 0xFF);
        uint8_t b3 = buf[3] ^ ((crc >> 24) & 0xFF);
        uint8_t b4 = buf[4];
        uint8_t b5 = buf[5];
        uint8_t b6 = buf[6];
        uint8_t b7 = buf[7];

        crc = crc32_table[0][b7] ^ crc32_table[1][b6] ^ crc32_table[2][b5] ^
              crc32_table[3][b4] ^ crc32_table[4][b3] ^ crc32_table[5][b2] ^
              crc32_table[6][b1] ^ crc32_table[7][b0];

        buf += 8;
        length -= 8;
    }

    while (length--) {
        crc = (crc >> 8) ^ crc32_table[0][(crc ^ *buf++) & 0xFF];
    }

    return crc ^ 0xFFFFFFFF;
}

uint32_t compute_icrc(int id, const char* eth_packet) {
    ipv4_header_t* iip = (ipv4_header_t*)(eth_packet + sizeof(eth_header_t));
    int len = ntohs(iip->total_length) - 4;

    char pack[5555];
    memcpy(pack + 8, eth_packet + sizeof(eth_header_t), len);
    len += 8;
    for(int i = 0; i < 8; i++) {
        pack[i] = 0xFF;
    }

    ipv4_header_t* ip = (ipv4_header_t*)(pack + 8);
    udp_header_t* udp = (udp_header_t*)(pack + 8 + sizeof(ipv4_header_t));
    bth_header_t* bth = (bth_header_t*)(pack + 8 + sizeof(ipv4_header_t) + sizeof(udp_header_t));

    ip->tos = ip->tos | 0xFF;
    ip->ttl = ip->ttl | 0xFF;
    ip->checksum = ip->checksum | 0xFFFF;
    udp->checksum = udp->checksum | 0xFFFF;
    bth->qpn = bth->qpn | 0x000000FF;

    return crc32(pack, len);
}

uint64_t get_now_ts() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

char* uint32_to_ip_string_big_endian(uint32_t value) {
    static char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d.%d.%d.%d",
        value & 0xFF,
        (value >> 8) & 0xFF,
        (value >> 16) & 0xFF,
        (value >> 24) & 0xFF);
    return buffer;
}
