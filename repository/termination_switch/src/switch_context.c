
#include <string.h>
#include <stdio.h>
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
    state->agg_buffer.operation_type = OPERATION_TYPE_NULL;
    state->agg_buffer.root_rank = -1;
    memset(state->agg_buffer.buffer, 0, sizeof(state->agg_buffer.buffer));

    // 初始化广播缓冲区
    state->bcast_buffer.state = 0;
    state->bcast_buffer.len = 0;
    state->bcast_buffer.psn = -1;
    state->bcast_buffer.packet_type = 0;
    state->bcast_buffer.operation_type = OPERATION_TYPE_NULL;
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

    // 初始化所有PSN状态
    for (int i = 0; i < SWITCH_ARRAY_LENGTH; i++) {
        init_psn_state(&ctx->psn_states[i]);
    }

    // 初始化连接数组
    memset(ctx->conns, 0, sizeof(ctx->conns));
    ctx->fan_in = 0;
    ctx->is_root = 0;

    // 初始化PSN管理
    memset(ctx->agg_epsn, 0, sizeof(ctx->agg_epsn));
    ctx->down_epsn = 0;
    memset(ctx->latest_ack, 0, sizeof(ctx->latest_ack));
    ctx->down_ack = 0;

    // 初始化路由表
    memset(&ctx->routing_table, 0, sizeof(ctx->routing_table));
    ctx->routing_table.count = 0;
    memset(ctx->rank_to_conn, -1, sizeof(ctx->rank_to_conn));

    // 初始化元数据
    ctx->operation_type = OPERATION_TYPE_ALLREDUCE;
    ctx->root_rank = -1;
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

    // 销毁所有PSN状态
    for (int i = 0; i < SWITCH_ARRAY_LENGTH; i++) {
        destroy_psn_state(&ctx->psn_states[i]);
    }

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
    memset(ctx->latest_ack, 0, sizeof(ctx->latest_ack));
    ctx->down_ack = 0;

    // 重置元数据
    pthread_mutex_lock(&ctx->meta_mutex);
    ctx->operation_type = OPERATION_TYPE_ALLREDUCE;
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
        case OPERATION_TYPE_ALLREDUCE: return "ALLREDUCE";
        case OPERATION_TYPE_REDUCE: return "REDUCE";
        case OPERATION_TYPE_BROADCAST: return "BROADCAST";
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
        printf("    Src IP:  0x%08x\n", rule->src_ip);
        printf("    Dst IP:  0x%08x\n", rule->dst_ip);
        printf("    Dir:     %s\n", rule->direction == DIR_UP ? "UP" : "DOWN");
        printf("    Out Cnt: %d\n", rule->out_conns_cnt);
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
 * @brief 将交换机上下文导出为YAML格式
 */
int switch_context_export_yaml(switch_context_t *ctx, const char *filename) {
    if (!ctx || !filename) {
        fprintf(stderr, "Invalid parameters for YAML export\n");
        return -1;
    }

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        perror("Failed to open file for YAML export");
        return -1;
    }

    fprintf(fp, "# Switch Context Export\n");
    fprintf(fp, "# Generated automatically\n");
    fprintf(fp, "\n");

    // 基本信息
    fprintf(fp, "basic_info:\n");
    fprintf(fp, "  switch_id: %d\n", ctx->switch_id);
    fprintf(fp, "  state: %s\n", get_state_name(ctx->state));
    fprintf(fp, "  is_root: %s\n", ctx->is_root ? "true" : "false");
    fprintf(fp, "  controller_fd: %d\n", ctx->controller_fd);
    fprintf(fp, "\n");

    // 连接信息
    fprintf(fp, "connections:\n");
    fprintf(fp, "  fan_in: %d\n", ctx->fan_in);
    fprintf(fp, "  num_receivers: %d\n", ctx->num_receivers);
    fprintf(fp, "\n");

    // 元数据
    fprintf(fp, "metadata:\n");
    fprintf(fp, "  operation_type: %s\n", get_operation_name(ctx->operation_type));
    fprintf(fp, "  root_rank: %d\n", ctx->root_rank);
    fprintf(fp, "\n");

    // PSN 管理
    fprintf(fp, "psn_management:\n");
    fprintf(fp, "  uplink_expected_psn:\n");
    for (int i = 0; i < ctx->fan_in; i++) {
        fprintf(fp, "    - conn_%d: %d\n", i, ctx->agg_epsn[i]);
    }
    fprintf(fp, "  downlink_expected_psn: %d\n", ctx->down_epsn);
    fprintf(fp, "  uplink_latest_ack:\n");
    for (int i = 0; i < ctx->fan_in; i++) {
        fprintf(fp, "    - conn_%d: %d\n", i, ctx->latest_ack[i]);
    }
    fprintf(fp, "  downlink_latest_ack: %d\n", ctx->down_ack);
    fprintf(fp, "\n");

    // 路由表
    fprintf(fp, "routing_table:\n");
    fprintf(fp, "  rule_count: %d\n", ctx->routing_table.count);
    fprintf(fp, "  rules:\n");
    for (int i = 0; i < ctx->routing_table.count; i++) {
        rule_t *rule = &ctx->routing_table.rules[i];
        fprintf(fp, "    - id: %d\n", i);
        fprintf(fp, "      src_ip: 0x%08x\n", rule->src_ip);
        fprintf(fp, "      dst_ip: 0x%08x\n", rule->dst_ip);
        fprintf(fp, "      direction: %s\n", rule->direction == DIR_UP ? "UP" : "DOWN");
        fprintf(fp, "      out_conns_count: %d\n", rule->out_conns_cnt);
    }
    fprintf(fp, "\n");

    // PSN 状态统计
    int active_psn = 0;
    for (int i = 0; i < SWITCH_ARRAY_LENGTH; i++) {
        if (ctx->psn_states[i].agg_buffer.state != 0 ||
            ctx->psn_states[i].bcast_buffer.state != 0) {
            active_psn++;
        }
    }
    fprintf(fp, "psn_states:\n");
    fprintf(fp, "  total_slots: %d\n", SWITCH_ARRAY_LENGTH);
    fprintf(fp, "  active_count: %d\n", active_psn);
    fprintf(fp, "\n");

    fclose(fp);
    printf("Context exported to: %s\n", filename);
    return 0;
}
