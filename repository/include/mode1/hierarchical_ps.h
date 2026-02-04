#ifndef MODE1_HIERARCHICAL_PS_H
#define MODE1_HIERARCHICAL_PS_H

#include <stdint.h>
#include "qp_manager.h"
#include "inc_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

// 交换机角色
typedef enum {
    SWITCH_ROLE_LEAF = 0,   // 叶子交换机：连接 Worker
    SWITCH_ROLE_SPINE,      // 脊交换机：连接叶子交换机
    SWITCH_ROLE_ROOT        // 根交换机：全局聚合点
} switch_role_t;

// 层级上下文
typedef struct {
    switch_role_t role;
    int switch_id;

    // 上行连接（到父交换机）
    int parent_conn_id;

    // 下行连接（到子交换机或 Worker）
    int *child_conn_ids;
    int child_count;

    // 本地 Host 连接
    int *host_conn_ids;
    int host_count;
} hierarchical_ctx_t;

// 初始化层级上下文
hierarchical_ctx_t *hierarchical_init(switch_role_t role, int switch_id);

// 销毁层级上下文
void hierarchical_destroy(hierarchical_ctx_t *ctx);

// 设置父连接
void hierarchical_set_parent(hierarchical_ctx_t *ctx, int conn_id);

// 添加子连接
int hierarchical_add_child(hierarchical_ctx_t *ctx, int conn_id);

// 添加 Host 连接
int hierarchical_add_host(hierarchical_ctx_t *ctx, int conn_id);

#ifdef __cplusplus
}
#endif

#endif // MODE1_HIERARCHICAL_PS_H
