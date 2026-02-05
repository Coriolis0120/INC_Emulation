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

// ==================== 全局上下文 ====================
static switch_context_t ctx;
static packet_metadata_t meta;

static void init_devices(void) {

    // for each port
    for(int i = 0; i < ctx.fan_in; i++) {

        char errbuf[PCAP_ERRBUF_SIZE];
        ctx.conns[i].handle = pcap_create(ctx.conns[i].device, errbuf);
        pcap_set_snaplen(ctx.conns[i].handle, BUFSIZ);
        pcap_set_promisc(ctx.conns[i].handle, 1);
        pcap_set_timeout(ctx.conns[i].handle, 1);  // 1ms timeout
        pcap_set_immediate_mode(ctx.conns[i].handle, 1);
        pcap_setnonblock(ctx.conns[i].handle, 1, errbuf);
        if (pcap_activate(ctx.conns[i].handle) != 0) {
            fprintf(stderr, "pcap_activate failed: %s\n", pcap_geterr(ctx.conns[i].handle));
            return;
        }
        if (ctx.conns[i].handle == NULL) {
            fprintf(stderr, "Could not open device: %s, err: %s\n", ctx.conns[i].device, errbuf);
            return;
        }

        struct bpf_program fp;
        char filter_exp[100];
        char ip_str[INET_ADDRSTRLEN];
        
        // RoCEv2 filter
        if (!inet_ntop(AF_INET, &(ctx.conns[i].peer_ip), ip_str, sizeof(ip_str))) {
            perror("inet_ntop failed");
            continue;
        }

        printf("port %d: device: %s\n",i, ctx.conns[i].device);
        snprintf(filter_exp, sizeof(filter_exp), "udp port 4791 and src host %s", ip_str);

        if (pcap_compile(ctx.conns[i].handle, &fp, filter_exp, 0, PCAP_NETMASK_UNKNOWN) == -1) {
            fprintf(stderr, "Filter error: %s\n", pcap_geterr(ctx.conns[i].handle));
            continue;
        }

        if (pcap_setfilter(ctx.conns[i].handle, &fp) == -1) {
            fprintf(stderr, "Set filter error: %s\n", pcap_geterr(ctx.conns[i].handle));
            pcap_freecode(&fp);
            continue;
        }
        pcap_freecode(&fp);

        // add fd into epoll
        int fd = pcap_get_selectable_fd(ctx.conns[i].handle);
        if (fd == -1) {
            fprintf(stderr, "Cannot get selectable file descriptor for %s\n", ctx.conns[i].device);
            continue;
        }
        
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.u32 = i;
        
        
        if (epoll_ctl(ctx.epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
            perror("epoll_ctl failed");
            close(ctx.epoll_fd);
            return;
        }

    }

}

static void broadcast_roce_data(packet_metadata_t *meta_p){
    int size = 58 + PAYLOAD_LEN;
    uint8_t *packet = (uint8_t *)meta_p->header.eth;

    for(int i = 0; i < ctx.fan_in; i++){
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
        meta_p->header.bth->apsn = htonl((1 << 31) | (meta_p->psn & 0x00FFFFFF));

        uint32_t *icrc = (uint32_t *)(packet + size - 4);
        *icrc = compute_icrc(-1, packet);

        if(pcap_sendpacket(conn->handle, packet, size) == -1) {
            fprintf(stderr, "Error sending packet: %s\n", pcap_geterr(conn->handle));
        }
    }
}

static void send_roce_ack(uint32_t conn_id, packet_metadata_t *meta_p){
    connection_t *conn = &ctx.conns[conn_id];
    int size = 62;
    uint8_t *packet = (uint8_t *)meta_p->header.eth;

    memcpy(meta_p->header.eth->dst_mac, conn->peer_mac, 6);
    memcpy(meta_p->header.eth->src_mac, conn->my_mac, 6);

    meta_p->header.ip->src_ip = conn->my_ip;
    meta_p->header.ip->dst_ip = conn->peer_ip;

    meta_p->header.udp->src_port = htons(conn->my_port);
    meta_p->header.udp->dst_port = htons(conn->peer_port);

    meta_p->header.bth->qpn = htonl(conn->peer_qp);
    meta_p->header.bth->apsn = htonl(meta_p->psn & 0x00FFFFFF);

    uint32_t *icrc = (uint32_t *)(packet + size - 4);
    *icrc = compute_icrc(-1, packet);

    if(pcap_sendpacket(conn->handle, packet, size) == -1) {
        fprintf(stderr, "Error sending packet: %s\n", pcap_geterr(conn->handle));
    }
}

static inline void clear_state_data(uint32_t psn){
    uint32_t idx = Idx(psn + WINDOW_SIZE);
    ctx.arrival_state[idx] = 0;
}

/**
 * simulation of p4 switch — broadcast mode
 */
void pipeline(packet_metadata_t *meta_p, const struct pcap_pkthdr *pkthdr, const uint8_t *packet) {

    // parser: extract header info
    printf("[PIPELINE] Starting packet processing, ingress_conn=%d\n", meta_p->ingress_conn);
    fflush(stdout);

    meta_p->header.eth = (eth_header_t*)packet;
    meta_p->header.ip = (ipv4_header_t*)(packet + sizeof(eth_header_t));
    meta_p->header.udp = (udp_header_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t));
    meta_p->header.bth = (bth_header_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(udp_header_t));
    meta_p->psn = ntohl(meta_p->header.bth->apsn) & 0x00FFFFFF;
    printf("[PIPELINE] Extracted headers: PSN=%u, opcode=0x%02x\n", meta_p->psn, meta_p->header.bth->opcode);
    fflush(stdout);
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

    uint32_t idx = Idx(meta_p->psn);

    if(meta_p->type == PACKET_TYPE_DATA){
        // DATA 来自 root，检查 epsn 后直接广播
        printf("[PIPELINE] Processing DATA packet, PSN=%u\n", meta_p->psn);
        fflush(stdout);
        if((ctx.data_epsn[meta_p->ingress_conn] != meta_p->psn)
            &&(!(ctx.arrival_state[idx] & MASK_CONN(meta_p->ingress_conn)))){
            return; // 超前epsn，丢弃
        }

        ctx.arrival_state[idx] |= MASK_CONN(meta_p->ingress_conn);
        clear_state_data(meta_p->psn);
        ctx.data_epsn[meta_p->ingress_conn]++;
        broadcast_roce_data(meta_p);

        printf("[PIPELINE] DATA: broadcast completed, PSN=%u\n", meta_p->psn);
        fflush(stdout);

    }
    else if(meta_p->type == PACKET_TYPE_ACK){
        printf("[PIPELINE] Processing ACK packet, PSN=%u, from port=%d\n", meta_p->psn, meta_p->ingress_conn);
        fflush(stdout);

        ctx.acked_psn[meta_p->ingress_conn] = meta_p->psn;

        // Spine 不需要转发 ACK（它是广播源头）
        if(ctx.is_spine) {
            printf("[PIPELINE] ACK: Spine received ACK, PSN=%u (no forwarding)\n", meta_p->psn);
            fflush(stdout);
        } else {
            // Leaf: 取 bitmap 中所有 port 的 acked_psn 最小值作为回给 root 的 ACK PSN
            uint32_t min_psn = 0x7fffffff;
            for(int i = 0; i < ctx.fan_in; i++){
                if(!(ctx.bitmap_mask & MASK_CONN(i)))
                    continue;
                if(ctx.acked_psn[i] < min_psn)
                    min_psn = ctx.acked_psn[i];
            }

            meta_p->psn = min_psn;
            send_roce_ack(ctx.root_conn, meta_p);
            printf("[PIPELINE] ACK: sent ACK to root, min_PSN=%u\n", min_psn);
            fflush(stdout);
        }

    }

    printf("[PIPELINE] Completed packet processing, PSN=%u\n", meta_p->psn);
    fflush(stdout);
}


/**
 * port
 * epoll from all ports and process the packets one by one, in one thread 
 * */
void epoll_process_packets(){

    const unsigned char *packet;
    struct pcap_pkthdr *pkthdr;
    
    while (true) {
        struct epoll_event events[MAX_EVENTS];
        int nfds = epoll_wait(ctx.epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            memset(&meta, 0, sizeof(packet_metadata_t));
            meta.ingress_conn = events[i].data.u32;

            while ((pcap_next_ex(ctx.conns[meta.ingress_conn].handle, &pkthdr, &packet)) == 1) {
                printf("recv packet from port %d\n",meta.ingress_conn);
                fflush(stdout);
                pipeline(&meta, pkthdr, packet);
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

    printf("=== INC Switch Broadcast (Non-Termination) ===\n");
    printf("Controller IP: %s, Switch ID: %d\n", controller_ip, switch_id);

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

    printf("Config received: fan_in=%d, is_spine=%d, root_conn=%d, bitmap_mask=0x%x\n",
           ctx.fan_in, ctx.is_spine, ctx.root_conn, ctx.bitmap_mask);

    init_devices();
    
    epoll_process_packets();

    // 清理资源
    switch_context_cleanup(&ctx);

    printf("Switch stopped\n");
    return 0;
}
