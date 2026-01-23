#include "api.h"
#include "util.h"
#include <assert.h>
#include <string.h>
#include "topo_parser.h"

// 测试数据大小
#define IN_DATA_COUNT 4096

// 数据缓冲区
int32_t in_data[IN_DATA_COUNT];
int32_t dst_data[IN_DATA_COUNT];

// 记录开始时间
clock_t start_time;

void print_cost_time(const char *prefix) {
    clock_t end = clock();
    double elapsed_time = (double)(end - start_time) / CLOCKS_PER_SEC;
    printf("%s, Time taken: %f milliseconds\n", prefix, elapsed_time * 1000);
}

// 初始化数据：in_data[i] = i * (rank + 1)
void init_data(int rank) {
    for (int i = 0; i < IN_DATA_COUNT; i++) {
        in_data[i] = i * (rank + 1);
    }
    memset(dst_data, 0, sizeof(dst_data));
}

// 验证 Reduce 结果（仅 root 节点）
bool verify_reduce_result(int world_size, int rank, int root_rank) {
    if (rank != root_rank) {
        printf("  [Rank %d] Non-root, skipping verification\n", rank);
        return true;
    }

    // 期望值：sum of i*(r+1) for r in [0, world_size)
    // = i * (1 + 2 + ... + world_size) = i * world_size*(world_size+1)/2
    int expected_multiplier = world_size * (world_size + 1) / 2;

    bool all_correct = true;
    for (int i = 0; i < IN_DATA_COUNT; i++) {
        int expected = i * expected_multiplier;
        if (dst_data[i] != expected) {
            if (all_correct) {
                printf("  [Rank %d] ERROR: Reduce verification failed!\n", rank);
            }
            printf("    idx %d: got %d, expected %d\n", i, dst_data[i], expected);
            all_correct = false;
            if (i >= 5) break;
        }
    }

    if (all_correct) {
        printf("  [Rank %d] Reduce result verified: PASS (first 3: %d, %d, %d)\n",
               rank, dst_data[0], dst_data[1], dst_data[2]);
    }
    return all_correct;
}

// 验证 AllReduce 结果（所有节点）
bool verify_allreduce_result(int world_size, int rank) {
    // 期望值：sum of i*(r+1) for r in [0, world_size)
    int expected_multiplier = world_size * (world_size + 1) / 2;

    bool all_correct = true;
    for (int i = 0; i < IN_DATA_COUNT; i++) {
        int expected = i * expected_multiplier;
        if (dst_data[i] != expected) {
            if (all_correct) {
                printf("  [Rank %d] ERROR: AllReduce verification failed!\n", rank);
            }
            printf("    idx %d: got %d, expected %d\n", i, dst_data[i], expected);
            all_correct = false;
            if (i >= 5) break;
        }
    }

    if (all_correct) {
        printf("  [Rank %d] AllReduce result verified: PASS (first 3: %d, %d, %d)\n",
               rank, dst_data[0], dst_data[1], dst_data[2]);
    }
    return all_correct;
}

// 验证 Broadcast 结果
// root 节点的数据应该被广播到所有其他节点
// root 节点的 in_data[i] = i * (root_rank + 1)
bool verify_broadcast_result(int world_size, int rank, int root_rank) {
    // 期望值：root 节点的数据 = i * (root_rank + 1)
    int expected_multiplier = root_rank + 1;

    bool all_correct = true;
    for (int i = 0; i < IN_DATA_COUNT; i++) {
        int expected = i * expected_multiplier;
        if (in_data[i] != expected) {
            if (all_correct) {
                printf("  [Rank %d] ERROR: Broadcast verification failed!\n", rank);
            }
            printf("    idx %d: got %d, expected %d\n", i, in_data[i], expected);
            all_correct = false;
            if (i >= 5) break;
        }
    }

    if (all_correct) {
        printf("  [Rank %d] Broadcast result verified: PASS (first 3: %d, %d, %d)\n",
               rank, in_data[0], in_data[1], in_data[2]);
    }
    return all_correct;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <world_size> <master_addr> <rank>\n", argv[0]);
        printf("Example: %s 2 192.168.0.1 0\n", argv[0]);
        return -1;
    }

    int world_size = atoi(argv[1]);
    char *master_addr = argv[2];
    int rank = atoi(argv[3]);

    printf("=== Multi-Operation Test ===\n");
    printf("world_size: %d\n", world_size);
    printf("master_addr: %s\n", master_addr);
    printf("rank: %d\n", rank);
    printf("data_count: %d\n", IN_DATA_COUNT);
    printf("============================\n\n");

    // 创建通信组和通信器
    printf("Rank %d: Creating communication group...\n", rank);
    struct inccl_group *group = inccl_group_create(world_size, rank, master_addr);

    printf("Rank %d: Creating communicator...\n", rank);
    struct inccl_communicator *comm = inccl_communicator_create(group, IN_DATA_COUNT * 4);

    int test_passed = 0;
    int test_failed = 0;

    // ========== 测试 1: Reduce (root=0) ==========
    printf("\n========== Test 1: Reduce (root=0) ==========\n");
    init_data(rank);

    printf("Rank %d: Starting Reduce (root=0)...\n", rank);
    fflush(stdout);
    start_time = clock();

    inccl_reduce_sendrecv(comm, in_data, IN_DATA_COUNT, dst_data, 0);

    print_cost_time("Reduce (root=0) completed");

    if (verify_reduce_result(world_size, rank, 0)) {
        test_passed++;
    } else {
        test_failed++;
    }

    // ========== 测试 2: AllReduce ==========
    printf("\n========== Test 2: AllReduce ==========\n");
    init_data(rank);

    printf("Rank %d: Starting AllReduce...\n", rank);
    fflush(stdout);
    start_time = clock();

    inccl_allreduce_sendrecv(comm, in_data, IN_DATA_COUNT, dst_data);

    print_cost_time("AllReduce completed");

    if (verify_allreduce_result(world_size, rank)) {
        test_passed++;
    } else {
        test_failed++;
    }

    // ========== 测试 3: Reduce (root=1) ==========
    printf("\n========== Test 3: Reduce (root=1) ==========\n");
    init_data(rank);

    int root_rank = (world_size > 1) ? 1 : 0;
    printf("Rank %d: Starting Reduce (root=%d)...\n", rank, root_rank);
    fflush(stdout);
    start_time = clock();

    inccl_reduce_sendrecv(comm, in_data, IN_DATA_COUNT, dst_data, root_rank);

    print_cost_time("Reduce (root=1) completed");

    if (verify_reduce_result(world_size, rank, root_rank)) {
        test_passed++;
    } else {
        test_failed++;
    }

    // ========== 测试 4: AllReduce (again) ==========
    printf("\n========== Test 4: AllReduce (again) ==========\n");
    init_data(rank);

    printf("Rank %d: Starting AllReduce (again)...\n", rank);
    fflush(stdout);
    start_time = clock();

    inccl_allreduce_sendrecv(comm, in_data, IN_DATA_COUNT, dst_data);

    print_cost_time("AllReduce (again) completed");

    if (verify_allreduce_result(world_size, rank)) {
        test_passed++;
    } else {
        test_failed++;
    }

    // ========== 测试 5: Broadcast (root=0) ==========
    printf("\n========== Test 5: Broadcast (root=0) ==========\n");
    init_data(rank);

    printf("Rank %d: Starting Broadcast (root=0)...\n", rank);
    printf("Rank %d: Before broadcast, in_data[0..2] = %d, %d, %d\n", rank, in_data[0], in_data[1], in_data[2]);
    fflush(stdout);
    start_time = clock();

    inccl_broadcast_sendrecv(comm, in_data, IN_DATA_COUNT, 0);

    print_cost_time("Broadcast (root=0) completed");
    printf("Rank %d: After broadcast, in_data[0..2] = %d, %d, %d\n", rank, in_data[0], in_data[1], in_data[2]);

    if (verify_broadcast_result(world_size, rank, 0)) {
        test_passed++;
    } else {
        test_failed++;
    }

    // ========== 测试 6: Broadcast (root=1) ==========
    if (world_size > 1) {
        printf("\n========== Test 6: Broadcast (root=1) ==========\n");
        init_data(rank);

        printf("Rank %d: Starting Broadcast (root=1)...\n", rank);
        printf("Rank %d: Before broadcast, in_data[0..2] = %d, %d, %d\n", rank, in_data[0], in_data[1], in_data[2]);
        fflush(stdout);
        start_time = clock();

        inccl_broadcast_sendrecv(comm, in_data, IN_DATA_COUNT, 1);

        print_cost_time("Broadcast (root=1) completed");
        printf("Rank %d: After broadcast, in_data[0..2] = %d, %d, %d\n", rank, in_data[0], in_data[1], in_data[2]);

        if (verify_broadcast_result(world_size, rank, 1)) {
            test_passed++;
        } else {
            test_failed++;
        }
    }

    // ========== 测试总结 ==========
    printf("\n========================================\n");
    printf("       Multi-Operation Test Summary\n");
    printf("========================================\n");
    printf("Rank: %d\n", rank);
    printf("Tests passed: %d\n", test_passed);
    printf("Tests failed: %d\n", test_failed);
    printf("Overall: %s\n", (test_failed == 0) ? "ALL PASSED" : "SOME FAILED");
    printf("========================================\n");

    // 清理资源
    // inccl_communicator_destroy(comm);
    // inccl_group_destroy(group);

    return (test_failed == 0) ? 0 : 1;
}
