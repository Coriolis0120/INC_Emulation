/**
 * @file test_controller.c
 * @brief 简单的控制器测试程序
 *
 * 用于测试交换机的 controller 通信功能，包括YAML配置发送
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
#define CMD_START 5

/**
 * @brief 生成测试用的YAML配置
 *
 * 硬编码的拓扑配置，用于测试
 */
static const char* generate_test_yaml() {
    static const char yaml[] =
        "world_size: 2\n"
        "switch:\n"
        "  id: 0\n"
        "  connections:\n"
        "    - conn_id: 0\n"
        "      is_switch: false\n"
        "      peer_id: 0\n"
        "      my_ip: \"192.168.1.1\"\n"
        "      my_mac: \"00:11:22:33:44:55\"\n"
        "      my_name: \"eth0\"\n"
        "      my_port: 4791\n"
        "      my_qp: 100\n"
        "      peer_ip: \"192.168.1.10\"\n"
        "      peer_mac: \"fa:16:3e:87:09:4c\"\n"
        "      peer_port: 4791\n"
        "      peer_qp: 200\n"
        "    - conn_id: 1\n"
        "      is_switch: false\n"
        "      peer_id: 1\n"
        "      my_ip: \"192.168.1.2\"\n"
        "      my_mac: \"00:11:22:33:44:56\"\n"
        "      my_name: \"eth1\"\n"
        "      my_port: 4791\n"
        "      my_qp: 101\n"
        "      peer_ip: \"192.168.1.11\"\n"
        "      peer_mac: \"fa:16:3e:7b:25:36\"\n"
        "      peer_port: 4791\n"
        "      peer_qp: 201\n"
        "  rules:\n"
        "    - src_ip: \"192.168.1.10\"\n"
        "      dst_ip: \"192.168.1.1\"\n"
        "      primitive: 1\n"
        "      primitive_param: -1\n"
        "      conn_id: 0\n"
        "      direction: 0\n"
        "      root: true\n"
        "      ack_conn: 0\n"
        "      out_conns: [0, 1]\n"
        "    - src_ip: \"192.168.1.11\"\n"
        "      dst_ip: \"192.168.1.2\"\n"
        "      primitive: 1\n"
        "      primitive_param: -1\n"
        "      conn_id: 1\n"
        "      direction: 0\n"
        "      root: true\n"
        "      ack_conn: 1\n"
        "      out_conns: [0, 1]\n"
        "    - src_ip: \"192.168.1.10\"\n"
        "      dst_ip: \"192.168.1.1\"\n"
        "      primitive: 2\n"
        "      primitive_param: 0\n"
        "      conn_id: 0\n"
        "      direction: 0\n"
        "      root: true\n"
        "      ack_conn: 0\n"
        "      out_conns: [0]\n"
        "    - src_ip: \"192.168.1.11\"\n"
        "      dst_ip: \"192.168.1.2\"\n"
        "      primitive: 2\n"
        "      primitive_param: 0\n"
        "      conn_id: 1\n"
        "      direction: 0\n"
        "      root: true\n"
        "      ack_conn: 1\n"
        "      out_conns: [0]\n"
        "    - src_ip: \"192.168.1.10\"\n"
        "      dst_ip: \"192.168.1.1\"\n"
        "      primitive: 2\n"
        "      primitive_param: 1\n"
        "      conn_id: 0\n"
        "      direction: 0\n"
        "      root: true\n"
        "      ack_conn: 0\n"
        "      out_conns: [1]\n"
        "    - src_ip: \"192.168.1.11\"\n"
        "      dst_ip: \"192.168.1.2\"\n"
        "      primitive: 2\n"
        "      primitive_param: 1\n"
        "      conn_id: 1\n"
        "      direction: 0\n"
        "      root: true\n"
        "      ack_conn: 1\n"
        "      out_conns: [1]\n"
        "    - src_ip: \"192.168.1.10\"\n"
        "      dst_ip: \"192.168.1.1\"\n"
        "      primitive: 3\n"
        "      primitive_param: 0\n"
        "      conn_id: 0\n"
        "      direction: 0\n"
        "      root: true\n"
        "      ack_conn: 0\n"
        "      out_conns: [1]\n"
        "    - src_ip: \"192.168.1.11\"\n"
        "      dst_ip: \"192.168.1.2\"\n"
        "      primitive: 3\n"
        "      primitive_param: 1\n"
        "      conn_id: 1\n"
        "      direction: 0\n"
        "      root: true\n"
        "      ack_conn: 1\n"
        "      out_conns: [0]\n";

    return yaml;
}

/**
 * @brief 发送YAML配置到交换机
 */
static int send_yaml_config(int sockfd) {
    const char *yaml = generate_test_yaml();
    size_t yaml_len = strlen(yaml);

    printf("\n========== Sending YAML Configuration ==========\n");
    printf("YAML size: %zu bytes\n", yaml_len);
    printf("YAML content:\n%s\n", yaml);

    // 1. 发送命令字节
    uint8_t cmd = CMD_SET_CONNECTIONS;
    if (send(sockfd, &cmd, sizeof(cmd), 0) != sizeof(cmd)) {
        perror("Failed to send command");
        return -1;
    }
    printf("Sent CMD_SET_CONNECTIONS command\n");

    // 2. 发送YAML长度 (网络字节序)
    uint32_t yaml_size = htonl(yaml_len);
    if (send(sockfd, &yaml_size, sizeof(yaml_size), 0) != sizeof(yaml_size)) {
        perror("Failed to send YAML size");
        return -1;
    }
    printf("Sent YAML size: %zu\n", yaml_len);

    // 3. 发送YAML内容
    size_t total_sent = 0;
    while (total_sent < yaml_len) {
        ssize_t sent = send(sockfd, yaml + total_sent, yaml_len - total_sent, 0);
        if (sent <= 0) {
            perror("Failed to send YAML content");
            return -1;
        }
        total_sent += sent;
    }
    printf("Sent YAML content successfully\n");
    printf("========== YAML Configuration Sent ==========\n\n");

    return 0;
}

int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    printf("=== Test Controller (YAML Version) ===\n\n");

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
    printf("Waiting for switch connection...\n\n");

    // 接受连接
    client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
        perror("Accept failed");
        close(server_fd);
        return 1;
    }

    printf("========================================\n");
    printf("Switch connected from %s:%d\n",
           inet_ntoa(client_addr.sin_addr),
           ntohs(client_addr.sin_port));
    printf("========================================\n\n");

    // 等待一下让交换机准备好
    sleep(1);

    // 发送YAML配置
    if (send_yaml_config(client_fd) < 0) {
        printf("Failed to send YAML config\n");
        close(client_fd);
        close(server_fd);
        return 1;
    }

    // 等待交换机处理
    sleep(2);

    // 发送START命令
    printf("Sending START command...\n");
    uint8_t cmd = CMD_START;
    send(client_fd, &cmd, sizeof(cmd), 0);
    sleep(1);

    printf("\n========================================\n");
    printf("Test complete!\n");
    printf("The switch should have parsed the YAML and printed:\n");
    printf("  - 2 connections\n");
    printf("  - 8 rules (2 AllReduce + 4 Reduce + 2 Broadcast)\n");
    printf("========================================\n");

    printf("\nPress Ctrl+C to exit.\n");

    // 保持连接
    while (1) {
        sleep(1);
    }

    close(client_fd);
    close(server_fd);
    return 0;
}
