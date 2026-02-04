/**
 * @file host_test.c
 * @brief Mode1 Host 端测试程序
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
    printf("Usage: %s <switch_ip> <switch_port> <rank> <world_size> [count]\n", prog);
    printf("Example: %s 192.168.0.19 52400 0 2 1024\n", prog);
    printf("  count: number of int32 elements (default: %d)\n", DEFAULT_COUNT);
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        print_usage(argv[0]);
        return 1;
    }

    const char *switch_ip = argv[1];
    int switch_port = atoi(argv[2]);
    int rank = atoi(argv[3]);
    int world_size = atoi(argv[4]);
    int count = (argc > 5) ? atoi(argv[5]) : DEFAULT_COUNT;

    printf("=== Mode1 Host Test ===\n");
    printf("Switch: %s:%d\n", switch_ip, switch_port);
    printf("Rank: %d / %d\n", rank, world_size);
    printf("Data count: %d (%d bytes)\n", count, count * 4);
    printf("=======================\n\n");

    // 初始化 Host 上下文
    mode1_host_ctx_t *ctx = mode1_host_init(switch_ip, switch_port,
                                             rank, world_size, 1);
    if (!ctx) {
        fprintf(stderr, "Failed to init host context\n");
        return 1;
    }

    printf("[Host %d] Initialized successfully\n", rank);

    // 准备测试数据
    int32_t *src = malloc(count * sizeof(int32_t));
    int32_t *dst = malloc(count * sizeof(int32_t));

    // 每个 rank 的数据: src[i] = rank + 1
    for (int i = 0; i < count; i++) {
        src[i] = rank + 1;
    }

    printf("[Host %d] Test data: src[0]=%d\n", rank, src[0]);

    // 等待所有 Host 连接完成
    printf("[Host %d] Waiting 2s for all hosts to connect...\n", rank);
    sleep(2);

    // 测试 AllReduce
    printf("[Host %d] Starting AllReduce test...\n", rank);
    struct timeval start, end;
    gettimeofday(&start, NULL);

    int ret = mode1_allreduce(ctx, src, count, dst);

    gettimeofday(&end, NULL);
    long elapsed = (end.tv_sec - start.tv_sec) * 1000000 +
                   (end.tv_usec - start.tv_usec);

    if (ret == 0) {
        // 验证结果: 期望值 = sum(1..world_size) = world_size*(world_size+1)/2
        int expected = world_size * (world_size + 1) / 2;
        int pass = 1;
        for (int i = 0; i < count; i++) {
            if (dst[i] != expected) {
                printf("[Host %d] FAIL: dst[%d]=%d, expected=%d\n",
                       rank, i, dst[i], expected);
                pass = 0;
                break;
            }
        }
        if (pass) {
            printf("[Host %d] PASS: AllReduce result correct, dst[0]=%d\n",
                   rank, dst[0]);
        }
        printf("[Host %d] AllReduce time: %ld us\n", rank, elapsed);
    } else {
        printf("[Host %d] FAIL: AllReduce returned %d\n", rank, ret);
    }

    free(src);
    free(dst);

    // 清理
    mode1_host_destroy(ctx);
    printf("[Host %d] Test complete\n", rank);

    return 0;
}
