/**
 * @file controller_comm.c
 * @brief 交换机与控制器通信模块实现
 */

#include "controller_comm.h"
#include "parameter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/**
 * @brief 后台接收线程函数（前向声明）
 */
extern void *background_receiving(void *arg);

/**
 * @brief 生成所有路由规则
 *
 * 根据拓扑信息生成 AllReduce、Reduce(所有rank)、Broadcast(所有rank) 的规则
 *
 * @param ctx 交换机上下文
 * @param num_ranks 总rank数量
 * @return 0表示成功，-1表示失败
 */
static int generate_all_rules(switch_context_t *ctx, int num_ranks) {
    printf("[Rules] Generating routing rules for %d ranks...\n", num_ranks);

    // TODO: 实现规则生成逻辑
    // 1. 生成 AllReduce 规则
    //    - 上行：所有 host -> switch 的路由
    //    - 下行：switch -> 所有 host 的路由

    // 2. 为每个 rank 生成 Reduce 规则
    //    - 上行：所有 host -> switch -> root
    //    - 下行：只有 root 接收结果

    // 3. 为每个 rank 生成 Broadcast 规则
    //    - 上行：source rank -> switch
    //    - 下行：switch -> 所有其他 host

    printf("[Rules] Generated rules for:\n");
    printf("  - AllReduce\n");
    printf("  - Reduce (destinations: rank 0 to %d)\n", num_ranks - 1);
    printf("  - Broadcast (sources: rank 0 to %d)\n", num_ranks - 1);

    return 0;
}

/**
 * @brief 启动接收线程
 */
static int start_receiver_threads(switch_context_t *ctx) {
    int num_receivers = ctx->num_receivers;

    if (num_receivers <= 0) {
        fprintf(stderr, "[Receiver] Invalid num_receivers: %d\n", num_receivers);
        return -1;
    }

    printf("[Receiver] Starting %d receiver threads...\n", num_receivers);

    for (int i = 0; i < num_receivers; i++) {
        // 创建接收线程，传递上下文指针
        if (pthread_create(&ctx->receiver_threads[i], NULL, background_receiving, ctx) != 0) {
            fprintf(stderr, "[Receiver] Failed to create receiver thread %d\n", i);

            // 清理已创建的线程
            for (int j = 0; j < i; j++) {
                pthread_cancel(ctx->receiver_threads[j]);
                pthread_join(ctx->receiver_threads[j], NULL);
            }
            return -1;
        }
    }

    printf("[Receiver] All %d receiver threads started\n", num_receivers);

    return 0;
}

/**
 * @brief 连接到控制器
 */
int controller_connect(switch_context_t *ctx, const char *controller_ip, int controller_port) {
    int sockfd;
    struct sockaddr_in controller_addr;

    printf("[Controller] Connecting to %s:%d...\n", controller_ip, controller_port);

    // 创建TCP客户端套接字
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("[Controller] Socket creation failed");
        return -1;
    }

    // 配置控制器地址信息
    memset(&controller_addr, 0, sizeof(controller_addr));
    controller_addr.sin_family = AF_INET;
    controller_addr.sin_port = htons(controller_port);
    if (inet_pton(AF_INET, controller_ip, &controller_addr.sin_addr) <= 0) {
        perror("[Controller] Invalid IP address");
        close(sockfd);
        return -1;
    }

    // 连接控制器
    if (connect(sockfd, (struct sockaddr *)&controller_addr, sizeof(controller_addr)) < 0) {
        perror("[Controller] Connection failed");
        close(sockfd);
        return -1;
    }

    printf("[Controller] Connected successfully\n");

    // 保存文件描述符
    pthread_mutex_lock(&ctx->state_mutex);
    ctx->controller_fd = sockfd;
    pthread_mutex_unlock(&ctx->state_mutex);

    return 0;
}

/**
 * @brief 处理 STALL 指令
 */
int handle_cmd_stall(switch_context_t *ctx) {
    printf("[Controller] Received STALL command\n");

    pthread_mutex_lock(&ctx->state_mutex);
    ctx->state = SWITCH_STATE_STALLED;
    pthread_mutex_unlock(&ctx->state_mutex);

    printf("[Controller] Switch is now STALLED\n");
    return 0;
}

/**
 * @brief 处理 RESET 指令
 */
int handle_cmd_reset(switch_context_t *ctx) {
    printf("[Controller] Received RESET command\n");

    // 重置PSN状态
    switch_context_reset_psn_states(ctx);

    // 清空路由表
    pthread_mutex_lock(&ctx->state_mutex);
    memset(&ctx->routing_table, 0, sizeof(ctx->routing_table));
    ctx->routing_table.count = 0;
    memset(ctx->rank_to_conn, -1, sizeof(ctx->rank_to_conn));
    pthread_mutex_unlock(&ctx->state_mutex);

    // 清空连接信息
    memset(ctx->conns, 0, sizeof(ctx->conns));
    ctx->fan_in = 0;
    ctx->is_root = 0;

    printf("[Controller] Reset complete\n");
    return 0;
}

/**
 * @brief 处理 RESET_PSN 指令
 */
int handle_cmd_reset_psn(switch_context_t *ctx) {
    printf("[Controller] Received RESET_PSN command\n");

    // 仅重置PSN状态
    switch_context_reset_psn_states(ctx);

    printf("[Controller] PSN reset complete\n");
    return 0;
}

/**
 * @brief 处理 SET_CONNECTIONS 指令
 */

int handle_cmd_set_connections(switch_context_t *ctx, int sockfd) {
    printf("[Controller] Received SET_CONNECTIONS command\n");

    // 1. 接收YAML文件大小（4字节）
    uint32_t yaml_size;
    ssize_t bytes = recv(sockfd, &yaml_size, sizeof(yaml_size), MSG_WAITALL);
    if (bytes != sizeof(yaml_size)) {
        fprintf(stderr, "[Controller] Failed to receive YAML size\n");
        return -1;
    }
    yaml_size = ntohl(yaml_size);  // 网络字节序转主机字节序

    printf("[Controller] YAML size: %u bytes\n", yaml_size);

    // 2. 接收YAML内容
    char *yaml_buffer = malloc(yaml_size + 1);
    if (!yaml_buffer) {
        fprintf(stderr, "[Controller] Failed to allocate YAML buffer\n");
        return -1;
    }

    bytes = recv(sockfd, yaml_buffer, yaml_size, MSG_WAITALL);
    if (bytes != yaml_size) {
        fprintf(stderr, "[Controller] Failed to receive YAML content\n");
        free(yaml_buffer);
        return -1;
    }
    yaml_buffer[yaml_size] = '\0';

    printf("[Controller] Received YAML content:\n%s\n", yaml_buffer);

    // 3. TODO: 解析YAML并配置连接
    // parse_topology_yaml(yaml_buffer, ctx);
    // 解析后需要设置：
    //   - ctx->conns[]: 连接配置数组
    //   - ctx->num_receivers: 接收线程数量
    //   - ctx->is_root: 是否为根节点
    //   - ctx->fan_in: 扇入数量（用于显示）
    //   - num_ranks: 总rank数量（用于生成规则）

    int num_ranks = 4;  // TODO: 从YAML中解析

    free(yaml_buffer);

    // 4. 生成所有路由规则
    if (generate_all_rules(ctx, num_ranks) < 0) {
        fprintf(stderr, "[Controller] Failed to generate routing rules\n");
        return -1;
    }

    // 5. 启动接收线程（在配置完成后）
    if (start_receiver_threads(ctx) < 0) {
        fprintf(stderr, "[Controller] Failed to start receiver threads\n");
        return -1;
    }

    printf("[Controller] SET_CONNECTIONS complete\n");
    return 0;
}

/**
 * @brief 处理 START 指令
 */
int handle_cmd_start(switch_context_t *ctx) {
    printf("[Controller] Received START command\n");

    pthread_mutex_lock(&ctx->state_mutex);
    ctx->state = SWITCH_STATE_RUNNING;
    pthread_mutex_unlock(&ctx->state_mutex);

    printf("[Controller] Switch is now RUNNING\n");
    return 0;
}

/**
 * @brief 控制器通信线程
 */
void *controller_thread(void *arg) {
    switch_context_t *ctx = (switch_context_t *)arg;
    int sockfd = ctx->controller_fd;

    printf("[Controller] Communication thread started\n");

    while (1) {
        // 接收指令（1字节）
        uint8_t cmd;
        ssize_t bytes = recv(sockfd, &cmd, sizeof(cmd), 0);

        if (bytes <= 0) {
            if (bytes == 0) {
                printf("[Controller] Connection closed by controller\n");
            } else {
                perror("[Controller] recv failed");
            }
            break;
        }

        printf("[Controller] Received command: %d\n", cmd);

        // 处理指令
        switch (cmd) {
            case CMD_STALL:
                handle_cmd_stall(ctx);
                break;

            case CMD_RESET:
                handle_cmd_reset(ctx);
                break;

            case CMD_RESET_PSN:
                handle_cmd_reset_psn(ctx);
                break;

            case CMD_SET_CONNECTIONS:
                handle_cmd_set_connections(ctx, sockfd);
                break;

            case CMD_START:
                handle_cmd_start(ctx);
                break;

            default:
                fprintf(stderr, "[Controller] Unknown command: %d\n", cmd);
                break;
        }
    }

    printf("[Controller] Communication thread exited\n");
    return NULL;
}
