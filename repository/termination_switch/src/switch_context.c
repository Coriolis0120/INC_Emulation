
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "switch_context.h"

/**
 * @brief 初始化PSN状态
 *
 * @param state PSN状态指针
 */
static void init_psn_state(psn_state_t *state) {
    state->degree = 0;
    state->r_degree = 0;

    memset(state->arrival, 0, sizeof(state->arrival));
    memset(state->r_arrival, 0, sizeof(state->r_arrival));

    // 初始化聚合缓冲区
    state->agg_buffer.state = 0;
    state->agg_buffer.len = 0;
    state->agg_buffer.psn = -1;
    state->agg_buffer.packet_type = 0;
    state->agg_buffer.operation_type = PRIMITIVE_TYPE_NULL;
    state->agg_buffer.root_rank = -1;
    memset(state->agg_buffer.buffer, 0, sizeof(state->agg_buffer.buffer));

    // 初始化广播缓冲区
    state->bcast_buffer.state = 0;
    state->bcast_buffer.len = 0;
    state->bcast_buffer.psn = -1;
    state->bcast_buffer.packet_type = 0;
    state->bcast_buffer.operation_type = PRIMITIVE_TYPE_NULL;
    state->bcast_buffer.root_rank = -1;
    memset(state->bcast_buffer.buffer, 0, sizeof(state->bcast_buffer.buffer));

    // 初始化互斥锁
    pthread_mutex_init(&state->mutex, NULL);
}

/**
 * @brief 销毁PSN状态
 *
 * @param state PSN状态指针
 */
static void destroy_psn_state(psn_state_t *state) {
    pthread_mutex_destroy(&state->mutex);
}

/**
 * @brief 初始化交换机上下文（静态分配版本）
 *
 * @param ctx 预分配的上下文指针（通常是全局变量或栈变量）
 * @param switch_id 交换机ID
 * @param thread_pool_size 线程池大小
 * @return 0表示成功，-1表示失败
 */

int switch_context_init(switch_context_t *ctx, int switch_id, int thread_pool_size) {
    if (!ctx) {
        fprintf(stderr, "switch_context_init: ctx is NULL\n");
        return -1;
    }

    // 初始化所有PSN状态（静态分配，无需malloc）
    for (int i = 0; i < SWITCH_ARRAY_LENGTH; i++) {
        init_psn_state(&ctx->psn_states[i]);
    }

    // 初始化 send_to_bcast 数组（静态分配，无需malloc）
    memset(ctx->send_to_bcast, 0, sizeof(ctx->send_to_bcast));

    // 初始化连接数组
    memset(ctx->conns, 0, sizeof(ctx->conns));
    ctx->fan_in = 0;
    ctx->is_root = 0;

    // 初始化PSN管理
    memset(ctx->agg_epsn, 0, sizeof(ctx->agg_epsn));
    ctx->down_epsn = 0;
    // 初始化 latest_ack 为 -1，这样 PSN=0 的 ACK 才能被正确处理
    for (int i = 0; i < MAX_CONNECTIONS_NUM; i++) {
        ctx->latest_ack[i] = -1;
    }
    ctx->down_ack = -1;
    memset(ctx->send_psn, 0, sizeof(ctx->send_psn));  // 初始化发送PSN计数器

    // 初始化路由表
    memset(&ctx->routing_table, 0, sizeof(ctx->routing_table));
    ctx->routing_table.count = 0;
    memset(ctx->rank_to_conn, -1, sizeof(ctx->rank_to_conn));

    // 初始化元数据
    ctx->operation_type = PRIMITIVE_TYPE_ALLREDUCE;
    ctx->root_rank = -1;
    ctx->ctrl_psn = -1;  // 初始化为 -1，表示还没有收到控制消息
    ctx->ctrl_forwarded = 0;  // 初始化为 0，表示控制包未转发
    pthread_mutex_init(&ctx->meta_mutex, NULL);

    // 初始化控制器通信
    ctx->controller_fd = -1;
    ctx->state = SWITCH_STATE_INIT;
    pthread_mutex_init(&ctx->state_mutex, NULL);

    // 初始化接收线程管理
    memset(ctx->receiver_threads, 0, sizeof(ctx->receiver_threads));
    ctx->num_receivers = 0;

    // 初始化线程池
    ctx->thpool = thpool_init(thread_pool_size);
    if (!ctx->thpool) {
        fprintf(stderr, "Failed to initialize thread pool\n");
        switch_context_cleanup(ctx);
        return -1;
    }

    ctx->switch_id = switch_id;

    printf("Switch context initialized: ID=%d, PSN slots=%d, Thread pool size=%d\n",
           switch_id, SWITCH_ARRAY_LENGTH, thread_pool_size);

    return 0;
}

/**
 * @brief 清理交换机上下文（不释放ctx本身的内存）
 *
 * @param ctx 交换机上下文指针
 */
void switch_context_cleanup(switch_context_t *ctx) {
    if (!ctx) {
        return;
    }

    // 销毁线程池
    if (ctx->thpool) {
        thpool_wait(ctx->thpool);
        thpool_destroy(ctx->thpool);
        ctx->thpool = NULL;
    }

    // 销毁元数据互斥锁
    pthread_mutex_destroy(&ctx->meta_mutex);

    // 关闭控制器连接
    if (ctx->controller_fd >= 0) {
        close(ctx->controller_fd);
        ctx->controller_fd = -1;
    }
    pthread_mutex_destroy(&ctx->state_mutex);

    // 销毁所有PSN状态（静态分配，无需free）
    for (int i = 0; i < SWITCH_ARRAY_LENGTH; i++) {
        destroy_psn_state(&ctx->psn_states[i]);
    }

    // send_to_bcast 是静态数组，无需释放

    printf("Switch context cleaned up\n");
}

/**
 * @brief 重置交换机上下文的PSN状态
 *
 * 用于拓扑重配置或新的通信组开始时
 *
 * @param ctx 交换机上下文指针
 */
void switch_context_reset_psn_states(switch_context_t *ctx) {
    if (!ctx) {
        return;
    }

    // 重置所有PSN状态
    for (int i = 0; i < SWITCH_ARRAY_LENGTH; i++) {
        psn_state_t *state = &ctx->psn_states[i];
        pthread_mutex_lock(&state->mutex);

        state->degree = 0;
        state->r_degree = 0;
        memset(state->arrival, 0, sizeof(state->arrival));
        memset(state->r_arrival, 0, sizeof(state->r_arrival));

        state->agg_buffer.state = 0;
        state->agg_buffer.len = 0;
        state->agg_buffer.psn = -1;

        state->bcast_buffer.state = 0;
        state->bcast_buffer.len = 0;
        state->bcast_buffer.psn = -1;

        pthread_mutex_unlock(&state->mutex);
    }

    // 重置PSN管理
    memset(ctx->agg_epsn, 0, sizeof(ctx->agg_epsn));
    ctx->down_epsn = 0;
    // 重置 latest_ack 为 -1
    for (int i = 0; i < MAX_CONNECTIONS_NUM; i++) {
        ctx->latest_ack[i] = -1;
    }
    ctx->down_ack = -1;

    // 重置元数据
    pthread_mutex_lock(&ctx->meta_mutex);
    ctx->operation_type = PRIMITIVE_TYPE_ALLREDUCE;
    ctx->root_rank = -1;
    pthread_mutex_unlock(&ctx->meta_mutex);

    printf("PSN states reset\n");
}

/**
 * @brief 获取状态名称字符串
 */
static const char* get_state_name(switch_state_t state) {
    switch (state) {
        case SWITCH_STATE_INIT: return "INIT";
        case SWITCH_STATE_STALLED: return "STALLED";
        case SWITCH_STATE_RUNNING: return "RUNNING";
        default: return "UNKNOWN";
    }
}

/**
 * @brief 获取操作类型名称字符串
 */
static const char* get_operation_name(primitive_type_t op) {
    switch (op) {
        case PRIMITIVE_TYPE_NULL: return "NULL";
        case PRIMITIVE_TYPE_ALLREDUCE: return "ALLREDUCE";
        case PRIMITIVE_TYPE_REDUCE: return "REDUCE";
        case PRIMITIVE_TYPE_BROADCAST: return "BROADCAST";
        default: return "UNKNOWN";
    }
}

/**
 * @brief 获取原语类型名称字符串
 */
static const char* get_primitive_name(primitive_type_t prim) {
    switch (prim) {
        case PRIMITIVE_TYPE_NULL: return "NULL";
        case PRIMITIVE_TYPE_ALLREDUCE: return "ALLREDUCE";
        case PRIMITIVE_TYPE_REDUCE: return "REDUCE";
        case PRIMITIVE_TYPE_BROADCAST: return "BROADCAST";
        default: return "UNKNOWN";
    }
}

/**
 * @brief 打印交换机上下文状态
 */
void switch_context_print(switch_context_t *ctx) {
    if (!ctx) {
        printf("Context is NULL\n");
        return;
    }

    printf("\n");
    printf("========================================\n");
    printf("       Switch Context Status\n");
    printf("========================================\n");
    printf("\n");

    // 基本信息
    printf("=== Basic Info ===\n");
    printf("  Switch ID:        %d\n", ctx->switch_id);
    printf("  State:            %s\n", get_state_name(ctx->state));
    printf("  Is Root:          %s\n", ctx->is_root ? "Yes" : "No");
    printf("  Controller FD:    %d\n", ctx->controller_fd);
    printf("\n");

    // 连接信息
    printf("=== Connections ===\n");
    printf("  Fan-in:           %d\n", ctx->fan_in);
    printf("  Num Receivers:    %d\n", ctx->num_receivers);
    printf("  Total Conns:      (check conns array)\n");
    printf("\n");

    // 元数据
    printf("=== Metadata ===\n");
    printf("  Operation Type:   %s\n", get_operation_name(ctx->operation_type));
    printf("  Root Rank:        %d\n", ctx->root_rank);
    printf("\n");

    // PSN 管理
    printf("=== PSN Management ===\n");
    printf("  Expected PSN (uplink):\n");
    for (int i = 0; i < ctx->fan_in && i < 5; i++) {
        printf("    [%d]: %d\n", i, ctx->agg_epsn[i]);
    }
    if (ctx->fan_in > 5) {
        printf("    ... (%d more)\n", ctx->fan_in - 5);
    }
    printf("  Expected PSN (downlink): %d\n", ctx->down_epsn);
    printf("  Latest ACK (uplink):\n");
    for (int i = 0; i < ctx->fan_in && i < 5; i++) {
        printf("    [%d]: %d\n", i, ctx->latest_ack[i]);
    }
    if (ctx->fan_in > 5) {
        printf("    ... (%d more)\n", ctx->fan_in - 5);
    }
    printf("  Latest ACK (downlink): %d\n", ctx->down_ack);
    printf("\n");

    // 路由表
    printf("=== Routing Table ===\n");
    printf("  Rule Count:       %d\n", ctx->routing_table.count);
    for (int i = 0; i < ctx->routing_table.count && i < 5; i++) {
        rule_t *rule = &ctx->routing_table.rules[i];
        printf("  Rule[%d]:\n", i);
        printf("    Src IP:     0x%08x\n", rule->src_ip);
        printf("    Dst IP:     0x%08x\n", rule->dst_ip);
        printf("    Primitive:  %s (%d)\n", get_primitive_name(rule->primitive), rule->primitive);
        printf("    Param:      %d\n", rule->primitive_param);
        printf("    Direction:  %s\n", rule->direction == DIR_UP ? "UP" : "DOWN");
        printf("    Root:       %s\n", rule->root ? "Yes" : "No");
        printf("    Conn ID:    %d\n", rule->conn_id);
        printf("    Out Cnt:    %d\n", rule->out_conns_cnt);
    }
    if (ctx->routing_table.count > 5) {
        printf("  ... (%d more rules)\n", ctx->routing_table.count - 5);
    }
    printf("\n");

    // PSN 状态统计
    printf("=== PSN States Summary ===\n");
    int active_psn = 0;
    for (int i = 0; i < SWITCH_ARRAY_LENGTH; i++) {
        if (ctx->psn_states[i].agg_buffer.state != 0 ||
            ctx->psn_states[i].bcast_buffer.state != 0) {
            active_psn++;
        }
    }
    printf("  Total PSN Slots:  %d\n", SWITCH_ARRAY_LENGTH);
    printf("  Active PSN:       %d\n", active_psn);
    printf("\n");

    printf("========================================\n");
    printf("\n");
}

/**
 * @brief 获取连接到父交换机的连接 ID
 *
 * 遍历所有连接，找到 is_switch=1 且 peer_id < switch_id 的连接
 *
 * @param ctx 交换机上下文指针
 * @return 父交换机连接 ID，如果没有则返回 -1
 */
int get_parent_switch_conn(switch_context_t *ctx) {
    if (!ctx) {
        return -1;
    }

    for (int i = 0; i < ctx->fan_in; i++) {
        connection_t *conn = &ctx->conns[i];
        if (conn->is_switch && conn->peer_id < ctx->switch_id) {
            return i;
        }
    }
    return -1;
}
