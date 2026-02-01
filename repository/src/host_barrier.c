#include "api.h"
#include "util.h"
#include <assert.h>
#include <sys/time.h>
#include "topo_parser.h"

// 默认迭代次数
#define DEFAULT_ITERATIONS 1000

int main(int argc, char *argv[]) {
    if (argc < 4 || argc > 5) {
        printf("Usage: %s <world_size> <master_addr> <rank> [iterations]\n", argv[0]);
        printf("Example: %s 4 192.168.0.6 0 1000\n", argv[0]);
        return -1;
    }

    int world_size = atoi(argv[1]);
    char *master_addr = argv[2];
    int rank = atoi(argv[3]);
    int iterations = (argc == 5) ? atoi(argv[4]) : DEFAULT_ITERATIONS;

    printf("=== Barrier Performance Test ===\n");
    printf("world_size: %d\n", world_size);
    printf("master_addr: %s\n", master_addr);
    printf("rank: %d\n", rank);
    printf("iterations: %d\n", iterations);
    printf("================================\n\n");
    fflush(stdout);

    // 创建通信组
    printf("Rank %d: Creating communication group...\n", rank);
    fflush(stdout);
    struct inccl_group *group = inccl_group_create(world_size, rank, master_addr);

    // 创建通信器
    printf("Rank %d: Creating communicator...\n", rank);
    fflush(stdout);
    struct inccl_communicator *comm = inccl_communicator_create(group, 4096);

    // 预热：先执行几次 Barrier
    printf("Rank %d: Warmup (5 iterations)...\n", rank);
    fflush(stdout);
    for (int i = 0; i < 5; i++) {
        inccl_barrier(comm);
    }

    // 开始性能测试
    printf("Rank %d: Starting performance test (%d iterations)...\n", rank, iterations);
    fflush(stdout);

    struct timeval start, end;
    gettimeofday(&start, NULL);

    for (int i = 0; i < iterations; i++) {
        inccl_barrier(comm);
    }

    gettimeofday(&end, NULL);

    // 计算性能指标
    double elapsed_us = (end.tv_sec - start.tv_sec) * 1000000.0 +
                        (end.tv_usec - start.tv_usec);
    double elapsed_ms = elapsed_us / 1000.0;
    double elapsed_s = elapsed_ms / 1000.0;
    double avg_latency_us = elapsed_us / iterations;
    double requests_per_sec = iterations / elapsed_s;

    // 打印结果
    printf("\n");
    printf("=== Barrier Performance Results (Rank %d) ===\n", rank);
    printf("Iterations:        %d\n", iterations);
    printf("Total time:        %.3f ms (%.3f s)\n", elapsed_ms, elapsed_s);
    printf("Avg latency:       %.3f us (%.3f ms)\n", avg_latency_us, avg_latency_us / 1000.0);
    printf("Throughput:        %.2f requests/sec\n", requests_per_sec);
    printf("==============================================\n");
    fflush(stdout);

    printf("\nRank %d: Test completed.\n", rank);
    fflush(stdout);

    return 0;
}
