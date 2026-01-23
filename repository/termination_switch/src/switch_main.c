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
#include "switch_context.h"
#include "controller_comm.h"
#include "parameter.h"
#include "util.h"

// ==================== 宏定义 ====================
#define Idx(psn) ((psn) % SWITCH_ARRAY_LENGTH)

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

    int size = build_eth_packet(
        packet, type, (char*)data, len * sizeof(uint32_t),
        conn->my_mac, conn->peer_mac,
        conn->my_ip, conn->peer_ip,
        conn->my_port, conn->peer_port,
        conn->peer_qp, psn, psn + 1, packet_type, NULL
    );

    if (pcap_sendpacket(conn->handle, packet, size) == -1) {
        fprintf(stderr, "[SWITCH] Error sending packet: %s\n", pcap_geterr(conn->handle));
        return -1;
    }
    return 0;
}

/**
 * @brief 转发数据包（支持单播和多播）
 */
static void forwarding(switch_context_t *ctx, rule_t *rule, uint32_t psn, int type, uint32_t *data, int len, int packet_type) {
    printf("[SWITCH] forwarding: psn=%u, type=%d, len=%d, out_cnt=%d\n", psn, type, len, rule->out_conns_cnt);

    if (type == PACKET_TYPE_ACK || type == PACKET_TYPE_NAK) {
        // ACK/NAK: 单播到 ack_conn
        send_packet(rule->ack_conn, type, data, len, psn, packet_type);
    } else if (type == PACKET_TYPE_DATA || type == PACKET_TYPE_DATA_SINGLE) {
        // DATA: 发送到所有 out_conns
        for (int i = 0; i < rule->out_conns_cnt; i++) {
            send_packet(rule->out_conns[i], PACKET_TYPE_DATA, data, len, psn, packet_type);
        }
    }
}

/**
 * @brief 缓存并广播数据（下行）
 */
static void cache_and_broadcast(switch_context_t *ctx, rule_t *rule, uint32_t psn, uint32_t *data, int len, int packet_type) {
    psn_state_t *state = &ctx->psn_states[Idx(psn)];

    // 缓存到广播缓冲区
    memcpy(state->bcast_buffer.buffer, data, len * sizeof(uint32_t));
    state->bcast_buffer.len = len;
    state->bcast_buffer.packet_type = packet_type;
    state->bcast_buffer.state = 1;
    state->bcast_buffer.psn = psn;

    ctx->down_epsn++;

    // 广播到所有子节点，每个连接使用独立的发送 PSN
    // 注意：Switch 广播时不发送 Immediate Data，所以需要将 opcode 转换为普通 Send
    int broadcast_opcode = strip_immediate_opcode(packet_type);

    printf("[SWITCH] AllReduce: broadcasting PSN=%u\n", psn);
    for (int i = 0; i < rule->out_conns_cnt; i++) {
        connection_t *conn = rule->out_conns[i];
        int conn_id = -1;
        // 找到连接的 ID
        for (int j = 0; j < ctx->fan_in; j++) {
            if (&ctx->conns[j] == conn) {
                conn_id = j;
                break;
            }
        }
        if (conn_id < 0) {
            fprintf(stderr, "[SWITCH] ERROR: Cannot find conn_id for connection\n");
            continue;
        }

        uint32_t send_psn = ctx->send_psn[conn_id]++;
        printf("[SWITCH] Broadcasting to conn %d: recv_PSN=%u, send_PSN=%u\n", conn_id, psn, send_psn);
        send_packet(conn, PACKET_TYPE_DATA, state->bcast_buffer.buffer, len, send_psn, broadcast_opcode);
    }
}

/**
 * @brief Broadcast 专用处理函数
 *
 * Broadcast 不需要聚合，只需要将 root 节点的数据转发给所有非 root 节点
 */
static void handle_broadcast(switch_context_t *ctx, rule_t *rule, int conn_id, uint32_t psn, uint32_t *data, int len, int packet_type) {
    int root_rank = ctx->root_rank;

    printf("[SWITCH] Broadcast: received from conn %d (root_rank=%d), PSN=%u, len=%d\n",
           conn_id, root_rank, psn, len);

    // Switch 发送时不带 Immediate Data，需要转换 opcode
    int send_opcode = strip_immediate_opcode(packet_type);

    // 广播到所有非 root 节点
    for (int i = 0; i < ctx->fan_in; i++) {
        // 跳过发送者（root 节点）
        if (i == conn_id) {
            printf("[SWITCH] Broadcast: skipping sender conn %d\n", i);
            continue;
        }

        connection_t *conn = &ctx->conns[i];
        uint32_t send_psn = ctx->send_psn[i]++;
        printf("[SWITCH] Broadcast: sending to conn %d, recv_PSN=%u, send_PSN=%u\n", i, psn, send_psn);
        send_packet(conn, PACKET_TYPE_DATA, data, len, send_psn, send_opcode);
    }

    printf("[SWITCH] Broadcast complete for PSN=%u\n", psn);
}

/**
 * @brief 数据聚合函数
 *
 * 协议说明：
 * - 元数据通过 Immediate Data 传递，已在 packet_handler 中解析并存储到上下文
 * - 所有数据包的 payload 都是纯数据，不再有元数据前缀
 */
static void aggregate(switch_context_t *ctx, rule_t *rule, int conn_id, uint32_t psn, uint32_t *data, int len, int packet_type) {
    // 从上下文获取元数据（已在 packet_handler 中解析）
    primitive_type_t op_type = ctx->operation_type;
    int root_rank = ctx->root_rank;

    // Broadcast 不需要聚合，直接转发
    if (op_type == PRIMITIVE_TYPE_BROADCAST) {
        handle_broadcast(ctx, rule, conn_id, psn, data, len, packet_type);
        return;
    }

    psn_state_t *state = &ctx->psn_states[Idx(psn)];

    pthread_mutex_lock(&state->mutex);

    // 聚合数据（现在所有数据都是纯数据，不再有元数据前缀）
    if (state->degree == 0) {
        // 第一份数据：直接复制
        memcpy(state->agg_buffer.buffer, data, len * sizeof(uint32_t));
        state->agg_buffer.operation_type = op_type;
        state->agg_buffer.root_rank = root_rank;
    } else {
        // 后续数据：累加（所有元素都是数据）
        for (int i = 0; i < len; i++) {
            uint32_t dst_host = ntohl(state->agg_buffer.buffer[i]);
            uint32_t src_host = ntohl(data[i]);
            state->agg_buffer.buffer[i] = htonl(dst_host + src_host);
        }
    }

    state->agg_buffer.len = len;
    state->agg_buffer.packet_type = packet_type;
    state->agg_buffer.state = 1;
    state->agg_buffer.psn = psn;
    state->arrival[conn_id] = 1;
    state->degree++;

    int degree = state->degree;
    int fan_in = ctx->fan_in;

    // 打印聚合后的 degree
    printf("[SWITCH] aggregate: conn_id=%d, PSN=%u, len=%d, degree=%d/%d, op_type=%d, root_rank=%d\n",
           conn_id, psn, len, degree, fan_in, op_type, root_rank);

    pthread_mutex_unlock(&state->mutex);

    // 检查是否聚合完成
    if (degree == fan_in) {
        printf("[SWITCH] Aggregation complete for PSN=%u, op_type=%d\n", psn, op_type);

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
                if (target_conn >= 0 && target_conn < ctx->fan_in) {
                    // 使用该连接的发送 PSN 计数器
                    uint32_t send_psn = ctx->send_psn[target_conn]++;
                    printf("[SWITCH] Reduce: sending to root_rank=%d, recv_PSN=%u, send_PSN=%u via conn %d\n",
                           root_rank, psn, send_psn, target_conn);

                    connection_t *conn = &ctx->conns[target_conn];
                    // Switch 发送时不带 Immediate Data，需要转换 opcode
                    int send_opcode = strip_immediate_opcode(packet_type);
                    send_packet(conn, PACKET_TYPE_DATA, state->agg_buffer.buffer, len, send_psn, send_opcode);
                    printf("[SWITCH] Reduce result sent to rank %d via conn %d\n", root_rank, target_conn);
                } else {
                    fprintf(stderr, "[SWITCH] ERROR: Invalid target conn for root_rank=%d\n", root_rank);
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
            // 注意：向上级转发时也需要转换 opcode，因为 Switch 不发送 Immediate Data
            printf("[SWITCH] Intermediate switch: forwarding PSN=%u upstream\n", psn);
            int forward_opcode = strip_immediate_opcode(packet_type);
            forwarding(ctx, rule, psn, PACKET_TYPE_DATA, state->agg_buffer.buffer, len, forward_opcode);
        }
    }
}

/**
 * @brief 处理下行 ACK
 */
static void handle_downstream_ack(switch_context_t *ctx, rule_t *rule, uint32_t psn, int is_nak) {
    if (!is_nak) {
        // ACK: 清理已确认的缓冲区
        printf("[SWITCH] Downstream ACK for PSN=%u\n", psn);

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
        }
    } else {
        // NAK: 需要重传
        printf("[SWITCH] Downstream NAK for PSN=%u\n", psn);
        // TODO: 实现重传逻辑
    }
}

/**
 * @brief 处理上行 ACK
 */
static void handle_upstream_ack(switch_context_t *ctx, rule_t *rule, int conn_id, uint32_t psn, int is_nak) {
    if (!is_nak) {
        // ACK
        printf("[SWITCH] Upstream ACK from conn %d for PSN=%u\n", conn_id, psn);

        if ((int)psn > ctx->latest_ack[conn_id]) {
            psn_state_t *state = &ctx->psn_states[Idx(psn)];
            pthread_mutex_lock(&state->mutex);

            if (state->r_arrival[conn_id] == 0) {
                state->r_arrival[conn_id] = 1;
                state->r_degree++;

                if (state->r_degree == ctx->fan_in) {
                    // 广播完成
                    printf("[SWITCH] Broadcast complete for PSN=%u\n", psn);
                    state->r_degree = 0;
                    memset(state->r_arrival, 0, sizeof(state->r_arrival));
                    state->bcast_buffer.state = 0;
                }
            }

            pthread_mutex_unlock(&state->mutex);
            ctx->latest_ack[conn_id] = psn;
        }
    } else {
        // NAK: 需要重传
        printf("[SWITCH] Upstream NAK from conn %d for PSN=%u\n", conn_id, psn);
        // TODO: 实现重传逻辑
    }
}

/**
 * @brief 数据包处理回调函数
 *
 * 协议说明：
 * - 控制消息：使用 Send Only with Immediate (opcode 0x05)，payload 很小（4字节），只携带元数据
 * - 数据消息：使用普通 Send (opcode 0x04/0x00/0x01/0x02)，不携带 Immediate Data
 * - Immediate Data 格式: [dest_rank:16][primitive:2][operator:2][datatype:4][reserved:8]
 */
static void packet_handler(uint8_t *user_data, const struct pcap_pkthdr *pkthdr, const uint8_t *packet) {
    switch_context_t *ctx = &g_switch_ctx;
    int conn_id = atoi((char*)user_data);

    // 解析数据包
    eth_header_t *eth = (eth_header_t*)packet;
    ipv4_header_t *ip = (ipv4_header_t*)(packet + sizeof(eth_header_t));
    udp_header_t *udp = (udp_header_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t));
    bth_header_t *bth = (bth_header_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(udp_header_t));

    uint32_t psn = ntohl(bth->apsn) & 0x00FFFFFF;
    uint8_t opcode = bth->opcode;

    printf("[SWITCH] conn_id=%d: Received packet PSN=%u, opcode=0x%02x\n", conn_id, psn, opcode);

    // 查找路由规则
    // 从上下文获取当前操作类型
    primitive_type_t prim = ctx->operation_type;
    int prim_param = ctx->root_rank;

    // 检查是否是 Send with Immediate (opcode 0x05 或 0x03)
    // 0x05 = RC Send Only with Immediate
    // 0x03 = RC Send Last with Immediate
    int has_imm_data = (opcode == RDMA_OPCODE_SEND_ONLY_WITH_IMM || opcode == RDMA_OPCODE_SEND_LAST_WITH_IMM);

    // 计算 payload 长度来判断是否是控制消息
    // UDP length = UDP header (8) + BTH (12) + [ImmDt (4)] + Payload + ICRC (4)
    int imm_offset = has_imm_data ? 4 : 0;
    int payload_len = ntohs(udp->length) - sizeof(udp_header_t) - sizeof(bth_header_t) - 4 - imm_offset;

    // 判断是否是控制消息：opcode 0x05 且 payload 很小（<= 4 字节）
    int is_control_message = (opcode == RDMA_OPCODE_SEND_ONLY_WITH_IMM && payload_len <= 4);

    if (has_imm_data) {
        // 提取 Immediate Data
        uint32_t *imm_ptr = (uint32_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) +
                                         sizeof(udp_header_t) + sizeof(bth_header_t));
        uint32_t imm_data = ntohl(*imm_ptr);

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

        printf("[SWITCH] Immediate Data: imm=0x%08X, dest_rank=%u, prim=%d, op=%d, dtype=%d\n",
               imm_data, dest_rank, primitive, op, datatype);
        printf("[SWITCH] Parsed: operation_type=%d, root_rank=%d, is_control=%d, ctrl_psn=%d\n",
               prim, prim_param, is_control_message, ctx->ctrl_psn);
    }

    // 如果是控制消息，只需要发送 ACK，不需要聚合
    if (is_control_message) {
        printf("[SWITCH] Control message received from conn %d, PSN=%u\n", conn_id, psn);

        // 查找规则（使用刚解析的元数据）
        char src_str[INET_ADDRSTRLEN], dst_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ip->src_ip, src_str, sizeof(src_str));
        inet_ntop(AF_INET, &ip->dst_ip, dst_str, sizeof(dst_str));
        printf("[SWITCH] lookup_rule: src=%s, dst=%s, prim=%d, param=%d\n", src_str, dst_str, prim, prim_param);

        rule_t *rule = lookup_rule(&ctx->routing_table, ip->src_ip, ip->dst_ip, prim, prim_param);
        if (!rule) {
            printf("[SWITCH] No rule found for control message from conn %d\n", conn_id);
            return;
        }

        // 发送 ACK 并更新 ePSN
        ctx->agg_epsn[conn_id]++;
        forwarding(ctx, rule, psn, PACKET_TYPE_ACK, NULL, 0, RDMA_OPCODE_ACK);
        printf("[SWITCH] Control message ACK sent, ePSN[%d] updated to %d\n", conn_id, ctx->agg_epsn[conn_id]);
        return;
    }

    // 调试：打印查找参数
    char src_str[INET_ADDRSTRLEN], dst_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip->src_ip, src_str, sizeof(src_str));
    inet_ntop(AF_INET, &ip->dst_ip, dst_str, sizeof(dst_str));
    printf("[SWITCH] lookup_rule: src=%s, dst=%s, prim=%d, param=%d\n", src_str, dst_str, prim, prim_param);

    rule_t *rule = lookup_rule(&ctx->routing_table, ip->src_ip, ip->dst_ip, prim, prim_param);
    if (!rule) {
        printf("[SWITCH] No rule found for packet from conn %d\n", conn_id);
        return;
    }
    printf("[SWITCH] Rule found: direction=%d, root=%d, out_cnt=%d\n", rule->direction, rule->root, rule->out_conns_cnt);

    // 处理数据包
    if (opcode == RDMA_OPCODE_SEND_ONLY_WITH_IMM || opcode == RDMA_OPCODE_SEND_LAST_WITH_IMM ||
        opcode == RDMA_OPCODE_SEND_ONLY || opcode == RDMA_OPCODE_SEND_FIRST ||
        opcode == RDMA_OPCODE_SEND_MIDDLE || opcode == RDMA_OPCODE_SEND_LAST) {
        // RDMA SEND 数据包
        uint32_t *data = (uint32_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) +
                                      sizeof(udp_header_t) + sizeof(bth_header_t) + imm_offset);
        int data_len = payload_len / sizeof(uint32_t);

        printf("[SWITCH] Data packet: imm_offset=%d, data_len=%d\n", imm_offset, data_len);

        if (rule->direction == DIR_DOWN) {
            // 下行数据：缓存并广播
            printf("[SWITCH] Downstream data PSN=%u\n", psn);

            if ((int)psn < ctx->down_epsn) {
                // 滞后：发送 ACK
                forwarding(ctx, rule, ctx->down_epsn - 1, PACKET_TYPE_ACK, NULL, 0, RDMA_OPCODE_ACK);
            } else if ((int)psn > ctx->down_epsn) {
                // 超前：发送 NAK
                forwarding(ctx, rule, ctx->down_epsn, PACKET_TYPE_NAK, NULL, 0, RDMA_OPCODE_ACK);
            } else {
                // 正常：发送 ACK 并缓存广播
                forwarding(ctx, rule, psn, PACKET_TYPE_ACK, NULL, 0, RDMA_OPCODE_ACK);
                cache_and_broadcast(ctx, rule, psn, data, data_len, opcode);
            }
        } else {
            // 上行数据：聚合
            printf("[SWITCH] Upstream data PSN=%u from conn %d\n", psn, conn_id);

            if ((int)psn < ctx->agg_epsn[conn_id]) {
                // 滞后：发送 ACK
                forwarding(ctx, rule, ctx->agg_epsn[conn_id] - 1, PACKET_TYPE_ACK, NULL, 0, RDMA_OPCODE_ACK);
            } else if ((int)psn > ctx->agg_epsn[conn_id]) {
                // 超前：发送 NAK
                forwarding(ctx, rule, ctx->agg_epsn[conn_id], PACKET_TYPE_NAK, NULL, 0, RDMA_OPCODE_ACK);
            } else {
                // 正常：发送 ACK 并聚合
                ctx->agg_epsn[conn_id]++;
                forwarding(ctx, rule, psn, PACKET_TYPE_ACK, NULL, 0, RDMA_OPCODE_ACK);
                aggregate(ctx, rule, conn_id, psn, data, data_len, opcode);
            }
        }
    } else if (opcode == RDMA_OPCODE_ACK) {
        // ACK 包
        aeth_t *aeth = (aeth_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) +
                                  sizeof(udp_header_t) + sizeof(bth_header_t));
        int is_nak = (ntohl(aeth->syn_msn) >> 29) != 0;

        if (rule->direction == DIR_DOWN) {
            handle_downstream_ack(ctx, rule, psn, is_nak);
        } else {
            handle_upstream_ack(ctx, rule, conn_id, psn, is_nak);
        }
    } else {
        printf("[SWITCH] Unknown opcode 0x%02x\n", opcode);
    }
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

    printf("[Receiver] Starting %d receiver threads...\n", ctx->fan_in);

    // 为每个连接创建 pcap handle 并启动接收
    for (int i = 0; i < ctx->fan_in; i++) {
        connection_t *conn = &ctx->conns[i];

        // 创建 pcap handle
        char errbuf[PCAP_ERRBUF_SIZE];
        conn->handle = pcap_create(conn->device, errbuf);
        if (!conn->handle) {
            fprintf(stderr, "[Receiver] Failed to create pcap for %s: %s\n", conn->device, errbuf);
            continue;
        }

        pcap_set_snaplen(conn->handle, BUFSIZ);
        pcap_set_promisc(conn->handle, 1);
        pcap_set_timeout(conn->handle, 1);
        pcap_set_immediate_mode(conn->handle, 1);

        if (pcap_activate(conn->handle) != 0) {
            fprintf(stderr, "[Receiver] Failed to activate pcap for %s: %s\n", conn->device, pcap_geterr(conn->handle));
            continue;
        }

        // 设置过滤器
        struct bpf_program fp;
        char filter_exp[128];
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &conn->peer_ip, ip_str, sizeof(ip_str));
        snprintf(filter_exp, sizeof(filter_exp), "udp port 4791 and src host %s", ip_str);

        printf("[Receiver] Connection %d: device=%s, filter=%s\n", i, conn->device, filter_exp);

        if (pcap_compile(conn->handle, &fp, filter_exp, 0, PCAP_NETMASK_UNKNOWN) == -1) {
            fprintf(stderr, "[Receiver] Failed to compile filter: %s\n", pcap_geterr(conn->handle));
            continue;
        }
        if (pcap_setfilter(conn->handle, &fp) == -1) {
            fprintf(stderr, "[Receiver] Failed to set filter: %s\n", pcap_geterr(conn->handle));
            continue;
        }

        conn->ok = 1;
    }

    // 使用 select 或轮询方式处理多个连接
    // 简化实现：为每个连接创建独立线程
    pthread_t threads[MAX_CONNECTIONS_NUM];
    static char thread_ids[MAX_CONNECTIONS_NUM][8];

    for (int i = 0; i < ctx->fan_in; i++) {
        if (!ctx->conns[i].ok) continue;

        snprintf(thread_ids[i], sizeof(thread_ids[i]), "%d", i);

        pthread_create(&threads[i], NULL, (void*(*)(void*))pcap_receiver_thread, thread_ids[i]);
    }

    // 等待所有线程
    for (int i = 0; i < ctx->fan_in; i++) {
        if (ctx->conns[i].ok) {
            pthread_join(threads[i], NULL);
        }
    }

    return NULL;
}

/**
 * @brief 单个连接的 pcap 接收线程
 */
static void *pcap_receiver_thread(void *arg) {
    int conn_id = atoi((char*)arg);
    switch_context_t *ctx = &g_switch_ctx;
    connection_t *conn = &ctx->conns[conn_id];

    printf("[Receiver] Thread %d started for device %s\n", conn_id, conn->device);

    char id_str[8];
    snprintf(id_str, sizeof(id_str), "%d", conn_id);

    pcap_loop(conn->handle, -1, packet_handler, (uint8_t*)id_str);

    pcap_close(conn->handle);
    printf("[Receiver] Thread %d stopped\n", conn_id);

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

    // 设置为根交换机（单交换机场景）
    g_switch_ctx.is_root = 1;

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

    // 等待接收线程结束
    pthread_join(receiver_tid, NULL);

    // 清理资源
    switch_context_cleanup(&g_switch_ctx);

    printf("Switch stopped\n");
    return 0;
}
