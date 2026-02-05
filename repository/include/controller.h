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
    std::vector<int> in_conns;    // 需要聚合的输入连接列表
    std::vector<int> out_conns;   // 转发给哪些连接
};

// ============================================
// 交换机配置结构体
// ============================================
struct SwitchConfig {
    int id;
    bool is_root;  // 是否是根交换机（树形拓扑的顶层）
    int reduce_root_conn;  // Reduce 操作中 root 节点所在的连接 ID (-1 表示不在本交换机下)
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
        node["in_conns"] = rule.in_conns;
        node["out_conns"] = rule.out_conns;
        return node;
    }
};

template<>
struct convert<SwitchConfig> {
    static Node encode(const SwitchConfig& sw) {
        Node node;
        node["id"] = sw.id;
        node["is_root"] = sw.is_root;
        node["reduce_root_conn"] = sw.reduce_root_conn;
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
        ip_list = new uint32_t[ws];  // 分配数组，不是单个值
    }

    ~controller_group(){
        delete[] ip_list;  // 使用 delete[] 释放数组
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
        qp_list = new uint32_t[g->world_size];  // 分配数组，不是单个值
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

    // 辅助函数：获取所有连接到子交换机的连接 ID 列表
    std::vector<int> get_switch_conns(int switch_id) {
        std::vector<int> result;
        for (auto& conn : switches[switch_id].connections) {
            if (conn.is_switch) {
                result.push_back(conn.conn_id);
            }
        }
        return result;
    }

    // 辅助函数：获取连接到父交换机的连接 ID（对于叶子交换机）
    int get_parent_conn(int switch_id) {
        for (auto& conn : switches[switch_id].connections) {
            if (conn.is_switch && conn.peer_id < switch_id) {
                return conn.conn_id;
            }
        }
        return -1;
    }

    /**
     * @brief 生成 AllReduce 规则 - 支持多层拓扑
     *
     * 1-2-4 拓扑：
     * - 叶子交换机：聚合本地主机数据，向上转发到根交换机
     * - 根交换机：聚合来自子交换机的数据，广播回子交换机
     * - 叶子交换机：接收广播数据，转发给本地主机
     */
    void generate_allreduce_rules() {
        for (int sw_id = 0; sw_id < TOPOLOGY_SIZE; sw_id++) {
            auto& sw = switches[sw_id];
            std::vector<int> host_conns = get_host_conns(sw_id);
            std::vector<int> switch_conns = get_switch_conns(sw_id);

            if (sw.is_root) {
                // 根交换机：从子交换机接收聚合数据
                for (auto& conn : sw.connections) {
                    if (!conn.is_switch) continue;  // 只处理交换机连接

                    RuleConfig rule;
                    rule.src_ip = conn.peer_ip;
                    rule.dst_ip = conn.my_ip;
                    rule.primitive = 1;  // ALLREDUCE
                    rule.primitive_param = -1;
                    rule.conn_id = conn.conn_id;
                    rule.direction = 0;  // DIR_UP
                    rule.root = true;    // 根交换机是聚合根
                    rule.ack_conn = conn.conn_id;
                    rule.in_conns = switch_conns;  // 需要聚合所有子交换机的数据
                    rule.out_conns = switch_conns;  // 广播给所有子交换机

                    sw.rules.push_back(rule);
                }
            } else {
                // 叶子交换机：从主机接收数据，聚合后向上转发
                int parent_conn = get_parent_conn(sw_id);

                for (auto& conn : sw.connections) {
                    if (conn.is_switch) continue;  // 只处理主机连接

                    RuleConfig rule;
                    rule.src_ip = conn.peer_ip;
                    rule.dst_ip = conn.my_ip;
                    rule.primitive = 1;  // ALLREDUCE
                    rule.primitive_param = -1;
                    rule.conn_id = conn.conn_id;
                    rule.direction = 0;  // DIR_UP
                    rule.root = false;   // 叶子交换机不是根
                    rule.ack_conn = conn.conn_id;
                    rule.in_conns = host_conns;  // 需要聚合所有本地主机的数据
                    rule.out_conns = {parent_conn};  // 向上转发到父交换机

                    sw.rules.push_back(rule);
                }

                // 叶子交换机：从父交换机接收广播数据，转发给本地主机
                if (parent_conn >= 0) {
                    auto& pconn = sw.connections[parent_conn];
                    RuleConfig rule;
                    rule.src_ip = pconn.peer_ip;
                    rule.dst_ip = pconn.my_ip;
                    rule.primitive = 1;  // ALLREDUCE
                    rule.primitive_param = -1;
                    rule.conn_id = parent_conn;
                    rule.direction = 1;  // DIR_DOWN
                    rule.root = false;
                    rule.ack_conn = parent_conn;
                    rule.in_conns = {parent_conn};  // 只从父交换机接收
                    rule.out_conns = host_conns;  // 广播给本地主机

                    sw.rules.push_back(rule);
                }
            }
        }
    }

    /**
     * @brief 生成 Reduce 规则 - 支持多层拓扑
     *
     * Reduce: 所有节点数据聚合到 root_rank
     * - 叶子交换机：聚合本地主机数据，向上转发
     * - 根交换机：聚合后发送给 root_rank 所在的子交换机
     */
    void generate_reduce_rules() {
        int world_size = group->world_size;

        for (int root_rank = 0; root_rank < world_size; root_rank++) {
            auto root_loc = rank_to_conn[root_rank];
            int root_sw_id = root_loc.first;
            int root_conn_id = root_loc.second;

            for (int sw_id = 0; sw_id < TOPOLOGY_SIZE; sw_id++) {
                auto& sw = switches[sw_id];
                std::vector<int> host_conns = get_host_conns(sw_id);
                std::vector<int> switch_conns = get_switch_conns(sw_id);

                if (sw.is_root) {
                    // Spine：只从非 root 所在的子交换机接收，转发给 root_rank 所在子交换机
                    // 计算需要聚合的子交换机连接（排除 root_sw）
                    std::vector<int> non_root_switch_conns;
                    for (auto& c : sw.connections) {
                        if (c.is_switch && c.peer_id != root_sw_id) {
                            non_root_switch_conns.push_back(c.conn_id);
                        }
                    }

                    for (auto& conn : sw.connections) {
                        if (!conn.is_switch) continue;
                        // 跳过 root_rank 所在的子交换机（它不会发数据给 Spine）
                        if (conn.peer_id == root_sw_id) continue;

                        int target_conn = -1;
                        for (auto& c : sw.connections) {
                            if (c.is_switch && c.peer_id == root_sw_id) {
                                target_conn = c.conn_id;
                                break;
                            }
                        }

                        RuleConfig rule;
                        rule.src_ip = conn.peer_ip;
                        rule.dst_ip = conn.my_ip;
                        rule.primitive = 2;
                        rule.primitive_param = root_rank;
                        rule.conn_id = conn.conn_id;
                        rule.direction = 0;
                        rule.root = false;  // Spine 不是 root，只是中转
                        rule.ack_conn = conn.conn_id;
                        rule.in_conns = non_root_switch_conns;  // 聚合所有非 root_sw 的子交换机
                        rule.out_conns = {target_conn};

                        sw.rules.push_back(rule);
                    }
                } else {
                    // 叶子交换机
                    int parent_conn = get_parent_conn(sw_id);
                    int local_root_conn = get_conn_for_rank(sw_id, root_rank);
                    bool is_root_sw = (sw_id == root_sw_id);

                    // 计算 in_conns：root 所在交换机需要聚合 host + parent，非 root 只聚合 host
                    std::vector<int> in_conns_for_leaf = host_conns;
                    if (is_root_sw && parent_conn >= 0) {
                        in_conns_for_leaf.push_back(parent_conn);
                    }

                    // 从主机接收数据
                    for (auto& conn : sw.connections) {
                        if (conn.is_switch) continue;

                        std::vector<int> out;
                        if (is_root_sw) {
                            // root 所在交换机：聚合后发给 root_rank
                            out = {local_root_conn};
                        } else {
                            // 非 root 交换机：聚合后发给 parent
                            out = {parent_conn};
                        }

                        RuleConfig rule;
                        rule.src_ip = conn.peer_ip;
                        rule.dst_ip = conn.my_ip;
                        rule.primitive = 2;
                        rule.primitive_param = root_rank;
                        rule.conn_id = conn.conn_id;
                        rule.direction = 0;
                        rule.root = is_root_sw;  // root 所在叶子交换机是 root
                        rule.ack_conn = conn.conn_id;
                        rule.in_conns = in_conns_for_leaf;  // 聚合来源
                        rule.out_conns = out;

                        sw.rules.push_back(rule);
                    }

                    // root 所在交换机：接收来自 Spine 的聚合数据
                    if (is_root_sw && parent_conn >= 0) {
                        auto& pconn = sw.connections[parent_conn];
                        RuleConfig rule;
                        rule.src_ip = pconn.peer_ip;
                        rule.dst_ip = pconn.my_ip;
                        rule.primitive = 2;
                        rule.primitive_param = root_rank;
                        rule.conn_id = parent_conn;
                        rule.direction = 1;
                        rule.root = true;  // 接收下行数据时也是 root
                        rule.ack_conn = parent_conn;
                        rule.in_conns = in_conns_for_leaf;  // 同样的聚合来源
                        rule.out_conns = {local_root_conn};

                        sw.rules.push_back(rule);
                    }
                }
            }
        }
    }

    /**
     * @brief 生成 Broadcast 规则 - 支持多层拓扑
     *
     * Broadcast: sender_rank 的数据广播给所有其他节点
     * - sender 所在叶子交换机：向上转发到根交换机
     * - 根交换机：广播给所有子交换机
     * - 其他叶子交换机：转发给本地主机
     */
    void generate_broadcast_rules() {
        int world_size = group->world_size;

        for (int sender_rank = 0; sender_rank < world_size; sender_rank++) {
            auto sender_loc = rank_to_conn[sender_rank];
            int sender_sw_id = sender_loc.first;

            for (int sw_id = 0; sw_id < TOPOLOGY_SIZE; sw_id++) {
                auto& sw = switches[sw_id];
                std::vector<int> host_conns = get_host_conns(sw_id);
                std::vector<int> switch_conns = get_switch_conns(sw_id);

                if (sw.is_root) {
                    // 根交换机：从 sender 所在子交换机接收，广播给其他子交换机
                    for (auto& conn : sw.connections) {
                        if (!conn.is_switch) continue;
                        if (conn.peer_id != sender_sw_id) continue;

                        // 广播给除了来源之外的所有子交换机
                        std::vector<int> other_switch_conns;
                        for (int sc : switch_conns) {
                            if (sc != conn.conn_id) {
                                other_switch_conns.push_back(sc);
                            }
                        }

                        RuleConfig rule;
                        rule.src_ip = conn.peer_ip;
                        rule.dst_ip = conn.my_ip;
                        rule.primitive = 3;  // BROADCAST
                        rule.primitive_param = sender_rank;
                        rule.conn_id = conn.conn_id;
                        rule.direction = 0;  // DIR_UP
                        rule.root = false;  // 根交换机不是 root，只是中转
                        rule.ack_conn = conn.conn_id;
                        rule.in_conns = {conn.conn_id};  // 只从 sender 所在子交换机接收
                        rule.out_conns = other_switch_conns;

                        sw.rules.push_back(rule);
                    }
                } else {
                    int parent_conn = get_parent_conn(sw_id);
                    int sender_conn = get_conn_for_rank(sw_id, sender_rank);

                    if (sw_id == sender_sw_id && sender_conn >= 0) {
                        // sender 所在交换机：这是 root！同时发给 spine 和本地其他 host
                        auto& conn = sw.connections[sender_conn];

                        // out_conns = parent + 本地其他 host
                        std::vector<int> out_conns;
                        if (parent_conn >= 0) {
                            out_conns.push_back(parent_conn);
                        }
                        for (int hc : host_conns) {
                            if (hc != sender_conn) {
                                out_conns.push_back(hc);
                            }
                        }

                        RuleConfig rule;
                        rule.src_ip = conn.peer_ip;
                        rule.dst_ip = conn.my_ip;
                        rule.primitive = 3;
                        rule.primitive_param = sender_rank;
                        rule.conn_id = sender_conn;
                        rule.direction = 0;
                        rule.root = true;  // sender 所在叶子交换机是 root
                        rule.ack_conn = sender_conn;
                        rule.in_conns = {sender_conn};  // 只从 sender 接收
                        rule.out_conns = out_conns;

                        sw.rules.push_back(rule);
                    }

                    // 非 sender 所在交换机：接收下行广播数据
                    if (sw_id != sender_sw_id && parent_conn >= 0) {
                        auto& pconn = sw.connections[parent_conn];
                        RuleConfig rule;
                        rule.src_ip = pconn.peer_ip;
                        rule.dst_ip = pconn.my_ip;
                        rule.primitive = 3;
                        rule.primitive_param = sender_rank;
                        rule.conn_id = parent_conn;
                        rule.direction = 1;  // DIR_DOWN
                        rule.root = false;
                        rule.ack_conn = parent_conn;
                        rule.in_conns = {parent_conn};  // 只从父交换机接收
                        rule.out_conns = host_conns;  // 转发给所有本地 host

                        sw.rules.push_back(rule);
                    }
                }
            }
        }
    }

    /**
     * @brief 生成 Barrier 规则 - 支持多层拓扑
     *
     * Barrier: 所有节点同步，类似 AllReduce 但不传输数据
     */
    void generate_barrier_rules() {
        for (int sw_id = 0; sw_id < TOPOLOGY_SIZE; sw_id++) {
            auto& sw = switches[sw_id];
            std::vector<int> host_conns = get_host_conns(sw_id);
            std::vector<int> switch_conns = get_switch_conns(sw_id);

            if (sw.is_root) {
                // 根交换机：从子交换机接收 barrier 消息
                for (auto& conn : sw.connections) {
                    if (!conn.is_switch) continue;

                    RuleConfig rule;
                    rule.src_ip = conn.peer_ip;
                    rule.dst_ip = conn.my_ip;
                    rule.primitive = 4;  // BARRIER
                    rule.primitive_param = -1;
                    rule.conn_id = conn.conn_id;
                    rule.direction = 0;  // DIR_UP
                    rule.root = true;
                    rule.ack_conn = conn.conn_id;
                    rule.in_conns = switch_conns;  // 聚合所有子交换机
                    rule.out_conns = switch_conns;

                    sw.rules.push_back(rule);
                }
            } else {
                // 叶子交换机
                int parent_conn = get_parent_conn(sw_id);

                for (auto& conn : sw.connections) {
                    if (conn.is_switch) continue;

                    RuleConfig rule;
                    rule.src_ip = conn.peer_ip;
                    rule.dst_ip = conn.my_ip;
                    rule.primitive = 4;  // BARRIER
                    rule.primitive_param = -1;
                    rule.conn_id = conn.conn_id;
                    rule.direction = 0;  // DIR_UP
                    rule.root = false;
                    rule.ack_conn = conn.conn_id;
                    rule.in_conns = host_conns;  // 聚合所有本地主机
                    rule.out_conns = {parent_conn};

                    sw.rules.push_back(rule);
                }

                // 从父交换机接收广播确认
                if (parent_conn >= 0) {
                    auto& pconn = sw.connections[parent_conn];
                    RuleConfig rule;
                    rule.src_ip = pconn.peer_ip;
                    rule.dst_ip = pconn.my_ip;
                    rule.primitive = 4;  // BARRIER
                    rule.primitive_param = -1;
                    rule.conn_id = parent_conn;
                    rule.direction = 1;  // DIR_DOWN
                    rule.root = false;
                    rule.ack_conn = parent_conn;
                    rule.in_conns = {parent_conn};  // 只从父交换机接收
                    rule.out_conns = host_conns;

                    sw.rules.push_back(rule);
                }
            }
        }
    }

    /**
     * @brief 生成 ReduceScatter 规则
     */
    void generate_reducescatter_rules() {
        for (int sw_id = 0; sw_id < TOPOLOGY_SIZE; sw_id++) {
            auto& sw = switches[sw_id];
            std::vector<int> host_conns = get_host_conns(sw_id);
            std::vector<int> switch_conns = get_switch_conns(sw_id);

            if (sw.is_root) {
                for (auto& conn : sw.connections) {
                    if (!conn.is_switch) continue;

                    RuleConfig rule;
                    rule.src_ip = conn.peer_ip;
                    rule.dst_ip = conn.my_ip;
                    rule.primitive = 5;  // REDUCESCATTER
                    rule.primitive_param = -1;
                    rule.conn_id = conn.conn_id;
                    rule.direction = 0;
                    rule.root = true;
                    rule.ack_conn = conn.conn_id;
                    rule.in_conns = switch_conns;  // 聚合所有子交换机
                    rule.out_conns = switch_conns;

                    sw.rules.push_back(rule);
                }
            } else {
                int parent_conn = get_parent_conn(sw_id);

                for (auto& conn : sw.connections) {
                    if (conn.is_switch) continue;

                    RuleConfig rule;
                    rule.src_ip = conn.peer_ip;
                    rule.dst_ip = conn.my_ip;
                    rule.primitive = 5;  // REDUCESCATTER
                    rule.primitive_param = -1;
                    rule.conn_id = conn.conn_id;
                    rule.direction = 0;
                    rule.root = false;
                    rule.ack_conn = conn.conn_id;
                    rule.in_conns = host_conns;  // 聚合所有本地主机
                    rule.out_conns = {parent_conn};

                    sw.rules.push_back(rule);
                }

                if (parent_conn >= 0) {
                    auto& pconn = sw.connections[parent_conn];
                    RuleConfig rule;
                    rule.src_ip = pconn.peer_ip;
                    rule.dst_ip = pconn.my_ip;
                    rule.primitive = 5;  // REDUCESCATTER
                    rule.primitive_param = -1;
                    rule.conn_id = parent_conn;
                    rule.direction = 1;
                    rule.root = false;
                    rule.ack_conn = parent_conn;
                    rule.in_conns = {parent_conn};  // 只从父交换机接收
                    rule.out_conns = host_conns;

                    sw.rules.push_back(rule);
                }
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
            sc.is_root = (i == 0);  // Switch 0 是根交换机
            sc.reduce_root_conn = -1;  // 默认 -1，稍后根据 root_rank 设置

            int conn_id = 0;
            for (auto& port : info.ports) {
                ConnectionConfig cc;
                cc.conn_id = conn_id;
                cc.my_ip = port.ip;
                cc.my_mac = port.mac;
                cc.my_name = port.name;
                cc.my_port = RDMA_HOST_PORT;  // 默认使用 Host 端口，Switch 间连接后面会覆盖
                cc.my_qp = id + conn_id;
                // 其他字段稍后填充
                sc.connections.push_back(cc);
                conn_id++;
            }
            switches.push_back(sc);
        }

        // 2) 填充对端信息 - 1-2-4 拓扑
        // Switch 0 (root): 连接 Switch 1 和 Switch 2
        // Switch 1: 连接 Switch 0 和 pku1, pku2 (rank 0, 1)
        // Switch 2: 连接 Switch 0 和 pku3, pku4 (rank 2, 3)
        char rankip[INET_ADDRSTRLEN];
        struct in_addr addr;

        // ========== Switch 0 (root) 的连接 ==========
        // conn 0: 连接 Switch 1 (vm1)
        switches[0].connections[0].is_switch = true;
        switches[0].connections[0].peer_id = 1;  // Switch 1
        switches[0].connections[0].peer_ip = "172.16.0.5";
        switches[0].connections[0].peer_mac = "fa:16:3e:87:09:4c";
        switches[0].connections[0].peer_port = RDMA_SWITCH_PORT;
        switches[0].connections[0].my_port = RDMA_SWITCH_PORT;
        switches[0].connections[0].peer_qp = id;

        // conn 1: 连接 Switch 2 (vm2)
        switches[0].connections[1].is_switch = true;
        switches[0].connections[1].peer_id = 2;  // Switch 2
        switches[0].connections[1].peer_ip = "10.10.0.7";
        switches[0].connections[1].peer_mac = "fa:16:3e:7b:25:36";
        switches[0].connections[1].peer_port = RDMA_SWITCH_PORT;
        switches[0].connections[1].my_port = RDMA_SWITCH_PORT;
        switches[0].connections[1].peer_qp = id + 1;

        // ========== Switch 1 (vm1) 的连接 ==========
        // conn 0: 连接 Switch 0 (root)
        switches[1].connections[0].is_switch = true;
        switches[1].connections[0].peer_id = 0;  // Switch 0
        switches[1].connections[0].peer_ip = "172.16.0.21";
        switches[1].connections[0].peer_mac = "fa:16:3e:9a:26:99";
        switches[1].connections[0].peer_port = RDMA_SWITCH_PORT;
        switches[1].connections[0].my_port = RDMA_SWITCH_PORT;
        switches[1].connections[0].peer_qp = id;

        // conn 1: 连接 pku1, pku2 (rank 0, 1)
        // 为每个 rank 创建独立的连接配置
        // pku1 (rank 0)
        switches[1].connections[1].is_switch = false;
        switches[1].connections[1].peer_id = 0;  // rank 0 (pku1)
        addr.s_addr = group->ip_list[0];
        inet_ntop(AF_INET, &addr, rankip, INET_ADDRSTRLEN);
        switches[1].connections[1].peer_ip = rankip;
        switches[1].connections[1].peer_mac = "fa:16:3e:5f:54:f6";  // pku1 eth1 MAC
        switches[1].connections[1].peer_port = 4791;
        switches[1].connections[1].peer_qp = qp_list[0];
        rank_to_conn[0] = {1, 1};

        // pku2 (rank 1) - 添加新连接
        if (group->world_size > 1) {
            ConnectionConfig cc_pku2;
            cc_pku2.conn_id = 2;
            cc_pku2.is_switch = false;
            cc_pku2.peer_id = 1;  // rank 1 (pku2)
            addr.s_addr = group->ip_list[1];
            inet_ntop(AF_INET, &addr, rankip, INET_ADDRSTRLEN);
            cc_pku2.peer_ip = rankip;
            cc_pku2.peer_mac = "fa:16:3e:c9:8e:7d";  // pku2 eth1 MAC
            cc_pku2.my_ip = "10.1.1.25";
            cc_pku2.my_mac = "fa:16:3e:89:1f:9e";
            cc_pku2.my_name = "eth2";
            cc_pku2.my_port = 4791;
            cc_pku2.my_qp = id + 2;
            cc_pku2.peer_port = 4791;
            cc_pku2.peer_qp = qp_list[1];
            switches[1].connections.push_back(cc_pku2);
            rank_to_conn[1] = {1, 2};
        }

        // ========== Switch 2 (vm2) 的连接 ==========
        // conn 0: 连接 Switch 0 (root)
        switches[2].connections[0].is_switch = true;
        switches[2].connections[0].peer_id = 0;  // Switch 0
        switches[2].connections[0].peer_ip = "10.10.0.14";
        switches[2].connections[0].peer_mac = "fa:16:3e:3d:ab:78";
        switches[2].connections[0].peer_port = RDMA_SWITCH_PORT;
        switches[2].connections[0].my_port = RDMA_SWITCH_PORT;
        switches[2].connections[0].peer_qp = id + 1;

        // conn 1: 连接 pku3 (rank 2)
        if (group->world_size > 2) {
            switches[2].connections[1].is_switch = false;
            switches[2].connections[1].peer_id = 2;  // rank 2 (pku3)
            addr.s_addr = group->ip_list[2];
            inet_ntop(AF_INET, &addr, rankip, INET_ADDRSTRLEN);
            switches[2].connections[1].peer_ip = rankip;
            switches[2].connections[1].peer_mac = "fa:16:3e:bf:0a:a0";  // pku3 eth1 MAC
            switches[2].connections[1].peer_port = 4791;
            switches[2].connections[1].peer_qp = qp_list[2];
            rank_to_conn[2] = {2, 1};
        }

        // conn 2: 连接 pku4 (rank 3)
        if (group->world_size > 3) {
            ConnectionConfig cc_pku4;
            cc_pku4.conn_id = 2;
            cc_pku4.is_switch = false;
            cc_pku4.peer_id = 3;  // rank 3 (pku4)
            addr.s_addr = group->ip_list[3];
            inet_ntop(AF_INET, &addr, rankip, INET_ADDRSTRLEN);
            cc_pku4.peer_ip = rankip;
            cc_pku4.peer_mac = "fa:16:3e:07:9a:2c";  // pku4 eth1 MAC
            cc_pku4.my_ip = "10.2.1.18";
            cc_pku4.my_mac = "fa:16:3e:10:c2:d0";
            cc_pku4.my_name = "eth2";
            cc_pku4.my_port = 4791;
            cc_pku4.my_qp = id + 3;
            cc_pku4.peer_port = 4791;
            cc_pku4.peer_qp = qp_list[3];
            switches[2].connections.push_back(cc_pku4);
            rank_to_conn[3] = {2, 2};
        }

        printf("[Controller] connections initialized\n");

        // 4) 设置 reduce_root_conn (Reduce 操作中 root 节点所在的连接)
        // 假设 root_rank = 0，在 Switch 1 下的 conn 1 (pku1)
        // Switch 0 (Spine): root 在 Switch 1 下，所以 reduce_root_conn = 0 (连接到 Switch 1)
        // Switch 1 (Leaf1): root 是 pku1 (rank 0)，对应 conn 1
        // Switch 2 (Leaf2): root 不在这里，reduce_root_conn = -1
        switches[0].reduce_root_conn = 0;  // Spine -> Switch 1
        switches[1].reduce_root_conn = 1;  // Leaf1 -> pku1 (rank 0)
        switches[2].reduce_root_conn = -1; // Leaf2 没有 root
        printf("[Controller] reduce_root_conn set: sw0=%d, sw1=%d, sw2=%d\n",
               switches[0].reduce_root_conn, switches[1].reduce_root_conn, switches[2].reduce_root_conn);

        // 5) 生成规则
        generate_allreduce_rules();
        printf("[Controller] AllReduce rules generated\n");

        generate_reduce_rules();
        printf("[Controller] Reduce rules generated\n");

        generate_broadcast_rules();
        printf("[Controller] Broadcast rules generated\n");

        generate_barrier_rules();
        printf("[Controller] Barrier rules generated\n");

        // ReduceScatter 暂不支持
        // generate_reducescatter_rules();
        // printf("[Controller] ReduceScatter rules generated\n");

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
        delete[] qp_list;  // 使用 delete[] 释放数组
    }
};


