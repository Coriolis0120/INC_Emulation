#include "api.h"
#include "util.h"
#include <assert.h>
#include <sys/time.h>
#include "topo_parser.h"

// 动态分配的数据缓冲区
int32_t *src_data = NULL;
int32_t *dst_data = NULL;
int data_count = 0;

// 记录开始时间
struct timeval start_time;

void print_cost_time(const char *prefix, int rank, int recv_count) {
    struct timeval end;
    gettimeofday(&end, NULL);
    double elapsed_ms = (end.tv_sec - start_time.tv_sec) * 1000.0 +
                        (end.tv_usec - start_time.tv_usec) / 1000.0;

    double data_bytes = data_count * 4.0;
    double recv_bytes = recv_count * 4.0;
    double elapsed_sec = elapsed_ms / 1000.0;
    double throughput_mbps = (data_bytes * 8.0) / (elapsed_sec * 1e6);

    printf("\n=== Performance Results (Rank %d) ===\n", rank);
    printf("Send data size: %.2f MB (%d elements)\n", data_bytes / (1024.0 * 1024.0), data_count);
    printf("Recv data size: %.2f MB (%d elements)\n", recv_bytes / (1024.0 * 1024.0), recv_count);
    printf("Time: %.3f ms (%.3f s)\n", elapsed_ms, elapsed_sec);
    printf("Throughput: %.2f Mbps (%.2f MB/s)\n", throughput_mbps, throughput_mbps / 8.0);
    printf("=====================================\n");
    fflush(stdout);
}

// 初始化数据
void init_data(int rank) {
    for (int i = 0; i < data_count; i++) {
        src_data[i] = i * (rank + 1);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        printf("Usage: %s <world_size> <master_addr> <rank> <data_count>\n", argv[0]);
        printf("Example: %s 4 192.168.0.6 0 262144\n", argv[0]);
        printf("Note: data_count must be divisible by world_size\n");
        return -1;
    }

    int world_size = atoi(argv[1]);
    char *master_addr = argv[2];
    int rank = atoi(argv[3]);
    data_count = atoi(argv[4]);

    // 检查 data_count 是否能被 world_size 整除
    if (data_count % world_size != 0) {
        printf("ERROR: data_count (%d) must be divisible by world_size (%d)\n",
               data_count, world_size);
        return -1;
    }

    int recv_count = data_count / world_size;

    printf("=== ReduceScatter Test Configuration ===\n");
    printf("world_size: %d\n", world_size);
    printf("master_addr: %s\n", master_addr);
    printf("rank: %d\n", rank);
    printf("data_count: %d (%d KB)\n", data_count, data_count * 4 / 1024);
    printf("recv_count: %d (%d KB)\n", recv_count, recv_count * 4 / 1024);
    printf("========================================\n\n");

    // 分配内存
    src_data = (int32_t *)malloc(data_count * sizeof(int32_t));
    dst_data = (int32_t *)malloc(recv_count * sizeof(int32_t));
    if (!src_data || !dst_data) {
        printf("ERROR: Failed to allocate memory\n");
        return -1;
    }
    memset(dst_data, 0, recv_count * sizeof(int32_t));

    // 初始化数据
    init_data(rank);

    printf("Rank %d: Initialized src_data (first 5): %d, %d, %d, %d, %d\n",
           rank, src_data[0], src_data[1], src_data[2], src_data[3], src_data[4]);

    // 创建通信组
    printf("Rank %d: Creating communication group...\n", rank);
    struct inccl_group *group = inccl_group_create(world_size, rank, master_addr);

    // 创建通信器
    printf("Rank %d: Creating communicator...\n", rank);
    struct inccl_communicator *comm = inccl_communicator_create(group, data_count * 4);

    printf("Rank %d: Starting ReduceScatter operation...\n", rank);
    fflush(stdout);

    gettimeofday(&start_time, NULL);

    // 执行 ReduceScatter 操作
    inccl_reducescatter(comm, src_data, data_count, dst_data);

    // 打印耗时
    print_cost_time("ReduceScatter", rank, recv_count);

    // 验证结果
    printf("\n=== Rank %d: Verifying ReduceScatter Result ===\n", rank);

    // 计算期望值
    // 每个 rank 发送 src_data[i] = i * (rank + 1)
    // 聚合后 sum[i] = i * (1 + 2 + ... + world_size) = i * world_size * (world_size + 1) / 2
    // rank r 接收 sum[r * recv_count ... (r+1) * recv_count - 1]
    int expected_multiplier = world_size * (world_size + 1) / 2;
    int start_idx = rank * recv_count;

    printf("Expected multiplier: %d\n", expected_multiplier);
    printf("This rank receives indices [%d, %d)\n", start_idx, start_idx + recv_count);
    printf("\nFirst 10 elements:\n");

    bool all_correct = true;
    for (int i = 0; i < recv_count; i++) {
        int global_idx = start_idx + i;
        int expected = global_idx * expected_multiplier;

        if (i < 10) {
            printf("  idx %d (global %d): dst=%d, expected=%d %s\n",
                   i, global_idx, dst_data[i], expected,
                   (dst_data[i] == expected) ? "OK" : "FAIL");
        }

        if (dst_data[i] != expected) {
            if (all_correct) {
                printf("\nERROR: Mismatch detected!\n");
            }
            if (i >= 10) {
                printf("  ERROR at idx %d: got %d, expected %d\n",
                       i, dst_data[i], expected);
            }
            all_correct = false;
            if (i >= 15) break;
        }
    }

    if (all_correct) {
        printf("\nSUCCESS: All %d elements verified correctly!\n", recv_count);
    } else {
        printf("\nFAILED: Result verification failed!\n");
    }

    printf("\n=== Test Summary ===\n");
    printf("Operation: ReduceScatter\n");
    printf("World size: %d\n", world_size);
    printf("Current rank: %d\n", rank);
    printf("Data count: %d elements\n", data_count);
    printf("Recv count: %d elements\n", recv_count);
    printf("====================\n");

    printf("\nRank %d: Test completed.\n", rank);

    free(src_data);
    free(dst_data);
    return 0;
}
