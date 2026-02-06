/**
 * @file host_broadcast_test.c
 * @brief Mode1 Broadcast 测试程序
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include "mode1/host_api.h"

#define DEFAULT_COUNT 16

static void print_usage(const char *prog) {
    printf("Usage: %s <switch_ip> <switch_port> <rank> <world_size> <root> [count]\n", prog);
    printf("Example: %s 192.168.0.19 52400 0 2 0 1024\n", prog);
    printf("  root: broadcast source rank\n");
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
    int root = atoi(argv[5]);
    int count = (argc > 6) ? atoi(argv[6]) : DEFAULT_COUNT;

    printf("=== Mode1 Broadcast Test ===\n");
    printf("Switch: %s:%d\n", switch_ip, switch_port);
    printf("Rank: %d / %d, Root: %d\n", rank, world_size, root);
    printf("Data count: %d (%d bytes)\n", count, count * 4);
    printf("============================\n\n");

    // 初始化 Host 上下文
    mode1_host_ctx_t *ctx = mode1_host_init(switch_ip, switch_port,
                                             rank, world_size, 1);
    if (!ctx) {
        fprintf(stderr, "Failed to init host context\n");
        return 1;
    }

    printf("[Host %d] Initialized successfully\n", rank);

    // 准备测试数据
    int32_t *data = malloc(count * sizeof(int32_t));
    if (!data) {
        fprintf(stderr, "Failed to allocate memory\n");
        mode1_host_destroy(ctx);
        return 1;
    }

    // root 节点初始化数据，其他节点清零
    if (rank == root) {
        for (int i = 0; i < count; i++) {
            data[i] = 42 + i;  // 测试数据
        }
        printf("[Host %d] Root data: data[0]=%d, data[1]=%d\n",
               rank, data[0], count > 1 ? data[1] : 0);
    } else {
        memset(data, 0, count * sizeof(int32_t));
        printf("[Host %d] Non-root, data cleared\n", rank);
    }

    // 等待所有 Host 连接完成
    printf("[Host %d] Waiting 2s for all hosts to connect...\n", rank);
    sleep(2);

    // 测试 Broadcast
    printf("[Host %d] Starting Broadcast test (root=%d)...\n", rank, root);
    struct timeval start, end;
    gettimeofday(&start, NULL);

    int ret = mode1_broadcast(ctx, data, count, root);

    gettimeofday(&end, NULL);
    long elapsed = (end.tv_sec - start.tv_sec) * 1000000 +
                   (end.tv_usec - start.tv_usec);

    if (ret == 0) {
        // 验证结果: 所有节点应该收到 root 的数据
        int pass = 1;
        for (int i = 0; i < count; i++) {
            int expected = 42 + i;
            if (data[i] != expected) {
                printf("[Host %d] FAIL: data[%d]=%d, expected=%d\n",
                       rank, i, data[i], expected);
                pass = 0;
                break;
            }
        }
        if (pass) {
            printf("[Host %d] PASS: Broadcast result correct, data[0]=%d\n",
                   rank, data[0]);
        }
        printf("[Host %d] Broadcast time: %ld us\n", rank, elapsed);
    } else {
        printf("[Host %d] FAIL: Broadcast returned %d\n", rank, ret);
    }

    free(data);
    mode1_host_destroy(ctx);
    printf("[Host %d] Test complete\n", rank);

    return 0;
}
