/**
 * @file host_broadcast_test_v2.c
 * @brief Broadcast 测试程序 - 简化版（无控制消息）
 *
 * 直接发送数据，不需要控制消息握手
 */

#include "api.h"
#include "util.h"
#include <assert.h>
#include <sys/time.h>

// 超时设置
#define BROADCAST_TIMEOUT_US (120 * 1000000UL)  // 120 秒

// 动态分配的数据缓冲区
static int32_t *data = NULL;
static int data_count = 0;

// 记录开始时间
static struct timeval start_time;

static void print_cost_time(const char *prefix, int rank) {
    struct timeval end;
    gettimeofday(&end, NULL);
    double elapsed_ms = (end.tv_sec - start_time.tv_sec) * 1000.0 +
                        (end.tv_usec - start_time.tv_usec) / 1000.0;

    double data_bytes = data_count * 4.0;
    double elapsed_sec = elapsed_ms / 1000.0;
    double throughput_mbps = (data_bytes * 8.0) / (elapsed_sec * 1e6);

    printf("Broadcast completed, Time: %.0f ms, Throughput: %.2f Mbps\n",
           elapsed_ms, throughput_mbps);
    fflush(stdout);
}

// Root 节点初始化数据
static void init_broadcast_data(int rank, int root_rank) {
    if (rank == root_rank) {
        for (int i = 0; i < data_count; i++) {
            data[i] = i * 10 + root_rank;
        }
    } else {
        memset(data, 0, data_count * sizeof(int32_t));
    }
}

// 验证结果
static int verify_broadcast_data(int rank, int root_rank) {
    int errors = 0;
    for (int i = 0; i < data_count; i++) {
        int expected = i * 10 + root_rank;
        if (data[i] != expected) {
            if (errors < 5) {
                printf("  ERROR idx %d: got %d, expected %d\n", i, data[i], expected);
            }
            errors++;
        }
    }
    return errors;
}

/**
 * 简化版 Broadcast - Root 发送
 */
static void broadcast_root_send(struct inccl_communicator *comm, int message_num, int window_size) {
    struct ibv_send_wr wr;
    struct ibv_sge send_sge;
    struct ibv_send_wr *send_bad_wr;

    int send_num = 0;
    int send_complete = 0;

    // QP max_send_wr 限制为 16384
    int max_initial_send = 16384;
    int initial_send = window_size < max_initial_send ? window_size : max_initial_send;
    if (initial_send > message_num) initial_send = message_num;

    // 准备初始窗口数据
    for (int i = 0; i < initial_send; i++) {
        int32_t *msg_buf = (int32_t*)(comm->send_payload + i * PAYLOAD_COUNT * sizeof(int32_t));
        for (int j = 0; j < PAYLOAD_COUNT; j++) {
            int src_idx = i * PAYLOAD_COUNT + j;
            if (src_idx < data_count) {
                msg_buf[j] = htonl(data[src_idx]);
            } else {
                msg_buf[j] = 0;
            }
        }
    }

    // 发送初始窗口
    for (int i = 0; i < initial_send; i++) {
        memset(&send_sge, 0, sizeof(send_sge));
        send_sge.addr = (uintptr_t)(comm->send_payload + i * PAYLOAD_COUNT * sizeof(int32_t));
        send_sge.length = PAYLOAD_COUNT * sizeof(int32_t);
        send_sge.lkey = comm->mr_send_payload->lkey;

        memset(&wr, 0, sizeof(wr));
        wr.wr_id = i;
        wr.sg_list = &send_sge;
        wr.num_sge = 1;
        wr.send_flags = IBV_SEND_SIGNALED;
        wr.opcode = IBV_WR_SEND;

        ibv_post_send(comm->qp, &wr, &send_bad_wr);
        send_num++;
    }

    printf("Broadcast_Simple: Root posted %d initial sends\n", send_num);
    fflush(stdout);

    // 等待发送完成，滑动窗口发送剩余数据
    struct ibv_wc wc[64];
    struct timeval now;

    while (send_complete < message_num) {
        gettimeofday(&now, NULL);
        uint64_t elapsed = (now.tv_sec - start_time.tv_sec) * 1000000UL +
                          (now.tv_usec - start_time.tv_usec);
        if (elapsed > BROADCAST_TIMEOUT_US) {
            printf("Broadcast_Simple: ROOT TIMEOUT! completed=%d/%d\n", send_complete, message_num);
            fflush(stdout);
            return;
        }

        int n = ibv_poll_cq(comm->cq, 64, wc);
        for (int i = 0; i < n; i++) {
            if (wc[i].status == IBV_WC_SUCCESS && wc[i].opcode == IBV_WC_SEND) {
                send_complete++;

                // 滑动窗口：发送下一个消息
                if (send_num < message_num) {
                    int slot = send_num % window_size;
                    int32_t *msg_buf = (int32_t*)(comm->send_payload + slot * PAYLOAD_COUNT * sizeof(int32_t));
                    for (int j = 0; j < PAYLOAD_COUNT; j++) {
                        int src_idx = send_num * PAYLOAD_COUNT + j;
                        if (src_idx < data_count) {
                            msg_buf[j] = htonl(data[src_idx]);
                        } else {
                            msg_buf[j] = 0;
                        }
                    }

                    memset(&send_sge, 0, sizeof(send_sge));
                    send_sge.addr = (uintptr_t)msg_buf;
                    send_sge.length = PAYLOAD_COUNT * sizeof(int32_t);
                    send_sge.lkey = comm->mr_send_payload->lkey;

                    memset(&wr, 0, sizeof(wr));
                    wr.wr_id = send_num;
                    wr.sg_list = &send_sge;
                    wr.num_sge = 1;
                    wr.send_flags = IBV_SEND_SIGNALED;
                    wr.opcode = IBV_WR_SEND;

                    ibv_post_send(comm->qp, &wr, &send_bad_wr);
                    send_num++;
                }
            }
        }
    }

    printf("Broadcast_Simple: Root done, sent %d messages\n", send_complete);
    fflush(stdout);
}

/**
 * 简化版 Broadcast - 非 Root 接收
 */
static void broadcast_recv(struct inccl_communicator *comm, int message_num, int window_size) {
    struct ibv_recv_wr rr;
    struct ibv_sge recv_sge;
    struct ibv_recv_wr *recv_bad_wr;

    int receive_num = 0;
    int posted_recv = 0;

    // QP max_recv_wr 限制为 16384，所以初始 post 不能超过这个值
    int max_initial_post = 16384;
    int initial_post = window_size < max_initial_post ? window_size : max_initial_post;
    if (initial_post > message_num) initial_post = message_num;

    // 预先提交接收请求
    for (int i = 0; i < initial_post; i++) {
        memset(&recv_sge, 0, sizeof(recv_sge));
        recv_sge.addr = (uintptr_t)(comm->receive_payload + (i % window_size) * PAYLOAD_COUNT * sizeof(int32_t));
        recv_sge.length = PAYLOAD_COUNT * sizeof(int32_t);
        recv_sge.lkey = comm->mr_receive_payload->lkey;

        memset(&rr, 0, sizeof(rr));
        rr.wr_id = i;
        rr.sg_list = &recv_sge;
        rr.num_sge = 1;

        ibv_post_recv(comm->qp, &rr, &recv_bad_wr);
        posted_recv++;
    }

    printf("Broadcast_Simple: Non-root posted %d initial recvs\n", posted_recv);
    fflush(stdout);

    // 接收数据
    struct ibv_wc wc[64];
    struct timeval now;

    while (receive_num < message_num) {
        gettimeofday(&now, NULL);
        uint64_t elapsed = (now.tv_sec - start_time.tv_sec) * 1000000UL +
                          (now.tv_usec - start_time.tv_usec);
        if (elapsed > BROADCAST_TIMEOUT_US) {
            printf("Broadcast_Simple: NON-ROOT TIMEOUT! recv=%d/%d\n", receive_num, message_num);
            fflush(stdout);
            return;
        }

        int n = ibv_poll_cq(comm->cq, 64, wc);
        for (int i = 0; i < n; i++) {
            if (wc[i].status == IBV_WC_SUCCESS &&
                (wc[i].opcode == IBV_WC_RECV || wc[i].opcode == IBV_WC_RECV_RDMA_WITH_IMM)) {

                uint64_t msg_idx = wc[i].wr_id;
                int slot = msg_idx % window_size;
                int32_t *pack = (int32_t*)(comm->receive_payload + slot * PAYLOAD_COUNT * sizeof(int32_t));

                // 复制数据
                for (int j = 0; j < PAYLOAD_COUNT; j++) {
                    int dst_idx = msg_idx * PAYLOAD_COUNT + j;
                    if (dst_idx < data_count) {
                        data[dst_idx] = ntohl(pack[j]);
                    }
                }
                receive_num++;

                // 滑动窗口：补充新的接收请求
                if (posted_recv < message_num) {
                    int new_slot = posted_recv % window_size;
                    memset(&recv_sge, 0, sizeof(recv_sge));
                    recv_sge.addr = (uintptr_t)(comm->receive_payload + new_slot * PAYLOAD_COUNT * sizeof(int32_t));
                    recv_sge.length = PAYLOAD_COUNT * sizeof(int32_t);
                    recv_sge.lkey = comm->mr_receive_payload->lkey;

                    memset(&rr, 0, sizeof(rr));
                    rr.wr_id = posted_recv;
                    rr.sg_list = &recv_sge;
                    rr.num_sge = 1;

                    ibv_post_recv(comm->qp, &rr, &recv_bad_wr);
                    posted_recv++;
                }
            }
        }
    }

    printf("Broadcast_Simple: Non-root done, recv=%d\n", receive_num);
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        printf("Usage: %s <world_size> <master_addr> <rank> <root_rank> <data_count>\n", argv[0]);
        return -1;
    }

    int world_size = atoi(argv[1]);
    char *master_addr = argv[2];
    int rank = atoi(argv[3]);
    int root_rank = atoi(argv[4]);
    data_count = atoi(argv[5]);

    printf("=== Broadcast Test ===\n");
    fflush(stdout);

    // 分配内存
    data = (int32_t *)malloc(data_count * sizeof(int32_t));
    if (!data) {
        printf("ERROR: malloc failed\n");
        return -1;
    }

    init_broadcast_data(rank, root_rank);

    // 创建通信组和通信器
    struct inccl_group *group = inccl_group_create(world_size, rank, master_addr);
    struct inccl_communicator *comm = inccl_communicator_create(group, data_count * 4);

    // 计算消息数量和窗口大小
    int message_num = (data_count + PAYLOAD_COUNT - 1) / PAYLOAD_COUNT;
    int window_size = SLIDING_WINDOW_SIZE < message_num ? SLIDING_WINDOW_SIZE : message_num;

    printf("Broadcast_Simple: rank=%d, root=%d, is_root=%d, len=%d, segment_num=%d\n",
           rank, root_rank, (rank == root_rank), data_count, message_num);
    printf("Broadcast_Simple: message_num=%d, window_size=%d\n", message_num, window_size);
    fflush(stdout);

    gettimeofday(&start_time, NULL);

    if (rank == root_rank) {
        broadcast_root_send(comm, message_num, window_size);
    } else {
        broadcast_recv(comm, message_num, window_size);
    }

    print_cost_time("Broadcast", rank);

    // 验证结果
    int errors = verify_broadcast_data(rank, root_rank);
    if (errors == 0) {
        printf("  PASS: Broadcast verified\n");
    } else {
        printf("  FAIL: Broadcast verification failed\n");
    }

    printf("\n=== Test Complete ===\n");
    printf("destory...\n");
    fflush(stdout);

    free(data);
    return 0;
}
