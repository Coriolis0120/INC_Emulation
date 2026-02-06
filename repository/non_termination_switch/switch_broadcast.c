/**
 * @file switch_broadcast.c
 * @brief INC 交换机 Broadcast 模式实现
 *
 * 实现功能：
 * - 连接控制器获取 YAML 配置
 * - 基于 pcap 的数据包接收（设备级共享）
 * - Broadcast 数据转发（非聚合）
 * - ACK 流控
 *
 * Broadcast 数据流：
 * - Root Host -> Leaf Switch -> Spine Switch -> 所有 Leaf Switch -> 所有 Host
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <pcap.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/epoll.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include "switch_context.h"
#include "controller_comm.h"
#include "parameter.h"
#include "util.h"
#include "opcode.h"

// ==================== 宏定义 ====================
#define Idx(psn) ((psn) % SWITCH_ARRAY_LENGTH)
#define MASK_CONN(port) (0x1<<(port))

// 日志宏 - 带时间戳
#define LOG(fmt, ...) do { \
    struct timeval _tv; \
    gettimeofday(&_tv, NULL); \
    printf("[%ld.%06ld][Switch %d] " fmt, _tv.tv_sec % 100000, _tv.tv_usec, ctx.switch_id, ##__VA_ARGS__); \
    fflush(stdout); \
} while(0)

// 详细日志宏 - 用于高频操作
#define LOG_DETAIL(fmt, ...) do { \
    struct timeval _tv; \
    gettimeofday(&_tv, NULL); \
    printf("[%ld.%06ld][Switch %d][DETAIL] " fmt, _tv.tv_sec % 100000, _tv.tv_usec, ctx.switch_id, ##__VA_ARGS__); \
    fflush(stdout); \
} while(0)

// ==================== 全局统计变量 ====================
static uint64_t g_total_recv_packets = 0;
static uint64_t g_total_data_packets = 0;
static uint64_t g_total_ack_packets = 0;
static uint64_t g_total_broadcasts = 0;
static uint64_t g_total_downstream = 0;
static uint64_t g_total_upstream = 0;
static uint64_t g_total_ack_sent = 0;
static uint64_t g_max_psn_seen = 0;
static uint64_t g_min_psn_seen = UINT64_MAX;

// ==================== 全局上下文 ====================
static switch_context_t ctx;
static packet_metadata_t meta;
static uint64_t g_last_stats_time = 0;

// 打印统计摘要
static void print_stats_summary(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t now = tv.tv_sec * 1000000 + tv.tv_usec;

    // 每秒打印一次统计
    if (now - g_last_stats_time < 1000000) return;
    g_last_stats_time = now;

    printf("\n========== STATS SUMMARY ==========\n");
    printf("  total_recv_packets:  %lu\n", g_total_recv_packets);
    printf("  total_data_packets:  %lu\n", g_total_data_packets);
    printf("  total_ack_packets:   %lu\n", g_total_ack_packets);
    printf("  total_broadcasts:    %lu\n", g_total_broadcasts);
    printf("  total_downstream:    %lu\n", g_total_downstream);
    printf("  total_upstream:      %lu\n", g_total_upstream);
    printf("  total_ack_sent:      %lu\n", g_total_ack_sent);
    printf("  PSN range: [%lu, %lu]\n", g_min_psn_seen, g_max_psn_seen);

    // pcap 丢包统计
    printf("--- PCAP Drop Stats ---\n");
    for(int i = 0; i < ctx.fan_in; i++) {
        if(ctx.conns[i].handle) {
            struct pcap_stat ps;
            if(pcap_stats(ctx.conns[i].handle, &ps) == 0) {
                printf("  port[%d]: recv=%u, drop=%u, ifdrop=%u\n",
                       i, ps.ps_recv, ps.ps_drop, ps.ps_ifdrop);
            }
        }
    }
    printf("===================================\n\n");
    fflush(stdout);
}

// 设备句柄结构：每个唯一设备一个 pcap 句柄
typedef struct {
    char device[64];
    pcap_t *handle;
    int fd;
    int first_conn;
} device_handle_t;

static device_handle_t g_device_handles[MAX_CONNECTIONS_NUM];
static int g_num_devices = 0;

// 根据源 IP 查找对应的连接 ID
static int find_conn_by_src_ip(uint32_t src_ip) {
    for (int i = 0; i < ctx.fan_in; i++) {
        if (ctx.conns[i].peer_ip == src_ip) {
            return i;
        }
    }
    return -1;
}

static void init_devices(void) {
    LOG("init_devices: Starting, fan_in=%d\n", ctx.fan_in);

    g_num_devices = 0;
    memset(g_device_handles, 0, sizeof(g_device_handles));

    // 第一遍：为每个唯一设备创建一个 pcap 句柄
    for(int i = 0; i < ctx.fan_in; i++) {
        const char *dev_name = ctx.conns[i].device;

        // 检查是否已经为该设备创建了句柄
        int existing_dev = -1;
        for (int j = 0; j < g_num_devices; j++) {
            if (strcmp(g_device_handles[j].device, dev_name) == 0) {
                existing_dev = j;
                break;
            }
        }

        if (existing_dev >= 0) {
            ctx.conns[i].handle = g_device_handles[existing_dev].handle;
            LOG("init_devices: port %d reuses device %s\n", i, dev_name);
            continue;
        }

        LOG("init_devices: Creating new handle for device %s (port %d)\n", dev_name, i);

        char errbuf[PCAP_ERRBUF_SIZE];
        pcap_t *handle = pcap_create(dev_name, errbuf);
        if (handle == NULL) {
            fprintf(stderr, "Could not create pcap for device: %s, err: %s\n", dev_name, errbuf);
            continue;
        }

        pcap_set_snaplen(handle, BUFSIZ);
        pcap_set_promisc(handle, 1);
        pcap_set_timeout(handle, 1);
        pcap_set_immediate_mode(handle, 1);
        pcap_set_buffer_size(handle, 512 * 1024 * 1024);

        if (pcap_activate(handle) != 0) {
            fprintf(stderr, "pcap_activate failed for %s: %s\n", dev_name, pcap_geterr(handle));
            pcap_close(handle);
            continue;
        }

        pcap_setnonblock(handle, 1, errbuf);

        // 构建过滤器
        char filter_exp[512] = "";
        int filter_len = 0;
        int port_num = ctx.conns[i].my_port;

        filter_len += snprintf(filter_exp + filter_len, sizeof(filter_exp) - filter_len,
                               "udp port %d and (", port_num);

        int first_ip = 1;
        for (int j = i; j < ctx.fan_in; j++) {
            if (strcmp(ctx.conns[j].device, dev_name) == 0) {
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &ctx.conns[j].peer_ip, ip_str, sizeof(ip_str));

                if (!first_ip) {
                    filter_len += snprintf(filter_exp + filter_len, sizeof(filter_exp) - filter_len, " or ");
                }
                filter_len += snprintf(filter_exp + filter_len, sizeof(filter_exp) - filter_len,
                                       "src host %s", ip_str);
                first_ip = 0;
            }
        }
        filter_len += snprintf(filter_exp + filter_len, sizeof(filter_exp) - filter_len, ")");

        LOG("init_devices: device %s filter: %s\n", dev_name, filter_exp);

        struct bpf_program fp;
        if (pcap_compile(handle, &fp, filter_exp, 0, PCAP_NETMASK_UNKNOWN) == -1) {
            fprintf(stderr, "Filter compile error for %s: %s\n", dev_name, pcap_geterr(handle));
            pcap_close(handle);
            continue;
        }

        if (pcap_setfilter(handle, &fp) == -1) {
            fprintf(stderr, "Set filter error for %s: %s\n", dev_name, pcap_geterr(handle));
            pcap_freecode(&fp);
            pcap_close(handle);
            continue;
        }
        pcap_freecode(&fp);

        int fd = pcap_get_selectable_fd(handle);
        if (fd == -1) {
            fprintf(stderr, "Cannot get selectable fd for %s\n", dev_name);
            pcap_close(handle);
            continue;
        }

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.u32 = g_num_devices;

        if (epoll_ctl(ctx.epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
            perror("epoll_ctl failed");
            pcap_close(handle);
            continue;
        }

        strncpy(g_device_handles[g_num_devices].device, dev_name, sizeof(g_device_handles[g_num_devices].device) - 1);
        g_device_handles[g_num_devices].handle = handle;
        g_device_handles[g_num_devices].fd = fd;
        g_device_handles[g_num_devices].first_conn = i;

        ctx.conns[i].handle = handle;

        LOG("init_devices: device %s: epoll added, fd=%d, dev_idx=%d\n", dev_name, fd, g_num_devices);
        g_num_devices++;
    }

    // 第二遍：确保所有连接都有句柄
    for (int i = 0; i < ctx.fan_in; i++) {
        if (ctx.conns[i].handle == NULL) {
            for (int j = 0; j < g_num_devices; j++) {
                if (strcmp(g_device_handles[j].device, ctx.conns[i].device) == 0) {
                    ctx.conns[i].handle = g_device_handles[j].handle;
                    break;
                }
            }
        }
    }

    LOG("init_devices: Completed, %d unique devices for %d connections\n", g_num_devices, ctx.fan_in);
}

// 发送 ACK 给指定连接
static void send_roce_ack(uint32_t conn_id, uint32_t psn){
    connection_t *conn = &ctx.conns[conn_id];
    g_total_ack_sent++;
    LOG_DETAIL("send_roce_ack: conn=%d, PSN=%u\n", conn_id, psn);

    int size = 62;
    uint8_t ack_pkt[62];
    memset(ack_pkt, 0, sizeof(ack_pkt));

    eth_header_t *eth = (eth_header_t *)ack_pkt;
    memcpy(eth->dst_mac, conn->peer_mac, 6);
    memcpy(eth->src_mac, conn->my_mac, 6);
    eth->ether_type = htons(0x0800);

    ipv4_header_t *ip = (ipv4_header_t *)(ack_pkt + 14);
    ip->version_ihl = 0x45;
    ip->tos = 0x00;
    ip->total_length = htons(48);
    ip->id = 0x1111;
    ip->flags_frag_off = htons(0x4000);
    ip->ttl = 0x40;
    ip->protocol = 0x11;
    ip->src_ip = conn->my_ip;
    ip->dst_ip = conn->peer_ip;
    ip->checksum = ipv4_checksum(ip);

    udp_header_t *udp = (udp_header_t *)(ack_pkt + 34);
    udp->src_port = htons(conn->my_port);
    udp->dst_port = htons(conn->peer_port);
    udp->length = htons(28);
    udp->checksum = 0;

    bth_header_t *bth = (bth_header_t *)(ack_pkt + 42);
    bth->opcode = RC_ACKNOWLEDGE;
    bth->se_m_pad = 0x00;
    bth->pkey = 0xffff;
    bth->qpn = htonl(conn->peer_qp & 0x00FFFFFF);
    bth->apsn = htonl(psn & 0x00FFFFFF);

    aeth_header_t *aeth = (aeth_header_t *)(ack_pkt + 54);
    aeth->syn_msn = htonl(psn | 0x1f000000);

    uint32_t *icrc = (uint32_t *)(ack_pkt + 58);
    *icrc = compute_icrc(-1, (const char *)ack_pkt);

    if(pcap_sendpacket(conn->handle, ack_pkt, size) == -1) {
        fprintf(stderr, "ERROR: send_roce_ack failed: %s\n", pcap_geterr(conn->handle));
    }
}

// 向上游 Spine 转发数据（Leaf -> Spine）
static void send_roce_data_upstream(packet_metadata_t *meta_p){
    if((int)ctx.root_conn < 0) {
        LOG("send_roce_data_upstream: ERROR - root_conn not set\n");
        return;
    }

    int conn_id = ctx.root_conn;
    connection_t *conn = &ctx.conns[conn_id];
    int size = meta_p->pkt_len;
    g_total_upstream++;

    LOG_DETAIL("send_roce_data_upstream: PSN=%u, conn=%d\n", meta_p->psn, conn_id);

    memcpy(meta_p->header.eth->dst_mac, conn->peer_mac, 6);
    memcpy(meta_p->header.eth->src_mac, conn->my_mac, 6);
    meta_p->header.ip->src_ip = conn->my_ip;
    meta_p->header.ip->dst_ip = conn->peer_ip;
    meta_p->header.ip->checksum = 0;
    meta_p->header.ip->checksum = ipv4_checksum(meta_p->header.ip);
    meta_p->header.udp->src_port = htons(conn->my_port);
    meta_p->header.udp->dst_port = htons(conn->peer_port);
    meta_p->header.bth->qpn = htonl(conn->peer_qp & 0x00FFFFFF);
    meta_p->header.bth->apsn = htonl((1 << 31) | (meta_p->psn & 0x00FFFFFF));

    uint8_t *packet = (uint8_t *)meta_p->header.eth;
    uint32_t *icrc = (uint32_t *)(packet + size - 4);
    *icrc = compute_icrc(-1, packet);

    if(pcap_sendpacket(conn->handle, packet, size) == -1) {
        fprintf(stderr, "ERROR: send_roce_data_upstream failed: %s\n", pcap_geterr(conn->handle));
    }
}

// Spine 广播数据给所有子交换机
static void broadcast_to_leaf_switches(packet_metadata_t *meta_p){
    int size = meta_p->pkt_len;
    g_total_broadcasts++;
    LOG_DETAIL("broadcast_to_leaf_switches: PSN=%u\n", meta_p->psn);

    for(int i = 0; i < ctx.fan_in; i++){
        if(!(ctx.bitmap_mask & MASK_CONN(i)))
            continue;
        connection_t *conn = &ctx.conns[i];

        memcpy(meta_p->header.eth->dst_mac, conn->peer_mac, 6);
        memcpy(meta_p->header.eth->src_mac, conn->my_mac, 6);
        meta_p->header.ip->src_ip = conn->my_ip;
        meta_p->header.ip->dst_ip = conn->peer_ip;
        meta_p->header.ip->checksum = 0;
        meta_p->header.ip->checksum = ipv4_checksum(meta_p->header.ip);
        meta_p->header.udp->src_port = htons(conn->my_port);
        meta_p->header.udp->dst_port = htons(conn->peer_port);
        meta_p->header.bth->qpn = htonl(conn->peer_qp & 0x00FFFFFF);
        meta_p->header.bth->apsn = htonl((1 << 31) | (meta_p->psn & 0x00FFFFFF));

        uint8_t *packet = (uint8_t *)meta_p->header.eth;
        uint32_t *icrc = (uint32_t *)(packet + size - 4);
        *icrc = compute_icrc(-1, packet);

        if(pcap_sendpacket(conn->handle, packet, size) == -1) {
            fprintf(stderr, "ERROR: broadcast_to_leaf failed: %s\n", pcap_geterr(conn->handle));
        }
    }
}

// Leaf 广播数据给所有主机
static void broadcast_to_hosts(packet_metadata_t *meta_p){
    int size = meta_p->pkt_len;
    g_total_downstream++;
    LOG_DETAIL("broadcast_to_hosts: PSN=%u\n", meta_p->psn);

    for(int i = 0; i < ctx.fan_in; i++){
        if(ctx.conns[i].is_switch)
            continue;
        connection_t *conn = &ctx.conns[i];

        memcpy(meta_p->header.eth->dst_mac, conn->peer_mac, 6);
        memcpy(meta_p->header.eth->src_mac, conn->my_mac, 6);
        meta_p->header.ip->src_ip = conn->my_ip;
        meta_p->header.ip->dst_ip = conn->peer_ip;
        meta_p->header.ip->checksum = 0;
        meta_p->header.ip->checksum = ipv4_checksum(meta_p->header.ip);
        meta_p->header.udp->src_port = htons(conn->my_port);
        meta_p->header.udp->dst_port = htons(conn->peer_port);
        meta_p->header.bth->qpn = htonl(conn->peer_qp & 0x00FFFFFF);
        meta_p->header.bth->apsn = htonl((1 << 31) | (meta_p->psn & 0x00FFFFFF));

        uint8_t *packet = (uint8_t *)meta_p->header.eth;
        uint32_t *icrc = (uint32_t *)(packet + size - 4);
        *icrc = compute_icrc(-1, packet);

        if(pcap_sendpacket(conn->handle, packet, size) == -1) {
            fprintf(stderr, "ERROR: broadcast_to_hosts failed: %s\n", pcap_geterr(conn->handle));
        }
    }
}

static inline void clear_state_data(uint32_t psn){
    uint32_t idx = Idx(psn + WINDOW_SIZE);
    ctx.arrival_state[idx] = 0;
}

/**
 * Broadcast pipeline
 * 数据流: Root Host -> Leaf -> Spine -> 所有 Leaf -> 所有 Host
 */
void pipeline(packet_metadata_t *meta_p, const struct pcap_pkthdr *pkthdr, const uint8_t *packet) {
    g_total_recv_packets++;
    meta_p->pkt_len = pkthdr->len;

    // 解析头部
    meta_p->header.eth = (eth_header_t*)packet;
    meta_p->header.ip = (ipv4_header_t*)(packet + sizeof(eth_header_t));
    meta_p->header.udp = (udp_header_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t));
    meta_p->header.bth = (bth_header_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(udp_header_t));
    meta_p->psn = ntohl(meta_p->header.bth->apsn) & 0x00FFFFFF;

    if (meta_p->psn > g_max_psn_seen) g_max_psn_seen = meta_p->psn;
    if (meta_p->psn < g_min_psn_seen) g_min_psn_seen = meta_p->psn;

    LOG("PIPELINE: PSN=%u, opcode=0x%02x, ingress=%d\n",
        meta_p->psn, meta_p->header.bth->opcode, meta_p->ingress_conn);

    switch(meta_p->header.bth->opcode){
        case RC_SEND_FIRST:
        case RC_SEND_MIDDLE:
        case RC_SEND_LAST:
        case RC_SEND_ONLY:
            meta_p->type = PACKET_TYPE_DATA;
            meta_p->header.payload = (int *)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(udp_header_t) + sizeof(bth_header_t));
            break;
        case RC_ACKNOWLEDGE:
            meta_p->type = PACKET_TYPE_ACK;
            meta_p->header.aeth = (aeth_header_t *)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(udp_header_t) + sizeof(bth_header_t));
            break;
        default:
            return;
    }

    if(meta_p->type == PACKET_TYPE_ACK){
        g_total_ack_packets++;
        LOG("PIPELINE: ACK PSN=%u from conn=%d\n", meta_p->psn, meta_p->ingress_conn);

        uint32_t idx = Idx(meta_p->psn);
        ctx.arrival_state[idx] |= MASK_CONN(meta_p->ingress_conn);

        if(ctx.is_spine) {
            // Spine: 收到 Leaf 的 ACK，转发给发送方所在的 Leaf
            // 对于 Broadcast，Spine 收到任一 Leaf 的 ACK 就转发给其他 Leaf
            for(int i = 0; i < ctx.fan_in; i++){
                if(i == meta_p->ingress_conn) continue;
                if(!(ctx.bitmap_mask & MASK_CONN(i))) continue;
                send_roce_ack(i, meta_p->psn);
            }
        } else {
            // Leaf: 聚合所有下游 ACK 后发给上游
            // 等待: Spine ACK + 非 root 的 Host ACK
            uint32_t expected = ctx.ctrl_expected_bitmap;
            if(ctx.root_conn >= 0) {
                expected |= MASK_CONN(ctx.root_conn);
            }
            // 排除 root host，因为它是发送方，不会发 ACK
            if(ctx.reduce_root_conn >= 0) {
                expected &= ~MASK_CONN(ctx.reduce_root_conn);
            }
            if((ctx.arrival_state[idx] & expected) == expected){
                // 发给 root host (reduce_root_conn)
                if(ctx.reduce_root_conn >= 0) {
                    send_roce_ack(ctx.reduce_root_conn, meta_p->psn);
                }
                ctx.arrival_state[idx] = 0;
            }
        }
        return;
    }

    // DATA 包处理
    g_total_data_packets++;
    LOG("PIPELINE: DATA PSN=%u from conn=%d\n", meta_p->psn, meta_p->ingress_conn);

    // 立即发送 ACK 给发送方
    send_roce_ack(meta_p->ingress_conn, meta_p->psn);

    if(ctx.is_spine) {
        // Spine: 收到 Leaf 的数据，广播给所有 Leaf（包括发送方）
        broadcast_to_leaf_switches(meta_p);
    } else {
        // Leaf: 判断数据来源
        if(ctx.conns[meta_p->ingress_conn].is_switch) {
            // 来自 Spine 的下行数据，广播给所有 Host
            broadcast_to_hosts(meta_p);
        } else {
            // 来自 Host 的上行数据
            // 1. 转发给 Spine
            send_roce_data_upstream(meta_p);
            // 2. 同时广播给同一 Leaf 下的其他 Host（不包括发送方）
            for(int i = 0; i < ctx.fan_in; i++){
                if(ctx.conns[i].is_switch) continue;  // 跳过 Switch 连接
                if(i == meta_p->ingress_conn) continue;  // 跳过发送方
                connection_t *conn = &ctx.conns[i];

                memcpy(meta_p->header.eth->dst_mac, conn->peer_mac, 6);
                memcpy(meta_p->header.eth->src_mac, conn->my_mac, 6);
                meta_p->header.ip->src_ip = conn->my_ip;
                meta_p->header.ip->dst_ip = conn->peer_ip;
                meta_p->header.ip->checksum = 0;
                meta_p->header.ip->checksum = ipv4_checksum(meta_p->header.ip);
                meta_p->header.udp->src_port = htons(conn->my_port);
                meta_p->header.udp->dst_port = htons(conn->peer_port);
                meta_p->header.bth->qpn = htonl(conn->peer_qp & 0x00FFFFFF);
                meta_p->header.bth->apsn = htonl((1 << 31) | (meta_p->psn & 0x00FFFFFF));

                uint8_t *packet = (uint8_t *)meta_p->header.eth;
                uint32_t *icrc = (uint32_t *)(packet + meta_p->pkt_len - 4);
                *icrc = compute_icrc(-1, packet);

                if(pcap_sendpacket(conn->handle, packet, meta_p->pkt_len) == -1) {
                    fprintf(stderr, "ERROR: broadcast to local host failed: %s\n", pcap_geterr(conn->handle));
                }
            }
        }
    }

    clear_state_data(meta_p->psn);
    LOG("PIPELINE: done PSN=%u\n", meta_p->psn);
}

void epoll_process_packets(){
    LOG("epoll_process_packets: Starting, %d devices\n", g_num_devices);

    const unsigned char *packet;
    struct pcap_pkthdr *pkthdr;

    while (1) {
        struct epoll_event events[MAX_EVENTS];
        int nfds = epoll_wait(ctx.epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            LOG("epoll_wait error, exiting\n");
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int dev_idx = events[i].data.u32;
            if (dev_idx >= g_num_devices) continue;

            pcap_t *handle = g_device_handles[dev_idx].handle;
            int batch_count = 0;

            while (batch_count < 256 && (pcap_next_ex(handle, &pkthdr, &packet)) == 1) {
                memset(&meta, 0, sizeof(packet_metadata_t));

                if (pkthdr->len < 34) {
                    batch_count++;
                    continue;
                }

                ipv4_header_t *ip = (ipv4_header_t *)(packet + 14);
                int conn_id = find_conn_by_src_ip(ip->src_ip);
                if (conn_id < 0) {
                    batch_count++;
                    continue;
                }

                meta.ingress_conn = conn_id;
                pipeline(&meta, pkthdr, packet);
                print_stats_summary();
                batch_count++;
            }
        }
    }
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    char *controller_ip = "192.168.0.3";
    int switch_id = 0;

    if (argc >= 2) {
        controller_ip = argv[1];
    }
    if (argc >= 3) {
        switch_id = atoi(argv[2]);
    }

    printf("=== INC Switch Broadcast ===\n");
    printf("Controller IP: %s, Switch ID: %d\n", controller_ip, switch_id);
    printf("PID: %d\n", getpid());
    fflush(stdout);

    init_crc32_table();

    if (switch_context_init(&ctx, switch_id) < 0) {
        fprintf(stderr, "Failed to initialize switch context\n");
        return 1;
    }

    if (controller_init(&ctx, controller_ip) < 0) {
        fprintf(stderr, "Failed to connect to controller\n");
        switch_context_cleanup(&ctx);
        return 1;
    }

    printf("Waiting for YAML config...\n");
    fflush(stdout);

    pthread_mutex_lock(&ctx.config_mutex);
    while (!ctx.config_ready) {
        pthread_cond_wait(&ctx.config_cond, &ctx.config_mutex);
    }
    pthread_mutex_unlock(&ctx.config_mutex);

    printf("=== Config received ===\n");
    printf("  fan_in=%d, is_spine=%d\n", ctx.fan_in, ctx.is_spine);
    printf("  root_conn=%d, bitmap_mask=0x%x\n", ctx.root_conn, ctx.bitmap_mask);
    printf("  reduce_root_conn=%d\n", ctx.reduce_root_conn);
    fflush(stdout);

    init_devices();
    epoll_process_packets();

    switch_context_cleanup(&ctx);
    printf("Switch stopped\n");
    return 0;
}
