/**
 * @file mpi_collective_test.c
 * @brief MPI 集合通信性能测试
 *
 * 测试 AllReduce, Reduce, Broadcast, AllGather 等操作的性能
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include <sys/time.h>

// 获取当前时间（微秒）
static inline long long get_time_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}

// 验证 AllReduce 结果
static int verify_allreduce(int *data, int count, int world_size) {
    int expected = world_size * (world_size + 1) / 2;  // sum of 1+2+...+world_size
    for (int i = 0; i < count; i++) {
        if (data[i] != expected) {
            return 0;
        }
    }
    return 1;
}

// 验证 Reduce 结果
static int verify_reduce(int *data, int count, int world_size, int rank, int root) {
    if (rank != root) return 1;  // 只有 root 需要验证
    int expected = world_size * (world_size + 1) / 2;
    for (int i = 0; i < count; i++) {
        if (data[i] != expected) {
            return 0;
        }
    }
    return 1;
}

// 验证 Broadcast 结果
static int verify_broadcast(int *data, int count, int root) {
    for (int i = 0; i < count; i++) {
        if (data[i] != root + 1) {
            return 0;
        }
    }
    return 1;
}

// 验证 AllGather 结果
static int verify_allgather(int *data, int count, int world_size) {
    for (int r = 0; r < world_size; r++) {
        for (int i = 0; i < count; i++) {
            if (data[r * count + i] != r + 1) {
                return 0;
            }
        }
    }
    return 1;
}

// 验证 ReduceScatter 结果
static int verify_reducescatter(int *data, int count, int world_size, int rank) {
    int expected = world_size * (world_size + 1) / 2;  // sum of 1+2+...+world_size
    for (int i = 0; i < count; i++) {
        if (data[i] != expected) {
            return 0;
        }
    }
    return 1;
}

// 测试 AllReduce (返回吞吐量 Mbps)
static double test_allreduce(int rank, int world_size, int count, int iterations) {
    int *send_buf = (int *)malloc(count * sizeof(int));
    int *recv_buf = (int *)malloc(count * sizeof(int));

    // 初始化数据
    for (int i = 0; i < count; i++) {
        send_buf[i] = rank + 1;
    }

    // 预热
    MPI_Allreduce(send_buf, recv_buf, count, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    MPI_Barrier(MPI_COMM_WORLD);
    long long start = get_time_us();

    for (int iter = 0; iter < iterations; iter++) {
        MPI_Allreduce(send_buf, recv_buf, count, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    }

    MPI_Barrier(MPI_COMM_WORLD);
    long long end = get_time_us();

    double avg_time_us = (double)(end - start) / iterations;
    double data_size_bits = (double)count * sizeof(int) * 8.0;
    double throughput_mbps = data_size_bits / (avg_time_us / 1e6) / 1e6;

    // 验证结果
    int valid = verify_allreduce(recv_buf, count, world_size);

    free(send_buf);
    free(recv_buf);

    return valid ? throughput_mbps : -1.0;
}

// 测试 Reduce (返回吞吐量 Mbps)
static double test_reduce(int rank, int world_size, int count, int iterations, int root) {
    int *send_buf = (int *)malloc(count * sizeof(int));
    int *recv_buf = (int *)malloc(count * sizeof(int));

    for (int i = 0; i < count; i++) {
        send_buf[i] = rank + 1;
    }

    // 预热
    MPI_Reduce(send_buf, recv_buf, count, MPI_INT, MPI_SUM, root, MPI_COMM_WORLD);

    MPI_Barrier(MPI_COMM_WORLD);
    long long start = get_time_us();

    for (int iter = 0; iter < iterations; iter++) {
        MPI_Reduce(send_buf, recv_buf, count, MPI_INT, MPI_SUM, root, MPI_COMM_WORLD);
    }

    MPI_Barrier(MPI_COMM_WORLD);
    long long end = get_time_us();

    double avg_time_us = (double)(end - start) / iterations;
    double data_size_bits = (double)count * sizeof(int) * 8.0;
    double throughput_mbps = data_size_bits / (avg_time_us / 1e6) / 1e6;

    int valid = verify_reduce(recv_buf, count, world_size, rank, root);

    free(send_buf);
    free(recv_buf);

    return valid ? throughput_mbps : -1.0;
}

// 测试 Broadcast (返回吞吐量 Mbps)
static double test_broadcast(int rank, int world_size, int count, int iterations, int root) {
    int *buf = (int *)malloc(count * sizeof(int));

    // 预热
    if (rank == root) {
        for (int i = 0; i < count; i++) buf[i] = root + 1;
    }
    MPI_Bcast(buf, count, MPI_INT, root, MPI_COMM_WORLD);

    MPI_Barrier(MPI_COMM_WORLD);
    long long start = get_time_us();

    for (int iter = 0; iter < iterations; iter++) {
        if (rank == root) {
            for (int i = 0; i < count; i++) buf[i] = root + 1;
        }
        MPI_Bcast(buf, count, MPI_INT, root, MPI_COMM_WORLD);
    }

    MPI_Barrier(MPI_COMM_WORLD);
    long long end = get_time_us();

    double avg_time_us = (double)(end - start) / iterations;
    double data_size_bits = (double)count * sizeof(int) * 8.0;
    double throughput_mbps = data_size_bits / (avg_time_us / 1e6) / 1e6;

    int valid = verify_broadcast(buf, count, root);

    free(buf);

    return valid ? throughput_mbps : -1.0;
}

// 测试 AllGather (返回吞吐量 Mbps)
static double test_allgather(int rank, int world_size, int count, int iterations) {
    int *send_buf = (int *)malloc(count * sizeof(int));
    int *recv_buf = (int *)malloc(count * world_size * sizeof(int));

    for (int i = 0; i < count; i++) {
        send_buf[i] = rank + 1;
    }

    // 预热
    MPI_Allgather(send_buf, count, MPI_INT, recv_buf, count, MPI_INT, MPI_COMM_WORLD);

    MPI_Barrier(MPI_COMM_WORLD);
    long long start = get_time_us();

    for (int iter = 0; iter < iterations; iter++) {
        MPI_Allgather(send_buf, count, MPI_INT, recv_buf, count, MPI_INT, MPI_COMM_WORLD);
    }

    MPI_Barrier(MPI_COMM_WORLD);
    long long end = get_time_us();

    double avg_time_us = (double)(end - start) / iterations;
    double data_size_bits = (double)count * world_size * sizeof(int) * 8.0;
    double throughput_mbps = data_size_bits / (avg_time_us / 1e6) / 1e6;

    int valid = verify_allgather(recv_buf, count, world_size);

    free(send_buf);
    free(recv_buf);

    return valid ? throughput_mbps : -1.0;
}

// 测试 ReduceScatter (返回吞吐量 Mbps)
static double test_reducescatter(int rank, int world_size, int count, int iterations) {
    int *send_buf = (int *)malloc(count * world_size * sizeof(int));
    int *recv_buf = (int *)malloc(count * sizeof(int));
    int *recvcounts = (int *)malloc(world_size * sizeof(int));

    // 每个进程接收 count 个元素
    for (int i = 0; i < world_size; i++) {
        recvcounts[i] = count;
    }

    // 初始化发送数据
    for (int i = 0; i < count * world_size; i++) {
        send_buf[i] = rank + 1;
    }

    // 预热
    MPI_Reduce_scatter(send_buf, recv_buf, recvcounts, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    MPI_Barrier(MPI_COMM_WORLD);
    long long start = get_time_us();

    for (int iter = 0; iter < iterations; iter++) {
        MPI_Reduce_scatter(send_buf, recv_buf, recvcounts, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    }

    MPI_Barrier(MPI_COMM_WORLD);
    long long end = get_time_us();

    double avg_time_us = (double)(end - start) / iterations;
    double data_size_bits = (double)count * sizeof(int) * 8.0;
    double throughput_mbps = data_size_bits / (avg_time_us / 1e6) / 1e6;

    int valid = verify_reducescatter(recv_buf, count, world_size, rank);

    free(send_buf);
    free(recv_buf);
    free(recvcounts);

    return valid ? throughput_mbps : -1.0;
}

// 测试 Barrier (返回 requests/second)
static double test_barrier(int rank, int world_size, int iterations) {
    // 预热
    MPI_Barrier(MPI_COMM_WORLD);

    MPI_Barrier(MPI_COMM_WORLD);
    long long start = get_time_us();

    for (int iter = 0; iter < iterations; iter++) {
        MPI_Barrier(MPI_COMM_WORLD);
    }

    MPI_Barrier(MPI_COMM_WORLD);
    long long end = get_time_us();

    double total_time_s = (double)(end - start) / 1e6;
    double requests_per_sec = (double)iterations / total_time_s;

    return requests_per_sec;
}

static void print_usage(const char *prog) {
    printf("Usage: mpirun -np <N> -hostfile <file> %s [options]\n", prog);
    printf("Options:\n");
    printf("  -s <size>    Data size in KB (default: 1024)\n");
    printf("  -i <iter>    Number of iterations per run (default: 10)\n");
    printf("  -r <runs>    Number of runs to average (default: 1)\n");
    printf("  -t <type>    Test type: all, allreduce, reduce, broadcast, allgather, reducescatter, barrier\n");
}

int main(int argc, char *argv[]) {
    MPI_Init(&argc, &argv);

    // 禁用输出缓冲，确保日志立即输出
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    int rank, world_size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    // 默认参数
    int size_kb = 1024;      // 1 MB
    int iterations = 10;
    int runs = 1;            // 运行次数
    const char *test_type = "all";

    // 解析参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            size_kb = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            iterations = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            runs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            test_type = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0) {
            if (rank == 0) print_usage(argv[0]);
            MPI_Finalize();
            return 0;
        }
    }

    int count = size_kb * 1024 / sizeof(int);

    if (rank == 0) {
        printf("=== MPI Collective Test ===\n");
        printf("World size: %d\n", world_size);
        printf("Data size: %d KB (%d integers)\n", size_kb, count);
        printf("Iterations per run: %d\n", iterations);
        printf("Runs to average: %d\n", runs);
        printf("===========================\n\n");
    }

    MPI_Barrier(MPI_COMM_WORLD);

    double throughput_sum;
    int valid_runs;
    double data_size_kb;

    // AllReduce
    if (strcmp(test_type, "all") == 0 || strcmp(test_type, "allreduce") == 0) {
        throughput_sum = 0.0;
        valid_runs = 0;
        data_size_kb = (double)count * sizeof(int) / 1024.0;
        for (int r = 0; r < runs; r++) {
            double tp = test_allreduce(rank, world_size, count, iterations);
            if (tp > 0) {
                throughput_sum += tp;
                valid_runs++;
            }
        }
        if (rank == 0) {
            if (valid_runs > 0) {
                printf("AllReduce: size=%.1f KB, avg_throughput=%.2f Mbps (%d runs), PASS\n",
                       data_size_kb, throughput_sum / valid_runs, valid_runs);
            } else {
                printf("AllReduce: size=%.1f KB, FAIL\n", data_size_kb);
            }
        }
    }
    // Reduce
    if (strcmp(test_type, "all") == 0 || strcmp(test_type, "reduce") == 0) {
        throughput_sum = 0.0;
        valid_runs = 0;
        data_size_kb = (double)count * sizeof(int) / 1024.0;
        for (int r = 0; r < runs; r++) {
            double tp = test_reduce(rank, world_size, count, iterations, 0);
            if (tp > 0) {
                throughput_sum += tp;
                valid_runs++;
            }
        }
        if (rank == 0) {
            if (valid_runs > 0) {
                printf("Reduce:    size=%.1f KB, avg_throughput=%.2f Mbps (%d runs), PASS\n",
                       data_size_kb, throughput_sum / valid_runs, valid_runs);
            } else {
                printf("Reduce:    size=%.1f KB, FAIL\n", data_size_kb);
            }
        }
    }
    // Broadcast
    if (strcmp(test_type, "all") == 0 || strcmp(test_type, "broadcast") == 0) {
        throughput_sum = 0.0;
        valid_runs = 0;
        data_size_kb = (double)count * sizeof(int) / 1024.0;
        for (int r = 0; r < runs; r++) {
            double tp = test_broadcast(rank, world_size, count, iterations, 0);
            if (tp > 0) {
                throughput_sum += tp;
                valid_runs++;
            }
        }
        if (rank == 0) {
            if (valid_runs > 0) {
                printf("Broadcast: size=%.1f KB, avg_throughput=%.2f Mbps (%d runs), PASS\n",
                       data_size_kb, throughput_sum / valid_runs, valid_runs);
            } else {
                printf("Broadcast: size=%.1f KB, FAIL\n", data_size_kb);
            }
        }
    }
    // AllGather
    if (strcmp(test_type, "all") == 0 || strcmp(test_type, "allgather") == 0) {
        throughput_sum = 0.0;
        valid_runs = 0;
        data_size_kb = (double)count * world_size * sizeof(int) / 1024.0;
        for (int r = 0; r < runs; r++) {
            double tp = test_allgather(rank, world_size, count, iterations);
            if (tp > 0) {
                throughput_sum += tp;
                valid_runs++;
            }
        }
        if (rank == 0) {
            if (valid_runs > 0) {
                printf("AllGather: size=%.1f KB, avg_throughput=%.2f Mbps (%d runs), PASS\n",
                       data_size_kb, throughput_sum / valid_runs, valid_runs);
            } else {
                printf("AllGather: size=%.1f KB, FAIL\n", data_size_kb);
            }
        }
    }
    // ReduceScatter
    if (strcmp(test_type, "all") == 0 || strcmp(test_type, "reducescatter") == 0) {
        throughput_sum = 0.0;
        valid_runs = 0;
        data_size_kb = (double)count * sizeof(int) / 1024.0;
        for (int r = 0; r < runs; r++) {
            double tp = test_reducescatter(rank, world_size, count, iterations);
            if (tp > 0) {
                throughput_sum += tp;
                valid_runs++;
            }
        }
        if (rank == 0) {
            if (valid_runs > 0) {
                printf("ReduceScatter: size=%.1f KB, avg_throughput=%.2f Mbps (%d runs), PASS\n",
                       data_size_kb, throughput_sum / valid_runs, valid_runs);
            } else {
                printf("ReduceScatter: size=%.1f KB, FAIL\n", data_size_kb);
            }
        }
    }
    // Barrier
    if (strcmp(test_type, "all") == 0 || strcmp(test_type, "barrier") == 0) {
        throughput_sum = 0.0;
        valid_runs = 0;
        for (int r = 0; r < runs; r++) {
            double rps = test_barrier(rank, world_size, iterations);
            if (rps > 0) {
                throughput_sum += rps;
                valid_runs++;
            }
        }
        if (rank == 0) {
            if (valid_runs > 0) {
                printf("Barrier:   avg_rate=%.2f req/s (%d runs), PASS\n",
                       throughput_sum / valid_runs, valid_runs);
            } else {
                printf("Barrier:   FAIL\n");
            }
        }
    }

    MPI_Finalize();
    return 0;
}
