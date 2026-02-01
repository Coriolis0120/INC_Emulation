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

void print_cost_time(const char *prefix, int rank, int64_t recv_count) {
    struct timeval end;
    gettimeofday(&end, NULL);
    double elapsed_ms = (end.tv_sec - start_time.tv_sec) * 1000.0 +
                        (end.tv_usec - start_time.tv_usec) / 1000.0;

    double data_bytes = data_count * 4.0;
    double recv_bytes = recv_count * 4.0;
    double elapsed_sec = elapsed_ms / 1000.0;
    double throughput_mbps = (recv_bytes * 8.0) / (elapsed_sec * 1e6);

    printf("\n=== Performance Results (Rank %d) ===\n", rank);
    printf("Send data size: %.2f MB (%d elements)\n", data_bytes / (1024.0 * 1024.0), data_count);
    printf("Recv data size: %.2f MB (%ld elements)\n", recv_bytes / (1024.0 * 1024.0), recv_count);
    printf("Time: %.3f ms (%.3f s)\n", elapsed_ms, elapsed_sec);
    printf("Throughput: %.2f Mbps (%.2f MB/s)\n", throughput_mbps, throughput_mbps / 8.0);
    printf("=====================================\n");
    fflush(stdout);
}

// 初始化数据：每个 rank 的数据是 (rank + 1) * 1000 + i
void init_data(int rank) {
    int base = (rank + 1) * 1000;
    for (int i = 0; i < data_count; i++) {
        src_data[i] = base + i;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        printf("Usage: %s <world_size> <master_addr> <rank> <data_count>\n", argv[0]);
        printf("Example: %s 4 192.168.0.6 0 1024\n", argv[0]);
        return -1;
    }

    int world_size = atoi(argv[1]);
    char *master_addr = argv[2];
    int rank = atoi(argv[3]);
    data_count = atoi(argv[4]);

    int64_t recv_count = (int64_t)data_count * world_size;

    printf("=== AllGather Test Configuration ===\n");
    printf("world_size: %d\n", world_size);
    printf("master_addr: %s\n", master_addr);
    printf("rank: %d\n", rank);
    printf("data_count (per rank): %d (%d KB)\n", data_count, data_count * 4 / 1024);
    printf("recv_count (total): %ld (%ld MB)\n", recv_count, recv_count * 4 / (1024 * 1024));
    printf("====================================\n\n");
    fflush(stdout);

    // 分配内存
    src_data = (int32_t *)malloc((size_t)data_count * sizeof(int32_t));
    dst_data = (int32_t *)malloc((size_t)recv_count * sizeof(int32_t));
    if (!src_data || !dst_data) {
        printf("ERROR: Failed to allocate memory (need %ld MB)\n", recv_count * 4 / (1024 * 1024));
        return -1;
    }
    memset(dst_data, 0, (size_t)recv_count * sizeof(int32_t));

    // 初始化数据
    init_data(rank);

    printf("Rank %d: Initialized src_data (first 5): %d, %d, %d, %d, %d\n",
           rank, src_data[0], src_data[1], src_data[2], src_data[3], src_data[4]);
    fflush(stdout);

    // 创建通信组
    printf("Rank %d: Creating communication group...\n", rank);
    fflush(stdout);
    struct inccl_group *group = inccl_group_create(world_size, rank, master_addr);

    // 创建通信器 - AllGather 每次 Broadcast 只传输 data_count 个元素
    printf("Rank %d: Creating communicator...\n", rank);
    fflush(stdout);
    struct inccl_communicator *comm = inccl_communicator_create(group, data_count * 4);

    printf("Rank %d: Starting AllGather operation...\n", rank);
    fflush(stdout);

    gettimeofday(&start_time, NULL);

    // 执行 AllGather 操作
    inccl_allgather(comm, src_data, data_count, dst_data);

    // 打印耗时
    print_cost_time("AllGather", rank, recv_count);

    // 验证结果
    printf("\n=== Rank %d: Verifying AllGather Result ===\n", rank);

    bool all_correct = true;
    for (int r = 0; r < world_size; r++) {
        int base = (r + 1) * 1000;
        int offset = r * data_count;

        printf("Chunk from rank %d (offset %d):\n", r, offset);

        for (int i = 0; i < data_count; i++) {
            int expected = base + i;
            int actual = dst_data[offset + i];

            if (i < 5) {
                printf("  [%d]: got %d, expected %d %s\n",
                       offset + i, actual, expected,
                       (actual == expected) ? "OK" : "FAIL");
            }

            if (actual != expected) {
                all_correct = false;
                if (i >= 5) {
                    printf("  ERROR at [%d]: got %d, expected %d\n",
                           offset + i, actual, expected);
                    break;
                }
            }
        }
    }

    if (all_correct) {
        printf("\nSUCCESS: All %d elements verified correctly!\n", recv_count);
    } else {
        printf("\nFAILED: Result verification failed!\n");
    }

    printf("\n=== Test Summary ===\n");
    printf("Operation: AllGather\n");
    printf("World size: %d\n", world_size);
    printf("Current rank: %d\n", rank);
    printf("Data count (per rank): %d elements\n", data_count);
    printf("Recv count (total): %d elements\n", recv_count);
    printf("====================\n");

    printf("\nRank %d: Test completed.\n", rank);
    fflush(stdout);

    free(src_data);
    free(dst_data);
    return 0;
}
