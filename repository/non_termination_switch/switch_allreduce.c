/**
 * @file switch_main.c
 * @brief INC 交换机主程序（重构版本）
 *
 * 实现功能：
 * - 连接控制器获取 YAML 配置
 * - 基于 pcap 的数据包接收
 * - 数据聚合和广播
 * - ACK/NAK 流控
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
static uint64_t g_total_aggregations = 0;
static uint64_t g_total_broadcasts = 0;
static uint64_t g_total_downstream = 0;
static uint64_t g_total_upstream = 0;
static uint64_t g_total_send_to_hosts = 0;
static uint64_t g_total_send_to_spine = 0;
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
    printf("  total_aggregations:  %lu\n", g_total_aggregations);
    printf("  total_broadcasts:    %lu\n", g_total_broadcasts);
    printf("  total_downstream:    %lu\n", g_total_downstream);
    printf("  total_upstream:      %lu\n", g_total_upstream);
    printf("  total_send_to_hosts: %lu\n", g_total_send_to_hosts);
    printf("  total_send_to_spine: %lu\n", g_total_send_to_spine);
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
    int first_conn;  // 该设备的第一个连接 ID（用于 epoll 事件）
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
            // 复用已有的句柄
            ctx.conns[i].handle = g_device_handles[existing_dev].handle;
            LOG("init_devices: port %d reuses device %s (handle from port %d)\n",
                i, dev_name, g_device_handles[existing_dev].first_conn);
            continue;
        }

        // 创建新的设备句柄
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
        pcap_set_buffer_size(handle, 256 * 1024 * 1024);

        if (pcap_activate(handle) != 0) {
            fprintf(stderr, "pcap_activate failed for %s: %s\n", dev_name, pcap_geterr(handle));
            pcap_close(handle);
            continue;
        }

        pcap_setnonblock(handle, 1, errbuf);

        // 构建过滤器：捕获该设备上所有相关连接的 UDP 包
        // 收集该设备上所有连接的 peer_ip
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

        // 添加到 epoll
        int fd = pcap_get_selectable_fd(handle);
        if (fd == -1) {
            fprintf(stderr, "Cannot get selectable fd for %s\n", dev_name);
            pcap_close(handle);
            continue;
        }

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.u32 = g_num_devices;  // 使用设备索引

        if (epoll_ctl(ctx.epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
            perror("epoll_ctl failed");
            pcap_close(handle);
            continue;
        }

        // 保存设备句柄信息
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
            // 查找该设备的句柄
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

// 发送速率限制：每发送 SEND_BATCH_SIZE 个包后等待 SEND_DELAY_US 微秒
#define SEND_BATCH_SIZE 32
#define SEND_DELAY_US 100

// 发送聚合结果给单个主机（用于重传处理）
static void send_roce_data_to_host(uint32_t conn_id, packet_metadata_t *meta_p){
    connection_t *conn = &ctx.conns[conn_id];
    int size = meta_p->pkt_len;
    uint32_t idx = Idx(meta_p->psn);

    LOG_DETAIL("send_roce_data_to_host: conn=%d, PSN=%u, size=%d\n", conn_id, meta_p->psn, size);

    memcpy(meta_p->header.eth->dst_mac, conn->peer_mac, 6);
    memcpy(meta_p->header.eth->src_mac, conn->my_mac, 6);
    meta_p->header.ip->src_ip = conn->my_ip;
    meta_p->header.ip->dst_ip = conn->peer_ip;
    meta_p->header.ip->checksum = 0;
    meta_p->header.ip->checksum = ipv4_checksum(meta_p->header.ip);
    meta_p->header.udp->src_port = htons(conn->my_port);
    meta_p->header.udp->dst_port = htons(conn->peer_port);
    meta_p->header.bth->opcode = RC_SEND_ONLY;
    meta_p->header.bth->se_m_pad = 0x00;
    meta_p->header.bth->pkey = 0xffff;
    meta_p->header.bth->qpn = htonl(conn->peer_qp & 0x00FFFFFF);
    meta_p->header.bth->apsn = htonl((1 << 31) | (meta_p->psn & 0x00FFFFFF));

    // 从 aggregator 缓存中填充 payload
    int payload_len = size - 58;
    int num_ints = payload_len > 0 ? payload_len / sizeof(int) : 0;
    for(int i = 0; i < num_ints; i++){
        meta_p->header.payload[i] = htonl(ctx.aggregator[idx][i]);
    }

    uint8_t *packet = (uint8_t *)meta_p->header.eth;
    uint32_t *icrc = (uint32_t *)(packet + size - 4);
    *icrc = compute_icrc(-1, packet);

    if(pcap_sendpacket(conn->handle, packet, size) == -1) {
        fprintf(stderr, "ERROR: send_roce_data_to_host failed: %s\n", pcap_geterr(conn->handle));
    } else {
        LOG_DETAIL("send_roce_data_to_host: SENT to conn %d, PSN=%u\n", conn_id, meta_p->psn);
    }
}

// 叶子交换机广播数据给所有主机（下行广播）
static void broadcast_roce_data_to_hosts(packet_metadata_t *meta_p){
    int size = meta_p->pkt_len;
    g_total_downstream++;
    LOG_DETAIL("broadcast_roce_data_to_hosts: START PSN=%u, size=%d, total_downstream=%lu\n",
               meta_p->psn, size, g_total_downstream);

    // 速率限制：每 SEND_BATCH_SIZE 个包后延迟
    static uint64_t send_counter = 0;

    int sent_count = 0;
    for(int i = 0; i < ctx.fan_in; i++){
        // 只发送给主机连接（非交换机连接）
        if(ctx.conns[i].is_switch) {
            LOG_DETAIL("  skip conn %d (is_switch=true)\n", i);
            continue;
        }
        connection_t *conn = &ctx.conns[i];

        LOG_DETAIL("  preparing for conn %d: peer_ip=%s, peer_qp=%u\n",
                   i, uint32_to_ip_string_big_endian(conn->peer_ip), conn->peer_qp);

        memcpy(meta_p->header.eth->dst_mac, conn->peer_mac, 6);
        memcpy(meta_p->header.eth->src_mac, conn->my_mac, 6);
        meta_p->header.ip->src_ip = conn->my_ip;
        meta_p->header.ip->dst_ip = conn->peer_ip;
        meta_p->header.ip->checksum = 0;
        meta_p->header.ip->checksum = ipv4_checksum(meta_p->header.ip);
        meta_p->header.udp->src_port = htons(conn->my_port);
        meta_p->header.udp->dst_port = htons(conn->peer_port);
        meta_p->header.bth->opcode = RC_SEND_ONLY;
        meta_p->header.bth->se_m_pad = 0x00;
        meta_p->header.bth->pkey = 0xffff;
        meta_p->header.bth->qpn = htonl(conn->peer_qp & 0x00FFFFFF);
        meta_p->header.bth->apsn = htonl((1 << 31) | (meta_p->psn & 0x00FFFFFF));

        uint8_t *packet = (uint8_t *)meta_p->header.eth;
        uint32_t *icrc = (uint32_t *)(packet + size - 4);
        *icrc = compute_icrc(-1, packet);

        if(pcap_sendpacket(conn->handle, packet, size) == -1) {
            fprintf(stderr, "ERROR: broadcast_roce_data_to_hosts: pcap_sendpacket failed for conn %d: %s\n",
                    i, pcap_geterr(conn->handle));
        } else {
            g_total_send_to_hosts++;
            sent_count++;
            send_counter++;
            LOG_DETAIL("  SENT to port %d, PSN=%u, total_send_to_hosts=%lu\n",
                       i, meta_p->psn, g_total_send_to_hosts);

            // 速率限制
            if(send_counter % SEND_BATCH_SIZE == 0) {
                usleep(SEND_DELAY_US);
            }
        }
    }
    LOG_DETAIL("broadcast_roce_data_to_hosts: END PSN=%u, sent_count=%d\n", meta_p->psn, sent_count);
}

// Reduce 专用：叶子交换机只发送给 root host（不广播）
static void send_roce_data_to_root_host(packet_metadata_t *meta_p){
    if(ctx.reduce_root_conn < 0) {
        LOG("send_roce_data_to_root_host: ERROR - reduce_root_conn not set\n");
        return;
    }

    int conn_id = ctx.reduce_root_conn;
    connection_t *conn = &ctx.conns[conn_id];
    int size = meta_p->pkt_len;
    uint32_t idx = Idx(meta_p->psn);

    g_total_downstream++;
    LOG_DETAIL("send_roce_data_to_root_host: PSN=%u, conn=%d, size=%d\n",
               meta_p->psn, conn_id, size);

    memcpy(meta_p->header.eth->dst_mac, conn->peer_mac, 6);
    memcpy(meta_p->header.eth->src_mac, conn->my_mac, 6);
    meta_p->header.ip->src_ip = conn->my_ip;
    meta_p->header.ip->dst_ip = conn->peer_ip;
    meta_p->header.ip->checksum = 0;
    meta_p->header.ip->checksum = ipv4_checksum(meta_p->header.ip);
    meta_p->header.udp->src_port = htons(conn->my_port);
    meta_p->header.udp->dst_port = htons(conn->peer_port);
    meta_p->header.bth->opcode = RC_SEND_ONLY;  // 每个聚合结果是独立的包
    meta_p->header.bth->se_m_pad = 0x00;  // 清除 padding 标志
    meta_p->header.bth->pkey = 0xffff;    // 默认 partition key
    meta_p->header.bth->qpn = htonl(conn->peer_qp & 0x00FFFFFF);
    meta_p->header.bth->apsn = htonl((1 << 31) | (meta_p->psn & 0x00FFFFFF));

    // 从 aggregator 缓存中填充 payload
    int payload_len = size - 58;
    int num_ints = payload_len > 0 ? payload_len / sizeof(int) : 0;
    for(int i = 0; i < num_ints; i++){
        meta_p->header.payload[i] = htonl(ctx.aggregator[idx][i]);
    }

    uint8_t *packet = (uint8_t *)meta_p->header.eth;
    uint32_t *icrc = (uint32_t *)(packet + size - 4);
    *icrc = compute_icrc(-1, packet);

    if(pcap_sendpacket(conn->handle, packet, size) == -1) {
        fprintf(stderr, "ERROR: send_roce_data_to_root_host failed: %s\n", pcap_geterr(conn->handle));
    } else {
        g_total_send_to_hosts++;
        LOG_DETAIL("send_roce_data_to_root_host: SENT PSN=%u to root conn %d\n", meta_p->psn, conn_id);
    }
}

// Spine 广播聚合数据给所有子交换机
static void broadcast_roce_data(packet_metadata_t *meta_p){
    int size = meta_p->pkt_len;
    int *agg = ctx.aggregator[Idx(meta_p->psn)];
    g_total_broadcasts++;
    LOG_DETAIL("broadcast_roce_data: START PSN=%u, size=%d, idx=%u, total_broadcasts=%lu\n",
               meta_p->psn, size, Idx(meta_p->psn), g_total_broadcasts);

    // 速率限制
    static uint64_t spine_send_counter = 0;

    int payload_len = size - 58;
    int num_ints = payload_len > 0 ? payload_len / sizeof(int) : 0;
    LOG_DETAIL("  payload_len=%d, num_ints=%d\n", payload_len, num_ints);

    int sent_count = 0;
    for(int i = 0; i < ctx.fan_in; i++){
        if(!(ctx.bitmap_mask & MASK_CONN(i))) {
            LOG_DETAIL("  skip conn %d (not in bitmap_mask=0x%x)\n", i, ctx.bitmap_mask);
            continue;
        }
        connection_t *conn = &ctx.conns[i];

        memcpy(meta_p->header.eth->dst_mac, conn->peer_mac, 6);
        memcpy(meta_p->header.eth->src_mac, conn->my_mac, 6);
        meta_p->header.ip->src_ip = conn->my_ip;
        meta_p->header.ip->dst_ip = conn->peer_ip;
        meta_p->header.ip->checksum = 0;
        meta_p->header.ip->checksum = ipv4_checksum(meta_p->header.ip);
        meta_p->header.udp->src_port = htons(conn->my_port);
        meta_p->header.udp->dst_port = htons(conn->peer_port);
        meta_p->header.bth->opcode = RC_SEND_ONLY;
        meta_p->header.bth->se_m_pad = 0x00;
        meta_p->header.bth->pkey = 0xffff;
        meta_p->header.bth->qpn = htonl(conn->peer_qp & 0x00FFFFFF);
        meta_p->header.bth->apsn = htonl((1 << 31) | (meta_p->psn & 0x00FFFFFF));

        for(int j = 0; j < num_ints; j++){
            meta_p->header.payload[j] = htonl(agg[j]);
        }

        uint8_t *packet = (uint8_t *)meta_p->header.eth;
        uint32_t *icrc = (uint32_t *)(packet + size - 4);
        *icrc = compute_icrc(-1, packet);

        if(pcap_sendpacket(conn->handle, packet, size) == -1) {
            fprintf(stderr, "ERROR: broadcast_roce_data: pcap_sendpacket failed for conn %d: %s\n",
                    i, pcap_geterr(conn->handle));
        } else {
            sent_count++;
            spine_send_counter++;
            LOG_DETAIL("  SENT to port %d, PSN=%u, peer_qp=%u\n", i, meta_p->psn, conn->peer_qp);

            // 速率限制
            if(spine_send_counter % SEND_BATCH_SIZE == 0) {
                usleep(SEND_DELAY_US);
            }
        }
    }
    LOG_DETAIL("broadcast_roce_data: END PSN=%u, sent_count=%d\n", meta_p->psn, sent_count);
}

// Reduce 专用：Spine 只发送给包含 root 节点的子交换机
static void send_roce_data_to_root_leaf(packet_metadata_t *meta_p){
    if(ctx.reduce_root_conn < 0) {
        LOG("send_roce_data_to_root_leaf: ERROR - reduce_root_conn not set\n");
        return;
    }

    int conn_id = ctx.reduce_root_conn;
    connection_t *conn = &ctx.conns[conn_id];
    int size = meta_p->pkt_len;
    int *agg = ctx.aggregator[Idx(meta_p->psn)];

    g_total_broadcasts++;
    LOG_DETAIL("send_roce_data_to_root_leaf: PSN=%u, conn=%d, size=%d\n",
               meta_p->psn, conn_id, size);

    int payload_len = size - 58;
    int num_ints = payload_len > 0 ? payload_len / sizeof(int) : 0;

    memcpy(meta_p->header.eth->dst_mac, conn->peer_mac, 6);
    memcpy(meta_p->header.eth->src_mac, conn->my_mac, 6);
    meta_p->header.ip->src_ip = conn->my_ip;
    meta_p->header.ip->dst_ip = conn->peer_ip;
    meta_p->header.ip->checksum = 0;
    meta_p->header.ip->checksum = ipv4_checksum(meta_p->header.ip);
    meta_p->header.udp->src_port = htons(conn->my_port);
    meta_p->header.udp->dst_port = htons(conn->peer_port);
    meta_p->header.bth->opcode = RC_SEND_ONLY;
    meta_p->header.bth->se_m_pad = 0x00;
    meta_p->header.bth->pkey = 0xffff;
    meta_p->header.bth->qpn = htonl(conn->peer_qp & 0x00FFFFFF);
    meta_p->header.bth->apsn = htonl((1 << 31) | (meta_p->psn & 0x00FFFFFF));

    for(int j = 0; j < num_ints; j++){
        meta_p->header.payload[j] = htonl(agg[j]);
    }

    uint8_t *packet = (uint8_t *)meta_p->header.eth;
    uint32_t *icrc = (uint32_t *)(packet + size - 4);
    *icrc = compute_icrc(-1, packet);

    if(pcap_sendpacket(conn->handle, packet, size) == -1) {
        fprintf(stderr, "ERROR: send_roce_data_to_root_leaf failed: %s\n",
                pcap_geterr(conn->handle));
    } else {
        LOG_DETAIL("send_roce_data_to_root_leaf: SENT PSN=%u to leaf conn %d\n",
                   meta_p->psn, conn_id);
    }
}

static void send_roce_data(uint32_t conn_id, packet_metadata_t *meta_p){
    connection_t *conn = &ctx.conns[conn_id];
    g_total_send_to_spine++;
    g_total_upstream++;

    // 速率限制
    static uint64_t upstream_send_counter = 0;

    LOG_DETAIL("send_roce_data: START conn=%d, PSN=%u, pkt_len=%u, total_upstream=%lu\n",
               conn_id, meta_p->psn, meta_p->pkt_len, g_total_upstream);

    memcpy(meta_p->header.eth->dst_mac, conn->peer_mac, 6);
    memcpy(meta_p->header.eth->src_mac, conn->my_mac, 6);

    meta_p->header.ip->src_ip = conn->my_ip;
    meta_p->header.ip->dst_ip = conn->peer_ip;
    meta_p->header.ip->checksum = 0;
    meta_p->header.ip->checksum = ipv4_checksum(meta_p->header.ip);

    meta_p->header.udp->src_port = htons(conn->my_port);
    meta_p->header.udp->dst_port = htons(conn->peer_port);

    meta_p->header.bth->opcode = RC_SEND_ONLY;
    meta_p->header.bth->se_m_pad = 0x00;
    meta_p->header.bth->pkey = 0xffff;
    meta_p->header.bth->qpn = htonl(conn->peer_qp & 0x00FFFFFF);
    meta_p->header.bth->apsn = htonl((1 << 31) | (meta_p->psn & 0x00FFFFFF));

    int payload_len = meta_p->pkt_len - 58;
    LOG_DETAIL("  payload_len=%d, peer_qp=%u\n", payload_len, conn->peer_qp);
    if (payload_len > 0) {
        int *agg = ctx.aggregator[Idx(meta_p->psn)];
        int num_ints = payload_len / sizeof(int);
        for(int i = 0; i < num_ints; i++){
            meta_p->header.payload[i] = htonl(agg[i]);
        }
    }

    int size = meta_p->pkt_len;
    uint8_t *packet = (uint8_t *)meta_p->header.eth;
    uint32_t *icrc = (uint32_t *)(packet + size - 4);
    *icrc = compute_icrc(-1, packet);

    if(pcap_sendpacket(conn->handle, packet, size) == -1) {
        fprintf(stderr, "ERROR: send_roce_data: pcap_sendpacket failed: %s\n", pcap_geterr(conn->handle));
    } else {
        upstream_send_counter++;
        LOG_DETAIL("send_roce_data: SENT conn=%d, PSN=%u, size=%d\n", conn_id, meta_p->psn, size);

        // 速率限制
        if(upstream_send_counter % SEND_BATCH_SIZE == 0) {
            usleep(SEND_DELAY_US);
        }
    }
}

// ACK 广播：发送给 bitmap_mask 中的所有连接，但跳过发送方
static void broadcast_roce_ack(packet_metadata_t *meta_p, int ingress_conn){
    int size = 62;
    uint8_t *packet = (uint8_t *)meta_p->header.eth;
    LOG_DETAIL("broadcast_roce_ack: START PSN=%u, ingress=%d\n", meta_p->psn, ingress_conn);

    int sent_count = 0;
    for(int i = 0; i < ctx.fan_in; i++){
        // 跳过发送方，避免 ACK 循环
        if(i == ingress_conn) {
            LOG_DETAIL("  skip port %d (ingress)\n", i);
            continue;
        }
        if(!(ctx.bitmap_mask & MASK_CONN(i)))
            continue;
        connection_t *conn = &ctx.conns[i];

        memcpy(meta_p->header.eth->dst_mac, conn->peer_mac, 6);
        memcpy(meta_p->header.eth->src_mac, conn->my_mac, 6);

        meta_p->header.ip->src_ip = conn->my_ip;
        meta_p->header.ip->dst_ip = conn->peer_ip;

        meta_p->header.udp->src_port = htons(conn->my_port);
        meta_p->header.udp->dst_port = htons(conn->peer_port);

        meta_p->header.bth->qpn = htonl(conn->peer_qp);

        uint32_t *icrc = (uint32_t *)(packet + size - 4);
        *icrc = compute_icrc(-1, packet);

        if(pcap_sendpacket(conn->handle, packet, size) == -1) {
            fprintf(stderr, "ERROR: broadcast_roce_ack failed: %s\n", pcap_geterr(conn->handle));
        } else {
            sent_count++;
            LOG_DETAIL("broadcast_roce_ack: sent to port %d, PSN=%u\n", i, meta_p->psn);
        }
    }
    LOG_DETAIL("broadcast_roce_ack: END PSN=%u, sent_count=%d\n", meta_p->psn, sent_count);
}

/**
 * 发送 ACK 给指定连接（ACK 反射）
 */
static void send_roce_ack(uint32_t conn_id, uint32_t psn){
    connection_t *conn = &ctx.conns[conn_id];
    g_total_ack_sent++;
    LOG_DETAIL("send_roce_ack: START conn=%d, PSN=%u, total_ack_sent=%lu\n",
               conn_id, psn, g_total_ack_sent);
    LOG_DETAIL("  peer_ip=%s, peer_qp=%u, peer_port=%u\n",
               uint32_to_ip_string_big_endian(conn->peer_ip), conn->peer_qp, conn->peer_port);
    // ACK 包: ETH(14) + IP(20) + UDP(8) + BTH(12) + AETH(4) + ICRC(4) = 62
    int size = 62;
    uint8_t ack_pkt[62];
    memset(ack_pkt, 0, sizeof(ack_pkt));

    // ETH header
    eth_header_t *eth = (eth_header_t *)ack_pkt;
    memcpy(eth->dst_mac, conn->peer_mac, 6);
    memcpy(eth->src_mac, conn->my_mac, 6);
    eth->ether_type = htons(0x0800);

    // IP header
    ipv4_header_t *ip = (ipv4_header_t *)(ack_pkt + 14);
    ip->version_ihl = 0x45;
    ip->tos = 0x00;
    ip->total_length = htons(48);  // IP total = 20 + 8 + 12 + 4 + 4 = 48
    ip->id = 0x1111;
    ip->flags_frag_off = htons(0x4000);
    ip->ttl = 0x40;
    ip->protocol = 0x11;  // UDP
    ip->src_ip = conn->my_ip;
    ip->dst_ip = conn->peer_ip;
    ip->checksum = ipv4_checksum(ip);

    // UDP header
    udp_header_t *udp = (udp_header_t *)(ack_pkt + 34);
    udp->src_port = htons(conn->my_port);
    udp->dst_port = htons(conn->peer_port);
    udp->length = htons(28);  // UDP len = 8 + 12 + 4 + 4 = 28
    udp->checksum = 0;

    // BTH header
    bth_header_t *bth = (bth_header_t *)(ack_pkt + 42);
    bth->opcode = RC_ACKNOWLEDGE;
    bth->se_m_pad = 0x00;
    bth->pkey = 0xffff;
    bth->qpn = htonl(conn->peer_qp & 0x00FFFFFF);
    bth->apsn = htonl(psn & 0x00FFFFFF);  // No A bit for ACK

    // AETH header - syndrome 0x1f = ACK, MSN = psn
    aeth_header_t *aeth = (aeth_header_t *)(ack_pkt + 54);
    aeth->syn_msn = htonl(psn | 0x1f000000);

    // ICRC
    uint32_t *icrc = (uint32_t *)(ack_pkt + 58);
    *icrc = compute_icrc(-1, (const char *)ack_pkt);

    if(pcap_sendpacket(conn->handle, ack_pkt, size) == -1) {
        fprintf(stderr, "ERROR: send_roce_ack failed for conn %d, PSN=%u: %s\n",
                conn_id, psn, pcap_geterr(conn->handle));
    } else {
        LOG_DETAIL("send_roce_ack: SENT conn=%d, PSN=%u, size=%d\n", conn_id, psn, size);
    }
}

static inline void clear_state_data(uint32_t psn){
    uint32_t idx = Idx(psn + WINDOW_SIZE);
    // 清理未来要复用的槽位
    ctx.arrival_state[idx] = 0;
    ctx.degree[idx] = 0;
    memset(ctx.aggregator[idx], 0, PAYLOAD_LEN);
}

/**
 * simulation of p4 switch
 */
void pipeline(packet_metadata_t *meta_p, const struct pcap_pkthdr *pkthdr, const uint8_t *packet) {
    g_total_recv_packets++;

    // parser: extract header info
    LOG("PIPELINE: ingress_conn=%d, pkt_len=%d, total_recv=%lu\n",
        meta_p->ingress_conn, pkthdr->len, g_total_recv_packets);
    meta_p->pkt_len = pkthdr->len;  // 保存实际包大小

    meta_p->header.eth = (eth_header_t*)packet;
    meta_p->header.ip = (ipv4_header_t*)(packet + sizeof(eth_header_t));
    meta_p->header.udp = (udp_header_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t));
    meta_p->header.bth = (bth_header_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(udp_header_t));
    meta_p->psn = ntohl(meta_p->header.bth->apsn) & 0x00FFFFFF;

    // 更新 PSN 统计
    if (meta_p->psn > g_max_psn_seen) g_max_psn_seen = meta_p->psn;
    if (meta_p->psn < g_min_psn_seen) g_min_psn_seen = meta_p->psn;

    LOG("PIPELINE: PSN=%u, opcode=0x%02x, psn_range=[%lu,%lu]\n",
        meta_p->psn, meta_p->header.bth->opcode, g_min_psn_seen, g_max_psn_seen);
    switch(meta_p->header.bth->opcode){
        case RC_SEND_FIRST:
        case RC_SEND_MIDDLE:
        case RC_SEND_LAST:
        case RC_SEND_ONLY:
            meta_p->type = PACKET_TYPE_DATA;
            meta_p->header.payload = (int *)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(udp_header_t) + sizeof(bth_header_t));
            break;
        case RC_SEND_ONLY_WITH_IMMEDIATE:
            meta_p->type = PACKET_TYPE_CONTROLL;
            meta_p->header.inc = (ctl_header_t *)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(udp_header_t) + sizeof(bth_header_t));
            break;
        case RC_ACKNOWLEDGE:
            meta_p->type = PACKET_TYPE_ACK;
            meta_p->header.aeth = (aeth_header_t *)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(udp_header_t) + sizeof(bth_header_t));
            break;
    }

    // psn offset
    // only reduce with destination rank0, no offset, no control packet.
    if(meta_p->type == PACKET_TYPE_ACK){
        g_total_ack_packets++;
        LOG("PIPELINE: ACK packet, PSN=%u, from port=%d, total_ack=%lu\n",
            meta_p->psn, meta_p->ingress_conn, g_total_ack_packets);

        // ACK 包：Leaf 收到 root Host 的 ACK 后，广播给所有其他 Host
        // 这实现了全局同步的应用层滑动窗口
        if (!ctx.is_spine && meta_p->ingress_conn == ctx.reduce_root_conn) {
            LOG("PIPELINE: ACK from root host, broadcast to other hosts, PSN=%u\n", meta_p->psn);
            for (int i = 0; i < ctx.fan_in; i++) {
                if (!ctx.conns[i].is_switch && i != ctx.reduce_root_conn) {
                    send_roce_ack(i, meta_p->psn);
                }
            }
        }

    }
    else if(meta_p->type == PACKET_TYPE_DATA){
        g_total_data_packets++;
        LOG("PIPELINE: DATA packet, PSN=%u, port=%d, total_data=%lu\n",
            meta_p->psn, meta_p->ingress_conn, g_total_data_packets);

        // 判断数据流方向：
        // - 对于叶子交换机，来自父交换机(root_conn)的数据是下行广播，直接转发给主机
        // - 其他情况是上行聚合
        if (!ctx.is_spine && meta_p->ingress_conn == (int)ctx.root_conn) {
            // 下行数据：Leaf 收到 Spine 的广播，发 ACK 给 Spine
            send_roce_ack(meta_p->ingress_conn, meta_p->psn);

            uint32_t idx = Idx(meta_p->psn);
            // 标记已从父节点收到聚合结果（用于重传判断）
            ctx.arrival_state[idx] |= MASK_CONN(ctx.root_conn);

            // 缓存聚合结果到 aggregator（用于重传时直接发送）
            int payload_len = meta_p->pkt_len - 58;
            int num_ints = payload_len > 0 ? payload_len / sizeof(int) : 0;
            for(int i = 0; i < num_ints; ++i){
                ctx.aggregator[idx][i] = ntohl(meta_p->header.payload[i]);
            }

            // AllReduce: 广播给所有 host
            LOG("PIPELINE: DOWNSTREAM AllReduce broadcast to all hosts, PSN=%u\n", meta_p->psn);
            broadcast_roce_data_to_hosts(meta_p);

            // 清理未来要复用的槽位，实现窗口循环
            clear_state_data(meta_p->psn);

            LOG("PIPELINE: done, PSN=%u\n", meta_p->psn);
            return;
        }

        // 上行数据：Leaf 立即发 ACK（保持 RDMA 流控）
        // Host 端使用应用层滑动窗口来同步发送速度
        if (!ctx.is_spine) {
            LOG("PIPELINE: Sending ACK to conn=%d for PSN=%u\n", meta_p->ingress_conn, meta_p->psn);
            send_roce_ack(meta_p->ingress_conn, meta_p->psn);
        }

        uint32_t idx = Idx(meta_p->psn);

        // 动态同步第一个 PSN
        if (!ctx.psn_synced[meta_p->ingress_conn]) {
            ctx.data_epsn[meta_p->ingress_conn] = meta_p->psn;
            ctx.psn_synced[meta_p->ingress_conn] = 1;
            LOG("PIPELINE: SYNC first PSN=%u for conn %d\n",
                meta_p->psn, meta_p->ingress_conn);
        }

        LOG("PIPELINE: idx=%u, epsn[%d]=%u, arrival=0x%x, mask=0x%x\n",
            idx, meta_p->ingress_conn, ctx.data_epsn[meta_p->ingress_conn],
            ctx.arrival_state[idx], ctx.bitmap_mask);

        // 允许 PSN 在窗口范围内乱序到达
        // 检查 PSN 是否在有效范围内（epsn 到 epsn + WINDOW_SIZE）
        int32_t psn_diff = (int32_t)meta_p->psn - (int32_t)ctx.data_epsn[meta_p->ingress_conn];
        if(psn_diff < 0 || psn_diff >= WINDOW_SIZE) {
            // PSN 超出有效范围
            if((ctx.arrival_state[idx] & MASK_CONN(meta_p->ingress_conn)) != 0) {
                // 已经收到过这个 PSN，可能是重传
                // 检查是否已从父节点收到聚合结果
                if(ctx.is_spine) {
                    // Spine: 检查聚合是否完成，完成则重新广播
                    if(ctx.arrival_state[idx] == ctx.bitmap_mask){
                        LOG("PIPELINE: RETRANS - Spine resend broadcast, PSN=%u\n", meta_p->psn);
                        broadcast_roce_data(meta_p);
                    }
                } else {
                    // Leaf: 检查是否已从父节点(root_conn)收到聚合结果
                    if(ctx.arrival_state[idx] & MASK_CONN(ctx.root_conn)) {
                        // 已收到父节点的聚合结果，直接发给重传的 Host
                        LOG("PIPELINE: RETRANS - Leaf resend to host %d, PSN=%u\n",
                            meta_p->ingress_conn, meta_p->psn);
                        send_roce_data_to_host(meta_p->ingress_conn, meta_p);
                    } else {
                        // 未从父节点收到聚合结果，需要向父节点重传
                        // 使用 degree 计数，当所有 host 都重传后才向父节点重传
                        ctx.degree[idx]++;
                        int host_count = __builtin_popcount(ctx.ctrl_expected_bitmap);
                        if(ctx.degree[idx] % host_count == 0) {
                            LOG("PIPELINE: RETRANS - Leaf resend to Spine, PSN=%u, degree=%d\n",
                                meta_p->psn, ctx.degree[idx]);
                            send_roce_data(ctx.root_conn, meta_p);
                        }
                    }
                }
            } else {
                LOG("PIPELINE: DROP - PSN out of range, PSN=%u, epsn=%d, diff=%d\n",
                    meta_p->psn, ctx.data_epsn[meta_p->ingress_conn], psn_diff);
            }
            return;
        }

        // PSN 在有效范围内，检查是否已经收到过
        if((ctx.arrival_state[idx] & MASK_CONN(meta_p->ingress_conn)) != 0) {
            LOG("PIPELINE: DROP - duplicate PSN=%u\n", meta_p->psn);
            return;
        }

        // 更新 epsn（如果收到的是期望的 PSN）
        if(meta_p->psn == (uint32_t)ctx.data_epsn[meta_p->ingress_conn]) {
            ctx.data_epsn[meta_p->ingress_conn]++;
            // 跳过已经收到的 PSN
            while(ctx.arrival_state[Idx(ctx.data_epsn[meta_p->ingress_conn])] & MASK_CONN(meta_p->ingress_conn)) {
                ctx.data_epsn[meta_p->ingress_conn]++;
            }
        }

        ctx.arrival_state[idx] |= MASK_CONN(meta_p->ingress_conn);
        LOG("PIPELINE: aggregating, new_arrival=0x%x, epsn=%d\n",
            ctx.arrival_state[idx], ctx.data_epsn[meta_p->ingress_conn]);

        // 计算实际 payload 大小并聚合
        int payload_len = meta_p->pkt_len - 58;
        int num_ints = payload_len > 0 ? payload_len / sizeof(int) : 0;
        for(int i = 0; i < num_ints; ++i){
            ctx.aggregator[idx][i] += ntohl(meta_p->header.payload[i]);
        }

        // Spine 等待所有子交换机 (bitmap_mask)，叶子交换机等待所有主机 (ctrl_expected_bitmap)
        uint32_t expected = ctx.is_spine ? ctx.bitmap_mask : ctx.ctrl_expected_bitmap;
        if((ctx.arrival_state[idx] & expected) == expected){
            g_total_aggregations++;
            LOG("PIPELINE: aggregation COMPLETE, PSN=%u, is_spine=%d, total_agg=%lu\n",
                meta_p->psn, ctx.is_spine, g_total_aggregations);
            if(ctx.is_spine) {
                // AllReduce: Spine 广播给所有子交换机
                broadcast_roce_data(meta_p);
            } else {
                send_roce_data(ctx.root_conn, meta_p);
            }
            clear_state_data(meta_p->psn);
        } else {
            LOG("PIPELINE: waiting more, arrival=0x%x, need=0x%x\n", ctx.arrival_state[idx], expected);
        }

    }

    LOG("PIPELINE: done, PSN=%u\n", meta_p->psn);
}


/**
 * port
 * epoll from all devices and process the packets one by one, in one thread
 * 使用设备级别的 epoll，根据源 IP 分发到对应的连接
 * */
void epoll_process_packets(){
    LOG("epoll_process_packets: Starting main loop, %d devices\n", g_num_devices);

    const unsigned char *packet;
    struct pcap_pkthdr *pkthdr;

    while (true) {
        struct epoll_event events[MAX_EVENTS];
        int nfds = epoll_wait(ctx.epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            LOG("epoll_process_packets: epoll_wait error, exiting\n");
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int dev_idx = events[i].data.u32;
            if (dev_idx >= g_num_devices) {
                LOG("epoll_process_packets: invalid dev_idx=%d\n", dev_idx);
                continue;
            }

            pcap_t *handle = g_device_handles[dev_idx].handle;

            // 每次最多处理 256 个包，然后轮询其他设备
            int batch_count = 0;
            while (batch_count < 256 && (pcap_next_ex(handle, &pkthdr, &packet)) == 1) {
                memset(&meta, 0, sizeof(packet_metadata_t));

                // 从包中提取源 IP，查找对应的连接
                if (pkthdr->len < 34) {
                    batch_count++;
                    continue;
                }
                ipv4_header_t *ip = (ipv4_header_t *)(packet + 14);
                uint32_t src_ip = ip->src_ip;

                int conn_id = find_conn_by_src_ip(src_ip);
                if (conn_id < 0) {
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &src_ip, ip_str, sizeof(ip_str));
                    LOG("recv: unknown src_ip=%s, dropping\n", ip_str);
                    batch_count++;
                    continue;
                }

                meta.ingress_conn = conn_id;
                LOG("recv: dev=%d, conn=%d, len=%d\n", dev_idx, conn_id, pkthdr->len);
                pipeline(&meta, pkthdr, packet);
                print_stats_summary();
                batch_count++;
            }
        }
    }
}

/**
 * @brief 主函数
 */
int main(int argc, char *argv[]) {
    // 禁用 stdout/stderr 缓冲，确保日志实时输出
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

    printf("=== INC Switch Reduce (Non-Termination) ===\n");
    printf("Controller IP: %s, Switch ID: %d\n", controller_ip, switch_id);
    printf("PID: %d\n", getpid());

    // 初始化 CRC32 表
    init_crc32_table();

    // 初始化交换机上下文
    if (switch_context_init(&ctx, switch_id) < 0) {
        fprintf(stderr, "Failed to initialize switch context\n");
        return 1;
    }

    // is_spine 将从 Controller 发送的 YAML 配置中获取

    // 连接到控制器
    if (controller_init(&ctx, controller_ip) < 0) {
        fprintf(stderr, "Failed to connect to controller\n");
        switch_context_cleanup(&ctx);
        return 1;
    }

    printf("Switch started successfully\n");
    printf("Waiting for YAML config from controller...\n");

    // 等待 YAML 配置就绪
    pthread_mutex_lock(&ctx.config_mutex);
    while (!ctx.config_ready) {
        pthread_cond_wait(&ctx.config_cond, &ctx.config_mutex);
    }
    pthread_mutex_unlock(&ctx.config_mutex);

    printf("=== Config received ===\n");
    printf("  fan_in=%d\n", ctx.fan_in);
    printf("  is_spine=%d\n", ctx.is_spine);
    printf("  root_conn=%d\n", ctx.root_conn);
    printf("  bitmap_mask=0x%x\n", ctx.bitmap_mask);
    printf("=======================\n");

    // 打印连接详情
    for(int i = 0; i < ctx.fan_in; i++) {
        char my_ip[16], peer_ip[16];
        inet_ntop(AF_INET, &ctx.conns[i].my_ip, my_ip, sizeof(my_ip));
        inet_ntop(AF_INET, &ctx.conns[i].peer_ip, peer_ip, sizeof(peer_ip));
        printf("  conn[%d]: dev=%s, my=%s, peer=%s\n",
               i, ctx.conns[i].device, my_ip, peer_ip);
    }

    init_devices();
    
    epoll_process_packets();

    // 清理资源
    switch_context_cleanup(&ctx);

    printf("Switch stopped\n");
    return 0;
}
