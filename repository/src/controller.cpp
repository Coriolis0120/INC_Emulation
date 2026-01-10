#include "controller.h"

switch_info switch_topology[TOPOLOGY_SIZE];

int controller_group::group_num = 0;
int controller_communicator::communicator_num = 10;


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
    controller_group *group;
    int world_size;
    std::vector<controller_communicator *> comms;
    // 持续处理来自同一 rank0 的控制请求：先建立 group，再可能建立多个 communicator
    while (true) {
        // 读取 1 字节请求类型（'G'：创建组；'C'：创建通信器并分发路由）
        bytes = recv(client_fd, &req_type, 1, MSG_WAITALL);
        printf("begin to recv from rank0.\n");
        if(req_type == 'G'){
            // 组控制：收集 world_size 和各节点 IP，分配 group id 返回给 rank0
            bytes = recv(client_fd, &world_size, sizeof(int), MSG_WAITALL);
            group = new controller_group(world_size);
            for (int i = 0; i < world_size; ++i) {
                recv(client_fd, &group->ip_list[i], sizeof(uint32_t), MSG_WAITALL);
                printf("recv the ip %d.\n",group->ip_list[i]);
            }
            send(client_fd, &group->id, sizeof(uint32_t), 0); // 将 group id 返给 rank0

        }
        else if(req_type == 'C'){
            // 通信器控制：收集各 rank 的 QP 编号，计算路由并将拓扑下发到交换机和 rank0
            controller_communicator *comm = new controller_communicator(group);
            comms.push_back(comm);
            // 收集每个 rank 的 qp 信息
            for (int i = 0; i < world_size; ++i) {
                recv(client_fd, &comm->qp_list[i], sizeof(uint32_t), MSG_WAITALL);
                printf("recv the qp num %d.\n",comm->qp_list[i]);
            }
            
            // 计算路由并生成 YAML（/home/ubuntu/topology.yaml）
            comm->calculate_route(switch_topology);
            printf("route_calculated\n");
            // 先向所有交换机发送其 id，作为后续 YAML 文件的上下文
            for(int i=0;i<TOPOLOGY_SIZE;++i){
                send(switch_topology[i].fd, &i, 4, 0);
            }

            printf("send id to switches.\n");

            // 下发路由文件到所有交换机
            for (int i = 0; i < TOPOLOGY_SIZE; ++i) {
                int fd = switch_topology[i].fd;
                send_file_with_length(fd, "/root/topology.yaml");
            }
            
            // 同步将路由文件传给 rank0，让其感知拓扑
            send_file_with_length(client_fd, "/root/topology.yaml");
        }
    }

    close(client_fd);
}

int main() {
    // 初始化先验拓扑信息：键是交换机控制平面 IP，值是其端口与标识
    std::map<std::string,switch_info> preknowledge_switchtopo;

    {// 这里演示填充先验信息（未来可从配置/外部服务读取）
        switch_info info;
        // info.control_ip = "10.215.8.118";
        // info.id = 0;
        // info.ports.push_back({"ens4","10.0.4.1","52:54:00:12:e0:f5"});
        // info.ports.push_back({"ens5","10.0.5.1","52:54:00:21:b1:50"});
        // preknowledge_switchtopo["10.215.8.118"] = info;
        // info.ports.clear();
        // info.control_ip = "10.215.8.221";
        // info.id = 1;
        // info.ports.push_back({"ens4","10.0.0.1","52:54:00:57:fa:a3"});
        // info.ports.push_back({"ens5","10.0.1.1","52:54:00:5f:de:69"});
        // info.ports.push_back({"ens6","10.0.4.2","52:54:00:60:e0:a1"});
        // preknowledge_switchtopo["10.215.8.221"] = info;
        // info.ports.clear();
        // info.control_ip = "10.215.8.78";
        // info.id = 2;
        // info.ports.push_back({"ens4","10.0.2.1","52:54:00:e4:f6:fa"});
        // info.ports.push_back({"ens5","10.0.3.1","52:54:00:ed:37:6a"});
        // info.ports.push_back({"ens6","10.0.5.2","52:54:00:ef:a1:46"});
        // preknowledge_switchtopo["10.215.8.78"] = info;
        // 当前使用的交换机 0 的示例配置
        info.control_ip = "192.168.0.19"; // 交换机控制 IP（用于匹配连接来源）
        info.id = 0;                       // 拓扑中该交换机的唯一编号
        // 记录交换机的业务端口：网卡名、端口子网的网关 IP、MAC 地址
        info.ports.push_back({"eth1","172.16.0.21","fa:16:3e:9a:26:99"});
        info.ports.push_back({"eth2","10.10.0.14","fa:16:3e:3d:ab:78"});
        // 存入先验映射，后续根据连接的来源 IP 关联到具体交换机信息
        preknowledge_switchtopo["192.168.0.19"] = info;
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