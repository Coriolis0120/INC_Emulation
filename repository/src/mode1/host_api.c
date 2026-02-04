/**
 * @file host_api.c
 * @brief Mode1 Host 端 API 实现
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "mode1/host_api.h"
#include "mode1/inc_engine.h"

#define BUF_SIZE (64 * 1024)  // 64KB per slot
#define PAYLOAD_SIZE 4096     // 每个消息的 payload 大小
#define TIMEOUT_US 120000000  // 120秒超时（支持大数据量）
#define RECV_SLOTS 16384      // 接收缓冲区槽位数（滑动窗口）
#define MAX_OUTSTANDING 8192  // 最大未完成的发送/接收请求数

// QP 信息结构（与 qp_manager.h 一致）
typedef struct {
    uint32_t qpn;
    uint32_t psn;
    union ibv_gid gid;
    uint16_t lid;
} qp_info_t;

// TCP 连接到 Switch
static int connect_to_switch(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port)
    };
    inet_pton(AF_INET, ip, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// QP 状态转换到 INIT
static int qp_to_init(mode1_host_ctx_t *ctx) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = 1,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
    };
    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    return ibv_modify_qp(ctx->qp, &attr, flags);
}

// QP 状态转换到 RTR
static int qp_to_rtr(mode1_host_ctx_t *ctx, qp_info_t *remote, int gid_idx) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024,
        .dest_qp_num = remote->qpn,
        .rq_psn = remote->psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {
            .is_global = 1,
            .dlid = remote->lid,
            .sl = 0,
            .src_path_bits = 0,
            .port_num = 1,
            .grh = {
                .dgid = remote->gid,
                .flow_label = 0,
                .hop_limit = 64,
                .sgid_index = gid_idx,
                .traffic_class = 0
            }
        }
    };
    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    return ibv_modify_qp(ctx->qp, &attr, flags);
}

// QP 状态转换到 RTS
static int qp_to_rts(mode1_host_ctx_t *ctx, uint32_t local_psn) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .timeout = 14,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .sq_psn = local_psn,
        .max_rd_atomic = 1
    };
    int flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    return ibv_modify_qp(ctx->qp, &attr, flags);
}

// 初始化 Host 上下文
mode1_host_ctx_t *mode1_host_init(const char *switch_ip, int switch_port,
                                   int rank, int world_size, int gid_idx) {
    mode1_host_ctx_t *ctx = calloc(1, sizeof(mode1_host_ctx_t));
    if (!ctx) return NULL;

    ctx->rank = rank;
    ctx->world_size = world_size;
    ctx->tcp_fd = -1;
    ctx->gid_idx = gid_idx;

    // 获取 RDMA 设备
    int num_devices;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "No RDMA devices\n");
        goto err;
    }

    ctx->ctx = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);
    if (!ctx->ctx) goto err;

    // 创建 PD
    ctx->pd = ibv_alloc_pd(ctx->ctx);
    if (!ctx->pd) goto err;

    // 创建 CQ - 增大以支持更多消息
    ctx->cq = ibv_create_cq(ctx->ctx, 16384, NULL, NULL, 0);
    if (!ctx->cq) goto err;

    // 分配缓冲区 - 增大接收缓冲区以支持滑动窗口
    ctx->buf_size = RECV_SLOTS * PAYLOAD_SIZE;  // 4096 * 4KB = 16MB
    ctx->send_buf = malloc(BUF_SIZE);
    ctx->recv_buf = malloc(ctx->buf_size);
    if (!ctx->send_buf || !ctx->recv_buf) goto err;

    // 注册 MR
    ctx->mr = ibv_reg_mr(ctx->pd, ctx->send_buf, BUF_SIZE,
                         IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->mr) goto err;

    ctx->recv_mr = ibv_reg_mr(ctx->pd, ctx->recv_buf, ctx->buf_size,
                              IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!ctx->recv_mr) goto err;

    // 创建 QP
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = ctx->cq,
        .recv_cq = ctx->cq,
        .cap = {
            .max_send_wr = 16384,
            .max_recv_wr = 16384,
            .max_send_sge = 1,
            .max_recv_sge = 1
        },
        .qp_type = IBV_QPT_RC
    };
    ctx->qp = ibv_create_qp(ctx->pd, &qp_attr);
    if (!ctx->qp) {
        fprintf(stderr, "Failed to create QP\n");
        goto err;
    }

    // 获取本地 GID
    if (ibv_query_gid(ctx->ctx, 1, gid_idx, &ctx->local_gid)) {
        fprintf(stderr, "Failed to query GID\n");
        goto err;
    }

    // 连接到 Switch
    ctx->tcp_fd = connect_to_switch(switch_ip, switch_port);
    if (ctx->tcp_fd < 0) {
        fprintf(stderr, "Failed to connect to switch %s:%d\n", switch_ip, switch_port);
        goto err;
    }

    // 准备本地 QP 信息
    qp_info_t local_info = {
        .qpn = ctx->qp->qp_num,
        .psn = rand() & 0xFFFFFF,
        .gid = ctx->local_gid,
        .lid = 0
    };
    ctx->local_psn = local_info.psn;

    // QP 转换到 INIT
    if (qp_to_init(ctx)) {
        fprintf(stderr, "Failed to modify QP to INIT\n");
        goto err;
    }

    // 交换 QP 信息 (客户端：先接收后发送)
    qp_info_t remote_info;
    if (read(ctx->tcp_fd, &remote_info, sizeof(qp_info_t)) != sizeof(qp_info_t)) {
        fprintf(stderr, "Failed to recv QP info\n");
        goto err;
    }
    if (write(ctx->tcp_fd, &local_info, sizeof(qp_info_t)) != sizeof(qp_info_t)) {
        fprintf(stderr, "Failed to send QP info\n");
        goto err;
    }

    // QP 转换到 RTR
    if (qp_to_rtr(ctx, &remote_info, gid_idx)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        goto err;
    }

    // QP 转换到 RTS
    if (qp_to_rts(ctx, local_info.psn)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        goto err;
    }

    printf("[Host %d] Connected to switch, QPN=%u\n", rank, ctx->qp->qp_num);
    return ctx;

err:
    mode1_host_destroy(ctx);
    return NULL;
}

// 销毁 Host 上下文
void mode1_host_destroy(mode1_host_ctx_t *ctx) {
    if (!ctx) return;

    if (ctx->qp) ibv_destroy_qp(ctx->qp);
    if (ctx->recv_mr) ibv_dereg_mr(ctx->recv_mr);
    if (ctx->mr) ibv_dereg_mr(ctx->mr);
    if (ctx->cq) ibv_destroy_cq(ctx->cq);
    if (ctx->pd) ibv_dealloc_pd(ctx->pd);
    if (ctx->ctx) ibv_close_device(ctx->ctx);

    free(ctx->send_buf);
    free(ctx->recv_buf);
    if (ctx->tcp_fd >= 0) close(ctx->tcp_fd);
    free(ctx);
}

// 构造 imm_data: [slot_id:16][prim:2][op:2][dtype:4][rank:8]
static uint32_t make_imm_data(uint16_t slot_id, primitive_t prim,
                               reduce_op_t op, data_type_t dtype, uint8_t rank) {
    return ((uint32_t)slot_id << 16) |
           ((prim & 0x3) << 14) |
           ((op & 0x3) << 12) |
           ((dtype & 0xF) << 8) |
           (rank & 0xFF);
}

// 发送 RDMA 消息
static int post_send(mode1_host_ctx_t *ctx, void *buf, uint32_t len,
                     uint32_t imm_data, uint64_t wr_id) {
    struct ibv_sge sge = {
        .addr = (uintptr_t)buf,
        .length = len,
        .lkey = ctx->mr->lkey
    };
    struct ibv_send_wr wr = {
        .wr_id = wr_id,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_SEND_WITH_IMM,
        .send_flags = IBV_SEND_SIGNALED,
        .imm_data = htonl(imm_data)
    };
    struct ibv_send_wr *bad_wr;
    return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

// 投递接收请求
static int post_recv(mode1_host_ctx_t *ctx, void *buf, uint32_t len, uint64_t wr_id) {
    struct ibv_sge sge = {
        .addr = (uintptr_t)buf,
        .length = len,
        .lkey = ctx->recv_mr->lkey
    };
    struct ibv_recv_wr wr = {
        .wr_id = wr_id,
        .sg_list = &sge,
        .num_sge = 1
    };
    struct ibv_recv_wr *bad_wr;
    return ibv_post_recv(ctx->qp, &wr, &bad_wr);
}

// AllReduce 操作 - 支持多消息分片，使用滑动窗口
int mode1_allreduce(mode1_host_ctx_t *ctx, int32_t *src, uint32_t count,
                    int32_t *dst) {
    if (!ctx || !src || !dst) return -1;

    uint32_t data_len = count * sizeof(int32_t);
    uint32_t msg_count = (data_len + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE;

    // 限制初始投递的请求数，避免超出 QP 队列限制
    uint32_t initial_recv = (msg_count < MAX_OUTSTANDING) ? msg_count : MAX_OUTSTANDING;
    uint32_t initial_send = (msg_count < MAX_OUTSTANDING) ? msg_count : MAX_OUTSTANDING;

    // 先投递初始接收请求
    for (uint32_t i = 0; i < initial_recv; i++) {
        uint32_t slot = i % RECV_SLOTS;
        void *recv_ptr = (char*)ctx->recv_buf + slot * PAYLOAD_SIZE;
        if (post_recv(ctx, recv_ptr, PAYLOAD_SIZE, i) != 0) {
            fprintf(stderr, "Failed to post recv %u\n", i);
            return -1;
        }
    }
    uint32_t posted_recv = initial_recv;

    // 发送初始消息
    for (uint32_t i = 0; i < initial_send; i++) {
        uint32_t offset = i * PAYLOAD_SIZE;
        uint32_t chunk_len = (offset + PAYLOAD_SIZE <= data_len) ?
                             PAYLOAD_SIZE : (data_len - offset);

        memcpy(ctx->send_buf, (char*)src + offset, chunk_len);
        uint32_t imm = make_imm_data(i, PRIM_ALLREDUCE, OP_SUM,
                                      DTYPE_INT32, ctx->rank);

        if (post_send(ctx, ctx->send_buf, chunk_len, imm, i) != 0) {
            fprintf(stderr, "Failed to post send %u\n", i);
            return -1;
        }
    }
    uint32_t posted_send = initial_send;

    printf("[Host %d] AllReduce: sent %u bytes in %u messages\n",
           ctx->rank, data_len, msg_count);

    // 滑动窗口：等待完成并补充请求
    struct ibv_wc wc;
    uint32_t send_done = 0, recv_done = 0;
    struct timeval start, now;
    gettimeofday(&start, NULL);

    while (send_done < msg_count || recv_done < msg_count) {
        int n = ibv_poll_cq(ctx->cq, 1, &wc);
        if (n < 0) {
            fprintf(stderr, "CQ poll error\n");
            return -1;
        }
        if (n > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "WC error: %d\n", wc.status);
                return -1;
            }
            if (wc.opcode == IBV_WC_SEND) {
                send_done++;
                // 补充发送请求
                if (posted_send < msg_count) {
                    uint32_t i = posted_send;
                    uint32_t offset = i * PAYLOAD_SIZE;
                    uint32_t chunk_len = (offset + PAYLOAD_SIZE <= data_len) ?
                                         PAYLOAD_SIZE : (data_len - offset);
                    memcpy(ctx->send_buf, (char*)src + offset, chunk_len);
                    uint32_t imm = make_imm_data(i, PRIM_ALLREDUCE, OP_SUM,
                                                  DTYPE_INT32, ctx->rank);
                    post_send(ctx, ctx->send_buf, chunk_len, imm, i);
                    posted_send++;
                }
            } else if (wc.opcode == IBV_WC_RECV) {
                uint32_t imm = ntohl(wc.imm_data);
                uint16_t slot_id = (imm >> 16) & 0xFFFF;
                uint32_t offset = slot_id * PAYLOAD_SIZE;
                uint32_t chunk_len = (offset + PAYLOAD_SIZE <= data_len) ?
                                     PAYLOAD_SIZE : (data_len - offset);

                uint32_t slot = slot_id % RECV_SLOTS;
                void *recv_ptr = (char*)ctx->recv_buf + slot * PAYLOAD_SIZE;
                memcpy((char*)dst + offset, recv_ptr, chunk_len);
                recv_done++;

                // 补充接收请求
                if (posted_recv < msg_count) {
                    post_recv(ctx, recv_ptr, PAYLOAD_SIZE, posted_recv);
                    posted_recv++;
                }
            }
        }

        // 检查超时
        gettimeofday(&now, NULL);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000000 +
                       (now.tv_usec - start.tv_usec);
        if (elapsed > TIMEOUT_US) {
            fprintf(stderr, "AllReduce timeout: send=%u/%u recv=%u/%u\n",
                    send_done, msg_count, recv_done, msg_count);
            return -1;
        }
    }

    printf("[Host %d] AllReduce: received result\n", ctx->rank);
    return 0;
}

// Reduce 操作（占位实现）
int mode1_reduce(mode1_host_ctx_t *ctx, int32_t *src, uint32_t count,
                 int32_t *dst, int root) {
    if (!ctx || !src) return -1;

    // TODO: 实现完整的 Reduce
    printf("[Host] Reduce: count=%u, root=%d (not implemented)\n", count, root);
    return 0;
}

// Broadcast 操作（占位实现）
int mode1_broadcast(mode1_host_ctx_t *ctx, int32_t *data, uint32_t count,
                    int root) {
    if (!ctx || !data) return -1;

    // TODO: 实现完整的 Broadcast
    printf("[Host] Broadcast: count=%u, root=%d (not implemented)\n", count, root);
    return 0;
}
