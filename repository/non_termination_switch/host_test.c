/**
 * @file host_test.c
 * @brief Host 测试程序，使用 INCCL API 进行真实 RDMA 通信测试
 */

#include "api.h"
#include "util.h"
#include <assert.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>

// 禁用 stdout 缓冲
#define PRINTF(...) do { printf(__VA_ARGS__); fflush(stdout); } while(0)

// 动态分配的数据缓冲区
int32_t *in_data = NULL;
int32_t *dst_data = NULL;
int data_count = 0;

struct timeval start_time;

// 格式化数据大小
static void format_size(uint64_t bytes, char *buf, size_t buf_size) {
    if (bytes >= 1024 * 1024 * 1024) {
        snprintf(buf, buf_size, "%.2f GB", (double)bytes / (1024 * 1024 * 1024));
    } else if (bytes >= 1024 * 1024) {
        snprintf(buf, buf_size, "%.2f MB", (double)bytes / (1024 * 1024));
    } else if (bytes >= 1024) {
        snprintf(buf, buf_size, "%.2f KB", (double)bytes / 1024);
    } else {
        snprintf(buf, buf_size, "%lu B", (unsigned long)bytes);
    }
}

void print_cost_time(const char *prefix) {
    struct timeval end;
    gettimeofday(&end, NULL);
    double elapsed_ms = (end.tv_sec - start_time.tv_sec) * 1000.0 +
                        (end.tv_usec - start_time.tv_usec) / 1000.0;
    double elapsed_sec = elapsed_ms / 1000.0;

    // 计算数据量和吞吐量
    uint64_t data_bytes = (uint64_t)data_count * sizeof(int32_t);
    double throughput_MBps = (double)data_bytes / (elapsed_sec * 1024 * 1024);
    double throughput_Mbps = (double)data_bytes * 8.0 / (elapsed_sec * 1e6);

    char size_str[32];
    format_size(data_bytes, size_str, sizeof(size_str));

    PRINTF("%s\n", prefix);
    PRINTF("  Data size: %s (%lu bytes)\n", size_str, (unsigned long)data_bytes);
    PRINTF("  Time: %.3f ms (%.3f s)\n", elapsed_ms, elapsed_sec);
    PRINTF("  Throughput: %.2f MB/s (%.2f Mbps)\n", throughput_MBps, throughput_Mbps);
}

void init_data(int rank) {
    for(int i = 0; i < data_count; i++) {
        in_data[i] = i * (rank + 1);
    }
}

void clear_dst_data() {
    memset(dst_data, 0, data_count * sizeof(int32_t));
}

int verify_allreduce(int world_size) {
    int expected_multiplier = 0;
    for(int r = 0; r < world_size; r++) {
        expected_multiplier += (r + 1);
    }

    int errors = 0;
    for(int i = 0; i < data_count && errors < 5; i++) {
        int expected = i * expected_multiplier;
        if(dst_data[i] != expected) {
            PRINTF("  ERROR idx %d: got %d, expected %d\n", i, dst_data[i], expected);
            errors++;
        }
    }
    return errors == 0;
}

int main(int argc, char *argv[]) {
    // 禁用所有缓冲
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    if(argc < 4) {
        PRINTF("Usage: %s <rank> <world_size> [data_size_bytes] [master_addr]\n", argv[0]);
        PRINTF("Example: %s 0 4 1048576 192.168.0.6\n", argv[0]);
        PRINTF("  rank: 节点编号\n");
        PRINTF("  world_size: 总节点数\n");
        PRINTF("  data_size_bytes: 测试数据大小（字节），默认 1MB\n");
        PRINTF("  master_addr: rank0 的 IP 地址，默认 10.1.1.21\n");
        return -1;
    }

    int rank = atoi(argv[1]);
    int world_size = atoi(argv[2]);
    uint64_t data_size = (argc >= 4) ? atol(argv[3]) : (1024 * 1024);  // 默认 1MB
    char *master_addr = (argc >= 5) ? argv[4] : "10.1.1.21";  // pku1 的数据网 IP

    // 计算 data_count (int32_t 数量)
    data_count = data_size / sizeof(int32_t);
    if (data_count < 1) data_count = 1;

    char size_str[32];
    format_size(data_size, size_str, sizeof(size_str));

    PRINTF("==========================================\n");
    PRINTF("       Host Test Program (INCCL)\n");
    PRINTF("==========================================\n");
    PRINTF("Rank: %d / %d\n", rank, world_size);
    PRINTF("Master addr: %s\n", master_addr);
    PRINTF("Data size: %s (%lu bytes)\n", size_str, (unsigned long)data_size);
    PRINTF("Data count: %d int32_t elements\n", data_count);
    PRINTF("==========================================\n\n");

    // 分配内存
    in_data = (int32_t *)malloc(data_count * sizeof(int32_t));
    dst_data = (int32_t *)malloc(data_count * sizeof(int32_t));
    if(!in_data || !dst_data) {
        PRINTF("ERROR: Failed to allocate memory\n");
        return -1;
    }

    // 初始化数据
    init_data(rank);
    clear_dst_data();

    // 创建通信组
    PRINTF("[%d] Creating group...\n", rank);
    struct inccl_group *group = inccl_group_create(world_size, rank, master_addr);
    if(!group) {
        PRINTF("[%d] ERROR: Failed to create group\n", rank);
        free(in_data);
        free(dst_data);
        return -1;
    }
    PRINTF("[%d] Group created successfully\n", rank);

    // 创建通信器
    PRINTF("[%d] Creating communicator...\n", rank);
    struct inccl_communicator *comm = inccl_communicator_create(group, data_count * sizeof(int32_t));
    if(!comm) {
        PRINTF("[%d] ERROR: Failed to create communicator\n", rank);
        free(in_data);
        free(dst_data);
        return -1;
    }
    PRINTF("[%d] Communicator created successfully\n", rank);

    // 执行 AllReduce 测试
    PRINTF("\n[%d] ========== AllReduce Test ==========\n", rank);

    gettimeofday(&start_time, NULL);
    inccl_allreduce_sendrecv(comm, in_data, data_count, dst_data);
    print_cost_time("AllReduce completed");

    // 验证结果
    if(verify_allreduce(world_size)) {
        PRINTF("  PASS: AllReduce verified (first 3: %d, %d, %d)\n",
               dst_data[0], dst_data[1], dst_data[2]);
    } else {
        PRINTF("  FAIL: AllReduce verification failed\n");
    }

    PRINTF("\n[%d] === Test Complete ===\n", rank);

    // 清理
    free(in_data);
    free(dst_data);
    return 0;
}
