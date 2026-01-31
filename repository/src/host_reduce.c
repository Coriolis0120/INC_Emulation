#include "api.h"
#include "util.h"
#include <assert.h>
#include <sys/time.h>
#include "topo_parser.h"

// 动态分配的数据缓冲区
int32_t *in_data = NULL;
int32_t *dst_data = NULL;
int data_count = 0;

// 记录开始时间
struct timeval start_time;

// 打印耗时和吞吐量
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

// 根据 rank 初始化输入数据，便于聚合后验证结果。
void init_data_to_aggregate(int rank) {
    // 逐元素写入，确保不同 rank 的数据可区分。
    for(int i = 0; i < data_count; i++) {
        in_data[i] = i * (rank+1);
    }
}


int main(int argc, char *argv[]) {
    // 需要五个参数：world_size master_addr rank root_rank data_count
    if(argc != 6) {
        printf("Usage: %s <world_size> <master_addr> <rank> <root_rank> <data_count>\n", argv[0]);
        printf("Example: %s 2 192.168.0.1 0 0 262144\n", argv[0]);
        return -1;
    }

    // 从命令行读取参数
    int world_size = atoi(argv[1]);
    char *master_addr = argv[2];
    int rank = atoi(argv[3]);
    int root_rank = atoi(argv[4]);
    data_count = atoi(argv[5]);
    
    printf("=== Reduce Test Configuration ===\n");
    printf("world_size: %d\n", world_size);
    printf("master_addr: %s\n", master_addr);
    printf("rank: %d\n", rank);
    printf("root_rank: %d\n", root_rank);
    printf("data_count: %d (%d KB)\n", data_count, data_count * 4 / 1024);
    printf("==================================\n\n");

    // 分配内存
    in_data = (int32_t *)malloc(data_count * sizeof(int32_t));
    dst_data = (int32_t *)malloc(data_count * sizeof(int32_t));
    if(!in_data || !dst_data) {
        printf("ERROR: Failed to allocate memory\n");
        return -1;
    }
    memset(dst_data, 0, data_count * sizeof(int32_t));

    // 准备待聚合的数据。
    init_data_to_aggregate(rank);
    
    printf("Rank %d: Initialized input data (first 5 elements): ", rank);
    for(int i = 0; i < 5; i++) {
        printf("%d ", in_data[i]);
    }
    printf("\n");

    // 创建通信组（组 id 默认 0）。
    printf("Rank %d: Creating communication group...\n", rank);
    struct inccl_group *group = inccl_group_create(world_size, rank, master_addr);

    // 基于组创建通信器，并声明一次传输所需的字节数。
    printf("Rank %d: Creating communicator...\n", rank);
    struct inccl_communicator *comm = inccl_communicator_create(group, data_count * 4);

    printf("Rank %d: Starting Reduce operation...\n", rank);
    fflush(stdout);

    // 记录开始时间。
    gettimeofday(&start_time, NULL);

    // 执行 Reduce 操作
    inccl_reduce_sendrecv(comm, in_data, data_count, dst_data, root_rank);

    // 打印耗时和吞吐量
    print_cost_time("Reduce", rank);
    
    // 验证结果
    if(rank == root_rank) {
        // Root节点：验证聚合结果
        printf("\n=== Root Rank %d: Verifying Reduce Result ===\n", rank);
        
        // 计算期望值：所有rank的数据之和
        // rank 0: i * 1, rank 1: i * 2, ... 
        // 对于world_size=2: 期望值 = i * 1 + i * 2 = i * 3
        int expected_multiplier = 0;
        for(int r = 0; r < world_size; r++) {
            expected_multiplier += (r + 1);
        }
        
        printf("Expected multiplier: %d (sum of 1..%d)\n", expected_multiplier, world_size);
        printf("\nFirst 10 elements:\n");
        
        bool all_correct = true;
        for(int i = 0; i < data_count; i++) {
            int expected = i * expected_multiplier;
            
            if(i < 10) {
                printf("  idx %d: in_data=%d, dst_data=%d, expected=%d %s\n", 
                       i, in_data[i], dst_data[i], expected,
                       (dst_data[i] == expected) ? "✓" : "✗");
            }
            
            if(dst_data[i] != expected) {
                if(all_correct) {
                    printf("\nERROR: Mismatch detected!\n");
                }
                printf("  ERROR at idx %d: got %d, expected %d\n", i, dst_data[i], expected);
                all_correct = false;
                if(i >= 10) break; // 只打印前几个错误
            }
        }
        
        if(all_correct) {
            printf("\nSUCCESS: All %d elements verified correctly!\n", data_count);
            printf("Root rank %d received correct aggregated result.\n", rank);
        } else {
            printf("\nFAILED: Result verification failed!\n");
        }
        
    } else {
        // 非Root节点：不应该接收到数据
        printf("\n=== Non-Root Rank %d: Checking Result ===\n", rank);
        printf("This rank should NOT receive aggregated data.\n");
        
        // 检查dst_data是否未被修改（应该全为0或保持初始值）
        bool data_unchanged = true;
        for(int i = 0; i < 10; i++) {
            if(dst_data[i] != 0) {
                data_unchanged = false;
                break;
            }
        }
        
        if(data_unchanged) {
            printf("Correct: dst_data remains unmodified (first 10 elements are 0).\n");
        } else {
            printf("Warning: dst_data was modified (unexpected for non-root rank).\n");
            printf("First 5 elements of dst_data: ");
            for(int i = 0; i < 5; i++) {
                printf("%d ", dst_data[i]);
            }
            printf("\n");
        }
        
        printf("Rank %d completed Reduce operation successfully.\n", rank);
    }

    
    printf("\n=== Test Summary ===\n");
    printf("Operation: Reduce\n");
    printf("World size: %d\n", world_size);
    printf("Root rank: %d\n", root_rank);
    printf("Current rank: %d\n", rank);
    printf("Data count: %d elements\n", data_count);
    printf("====================\n");

    printf("\nRank %d: Test completed.\n", rank);

    return 0;
}
