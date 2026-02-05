
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>
#include "switch_context.h"

/**
 * @brief 初始化交换机上下文（静态分配版本）
 *
 * @param ctx 预分配的上下文指针（通常是全局变量或栈变量）
 * @param switch_id 交换机ID
 * @return 0表示成功，-1表示失败
 */

int switch_context_init(switch_context_t *ctx, int switch_id) {
    if (!ctx) {
        fprintf(stderr, "switch_context_init: ctx is NULL\n");
        return -1;
    }

    // 初始化连接数组
    memset(ctx->conns, 0, sizeof(ctx->conns));
    ctx->fan_in = 0;
    ctx->is_spine = 0;
    ctx->epoll_fd = -1;

    // create epoll for ports
    ctx->epoll_fd = epoll_create1(0);
    if (ctx->epoll_fd == -1) {
        perror("epoll_create1 failed");
        return -1;
    }

    // 初始化PSN管理
    memset(ctx->data_epsn, 0, sizeof(ctx->data_epsn));
    memset(ctx->acked_psn, 0, sizeof(ctx->acked_psn));
    memset(ctx->psn_synced, 0, sizeof(ctx->psn_synced));

    // 初始化路由表
    memset(&ctx->routing_table, 0, sizeof(ctx->routing_table));
    ctx->routing_table.count = 0;

    // 初始化元数据
    ctx->operation_type = PRIMITIVE_TYPE_ALLREDUCE;
    ctx->primitive_param = -1;

    // 动态分配聚合资源
    ctx->arrival_state = (uint32_t *)calloc(SWITCH_ARRAY_LENGTH, sizeof(uint32_t));
    ctx->degree = (int *)calloc(SWITCH_ARRAY_LENGTH, sizeof(int));
    ctx->aggregator = (int **)malloc(SWITCH_ARRAY_LENGTH * sizeof(int *));

    if (!ctx->arrival_state || !ctx->degree || !ctx->aggregator) {
        fprintf(stderr, "switch_context_init: failed to allocate memory\n");
        return -1;
    }

    int payload_ints = PAYLOAD_LEN / sizeof(int);
    for (int i = 0; i < SWITCH_ARRAY_LENGTH; i++) {
        ctx->aggregator[i] = (int *)calloc(payload_ints, sizeof(int));
        if (!ctx->aggregator[i]) {
            fprintf(stderr, "switch_context_init: failed to allocate aggregator[%d]\n", i);
            return -1;
        }
    }
    printf("Allocated aggregator: %d slots x %d ints = %lu MB\n",
           SWITCH_ARRAY_LENGTH, payload_ints,
           (unsigned long)SWITCH_ARRAY_LENGTH * payload_ints * sizeof(int) / 1024 / 1024);

    // 初始化控制器通信
    ctx->controller_fd = -1;
    ctx->state = SWITCH_STATE_INIT;

    // 初始化同步机制
    pthread_mutex_init(&ctx->config_mutex, NULL);
    pthread_cond_init(&ctx->config_cond, NULL);
    ctx->config_ready = 0;

    ctx->switch_id = switch_id;

    // bitmap_mask, root_conn, is_spine 将从 Controller 的 YAML 配置中动态获取
    // 这里只设置默认值
    ctx->is_spine = 0;
    ctx->root_conn = 0;
    ctx->bitmap_mask = 0;

    printf("Switch %d initialized, waiting for YAML config...\n", switch_id);

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

    // 释放动态分配的聚合资源
    if (ctx->aggregator) {
        for (int i = 0; i < SWITCH_ARRAY_LENGTH; i++) {
            if (ctx->aggregator[i]) {
                free(ctx->aggregator[i]);
            }
        }
        free(ctx->aggregator);
        ctx->aggregator = NULL;
    }
    if (ctx->arrival_state) {
        free(ctx->arrival_state);
        ctx->arrival_state = NULL;
    }
    if (ctx->degree) {
        free(ctx->degree);
        ctx->degree = NULL;
    }

    // 销毁同步资源
    pthread_mutex_destroy(&ctx->config_mutex);
    pthread_cond_destroy(&ctx->config_cond);

    // 关闭 epoll fd
    if (ctx->epoll_fd >= 0) {
        close(ctx->epoll_fd);
        ctx->epoll_fd = -1;
    }

    // 关闭控制器连接
    if (ctx->controller_fd >= 0) {
        close(ctx->controller_fd);
        ctx->controller_fd = -1;
    }

    // 关闭所有 pcap handles
    for (int i = 0; i < ctx->fan_in; i++) {
        if (ctx->conns[i].handle) {
            pcap_close(ctx->conns[i].handle);
            ctx->conns[i].handle = NULL;
        }
    }

    printf("Switch context cleaned up\n");
}
