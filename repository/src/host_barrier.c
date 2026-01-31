#include "api.h"
#include "util.h"
#include <assert.h>
#include <sys/time.h>
#include "topo_parser.h"

// 记录开始时间
struct timeval start_time;

void print_cost_time(const char *prefix, int rank) {
    struct timeval end;
    gettimeofday(&end, NULL);
    double elapsed_ms = (end.tv_sec - start_time.tv_sec) * 1000.0 +
                        (end.tv_usec - start_time.tv_usec) / 1000.0;

    printf("\n=== Performance Results (Rank %d) ===\n", rank);
    printf("Time: %.3f ms (%.3f s)\n", elapsed_ms, elapsed_ms / 1000.0);
    printf("=====================================\n");
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <world_size> <master_addr> <rank>\n", argv[0]);
        printf("Example: %s 4 192.168.0.6 0\n", argv[0]);
        return -1;
    }

    int world_size = atoi(argv[1]);
    char *master_addr = argv[2];
    int rank = atoi(argv[3]);

    printf("=== Barrier Test Configuration ===\n");
    printf("world_size: %d\n", world_size);
    printf("master_addr: %s\n", master_addr);
    printf("rank: %d\n", rank);
    printf("==================================\n\n");

    // 创建通信组
    printf("Rank %d: Creating communication group...\n", rank);
    struct inccl_group *group = inccl_group_create(world_size, rank, master_addr);

    // 创建通信器（Barrier 不需要大缓冲区）
    printf("Rank %d: Creating communicator...\n", rank);
    struct inccl_communicator *comm = inccl_communicator_create(group, 4096);

    printf("Rank %d: Starting Barrier operation...\n", rank);
    fflush(stdout);

    gettimeofday(&start_time, NULL);

    // 执行 Barrier 操作
    inccl_barrier(comm);

    // 打印耗时
    print_cost_time("Barrier", rank);

    printf("\n=== Test Summary ===\n");
    printf("Operation: Barrier\n");
    printf("World size: %d\n", world_size);
    printf("Current rank: %d\n", rank);
    printf("====================\n");

    printf("\nRank %d: Test completed.\n", rank);

    return 0;
}
