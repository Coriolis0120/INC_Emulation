#include "api.h"
#include "util.h"
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>

int32_t *in_data = NULL;
int32_t *dst_data = NULL;
int data_count = 0;
struct timeval start_time;

void print_cost_time(const char *prefix) {
    struct timeval end;
    gettimeofday(&end, NULL);
    double elapsed_ms = (end.tv_sec - start_time.tv_sec) * 1000.0 +
                        (end.tv_usec - start_time.tv_usec) / 1000.0;
    double throughput_mbps = (data_count * 4.0 * 8.0) / (elapsed_ms / 1000.0 * 1e6);
    printf("%s, Time: %.3f ms, Throughput: %.2f Mbps\n", prefix, elapsed_ms, throughput_mbps);
}

void init_data(int rank) {
    for(int i = 0; i < data_count; i++) {
        in_data[i] = i * (rank + 1);
    }
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
            printf("  ERROR idx %d: got %d, expected %d\n", i, dst_data[i], expected);
            errors++;
        }
    }
    return errors == 0;
}

int main(int argc, char *argv[]) {
    if(argc != 5) {
        printf("Usage: %s <world_size> <master_addr> <rank> <data_count>\n", argv[0]);
        return -1;
    }

    int world_size = atoi(argv[1]);
    char *master_addr = argv[2];
    int rank = atoi(argv[3]);
    data_count = atoi(argv[4]);

    printf("=== Simple AllReduce Test V2 (No Control Packet) ===\n");
    printf("world_size: %d, rank: %d, data_count: %d\n", world_size, rank, data_count);
    fflush(stdout);

    in_data = (int32_t *)malloc(data_count * sizeof(int32_t));
    dst_data = (int32_t *)malloc(data_count * sizeof(int32_t));
    if(!in_data || !dst_data) {
        printf("ERROR: malloc failed\n");
        return -1;
    }

    init_data(rank);
    memset(dst_data, 0, data_count * sizeof(int32_t));

    printf("Rank %d: Creating group...\n", rank);
    fflush(stdout);
    struct inccl_group *group = inccl_group_create(world_size, rank, master_addr);
    if(!group) {
        printf("ERROR: Failed to create group\n");
        return -1;
    }

    printf("Rank %d: Creating communicator...\n", rank);
    fflush(stdout);
    struct inccl_communicator *comm = inccl_communicator_create(group, data_count * 4);
    if(!comm) {
        printf("ERROR: Failed to create communicator\n");
        return -1;
    }

    printf("\n=== AllReduce Test ===\n");
    fflush(stdout);

    gettimeofday(&start_time, NULL);
    inccl_allreduce_simple(comm, in_data, data_count, dst_data);
    print_cost_time("AllReduce completed");

    if(verify_allreduce(world_size)) {
        printf("  PASS: AllReduce verified\n");
    } else {
        printf("  FAIL: AllReduce verification failed\n");
    }

    printf("\n=== Test Complete ===\n");
    fflush(stdout);

    free(in_data);
    free(dst_data);
    return 0;
}
