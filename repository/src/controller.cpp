#include "controller.h"

switch_info switch_topology[TOPOLOGY_SIZE];

int controller_group::group_num = 0;
int controller_communicator::communicator_num = 10;

// 控制器指令定义
#define CMD_STALL 1
#define CMD_RESET 2
#define CMD_RESET_PSN 3
#define CMD_SET_CONNECTIONS 4
#define CMD_START 5

/**
 * @brief 发送单字节命令到交换机
 */
static int send_command(int fd, uint8_t cmd) {
    if (send(fd, &cmd, sizeof(cmd), 0) != sizeof(cmd)) {
        perror("[Controller] Failed to send command");
        return -1;
    }
    return 0;
}

/**
 * @brief 发送 YAML 配置到交换机（简化协议）
 *
 * 协议: 4字节长度(网络字节序) + YAML内容
 */
static int send_yaml_to_switch(int fd, const std::string& yaml_content) {
    // 1. 发送 YAML 长度 (网络字节序)
    uint32_t yaml_size = htonl(yaml_content.size());
    if (send(fd, &yaml_size, sizeof(yaml_size), 0) != sizeof(yaml_size)) {
        perror("[Controller] Failed to send YAML size");
        return -1;
    }

    // 3. 发送 YAML 内容
    size_t total_sent = 0;
    while (total_sent < yaml_content.size()) {
        ssize_t sent = send(fd, yaml_content.c_str() + total_sent,
                           yaml_content.size() - total_sent, 0);
        if (sent <= 0) {
            perror("[Controller] Failed to send YAML content");
            return -1;
        }
        total_sent += sent;
    }

    printf("[Controller] YAML sent to fd %d successfully (%zu bytes)\n", fd, yaml_content.size());
    return 0;
}

/**
 * @brief 发送 START 命令到交换机
 */
static int send_start_command(int fd) {
    return send_command(fd, CMD_START);
}

/**
 * @brief 发送 RESET 命令到交换机
 */
static int send_reset_command(int fd) {
    return send_command(fd, CMD_RESET);
}

/**
 * @brief 发送 RESET_PSN 命令到交换机
 */
static int send_reset_psn_command(int fd) {
    return send_command(fd, CMD_RESET_PSN);
}

/**
 * @brief 发送 STALL 命令到交换机
 */
static int send_stall_command(int fd) {
    return send_command(fd, CMD_STALL);
}

// 保留旧的文件发送函数用于向 rank0 发送拓扑
static void send_file_with_length(int fd, const char *file_path) {
    // 打开文件并获取大小
    FILE *file = fopen(file_path, "rb");
    if (!file) {
        perror("fopen failed");
        return;
    }

    // 获取文件大小
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    // 转换为网络字节序
    uint32_t net_file_size = htonl((uint32_t)file_size);

    // 1. 发送文件长度（4字节）
    size_t sent = 0;
    while (sent < sizeof(net_file_size)) {
        ssize_t ret = send(fd, (char*)&net_file_size + sent, sizeof(net_file_size) - sent, 0);
        if (ret <= 0) {
            perror("send file size failed");
            fclose(file);
            return;
        }
        sent += ret;
    }

    // 2. 发送文件内容
    char buffer[4096];
    size_t total_sent = 0;
    while (total_sent < file_size) {
        // 读取文件块
        size_t to_read = (sizeof(buffer) < (file_size - total_sent)) ? sizeof(buffer) : (file_size - total_sent);
        size_t bytes_read = fread(buffer, 1, to_read, file);
        if (bytes_read <= 0) {
            perror("fread failed");
            break;
        }

        // 发送块
        sent = 0;
        while (sent < bytes_read) {
            ssize_t ret = send(fd, buffer + sent, bytes_read - sent, 0);
            if (ret <= 0) {
                perror("send file content failed");
                fclose(file);
                return;
            }
            sent += ret;
        }
        total_sent += sent;
    }

    printf("send file to fd: %d succeed.\n", fd);
    fclose(file);
}

static void group_session(int client_fd) {
    char req_type;
    ssize_t bytes;
    char buffer[4096];
    controller_group *group = nullptr;
    int world_size = 0;
    std::vector<controller_communicator *> comms;
    // 持续处理来自同一 rank0 的控制请求：先建立 group，再可能建立多个 communicator
    while (true) {
        // 读取 1 字节请求类型（'G'：创建组；'C'：创建通信器并分发路由）

        bytes = recv(client_fd, &req_type, 1, MSG_WAITALL);

        // 检查连接是否关闭或出错
        if (bytes <= 0) {
            if (bytes == 0) {
                printf("[Controller] Client disconnected normally.\n");
            } else {
                perror("[Controller] recv error");
            }
            break;  // 退出循环，结束会话
        }

        printf("begin to recv from rank0, req_type='%c'\n", req_type);
        if(req_type == 'G'){
            printf("recv the group creation request.\n");
            // 组控制：收集 world_size 和各节点 IP，分配 group id 返回给 rank0
            bytes = recv(client_fd, &world_size, sizeof(int), MSG_WAITALL);
            group = new controller_group(world_size);
            for (int i = 0; i < world_size; ++i) {
                recv(client_fd, &group->ip_list[i], sizeof(uint32_t), MSG_WAITALL);
                // Convert from network byte order (big-endian) to host byte order
                uint32_t ip_addr = ntohl(group->ip_list[i]);
                printf("recv the ip %d.%d.%d.%d.\n",
                       (ip_addr >> 24) & 0xFF,
                       (ip_addr >> 16) & 0xFF,
                       (ip_addr >> 8) & 0xFF,
                       ip_addr & 0xFF);
            }
            send(client_fd, &group->id, sizeof(uint32_t), 0); // 将 group id 返给 rank0

        }
        else if(req_type == 'C'){
            printf("recv the communicator creation request.\n");
            // 通信器控制：收集各 rank 的 QP 编号，计算路由并将拓扑下发到交换机和 rank0
            controller_communicator *comm = new controller_communicator(group);
            comms.push_back(comm);
            // 收集每个 rank 的 qp 信息
            for (int i = 0; i < world_size; ++i) {
                recv(client_fd, &comm->qp_list[i], sizeof(uint32_t), MSG_WAITALL);
                printf("recv the qp num %d.\n",comm->qp_list[i]);
            }
            printf("all qp nums received.\n");
            // 计算路由并生成 YAML（/root/topology.yaml）
            comm->calculate_route(switch_topology);
            printf("route_calculated\n");

            // 使用新协议：为每个交换机生成并发送其专属的 YAML 配置
            printf("[Controller] Sending YAML configurations to switches...\n");
            for (int i = 0; i < TOPOLOGY_SIZE; ++i) {
                int fd = switch_topology[i].fd;
                if (fd < 0) {
                    fprintf(stderr, "[Controller] Switch %d not connected, skipping\n", i);
                    continue;
                }

                // 生成该交换机的专属 YAML 配置
                std::string yaml_content = comm->generate_yaml_for_switch(i);
                if (yaml_content.empty()) {
                    fprintf(stderr, "[Controller] Failed to generate YAML for switch %d\n", i);
                    continue;
                }

                printf("[Controller] Sending YAML to switch %d (%zu bytes):\n%s\n",
                       i, yaml_content.size(), yaml_content.c_str());

                // 发送 YAML 配置
                if (send_yaml_to_switch(fd, yaml_content) < 0) {
                    fprintf(stderr, "[Controller] Failed to send YAML to switch %d\n", i);
                }
            }
            printf("[Controller] YAML configurations sent to all switches\n");

            // 同步将路由文件传给 rank0，让其感知拓扑
            send_file_with_length(client_fd, "/root/topology.yaml");
        }else if(req_type == 'R'){
            // 编写规则
            
        }
    }

    close(client_fd);
}

int main() {
    // 初始化先验拓扑信息：键是交换机控制平面 IP，值是其端口与标识
    std::map<std::string,switch_info> preknowledge_switchtopo;

    {// 1-2-4 拓扑配置：1 个顶层交换机 + 2 个二层交换机
        switch_info info;

        // Switch 0: 顶层交换机 (192.168.0.19)
        info.control_ip = "192.168.0.19";
        info.id = 0;
        info.ports.push_back({"eth1","172.16.0.21","fa:16:3e:9a:26:99"});  // 连接 vm1
        info.ports.push_back({"eth2","10.10.0.14","fa:16:3e:3d:ab:78"});   // 连接 vm2
        preknowledge_switchtopo["192.168.0.19"] = info;

        // Switch 1: 二层交换机 vm1 (192.168.0.6)
        info.ports.clear();
        info.control_ip = "192.168.0.6";
        info.id = 1;
        info.ports.push_back({"eth1","172.16.0.5","fa:16:3e:87:09:4c"});   // 连接顶层交换机
        info.ports.push_back({"eth2","10.1.1.25","fa:16:3e:89:1f:9e"});    // 连接 pku1, pku2
        preknowledge_switchtopo["192.168.0.6"] = info;

        // Switch 2: 二层交换机 vm2 (192.168.0.24)
        info.ports.clear();
        info.control_ip = "192.168.0.24";
        info.id = 2;
        info.ports.push_back({"eth1","10.10.0.7","fa:16:3e:7b:25:36"});    // 连接顶层交换机
        info.ports.push_back({"eth2","10.2.1.18","fa:16:3e:10:c2:d0"});    // 连接 pku3, pku4
        preknowledge_switchtopo["192.168.0.24"] = info;
    }

    // 与所有交换机建立控制连接（作为服务器，等待交换机连接上来）

    int controller_switch_fd = socket(AF_INET, SOCK_STREAM, 0); // 创建 TCP 套接字
    if (controller_switch_fd < 0) {
        perror("socket creation failed");
        return 1;
    }

    int opt = 1; // 启用地址复用，避免端口处于 TIME_WAIT 时无法重启
    if (setsockopt(controller_switch_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        return 1;
    }

    
    sockaddr_in addr1{};                 // 绑定地址结构体
    addr1.sin_family = AF_INET;          // IPv4
    addr1.sin_addr.s_addr = INADDR_ANY;  // 监听本机所有网卡
    addr1.sin_port = htons(CONTROLLER_SWITCH_PORT); // 交换机控制连接端口
    
    if (bind(controller_switch_fd, (sockaddr*)&addr1, sizeof(addr1)) < 0) { // 绑定端口
        perror("bind failed");
        return 1;
    }

    if (listen(controller_switch_fd, 10) < 0) { // 开始监听，最多 10 个排队连接
        perror("listen failed");
        return 1;
    }

    // 依次接受来自各交换机的连接，并根据来源 IP 映射到拓扑中的交换机
    for(int i = 0;i<TOPOLOGY_SIZE;++i){
        printf("waiting for switch %d to connect...\n", i);

        // 这里的鲁棒性很差吧，假设交换机都会按时连接过来
        // 实际使用中应有更复杂的连接管理和错误处理
        // 怎么保证交换机连过来的时序要早于 controller_group 的连接请求？

        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        char client_ip[INET_ADDRSTRLEN];
        int sockfd = accept(controller_switch_fd, (sockaddr*)&client_addr, &addr_len); // 接受一个交换机连接
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);         // 将来源 IP 转为字符串
        switch_info &temp = preknowledge_switchtopo[client_ip]; // 查找该交换机的先验信息
        
        //std::cout << temp.id << std::endl;
        //std::cout << temp.ports.size() << std::endl;
        
        temp.fd = sockfd;                          // 保存连接句柄到临时结构
        switch_topology[temp.id] = temp;           // 放入全局拓扑数组（按交换机 id 索引）
        switch_topology[temp.id].fd = sockfd;      // 再次确保 fd 字段已设置
        //std::cout << switch_topology[temp.id].ports.size() << std::endl;
    }

    printf("connect with switches success.\n"); // 到此完成与全部交换机的控制连接
    // 建立与计算节点 rank0（进程组管理者）的控制连接（同样作为服务器）
    // 同样地：鲁棒性很差，时序上假设交换机都已连上来
    
    int controller_group_fd = socket(AF_INET, SOCK_STREAM, 0); // 创建 TCP 套接字
    if (controller_group_fd < 0) {
        perror("socket creation failed");
        return 1;
    }

    if (setsockopt(controller_group_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) { // 地址复用
        perror("setsockopt failed");
        return 1;
    }

    sockaddr_in addr{};                 // 控制 rank0 的监听地址
    addr.sin_family = AF_INET;          // IPv4
    addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有网卡
    addr.sin_port = htons(CONTROLLER_GROUP_PORT); // 组控制端口
    
    if (bind(controller_group_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { // 绑定端口
        perror("bind failed");
        return 1;
    }

    if (listen(controller_group_fd, 5) < 0) { // 开始监听，队列长度 5
        perror("listen failed");
        return 1;
    }

    // 主循环：接受每个 rank0 控制连接，并为其启动一个会话线程
    while (true) {
        printf("waiting for rank0 to connect...\n");
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(controller_group_fd, (sockaddr*)&client_addr, &addr_len); // 接受一个 rank0 连接
        if (client_fd < 0) {
            perror("accept error");
            continue;
        }

        // 打印客户端信息，便于调试观察
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "New connection from: " << client_ip 
                  << ":" << ntohs(client_addr.sin_port) << std::endl;

        // 创建会话线程：在 group_session 中处理该 rank0 的组/通信器请求
        std::thread(group_session, client_fd).detach();
    }

    close(controller_group_fd); // 关闭监听套接字（正常情况下不会走到这里）
    return 0;                   // 程序结束

    
}