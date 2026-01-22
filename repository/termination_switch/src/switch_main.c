/**
 * @file switch_main.c
 * @brief INC 交换机主程序（重构版本）
 *
 * 重构目标：
 * - 使用 switch_context_t 替代全局变量
 * - 模块化设计，便于维护和扩展
 * - 保持与原版本功能完全一致
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "switch_context.h"
#include "controller_comm.h"
#include "parameter.h"

// 全局上下文（静态分配）
static switch_context_t g_switch_ctx;

/**
 * @brief 连接到控制器并启动通信线程
 *
 * @param ctx 交换机上下文
 * @param controller_ip 控制器IP地址
 * @return 0表示成功，-1表示失败
 */
static int controller_init(switch_context_t *ctx, const char *controller_ip) {
    printf("Connecting to controller...\n");

    // 连接到控制器
    if (controller_connect(ctx, controller_ip, CONTROLLER_SWITCH_PORT) < 0) {
        fprintf(stderr, "Failed to connect to controller\n");
        return -1;
    }

    // 启动控制器通信线程
    pthread_t controller_tid;
    if (pthread_create(&controller_tid, NULL, controller_thread, ctx) != 0) {
        fprintf(stderr, "Failed to create controller thread\n");
        return -1;
    }
    pthread_detach(controller_tid);  // 分离线程，自动回收资源

    printf("Controller connection established\n");
    return 0;
}

/**
 * @brief 后台接收线程
 *
 * @param arg 交换机上下文指针 (switch_context_t*)
 * @return NULL
 */
void *background_receiving(void *arg) {
    switch_context_t *ctx = (switch_context_t *)arg;

    printf("Receiving thread started\n");

    // TODO: 实现数据包接收逻辑
    // 每个线程需要知道自己负责哪个连接
    // 可以通过线程局部存储或其他方式确定

    return NULL;
}

/**
 * @brief 主函数
 */
int main(int argc, char *argv[]) {
    char *controller_ip = "192.168.0.3";  // 默认控制器IP

    // 支持命令行参数
    if (argc >= 2) {
        controller_ip = argv[1];
    }

    printf("=== INC Switch (Refactored Version) ===\n");
    printf("Controller IP: %s\n", controller_ip);

    // 初始化交换机上下文
    if (switch_context_init(&g_switch_ctx, 0, 4) < 0) {
        fprintf(stderr, "Failed to initialize switch context\n");
        return 1;
    }

    // 连接到控制器
    if (controller_init(&g_switch_ctx, controller_ip) < 0) {
        fprintf(stderr, "Failed to connect to controller\n");
        switch_context_cleanup(&g_switch_ctx);
        return 1;
    }

    printf("Switch started successfully\n");
    printf("Waiting for controller commands...\n");

    // 等待所有接收线程结束（由 controller 的 SET_CONNECTIONS 指令创建）
    // 注意：只有在接收到 SET_CONNECTIONS 后才会有接收线程
    while (g_switch_ctx.num_receivers == 0) {
        sleep(1);  // 等待接收线程被创建
    }

    for (int i = 0; i < g_switch_ctx.num_receivers; i++) {
        pthread_join(g_switch_ctx.receiver_threads[i], NULL);
    }

    // 清理资源
    switch_context_cleanup(&g_switch_ctx);

    printf("Switch stopped\n");
    return 0;
}
