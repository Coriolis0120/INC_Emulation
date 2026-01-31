#include "api.h"
#include "util.h"
#include <assert.h>
#include <sys/time.h>
#include "topo_parser.h"

// 动态分配的数据缓冲区
int32_t *data = NULL;
int data_count = 0;

// 记录开始时间
struct timeval start_time;

void print_cost_time(const char *prefix, int rank) {
    struct timeval end;
    gettimeofday(&end, NULL);
    double elapsed_ms = (end.tv_sec - start_time.tv_sec) * 1000.0 +
                        (end.tv_usec - start_time.tv_usec) / 1000.0;

    double data_bytes = data_count * 4.0;
    double elapsed_sec = elapsed_ms / 1000.0;
    double throughput_mbps = (data_bytes * 8.0) / (elapsed_sec * 1e6);

    printf("\n=== Performance Results (Rank %d) ===\n", rank);
    printf("Data size: %.2f MB (%d elements)\n", data_bytes / (1024.0 * 1024.0), data_count);
    printf("Time: %.3f ms (%.3f s)\n", elapsed_ms, elapsed_sec);
    printf("Throughput: %.2f Mbps (%.2f MB/s)\n", throughput_mbps, throughput_mbps / 8.0);
    printf("=====================================\n");
    fflush(stdout);
}

// Root 节点初始化数据
void init_broadcast_data(int rank, int root_rank) {
    if (rank == root_rank) {
        // Root 节点：初始化要广播的数据
        for (int i = 0; i < data_count; i++) {
            data[i] = i * (root_rank + 1);
        }
    } else {
        // 非 Root 节点：清零，等待接收
        memset(data, 0, data_count * sizeof(int32_t));
    }
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        printf("Usage: %s <world_size> <master_addr> <rank> <root_rank> <data_count>\n", argv[0]);
        printf("Example: %s 2 192.168.0.1 0 0 262144\n", argv[0]);
        return -1;
    }

    int world_size = atoi(argv[1]);
    char *master_addr = argv[2];
    int rank = atoi(argv[3]);
    int root_rank = atoi(argv[4]);
    data_count = atoi(argv[5]);

    printf("=== Broadcast Test Configuration ===\n");
    printf("world_size: %d\n", world_size);
    printf("master_addr: %s\n", master_addr);
    printf("rank: %d\n", rank);
    printf("root_rank: %d\n", root_rank);
    printf("data_count: %d (%d KB)\n", data_count, data_count * 4 / 1024);
    printf("====================================\n\n");

    // 分配内存
    data = (int32_t *)malloc(data_count * sizeof(int32_t));
    if (!data) {
        printf("ERROR: Failed to allocate memory\n");
        return -1;
    }

    // 初始化数据
    init_broadcast_data(rank, root_rank);

    printf("Rank %d: Before broadcast, data[0..4] = %d, %d, %d, %d, %d\n",
           rank, data[0], data[1], data[2], data[3], data[4]);

    // 创建通信组和通信器
    printf("Rank %d: Creating communication group...\n", rank);
    struct inccl_group *group = inccl_group_create(world_size, rank, master_addr);

    printf("Rank %d: Creating communicator...\n", rank);
    struct inccl_communicator *comm = inccl_communicator_create(group, data_count * 4);

    printf("Rank %d: Starting Broadcast operation...\n", rank);
    fflush(stdout);

    gettimeofday(&start_time, NULL);

    // 执行 Broadcast 操作
    inccl_broadcast_sendrecv(comm, data, data_count, root_rank);

    // 打印耗时和吞吐量
    print_cost_time("Broadcast", rank);

    printf("Rank %d: After broadcast, data[0..4] = %d, %d, %d, %d, %d\n",
           rank, data[0], data[1], data[2], data[3], data[4]);

    // 验证结果
    printf("\n=== Rank %d: Verifying Broadcast Result ===\n", rank);

    int expected_multiplier = root_rank + 1;
    bool all_correct = true;

    for (int i = 0; i < data_count; i++) {
        int expected = i * expected_multiplier;

        if (i < 10) {
            printf("  idx %d: data=%d, expected=%d %s\n",
                   i, data[i], expected,
                   (data[i] == expected) ? "✓" : "✗");
        }

        if (data[i] != expected) {
            if (all_correct) {
                printf("\nERROR: Mismatch detected!\n");
            }
            printf("  ERROR at idx %d: got %d, expected %d\n", i, data[i], expected);
            all_correct = false;
            if (i >= 10) break;
        }
    }

    if (all_correct) {
        printf("\nSUCCESS: All %d elements verified correctly!\n", data_count);
    } else {
        printf("\nFAILED: Result verification failed!\n");
    }

    printf("\n=== Test Summary ===\n");
    printf("Operation: Broadcast\n");
    printf("World size: %d\n", world_size);
    printf("Root rank: %d\n", root_rank);
    printf("Current rank: %d\n", rank);
    printf("Data count: %d elements\n", data_count);
    printf("====================\n");

    printf("\nRank %d: Test completed.\n", rank);

    free(data);
    return 0;
}
