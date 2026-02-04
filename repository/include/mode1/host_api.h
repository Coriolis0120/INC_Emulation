#ifndef MODE1_HOST_API_H
#define MODE1_HOST_API_H

#include <stdint.h>
#include <infiniband/verbs.h>

#ifdef __cplusplus
extern "C" {
#endif

// Host 连接上下文
typedef struct {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    struct ibv_mr *recv_mr;

    void *send_buf;
    void *recv_buf;
    uint32_t buf_size;

    union ibv_gid local_gid;
    int gid_idx;
    uint32_t local_psn;

    int tcp_fd;
    int rank;
    int world_size;
} mode1_host_ctx_t;

// 初始化 Host 上下文
mode1_host_ctx_t *mode1_host_init(const char *switch_ip, int switch_port,
                                   int rank, int world_size, int gid_idx);

// 销毁 Host 上下文
void mode1_host_destroy(mode1_host_ctx_t *ctx);

// AllReduce 操作
int mode1_allreduce(mode1_host_ctx_t *ctx, int32_t *src, uint32_t count,
                    int32_t *dst);

// Reduce 操作
int mode1_reduce(mode1_host_ctx_t *ctx, int32_t *src, uint32_t count,
                 int32_t *dst, int root);

// Broadcast 操作
int mode1_broadcast(mode1_host_ctx_t *ctx, int32_t *data, uint32_t count,
                    int root);

#ifdef __cplusplus
}
#endif

#endif // MODE1_HOST_API_H
