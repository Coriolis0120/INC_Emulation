/**
 * @file hierarchical_ps.c
 * @brief 分级参数服务器 - 层级通信管理
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mode1/hierarchical_ps.h"

#define MAX_CHILDREN 16
#define MAX_HOSTS_PER_SWITCH 32

// ============ 初始化与销毁 ============

hierarchical_ctx_t *hierarchical_init(switch_role_t role, int switch_id) {
    hierarchical_ctx_t *ctx = calloc(1, sizeof(hierarchical_ctx_t));
    if (!ctx) return NULL;

    ctx->role = role;
    ctx->switch_id = switch_id;
    ctx->parent_conn_id = -1;

    ctx->child_conn_ids = calloc(MAX_CHILDREN, sizeof(int));
    ctx->host_conn_ids = calloc(MAX_HOSTS_PER_SWITCH, sizeof(int));

    if (!ctx->child_conn_ids || !ctx->host_conn_ids) {
        free(ctx->child_conn_ids);
        free(ctx->host_conn_ids);
        free(ctx);
        return NULL;
    }

    const char *role_str = (role == SWITCH_ROLE_ROOT) ? "ROOT" :
                           (role == SWITCH_ROLE_SPINE) ? "SPINE" : "LEAF";
    printf("[Hierarchical] Switch %d initialized as %s\n", switch_id, role_str);

    return ctx;
}

void hierarchical_destroy(hierarchical_ctx_t *ctx) {
    if (!ctx) return;
    free(ctx->child_conn_ids);
    free(ctx->host_conn_ids);
    free(ctx);
}

// ============ 连接管理 ============

void hierarchical_set_parent(hierarchical_ctx_t *ctx, int conn_id) {
    if (!ctx) return;
    ctx->parent_conn_id = conn_id;
    printf("[Hierarchical] Set parent conn: %d\n", conn_id);
}

int hierarchical_add_child(hierarchical_ctx_t *ctx, int conn_id) {
    if (!ctx || ctx->child_count >= MAX_CHILDREN) return -1;
    ctx->child_conn_ids[ctx->child_count++] = conn_id;
    printf("[Hierarchical] Added child conn: %d\n", conn_id);
    return 0;
}

int hierarchical_add_host(hierarchical_ctx_t *ctx, int conn_id) {
    if (!ctx || ctx->host_count >= MAX_HOSTS_PER_SWITCH) return -1;
    ctx->host_conn_ids[ctx->host_count++] = conn_id;
    printf("[Hierarchical] Added host conn: %d\n", conn_id);
    return 0;
}
