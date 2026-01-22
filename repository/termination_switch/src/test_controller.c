/**
 * @file test_controller.c
 * @brief 简单的控制器测试程序
 *
 * 用于测试交换机的 controller 通信功能
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define CONTROLLER_SWITCH_PORT 52311

// 控制器指令
#define CMD_STALL 1
#define CMD_RESET 2
#define CMD_RESET_PSN 3
#define CMD_SET_CONNECTIONS 4
#define CMD_SET_RULE 5
#define CMD_START 6

int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    printf("=== Test Controller ===\n");

    // 创建监听socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        return 1;
    }

    // 设置地址重用
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 绑定地址
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(CONTROLLER_SWITCH_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_fd);
        return 1;
    }

    // 监听
    if (listen(server_fd, 5) < 0) {
        perror("Listen failed");
        close(server_fd);
        return 1;
    }

    printf("Controller listening on port %d...\n", CONTROLLER_SWITCH_PORT);

    // 接受连接
    client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
        perror("Accept failed");
        close(server_fd);
        return 1;
    }

    printf("Switch connected from %s:%d\n",
           inet_ntoa(client_addr.sin_addr),
           ntohs(client_addr.sin_port));

    // 发送测试指令
    sleep(1);

    printf("\nSending STALL command...\n");
    uint8_t cmd = CMD_STALL;
    send(client_fd, &cmd, sizeof(cmd), 0);
    sleep(1);

    printf("Sending RESET_PSN command...\n");
    cmd = CMD_RESET_PSN;
    send(client_fd, &cmd, sizeof(cmd), 0);
    sleep(1);

    printf("Sending START command...\n");
    cmd = CMD_START;
    send(client_fd, &cmd, sizeof(cmd), 0);
    sleep(1);

    printf("\nTest complete. Press Ctrl+C to exit.\n");

    // 保持连接
    while (1) {
        sleep(1);
    }

    close(client_fd);
    close(server_fd);
    return 0;
}
