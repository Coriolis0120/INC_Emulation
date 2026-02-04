
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

    // 初始化所有PSN状态（静态分配，无需malloc）
    for (int i = 0; i < SWITCH_ARRAY_LENGTH; i++) {
        init_psn_state(&ctx->psn_states[i]);
    }

    // 初始化 send_to_bcast 数组（静态分配，无需malloc）
    memset(ctx->send_to_bcast, 0, sizeof(ctx->send_to_bcast));

    // 初始化连接数组
    memset(ctx->conns, 0, sizeof(ctx->conns));
    ctx->fan_in = 0;
    ctx->is_spine = 0;
    ctx->epoll_fd = -1;
    
    // create epoll for ports
    ctx.epoll_fd = epoll_create1(0);
    if (ctx.epoll_fd == -1) {
        perror("epoll_create1 failed");
        return ;
    }

    // 初始化PSN管理
    memset(ctx->data_epsn, 0, sizeof(ctx->data_epsn));
    memset(ctx->acked_psn, 0, sizeof(ctx->acked_psn));

    // 初始化路由表
    memset(&ctx->routing_table, 0, sizeof(ctx->routing_table));
    ctx->routing_table.count = 0;

    // 初始化元数据
    ctx->operation_type = PRIMITIVE_TYPE_ALLREDUCE;
    ctx->primitive_param = -1;
    
    // 初始化聚合资源
    memset(ctx->arrival_state, 0, sizeof(ctx->arrival_state));
    memset(ctx->aggregator, 0, sizeof(ctx->aggregator));

    // 初始化控制器通信
    ctx->controller_fd = -1;
    ctx->state = SWITCH_STATE_INIT;

    ctx->switch_id = switch_id;

    // statically encoded
    switch(switch_id){
        case 0:
            ctx->root_conn = 0;
            ctx->bitmap_mask = 0x2;
            break;
        case 1:
            ctx->root_conn = 1;
            ctx->bitmap_mask = 0x5;
            break;
        case 2:
            ctx->root_conn = 0;
            ctx->bitmap_mask = 0x6;
            break;
    }

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

    // 关闭控制器连接
    if (ctx->controller_fd >= 0) {
        close(ctx->controller_fd);
        ctx->controller_fd = -1;
    }

    // send_to_bcast 是静态数组，无需释放

    printf("Switch context cleaned up\n");
}
