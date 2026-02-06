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
#define TIMEOUT_US 300000000  // 300秒超时（支持大数据量）
#define RECV_SLOTS 8192       // 接收缓冲区槽位数（滑动窗口复用）
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
    printf("[Host %d] mode1_host_init: switch=%s:%d, world_size=%d, gid_idx=%d\n",
           rank, switch_ip, switch_port, world_size, gid_idx);
    fflush(stdout);

    mode1_host_ctx_t *ctx = calloc(1, sizeof(mode1_host_ctx_t));
    if (!ctx) {
        fprintf(stderr, "[Host %d] ERROR: calloc failed\n", rank);
        return NULL;
    }

    ctx->rank = rank;
    ctx->world_size = world_size;
    ctx->tcp_fd = -1;
    ctx->gid_idx = gid_idx;

    // 获取 RDMA 设备
    printf("[Host %d] Getting RDMA devices...\n", rank);
    fflush(stdout);
    int num_devices;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "[Host %d] ERROR: No RDMA devices\n", rank);
        goto err;
    }
    printf("[Host %d] Found %d RDMA devices\n", rank, num_devices);
    fflush(stdout);

    ctx->ctx = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);
    if (!ctx->ctx) {
        fprintf(stderr, "[Host %d] ERROR: ibv_open_device failed\n", rank);
        goto err;
    }
    printf("[Host %d] Opened RDMA device\n", rank);
    fflush(stdout);

    // 创建 PD
    ctx->pd = ibv_alloc_pd(ctx->ctx);
    if (!ctx->pd) {
        fprintf(stderr, "[Host %d] ERROR: ibv_alloc_pd failed\n", rank);
        goto err;
    }
    printf("[Host %d] Created PD\n", rank);
    fflush(stdout);

    // 创建 CQ - 增大以支持更多消息
    ctx->cq = ibv_create_cq(ctx->ctx, 16384, NULL, NULL, 0);
    if (!ctx->cq) {
        fprintf(stderr, "[Host %d] ERROR: ibv_create_cq failed\n", rank);
        goto err;
    }
    printf("[Host %d] Created CQ with size 16384\n", rank);
    fflush(stdout);

    // 分配缓冲区 - 增大接收缓冲区以支持滑动窗口
    ctx->buf_size = RECV_SLOTS * PAYLOAD_SIZE;  // 16384 * 4KB = 64MB
    printf("[Host %d] Allocating buffers: send=%d, recv=%u\n",
           rank, BUF_SIZE, ctx->buf_size);
    fflush(stdout);
    ctx->send_buf = malloc(BUF_SIZE);
    ctx->recv_buf = malloc(ctx->buf_size);
    if (!ctx->send_buf || !ctx->recv_buf) {
        fprintf(stderr, "[Host %d] ERROR: malloc failed\n", rank);
        goto err;
    }
    printf("[Host %d] Allocated buffers\n", rank);
    fflush(stdout);

    // 注册 MR
    ctx->mr = ibv_reg_mr(ctx->pd, ctx->send_buf, BUF_SIZE,
                         IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->mr) {
        fprintf(stderr, "[Host %d] ERROR: ibv_reg_mr (send) failed\n", rank);
        goto err;
    }
    printf("[Host %d] Registered send MR\n", rank);
    fflush(stdout);

    ctx->recv_mr = ibv_reg_mr(ctx->pd, ctx->recv_buf, ctx->buf_size,
                              IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!ctx->recv_mr) {
        fprintf(stderr, "[Host %d] ERROR: ibv_reg_mr (recv) failed\n", rank);
        goto err;
    }
    printf("[Host %d] Registered recv MR, size=%u\n", rank, ctx->buf_size);
    fflush(stdout);

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
        fprintf(stderr, "[Host %d] ERROR: Failed to create QP\n", rank);
        goto err;
    }
    printf("[Host %d] Created QP, QPN=%u\n", rank, ctx->qp->qp_num);
    fflush(stdout);

    // 获取本地 GID
    if (ibv_query_gid(ctx->ctx, 1, gid_idx, &ctx->local_gid)) {
        fprintf(stderr, "[Host %d] ERROR: Failed to query GID\n", rank);
        goto err;
    }
    printf("[Host %d] Got local GID\n", rank);
    fflush(stdout);

    // 连接到 Switch
    printf("[Host %d] Connecting to switch %s:%d...\n", rank, switch_ip, switch_port);
    fflush(stdout);
    ctx->tcp_fd = connect_to_switch(switch_ip, switch_port);
    if (ctx->tcp_fd < 0) {
        fprintf(stderr, "[Host %d] ERROR: Failed to connect to switch\n", rank);
        goto err;
    }
    printf("[Host %d] TCP connected, fd=%d\n", rank, ctx->tcp_fd);
    fflush(stdout);

    // 准备本地 QP 信息
    qp_info_t local_info = {
        .qpn = ctx->qp->qp_num,
        .psn = rand() & 0xFFFFFF,
        .gid = ctx->local_gid,
        .lid = 0
    };
    ctx->local_psn = local_info.psn;
    printf("[Host %d] Local QP info: QPN=%u, PSN=%u\n", rank, local_info.qpn, local_info.psn);
    fflush(stdout);

    // QP 转换到 INIT
    printf("[Host %d] Modifying QP to INIT...\n", rank);
    fflush(stdout);
    if (qp_to_init(ctx)) {
        fprintf(stderr, "[Host %d] ERROR: Failed to modify QP to INIT\n", rank);
        goto err;
    }
    printf("[Host %d] QP in INIT state\n", rank);
    fflush(stdout);

    // 交换 QP 信息 (客户端：先接收后发送)
    printf("[Host %d] Exchanging QP info...\n", rank);
    fflush(stdout);
    qp_info_t remote_info;
    if (read(ctx->tcp_fd, &remote_info, sizeof(qp_info_t)) != sizeof(qp_info_t)) {
        fprintf(stderr, "[Host %d] ERROR: Failed to recv QP info\n", rank);
        goto err;
    }
    printf("[Host %d] Received remote QP info: QPN=%u, PSN=%u\n",
           rank, remote_info.qpn, remote_info.psn);
    fflush(stdout);
    if (write(ctx->tcp_fd, &local_info, sizeof(qp_info_t)) != sizeof(qp_info_t)) {
        fprintf(stderr, "[Host %d] ERROR: Failed to send QP info\n", rank);
        goto err;
    }
    printf("[Host %d] Sent local QP info\n", rank);
    fflush(stdout);

    // QP 转换到 RTR
    printf("[Host %d] Modifying QP to RTR...\n", rank);
    fflush(stdout);
    if (qp_to_rtr(ctx, &remote_info, gid_idx)) {
        fprintf(stderr, "[Host %d] ERROR: Failed to modify QP to RTR\n", rank);
        goto err;
    }
    printf("[Host %d] QP in RTR state\n", rank);
    fflush(stdout);

    // QP 转换到 RTS
    printf("[Host %d] Modifying QP to RTS...\n", rank);
    fflush(stdout);
    if (qp_to_rts(ctx, local_info.psn)) {
        fprintf(stderr, "[Host %d] ERROR: Failed to modify QP to RTS\n", rank);
        goto err;
    }
    printf("[Host %d] QP in RTS state\n", rank);
    fflush(stdout);

    printf("[Host %d] Connected to switch, QPN=%u\n", rank, ctx->qp->qp_num);
    fflush(stdout);
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

// 构造 imm_data: [slot_id:20][prim:2][op:2][sender:4][root:4]
// sender: 发送者 rank (0-15), root: 目标 rank (0-15，仅 Reduce 使用)
static uint32_t make_imm_data(uint32_t slot_id, primitive_t prim,
                               reduce_op_t op, data_type_t dtype,
                               uint8_t sender, uint8_t root) {
    (void)dtype;
    return ((slot_id & 0xFFFFF) << 12) |
           ((prim & 0x3) << 10) |
           ((op & 0x3) << 8) |
           ((sender & 0xF) << 4) |
           (root & 0xF);
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

    printf("[Host %d] AllReduce start: count=%u, data_len=%u, msg_count=%u\n",
           ctx->rank, count, data_len, msg_count);
    fflush(stdout);

    // 限制初始投递的请求数，避免超出 QP 队列限制
    uint32_t initial_recv = (msg_count < MAX_OUTSTANDING) ? msg_count : MAX_OUTSTANDING;
    uint32_t initial_send = (msg_count < MAX_OUTSTANDING) ? msg_count : MAX_OUTSTANDING;

    printf("[Host %d] initial_recv=%u, initial_send=%u\n",
           ctx->rank, initial_recv, initial_send);
    fflush(stdout);

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
                                      DTYPE_INT32, ctx->rank, 0xF);

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
            fprintf(stderr, "[Host %d] CQ poll error\n", ctx->rank);
            return -1;
        }
        if (n > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[Host %d] WC error: %d, opcode=%d, wr_id=%lu\n",
                        ctx->rank, wc.status, wc.opcode, wc.wr_id);
                return -1;
            }
            if (wc.opcode == IBV_WC_SEND) {
                send_done++;
                if (send_done % 1000 == 0) {
                    printf("[Host %d] send_done=%u/%u\n", ctx->rank, send_done, msg_count);
                    fflush(stdout);
                }
                // 补充发送请求
                if (posted_send < msg_count) {
                    uint32_t i = posted_send;
                    uint32_t offset = i * PAYLOAD_SIZE;
                    uint32_t chunk_len = (offset + PAYLOAD_SIZE <= data_len) ?
                                         PAYLOAD_SIZE : (data_len - offset);
                    memcpy(ctx->send_buf, (char*)src + offset, chunk_len);
                    uint32_t imm = make_imm_data(i, PRIM_ALLREDUCE, OP_SUM,
                                                  DTYPE_INT32, ctx->rank, 0xF);
                    post_send(ctx, ctx->send_buf, chunk_len, imm, i);
                    posted_send++;
                }
            } else if (wc.opcode == IBV_WC_RECV) {
                uint32_t imm = ntohl(wc.imm_data);
                uint32_t slot_id = (imm >> 12) & 0xFFFFF;  // 20位 slot_id
                uint32_t offset = slot_id * PAYLOAD_SIZE;
                uint32_t chunk_len = (offset + PAYLOAD_SIZE <= data_len) ?
                                     PAYLOAD_SIZE : (data_len - offset);

                uint32_t slot = slot_id % RECV_SLOTS;
                void *recv_ptr = (char*)ctx->recv_buf + slot * PAYLOAD_SIZE;
                memcpy((char*)dst + offset, recv_ptr, chunk_len);
                recv_done++;

                if (recv_done % 1000 == 0) {
                    printf("[Host %d] recv_done=%u/%u, slot_id=%u\n",
                           ctx->rank, recv_done, msg_count, slot_id);
                    fflush(stdout);
                }

                // 补充接收请求 - 使用正确的缓冲区槽位
                if (posted_recv < msg_count) {
                    uint32_t new_slot = posted_recv % RECV_SLOTS;
                    void *new_recv_ptr = (char*)ctx->recv_buf + new_slot * PAYLOAD_SIZE;
                    post_recv(ctx, new_recv_ptr, PAYLOAD_SIZE, posted_recv);
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

// Reduce 操作 - 所有节点发送数据，只有 root 节点接收聚合结果
int mode1_reduce(mode1_host_ctx_t *ctx, int32_t *src, uint32_t count,
                 int32_t *dst, int root) {
    if (!ctx || !src) return -1;

    uint32_t data_len = count * sizeof(int32_t);
    uint32_t msg_count = (data_len + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE;
    int is_root = (ctx->rank == root);

    printf("[Host %d] Reduce start: count=%u, data_len=%u, msg_count=%u, root=%d, is_root=%d\n",
           ctx->rank, count, data_len, msg_count, root, is_root);
    fflush(stdout);

    // 限制初始投递的请求数
    uint32_t initial_send = (msg_count < MAX_OUTSTANDING) ? msg_count : MAX_OUTSTANDING;

    // 只有 root 节点需要接收结果
    uint32_t initial_recv = 0;
    if (is_root) {
        initial_recv = (msg_count < MAX_OUTSTANDING) ? msg_count : MAX_OUTSTANDING;
    }

    printf("[Host %d] Reduce: initial_recv=%u, initial_send=%u\n",
           ctx->rank, initial_recv, initial_send);
    fflush(stdout);

    // root 节点先投递接收请求
    if (is_root) {
        printf("[Host %d] Reduce: posting %u recv requests (root)\n", ctx->rank, initial_recv);
        fflush(stdout);
        for (uint32_t i = 0; i < initial_recv; i++) {
            uint32_t slot = i % RECV_SLOTS;
            void *recv_ptr = (char*)ctx->recv_buf + slot * PAYLOAD_SIZE;
            if (post_recv(ctx, recv_ptr, PAYLOAD_SIZE, i) != 0) {
                fprintf(stderr, "[Host %d] Reduce: Failed to post recv %u\n", ctx->rank, i);
                return -1;
            }
        }
    }
    uint32_t posted_recv = initial_recv;

    // 所有节点发送数据，imm_data 中携带 root_rank 信息
    // 格式: [slot_id:20][prim:2][op:2][root_rank:8]
    printf("[Host %d] Reduce: posting %u send requests\n", ctx->rank, initial_send);
    fflush(stdout);
    for (uint32_t i = 0; i < initial_send; i++) {
        uint32_t offset = i * PAYLOAD_SIZE;
        uint32_t chunk_len = (offset + PAYLOAD_SIZE <= data_len) ?
                             PAYLOAD_SIZE : (data_len - offset);

        memcpy(ctx->send_buf, (char*)src + offset, chunk_len);
        // 对于 Reduce，sender=ctx->rank, root=目标节点
        uint32_t imm = make_imm_data(i, PRIM_REDUCE, OP_SUM, DTYPE_INT32, ctx->rank, root);

        if (post_send(ctx, ctx->send_buf, chunk_len, imm, i) != 0) {
            fprintf(stderr, "[Host %d] Reduce: Failed to post send %u\n", ctx->rank, i);
            return -1;
        }
    }
    uint32_t posted_send = initial_send;

    printf("[Host %d] Reduce: initial sends posted, waiting for completions...\n", ctx->rank);
    fflush(stdout);

    // 滑动窗口：等待完成并补充请求
    struct ibv_wc wc;
    uint32_t send_done = 0, recv_done = 0;
    struct timeval start, now;
    gettimeofday(&start, NULL);

    // root 节点需要等待发送和接收都完成
    // 非 root 节点只需要等待发送完成
    uint32_t expected_recv = is_root ? msg_count : 0;

    printf("[Host %d] Reduce: expected send_done=%u, recv_done=%u\n",
           ctx->rank, msg_count, expected_recv);
    fflush(stdout);

    while (send_done < msg_count || recv_done < expected_recv) {
        int n = ibv_poll_cq(ctx->cq, 1, &wc);
        if (n < 0) {
            fprintf(stderr, "[Host %d] Reduce: CQ poll error\n", ctx->rank);
            return -1;
        }
        if (n > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[Host %d] Reduce: WC error: %d, opcode=%d, wr_id=%lu\n",
                        ctx->rank, wc.status, wc.opcode, wc.wr_id);
                return -1;
            }
            if (wc.opcode == IBV_WC_SEND) {
                send_done++;
                if (send_done % 1000 == 0 || send_done == msg_count) {
                    printf("[Host %d] Reduce: send_done=%u/%u\n",
                           ctx->rank, send_done, msg_count);
                    fflush(stdout);
                }
                // 补充发送请求
                if (posted_send < msg_count) {
                    uint32_t i = posted_send;
                    uint32_t offset = i * PAYLOAD_SIZE;
                    uint32_t chunk_len = (offset + PAYLOAD_SIZE <= data_len) ?
                                         PAYLOAD_SIZE : (data_len - offset);
                    memcpy(ctx->send_buf, (char*)src + offset, chunk_len);
                    uint32_t imm = make_imm_data(i, PRIM_REDUCE, OP_SUM, DTYPE_INT32, ctx->rank, root);
                    post_send(ctx, ctx->send_buf, chunk_len, imm, i);
                    posted_send++;
                }
            } else if (wc.opcode == IBV_WC_RECV && is_root) {
                // 只有 root 节点处理接收
                uint32_t imm = ntohl(wc.imm_data);
                uint32_t slot_id = (imm >> 12) & 0xFFFFF;
                uint32_t offset = slot_id * PAYLOAD_SIZE;
                uint32_t chunk_len = (offset + PAYLOAD_SIZE <= data_len) ?
                                     PAYLOAD_SIZE : (data_len - offset);

                uint32_t slot = slot_id % RECV_SLOTS;
                void *recv_ptr = (char*)ctx->recv_buf + slot * PAYLOAD_SIZE;
                memcpy((char*)dst + offset, recv_ptr, chunk_len);
                recv_done++;

                if (recv_done % 1000 == 0 || recv_done == msg_count) {
                    printf("[Host %d] Reduce: recv_done=%u/%u, slot_id=%u\n",
                           ctx->rank, recv_done, msg_count, slot_id);
                    fflush(stdout);
                }

                // 补充接收请求
                if (posted_recv < msg_count) {
                    uint32_t new_slot = posted_recv % RECV_SLOTS;
                    void *new_recv_ptr = (char*)ctx->recv_buf + new_slot * PAYLOAD_SIZE;
                    post_recv(ctx, new_recv_ptr, PAYLOAD_SIZE, posted_recv);
                    posted_recv++;
                }
            }
        }

        // 检查超时
        gettimeofday(&now, NULL);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000000 +
                       (now.tv_usec - start.tv_usec);
        if (elapsed > TIMEOUT_US) {
            fprintf(stderr, "[Host %d] Reduce timeout: send=%u/%u recv=%u/%u\n",
                    ctx->rank, send_done, msg_count, recv_done, expected_recv);
            return -1;
        }
    }

    printf("[Host %d] Reduce complete: is_root=%d\n", ctx->rank, is_root);
    fflush(stdout);
    return 0;
}

// Broadcast 操作 - root 节点发送数据，所有节点接收
int mode1_broadcast(mode1_host_ctx_t *ctx, int32_t *data, uint32_t count,
                    int root) {
    if (!ctx || !data) return -1;

    uint32_t data_len = count * sizeof(int32_t);
    uint32_t msg_count = (data_len + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE;
    int is_root = (ctx->rank == root);

    printf("[Host %d] Broadcast start: count=%u, data_len=%u, msg_count=%u, root=%d, is_root=%d\n",
           ctx->rank, count, data_len, msg_count, root, is_root);
    fflush(stdout);

    // 限制初始投递的请求数
    uint32_t initial_recv = (msg_count < MAX_OUTSTANDING) ? msg_count : MAX_OUTSTANDING;
    uint32_t initial_send = is_root ?
        ((msg_count < MAX_OUTSTANDING) ? msg_count : MAX_OUTSTANDING) : 0;

    printf("[Host %d] Broadcast: initial_recv=%u, initial_send=%u\n",
           ctx->rank, initial_recv, initial_send);
    fflush(stdout);

    // 所有节点都投递接收请求（包括 root，因为 root 也会收到广播）
    for (uint32_t i = 0; i < initial_recv; i++) {
        uint32_t slot = i % RECV_SLOTS;
        void *recv_ptr = (char*)ctx->recv_buf + slot * PAYLOAD_SIZE;
        if (post_recv(ctx, recv_ptr, PAYLOAD_SIZE, i) != 0) {
            fprintf(stderr, "[Host %d] Broadcast: Failed to post recv %u\n", ctx->rank, i);
            return -1;
        }
    }
    uint32_t posted_recv = initial_recv;

    // 只有 root 节点发送数据
    if (is_root) {
        for (uint32_t i = 0; i < initial_send; i++) {
            uint32_t offset = i * PAYLOAD_SIZE;
            uint32_t chunk_len = (offset + PAYLOAD_SIZE <= data_len) ?
                                 PAYLOAD_SIZE : (data_len - offset);

            memcpy(ctx->send_buf, (char*)data + offset, chunk_len);
            // sender=root, root=root (标识广播源)
            uint32_t imm = make_imm_data(i, PRIM_BROADCAST, OP_SUM,
                                          DTYPE_INT32, ctx->rank, root);

            if (post_send(ctx, ctx->send_buf, chunk_len, imm, i) != 0) {
                fprintf(stderr, "[Host %d] Broadcast: Failed to post send %u\n", ctx->rank, i);
                return -1;
            }
        }
    }
    uint32_t posted_send = initial_send;

    printf("[Host %d] Broadcast: initial posts done, waiting for completions...\n", ctx->rank);
    fflush(stdout);

    // 滑动窗口：等待完成并补充请求
    struct ibv_wc wc;
    uint32_t send_done = 0, recv_done = 0;
    struct timeval start, now;
    gettimeofday(&start, NULL);

    // root 需要等待发送和接收，非 root 只等待接收
    uint32_t expected_send = is_root ? msg_count : 0;

    while (send_done < expected_send || recv_done < msg_count) {
        int n = ibv_poll_cq(ctx->cq, 1, &wc);
        if (n < 0) {
            fprintf(stderr, "[Host %d] Broadcast: CQ poll error\n", ctx->rank);
            return -1;
        }
        if (n > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[Host %d] Broadcast: WC error: %d, opcode=%d, wr_id=%lu\n",
                        ctx->rank, wc.status, wc.opcode, wc.wr_id);
                return -1;
            }
            if (wc.opcode == IBV_WC_SEND && is_root) {
                send_done++;
                if (send_done % 1000 == 0 || send_done == msg_count) {
                    printf("[Host %d] Broadcast: send_done=%u/%u\n",
                           ctx->rank, send_done, msg_count);
                    fflush(stdout);
                }
                // 补充发送请求
                if (posted_send < msg_count) {
                    uint32_t i = posted_send;
                    uint32_t offset = i * PAYLOAD_SIZE;
                    uint32_t chunk_len = (offset + PAYLOAD_SIZE <= data_len) ?
                                         PAYLOAD_SIZE : (data_len - offset);
                    memcpy(ctx->send_buf, (char*)data + offset, chunk_len);
                    uint32_t imm = make_imm_data(i, PRIM_BROADCAST, OP_SUM,
                                                  DTYPE_INT32, ctx->rank, root);
                    post_send(ctx, ctx->send_buf, chunk_len, imm, i);
                    posted_send++;
                }
            } else if (wc.opcode == IBV_WC_RECV) {
                uint32_t imm = ntohl(wc.imm_data);
                uint32_t slot_id = (imm >> 12) & 0xFFFFF;
                uint32_t offset = slot_id * PAYLOAD_SIZE;
                uint32_t chunk_len = (offset + PAYLOAD_SIZE <= data_len) ?
                                     PAYLOAD_SIZE : (data_len - offset);

                uint32_t slot = slot_id % RECV_SLOTS;
                void *recv_ptr = (char*)ctx->recv_buf + slot * PAYLOAD_SIZE;
                memcpy((char*)data + offset, recv_ptr, chunk_len);
                recv_done++;

                if (recv_done % 1000 == 0 || recv_done == msg_count) {
                    printf("[Host %d] Broadcast: recv_done=%u/%u, slot_id=%u\n",
                           ctx->rank, recv_done, msg_count, slot_id);
                    fflush(stdout);
                }

                // 补充接收请求
                if (posted_recv < msg_count) {
                    uint32_t new_slot = posted_recv % RECV_SLOTS;
                    void *new_recv_ptr = (char*)ctx->recv_buf + new_slot * PAYLOAD_SIZE;
                    post_recv(ctx, new_recv_ptr, PAYLOAD_SIZE, posted_recv);
                    posted_recv++;
                }
            }
        }

        // 检查超时
        gettimeofday(&now, NULL);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000000 +
                       (now.tv_usec - start.tv_usec);
        if (elapsed > TIMEOUT_US) {
            fprintf(stderr, "[Host %d] Broadcast timeout: send=%u/%u recv=%u/%u\n",
                    ctx->rank, send_done, expected_send, recv_done, msg_count);
            return -1;
        }
    }

    printf("[Host %d] Broadcast complete\n", ctx->rank);
    fflush(stdout);
    return 0;
}
