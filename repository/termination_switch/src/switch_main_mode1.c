/**
 * @file switch_main_mode1.c
 * @brief Mode1 连接终结模式 - Switch 主程序
 *
 * 功能：
 * - 作为 RDMA 连接终结点，与 Host 建立 QP 连接
 * - 接收完整消息，执行聚合操作
 * - 支持分级 PS 架构（ROOT/LEAF）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "mode1/qp_manager.h"
#include "mode1/inc_engine.h"
#include "mode1/hierarchical_ps.h"

// 配置参数
#define DEFAULT_GID_IDX 1
#define DEFAULT_LISTEN_PORT 52400
#define MAX_PENDING_CONNS 16
// 使用 qp_manager.h 中定义的 RECV_BUF_PER_CONN 作为每个连接的接收窗口大小

// 全局状态
static volatile int g_running = 1;
static switch_roce_ctx_t *g_roce_ctx = NULL;
static hierarchical_ctx_t *g_hier_ctx = NULL;
static int g_is_root = 0;
static int g_expected_children = 1;  // ROOT 期望的子交换机数量（可通过 -c 参数配置）
static int g_expected_hosts = 2;     // LEAF 期望的 Host 数量（可通过 -n 参数配置）

// 信号处理
static void signal_handler(int sig) {
    (void)sig;
    printf("\n[Mode1] Received signal, shutting down...\n");
    g_running = 0;
}

// 创建 TCP 监听 socket
static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, MAX_PENDING_CONNS) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

// 连接到 Spine Switch (作为客户端) - 带重试机制
static int connect_to_spine(const char *spine_ip, int port) {
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port)
    };
    inet_pton(AF_INET, spine_ip, &addr.sin_addr);

    // 重试连接，最多尝试 30 次，每次间隔 1 秒
    for (int retry = 0; retry < 30; retry++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("socket");
            sleep(1);
            continue;
        }

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            printf("[Mode1] Connected to Spine after %d retries\n", retry);
            fflush(stdout);
            return fd;
        }

        close(fd);
        if (retry < 29) {
            printf("[Mode1] Retry %d: waiting for Spine...\n", retry + 1);
            fflush(stdout);
            sleep(1);
        }
    }

    return -1;
}

// 根据客户端 IP 选择下行设备索引
// 172.16.x.x -> 设备 0 (rxe_eth1)
// 10.10.x.x  -> 设备 1 (rxe_eth2)
static int select_downlink_by_ip(const char *ip_str) {
    if (strncmp(ip_str, "172.16.", 7) == 0) return 0;
    if (strncmp(ip_str, "10.10.", 6) == 0) return 1;
    return 0;  // 默认使用第一个设备
}

// 广播聚合结果给所有 Host
static void broadcast_to_hosts(uint32_t slot_id, void *data, uint32_t len) {
    uint32_t imm = (slot_id << 16) | (PRIM_ALLREDUCE << 14);
    struct ibv_wc wc[64];
    static int send_count = 0;

    // 将数据复制到已注册的发送缓冲区
    if (len > g_roce_ctx->send_buf_size) {
        fprintf(stderr, "[Mode1] Data too large for send buffer: %u > %u\n",
                len, g_roce_ctx->send_buf_size);
        return;
    }
    memcpy(g_roce_ctx->send_buf, data, len);

    // 每发送一定数量后清理 CQ，防止队列满
    send_count++;
    if (send_count % 1000 == 0) {
        // 多次轮询确保有足够空间
        for (int poll = 0; poll < 64; poll++) {
            int n = ibv_poll_cq(g_roce_ctx->send_cq, 64, wc);
            if (n <= 0) break;
        }
    }

    for (int i = 0; i < g_roce_ctx->host_count; i++) {
        qp_conn_t *conn = g_roce_ctx->host_conns[i];
        if (conn && conn->is_connected) {
            qp_post_send(g_roce_ctx, conn, slot_id, g_roce_ctx->send_buf, len, imm);
        }
    }
}

// 转发聚合结果给父交换机 (Leaf -> Spine)
static void forward_to_parent(uint32_t slot_id, void *data, uint32_t len) {
    uplink_ctx_t *uplink = g_roce_ctx->uplink;
    if (!uplink || !uplink->conn || !uplink->conn->is_connected) {
        fprintf(stderr, "[Mode1] No parent connection\n");
        return;
    }

    // 先清理发送 CQ，防止队列满（多次轮询确保有空间）
    struct ibv_wc wc[64];
    static int uplink_send_count = 0;
    uplink_send_count++;
    if (uplink_send_count % 1000 == 0) {
        for (int poll = 0; poll < 64; poll++) {
            int n = ibv_poll_cq(uplink->send_cq, 64, wc);
            if (n <= 0) break;
        }
    }

    uint32_t imm = (slot_id << 16) | (PRIM_ALLREDUCE << 14) | 0xFF;

    if (len > uplink->buf_size) {
        fprintf(stderr, "[Mode1] Data too large for uplink buffer\n");
        return;
    }
    memcpy(uplink->send_buf, data, len);

    uplink_post_send(uplink, slot_id, uplink->send_buf, len, imm);
}

// 广播结果给所有子交换机 (Spine -> Leaf)
static void broadcast_to_children(uint32_t slot_id, void *data, uint32_t len) {
    uint32_t imm = (slot_id << 16) | (PRIM_ALLREDUCE << 14) | 0xFE;
    struct ibv_wc wc[64];
    static int downlink_send_count = 0;

    // 使用多下行设备时
    if (g_roce_ctx->downlink_count > 0) {
        downlink_send_count++;
        for (int d = 0; d < g_roce_ctx->downlink_count; d++) {
            downlink_ctx_t *dl = g_roce_ctx->downlinks[d];
            if (!dl) continue;

            // 每发送一定数量后清理 CQ，防止队列满
            if (downlink_send_count % 1000 == 0) {
                for (int poll = 0; poll < 64; poll++) {
                    int n = ibv_poll_cq(dl->send_cq, 64, wc);
                    if (n <= 0) break;
                }
            }

            if (len > dl->buf_size) continue;
            memcpy(dl->send_buf, data, len);

            for (int i = 0; i < dl->conn_count; i++) {
                qp_conn_t *conn = dl->conns[i];
                if (conn && conn->is_connected) {
                    int ret = downlink_post_send(dl, conn, slot_id, dl->send_buf, len, imm);
                    if (ret != 0) {
                        fprintf(stderr, "[Mode1] downlink_post_send failed: %d\n", ret);
                    }
                }
            }
        }
        return;
    }

    // 原有逻辑：使用默认设备
    if (len > g_roce_ctx->send_buf_size) {
        fprintf(stderr, "[Mode1] Data too large for send buffer\n");
        return;
    }
    memcpy(g_roce_ctx->send_buf, data, len);

    for (int i = 0; i < g_roce_ctx->child_count; i++) {
        qp_conn_t *conn = g_roce_ctx->child_conns[i];
        if (conn && conn->is_connected) {
            qp_post_send(g_roce_ctx, conn, slot_id, g_roce_ctx->send_buf, len, imm);
        }
    }
    printf("[Mode1] Broadcast result to %d children\n", g_roce_ctx->child_count);
}

// 消息处理循环
static void *message_loop(void *arg) {
    (void)arg;
    struct ibv_wc wc[16];

    printf("[Mode1] Message loop started\n");

    while (g_running) {
        // ROOT: 轮询所有下行设备的 CQ
        if (g_is_root && g_roce_ctx->downlink_count > 0) {
            for (int d = 0; d < g_roce_ctx->downlink_count; d++) {
                downlink_ctx_t *dl = g_roce_ctx->downlinks[d];
                if (!dl || dl->conn_count == 0) continue;  // 跳过没有连接的设备

                int n = downlink_poll_recv_cq(dl, wc, 16);
                for (int i = 0; i < n; i++) {
                    if (wc[i].status != IBV_WC_SUCCESS) {
                        fprintf(stderr, "WC error on device %d: %d (wr_id=%lu)\n", d, wc[i].status, wc[i].wr_id);
                        continue;
                    }

                    // 使用 wr_id 直接计算缓冲区位置，确保不同连接使用不同的缓冲区
                    void *data = (char*)dl->recv_buf + wc[i].wr_id * 65536;
                    uint32_t len = wc[i].byte_len;
                    uint32_t imm = ntohl(wc[i].imm_data);

                    uint16_t slot_id = (imm >> 16) & 0xFFFF;
                    int prim = (imm >> 14) & 0x3;
                    int op = (imm >> 12) & 0x3;
                    int dtype = (imm >> 8) & 0xF;
                    int rank = imm & 0xFF;
                    int from_child = (rank == 0xFF);

                    printf("[Mode1] ROOT recv from downlink %d: slot=%u, from_child=%d, len=%u\n",
                           d, slot_id, from_child, len);

                    int complete = inc_engine_submit(slot_id, from_child ? (100 + d) : rank,
                                                     data, len, prim, op, dtype, g_expected_children);
                    if (complete == 1) {
                        uint32_t result_len;
                        void *result = inc_engine_get_result(slot_id, &result_len);
                        if (result) {
                            broadcast_to_children(slot_id, result, result_len);
                            inc_engine_reset_slot(slot_id);
                        }
                    }

                    // 补充接收请求
                    int conn_idx = wc[i].wr_id / RECV_BUF_PER_CONN;
                    if (conn_idx < dl->conn_count && dl->conns[conn_idx]) {
                        downlink_post_recv(dl, dl->conns[conn_idx], wc[i].wr_id, data, 65536);
                    }
                }
            }
        } else {
            // 原有逻辑：轮询默认 CQ
            int n = qp_poll_recv_cq(g_roce_ctx, wc, 16);
            for (int i = 0; i < n; i++) {
                if (wc[i].status != IBV_WC_SUCCESS) {
                    fprintf(stderr, "WC error: %d\n", wc[i].status);
                    continue;
                }

                void *data = qp_get_recv_buf(g_roce_ctx, wc[i].wr_id);
                uint32_t len = wc[i].byte_len;
                uint32_t imm = ntohl(wc[i].imm_data);

                uint16_t slot_id = (imm >> 16) & 0xFFFF;
                int prim = (imm >> 14) & 0x3;
                int op = (imm >> 12) & 0x3;
                int dtype = (imm >> 8) & 0xF;
                int rank = imm & 0xFF;
                int from_child = (rank == 0xFF);

                if (g_is_root) {
                    printf("[Mode1] ROOT recv: slot=%u, from_child=%d, len=%u\n",
                           slot_id, from_child, len);
                    int complete = inc_engine_submit(slot_id, from_child ? 100 : rank,
                                                     data, len, prim, op, dtype, g_expected_children);
                    if (complete == 1) {
                        uint32_t result_len;
                        void *result = inc_engine_get_result(slot_id, &result_len);
                        if (result) {
                            broadcast_to_children(slot_id, result, result_len);
                            inc_engine_reset_slot(slot_id);
                        }
                    }
                } else {
                    printf("[Mode1] LEAF recv from host: slot=%u, rank=%d, len=%u\n",
                           slot_id, rank, len);
                    fflush(stdout);
                    int expected = g_expected_hosts;  // 使用配置的期望 Host 数量
                    printf("[Mode1] LEAF: expected=%d hosts, calling inc_engine_submit\n", expected);
                    fflush(stdout);
                    int complete = inc_engine_submit(slot_id, rank, data, len,
                                                     prim, op, dtype, expected);
                    printf("[Mode1] LEAF: inc_engine_submit returned %d\n", complete);
                    fflush(stdout);
                    if (complete == 1) {
                        uint32_t result_len;
                        void *result = inc_engine_get_result(slot_id, &result_len);
                        printf("[Mode1] LEAF: slot %u complete, result=%p, len=%u\n", slot_id, result, result_len);
                        fflush(stdout);
                        if (result) {
                            int has_uplink = (g_roce_ctx->uplink != NULL);
                            int has_conn = has_uplink && (g_roce_ctx->uplink->conn != NULL);
                            int is_conn = has_conn && g_roce_ctx->uplink->conn->is_connected;
                            printf("[Mode1] LEAF: uplink=%d, conn=%d, connected=%d\n", has_uplink, has_conn, is_conn);
                            fflush(stdout);
                            if (has_uplink && has_conn && is_conn) {
                                printf("[Mode1] LEAF: forwarding slot %u to parent\n", slot_id);
                                fflush(stdout);
                                forward_to_parent(slot_id, result, result_len);
                                // 转发后立即重置 slot，防止新消息被错误聚合
                                inc_engine_reset_slot(slot_id);
                            } else {
                                broadcast_to_hosts(slot_id, result, result_len);
                                inc_engine_reset_slot(slot_id);
                            }
                        }
                    }
                }
            }
        }

        // LEAF: 轮询上行 CQ (来自父交换机)
        if (!g_is_root && g_roce_ctx->uplink) {
            int n = uplink_poll_recv_cq(g_roce_ctx->uplink, wc, 16);
            for (int i = 0; i < n; i++) {
                if (wc[i].status != IBV_WC_SUCCESS) continue;

                int slot_idx = wc[i].wr_id - 1000;
                void *data = (char*)g_roce_ctx->uplink->recv_buf + slot_idx * 65536;
                uint32_t len = wc[i].byte_len;
                uint32_t imm = ntohl(wc[i].imm_data);
                uint16_t slot_id = (imm >> 16) & 0xFFFF;

                printf("[Mode1] LEAF recv from parent: slot=%u, len=%u\n", slot_id, len);
                broadcast_to_hosts(slot_id, data, len);
                inc_engine_reset_slot(slot_id);

                // 补充接收请求
                uplink_post_recv(g_roce_ctx->uplink, wc[i].wr_id, data, 65536);
            }
        }

        usleep(100);
    }

    return NULL;
}

// 打印用法
static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -p <port>   Listen port (default: %d)\n", DEFAULT_LISTEN_PORT);
    printf("  -g <idx>    GID index (default: %d)\n", DEFAULT_GID_IDX);
    printf("  -r          Run as ROOT switch\n");
    printf("  -c <num>    Expected children count for ROOT (default: 1)\n");
    printf("  -n <num>    Expected hosts count for LEAF (default: 2)\n");
    printf("  -d <devs>   Downlink RDMA devices for ROOT (comma-separated, e.g., rxe_eth1,rxe_eth2)\n");
    printf("  -s <ip>     Spine switch IP (for LEAF mode)\n");
    printf("  -u <dev>    Uplink RDMA device (e.g., rxe_eth1)\n");
    printf("  -h          Show this help\n");
}

int main(int argc, char *argv[]) {
    // 初始化随机种子，使用时间和进程ID确保不同进程有不同的种子
    srand(time(NULL) ^ getpid());

    int listen_port = DEFAULT_LISTEN_PORT;
    int gid_idx = DEFAULT_GID_IDX;
    int is_root = 0;
    int expected_children = 1;
    int expected_hosts = 2;
    char *spine_ip = NULL;
    char *uplink_dev = NULL;
    char *downlink_devs = NULL;

    // 解析参数
    int opt;
    while ((opt = getopt(argc, argv, "p:g:rc:n:d:s:u:h")) != -1) {
        switch (opt) {
        case 'p': listen_port = atoi(optarg); break;
        case 'g': gid_idx = atoi(optarg); break;
        case 'r': is_root = 1; break;
        case 'c': expected_children = atoi(optarg); break;
        case 'n': expected_hosts = atoi(optarg); break;
        case 'd': downlink_devs = optarg; break;
        case 's': spine_ip = optarg; break;
        case 'u': uplink_dev = optarg; break;
        case 'h':
        default:
            print_usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    printf("=== Mode1: Connection Termination Switch ===\n");
    printf("Listen port: %d\n", listen_port);
    printf("GID index: %d\n", gid_idx);
    printf("Role: %s\n", is_root ? "ROOT" : "LEAF");
    if (is_root) {
        printf("Expected children: %d\n", expected_children);
    } else {
        printf("Expected hosts: %d\n", expected_hosts);
    }
    printf("============================================\n\n");
    fflush(stdout);

    // 调试：打印 spine_ip 和 uplink_dev
    printf("[Debug] spine_ip=%s, uplink_dev=%s, is_root=%d\n",
           spine_ip ? spine_ip : "NULL",
           uplink_dev ? uplink_dev : "NULL",
           is_root);
    fflush(stdout);

    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 设置全局变量
    g_is_root = is_root;
    g_expected_children = expected_children;
    g_expected_hosts = expected_hosts;

    // 初始化 RoCE 上下文
    g_roce_ctx = qp_manager_init(NULL, gid_idx);
    if (!g_roce_ctx) {
        fprintf(stderr, "Failed to init QP manager\n");
        return 1;
    }

    // 初始化层级上下文
    switch_role_t role = is_root ? SWITCH_ROLE_ROOT : SWITCH_ROLE_LEAF;
    g_hier_ctx = hierarchical_init(role, 0);

    // 初始化计算引擎
    inc_engine_init(MAX_AGG_SLOTS);

    // ROOT: 初始化下行设备（用于连接不同网段的 Leaf）
    if (is_root && downlink_devs) {
        char *devs_copy = strdup(downlink_devs);
        char *token = strtok(devs_copy, ",");
        while (token && g_roce_ctx->downlink_count < MAX_SWITCHES) {
            printf("[Mode1] Initializing downlink device: %s\n", token);
            downlink_ctx_t *dl = downlink_ctx_init(token, gid_idx);
            if (dl) {
                g_roce_ctx->downlinks[g_roce_ctx->downlink_count++] = dl;
            } else {
                fprintf(stderr, "Failed to init downlink device %s\n", token);
            }
            token = strtok(NULL, ",");
        }
        free(devs_copy);
        printf("[Mode1] Initialized %d downlink devices\n", g_roce_ctx->downlink_count);
        fflush(stdout);
    }

    // 创建监听 socket
    int listen_fd = create_listen_socket(listen_port);
    if (listen_fd < 0) {
        fprintf(stderr, "Failed to create listen socket\n");
        goto cleanup;
    }
    printf("[Mode1] Listening on port %d\n", listen_port);
    fflush(stdout);

    // LEAF: 连接到 Spine Switch
    if (!is_root && spine_ip && uplink_dev) {
        printf("[Mode1] Connecting to Spine at %s:%d via %s...\n",
               spine_ip, listen_port, uplink_dev);
        fflush(stdout);

        // 初始化上行连接上下文（使用独立 RDMA 设备）
        printf("[Mode1] Initializing uplink context...\n");
        fflush(stdout);
        g_roce_ctx->uplink = uplink_ctx_init(uplink_dev, gid_idx);
        if (!g_roce_ctx->uplink) {
            fprintf(stderr, "Failed to init uplink context\n");
            goto cleanup;
        }
        printf("[Mode1] Uplink context initialized\n");
        fflush(stdout);

        printf("[Mode1] Connecting TCP to Spine...\n");
        fflush(stdout);
        int spine_fd = connect_to_spine(spine_ip, listen_port);
        if (spine_fd < 0) {
            fprintf(stderr, "Failed to connect to Spine\n");
            goto cleanup;
        }
        printf("[Mode1] TCP connected, fd=%d\n", spine_fd);
        fflush(stdout);

        qp_conn_t *parent_conn = uplink_conn_create(g_roce_ctx->uplink);
        if (!parent_conn) {
            close(spine_fd);
            goto cleanup;
        }

        if (uplink_conn_exchange(g_roce_ctx->uplink, spine_fd) == 0) {
            g_roce_ctx->parent_conn = parent_conn;
            hierarchical_set_parent(g_hier_ctx, -1);

            // 投递接收请求
            for (int j = 0; j < RECV_BUF_PER_CONN; j++) {
                uplink_post_recv(g_roce_ctx->uplink, 1000 + j,
                    (char*)g_roce_ctx->uplink->recv_buf + j * 65536, 65536);
            }
            printf("[Mode1] Connected to Spine, posted %d recv WRs\n", RECV_BUF_PER_CONN);
            fflush(stdout);
            // 等待ROOT投递recv WRs
            printf("[Mode1] Waiting for ROOT to post recv WRs...\n");
            fflush(stdout);
            usleep(500000);  // 500ms
            printf("[Mode1] Ready to forward data\n");
            fflush(stdout);
        } else {
            fprintf(stderr, "Failed to exchange QP with Spine\n");
            goto cleanup;
        }
    }

    // 启动消息处理线程
    pthread_t msg_thread;
    pthread_create(&msg_thread, NULL, message_loop, NULL);

    // 主循环：接受连接
    int conn_id = 0;
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (g_running) perror("accept");
            continue;
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        printf("[Mode1] New connection from %s (conn_id=%d)\n", ip_str, conn_id);
        fflush(stdout);

        // ROOT 使用多下行设备时，根据 IP 选择设备
        if (is_root && g_roce_ctx->downlink_count > 0) {
            int dl_idx = select_downlink_by_ip(ip_str);
            if (dl_idx >= g_roce_ctx->downlink_count) dl_idx = 0;

            downlink_ctx_t *dl = g_roce_ctx->downlinks[dl_idx];
            printf("[Mode1] Using downlink device %d for %s\n", dl_idx, ip_str);
            fflush(stdout);

            qp_conn_t *conn = downlink_conn_create(dl, conn_id);
            if (!conn) {
                fprintf(stderr, "[Mode1] Failed to create downlink conn %d\n", conn_id);
                close(client_fd);
                conn_id++;
                continue;
            }
            printf("[Mode1] Created downlink conn %d, QPN=%u\n", conn_id, conn->local_info.qpn);
            fflush(stdout);

            if (downlink_conn_exchange(dl, conn, client_fd) == 0) {
                dl->conns[dl->conn_count++] = conn;
                g_roce_ctx->child_conns[g_roce_ctx->child_count++] = conn;
                hierarchical_add_child(g_hier_ctx, conn_id);
                printf("[Mode1] Added child switch conn %d on downlink %d\n", conn_id, dl_idx);

                // 投递接收请求 - 使用设备内的连接索引而不是全局conn_id
                int local_conn_idx = dl->conn_count - 1;  // 刚添加的连接索引
                for (int j = 0; j < RECV_BUF_PER_CONN; j++) {
                    uint64_t wr_id = local_conn_idx * RECV_BUF_PER_CONN + j;
                    void *buf = (char*)dl->recv_buf + wr_id * 65536;
                    downlink_post_recv(dl, conn, wr_id, buf, 65536);
                }
                printf("[Mode1] Posted %d recv WRs for conn %d (local_idx=%d)\n",
                       RECV_BUF_PER_CONN, conn_id, local_conn_idx);
            } else {
                fprintf(stderr, "[Mode1] downlink_conn_exchange failed for conn %d\n", conn_id);
                if (conn->qp) ibv_destroy_qp(conn->qp);
                free(conn);
            }
        } else {
            // 原有逻辑：使用默认设备
            qp_conn_t *conn = qp_conn_create(g_roce_ctx, conn_id);
            if (!conn) {
                close(client_fd);
                conn_id++;
                continue;
            }

            if (qp_conn_exchange(g_roce_ctx, conn, client_fd, 1) == 0) {
                if (is_root) {
                    g_roce_ctx->child_conns[g_roce_ctx->child_count++] = conn;
                    hierarchical_add_child(g_hier_ctx, conn_id);
                    printf("[Mode1] Added child switch conn %d\n", conn_id);
                } else {
                    g_roce_ctx->host_conns[g_roce_ctx->host_count++] = conn;
                    hierarchical_add_host(g_hier_ctx, conn_id);
                }

                for (int j = 0; j < RECV_BUF_PER_CONN; j++) {
                    uint64_t wr_id = conn_id * RECV_BUF_PER_CONN + j;
                    void *buf = qp_get_recv_buf(g_roce_ctx, wr_id);
                    qp_post_recv(g_roce_ctx, conn, wr_id, buf, 65536);
                }
                printf("[Mode1] Posted %d recv WRs for conn %d\n", RECV_BUF_PER_CONN, conn_id);
            } else {
                qp_conn_destroy(conn);
            }
        }

        conn_id++;
    }

    pthread_join(msg_thread, NULL);
    close(listen_fd);

cleanup:
    inc_engine_destroy();
    hierarchical_destroy(g_hier_ctx);
    qp_manager_destroy(g_roce_ctx);

    printf("[Mode1] Shutdown complete\n");
    return 0;
}
