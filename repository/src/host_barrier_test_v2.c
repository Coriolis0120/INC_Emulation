/**
 * @file host_barrier_test_v2.c
 * @brief Barrier 测试程序 - 基于 AllReduce 实现
 *
 * Barrier 本质上是一个简化的 AllReduce，所有节点发送少量数据进行同步
 */

#include "api.h"
#include "util.h"
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>

#define BARRIER_DATA_COUNT 1024  // Barrier 使用固定的小数据量

int32_t *in_data = NULL;
int32_t *dst_data = NULL;
struct timeval start_time;

void print_barrier_time(const char *prefix) {
    struct timeval end;
    gettimeofday(&end, NULL);
    double elapsed_ms = (end.tv_sec - start_time.tv_sec) * 1000.0 +
                        (end.tv_usec - start_time.tv_usec) / 1000.0;
    printf("%s, Time: %.3f ms\n", prefix, elapsed_ms);
}

void init_barrier_data(int rank) {
    for(int i = 0; i < BARRIER_DATA_COUNT; i++) {
        in_data[i] = rank + 1;  // 简单的数据，用于验证
    }
}

int verify_barrier(int world_size) {
    // AllReduce 后，每个元素应该是所有 rank 的和: 1+2+...+world_size
    int expected = 0;
    for(int r = 0; r < world_size; r++) {
        expected += (r + 1);
    }
    int errors = 0;
    for(int i = 0; i < BARRIER_DATA_COUNT && errors < 5; i++) {
        if(dst_data[i] != expected) {
            printf("  ERROR idx %d: got %d, expected %d\n", i, dst_data[i], expected);
            errors++;
        }
    }
    return errors == 0;
}

int main(int argc, char *argv[]) {
    if(argc != 4) {
        printf("Usage: %s <world_size> <master_addr> <rank>\n", argv[0]);
        return -1;
    }

    int world_size = atoi(argv[1]);
    char *master_addr = argv[2];
    int rank = atoi(argv[3]);

    printf("=== Barrier Test V2 (AllReduce-based) ===\n");
    printf("world_size: %d, rank: %d, barrier_data_count: %d\n",
           world_size, rank, BARRIER_DATA_COUNT);
    fflush(stdout);

    in_data = (int32_t *)malloc(BARRIER_DATA_COUNT * sizeof(int32_t));
    dst_data = (int32_t *)malloc(BARRIER_DATA_COUNT * sizeof(int32_t));
    if(!in_data || !dst_data) {
        printf("ERROR: malloc failed\n");
        return -1;
    }

    init_barrier_data(rank);
    memset(dst_data, 0, BARRIER_DATA_COUNT * sizeof(int32_t));

    printf("Rank %d: Creating group...\n", rank);
    fflush(stdout);
    struct inccl_group *group = inccl_group_create(world_size, rank, master_addr);
    if(!group) {
        printf("ERROR: Failed to create group\n");
        return -1;
    }

    printf("Rank %d: Creating communicator...\n", rank);
    fflush(stdout);
    struct inccl_communicator *comm = inccl_communicator_create(group, BARRIER_DATA_COUNT * 4);
    if(!comm) {
        printf("ERROR: Failed to create communicator\n");
        return -1;
    }

    printf("\n=== Barrier Test ===\n");
    fflush(stdout);

    gettimeofday(&start_time, NULL);
    // Barrier 使用 AllReduce 实现同步
    inccl_allreduce_simple(comm, in_data, BARRIER_DATA_COUNT, dst_data);
    print_barrier_time("Barrier completed");

    if(verify_barrier(world_size)) {
        printf("  PASS: Barrier verified\n");
    } else {
        printf("  FAIL: Barrier verification failed\n");
    }

    printf("\n=== Test Complete ===\n");
    fflush(stdout);

    free(in_data);
    free(dst_data);
    return 0;
}
