/**
 * @file controller_comm.cpp
 * @brief 交换机与控制器通信模块实现（简化版）
 *
 * 简化协议：连接后直接接收 YAML 配置
 * 格式：4字节长度(网络字节序) + YAML内容
 */

#include <yaml-cpp/yaml.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string>

extern "C" {
#include "controller_comm.h"
#include "parameter.h"
}

// ============================================
// YAML 解析函数 (使用 yaml-cpp)
// ============================================

/**
 * @brief 解析连接配置
 */
static int parse_connections(switch_context_t *ctx, const YAML::Node& connections) {
    if (!connections || !connections.IsSequence()) {
        printf("[YAML] No connections found or invalid format\n");
        return -1;
    }

    int conn_idx = 0;
    int host_conn_count = 0;  // 计算主机连接数
    for (const auto& conn_node : connections) {
        if (conn_idx >= MAX_CONNECTIONS_NUM) {
            printf("[YAML] Warning: Too many connections, max is %d\n", MAX_CONNECTIONS_NUM);
            break;
        }

        connection_t *conn = &ctx->conns[conn_idx];

        // 解析连接字段
        int conn_id = conn_node["conn_id"].as<int>(conn_idx);
        bool is_switch = conn_node["is_switch"].as<bool>(false);
        int peer_id = conn_node["peer_id"].as<int>(-1);

        std::string my_ip = conn_node["my_ip"].as<std::string>("");
        std::string my_mac = conn_node["my_mac"].as<std::string>("");
        std::string my_name = conn_node["my_name"].as<std::string>("");
        int my_port = conn_node["my_port"].as<int>(4791);
        int my_qp = conn_node["my_qp"].as<int>(0);

        std::string peer_ip = conn_node["peer_ip"].as<std::string>("");
        std::string peer_mac = conn_node["peer_mac"].as<std::string>("");
        int peer_port = conn_node["peer_port"].as<int>(4791);
        int peer_qp = conn_node["peer_qp"].as<int>(0);

        // 填充 connection_t 结构
        strncpy(conn->device, my_name.c_str(), sizeof(conn->device) - 1);
        conn->device[sizeof(conn->device) - 1] = '\0';

        conn->my_ip = inet_addr(my_ip.c_str());
        conn->peer_ip = inet_addr(peer_ip.c_str());
        conn->my_port = my_port;
        conn->peer_port = peer_port;
        conn->my_qp = my_qp;
        conn->peer_qp = peer_qp;
        conn->psn = 0;
        conn->msn = 0;
        conn->ok = 1;
        conn->is_switch = is_switch ? 1 : 0;
        conn->peer_id = peer_id;

        // 解析 MAC 地址
        if (!my_mac.empty()) {
            sscanf(my_mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &conn->my_mac[0], &conn->my_mac[1], &conn->my_mac[2],
                   &conn->my_mac[3], &conn->my_mac[4], &conn->my_mac[5]);
        }
        if (!peer_mac.empty()) {
            sscanf(peer_mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &conn->peer_mac[0], &conn->peer_mac[1], &conn->peer_mac[2],
                   &conn->peer_mac[3], &conn->peer_mac[4], &conn->peer_mac[5]);
        }

        // 如果对端是主机，建立 rank_to_conn 映射
        if (!is_switch && peer_id >= 0 && peer_id < MAX_RANKS) {
            ctx->rank_to_conn[peer_id] = conn_id;
            // 记录本地 Host 连接（用于 ReduceScatter 分散）
            if (host_conn_count < MAX_CONNECTIONS_NUM) {
                ctx->host_conns[host_conn_count] = conn_id;
            }
            host_conn_count++;  // 统计主机连接数
        }

        printf("[YAML] Connection %d: %s (%s) <-> %s, is_switch=%d, peer_id=%d\n",
               conn_id, my_name.c_str(), my_ip.c_str(), peer_ip.c_str(), is_switch, peer_id);

        conn_idx++;
    }

    ctx->fan_in = conn_idx;
    ctx->host_fan_in = host_conn_count;  // 设置主机连接数
    ctx->host_count = host_conn_count;   // 设置本地 Host 数量
    printf("[YAML] Total connections parsed: %d (host: %d)\n", conn_idx, host_conn_count);
    // 打印 host_conns 数组
    printf("[YAML] host_conns: ");
    for (int i = 0; i < host_conn_count; i++) {
        printf("%d ", ctx->host_conns[i]);
    }
    printf("\n");
    return 0;
}

/**
 * @brief 解析规则配置
 */
static int parse_rules(switch_context_t *ctx, const YAML::Node& rules) {
    if (!rules || !rules.IsSequence()) {
        printf("[YAML] No rules found or invalid format\n");
        return 0;  // 没有规则不算错误
    }

    for (const auto& rule_node : rules) {
        if (ctx->routing_table.count >= MAX_RULES) {
            printf("[YAML] Warning: Too many rules, max is %d\n", MAX_RULES);
            break;
        }

        rule_t rule;
        memset(&rule, 0, sizeof(rule));

        // 解析规则字段
        std::string src_ip = rule_node["src_ip"].as<std::string>("");
        std::string dst_ip = rule_node["dst_ip"].as<std::string>("");

        rule.src_ip = inet_addr(src_ip.c_str());
        rule.dst_ip = inet_addr(dst_ip.c_str());
        rule.primitive = (primitive_type_t)rule_node["primitive"].as<int>(0);
        rule.primitive_param = rule_node["primitive_param"].as<int>(-1);
        rule.conn_id = rule_node["conn_id"].as<int>(0);
        rule.direction = rule_node["direction"].as<int>(0);
        rule.root = rule_node["root"].as<bool>(false) ? 1 : 0;

        // 解析 ack_conn
        int ack_conn_id = rule_node["ack_conn"].as<int>(-1);
        if (ack_conn_id >= 0 && ack_conn_id < MAX_CONNECTIONS_NUM) {
            rule.ack_conn = &ctx->conns[ack_conn_id];
        }

        // 解析 out_conns 数组
        if (rule_node["out_conns"] && rule_node["out_conns"].IsSequence()) {
            int out_idx = 0;
            for (const auto& out_conn : rule_node["out_conns"]) {
                if (out_idx >= MAX_PORT_NUM) break;
                int conn_id = out_conn.as<int>(-1);
                if (conn_id >= 0 && conn_id < MAX_CONNECTIONS_NUM) {
                    rule.out_conns[out_idx] = &ctx->conns[conn_id];
                    out_idx++;
                }
            }
            rule.out_conns_cnt = out_idx;
        }

        // 添加到路由表
        ctx->routing_table.rules[ctx->routing_table.count] = rule;

        printf("[YAML] Rule %d: %s -> %s, primitive=%d, param=%d, dir=%s, root=%d, out_cnt=%d\n",
               ctx->routing_table.count, src_ip.c_str(), dst_ip.c_str(),
               rule.primitive, rule.primitive_param,
               rule.direction == DIR_UP ? "UP" : "DOWN",
               rule.root, rule.out_conns_cnt);

        ctx->routing_table.count++;
    }

    printf("[YAML] Total rules parsed: %d\n", ctx->routing_table.count);
    return 0;
}

/**
 * @brief 解析 YAML 配置并填充到上下文
 */
static int parse_yaml_config(switch_context_t *ctx, const char *yaml_buffer) {
    printf("[YAML] ========== Parsing YAML ==========\n");

    try {
        YAML::Node root = YAML::Load(yaml_buffer);

        // 1. 解析 switch 配置
        YAML::Node switch_node = root["switch"];
        if (!switch_node) {
            fprintf(stderr, "[YAML] Error: 'switch' section not found\n");
            return -1;
        }

        // 解析 switch id
        if (switch_node["id"]) {
            ctx->switch_id = switch_node["id"].as<int>();
            printf("[YAML] switch_id: %d\n", ctx->switch_id);
        }

        // 解析 is_root 标志
        if (switch_node["is_root"]) {
            ctx->is_root = switch_node["is_root"].as<bool>() ? 1 : 0;
            printf("[YAML] is_root: %d\n", ctx->is_root);
        } else {
            ctx->is_root = 0;  // 默认为非根交换机
            printf("[YAML] is_root not specified, defaulting to 0\n");
        }

        // 2. 清空旧的连接和规则
        memset(ctx->conns, 0, sizeof(ctx->conns));
        ctx->fan_in = 0;
        memset(&ctx->routing_table, 0, sizeof(ctx->routing_table));
        ctx->routing_table.count = 0;
        memset(ctx->rank_to_conn, -1, sizeof(ctx->rank_to_conn));

        // 3. 重置 PSN 状态（重要！每次收到新配置都要重置）
        switch_context_reset_psn_states(ctx);

        // 3. 解析 connections
        printf("[YAML] Parsing connections...\n");
        if (parse_connections(ctx, switch_node["connections"]) < 0) {
            fprintf(stderr, "[YAML] Error: Failed to parse connections\n");
            return -1;
        }

        // 4. 解析 rules
        printf("[YAML] Parsing rules...\n");
        if (parse_rules(ctx, switch_node["rules"]) < 0) {
            fprintf(stderr, "[YAML] Error: Failed to parse rules\n");
            return -1;
        }

        printf("[YAML] ========== YAML parsing complete ==========\n");
        return 0;

    } catch (const YAML::Exception& e) {
        fprintf(stderr, "[YAML] YAML parsing error: %s\n", e.what());
        return -1;
    }
}

// ============================================
// C 接口函数
// ============================================

extern "C" {

/**
 * @brief 连接到控制器
 */
int controller_connect(switch_context_t *ctx, const char *controller_ip, int controller_port) {
    int sockfd;
    struct sockaddr_in controller_addr;

    printf("[Controller] Connecting to %s:%d...\n", controller_ip, controller_port);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("[Controller] Socket creation failed");
        return -1;
    }

    memset(&controller_addr, 0, sizeof(controller_addr));
    controller_addr.sin_family = AF_INET;
    controller_addr.sin_port = htons(controller_port);
    if (inet_pton(AF_INET, controller_ip, &controller_addr.sin_addr) <= 0) {
        perror("[Controller] Invalid IP address");
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&controller_addr, sizeof(controller_addr)) < 0) {
        perror("[Controller] Connection failed");
        close(sockfd);
        return -1;
    }

    printf("[Controller] Connected successfully\n");

    ctx->controller_fd = sockfd;

    return 0;
}

/**
 * @brief 接收一次 YAML 配置
 *
 * 协议：4字节长度(网络字节序) + YAML内容
 *
 * @param ctx 交换机上下文
 * @param sockfd socket 文件描述符
 * @return 0表示成功，-1表示失败，1表示连接关闭
 */
static int receive_yaml_config(switch_context_t *ctx, int sockfd) {
    // 1. 接收 YAML 文件大小（4字节）
    uint32_t yaml_size;
    ssize_t bytes = recv(sockfd, &yaml_size, sizeof(yaml_size), MSG_WAITALL);
    if (bytes <= 0) {
        if (bytes == 0) {
            printf("[Controller] Connection closed\n");
            return 1;
        }
        perror("[Controller] Failed to receive YAML size");
        return -1;
    }
    yaml_size = ntohl(yaml_size);

    printf("[Controller] YAML size: %u bytes\n", yaml_size);

    // 2. 接收 YAML 内容
    char *yaml_buffer = (char *)malloc(yaml_size + 1);
    if (!yaml_buffer) {
        fprintf(stderr, "[Controller] Failed to allocate YAML buffer\n");
        return -1;
    }

    bytes = recv(sockfd, yaml_buffer, yaml_size, MSG_WAITALL);
    if (bytes != (ssize_t)yaml_size) {
        fprintf(stderr, "[Controller] Failed to receive YAML content\n");
        free(yaml_buffer);
        return -1;
    }
    yaml_buffer[yaml_size] = '\0';

    printf("[Controller] Received YAML:\n%s\n", yaml_buffer);

    // 3. 解析 YAML
    if (parse_yaml_config(ctx, yaml_buffer) < 0) {
        fprintf(stderr, "[Controller] Failed to parse YAML config\n");
        free(yaml_buffer);
        return -1;
    }

    free(yaml_buffer);

    // 4. 打印路由表信息
    printf("[Controller] Routing table loaded: %d rules, %d connections\n",
           ctx->routing_table.count, ctx->fan_in);

    return 0;
}

/**
 * @brief 控制器通信线程（简化版）
 *
 * 持续接收 YAML 配置，每次收到新配置就更新
 */
void *controller_thread(void *arg) {
    switch_context_t *ctx = (switch_context_t *)arg;
    int sockfd = ctx->controller_fd;

    printf("[Controller] Communication thread started, waiting for YAML...\n");

    while (1) {
        int ret = receive_yaml_config(ctx, sockfd);
        if (ret == 1) {
            // 连接关闭
            break;
        } else if (ret < 0) {
            // 错误，继续尝试
            fprintf(stderr, "[Controller] Error receiving config, retrying...\n");
            continue;
        }

        // 成功接收配置，设置 num_receivers 以便主线程启动接收
        ctx->num_receivers = ctx->fan_in;
        printf("[Controller] Config applied, num_receivers=%d\n", ctx->num_receivers);
    }

    printf("[Controller] Communication thread exited\n");
    return NULL;
}

} // extern "C"
