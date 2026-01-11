#include <yaml-cpp/yaml.h>
#include <vector>
#include <fstream>
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

// 定义连接配置结构体
struct ConnectionConfig {
    bool up;
    int host_id;
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

// 定义交换机配置结构体
struct SwitchConfig {
    int id;
    bool root;
    std::vector<ConnectionConfig> connections;
};

// YAML 序列化适配器
namespace YAML {
template<>
struct convert<ConnectionConfig> {
    static Node encode(const ConnectionConfig& conn) {
        Node node;
        node["up"] = conn.up;
        node["host_id"] = conn.host_id;
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
struct convert<SwitchConfig> {
    static Node encode(const SwitchConfig& sw) {
        Node node;
        node["id"] = sw.id;
        node["root"] = sw.root;
        node["connections"] = sw.connections;
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

    controller_communicator(controller_group *g):group(g){
        qp_list = new uint32_t(g->world_size);
        id = ++communicator_num;
    }

    // 根据当前全局拓扑 switch_topology 和已收集的 QP 信息，构造 YAML 需要的交换机路由描述
    void calculate_route(void *topology_info){
        // 1) 初始化 switches: 为每台交换机创建 SwitchConfig，并填入本端端口信息
        printf("in function calculate_route\n");
        for(int i=0;i<TOPOLOGY_SIZE;++i){
            auto &info = switch_topology[i];
            std::cout << info.id << std::endl;
            std::cout << info.ports.size() << std::endl;
            SwitchConfig sc;
            sc.root = (info.id==0);   // 暂定 id=0 的交换机为 root
            sc.id = info.id;
            int j = 0;
            for(auto &port:info.ports){
                ConnectionConfig cc;
                cc.my_ip = port.ip;
                std::cout << cc.my_ip << std::endl;
                cc.my_mac = port.mac;
                std::cout << cc.my_mac << std::endl;
                cc.my_name = port.name;
                std::cout << cc.my_name << std::endl;
                cc.my_port = 4791;            // 默认业务端口
                cc.my_qp = id+(j++);           // 临时占位：为交换机侧分配唯一 QP 编号
                sc.connections.push_back(cc);
            }
            switches.push_back(sc);
        }
        
        printf("in function calculate_route 151th line\n");
        printf("%ld\n", switches.size());
        // 2) 手工填充与主机 (rank) 的对端信息：目前为示例硬编码，后续应由算法计算

        char rankip[INET_ADDRSTRLEN];
        struct in_addr addr;
        // rank 0
        switches[0].connections[0].up = false;   // false 表示对端是主机而非上级交换机
        switches[0].connections[0].host_id = 0;
        addr.s_addr = group->ip_list[0]; 
        inet_ntop(AF_INET, &addr, rankip, INET_ADDRSTRLEN); 
        switches[0].connections[0].peer_ip = rankip;
        switches[0].connections[0].peer_mac = "fa:16:3e:87:09:4c"; // vm1 eth1 MAC
        switches[0].connections[0].peer_port = 4791;
        switches[0].connections[0].peer_qp = qp_list[0];
        // rank 1
        switches[0].connections[1].up = false;
        switches[0].connections[1].host_id = 1;
        addr.s_addr = group->ip_list[1]; 
        inet_ntop(AF_INET, &addr, rankip, INET_ADDRSTRLEN); 
        switches[0].connections[1].peer_ip = rankip;
        switches[0].connections[1].peer_mac = "fa:16:3e:7b:25:36"; // vm2 eth1 MAC
        switches[0].connections[1].peer_port = 4791;
        switches[0].connections[1].peer_qp = qp_list[1];

        // switches[1].connections[1].up = false;
        // switches[1].connections[1].host_id = 1;
        // addr.s_addr = group->ip_list[1];
        // inet_ntop(AF_INET, &addr, rankip, INET_ADDRSTRLEN);
        // switches[1].connections[1].peer_ip = rankip;
        // switches[1].connections[1].peer_mac = "52:54:00:8d:53:ea";
        // switches[1].connections[1].peer_port = 4791;
        // switches[1].connections[1].peer_qp = qp_list[1];

        // switches[0].connections[0].up = false;
        // switches[0].connections[0].host_id = 101;

        // printf("in function calculate_route 157th line\n");

        // switches[0].connections[0].peer_ip = switch_topology[1].ports[2].ip;

        // printf("in function calculate_route 161th line\n");

        // switches[0].connections[0].peer_mac = switch_topology[1].ports[2].mac;

        // printf("in function calculate_route 165th line\n");

        // switches[0].connections[0].peer_port = 4791;
        // switches[0].connections[0].peer_qp = id;

        // switches[0].connections[1].up = false;
        // switches[0].connections[1].host_id = 102;

        // printf("in function calculate_route 173th line\n");

        // switches[0].connections[1].peer_ip = switch_topology[2].ports[2].ip;

        // printf("in function calculate_route 177th line\n");

        // switches[0].connections[1].peer_mac = switch_topology[2].ports[2].mac;
        // switches[0].connections[1].peer_port = 4791;
        // switches[0].connections[1].peer_qp = id;

        // char rankip[INET_ADDRSTRLEN];
        // struct in_addr addr;

        // switches[1].connections[0].up = false;
        // switches[1].connections[0].host_id = 0;
        // addr.s_addr = group->ip_list[0];
        // inet_ntop(AF_INET, &addr, rankip, INET_ADDRSTRLEN);
        // switches[1].connections[0].peer_ip = rankip;
        // switches[1].connections[0].peer_mac = "52:54:00:20:78:e5";
        // switches[1].connections[0].peer_port = 4791;
        // switches[1].connections[0].peer_qp = qp_list[0];

        // switches[1].connections[1].up = false;
        // switches[1].connections[1].host_id = 1;
        // addr.s_addr = group->ip_list[1];
        // inet_ntop(AF_INET, &addr, rankip, INET_ADDRSTRLEN);
        // switches[1].connections[1].peer_ip = rankip;
        // switches[1].connections[1].peer_mac = "52:54:00:8d:53:ea";
        // switches[1].connections[1].peer_port = 4791;
        // switches[1].connections[1].peer_qp = qp_list[1];

        // switches[1].connections[2].up = true;
        // switches[1].connections[2].host_id = 100;
        // switches[1].connections[2].peer_ip = switch_topology[0].ports[0].ip;
        // switches[1].connections[2].peer_mac = switch_topology[0].ports[0].mac;
        // switches[1].connections[2].peer_port = 4791;
        // switches[1].connections[2].peer_qp = id;

        // switches[2].connections[0].up = false;
        // switches[2].connections[0].host_id = 2;
        // addr.s_addr = group->ip_list[2];
        // inet_ntop(AF_INET, &addr, rankip, INET_ADDRSTRLEN);
        // switches[2].connections[0].peer_ip = rankip;
        // switches[2].connections[0].peer_mac = "52:54:00:01:06:ec";
        // switches[2].connections[0].peer_port = 4791;
        // switches[2].connections[0].peer_qp = qp_list[2];

        // switches[2].connections[1].up = false;
        // switches[2].connections[1].host_id = 3;
        // addr.s_addr = group->ip_list[3];
        // inet_ntop(AF_INET, &addr, rankip, INET_ADDRSTRLEN);
        // switches[2].connections[1].peer_ip = rankip;
        // switches[2].connections[1].peer_mac = "52:54:00:a3:3e:9d";
        // switches[2].connections[1].peer_port = 4791;
        // switches[2].connections[1].peer_qp = qp_list[3];

        // switches[2].connections[2].up = true;
        // switches[2].connections[2].host_id = 100;
        // switches[2].connections[2].peer_ip = switch_topology[0].ports[1].ip;
        // switches[2].connections[2].peer_mac = switch_topology[0].ports[1].mac;
        // switches[2].connections[2].peer_port = 4791;
        // switches[2].connections[2].peer_qp = id;
        
        


        generate_yaml(); // 3) 将 switches 序列化为 YAML 文件
    }

    // 将当前 switches 写入 /root/topology.yaml 文件
    void generate_yaml() {
        YAML::Node root;
        root["switches"] = switches;
        
        std::ofstream fout("/root/topology.yaml");
        fout << root;
    }

    ~controller_communicator(){
        delete qp_list;
    }
};


