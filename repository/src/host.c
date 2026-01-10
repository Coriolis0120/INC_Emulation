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
// 聚合后输出缓冲区。
int32_t dst_data[IN_DATA_COUNT];

// 记录开始时间，用于计算耗时。
clock_t start_time;

// 打印以毫秒为单位的耗时信息。
void print_cost_time(const char * prefix) {
    clock_t end = clock();

    // 将时钟差转换为秒，再转毫秒。
    double elapsed_time = (double)(end - start_time) / CLOCKS_PER_SEC;
    printf("%s, Time taken: %f mileseconds\n", prefix, elapsed_time * 1000);
}

// 根据 rank 初始化输入数据，便于聚合后验证结果。
void init_data_to_aggregate(int rank) {     
    
    // 逐元素写入，确保不同 rank 的数据可区分。
    for(int i = 0; i < IN_DATA_COUNT; i++) {
        in_data[i] = i * (rank+1);
    }
}


int main(int argc, char *argv[]) {
    // 需要三个参数：world_size master_addr rank。
    if(argc != 4) {
        printf("need 3 params now\n");
    }

    // 从命令行读取总节点数、主节点地址与当前节点 rank。
    int world_size = atoi(argv[1]);
    char *master_addr = argv[2];
    int rank = atoi(argv[3]);
    printf("world_size: %d, master_addr: %s, rank: %d\n", world_size, master_addr, rank);
    
    // 准备待聚合的数据。
    init_data_to_aggregate(rank);

    // 创建通信组（组 id 默认 0）。
    struct inccl_group *group = inccl_group_create(world_size,rank,master_addr); // group_id=0

    // 基于组创建通信器，并声明一次传输所需的字节数。
    struct inccl_communicator *comm = inccl_communicator_create(group, IN_DATA_COUNT * 4);
    printf("start...\n");

    // 记录开始时间。
    start_time = clock();

    inccl_allreduce_sendrecv(comm, in_data, IN_DATA_COUNT, dst_data);
    // 执行 allreduce 写操作，将 in_data 聚合到 dst_data。
    //inccl_allreduce_write(comm, in_data, IN_DATA_COUNT, dst_data);

    // 打印耗时。
    print_cost_time("over");
    
    // 校验聚合结果；当前假设所有 rank 均为 0、1、2，总和等于 3*i。
    for(int i = 0; i < IN_DATA_COUNT; i++) {
        if(i < 10)
            printf("idx: %d, in data: %d, reduce data: %d, expected: %d\n", i, in_data[i], dst_data[i], 3 * i);
        if(dst_data[i] != 3 * i) {
            printf("ERROR at idx %d: got %d, expected %d\n", i, dst_data[i], 3 * i);
            break;
        }
    }
    printf("result ok\n");

    // 结束程序。
    return 0;
}