#include <yaml-cpp/yaml.h>
#include <vector>
#include <fstream>
#include <sstream>
#include <string>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <endian.h>
#include <byteswap.h>
#include <stdbool.h>
#include <getopt.h>
#include <sys/time.h>
#include <map>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <thread>
#include "parameter.h"


struct switch_port{
    std::string name;
    std::string ip;
    std::string mac;
};

struct switch_info{
    int fd;
    int id;
    std::string control_ip;
    std::vector<switch_port> ports;
    //maybe other info to add
};


extern switch_info switch_topology[TOPOLOGY_SIZE];

// ============================================
// 连接配置结构体
// ============================================
struct ConnectionConfig {
    int conn_id;                  // 连接 ID
    bool is_switch;               // 对端是否是交换机 (true=交换机, false=主机)
    int peer_id;                  // 对端 ID (is_switch=true 时为交换机 id, false 时为 rank)
    std::string my_ip;
    std::string my_mac;
    std::string my_name;
    int my_port;
    int my_qp;
    std::string peer_ip;
    std::string peer_mac;
    int peer_port;
    int peer_qp;
};

// ============================================
// 规则配置结构体
// ============================================
struct RuleConfig {
    std::string src_ip;           // 匹配：源 IP
    std::string dst_ip;           // 匹配：目的 IP
    int primitive;                // 原语类型: 1=AllReduce, 2=Reduce, 3=Broadcast
    int primitive_param;          // 原语参数: root_rank/sender_rank, AllReduce 时为 -1
    int conn_id;                  // 入连接 ID
    int direction;                // 0=DIR_UP, 1=DIR_DOWN
    bool root;                    // 当前交换机是否是这条规则的根
    int ack_conn;                 // ACK 发给哪个连接
    std::vector<int> out_conns;   // 转发给哪些连接
};

// ============================================
// 交换机配置结构体
// ============================================
struct SwitchConfig {
    int id;
    std::vector<ConnectionConfig> connections;
    std::vector<RuleConfig> rules;
};

// ============================================
// YAML 序列化适配器
// ============================================
namespace YAML {
template<>
struct convert<ConnectionConfig> {
    static Node encode(const ConnectionConfig& conn) {
        Node node;
        node["conn_id"] = conn.conn_id;
        node["is_switch"] = conn.is_switch;
        node["peer_id"] = conn.peer_id;
        node["my_ip"] = conn.my_ip;
        node["my_mac"] = conn.my_mac;
        node["my_name"] = conn.my_name;
        node["my_port"] = conn.my_port;
        node["my_qp"] = conn.my_qp;
        node["peer_ip"] = conn.peer_ip;
        node["peer_mac"] = conn.peer_mac;
        node["peer_port"] = conn.peer_port;
        node["peer_qp"] = conn.peer_qp;
        return node;
    }
};

template<>
struct convert<RuleConfig> {
    static Node encode(const RuleConfig& rule) {
        Node node;
        node["src_ip"] = rule.src_ip;
        node["dst_ip"] = rule.dst_ip;
        node["primitive"] = rule.primitive;
        node["primitive_param"] = rule.primitive_param;
        node["conn_id"] = rule.conn_id;
        node["direction"] = rule.direction;
        node["root"] = rule.root;
        node["ack_conn"] = rule.ack_conn;
        node["out_conns"] = rule.out_conns;
        return node;
    }
};

template<>
struct convert<SwitchConfig> {
    static Node encode(const SwitchConfig& sw) {
        Node node;
        node["id"] = sw.id;
        node["connections"] = sw.connections;
        node["rules"] = sw.rules;
        return node;
    }
};
}


struct controller_group{
    int id;
    int world_size; // world_size 记录了有多少计算节点（即有多少个rank）
    uint32_t *ip_list; // index is rank
    static int group_num;

    controller_group(int ws):world_size(ws){
        id = ++group_num;
        ip_list = new uint32_t(ws);
    }

    ~controller_group(){
        delete ip_list;
    }
};

// controller_communicator：管理一个 group 内的通信器，负责收集 QP 信息、计算路由并生成 YAML 拓扑
struct controller_communicator{
    controller_group *group;          // 指向所属 group（拥有 world_size 和 ip_list）
    int id;                           // 通信器编号，自增
    uint32_t *qp_list;                // 每个 rank 对应的 QP 编号数组，长度等于 group->world_size
    std::vector<SwitchConfig> switches; // 计算后的交换机路由配置，将被序列化到 YAML
    static int communicator_num;      // 用于生成唯一 id 的计数器

    // rank 到连接的映射: rank_to_conn[rank] = {switch_id, conn_id}
    std::map<int, std::pair<int, int>> rank_to_conn;

    controller_communicator(controller_group *g):group(g){
        qp_list = new uint32_t(g->world_size);
        id = ++communicator_num;
    }

    // 辅助函数：将 IP 地址转换为字符串
    std::string ip_to_string(uint32_t ip) {
        char buf[INET_ADDRSTRLEN];
        struct in_addr addr;
        addr.s_addr = ip;
        inet_ntop(AF_INET, &addr, buf, INET_ADDRSTRLEN);
        return std::string(buf);
    }

    // 辅助函数：获取所有连接到主机的连接 ID 列表
    std::vector<int> get_host_conns(int switch_id) {
        std::vector<int> result;
        for (auto& conn : switches[switch_id].connections) {
            if (!conn.is_switch) {
                result.push_back(conn.conn_id);
            }
        }
        return result;
    }

    // 辅助函数：获取连接到指定 rank 的连接 ID
    int get_conn_for_rank(int switch_id, int rank) {
        for (auto& conn : switches[switch_id].connections) {
            if (!conn.is_switch && conn.peer_id == rank) {
                return conn.conn_id;
            }
        }
        return -1;
    }

    // 辅助函数：获取除指定 rank 外的所有主机连接
    std::vector<int> get_host_conns_except(int switch_id, int except_rank) {
        std::vector<int> result;
        for (auto& conn : switches[switch_id].connections) {
            if (!conn.is_switch && conn.peer_id != except_rank) {
                result.push_back(conn.conn_id);
            }
        }
        return result;
    }

    /**
     * @brief 生成 AllReduce 规则
     *
     * 单交换机场景：所有上行规则的 root=true，聚合后广播给所有主机
     */
    void generate_allreduce_rules() {
        // 目前只支持单交换机 (TOPOLOGY_SIZE=1)
        for (int sw_id = 0; sw_id < TOPOLOGY_SIZE; sw_id++) {
            auto& sw = switches[sw_id];
            std::vector<int> host_conns = get_host_conns(sw_id);

            // 为每个主机连接生成上行规则
            for (auto& conn : sw.connections) {
                if (conn.is_switch) continue;  // 跳过交换机连接

                RuleConfig rule;
                rule.src_ip = conn.peer_ip;    // 来自主机
                rule.dst_ip = conn.my_ip;      // 到交换机
                rule.primitive = 1;            // PRIMITIVE_TYPE_ALLREDUCE
                rule.primitive_param = -1;     // AllReduce 无参数
                rule.conn_id = conn.conn_id;
                rule.direction = 0;            // DIR_UP
                rule.root = true;              // 单交换机场景，交换机就是 root
                rule.ack_conn = conn.conn_id;
                rule.out_conns = host_conns;   // 广播给所有主机

                sw.rules.push_back(rule);
            }
        }
    }

    /**
     * @brief 生成 Reduce 规则
     *
     * 为每个可能的 root_rank 生成一套规则
     * 聚合后只发送给 root_rank
     */
    void generate_reduce_rules() {
        int world_size = group->world_size;

        for (int root_rank = 0; root_rank < world_size; root_rank++) {
            // 目前只支持单交换机
            for (int sw_id = 0; sw_id < TOPOLOGY_SIZE; sw_id++) {
                auto& sw = switches[sw_id];
                int root_conn = get_conn_for_rank(sw_id, root_rank);

                // 为每个主机连接生成上行规则
                for (auto& conn : sw.connections) {
                    if (conn.is_switch) continue;

                    RuleConfig rule;
                    rule.src_ip = conn.peer_ip;
                    rule.dst_ip = conn.my_ip;
                    rule.primitive = 2;            // PRIMITIVE_TYPE_REDUCE
                    rule.primitive_param = root_rank;
                    rule.conn_id = conn.conn_id;
                    rule.direction = 0;            // DIR_UP
                    rule.root = true;              // 单交换机场景
                    rule.ack_conn = conn.conn_id;
                    rule.out_conns = {root_conn};  // 只发给 root_rank

                    sw.rules.push_back(rule);
                }
            }
        }
    }

    /**
     * @brief 生成 Broadcast 规则
     *
     * 为每个可能的 sender_rank 生成一套规则
     * sender 发送，其他所有人接收
     */
    void generate_broadcast_rules() {
        int world_size = group->world_size;

        for (int sender_rank = 0; sender_rank < world_size; sender_rank++) {
            // 目前只支持单交换机
            for (int sw_id = 0; sw_id < TOPOLOGY_SIZE; sw_id++) {
                auto& sw = switches[sw_id];
                std::vector<int> receivers = get_host_conns_except(sw_id, sender_rank);

                // 只为 sender 生成规则（上行后直接广播给其他人）
                int sender_conn = get_conn_for_rank(sw_id, sender_rank);
                if (sender_conn < 0) continue;

                auto& conn = sw.connections[sender_conn];

                RuleConfig rule;
                rule.src_ip = conn.peer_ip;
                rule.dst_ip = conn.my_ip;
                rule.primitive = 3;            // PRIMITIVE_TYPE_BROADCAST
                rule.primitive_param = sender_rank;
                rule.conn_id = conn.conn_id;
                rule.direction = 0;            // DIR_UP (从 sender 上行)
                rule.root = true;              // 单交换机场景
                rule.ack_conn = conn.conn_id;
                rule.out_conns = receivers;    // 发给除 sender 外的所有人

                sw.rules.push_back(rule);
            }
        }
    }

    /**
     * @brief 计算路由并生成规则
     *
     * 1. 初始化 connections
     * 2. 生成 AllReduce/Reduce/Broadcast 规则
     * 3. 生成 YAML 文件
     */
    void calculate_route(void *topology_info) {
        printf("[Controller] calculate_route: start\n");

        // 1) 初始化 switches 和 connections
        for (int i = 0; i < TOPOLOGY_SIZE; i++) {
            auto& info = switch_topology[i];
            SwitchConfig sc;
            sc.id = info.id;

            int conn_id = 0;
            for (auto& port : info.ports) {
                ConnectionConfig cc;
                cc.conn_id = conn_id;
                cc.my_ip = port.ip;
                cc.my_mac = port.mac;
                cc.my_name = port.name;
                cc.my_port = 4791;
                cc.my_qp = id + conn_id;
                // 其他字段稍后填充
                sc.connections.push_back(cc);
                conn_id++;
            }
            switches.push_back(sc);
        }

        // 2) 填充对端信息 (目前硬编码，后续应由拓扑算法计算)
        // TODO: 这里需要根据实际拓扑动态计算
        char rankip[INET_ADDRSTRLEN];
        struct in_addr addr;

        // rank 0 -> switch 0, conn 0
        switches[0].connections[0].is_switch = false;
        switches[0].connections[0].peer_id = 0;
        addr.s_addr = group->ip_list[0];
        inet_ntop(AF_INET, &addr, rankip, INET_ADDRSTRLEN);
        switches[0].connections[0].peer_ip = rankip;
        switches[0].connections[0].peer_mac = "fa:16:3e:87:09:4c";
        switches[0].connections[0].peer_port = 4791;
        switches[0].connections[0].peer_qp = qp_list[0];
        rank_to_conn[0] = {0, 0};

        // rank 1 -> switch 0, conn 1
        switches[0].connections[1].is_switch = false;
        switches[0].connections[1].peer_id = 1;
        addr.s_addr = group->ip_list[1];
        inet_ntop(AF_INET, &addr, rankip, INET_ADDRSTRLEN);
        switches[0].connections[1].peer_ip = rankip;
        switches[0].connections[1].peer_mac = "fa:16:3e:7b:25:36";
        switches[0].connections[1].peer_port = 4791;
        switches[0].connections[1].peer_qp = qp_list[1];
        rank_to_conn[1] = {0, 1};

        printf("[Controller] connections initialized\n");

        // 3) 生成规则
        generate_allreduce_rules();
        printf("[Controller] AllReduce rules generated\n");

        generate_reduce_rules();
        printf("[Controller] Reduce rules generated\n");

        generate_broadcast_rules();
        printf("[Controller] Broadcast rules generated\n");

        // 4) 生成 YAML
        generate_yaml();
        printf("[Controller] YAML generated\n");
    }

    // 将配置写入 /root/topology.yaml 文件
    void generate_yaml() {
        YAML::Node root;
        root["world_size"] = group->world_size;
        root["switches"] = switches;

        std::ofstream fout("/root/topology.yaml");
        fout << root;
        fout.close();
    }

    /**
     * @brief 生成指定交换机的YAML配置字符串
     *
     * @param switch_id 交换机ID
     * @return YAML格式的配置字符串
     */
    std::string generate_yaml_for_switch(int switch_id) {
        if (switch_id < 0 || switch_id >= (int)switches.size()) {
            return "";
        }

        YAML::Node root;
        root["world_size"] = group->world_size;
        root["switch"] = switches[switch_id];

        std::stringstream ss;
        ss << root;
        return ss.str();
    }

    /**
     * @brief 发送YAML配置到指定交换机
     *
     * 协议: 先发送1字节命令(CMD_SET_CONNECTIONS=4)，再发送4字节长度，最后发送YAML内容
     *
     * @param switch_id 交换机ID
     * @return 0成功，-1失败
     */
    int send_yaml_to_switch(int switch_id) {
        if (switch_id < 0 || switch_id >= TOPOLOGY_SIZE) {
            fprintf(stderr, "[Controller] Invalid switch_id: %d\n", switch_id);
            return -1;
        }

        int sockfd = switch_topology[switch_id].fd;
        if (sockfd < 0) {
            fprintf(stderr, "[Controller] Switch %d not connected\n", switch_id);
            return -1;
        }

        // 生成该交换机的YAML配置
        std::string yaml_content = generate_yaml_for_switch(switch_id);
        if (yaml_content.empty()) {
            fprintf(stderr, "[Controller] Failed to generate YAML for switch %d\n", switch_id);
            return -1;
        }

        printf("[Controller] Sending YAML to switch %d (%zu bytes):\n%s\n",
               switch_id, yaml_content.size(), yaml_content.c_str());

        // 1. 发送命令字节 (CMD_SET_CONNECTIONS = 4)
        uint8_t cmd = 4;
        if (send(sockfd, &cmd, sizeof(cmd), 0) != sizeof(cmd)) {
            perror("[Controller] Failed to send command");
            return -1;
        }

        // 2. 发送YAML长度 (网络字节序)
        uint32_t yaml_size = htonl(yaml_content.size());
        if (send(sockfd, &yaml_size, sizeof(yaml_size), 0) != sizeof(yaml_size)) {
            perror("[Controller] Failed to send YAML size");
            return -1;
        }

        // 3. 发送YAML内容
        size_t total_sent = 0;
        while (total_sent < yaml_content.size()) {
            ssize_t sent = send(sockfd, yaml_content.c_str() + total_sent,
                               yaml_content.size() - total_sent, 0);
            if (sent <= 0) {
                perror("[Controller] Failed to send YAML content");
                return -1;
            }
            total_sent += sent;
        }

        printf("[Controller] YAML sent to switch %d successfully\n", switch_id);
        return 0;
    }

    /**
     * @brief 发送YAML配置到所有已连接的交换机
     *
     * @return 成功发送的交换机数量
     */
    int send_yaml_to_all_switches() {
        int success_count = 0;
        for (int i = 0; i < TOPOLOGY_SIZE; i++) {
            if (switch_topology[i].fd >= 0) {
                if (send_yaml_to_switch(i) == 0) {
                    success_count++;
                }
            }
        }
        printf("[Controller] YAML sent to %d/%d switches\n", success_count, TOPOLOGY_SIZE);
        return success_count;
    }

    ~controller_communicator(){
        delete qp_list;
    }
};


