/**
 * @file qp_manager.c
 * @brief QP 连接管理 - 使用 libibverbs 管理 RDMA 连接
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "mode1/qp_manager.h"

// ============ 初始化与销毁 ============

switch_roce_ctx_t *qp_manager_init(const char *dev_name, int gid_idx) {
    switch_roce_ctx_t *ctx = calloc(1, sizeof(switch_roce_ctx_t));
    if (!ctx) {
        fprintf(stderr, "Failed to allocate switch_roce_ctx_t\n");
        return NULL;
    }

    ctx->gid_idx = gid_idx;
    ctx->ib_port = 1;
    pthread_mutex_init(&ctx->lock, NULL);

    // 获取设备列表
    int num_devices;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "No RDMA devices found\n");
        goto err_free_ctx;
    }

    // 查找指定设备或使用第一个
    struct ibv_device *ib_dev = NULL;
    if (dev_name) {
        for (int i = 0; i < num_devices; i++) {
            if (strcmp(ibv_get_device_name(dev_list[i]), dev_name) == 0) {
                ib_dev = dev_list[i];
                break;
            }
        }
    } else {
        ib_dev = dev_list[0];
    }

    if (!ib_dev) {
        fprintf(stderr, "RDMA device %s not found\n", dev_name ? dev_name : "(default)");
        goto err_free_list;
    }

    printf("[QP Manager] Using device: %s\n", ibv_get_device_name(ib_dev));

    // 打开设备
    ctx->ctx = ibv_open_device(ib_dev);
    if (!ctx->ctx) {
        fprintf(stderr, "Failed to open device\n");
        goto err_free_list;
    }

    ibv_free_device_list(dev_list);
    dev_list = NULL;

    // 创建 Protection Domain
    ctx->pd = ibv_alloc_pd(ctx->ctx);
    if (!ctx->pd) {
        fprintf(stderr, "Failed to allocate PD\n");
        goto err_close_dev;
    }

    // 创建完成队列 - 限制大小不超过设备限制 (32767)
    int cq_size = RECV_BUF_COUNT * 2;
    if (cq_size > 32767) cq_size = 32767;

    ctx->recv_cq = ibv_create_cq(ctx->ctx, cq_size, NULL, NULL, 0);
    if (!ctx->recv_cq) {
        fprintf(stderr, "Failed to create recv CQ\n");
        goto err_dealloc_pd;
    }

    ctx->send_cq = ibv_create_cq(ctx->ctx, cq_size, NULL, NULL, 0);
    if (!ctx->send_cq) {
        fprintf(stderr, "Failed to create send CQ\n");
        goto err_destroy_recv_cq;
    }

    // 分配接收缓冲区 - 使用 size_t 避免溢出
    ctx->recv_buf_size = (size_t)RECV_BUF_COUNT * RECV_BUF_SIZE;
    ctx->recv_buf = malloc(ctx->recv_buf_size);
    if (!ctx->recv_buf) {
        fprintf(stderr, "Failed to allocate recv buffer\n");
        goto err_destroy_send_cq;
    }
    memset(ctx->recv_buf, 0, ctx->recv_buf_size);

    // 注册内存区域
    ctx->mr = ibv_reg_mr(ctx->pd, ctx->recv_buf, ctx->recv_buf_size,
                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!ctx->mr) {
        fprintf(stderr, "Failed to register MR\n");
        goto err_free_buf;
    }

    // 分配发送缓冲区
    ctx->send_buf_size = RECV_BUF_SIZE;
    ctx->send_buf = malloc(ctx->send_buf_size);
    if (!ctx->send_buf) {
        fprintf(stderr, "Failed to allocate send buffer\n");
        goto err_dereg_mr;
    }

    // 注册发送缓冲区
    ctx->send_mr = ibv_reg_mr(ctx->pd, ctx->send_buf, ctx->send_buf_size,
                              IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->send_mr) {
        fprintf(stderr, "Failed to register send MR\n");
        goto err_free_send_buf;
    }

    // 获取 GID
    if (ibv_query_gid(ctx->ctx, ctx->ib_port, gid_idx, &ctx->local_gid) != 0) {
        fprintf(stderr, "Failed to query GID\n");
        goto err_dereg_send_mr;
    }

    printf("[QP Manager] Initialized successfully, GID index=%d\n", gid_idx);
    return ctx;

err_dereg_send_mr:
    ibv_dereg_mr(ctx->send_mr);
err_free_send_buf:
    free(ctx->send_buf);
err_dereg_mr:
    ibv_dereg_mr(ctx->mr);
err_free_buf:
    free(ctx->recv_buf);
err_destroy_send_cq:
    ibv_destroy_cq(ctx->send_cq);
err_destroy_recv_cq:
    ibv_destroy_cq(ctx->recv_cq);
err_dealloc_pd:
    ibv_dealloc_pd(ctx->pd);
err_close_dev:
    ibv_close_device(ctx->ctx);
err_free_list:
    if (dev_list) ibv_free_device_list(dev_list);
err_free_ctx:
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
    return NULL;
}

void qp_manager_destroy(switch_roce_ctx_t *ctx) {
    if (!ctx) return;

    // 销毁所有 Host 连接
    for (int i = 0; i < ctx->host_count; i++) {
        if (ctx->host_conns[i]) {
            qp_conn_destroy(ctx->host_conns[i]);
        }
    }

    // 销毁父连接
    if (ctx->parent_conn) {
        qp_conn_destroy(ctx->parent_conn);
    }

    // 销毁子连接
    for (int i = 0; i < ctx->child_count; i++) {
        if (ctx->child_conns[i]) {
            qp_conn_destroy(ctx->child_conns[i]);
        }
    }

    // 释放 RDMA 资源
    if (ctx->send_mr) ibv_dereg_mr(ctx->send_mr);
    if (ctx->send_buf) free(ctx->send_buf);
    if (ctx->mr) ibv_dereg_mr(ctx->mr);
    if (ctx->recv_buf) free(ctx->recv_buf);
    if (ctx->send_cq) ibv_destroy_cq(ctx->send_cq);
    if (ctx->recv_cq) ibv_destroy_cq(ctx->recv_cq);
    if (ctx->pd) ibv_dealloc_pd(ctx->pd);
    if (ctx->ctx) ibv_close_device(ctx->ctx);

    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
}

// ============ 连接管理 ============

qp_conn_t *qp_conn_create(switch_roce_ctx_t *ctx, int conn_id) {
    qp_conn_t *conn = calloc(1, sizeof(qp_conn_t));
    if (!conn) return NULL;

    conn->id = conn_id;
    conn->tcp_fd = -1;

    // 创建 QP - max_recv_wr 需要匹配 RECV_BUF_PER_CONN (16384)
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = ctx->send_cq,
        .recv_cq = ctx->recv_cq,
        .cap = {
            .max_send_wr = 16384,
            .max_recv_wr = 16384,
            .max_send_sge = 1,
            .max_recv_sge = 1
        },
        .qp_type = IBV_QPT_RC
    };

    conn->qp = ibv_create_qp(ctx->pd, &qp_attr);
    if (!conn->qp) {
        fprintf(stderr, "Failed to create QP for conn %d\n", conn_id);
        free(conn);
        return NULL;
    }

    // 保存本地 QP 信息
    conn->local_info.qpn = conn->qp->qp_num;
    conn->local_info.psn = rand() & 0xFFFFFF;
    memcpy(&conn->local_info.gid, &ctx->local_gid, sizeof(union ibv_gid));

    return conn;
}

void qp_conn_destroy(qp_conn_t *conn) {
    if (!conn) return;
    if (conn->qp) ibv_destroy_qp(conn->qp);
    if (conn->tcp_fd >= 0) close(conn->tcp_fd);
    free(conn);
}

// QP 状态转换到 INIT
static int qp_modify_to_init(switch_roce_ctx_t *ctx, qp_conn_t *conn) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = ctx->ib_port,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
    };

    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

    if (ibv_modify_qp(conn->qp, &attr, flags)) {
        fprintf(stderr, "Failed to modify QP to INIT\n");
        return -1;
    }
    return 0;
}

int qp_modify_to_rtr(switch_roce_ctx_t *ctx, qp_conn_t *conn) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024,
        .dest_qp_num = conn->remote_info.qpn,
        .rq_psn = conn->remote_info.psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {
            .is_global = 1,
            .dlid = conn->remote_info.lid,
            .sl = 0,
            .src_path_bits = 0,
            .port_num = ctx->ib_port,
            .grh = {
                .dgid = conn->remote_info.gid,
                .flow_label = 0,
                .hop_limit = 64,
                .sgid_index = ctx->gid_idx,
                .traffic_class = 0
            }
        }
    };

    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

    if (ibv_modify_qp(conn->qp, &attr, flags)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return -1;
    }
    return 0;
}

int qp_modify_to_rts(qp_conn_t *conn) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .timeout = 14,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .sq_psn = conn->local_info.psn,
        .max_rd_atomic = 1
    };

    int flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

    if (ibv_modify_qp(conn->qp, &attr, flags)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return -1;
    }

    conn->is_connected = 1;
    return 0;
}

// ============ TCP 交换 ============

int qp_conn_exchange(switch_roce_ctx_t *ctx, qp_conn_t *conn,
                     int tcp_fd, int is_server) {
    conn->tcp_fd = tcp_fd;

    // 先转换到 INIT 状态
    if (qp_modify_to_init(ctx, conn) != 0) {
        return -1;
    }

    // 交换 QP 信息
    if (is_server) {
        // 服务端：先发送后接收
        if (write(tcp_fd, &conn->local_info, sizeof(qp_info_t)) != sizeof(qp_info_t)) {
            fprintf(stderr, "Failed to send QP info\n");
            return -1;
        }
        if (read(tcp_fd, &conn->remote_info, sizeof(qp_info_t)) != sizeof(qp_info_t)) {
            fprintf(stderr, "Failed to recv QP info\n");
            return -1;
        }
    } else {
        // 客户端：先接收后发送
        if (read(tcp_fd, &conn->remote_info, sizeof(qp_info_t)) != sizeof(qp_info_t)) {
            fprintf(stderr, "Failed to recv QP info\n");
            return -1;
        }
        if (write(tcp_fd, &conn->local_info, sizeof(qp_info_t)) != sizeof(qp_info_t)) {
            fprintf(stderr, "Failed to send QP info\n");
            return -1;
        }
    }

    // 转换到 RTR
    if (qp_modify_to_rtr(ctx, conn) != 0) {
        return -1;
    }

    // 转换到 RTS
    if (qp_modify_to_rts(conn) != 0) {
        return -1;
    }

    printf("[QP] Conn %d established: local QPN=%u, remote QPN=%u\n",
           conn->id, conn->local_info.qpn, conn->remote_info.qpn);
    return 0;
}

// ============ 消息收发 ============

int qp_post_recv(switch_roce_ctx_t *ctx, qp_conn_t *conn,
                 uint64_t wr_id, void *buf, uint32_t len) {
    struct ibv_sge sge = {
        .addr = (uintptr_t)buf,
        .length = len,
        .lkey = ctx->mr->lkey
    };

    struct ibv_recv_wr wr = {
        .wr_id = wr_id,
        .sg_list = &sge,
        .num_sge = 1
    };

    struct ibv_recv_wr *bad_wr;
    if (ibv_post_recv(conn->qp, &wr, &bad_wr)) {
        fprintf(stderr, "Failed to post recv\n");
        return -1;
    }
    return 0;
}

int qp_post_send(switch_roce_ctx_t *ctx, qp_conn_t *conn,
                 uint64_t wr_id, void *buf, uint32_t len, uint32_t imm_data) {
    struct ibv_sge sge = {
        .addr = (uintptr_t)buf,
        .length = len,
        .lkey = ctx->send_mr->lkey
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
    if (ibv_post_send(conn->qp, &wr, &bad_wr)) {
        fprintf(stderr, "Failed to post send\n");
        return -1;
    }
    return 0;
}

// ============ CQ 轮询 ============

int qp_poll_recv_cq(switch_roce_ctx_t *ctx, struct ibv_wc *wc, int max_wc) {
    return ibv_poll_cq(ctx->recv_cq, max_wc, wc);
}

int qp_poll_send_cq(switch_roce_ctx_t *ctx, struct ibv_wc *wc, int max_wc) {
    return ibv_poll_cq(ctx->send_cq, max_wc, wc);
}

// ============ 辅助函数 ============

void *qp_get_recv_buf(switch_roce_ctx_t *ctx, uint64_t wr_id) {
    // wr_id = conn_id * RECV_BUF_PER_CONN + slot_idx
    // 直接使用 wr_id 作为槽位索引，确保不同连接使用不同的缓冲区
    uint64_t slot = wr_id % RECV_BUF_COUNT;
    return (char *)ctx->recv_buf + slot * RECV_BUF_SIZE;
}

void qp_conn_print(qp_conn_t *conn) {
    printf("QP Connection %d:\n", conn->id);
    printf("  Local:  QPN=%u, PSN=%u\n",
           conn->local_info.qpn, conn->local_info.psn);
    printf("  Remote: QPN=%u, PSN=%u\n",
           conn->remote_info.qpn, conn->remote_info.psn);
    printf("  Connected: %s\n", conn->is_connected ? "yes" : "no");
}

// ============ 上行连接 (Leaf -> Spine) ============

// 增加上行缓冲区以支持更大数据量 (8192 slots)
#define UPLINK_BUF_SLOTS 8192
#define UPLINK_BUF_SIZE ((size_t)UPLINK_BUF_SLOTS * 64 * 1024)  // 8192 * 64KB = 512MB

uplink_ctx_t *uplink_ctx_init(const char *dev_name, int gid_idx) {
    uplink_ctx_t *uplink = calloc(1, sizeof(uplink_ctx_t));
    if (!uplink) return NULL;

    uplink->gid_idx = gid_idx;
    uplink->ib_port = 1;
    uplink->buf_size = UPLINK_BUF_SIZE;

    // 获取设备
    int num_devices;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) goto err_free;

    struct ibv_device *ib_dev = NULL;
    for (int i = 0; i < num_devices; i++) {
        if (strcmp(ibv_get_device_name(dev_list[i]), dev_name) == 0) {
            ib_dev = dev_list[i];
            break;
        }
    }
    if (!ib_dev) {
        fprintf(stderr, "[Uplink] Device %s not found\n", dev_name);
        ibv_free_device_list(dev_list);
        goto err_free;
    }

    printf("[Uplink] Using device: %s\n", dev_name);
    uplink->ctx = ibv_open_device(ib_dev);
    ibv_free_device_list(dev_list);
    if (!uplink->ctx) goto err_free;

    uplink->pd = ibv_alloc_pd(uplink->ctx);
    if (!uplink->pd) goto err_close;

    uplink->recv_cq = ibv_create_cq(uplink->ctx, 8192, NULL, NULL, 0);
    uplink->send_cq = ibv_create_cq(uplink->ctx, 8192, NULL, NULL, 0);
    if (!uplink->recv_cq || !uplink->send_cq) goto err_pd;

    uplink->recv_buf = malloc(uplink->buf_size);
    uplink->send_buf = malloc(uplink->buf_size);
    if (!uplink->recv_buf || !uplink->send_buf) goto err_cq;

    uplink->mr = ibv_reg_mr(uplink->pd, uplink->recv_buf, uplink->buf_size,
                            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    uplink->send_mr = ibv_reg_mr(uplink->pd, uplink->send_buf, uplink->buf_size,
                                  IBV_ACCESS_LOCAL_WRITE);
    if (!uplink->mr || !uplink->send_mr) goto err_buf;

    ibv_query_gid(uplink->ctx, uplink->ib_port, gid_idx, &uplink->local_gid);
    return uplink;

err_buf:
    if (uplink->mr) ibv_dereg_mr(uplink->mr);
    if (uplink->send_mr) ibv_dereg_mr(uplink->send_mr);
    free(uplink->recv_buf);
    free(uplink->send_buf);
err_cq:
    if (uplink->recv_cq) ibv_destroy_cq(uplink->recv_cq);
    if (uplink->send_cq) ibv_destroy_cq(uplink->send_cq);
err_pd:
    ibv_dealloc_pd(uplink->pd);
err_close:
    ibv_close_device(uplink->ctx);
err_free:
    free(uplink);
    return NULL;
}

void uplink_ctx_destroy(uplink_ctx_t *uplink) {
    if (!uplink) return;
    if (uplink->conn) {
        if (uplink->conn->qp) ibv_destroy_qp(uplink->conn->qp);
        if (uplink->conn->tcp_fd >= 0) close(uplink->conn->tcp_fd);
        free(uplink->conn);
    }
    if (uplink->send_mr) ibv_dereg_mr(uplink->send_mr);
    if (uplink->mr) ibv_dereg_mr(uplink->mr);
    free(uplink->send_buf);
    free(uplink->recv_buf);
    if (uplink->send_cq) ibv_destroy_cq(uplink->send_cq);
    if (uplink->recv_cq) ibv_destroy_cq(uplink->recv_cq);
    if (uplink->pd) ibv_dealloc_pd(uplink->pd);
    if (uplink->ctx) ibv_close_device(uplink->ctx);
    free(uplink);
}

qp_conn_t *uplink_conn_create(uplink_ctx_t *uplink) {
    qp_conn_t *conn = calloc(1, sizeof(qp_conn_t));
    if (!conn) return NULL;
    conn->id = -1;
    conn->tcp_fd = -1;

    struct ibv_qp_init_attr attr = {
        .send_cq = uplink->send_cq,
        .recv_cq = uplink->recv_cq,
        .cap = { .max_send_wr = 16384, .max_recv_wr = 16384,
                 .max_send_sge = 1, .max_recv_sge = 1 },
        .qp_type = IBV_QPT_RC
    };
    conn->qp = ibv_create_qp(uplink->pd, &attr);
    if (!conn->qp) { free(conn); return NULL; }

    conn->local_info.qpn = conn->qp->qp_num;
    conn->local_info.psn = rand() & 0xFFFFFF;
    memcpy(&conn->local_info.gid, &uplink->local_gid, sizeof(union ibv_gid));
    uplink->conn = conn;
    return conn;
}

int uplink_conn_exchange(uplink_ctx_t *uplink, int tcp_fd) {
    qp_conn_t *conn = uplink->conn;
    if (!conn) return -1;
    conn->tcp_fd = tcp_fd;

    // INIT
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = uplink->ib_port,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
    };
    if (ibv_modify_qp(conn->qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        return -1;
    }

    // 客户端：先接收后发送
    printf("[Uplink] Waiting to receive QP info from server...\n");
    fflush(stdout);
    ssize_t rd = read(tcp_fd, &conn->remote_info, sizeof(qp_info_t));
    printf("[Uplink] read() returned %zd (expected %zu)\n", rd, sizeof(qp_info_t));
    fflush(stdout);
    if (rd != sizeof(qp_info_t))
        return -1;
    printf("[Uplink] Received QP info: QPN=%u, PSN=%u, GID[0-3]=%02x%02x%02x%02x\n",
           conn->remote_info.qpn, conn->remote_info.psn,
           conn->remote_info.gid.raw[0], conn->remote_info.gid.raw[1],
           conn->remote_info.gid.raw[2], conn->remote_info.gid.raw[3]);
    fflush(stdout);
    printf("[Uplink] Sending QP info: QPN=%u, PSN=%u\n", conn->local_info.qpn, conn->local_info.psn);
    fflush(stdout);
    ssize_t written = write(tcp_fd, &conn->local_info, sizeof(qp_info_t));
    printf("[Uplink] write() returned %zd (expected %zu)\n", written, sizeof(qp_info_t));
    fflush(stdout);
    if (written != sizeof(qp_info_t))
        return -1;

    // RTR
    struct ibv_qp_attr rtr_attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024,
        .dest_qp_num = conn->remote_info.qpn,
        .rq_psn = conn->remote_info.psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {
            .is_global = 1,
            .dlid = conn->remote_info.lid,
            .port_num = uplink->ib_port,
            .grh = {
                .dgid = conn->remote_info.gid,
                .hop_limit = 64,
                .sgid_index = uplink->gid_idx
            }
        }
    };
    if (ibv_modify_qp(conn->qp, &rtr_attr,
        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
        IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        return -1;
    }

    // RTS
    struct ibv_qp_attr rts_attr = {
        .qp_state = IBV_QPS_RTS,
        .timeout = 14,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .sq_psn = conn->local_info.psn,
        .max_rd_atomic = 1
    };
    if (ibv_modify_qp(conn->qp, &rts_attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC)) {
        return -1;
    }

    conn->is_connected = 1;
    printf("[Uplink] Connected: local QPN=%u, remote QPN=%u\n",
           conn->local_info.qpn, conn->remote_info.qpn);
    return 0;
}

int uplink_post_send(uplink_ctx_t *uplink, uint64_t wr_id,
                     void *buf, uint32_t len, uint32_t imm_data) {
    struct ibv_sge sge = {
        .addr = (uintptr_t)buf,
        .length = len,
        .lkey = uplink->send_mr->lkey
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
    return ibv_post_send(uplink->conn->qp, &wr, &bad_wr);
}

int uplink_post_recv(uplink_ctx_t *uplink, uint64_t wr_id,
                     void *buf, uint32_t len) {
    struct ibv_sge sge = {
        .addr = (uintptr_t)buf,
        .length = len,
        .lkey = uplink->mr->lkey
    };
    struct ibv_recv_wr wr = {
        .wr_id = wr_id,
        .sg_list = &sge,
        .num_sge = 1
    };
    struct ibv_recv_wr *bad_wr;
    return ibv_post_recv(uplink->conn->qp, &wr, &bad_wr);
}

int uplink_poll_recv_cq(uplink_ctx_t *uplink, struct ibv_wc *wc, int max_wc) {
    return ibv_poll_cq(uplink->recv_cq, max_wc, wc);
}

// ============ 下行连接 (Spine -> Leaf) ============

// 支持最多 MAX_SWITCHES 个连接，每个连接 RECV_BUF_PER_CONN 个槽位
// RECV_BUF_PER_CONN=8192, MAX_SWITCHES=8 -> 65536 slots
// 每个 slot 64KB -> 总共 4GB (需要 size_t 类型)
#define DOWNLINK_BUF_SLOTS (RECV_BUF_PER_CONN * MAX_SWITCHES)
#define DOWNLINK_BUF_SIZE ((size_t)DOWNLINK_BUF_SLOTS * 65536ULL)

downlink_ctx_t *downlink_ctx_init(const char *dev_name, int gid_idx) {
    downlink_ctx_t *downlink = calloc(1, sizeof(downlink_ctx_t));
    if (!downlink) return NULL;

    downlink->gid_idx = gid_idx;
    downlink->ib_port = 1;
    downlink->buf_size = DOWNLINK_BUF_SIZE;

    // 获取设备
    int num_devices;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) goto err_free;

    struct ibv_device *ib_dev = NULL;
    for (int i = 0; i < num_devices; i++) {
        if (strcmp(ibv_get_device_name(dev_list[i]), dev_name) == 0) {
            ib_dev = dev_list[i];
            break;
        }
    }
    if (!ib_dev) {
        fprintf(stderr, "[Downlink] Device %s not found\n", dev_name);
        ibv_free_device_list(dev_list);
        goto err_free;
    }

    printf("[Downlink] Using device: %s\n", dev_name);
    downlink->ctx = ibv_open_device(ib_dev);
    ibv_free_device_list(dev_list);
    if (!downlink->ctx) goto err_free;

    downlink->pd = ibv_alloc_pd(downlink->ctx);
    if (!downlink->pd) goto err_close;

    downlink->recv_cq = ibv_create_cq(downlink->ctx, 8192, NULL, NULL, 0);
    downlink->send_cq = ibv_create_cq(downlink->ctx, 8192, NULL, NULL, 0);
    if (!downlink->recv_cq || !downlink->send_cq) goto err_pd;

    downlink->recv_buf = malloc(downlink->buf_size);
    downlink->send_buf = malloc(downlink->buf_size);
    if (!downlink->recv_buf || !downlink->send_buf) goto err_cq;

    downlink->mr = ibv_reg_mr(downlink->pd, downlink->recv_buf, downlink->buf_size,
                              IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    downlink->send_mr = ibv_reg_mr(downlink->pd, downlink->send_buf, downlink->buf_size,
                                    IBV_ACCESS_LOCAL_WRITE);
    if (!downlink->mr || !downlink->send_mr) goto err_buf;

    ibv_query_gid(downlink->ctx, downlink->ib_port, gid_idx, &downlink->local_gid);
    return downlink;

err_buf:
    if (downlink->mr) ibv_dereg_mr(downlink->mr);
    if (downlink->send_mr) ibv_dereg_mr(downlink->send_mr);
    free(downlink->recv_buf);
    free(downlink->send_buf);
err_cq:
    if (downlink->recv_cq) ibv_destroy_cq(downlink->recv_cq);
    if (downlink->send_cq) ibv_destroy_cq(downlink->send_cq);
err_pd:
    ibv_dealloc_pd(downlink->pd);
err_close:
    ibv_close_device(downlink->ctx);
err_free:
    free(downlink);
    return NULL;
}

void downlink_ctx_destroy(downlink_ctx_t *downlink) {
    if (!downlink) return;
    for (int i = 0; i < downlink->conn_count; i++) {
        if (downlink->conns[i]) {
            if (downlink->conns[i]->qp) ibv_destroy_qp(downlink->conns[i]->qp);
            if (downlink->conns[i]->tcp_fd >= 0) close(downlink->conns[i]->tcp_fd);
            free(downlink->conns[i]);
        }
    }
    if (downlink->send_mr) ibv_dereg_mr(downlink->send_mr);
    if (downlink->mr) ibv_dereg_mr(downlink->mr);
    free(downlink->send_buf);
    free(downlink->recv_buf);
    if (downlink->send_cq) ibv_destroy_cq(downlink->send_cq);
    if (downlink->recv_cq) ibv_destroy_cq(downlink->recv_cq);
    if (downlink->pd) ibv_dealloc_pd(downlink->pd);
    if (downlink->ctx) ibv_close_device(downlink->ctx);
    free(downlink);
}

qp_conn_t *downlink_conn_create(downlink_ctx_t *downlink, int conn_id) {
    qp_conn_t *conn = calloc(1, sizeof(qp_conn_t));
    if (!conn) return NULL;
    conn->id = conn_id;
    conn->tcp_fd = -1;

    struct ibv_qp_init_attr attr = {
        .send_cq = downlink->send_cq,
        .recv_cq = downlink->recv_cq,
        .cap = { .max_send_wr = 16384, .max_recv_wr = 16384,
                 .max_send_sge = 1, .max_recv_sge = 1 },
        .qp_type = IBV_QPT_RC
    };
    conn->qp = ibv_create_qp(downlink->pd, &attr);
    if (!conn->qp) { free(conn); return NULL; }

    conn->local_info.qpn = conn->qp->qp_num;
    conn->local_info.psn = rand() & 0xFFFFFF;
    memcpy(&conn->local_info.gid, &downlink->local_gid, sizeof(union ibv_gid));
    return conn;
}

int downlink_conn_exchange(downlink_ctx_t *downlink, qp_conn_t *conn, int tcp_fd) {
    if (!conn) return -1;
    conn->tcp_fd = tcp_fd;

    // INIT
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = downlink->ib_port,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
    };
    printf("[Downlink] Modifying QP to INIT, port=%d\n", downlink->ib_port);
    if (ibv_modify_qp(conn->qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        perror("[Downlink] Failed to modify QP to INIT");
        return -1;
    }

    // 服务端：先发送后接收
    printf("[Downlink] Sending QP info: QPN=%u, PSN=%u, GID[0-3]=%02x%02x%02x%02x\n",
           conn->local_info.qpn, conn->local_info.psn,
           conn->local_info.gid.raw[0], conn->local_info.gid.raw[1],
           conn->local_info.gid.raw[2], conn->local_info.gid.raw[3]);
    fflush(stdout);
    ssize_t written = write(tcp_fd, &conn->local_info, sizeof(qp_info_t));
    printf("[Downlink] write() returned %zd (expected %zu)\n", written, sizeof(qp_info_t));
    fflush(stdout);
    if (written != sizeof(qp_info_t))
        return -1;
    ssize_t rd = read(tcp_fd, &conn->remote_info, sizeof(qp_info_t));
    printf("[Downlink] read() returned %zd (expected %zu)\n", rd, sizeof(qp_info_t));
    fflush(stdout);
    if (rd != sizeof(qp_info_t))
        return -1;
    printf("[Downlink] Received QP info: QPN=%u, PSN=%u, GID[0-3]=%02x%02x%02x%02x\n",
           conn->remote_info.qpn, conn->remote_info.psn,
           conn->remote_info.gid.raw[0], conn->remote_info.gid.raw[1],
           conn->remote_info.gid.raw[2], conn->remote_info.gid.raw[3]);
    fflush(stdout);

    // RTR
    struct ibv_qp_attr rtr_attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024,
        .dest_qp_num = conn->remote_info.qpn,
        .rq_psn = conn->remote_info.psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {
            .is_global = 1,
            .dlid = conn->remote_info.lid,
            .port_num = downlink->ib_port,
            .grh = {
                .dgid = conn->remote_info.gid,
                .hop_limit = 64,
                .sgid_index = downlink->gid_idx
            }
        }
    };
    printf("[Downlink] Modifying QP to RTR, dest_qpn=%u, rq_psn=%u\n",
           conn->remote_info.qpn, conn->remote_info.psn);
    fflush(stdout);
    if (ibv_modify_qp(conn->qp, &rtr_attr,
        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
        IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        perror("[Downlink] Failed to modify QP to RTR");
        return -1;
    }
    printf("[Downlink] QP in RTR state\n");
    fflush(stdout);

    // RTS
    struct ibv_qp_attr rts_attr = {
        .qp_state = IBV_QPS_RTS,
        .timeout = 14,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .sq_psn = conn->local_info.psn,
        .max_rd_atomic = 1
    };
    printf("[Downlink] Modifying QP to RTS, sq_psn=%u\n", conn->local_info.psn);
    fflush(stdout);
    if (ibv_modify_qp(conn->qp, &rts_attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC)) {
        perror("[Downlink] Failed to modify QP to RTS");
        return -1;
    }
    printf("[Downlink] QP in RTS state\n");
    fflush(stdout);

    conn->is_connected = 1;
    printf("[Downlink] Connected: local QPN=%u, remote QPN=%u\n",
           conn->local_info.qpn, conn->remote_info.qpn);
    fflush(stdout);
    return 0;
}

int downlink_post_send(downlink_ctx_t *downlink, qp_conn_t *conn,
                       uint64_t wr_id, void *buf, uint32_t len, uint32_t imm_data) {
    struct ibv_sge sge = {
        .addr = (uintptr_t)buf,
        .length = len,
        .lkey = downlink->send_mr->lkey
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
    return ibv_post_send(conn->qp, &wr, &bad_wr);
}

int downlink_post_recv(downlink_ctx_t *downlink, qp_conn_t *conn,
                       uint64_t wr_id, void *buf, uint32_t len) {
    struct ibv_sge sge = {
        .addr = (uintptr_t)buf,
        .length = len,
        .lkey = downlink->mr->lkey
    };
    struct ibv_recv_wr wr = {
        .wr_id = wr_id,
        .sg_list = &sge,
        .num_sge = 1
    };
    struct ibv_recv_wr *bad_wr;
    return ibv_post_recv(conn->qp, &wr, &bad_wr);
}

int downlink_poll_recv_cq(downlink_ctx_t *downlink, struct ibv_wc *wc, int max_wc) {
    return ibv_poll_cq(downlink->recv_cq, max_wc, wc);
}
