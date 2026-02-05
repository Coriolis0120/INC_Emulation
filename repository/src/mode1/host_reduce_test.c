/**
 * @file host_reduce_test.c
 * @brief Mode1 Host 端 Reduce 测试程序
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include "mode1/host_api.h"

#define DEFAULT_COUNT 16  // 默认测试数据元素数量

static void print_usage(const char *prog) {
    printf("Usage: %s <switch_ip> <switch_port> <rank> <world_size> <root_rank> [count]\n", prog);
    printf("Example: %s 10.1.1.25 52410 0 4 0 1024\n", prog);
    printf("  root_rank: the rank that receives the reduce result\n");
    printf("  count: number of int32 elements (default: %d)\n", DEFAULT_COUNT);
}

int main(int argc, char *argv[]) {
    if (argc < 6) {
        print_usage(argv[0]);
        return 1;
    }

    const char *switch_ip = argv[1];
    int switch_port = atoi(argv[2]);
    int rank = atoi(argv[3]);
    int world_size = atoi(argv[4]);
    int root_rank = atoi(argv[5]);
    int count = (argc > 6) ? atoi(argv[6]) : DEFAULT_COUNT;

    printf("=== Mode1 Reduce Test ===\n");
    printf("Switch: %s:%d\n", switch_ip, switch_port);
    printf("Rank: %d / %d\n", rank, world_size);
    printf("Root rank: %d\n", root_rank);
    printf("Data count: %d (%d bytes)\n", count, count * 4);
    printf("=========================\n\n");
    fflush(stdout);

    // 初始化 Host 上下文
    printf("[Host %d] Initializing...\n", rank);
    fflush(stdout);
    mode1_host_ctx_t *ctx = mode1_host_init(switch_ip, switch_port,
                                             rank, world_size, 1);
    if (!ctx) {
        fprintf(stderr, "[Host %d] Failed to init host context\n", rank);
        return 1;
    }

    printf("[Host %d] Initialized successfully\n", rank);
    fflush(stdout);

    // 准备测试数据
    int32_t *src = malloc(count * sizeof(int32_t));
    int32_t *dst = malloc(count * sizeof(int32_t));
    if (!src || !dst) {
        fprintf(stderr, "[Host %d] Failed to allocate memory\n", rank);
        return 1;
    }

    // 每个 rank 的数据: src[i] = rank + 1
    for (int i = 0; i < count; i++) {
        src[i] = rank + 1;
        dst[i] = 0;  // 初始化 dst
    }

    printf("[Host %d] Test data: src[0]=%d, is_root=%d\n",
           rank, src[0], (rank == root_rank));
    fflush(stdout);

    // 等待所有 Host 连接完成
    printf("[Host %d] Waiting 2s for all hosts to connect...\n", rank);
    fflush(stdout);
    sleep(2);

    // 测试 Reduce
    printf("[Host %d] Starting Reduce test (root=%d)...\n", rank, root_rank);
    fflush(stdout);
    struct timeval start, end;
    gettimeofday(&start, NULL);

    int ret = mode1_reduce(ctx, src, count, dst, root_rank);

    gettimeofday(&end, NULL);
    long elapsed = (end.tv_sec - start.tv_sec) * 1000000 +
                   (end.tv_usec - start.tv_usec);

    printf("[Host %d] mode1_reduce returned %d\n", rank, ret);
    fflush(stdout);

    if (ret == 0) {
        if (rank == root_rank) {
            // 只有 root 节点验证结果
            // 期望值 = sum(1..world_size) = world_size*(world_size+1)/2
            int expected = world_size * (world_size + 1) / 2;
            int pass = 1;
            int fail_idx = -1;
            int fail_val = 0;

            for (int i = 0; i < count; i++) {
                if (dst[i] != expected) {
                    fail_idx = i;
                    fail_val = dst[i];
                    pass = 0;
                    break;
                }
            }

            if (pass) {
                printf("[Host %d] PASS: Reduce result correct, dst[0]=%d\n",
                       rank, dst[0]);
            } else {
                printf("[Host %d] FAIL: dst[%d]=%d, expected=%d\n",
                       rank, fail_idx, fail_val, expected);
            }
            printf("[Host %d] Reduce time: %ld us\n", rank, elapsed);
        } else {
            // 非 root 节点只需要发送完成
            printf("[Host %d] PASS: Non-root send completed\n", rank);
            printf("[Host %d] Reduce time: %ld us\n", rank, elapsed);
        }
    } else {
        printf("[Host %d] FAIL: Reduce returned %d\n", rank, ret);
    }
    fflush(stdout);

    free(src);
    free(dst);

    // 清理
    mode1_host_destroy(ctx);
    printf("[Host %d] Test complete\n", rank);
    fflush(stdout);

    return (ret == 0) ? 0 : 1;
}
