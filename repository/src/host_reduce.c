#include "api.h"
#include "util.h"
#include <assert.h>
#include "topo_parser.h"
// 引入 INC 通信接口定义。
// 通用工具函数与类型定义。
// 断言用于结果校验。
// 拓扑解析相关定义（组建集群所需）。

// 模拟上层应用数据的元素数量。
#define IN_DATA_COUNT 4096
// 输入缓冲区，按 rank 写入不同的值。
int32_t in_data[IN_DATA_COUNT];
// 聚合后输出缓冲区（仅root节点使用）。
int32_t dst_data[IN_DATA_COUNT];

// 记录开始时间，用于计算耗时。
clock_t start_time;

// 打印以毫秒为单位的耗时信息。
void print_cost_time(const char * prefix) {
    clock_t end = clock();

    // 将时钟差转换为秒，再转毫秒。
    double elapsed_time = (double)(end - start_time) / CLOCKS_PER_SEC;
    printf("%s, Time taken: %f milliseconds\n", prefix, elapsed_time * 1000);
}

// 根据 rank 初始化输入数据，便于聚合后验证结果。
void init_data_to_aggregate(int rank) {

    // 逐元素写入，确保不同 rank 的数据可区分。
    for(int i = 0; i < IN_DATA_COUNT; i++) {
        in_data[i] = i * (rank+1);
    }

}


int main(int argc, char *argv[]) {
    // 需要四个参数：world_size master_addr rank root_rank。
    // 至于controller ip，是通过环境变量CONTROLLER_IP传入的。
    if(argc != 5) {
        printf("Usage: %s <world_size> <master_addr> <rank> <root_rank>\n", argv[0]);
        printf("Example: %s 2 192.168.0.1 0 0\n", argv[0]);
        return -1;
    }

    // 从命令行读取总节点数、主节点地址、当前节点 rank 和 root_rank。
    int world_size = atoi(argv[1]);
    char *master_addr = argv[2];
    int rank = atoi(argv[3]);
    int root_rank = atoi(argv[4]);
    
    printf("=== Reduce Test Configuration ===\n");
    printf("world_size: %d\n", world_size);
    printf("master_addr: %s\n", master_addr);
    printf("rank: %d\n", rank);
    printf("root_rank: %d\n", root_rank);
    printf("data_count: %d\n", IN_DATA_COUNT);
    printf("==================================\n\n");
    
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
    struct inccl_communicator *comm = inccl_communicator_create(group, IN_DATA_COUNT * 4);
    
    printf("Rank %d: Starting Reduce operation...\n", rank);
    fflush(stdout);
    
    // 记录开始时间。
    start_time = clock();

    // 执行 Reduce 操作（使用 RDMA WRITE 模式）
    // 所有节点发送数据，但只有 root_rank 接收聚合结果
    // inccl_reduce_write(comm, in_data, IN_DATA_COUNT, dst_data, root_rank);
    
    // 也可以使用 SEND/RECV 模式：
    inccl_reduce_sendrecv(comm, in_data, IN_DATA_COUNT, dst_data, root_rank);

    // 打印耗时。
    print_cost_time("Reduce operation completed");
    
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
        for(int i = 0; i < IN_DATA_COUNT; i++) {
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
            printf("\nSUCCESS: All %d elements verified correctly!\n", IN_DATA_COUNT);
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
    printf("Data count: %d elements\n", IN_DATA_COUNT);
    printf("====================\n");

    // 清理资源
    inccl_communicator_destroy(comm);
    inccl_group_destroy(group);

    printf("\nRank %d: Test completed.\n", rank);
    
    return 0;
}
