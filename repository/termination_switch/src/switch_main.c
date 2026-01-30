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
#include "switch_context.h"
#include "controller_comm.h"
#include "parameter.h"
#include "util.h"

// ==================== 宏定义 ====================
#define Idx(psn) ((psn) % SWITCH_ARRAY_LENGTH)

// 调试开关：设为 0 可禁用详细日志以提高性能
#define DEBUG_VERBOSE 1

// 重传相关参数
#define RETRANSMIT_TIMEOUT_US 2000000   // 2s 超时
#define RETRANSMIT_CHECK_INTERVAL_US 500000  // 500ms 检查间隔
#define MAX_RETRANSMIT_BATCH 8  // 每批最多重传的数据包数量（减少以降低发送压力）
#define RETRANSMIT_PACKET_DELAY_US 10000  // 每个重传包之间的延迟 10ms

// 重传数据包信息结构体（用于避免持锁发送）
typedef struct {
    connection_t *conn;
    uint32_t buffer[1024];
    int len;
    int psn;
    int opcode;
} retransmit_info_t;

#if DEBUG_VERBOSE
#define DBG_PRINT(...) printf(__VA_ARGS__)
#else
#define DBG_PRINT(...) do {} while(0)
#endif

// 注意：使用 util.h 中定义的 packet_type_t 枚举
// PACKET_TYPE_DATA = 0
// PACKET_TYPE_ACK = 1
// PACKET_TYPE_NAK = 2
// PACKET_TYPE_DATA_SINGLE = 3

// ==================== 全局上下文 ====================
static switch_context_t g_switch_ctx;

// ==================== 前向声明 ====================
static void packet_handler(uint8_t *user_data, const struct pcap_pkthdr *pkthdr, const uint8_t *packet);
static void *pcap_receiver_thread(void *arg);
static void *worker_thread(void *arg);
static void *sender_thread(void *arg);
static void process_packet(switch_context_t *ctx, queued_packet_t *pkt);

// ==================== 无锁队列操作 ====================

// 内存屏障宏
#define MEMORY_BARRIER() __sync_synchronize()

/**
 * @brief 初始化数据包队列
 */
static void pkt_queue_init(packet_queue_t *q) {
    q->head = 0;
    q->tail = 0;
    q->dropped = 0;
    MEMORY_BARRIER();
}

/**
 * @brief 入队（非阻塞，队列满时丢弃）
 * @return 1 成功，0 队列满
 */
static int pkt_queue_push(packet_queue_t *q, queued_packet_t *pkt) {
    unsigned int head = q->head;
    unsigned int next_head = (head + 1) & (PKT_QUEUE_SIZE - 1);
    MEMORY_BARRIER();
    unsigned int tail = q->tail;

    if (next_head == tail) {
        // 队列满，丢弃
        __sync_fetch_and_add(&q->dropped, 1);
        return 0;
    }

    // 复制数据
    memcpy(&q->packets[head], pkt, sizeof(queued_packet_t));
    MEMORY_BARRIER();
    q->head = next_head;
    return 1;
}

/**
 * @brief 出队（非阻塞）
 * @return 1 成功，0 队列空
 */
static int pkt_queue_pop(packet_queue_t *q, queued_packet_t *pkt) {
    unsigned int tail = q->tail;
    MEMORY_BARRIER();
    unsigned int head = q->head;

    if (tail == head) {
        // 队列空
        return 0;
    }

    memcpy(pkt, &q->packets[tail], sizeof(queued_packet_t));
    MEMORY_BARRIER();
    q->tail = (tail + 1) & (PKT_QUEUE_SIZE - 1);
    return 1;
}

// ==================== 发送队列操作函数 ====================

/**
 * @brief 初始化发送队列
 */
static void send_queue_init(send_queue_t *q) {
    q->head = 0;
    q->tail = 0;
    q->dropped = 0;
    pthread_mutex_init(&q->enqueue_mutex, NULL);
    MEMORY_BARRIER();
}

/**
 * @brief 发送队列入队（多生产者安全）
 * @return 1 成功，0 队列满
 */
static int send_queue_push(send_queue_t *q, send_packet_t *pkt) {
    pthread_mutex_lock(&q->enqueue_mutex);

    unsigned int head = q->head;
    unsigned int next_head = (head + 1) & (SEND_QUEUE_SIZE - 1);
    unsigned int tail = q->tail;

    if (next_head == tail) {
        pthread_mutex_unlock(&q->enqueue_mutex);
        __sync_fetch_and_add(&q->dropped, 1);
        return 0;
    }

    memcpy(&q->packets[head], pkt, sizeof(send_packet_t));
    MEMORY_BARRIER();
    q->head = next_head;

    pthread_mutex_unlock(&q->enqueue_mutex);
    return 1;
}

/**
 * @brief 发送队列出队（单消费者）
 * @return 1 成功，0 队列空
 */
static int send_queue_pop(send_queue_t *q, send_packet_t *pkt) {
    unsigned int tail = q->tail;
    MEMORY_BARRIER();
    unsigned int head = q->head;

    if (tail == head) {
        return 0;
    }

    memcpy(pkt, &q->packets[tail], sizeof(send_packet_t));
    MEMORY_BARRIER();
    q->tail = (tail + 1) & (SEND_QUEUE_SIZE - 1);
    return 1;
}

/**
 * @brief 清理发送队列
 */
static void send_queue_cleanup(send_queue_t *q) {
    pthread_mutex_destroy(&q->enqueue_mutex);
}

// ==================== 辅助函数 ====================

/**
 * @brief 将 Send with Immediate opcode 转换为普通 Send opcode
 *
 * Switch 在广播数据时不发送 Immediate Data，所以需要将 opcode 转换为普通 Send。
 * 否则 Host 的 RDMA 硬件会将 4 字节 payload 误解为 Immediate Data。
 */
static uint8_t strip_immediate_opcode(uint8_t opcode) {
    switch (opcode) {
        case RDMA_OPCODE_SEND_ONLY_WITH_IMM:  // 0x05 -> 0x04
            return RDMA_OPCODE_SEND_ONLY;
        case RDMA_OPCODE_SEND_LAST_WITH_IMM:  // 0x03 -> 0x02
            return RDMA_OPCODE_SEND_LAST;
        default:
            return opcode;
    }
}

/**
 * @brief 构建并发送以太网数据包
 */
static int send_packet(connection_t *conn, int type, uint32_t *data, int len, uint32_t psn, int packet_type) {
    uint8_t packet[4096];

    // 获取交换机 ID 用于日志
    int sw_id = g_switch_ctx.switch_id;

    // 转换 IP 地址用于日志
    char my_ip_str[INET_ADDRSTRLEN], peer_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &conn->my_ip, my_ip_str, sizeof(my_ip_str));
    inet_ntop(AF_INET, &conn->peer_ip, peer_ip_str, sizeof(peer_ip_str));

    const char *type_str = (type == PACKET_TYPE_ACK) ? "ACK" :
                           (type == PACKET_TYPE_NAK) ? "NAK" :
                           (type == PACKET_TYPE_DATA) ? "DATA" : "DATA_SINGLE";

    DBG_PRINT("[SW%d] send_packet: type=%s, PSN=%u, len=%d, opcode=0x%02x, %s -> %s\n",
           sw_id, type_str, psn, len, packet_type, my_ip_str, peer_ip_str);

    int size = build_eth_packet(
        packet, type, (char*)data, len * sizeof(uint32_t),
        conn->my_mac, conn->peer_mac,
        conn->my_ip, conn->peer_ip,
        conn->my_port, conn->peer_port,
        conn->peer_qp, psn, psn + 1, packet_type, NULL
    );

    // 加锁保护 pcap_sendpacket（pcap 不是线程安全的）
    pthread_mutex_lock(conn->send_mutex);
    int ret = pcap_sendpacket(conn->send_handle, packet, size);
    pthread_mutex_unlock(conn->send_mutex);

    if (ret == -1) {
        fprintf(stderr, "[SW%d] ERROR send_packet failed: %s\n", sw_id, pcap_geterr(conn->send_handle));
        return -1;
    }

    return 0;
}

/**
 * @brief 异步发送数据包 - 构建帧并入队到发送队列
 *
 * 与 send_packet 不同，此函数不直接发送，而是将帧入队
 * 由专用的 sender_thread 负责实际发送
 */
static int send_packet_async(connection_t *conn, int type, uint32_t *data, int len, uint32_t psn, int packet_type) {
    switch_context_t *ctx = &g_switch_ctx;
    int device_id = conn->device_id;

    // 调试：检查 device_id 是否有效
    if (device_id < 0 || device_id >= ctx->num_devices) {
        fprintf(stderr, "[ASYNC_SEND] ERROR: invalid device_id=%d (num_devices=%d)\n",
                device_id, ctx->num_devices);
        return -1;
    }

    send_packet_t spkt;
    spkt.conn_id = -1;  // 不需要，帧已包含完整信息

    int size = build_eth_packet(
        spkt.frame, type, (char*)data, len * sizeof(uint32_t),
        conn->my_mac, conn->peer_mac,
        conn->my_ip, conn->peer_ip,
        conn->my_port, conn->peer_port,
        conn->peer_qp, psn, psn + 1, packet_type, NULL
    );
    spkt.frame_len = size;

    if (!send_queue_push(&ctx->send_queues[device_id], &spkt)) {
        return -1;  // 队列满
    }
    return 0;
}

/**
 * @brief 转发数据包（支持单播和多播）
 */
static void forwarding(switch_context_t *ctx, rule_t *rule, uint32_t psn, int type, uint32_t *data, int len, int packet_type) {
    int sw_id = ctx->switch_id;
    const char *type_str = (type == PACKET_TYPE_ACK) ? "ACK" :
                           (type == PACKET_TYPE_NAK) ? "NAK" :
                           (type == PACKET_TYPE_DATA) ? "DATA" : "DATA_SINGLE";

    DBG_PRINT("[SW%d] forwarding: type=%s, PSN=%u, len=%d, out_cnt=%d, opcode=0x%02x\n",
           sw_id, type_str, psn, len, rule->out_conns_cnt, packet_type);

    if (type == PACKET_TYPE_ACK || type == PACKET_TYPE_NAK) {
        // ACK/NAK: 单播到 ack_conn
        DBG_PRINT("[SW%d] forwarding %s to ack_conn\n", sw_id, type_str);
        send_packet_async(rule->ack_conn, type, data, len, psn, packet_type);
        // 注意：移除 usleep，避免阻塞 worker 线程
    } else if (type == PACKET_TYPE_DATA || type == PACKET_TYPE_DATA_SINGLE) {
        // DATA: 发送到所有 out_conns，使用独立的 send_psn
        DBG_PRINT("[SW%d] forwarding DATA to %d out_conns\n", sw_id, rule->out_conns_cnt);
        for (int i = 0; i < rule->out_conns_cnt; i++) {
            connection_t *conn = rule->out_conns[i];
            // 查找连接 ID 以获取对应的 send_psn
            int conn_id = -1;
            for (int j = 0; j < ctx->fan_in; j++) {
                if (&ctx->conns[j] == conn) {
                    conn_id = j;
                    break;
                }
            }
            if (conn_id >= 0) {
                uint32_t send_psn = ctx->send_psn[conn_id]++;
                DBG_PRINT("[SW%d] forwarding DATA: recv_PSN=%u, send_PSN=%u\n",
                       sw_id, psn, send_psn);
                send_packet_async(conn, PACKET_TYPE_DATA, data, len, send_psn, packet_type);
            } else {
                // 找不到连接 ID，使用原始 PSN
                send_packet_async(conn, PACKET_TYPE_DATA, data, len, psn, packet_type);
            }
        }
    }
}

/**
 * @brief 缓存并广播数据（下行）
 *
 * 重要：使用 down_epsn 作为统一的广播 PSN，这样所有连接使用相同的 PSN，
 * ACK 处理时可以正确匹配 bcast_buffer 的索引。
 */
static void cache_and_broadcast(switch_context_t *ctx, rule_t *rule, uint32_t psn, uint32_t *data, int len, int packet_type) {
    int sw_id = ctx->switch_id;

    // 使用 down_epsn 作为统一的广播 PSN（所有连接使用相同的 PSN）
    uint32_t bcast_psn = ctx->down_epsn;
    psn_state_t *state = &ctx->psn_states[Idx(bcast_psn)];

    DBG_PRINT("[SW%d] BCAST_START: recv_PSN=%u, bcast_PSN=%u, len=%d, out_cnt=%d\n",
           sw_id, psn, bcast_psn, len, rule->out_conns_cnt);

    // 加锁保护 bcast_buffer 的写入（与重传线程同步）
    pthread_mutex_lock(&state->mutex);

    // 缓存到广播缓冲区，使用 bcast_psn 作为索引
    memcpy(state->bcast_buffer.buffer, data, len * sizeof(uint32_t));
    state->bcast_buffer.len = len;
    state->bcast_buffer.packet_type = packet_type;
    state->bcast_buffer.state = 1;
    state->bcast_buffer.psn = bcast_psn;

    pthread_mutex_unlock(&state->mutex);

    ctx->down_epsn++;

    // 广播到所有子节点，所有连接使用相同的 bcast_psn
    int broadcast_opcode = strip_immediate_opcode(packet_type);

    for (int i = 0; i < rule->out_conns_cnt; i++) {
        connection_t *conn = rule->out_conns[i];
        int conn_id = -1;
        for (int j = 0; j < ctx->fan_in; j++) {
            if (&ctx->conns[j] == conn) {
                conn_id = j;
                break;
            }
        }
        if (conn_id < 0) {
            fprintf(stderr, "[SW%d] ERROR: Cannot find conn_id for connection\n", sw_id);
            continue;
        }

        // 每个连接使用独立的 send_psn，避免与 ACK 包的 PSN 冲突
        uint32_t send_psn = ctx->send_psn[conn_id]++;
        // 记录每个连接的实际发送 PSN，用于重传
        state->bcast_send_psn[conn_id] = send_psn;
        // 记录 send_psn -> bcast_psn 的映射，用于 ACK 处理
        ctx->send_to_bcast[conn_id][send_psn % SWITCH_ARRAY_LENGTH] = bcast_psn;
        DBG_PRINT("[SW%d] Broadcast to conn %d: bcast_PSN=%u, send_PSN=%u\n",
               sw_id, conn_id, bcast_psn, send_psn);
        int send_ret = send_packet_async(conn, PACKET_TYPE_DATA, state->bcast_buffer.buffer, len, send_psn, broadcast_opcode);
        if (send_ret != 0) {
            DBG_PRINT("[SW%d] BCAST_SEND_FAIL: conn=%d, PSN=%u\n", sw_id, conn_id, bcast_psn);
            fflush(stdout);
        }
        // 注意：移除 usleep，避免阻塞 worker 线程
    }
}

/**
 * @brief Broadcast 专用处理函数
 *
 * Broadcast 不需要聚合，只需要将 root 节点的数据转发给所有非 root 节点
 */
static void handle_broadcast(switch_context_t *ctx, rule_t *rule, int conn_id, uint32_t psn, uint32_t *data, int len, int packet_type) {
    int sw_id = ctx->switch_id;
    int root_rank = ctx->root_rank;

    DBG_PRINT("[SW%d] handle_broadcast: from conn %d, root_rank=%d, PSN=%u, len=%d\n",
           sw_id, conn_id, root_rank, psn, len);

    int send_opcode = strip_immediate_opcode(packet_type);

    // 广播到所有非 root 节点
    for (int i = 0; i < ctx->fan_in; i++) {
        if (i == conn_id) {
            DBG_PRINT("[SW%d] Broadcast: skip sender conn %d\n", sw_id, i);
            continue;
        }

        connection_t *conn = &ctx->conns[i];
        uint32_t send_psn = ctx->send_psn[i]++;
        DBG_PRINT("[SW%d] Broadcast: to conn %d, recv_PSN=%u, send_PSN=%u\n",
               sw_id, i, psn, send_psn);
        send_packet_async(conn, PACKET_TYPE_DATA, data, len, send_psn, send_opcode);
    }

    DBG_PRINT("[SW%d] Broadcast complete for PSN=%u\n", sw_id, psn);
}

/**
 * @brief 数据聚合函数
 *
 * 协议说明：
 * - 元数据通过 Immediate Data 传递，已在 packet_handler 中解析并存储到上下文
 * - 所有数据包的 payload 都是纯数据，不再有元数据前缀
 */
static void aggregate(switch_context_t *ctx, rule_t *rule, int conn_id, uint32_t psn, uint32_t *data, int len, int packet_type) {
    int sw_id = ctx->switch_id;
    primitive_type_t op_type = ctx->operation_type;
    int root_rank = ctx->root_rank;

    const char *op_str = (op_type == PRIMITIVE_TYPE_ALLREDUCE) ? "AllReduce" :
                         (op_type == PRIMITIVE_TYPE_REDUCE) ? "Reduce" :
                         (op_type == PRIMITIVE_TYPE_BROADCAST) ? "Broadcast" : "NULL";

    DBG_PRINT("[SW%d] AGG_RECV: conn=%d, PSN=%u, len=%d, op=%s\n",
           sw_id, conn_id, psn, len, op_str);

    if (op_type == PRIMITIVE_TYPE_BROADCAST) {
        handle_broadcast(ctx, rule, conn_id, psn, data, len, packet_type);
        return;
    }

    psn_state_t *state = &ctx->psn_states[Idx(psn)];
    pthread_mutex_lock(&state->mutex);

    if (state->degree == 0) {
        memcpy(state->agg_buffer.buffer, data, len * sizeof(uint32_t));
        state->agg_buffer.operation_type = op_type;
        state->agg_buffer.root_rank = root_rank;
        // 存储第一个到达的数据包的 opcode，用于后续广播
        state->agg_buffer.packet_type = packet_type;
        // 打印第一份数据的前几个元素
        if (len > 0) {
            DBG_PRINT("[SW%d] aggregate: first data sample[0]=%u\n",
                   sw_id, ntohl(data[0]));
        }
    } else {
        for (int i = 0; i < len; i++) {
            uint32_t dst_host = ntohl(state->agg_buffer.buffer[i]);
            uint32_t src_host = ntohl(data[i]);
            state->agg_buffer.buffer[i] = htonl(dst_host + src_host);
        }
        // 不更新 packet_type，保持第一个数据包的 opcode
        // 打印聚合后的数据样本
        if (len > 0) {
            DBG_PRINT("[SW%d] aggregate: after sum sample[0]=%u\n",
                   sw_id, ntohl(state->agg_buffer.buffer[0]));
        }
    }

    state->agg_buffer.len = len;
    // packet_type 已在 degree==0 时设置，不再覆盖
    state->agg_buffer.state = 1;
    state->agg_buffer.state = 1;
    state->agg_buffer.psn = psn;
    state->arrival[conn_id] = 1;
    state->degree++;

    int degree = state->degree;
    // 叶子交换机只需要聚合主机连接的数据，根交换机需要聚合所有子交换机的数据
    int expected_degree = ctx->is_root ? ctx->fan_in : ctx->host_fan_in;

    // 打印聚合后的 degree
    DBG_PRINT("[SW%d] AGG_DEGREE: %d/%d, PSN=%u\n",
           sw_id, degree, expected_degree, psn);

    pthread_mutex_unlock(&state->mutex);

    // 检查是否聚合完成
    if (degree == expected_degree) {
        DBG_PRINT("[SW%d] AGG_COMPLETE: PSN=%u, op=%s, is_root=%d\n",
               sw_id, psn, op_str, ctx->is_root);

        if (ctx->is_root) {
            // 根交换机：根据操作类型处理
            if (op_type == PRIMITIVE_TYPE_ALLREDUCE) {
                // AllReduce: 广播到所有子节点
                cache_and_broadcast(ctx, rule, psn, state->agg_buffer.buffer, len, packet_type);

                // 清理聚合缓冲区
                pthread_mutex_lock(&state->mutex);
                state->degree = 0;
                memset(state->arrival, 0, sizeof(state->arrival));
                state->agg_buffer.state = 0;
                pthread_mutex_unlock(&state->mutex);

            } else if (op_type == PRIMITIVE_TYPE_REDUCE) {
                // Reduce: 仅发送到 root_rank
                int target_conn = ctx->rank_to_conn[root_rank];
                DBG_PRINT("[SW%d] Reduce: root_rank=%d, target_conn=%d\n",
                       sw_id, root_rank, target_conn);

                if (target_conn >= 0 && target_conn < ctx->fan_in) {
                    uint32_t send_psn = ctx->send_psn[target_conn]++;
                    DBG_PRINT("[SW%d] Reduce: send to conn %d, recv_PSN=%u, send_PSN=%u\n",
                           sw_id, target_conn, psn, send_psn);

                    connection_t *conn = &ctx->conns[target_conn];
                    int send_opcode = strip_immediate_opcode(packet_type);
                    send_packet_async(conn, PACKET_TYPE_DATA, state->agg_buffer.buffer, len, send_psn, send_opcode);
                } else {
                    fprintf(stderr, "[SW%d] ERROR: Invalid target_conn=%d for root_rank=%d\n",
                            sw_id, target_conn, root_rank);
                }

                // 清理聚合缓冲区
                pthread_mutex_lock(&state->mutex);
                state->degree = 0;
                memset(state->arrival, 0, sizeof(state->arrival));
                state->agg_buffer.state = 0;
                pthread_mutex_unlock(&state->mutex);
            }
        } else {
            // 中间交换机：向上级转发
            DBG_PRINT("[SW%d] LEAF_FORWARD: PSN=%u upstream, op=%s\n",
                   sw_id, psn, op_str);
            int forward_opcode = strip_immediate_opcode(packet_type);

            // 获取父交换机连接，记录 send_psn 用于重传
            int parent_conn = get_parent_switch_conn(ctx);
            if (parent_conn >= 0) {
                // 记录发送 PSN（在 forwarding 递增之前获取当前值）
                state->agg_buffer.send_psn = ctx->send_psn[parent_conn];
            }

            forwarding(ctx, rule, psn, PACKET_TYPE_DATA, state->agg_buffer.buffer, len, forward_opcode);
        }
    }
}

/**
 * @brief 处理下行 ACK
 */
static void handle_downstream_ack(switch_context_t *ctx, rule_t *rule, uint32_t psn, int is_nak) {
    int sw_id = ctx->switch_id;
    int parent_conn = get_parent_switch_conn(ctx);

    if (!is_nak) {
        if ((int)psn > ctx->down_ack) {
            for (int p = ctx->down_ack + 1; p <= (int)psn; p++) {
                psn_state_t *state = &ctx->psn_states[Idx(p)];
                pthread_mutex_lock(&state->mutex);
                if (state->agg_buffer.state == 1 && state->agg_buffer.psn == p) {
                    state->degree = 0;
                    memset(state->arrival, 0, sizeof(state->arrival));
                    state->agg_buffer.state = 0;
                }
                pthread_mutex_unlock(&state->mutex);
            }
            ctx->down_ack = psn;

            // 更新 acked_psn，用于重传线程判断
            if (parent_conn >= 0 && (int)psn > ctx->acked_psn[parent_conn]) {
                ctx->acked_psn[parent_conn] = psn;
            }
        }
    } else {
        // NAK 处理：重传从 psn 开始的所有已缓存聚合数据包到上游
        DBG_PRINT("[SW%d] DOWN_NAK: PSN=%u, retransmitting to upstream...\n", sw_id, psn);

        // 获取上游连接（对于叶子交换机，conn 0 是父交换机）
        int parent_conn = get_parent_switch_conn(ctx);
        if (parent_conn < 0) {
            DBG_PRINT("[SW%d] DOWN_NAK: No parent connection found\n", sw_id);
            return;
        }

        connection_t *conn = &ctx->conns[parent_conn];

        // 重传从 NAK 的 PSN 到当前 send_psn 之间的所有数据包
        int current_send_psn = ctx->send_psn[parent_conn];
        for (int p = (int)psn; p < current_send_psn; p++) {
            psn_state_t *state = &ctx->psn_states[Idx(p)];
            pthread_mutex_lock(&state->mutex);

            if (state->agg_buffer.state == 1) {
                int forward_opcode = strip_immediate_opcode(state->agg_buffer.packet_type);
                DBG_PRINT("[SW%d] RETRANSMIT_UP: PSN=%u, len=%d\n",
                       sw_id, p, state->agg_buffer.len);
                send_packet_async(conn, PACKET_TYPE_DATA, state->agg_buffer.buffer,
                           state->agg_buffer.len, p, forward_opcode);
            }

            pthread_mutex_unlock(&state->mutex);
        }
    }
}

/**
 * @brief 处理上行 ACK
 */
static void handle_upstream_ack(switch_context_t *ctx, rule_t *rule, int conn_id, uint32_t psn, int is_nak) {
    int sw_id = ctx->switch_id;

    if (!is_nak) {
        int expected_acks = ctx->is_root ? ctx->fan_in : ctx->host_fan_in;

        if ((int)psn > ctx->latest_ack[conn_id]) {
            for (int p = ctx->latest_ack[conn_id] + 1; p <= (int)psn; p++) {
                int bcast_psn = ctx->send_to_bcast[conn_id][p % SWITCH_ARRAY_LENGTH];
                psn_state_t *state = &ctx->psn_states[Idx(bcast_psn)];
                pthread_mutex_lock(&state->mutex);

                if (state->r_arrival[conn_id] == 0) {
                    state->r_arrival[conn_id] = 1;
                    state->r_degree++;

                    if (state->r_degree == expected_acks) {
                        DBG_PRINT("[SW%d] BCAST_DONE: PSN=%d\n", sw_id, bcast_psn);
                        state->r_degree = 0;
                        memset(state->r_arrival, 0, sizeof(state->r_arrival));
                        state->bcast_buffer.state = 0;
                    }
                }

                pthread_mutex_unlock(&state->mutex);
            }
            ctx->latest_ack[conn_id] = psn;
            if ((int)psn > ctx->acked_psn[conn_id]) {
                ctx->acked_psn[conn_id] = psn;
            }
        }
    } else {
        // NAK 处理：重传从 psn 开始的所有已缓存数据包
        // 注意：NAK 中的 psn 是子交换机的 down_epsn（即本交换机的 send_psn）
        // 需要通过 send_to_bcast 映射找到对应的 bcast_psn
        DBG_PRINT("[SW%d] UP_NAK: conn=%d, PSN=%u, retransmitting...\n", sw_id, conn_id, psn);

        connection_t *conn = &ctx->conns[conn_id];
        int current_send_psn = ctx->send_psn[conn_id];

        for (int send_p = (int)psn; send_p < current_send_psn; send_p++) {
            // 通过 send_psn 找到对应的 bcast_psn
            int bcast_psn = ctx->send_to_bcast[conn_id][send_p % SWITCH_ARRAY_LENGTH];
            psn_state_t *state = &ctx->psn_states[Idx(bcast_psn)];
            pthread_mutex_lock(&state->mutex);

            if (state->bcast_buffer.state == 1) {
                int broadcast_opcode = strip_immediate_opcode(state->bcast_buffer.packet_type);
                DBG_PRINT("[SW%d] RETRANSMIT: conn=%d, send_PSN=%d, bcast_PSN=%d, len=%d\n",
                       sw_id, conn_id, send_p, bcast_psn, state->bcast_buffer.len);
                send_packet_async(conn, PACKET_TYPE_DATA, state->bcast_buffer.buffer,
                           state->bcast_buffer.len, send_p, broadcast_opcode);
            }

            pthread_mutex_unlock(&state->mutex);
        }
    }
}

/**
 * @brief 数据包处理回调函数（非阻塞版本）
 *
 * 只做快速解析和入队，所有阻塞操作移到 worker 线程
 * 现在使用设备级别的 pcap handle，需要根据源 IP 查找 conn_id
 */
static void packet_handler(uint8_t *user_data, const struct pcap_pkthdr *pkthdr, const uint8_t *packet) {
    switch_context_t *ctx = &g_switch_ctx;
    int device_id = atoi((char*)user_data);

    // 快速解析 IP 头获取源 IP
    ipv4_header_t *ip = (ipv4_header_t*)(packet + sizeof(eth_header_t));
    uint32_t src_ip = ip->src_ip;

    // 过滤掉自己发出的包（源 IP 是本机的包）
    for (int i = 0; i < ctx->device_conn_count[device_id]; i++) {
        int cid = ctx->device_conn_list[device_id][i];
        if (ctx->conns[cid].my_ip == src_ip) {
            return;  // 忽略自己发出的包
        }
    }

    // 根据源 IP 查找 conn_id
    int conn_id = -1;
    for (int i = 0; i < ctx->device_conn_count[device_id]; i++) {
        int cid = ctx->device_conn_list[device_id][i];
        if (ctx->conns[cid].peer_ip == src_ip) {
            conn_id = cid;
            break;
        }
    }

    if (conn_id < 0) {
        // 未知源 IP，忽略
        return;
    }
    // 快速解析数据包头部（ip 已在上面声明）
    udp_header_t *udp = (udp_header_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t));
    bth_header_t *bth = (bth_header_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(udp_header_t));

    uint32_t psn = ntohl(bth->apsn) & 0x00FFFFFF;
    uint8_t opcode = bth->opcode;

    // 检查是否有 Immediate Data
    int has_imm = (opcode == RDMA_OPCODE_SEND_ONLY_WITH_IMM || opcode == RDMA_OPCODE_SEND_LAST_WITH_IMM);
    int imm_offset = has_imm ? 4 : 0;
    int payload_len = ntohs(udp->length) - sizeof(udp_header_t) - sizeof(bth_header_t) - 4 - imm_offset;

    // 构建队列数据包
    queued_packet_t qpkt;
    qpkt.conn_id = conn_id;
    qpkt.psn = psn;
    qpkt.opcode = opcode;
    qpkt.payload_len = payload_len;
    qpkt.src_ip = ip->src_ip;
    qpkt.dst_ip = ip->dst_ip;
    qpkt.has_imm = has_imm;

    // 复制 Immediate Data
    if (has_imm) {
        uint32_t *imm_ptr = (uint32_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) +
                                         sizeof(udp_header_t) + sizeof(bth_header_t));
        qpkt.imm_data = ntohl(*imm_ptr);
    } else {
        qpkt.imm_data = 0;
    }

    // 复制 payload 数据（限制最大长度）
    int data_len = payload_len / sizeof(uint32_t);
    if (data_len > 1024) data_len = 1024;
    if (data_len > 0) {
        uint32_t *data = (uint32_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) +
                                      sizeof(udp_header_t) + sizeof(bth_header_t) + imm_offset);
        memcpy(qpkt.data, data, data_len * sizeof(uint32_t));
    }

    // 解析 ACK/NAK 标志
    qpkt.is_nak = 0;
    if (opcode == RDMA_OPCODE_ACK) {
        aeth_t *aeth = (aeth_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) +
                                  sizeof(udp_header_t) + sizeof(bth_header_t));
        qpkt.is_nak = (ntohl(aeth->syn_msn) >> 29) != 0;
    }

    // 入队（非阻塞，队列满时丢弃）
    if (!pkt_queue_push(&ctx->pkt_queues[conn_id], &qpkt)) {
        // 队列满，已在 pkt_queue_push 中计数
    }
}

/**
 * @brief 处理队列中的数据包（在 worker 线程中调用）
 *
 * 这是原来 packet_handler 的处理逻辑，现在移到 worker 线程中执行
 */
static void process_packet(switch_context_t *ctx, queued_packet_t *pkt) {
    int conn_id = pkt->conn_id;
    int sw_id = ctx->switch_id;

    // 从队列数据包中获取信息
    uint32_t psn = pkt->psn;
    uint8_t opcode = pkt->opcode;
    int payload_len = pkt->payload_len;
    uint32_t src_ip_addr = pkt->src_ip;

    // 转换源 IP 用于日志
    char src_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &src_ip_addr, src_ip, sizeof(src_ip));

    // 使用 DBG_PRINT 减少日志输出，避免影响性能
    DBG_PRINT("[SW%d] RECV: conn=%d, PSN=%u, opcode=0x%02x, from=%s\n",
           sw_id, conn_id, psn, opcode, src_ip);

    // 从上下文获取当前操作类型
    primitive_type_t prim = ctx->operation_type;
    int prim_param = ctx->root_rank;

    // 检查是否有 Immediate Data
    int has_imm_data = pkt->has_imm;
    int imm_offset = has_imm_data ? 4 : 0;

    // 判断是否是控制消息：opcode 0x05 且 payload 很小（<= 4 字节）
    int is_control_message = (opcode == RDMA_OPCODE_SEND_ONLY_WITH_IMM && payload_len <= 4);

    if (has_imm_data) {
        // 从队列数据包中获取 Immediate Data
        uint32_t imm_data = pkt->imm_data;

        // 解析 Immediate Data
        uint16_t dest_rank = GET_IMM_DEST_RANK(imm_data);
        uint8_t primitive = GET_IMM_PRIMITIVE(imm_data);
        uint8_t op = GET_IMM_OPERATOR(imm_data);
        uint8_t datatype = GET_IMM_DATATYPE(imm_data);

        // 转换 primitive 到 primitive_type_t
        switch (primitive) {
            case CTL_PRIMITIVE_ALLREDUCE:
                prim = PRIMITIVE_TYPE_ALLREDUCE;
                break;
            case CTL_PRIMITIVE_REDUCE:
                prim = PRIMITIVE_TYPE_REDUCE;
                break;
            case CTL_PRIMITIVE_BROADCAST:
                prim = PRIMITIVE_TYPE_BROADCAST;
                break;
            default:
                prim = PRIMITIVE_TYPE_NULL;
                break;
        }

        // 设置 prim_param（对于 Reduce 是 root_rank，对于 AllReduce/Broadcast 是 -1）
        if (dest_rank == CTL_DEST_RANK_ALL) {
            prim_param = -1;
        } else {
            prim_param = (int)dest_rank;
        }

        // 存储到上下文供后续数据包使用
        pthread_mutex_lock(&ctx->meta_mutex);
        ctx->operation_type = prim;
        ctx->root_rank = prim_param;
        // 如果是控制消息，记录其 PSN 作为当前操作的基准
        if (is_control_message) {
            ctx->ctrl_psn = psn;
        }
        pthread_mutex_unlock(&ctx->meta_mutex);

        DBG_PRINT("[SW%d] IMM_DATA: imm=0x%08X, dest=%u, prim=%d, op=%d\n",
               sw_id, imm_data, dest_rank, primitive, op);
        DBG_PRINT("[SW%d] META_UPDATE: op_type=%d, root=%d, is_ctrl=%d\n",
               sw_id, prim, prim_param, is_control_message);
    }

    // 如果是控制消息，发送 ACK，非根交换机还需要向上转发
    if (is_control_message) {
        int sw_id = ctx->switch_id;
        const char *prim_str = (prim == PRIMITIVE_TYPE_ALLREDUCE) ? "AllReduce" :
                               (prim == PRIMITIVE_TYPE_REDUCE) ? "Reduce" :
                               (prim == PRIMITIVE_TYPE_BROADCAST) ? "Broadcast" : "NULL";

        DBG_PRINT("[SW%d] CTRL_RECV: conn=%d, PSN=%u, op=%s, root=%d\n",
               sw_id, conn_id, psn, prim_str, prim_param);

        // 查找规则（使用队列数据包中的 IP 地址）
        char src_str[INET_ADDRSTRLEN], dst_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &pkt->src_ip, src_str, sizeof(src_str));
        inet_ntop(AF_INET, &pkt->dst_ip, dst_str, sizeof(dst_str));

        rule_t *rule = lookup_rule(&ctx->routing_table, pkt->src_ip, pkt->dst_ip, prim, prim_param);
        if (!rule) {
            DBG_PRINT("[SW%d] CTRL_NO_RULE: src=%s, dst=%s\n", sw_id, src_str, dst_str);
            return;
        }

        // 发送 ACK 并更新 ePSN
        ctx->agg_epsn[conn_id]++;
        DBG_PRINT("[SW%d] CTRL_ACK: conn=%d, ePSN->%d\n", sw_id, conn_id, ctx->agg_epsn[conn_id]);
        forwarding(ctx, rule, psn, PACKET_TYPE_ACK, NULL, 0, RDMA_OPCODE_ACK);

        // 注意：不再向父交换机转发控制包
        // 根交换机会从聚合数据的第一个包中获取操作类型信息
        // 转发控制包会导致 PSN 冲突（控制包和数据包使用相同的 send_psn）
        return;
    }

    // 数据包规则查找（使用队列数据包中的 IP 地址）
    char dst_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &pkt->dst_ip, dst_str, sizeof(dst_str));

    rule_t *rule = lookup_rule(&ctx->routing_table, pkt->src_ip, pkt->dst_ip, prim, prim_param);
    if (!rule) {
        DBG_PRINT("[SW%d] NO_RULE: src=%s, dst=%s, prim=%d\n",
               sw_id, src_ip, dst_str, prim);
        return;
    }

    const char *dir_str = (rule->direction == DIR_UP) ? "UP" : "DOWN";
    DBG_PRINT("[SW%d] RULE: dir=%s, out_cnt=%d\n", sw_id, dir_str, rule->out_conns_cnt);

    // 处理数据包
    if (opcode == RDMA_OPCODE_SEND_ONLY_WITH_IMM || opcode == RDMA_OPCODE_SEND_LAST_WITH_IMM ||
        opcode == RDMA_OPCODE_SEND_ONLY || opcode == RDMA_OPCODE_SEND_FIRST ||
        opcode == RDMA_OPCODE_SEND_MIDDLE || opcode == RDMA_OPCODE_SEND_LAST) {
        // RDMA SEND 数据包（从队列数据包中获取数据）
        uint32_t *data = pkt->data;
        int data_len = payload_len / sizeof(uint32_t);

        DBG_PRINT("[SW%d] DATA: len=%d bytes\n", sw_id, data_len);

        if (rule->direction == DIR_DOWN) {
            // 下行数据：缓存并广播
            DBG_PRINT("[SW%d] DOWN_DATA: PSN=%u, ePSN=%d, len=%d\n", sw_id, psn, ctx->down_epsn, data_len);

            if ((int)psn < ctx->down_epsn) {
                DBG_PRINT("[SW%d] DOWN_LAG: PSN=%u < ePSN=%d, send ACK\n",
                       sw_id, psn, ctx->down_epsn);
                forwarding(ctx, rule, ctx->down_epsn - 1, PACKET_TYPE_ACK, NULL, 0, RDMA_OPCODE_ACK);
            } else if ((int)psn > ctx->down_epsn) {
                DBG_PRINT("[SW%d] DOWN_AHEAD: PSN=%u > ePSN=%d, send NAK\n",
                       sw_id, psn, ctx->down_epsn);
                forwarding(ctx, rule, ctx->down_epsn, PACKET_TYPE_NAK, NULL, 0, RDMA_OPCODE_ACK);
            } else {
                DBG_PRINT("[SW%d] DOWN_OK: PSN=%u, process\n", sw_id, psn);
                forwarding(ctx, rule, psn, PACKET_TYPE_ACK, NULL, 0, RDMA_OPCODE_ACK);
                cache_and_broadcast(ctx, rule, psn, data, data_len, opcode);
            }
        } else {
            // 上行数据：聚合
            DBG_PRINT("[SW%d] UP_DATA: conn=%d, PSN=%u, ePSN=%d, len=%d\n",
                   sw_id, conn_id, psn, ctx->agg_epsn[conn_id], data_len);

            if ((int)psn < ctx->agg_epsn[conn_id]) {
                DBG_PRINT("[SW%d] UP_LAG: conn=%d, PSN=%u < ePSN=%d\n",
                       sw_id, conn_id, psn, ctx->agg_epsn[conn_id]);
                forwarding(ctx, rule, ctx->agg_epsn[conn_id] - 1, PACKET_TYPE_ACK, NULL, 0, RDMA_OPCODE_ACK);
            } else if ((int)psn > ctx->agg_epsn[conn_id]) {
                DBG_PRINT("[SW%d] UP_AHEAD: conn=%d, PSN=%u > ePSN=%d\n",
                       sw_id, conn_id, psn, ctx->agg_epsn[conn_id]);
                forwarding(ctx, rule, ctx->agg_epsn[conn_id], PACKET_TYPE_NAK, NULL, 0, RDMA_OPCODE_ACK);
            } else {
                DBG_PRINT("[SW%d] UP_OK: conn=%d, PSN=%u, ePSN->%d\n",
                       sw_id, conn_id, psn, ctx->agg_epsn[conn_id] + 1);
                ctx->agg_epsn[conn_id]++;
                forwarding(ctx, rule, psn, PACKET_TYPE_ACK, NULL, 0, RDMA_OPCODE_ACK);
                aggregate(ctx, rule, conn_id, psn, data, data_len, opcode);
            }
        }
    } else if (opcode == RDMA_OPCODE_ACK) {
        // ACK 包（从队列数据包中获取 is_nak）
        int is_nak = pkt->is_nak;

        DBG_PRINT("[SW%d] ACK_RECV: conn=%d, PSN=%u, is_nak=%d, dir=%s\n",
               sw_id, conn_id, psn, is_nak, dir_str);

        if (rule->direction == DIR_DOWN) {
            handle_downstream_ack(ctx, rule, psn, is_nak);
        } else {
            handle_upstream_ack(ctx, rule, conn_id, psn, is_nak);
        }
    } else {
        DBG_PRINT("[SW%d] UNKNOWN_OPCODE: 0x%02x\n", sw_id, opcode);
    }
}

/**
 * @brief Worker 线程函数
 *
 * 从队列中取出数据包并处理，所有阻塞操作在此线程中执行
 */
static void *worker_thread(void *arg) {
    int conn_id = *(int*)arg;
    free(arg);

    switch_context_t *ctx = &g_switch_ctx;
    packet_queue_t *queue = &ctx->pkt_queues[conn_id];

    queued_packet_t pkt;
    unsigned int last_dropped = 0;

    while (ctx->running) {
        if (pkt_queue_pop(queue, &pkt)) {
            process_packet(ctx, &pkt);
        } else {
            usleep(10);
        }

        unsigned int dropped = queue->dropped;
        if (dropped > last_dropped) {
            DBG_PRINT("[Worker] Thread %d: %u packets dropped\n",
                   conn_id, dropped - last_dropped);
            fflush(stdout);
            last_dropped = dropped;
        }
    }

    return NULL;
}

/**
 * @brief 发送线程 - 专门负责从发送队列取出数据包并发送
 *
 * 每个设备一个发送线程，避免发送操作阻塞接收和处理
 */
static void *sender_thread(void *arg) {
    int device_id = *(int*)arg;
    free(arg);

    switch_context_t *ctx = &g_switch_ctx;
    send_queue_t *queue = &ctx->send_queues[device_id];
    pcap_t *send_handle = ctx->device_send_handles[device_id];

    printf("[Sender] Thread %d started for device %s\n",
           device_id, ctx->device_names[device_id]);
    fflush(stdout);

    send_packet_t pkt;
    unsigned int last_dropped = 0;

    while (ctx->running) {
        if (send_queue_pop(queue, &pkt)) {
            int ret = pcap_sendpacket(send_handle, pkt.frame, pkt.frame_len);
            if (ret == -1) {
                fprintf(stderr, "[Sender] Thread %d: pcap_sendpacket failed: %s\n",
                        device_id, pcap_geterr(send_handle));
            }
        } else {
            usleep(5);
        }

        unsigned int dropped = queue->dropped;
        if (dropped > last_dropped) {
            DBG_PRINT("[Sender] Thread %d: %u packets dropped (queue full)\n",
                   device_id, dropped - last_dropped);
            fflush(stdout);
            last_dropped = dropped;
        }
    }

    printf("[Sender] Thread %d stopped\n", device_id);
    fflush(stdout);
    return NULL;
}

/**
 * @brief 超时重传线程
 *
 * 定期检查未确认的数据包，超时后进行重传
 * 支持两种重传场景：
 * 1. 下行广播重传：根交换机/中间交换机向子节点重传广播数据
 * 2. 上行聚合重传：叶子交换机向父交换机重传聚合数据
 */
static void *retransmit_thread(void *arg) {
    switch_context_t *ctx = (switch_context_t *)arg;

    // 等待配置完成
    while (ctx->fan_in == 0) {
        usleep(100000);  // 100ms
    }

    // 等待发送队列初始化完成（num_senders > 0 表示所有 sender 线程已启动）
    while (ctx->num_senders == 0) {
        usleep(100000);  // 100ms
    }

    // 在配置完成后读取 switch_id
    int sw_id = ctx->switch_id;

    // 记录最后检查时间
    struct timeval last_check_time;
    gettimeofday(&last_check_time, NULL);

    while (1) {
        usleep(RETRANSMIT_CHECK_INTERVAL_US);

        struct timeval now;
        gettimeofday(&now, NULL);

        long elapsed_us = (now.tv_sec - last_check_time.tv_sec) * 1000000 +
                          (now.tv_usec - last_check_time.tv_usec);

        if (elapsed_us < RETRANSMIT_TIMEOUT_US) {
            continue;
        }

        // 1. 下行广播重传（使用 bcast_buffer）
        // 使用"先收集、后发送"模式避免持锁发送
        int down_epsn = ctx->down_epsn;
        int parent_conn = ctx->is_root ? -1 : get_parent_switch_conn(ctx);

        // 收集需要重传的数据包
        retransmit_info_t bcast_retransmit[MAX_RETRANSMIT_BATCH];
        int bcast_count = 0;

        // 遍历所有已广播的 PSN (从 0 到 down_epsn-1)
        for (int bcast_psn = 0; bcast_psn < down_epsn && bcast_count < MAX_RETRANSMIT_BATCH; bcast_psn++) {
            psn_state_t *state = &ctx->psn_states[Idx(bcast_psn)];
            pthread_mutex_lock(&state->mutex);

            if (state->bcast_buffer.state == 1) {
                // 检查每个连接是否已确认
                for (int i = 0; i < ctx->fan_in && bcast_count < MAX_RETRANSMIT_BATCH; i++) {
                    if (!ctx->conns[i].ok) continue;
                    if (i == parent_conn) continue;

                    // 检查该连接是否已确认此 PSN
                    if (state->r_arrival[i] == 0) {
                        // 复制数据到重传缓冲区
                        retransmit_info_t *info = &bcast_retransmit[bcast_count];
                        info->conn = &ctx->conns[i];
                        info->len = state->bcast_buffer.len;
                        // 使用该连接的实际发送 PSN 进行重传
                        info->psn = state->bcast_send_psn[i];
                        info->opcode = strip_immediate_opcode(state->bcast_buffer.packet_type);
                        memcpy(info->buffer, state->bcast_buffer.buffer, info->len * sizeof(uint32_t));
                        bcast_count++;
                    }
                }
            }

            pthread_mutex_unlock(&state->mutex);
        }

        // 释放锁后再发送
        for (int i = 0; i < bcast_count; i++) {
            retransmit_info_t *info = &bcast_retransmit[i];
            // 再次检查是否仍需要重传（避免与 BCAST_DONE 竞态）
            int bcast_psn_check = ctx->send_to_bcast[info->conn - ctx->conns][info->psn % SWITCH_ARRAY_LENGTH];
            psn_state_t *state_check = &ctx->psn_states[Idx(bcast_psn_check)];
            pthread_mutex_lock(&state_check->mutex);
            int still_needed = (state_check->bcast_buffer.state == 1);
            pthread_mutex_unlock(&state_check->mutex);

            if (still_needed) {
                DBG_PRINT("[SW%d] BCAST_RETRANSMIT: PSN=%d, len=%d\n", sw_id, info->psn, info->len);
                send_packet_async(info->conn, PACKET_TYPE_DATA, info->buffer, info->len, info->psn, info->opcode);
                usleep(RETRANSMIT_PACKET_DELAY_US);
            }
        }

        // 2. 上行聚合重传（叶子交换机向父交换机重传，使用 agg_buffer）
        // 注意：agg_buffer 按 recv_psn 索引，但需要用 send_psn 发送
        if (!ctx->is_root) {
            int parent_conn = get_parent_switch_conn(ctx);
            if (parent_conn >= 0) {
                int send_psn = ctx->send_psn[parent_conn];
                int acked_psn = ctx->acked_psn[parent_conn];

                if (send_psn > acked_psn + 1) {
                    connection_t *conn = &ctx->conns[parent_conn];
                    retransmit_info_t agg_retransmit[MAX_RETRANSMIT_BATCH];
                    int agg_count = 0;

                    // 遍历所有 PSN 状态，查找需要重传的 agg_buffer
                    // agg_buffer 按 recv_psn 存储，但 send_psn 记录在 agg_buffer.send_psn 中
                    for (int recv_p = 0; recv_p < SWITCH_ARRAY_LENGTH && agg_count < MAX_RETRANSMIT_BATCH; recv_p++) {
                        psn_state_t *state = &ctx->psn_states[recv_p];
                        pthread_mutex_lock(&state->mutex);

                        // 检查 agg_buffer 是否有效，且其 send_psn 在需要重传的范围内
                        if (state->agg_buffer.state == 1 &&
                            state->agg_buffer.send_psn > acked_psn &&
                            state->agg_buffer.send_psn < send_psn) {
                            retransmit_info_t *info = &agg_retransmit[agg_count];
                            info->conn = conn;
                            info->len = state->agg_buffer.len;
                            info->psn = state->agg_buffer.send_psn;  // 使用 send_psn 发送
                            info->opcode = strip_immediate_opcode(state->agg_buffer.packet_type);
                            memcpy(info->buffer, state->agg_buffer.buffer, info->len * sizeof(uint32_t));
                            agg_count++;
                        }

                        pthread_mutex_unlock(&state->mutex);
                    }

                    // 释放锁后再发送
                    for (int i = 0; i < agg_count; i++) {
                        retransmit_info_t *info = &agg_retransmit[i];
                        DBG_PRINT("[SW%d] AGG_RETRANSMIT: PSN=%d, len=%d\n", sw_id, info->psn, info->len);
                        send_packet_async(info->conn, PACKET_TYPE_DATA, info->buffer, info->len, info->psn, info->opcode);
                        usleep(RETRANSMIT_PACKET_DELAY_US);
                    }
                }
            }
        }

        gettimeofday(&last_check_time, NULL);
    }

    return NULL;
}

/**
 * @brief 后台接收线程
 */
void *background_receiving(void *arg) {
    switch_context_t *ctx = (switch_context_t *)arg;

    // 等待配置完成
    while (ctx->fan_in == 0) {
        usleep(100000);  // 100ms
    }

    // 设置运行标志
    ctx->running = 1;

    // 初始化数据包队列
    for (int i = 0; i < ctx->fan_in; i++) {
        pkt_queue_init(&ctx->pkt_queues[i]);
    }

    // 初始化发送队列
    for (int i = 0; i < MAX_CONNECTIONS_NUM; i++) {
        send_queue_init(&ctx->send_queues[i]);
    }
    ctx->num_senders = 0;

    // ========== 第一遍：收集设备信息和连接映射 ==========
    ctx->num_devices = 0;
    for (int i = 0; i < MAX_CONNECTIONS_NUM; i++) {
        ctx->device_conn_count[i] = 0;
    }

    for (int i = 0; i < ctx->fan_in; i++) {
        connection_t *conn = &ctx->conns[i];

        // 查找或创建设备
        int device_idx = -1;
        for (int d = 0; d < ctx->num_devices; d++) {
            if (strcmp(ctx->device_names[d], conn->device) == 0) {
                device_idx = d;
                break;
            }
        }

        if (device_idx < 0) {
            device_idx = ctx->num_devices;
            strncpy(ctx->device_names[device_idx], conn->device, 31);
            ctx->device_names[device_idx][31] = '\0';
            ctx->num_devices++;
        }

        // 记录连接到设备的映射
        conn->device_id = device_idx;
        ctx->device_conn_list[device_idx][ctx->device_conn_count[device_idx]] = i;
        ctx->device_conn_count[device_idx]++;
    }

    // ========== 第二遍：为每个设备创建 pcap handle ==========
    for (int d = 0; d < ctx->num_devices; d++) {
        char errbuf[PCAP_ERRBUF_SIZE];
        const char *device = ctx->device_names[d];

        // 创建接收用 pcap handle
        ctx->device_recv_handles[d] = pcap_create(device, errbuf);
        if (!ctx->device_recv_handles[d]) {
            fprintf(stderr, "[Receiver] Failed to create recv pcap for %s: %s\n", device, errbuf);
            continue;
        }

        pcap_set_snaplen(ctx->device_recv_handles[d], BUFSIZ);
        pcap_set_promisc(ctx->device_recv_handles[d], 1);
        pcap_set_timeout(ctx->device_recv_handles[d], 10);
        pcap_set_immediate_mode(ctx->device_recv_handles[d], 1);
        pcap_set_buffer_size(ctx->device_recv_handles[d], 64 * 1024 * 1024);

        if (pcap_activate(ctx->device_recv_handles[d]) != 0) {
            fprintf(stderr, "[Receiver] Failed to activate recv pcap for %s: %s\n",
                    device, pcap_geterr(ctx->device_recv_handles[d]));
            continue;
        }

        if (pcap_setnonblock(ctx->device_recv_handles[d], 1, errbuf) == -1) {
            fprintf(stderr, "[Receiver] Warning: Failed to set nonblock for %s: %s\n",
                    device, errbuf);
        }

        // 设置过滤器：根据设备上连接类型决定过滤端口
        // 检查该设备上是否有 Switch 间连接和 Host 连接
        struct bpf_program fp;
        char filter_exp[128];
        int has_switch_conn = 0, has_host_conn = 0;
        for (int c = 0; c < ctx->device_conn_count[d]; c++) {
            int conn_id = ctx->device_conn_list[d][c];
            if (ctx->conns[conn_id].is_switch) {
                has_switch_conn = 1;
            } else {
                has_host_conn = 1;
            }
        }

        // 构建过滤器表达式
        if (has_switch_conn && has_host_conn) {
            snprintf(filter_exp, sizeof(filter_exp), "udp port %d or udp port %d",
                     RDMA_HOST_PORT, RDMA_SWITCH_PORT);
        } else if (has_switch_conn) {
            snprintf(filter_exp, sizeof(filter_exp), "udp port %d", RDMA_SWITCH_PORT);
        } else {
            snprintf(filter_exp, sizeof(filter_exp), "udp port %d", RDMA_HOST_PORT);
        }

        if (pcap_compile(ctx->device_recv_handles[d], &fp, filter_exp, 0, PCAP_NETMASK_UNKNOWN) == -1) {
            fprintf(stderr, "[Receiver] Failed to compile filter: %s\n",
                    pcap_geterr(ctx->device_recv_handles[d]));
            continue;
        }
        if (pcap_setfilter(ctx->device_recv_handles[d], &fp) == -1) {
            fprintf(stderr, "[Receiver] Failed to set filter: %s\n",
                    pcap_geterr(ctx->device_recv_handles[d]));
            continue;
        }
        pcap_freecode(&fp);

        // 创建发送用 pcap handle
        ctx->device_send_handles[d] = pcap_create(device, errbuf);
        if (!ctx->device_send_handles[d]) {
            fprintf(stderr, "[Sender] Failed to create send pcap for %s: %s\n", device, errbuf);
            continue;
        }
        pcap_set_snaplen(ctx->device_send_handles[d], BUFSIZ);
        if (pcap_activate(ctx->device_send_handles[d]) != 0) {
            fprintf(stderr, "[Sender] Failed to activate send pcap for %s: %s\n",
                    device, pcap_geterr(ctx->device_send_handles[d]));
            continue;
        }
        pthread_mutex_init(&ctx->device_send_mutexes[d], NULL);

        // 标记该设备上的所有连接为 ok
        for (int c = 0; c < ctx->device_conn_count[d]; c++) {
            int conn_id = ctx->device_conn_list[d][c];
            ctx->conns[conn_id].send_handle = ctx->device_send_handles[d];
            ctx->conns[conn_id].send_mutex = &ctx->device_send_mutexes[d];
            ctx->conns[conn_id].ok = 1;

            // 为每个连接创建 UDP socket 占位，防止内核发送 ICMP port unreachable
            int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (udp_fd >= 0) {
                // 允许端口复用
                int reuse = 1;
                setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
                setsockopt(udp_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

                struct sockaddr_in addr;
                memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                // 根据连接类型选择端口
                int port = ctx->conns[conn_id].is_switch ? RDMA_SWITCH_PORT : RDMA_HOST_PORT;
                addr.sin_port = htons(port);
                addr.sin_addr.s_addr = ctx->conns[conn_id].my_ip;

                if (bind(udp_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                    ctx->udp_sockets[conn_id] = udp_fd;
                } else {
                    close(udp_fd);
                    ctx->udp_sockets[conn_id] = -1;
                }
            } else {
                ctx->udp_sockets[conn_id] = -1;
            }
        }
    }

    // ========== 启动发送线程 ==========
    for (int i = 0; i < ctx->num_devices; i++) {
        int *device_id = malloc(sizeof(int));
        *device_id = i;
        if (pthread_create(&ctx->sender_threads[i], NULL, sender_thread, device_id) == 0) {
            ctx->num_senders++;
        } else {
            fprintf(stderr, "[Receiver] Failed to create sender thread %d\n", i);
            free(device_id);
        }
    }

    // ========== 启动 worker 线程 ==========
    ctx->num_workers = 0;
    for (int i = 0; i < ctx->fan_in; i++) {
        int *conn_id = malloc(sizeof(int));
        *conn_id = i;
        if (pthread_create(&ctx->worker_threads[i], NULL, worker_thread, conn_id) == 0) {
            ctx->num_workers++;
        } else {
            fprintf(stderr, "[Receiver] Failed to create worker thread %d\n", i);
            free(conn_id);
        }
    }

    // ========== 启动接收线程 ==========
    pthread_t threads[MAX_CONNECTIONS_NUM];
    static char thread_ids[MAX_CONNECTIONS_NUM][8];

    for (int d = 0; d < ctx->num_devices; d++) {
        snprintf(thread_ids[d], sizeof(thread_ids[d]), "%d", d);
        pthread_create(&threads[d], NULL, (void*(*)(void*))pcap_receiver_thread, thread_ids[d]);
    }

    // 等待所有线程
    for (int d = 0; d < ctx->num_devices; d++) {
        pthread_join(threads[d], NULL);
    }

    return NULL;
}

/**
 * @brief 设备级别的 pcap 接收线程（使用 epoll）
 *
 * 每个设备一个线程，使用软件 demux 根据源 IP 分发到不同连接的队列
 */
static void *pcap_receiver_thread(void *arg) {
    int device_id = atoi((char*)arg);
    switch_context_t *ctx = &g_switch_ctx;
    pcap_t *handle = ctx->device_recv_handles[device_id];

    char id_str[8];
    snprintf(id_str, sizeof(id_str), "%d", device_id);

    // 获取 pcap 的可选择文件描述符
    int pcap_fd = pcap_get_selectable_fd(handle);
    if (pcap_fd < 0) {
        fprintf(stderr, "[Receiver] Thread %d: pcap_get_selectable_fd failed\n", device_id);
        return NULL;
    }

    // 创建 epoll 实例
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("[Receiver] epoll_create1 failed");
        return NULL;
    }

    // 添加 pcap fd 到 epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = pcap_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pcap_fd, &ev) < 0) {
        perror("[Receiver] epoll_ctl failed");
        close(epoll_fd);
        return NULL;
    }

    struct epoll_event events[1];

    while (ctx->running) {
        int nfds = epoll_wait(epoll_fd, events, 1, 100);

        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("[Receiver] epoll_wait failed");
            break;
        }

        if (nfds > 0) {
            int ret;
            do {
                ret = pcap_dispatch(handle, 100, packet_handler, (uint8_t*)id_str);
            } while (ret > 0);

            if (ret < 0) {
                break;
            }
        }
    }

    close(epoll_fd);
    pcap_close(handle);
    return NULL;
}

/**
 * @brief 连接到控制器并启动通信线程
 */
static int controller_init(switch_context_t *ctx, const char *controller_ip) {
    printf("Connecting to controller...\n");

    if (controller_connect(ctx, controller_ip, CONTROLLER_SWITCH_PORT) < 0) {
        fprintf(stderr, "Failed to connect to controller\n");
        return -1;
    }

    pthread_t controller_tid;
    if (pthread_create(&controller_tid, NULL, controller_thread, ctx) != 0) {
        fprintf(stderr, "Failed to create controller thread\n");
        return -1;
    }
    pthread_detach(controller_tid);

    printf("Controller connection established\n");
    return 0;
}

/**
 * @brief 主函数
 */
int main(int argc, char *argv[]) {
    char *controller_ip = "192.168.0.3";

    if (argc >= 2) {
        controller_ip = argv[1];
    }

    printf("=== INC Switch (Refactored Version) ===\n");
    printf("Controller IP: %s\n", controller_ip);

    // 初始化 CRC32 表
    init_crc32_table();

    // 初始化交换机上下文
    if (switch_context_init(&g_switch_ctx, 0, 4) < 0) {
        fprintf(stderr, "Failed to initialize switch context\n");
        return 1;
    }

    // is_root 将从 Controller 发送的 YAML 配置中获取

    // 连接到控制器
    if (controller_init(&g_switch_ctx, controller_ip) < 0) {
        fprintf(stderr, "Failed to connect to controller\n");
        switch_context_cleanup(&g_switch_ctx);
        return 1;
    }

    printf("Switch started successfully\n");
    printf("Waiting for controller commands...\n");

    // 启动接收线程
    pthread_t receiver_tid;
    pthread_create(&receiver_tid, NULL, background_receiving, &g_switch_ctx);

    // 启动超时重传线程
    pthread_t retransmit_tid;
    pthread_create(&retransmit_tid, NULL, retransmit_thread, &g_switch_ctx);

    // 等待接收线程结束
    pthread_join(receiver_tid, NULL);

    // 清理资源
    switch_context_cleanup(&g_switch_ctx);

    printf("Switch stopped\n");
    return 0;
}
