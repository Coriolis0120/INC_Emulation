#include "api.h"


/* 在各个rank节点和rank0之间建立临时连接，以及rank0和controller之间建立连接 */
struct inccl_group *inccl_group_create(int world_size, int rank, const char *master_ip){
    // 分配group结构体内存
    struct inccl_group *group = (struct inccl_group *)malloc(sizeof(struct inccl_group));

    // 保存当前节点的rank编号和总节点数
    group->rank = rank;
    group->world_size = world_size;

    // ========== 初始化本地InfiniBand设备 ==========
    group->local_ib_port = 1;           // 使用IB设备的第1个端口
    group->local_gid_idx = 1;           // 使用GID索引1
    struct ibv_device **dev_list = NULL; // IB设备列表
    int num_devices;                     // 设备数量
    dev_list = ibv_get_device_list(&num_devices); // 获取系统中所有IB设备
    // 调试信息：打印设备数量
    printf("devices num %d\n",num_devices);
    // 遍历并打印所有设备名称
    for(int i = 0; i < num_devices; i ++)
    {
        printf("%s\n",ibv_get_device_name(dev_list[i]));
    }

    // 打开第一个IB设备，获取设备上下文
    group->local_ib_ctx = ibv_open_device(dev_list[0]);

    // 释放设备列表内存
    ibv_free_device_list(dev_list);
    dev_list = NULL;
    // 查询设备属性（如最大QP数量、最大CQ数量等）
    ibv_query_device(group->local_ib_ctx, &group->local_device_attr);

    // 打印设备属性信息
    printf("\n===== Device Attributes =====\n");
    printf("Maximum QPs: %d\n", group->local_device_attr.max_qp);
    printf("Maximum CQs: %d\n", group->local_device_attr.max_cq);
    printf("Maximum MRs: %d\n", group->local_device_attr.max_mr);
    printf("Maximum PDs: %d\n", group->local_device_attr.max_pd);
    printf("Maximum QP WR: %d\n", group->local_device_attr.max_qp_wr);
    printf("Maximum CQ entries: %d\n", group->local_device_attr.max_cqe);
    printf("Maximum MR size: %lu bytes\n", group->local_device_attr.max_mr_size);
    printf("=============================\n\n");

    // 查询端口属性（如MTU、链路状态等）
    ibv_query_port(group->local_ib_ctx, group->local_ib_port, &group->local_port_attr);

    // 打印端口属性信息
    printf("\n===== Port Attributes =====\n");
    printf("Port state: %d (0=DOWN, 1=INIT, 2=ARMED, 3=ACTIVE)\n", group->local_port_attr.state);
    printf("Physical state: %d\n", group->local_port_attr.phys_state);
    printf("LID: 0x%04x (%d)\n", group->local_port_attr.lid, group->local_port_attr.lid);
    printf("Active MTU: %d (256*2^%d = %d bytes)\n",
           group->local_port_attr.active_mtu,
           group->local_port_attr.active_mtu,
           256 * (1 << group->local_port_attr.active_mtu));
    printf("Max MTU: %d (256*2^%d = %d bytes)\n",
           group->local_port_attr.max_mtu,
           group->local_port_attr.max_mtu,
           256 * (1 << group->local_port_attr.max_mtu));
    printf("Active speed: %d\n", group->local_port_attr.active_speed);
    printf("Active width: %d (1=1x, 2=4x, 4=8x, 8=12x)\n", group->local_port_attr.active_width);
    printf("GID table length: %d\n", group->local_port_attr.gid_tbl_len);
    printf("Link layer: %d (0=IB, 1=Ethernet)\n", group->local_port_attr.link_layer);
    printf("============================\n\n");

    // 查询GID（全局标识符，包含IP地址信息）
    ibv_query_gid(group->local_ib_ctx, group->local_ib_port, GID_IDX, &group->local_gid);
    // 打印本地IP地址（GID的最后4字节存储IPv4地址）
    char ip_str[INET_ADDRSTRLEN];
    printf("my ip: %s\n", uint32_to_ip_string_big_endian(*(uint32_t *)(group->local_gid.raw+12)));
    fflush(stdout);  // 强制刷新输出缓冲区
    // group->payload_mtu = (2<<(group->local_port_attr.active_mtu + 7))-inccl_HEADER_LEN;
    // printf("group payload mtu: %d\n", group->payload_mtu);

    // ========== Rank 0的特殊处理：作为master节点 ==========
    if(rank == 0){
        // rank0节点需要：1) 收集所有其他rank的信息  2) 与controller通信
        //group->controller_ip = getenv("CONTROLLER_IP");
        group->controller_ip = getenv("CONTROLLER_IP");  // 保存controller的IP地址
        printf("controller ip: %s \n", group->controller_ip);
        fflush(stdout);  // 强制刷新输出缓冲区
        // 分配数组存储与各个rank的TCP连接文件描述符
        group->group_fd_list = (int *)calloc(world_size, sizeof(int));
        // 分配数组存储各个rank的IP地址
        uint32_t *group_ip_list = (uint32_t *)calloc(world_size, sizeof(uint32_t));
        // 先保存自己（rank0）的IP地址
        memcpy(group_ip_list, group->local_gid.raw+12, sizeof(uint32_t));
        // *** 注意 *** //
        // group_ip_list中存储的是ip的网络字节序表示！大端法！

        // ===== 步骤1：与其他所有rank建立TCP连接 =====
        int sockfd = socket(AF_INET, SOCK_STREAM, 0); // 创建TCP socket
        if (sockfd < 0) {
            perror("Socket creation failed");
            exit(EXIT_FAILURE);
        }

        // 配置服务器地址：监听MASTER_PORT端口（52223）
        struct sockaddr_in addr = {.sin_family=AF_INET, .sin_port=htons(MASTER_PORT), .sin_addr.s_addr=INADDR_ANY};

        // 绑定socket到指定端口
        if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("Bind failed");
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        // 开始监听，最多接受world_size-1个连接（除了自己）
        if (listen(sockfd, world_size - 1) < 0) {
            perror("Listen failed");
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        // 接收来自其他rank的连接和信息
        struct inccl_rank_exchange_info info_buf; // 用于接收rank信息的缓冲区
        for (int i = 1; i < world_size; ++i) {
            // 接受一个rank的连接
            int rankfd = accept(sockfd, NULL, NULL);
            if (rankfd < 0) {
                perror("Accept failed");
                continue;
            }
            // 接收该rank发送的信息（rank编号和IP地址）
            recv(rankfd, &info_buf, sizeof(info_buf), MSG_WAITALL);
            // 保存该rank的连接文件描述符
            group->group_fd_list[info_buf.rank] = rankfd;
            // 保存该rank的IP地址
            group_ip_list[info_buf.rank] = info_buf.ip;
        }

        // 关闭监听socket（已经建立了所有需要的连接）
        close(sockfd);
        printf("connect to ranks success.\n");
        fflush(stdout);  // 强制刷新输出缓冲区

        // ===== 步骤2：与controller建立TCP连接 =====
        group->controller_fd = socket(AF_INET, SOCK_STREAM, 0); // 创建新的TCP socket
        if (group->controller_fd < 0) {
            perror("Socket creation failed");
            return NULL;
        }
        struct sockaddr_in controller_addr;
        // 清零地址结构体
        memset(&controller_addr, 0, sizeof(controller_addr));
        controller_addr.sin_family = AF_INET;  // IPv4协议
        controller_addr.sin_port = htons(CONTROLLER_GROUP_PORT); // controller的group管理端口（52200）
        // 将IP地址字符串转换为网络字节序
        if (inet_pton(AF_INET, group->controller_ip, &controller_addr.sin_addr) <= 0) {
            perror("Invalid IP address");
            close(group->controller_fd);
            return NULL;
        }

        // 连接到controller
        if (connect(group->controller_fd, (struct sockaddr*)&controller_addr, sizeof(controller_addr)) < 0) {
            perror("Connection failed");
            close(group->controller_fd);
            return NULL;
        }

        printf("connect to controller success.\n");
        fflush(stdout);  // 强制刷新输出缓冲区

        // ===== 步骤3：向controller注册group并获取group_id =====
        const char *prompt = "G"; // 发送"G"表示这是Group注册请求
        send(group->controller_fd, prompt, 1, 0);
        // 发送总节点数
        send(group->controller_fd, &world_size, sizeof(int), 0);
        // 发送所有节点的IP地址列表
        for (int i = 0; i < world_size; ++i) {
            send(group->controller_fd, &group_ip_list[i], sizeof(uint32_t), 0);
        }

        // 接收controller分配的group_id
        recv(group->controller_fd, &group->group_id, sizeof(int), MSG_WAITALL);
        printf("group id: %d!\n",group->group_id);
        fflush(stdout);  // 强制刷新输出缓冲区
    }
    else{
        // ========== 非Rank 0节点的处理：作为worker节点 ==========
        // 这些节点只需要连接到rank0（master）并发送自己的信息
        group->master_ip = master_ip;  // 保存master（rank0）的IP地址

        // 创建TCP socket连接到master
        group->master_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (group->master_fd < 0) {
            perror("Socket creation failed");
            exit(EXIT_FAILURE);
        }

        // 配置master的地址信息
        struct sockaddr_in master_addr;
        memset(&master_addr, 0, sizeof(master_addr));
        master_addr.sin_family = AF_INET;  // IPv4协议
        master_addr.sin_port = htons(MASTER_PORT);  // master监听的端口（52223）
        // 将master的IP地址字符串转换为网络字节序
        if (inet_pton(AF_INET, master_ip, &master_addr.sin_addr) <= 0) {
            perror("Invalid IP address");
            close(group->master_fd);
            return NULL;
        }

        // 连接到master（rank0）
        if (connect(group->master_fd, (struct sockaddr*)&master_addr, sizeof(master_addr)) < 0) {
            perror("Connection failed");
            close(group->master_fd);
            return NULL;
        }
        printf("connect to master success.\n");
        fflush(stdout);  // 强制刷新输出缓冲区

        // 发送本节点的信息给master
        struct inccl_rank_exchange_info info_buf;
        // 从GID中提取本地IP地址（最后4字节）
        memcpy(&info_buf.ip, group->local_gid.raw+12, sizeof(uint32_t));
        info_buf.rank = rank;  // 设置rank编号
        // 发送信息结构体给master
        send(group->master_fd, &info_buf, sizeof(info_buf), 0);
    }

    return group;  // 返回创建好的group结构体
}



/* 销毁group，释放资源 */
int inccl_group_destroy(struct inccl_group *group) {
    printf("destory...\n");
    return 1;
}

/* 创建communicator：分配RDMA资源并建立与switch的QP连接 */
struct inccl_communicator *inccl_communicator_create(struct inccl_group *group, uint32_t size){
    // 分配communicator结构体内存
    struct inccl_communicator *comm = (struct inccl_communicator *)malloc(sizeof(struct inccl_communicator));
    comm->group = group;  // 关联到group

    // ========== 步骤1：分配RDMA基础资源 ==========

    // 分配Protection Domain（保护域）：RDMA的安全边界，所有资源必须属于同一个PD
    comm->pd = ibv_alloc_pd(group->local_ib_ctx);

    // 计算segment数量（每个segment是4KB）
    int segment_num = size / 1024 / 4;
    //printf("segment_num: %d\n",segment_num);

    // 缓冲区大小是请求大小的2倍（发送和接收各一份）
    comm->payload_buf_size = size * 2;
    //printf("payload_buf_size: %d\n",comm->payload_buf_size);

    // 创建Completion Queue（完成队列）：用于通知RDMA操作完成
    comm->cq = ibv_create_cq(group->local_ib_ctx, segment_num, NULL, NULL,0);

    // ========== 步骤2：分配并注册内存区域（Memory Region） ==========
    // 分配发送缓冲区
    comm->send_payload = (char *)malloc(sizeof(char)*comm->payload_buf_size);
    // 分配接收缓冲区
    comm->receive_payload = (char *)malloc(sizeof(char)*comm->payload_buf_size);

    // 清零缓冲区
    memset(comm->send_payload, 0 , comm->payload_buf_size);
    memset(comm->receive_payload, 0 , comm->payload_buf_size);

    // 设置MR访问权限：本地写、远程读、远程写
    int mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE ;
    // 注册发送缓冲区为MR（让RDMA网卡可以直接访问这块内存）
    comm->mr_send_payload = ibv_reg_mr(comm->pd, comm->send_payload, comm->payload_buf_size, mr_flags);
    // 注册接收缓冲区为MR
    comm->mr_receive_payload = ibv_reg_mr(comm->pd, comm->receive_payload, comm->payload_buf_size, mr_flags);

    // ========== 步骤3：创建Queue Pair（队列对） ==========
    // 先创建dummy QP以确保不同rank的QP号不同
    struct ibv_qp **dummy_qps = NULL;
    if (group->rank > 0) {
        dummy_qps = (struct ibv_qp **)malloc(group->rank * sizeof(struct ibv_qp *));
        struct ibv_qp_init_attr dummy_attr;
        memset(&dummy_attr, 0, sizeof(dummy_attr));
        dummy_attr.qp_type = IBV_QPT_RC;
        dummy_attr.send_cq = comm->cq;
        dummy_attr.recv_cq = comm->cq;
        dummy_attr.cap.max_send_wr = 1;
        dummy_attr.cap.max_recv_wr = 1;
        dummy_attr.cap.max_send_sge = 1;
        dummy_attr.cap.max_recv_sge = 1;
        for (int i = 0; i < group->rank; i++) {
            dummy_qps[i] = ibv_create_qp(comm->pd, &dummy_attr);
        }
    }

    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type = IBV_QPT_RC;  // RC = Reliable Connection（可靠连接）
    qp_init_attr.sq_sig_all = 1;  // 所有发送操作都生成完成事件
    qp_init_attr.send_cq = comm->cq;  // 发送完成队列
    qp_init_attr.recv_cq = comm->cq;  // 接收完成队列
    qp_init_attr.cap.max_send_wr = segment_num;  // 最大发送工作请求数
    qp_init_attr.cap.max_recv_wr = segment_num;  // 最大接收工作请求数
    qp_init_attr.cap.max_send_sge = 2;  // 每个发送WR的最大scatter-gather元素数
    qp_init_attr.cap.max_recv_sge = 2;  // 每个接收WR的最大scatter-gather元素数
    comm->qp = ibv_create_qp(comm->pd, &qp_init_attr);  // 创建QP
    printf("my QP number = %d\n", comm->qp->qp_num);  // 打印QP编号（用于连接）
    // ========== 步骤4：交换QP号并获取拓扑配置 ==========
    if(group->rank == 0){
        // Rank 0：收集所有QP号，发送给controller，接收并分发topology.yaml

        // 分配数组存储所有rank的QP号
        uint32_t *group_qp_num_list = (uint32_t *)calloc(group->world_size, sizeof(uint32_t));
        group_qp_num_list[0] = comm->qp->qp_num;  // 先保存自己的QP号

        // 从其他所有rank接收QP号
        for (int i = 1; i < group->world_size; ++i) {
            recv(group->group_fd_list[i], &group_qp_num_list[i], sizeof(uint32_t), MSG_WAITALL);
        }

        // 向controller发送"C"表示Communicator创建请求
        const char *prompt = "C"; //COMMUNICATOR
        send(group->controller_fd, prompt, 1, 0);
        // 发送所有rank的QP号给controller
        for (int i = 0; i < group->world_size; ++i) {
            send(group->controller_fd, &group_qp_num_list[i], sizeof(uint32_t), 0);
            printf("rank%d qp num %d\n",i,group_qp_num_list[i]);
        }

        // 从controller接收topology.yaml文件并保存
        receive_file(group->controller_fd, "/root/NetLab/topology.yaml");
        // 将topology.yaml分发给所有其他rank
        for (int i = 1; i < group->world_size; ++i) {
            send_file_with_length(group->group_fd_list[i], "/root/NetLab/topology.yaml");
        }
    }
    else{
        // 非Rank 0：发送QP号给master，接收topology.yaml

        // 发送本节点的QP号给master
        send(group->master_fd, &comm->qp->qp_num, sizeof(uint32_t), 0);
        // 从master接收topology.yaml文件
        receive_file(group->master_fd, "/root/NetLab/topology.yaml");
    }

    // ========== 步骤5：解析拓扑配置，获取switch信息 ==========
    // 从topology.yaml中获取本节点对应的switch的IP和QP号（网络字节序）
    uint32_t switch_ip;
    uint32_t switch_qp_num;
    printf("parse yaml\n");
    get_switch_info("/root/NetLab/topology.yaml", group->rank, &switch_ip, &switch_qp_num);

    // 设置滑动窗口大小（流控参数）
    // 实际应用中需要与switch协商确定窗口大小
    comm->window_size = WINDOW_SIZE;

    // ========== 步骤6：准备连接到switch的QP ==========
    // 构造switch的GID：复制本地GID，然后替换最后4字节为switch的IP
    union ibv_gid switch_gid = group->local_gid;
    memcpy(switch_gid.raw+12,&switch_ip,4);  // 将switch IP写入GID的最后4字节

    // 打印连接信息（用于调试）
    //fprintf(stdout, "My QP number = 0x%x\n", comm->qp->qp_num);
    fprintf(stdout, "Remote QP number = 0x%x\n", switch_qp_num);
    fprintf(stdout, "Remote GID = %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
                switch_gid.raw[0], switch_gid.raw[1], switch_gid.raw[2], switch_gid.raw[3], switch_gid.raw[4], switch_gid.raw[5], switch_gid.raw[6], switch_gid.raw[7], switch_gid.raw[8], switch_gid.raw[9], switch_gid.raw[10], switch_gid.raw[11], switch_gid.raw[12], switch_gid.raw[13], switch_gid.raw[14], switch_gid.raw[15]);

    // QP属性结构体和标志位
    struct ibv_qp_attr attr;
    int qp_flags;
    // ===== QP状态转换1：RESET -> INIT =====
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;  // 目标状态：INIT
    attr.port_num = group->local_ib_port;  // 使用的物理端口号
    attr.pkey_index = 0;  // Partition Key索引（用于隔离不同的通信域）
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;  // 访问权限
    qp_flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;  // 指定要修改的属性
    int ret = ibv_modify_qp(comm->qp, &attr, qp_flags);  // 执行状态转换
    printf("ret of init %d\n",ret);  // 打印返回值（0表示成功）
    // ===== QP状态转换2：INIT -> RTR (Ready To Receive) =====
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;  // 目标状态：RTR（准备接收）
    attr.path_mtu = group->local_port_attr.active_mtu;  // 路径MTU（最大传输单元）
    printf("mtu: %d\n",attr.path_mtu);
    attr.dest_qp_num = switch_qp_num;  // 目标QP号（switch的QP号）
    attr.rq_psn = 0;  // 接收队列的起始PSN（Packet Sequence Number）
    attr.max_dest_rd_atomic = 1;  // 目标端最大未完成的RDMA读/原子操作数
    attr.min_rnr_timer = 12;  // RNR (Receiver Not Ready) 最小重试时间

    // 配置Address Handle（地址句柄）：用于指定数据包的目标地址
    attr.ah_attr.dlid = 0;  // 目标LID（对于RoCE v2不使用）
    attr.ah_attr.sl = 0;  // Service Level（服务等级）
    attr.ah_attr.src_path_bits = 0;  // 源路径位
    attr.ah_attr.is_global = 1;  // 使用全局路由（RoCE v2必须为1）
    attr.ah_attr.port_num = group->local_ib_port;  // 物理端口号

    // 配置GRH (Global Routing Header)：RoCE v2的路由信息
    attr.ah_attr.grh.dgid = switch_gid;  // 目标GID（switch的GID）
    attr.ah_attr.grh.hop_limit = 64;  // 跳数限制（类似IP的TTL）
    attr.ah_attr.grh.sgid_index = group->local_gid_idx;  // 源GID索引
    attr.ah_attr.grh.flow_label = 0;  // 流标签
    attr.ah_attr.grh.traffic_class = 0;  // 流量类别（类似IP的ToS）

    // 指定要修改的属性标志
    qp_flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
            IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    ret = ibv_modify_qp(comm->qp, &attr, qp_flags);  // 执行状态转换
    //modify_qp_to_rtr(comm->qp,attr.dest_qp_num,0,&switch_gid);
    printf("ret of rtr %d\n",ret);  // 打印返回值
    printf("qp status: %d\n",comm->qp->state);  // 打印当前QP状态
    // ===== QP状态转换3：RTR -> RTS (Ready To Send) =====
    attr.qp_state = IBV_QPS_RTS;  // 目标状态：RTS（准备发送）
    attr.timeout = 0x12;  // 超时时间（用于重传机制）
    attr.retry_cnt = 6;  // 重试次数
    attr.rnr_retry = 0;  // RNR重试次数（0表示无限重试）
    attr.sq_psn = 0;  // 发送队列的起始PSN
    attr.max_rd_atomic = 1;  // 本端最大未完成的RDMA读/原子操作数
    qp_flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    ret = ibv_modify_qp(comm->qp, &attr, qp_flags);  // 执行状态转换
    printf("ret of rts %d\n",ret);  // 打印返回值
    sleep(1);  // 等待1秒，确保连接稳定
    return comm;  // 返回创建好的communicator
}


/* 销毁communicator，释放RDMA资源 */
int inccl_communicator_destroy(struct inccl_communicator *comm);

/* 发送单个消息的辅助函数
 * comm: communicator对象
 * src_data: 源数据数组
 * idx: 消息索引（用于计算缓冲区偏移）
 * opcode: RDMA操作类型（IBV_WR_SEND 或 IBV_WR_RDMA_WRITE）
 */
static int post_send(struct inccl_communicator *comm, int32_t* src_data, int idx, enum ibv_wr_opcode opcode) {
    struct ibv_send_wr wr;           // 发送工作请求（Work Request）
    struct ibv_sge send_sge;         // Scatter-Gather元素（描述内存缓冲区）
    struct ibv_send_wr *send_bad_wr; // 用于返回失败的WR

    // 计算发送缓冲区的指针位置（每个消息占PAYLOAD_COUNT个int32_t）
    int *send_payload = (int *)(comm->send_payload + idx * PAYLOAD_COUNT * sizeof(int32_t));

    // 将数据从源缓冲区复制到发送缓冲区，并转换为网络字节序（大端）
    for(int i=0;i<PAYLOAD_COUNT;++i){
        send_payload[i] = htonl((src_data + idx * PAYLOAD_COUNT)[i]);
    }

    //memcpy(send_payload, src_data + idx * PAYLOAD_COUNT, PAYLOAD_COUNT * sizeof(uint32_t));

    // 配置Scatter-Gather元素（描述要发送的内存区域）
    memset(&send_sge, 0, sizeof(send_sge));
    send_sge.addr = (uintptr_t)(comm->send_payload + idx * PAYLOAD_COUNT * sizeof(int32_t));  // 缓冲区地址
    send_sge.length = PAYLOAD_COUNT * sizeof(int32_t);  // 数据长度
    send_sge.lkey = comm->mr_send_payload->lkey;  // 本地内存区域的key

    // 配置发送工作请求（Work Request）
    memset(&wr, 0, sizeof(wr));
    wr.next = NULL;  // 链表中的下一个WR（NULL表示只有一个）
    wr.wr_id = idx;  // 工作请求ID（用于在完成队列中识别）
    wr.sg_list = &send_sge;  // Scatter-Gather列表
    wr.num_sge = 1;  // SG元素数量
    wr.opcode = opcode ;  // 操作类型（SEND或RDMA_WRITE）
    wr.send_flags = IBV_SEND_SIGNALED; // 生成完成事件（用于确认发送完成）

    // 如果是RDMA WRITE操作，需要指定远程内存地址
    if(opcode == IBV_WR_RDMA_WRITE){
        wr.wr.rdma.remote_addr = (uintptr_t)(comm->receive_payload + idx * PAYLOAD_COUNT * sizeof(int32_t));  // 远程地址
        wr.wr.rdma.rkey = comm->mr_receive_payload->rkey;  // 远程内存区域的key
    }

    // 提交发送请求到发送队列
    return ibv_post_send(comm->qp, &wr, &send_bad_wr);
}

/* AllReduce操作：使用SEND/RECV语义（双边RDMA）
 * comm: communicator对象
 * src_data: 源数据数组
 * len: 数据元素个数（int32_t的数量）
 * dst_data: 目标数据数组（存储聚合后的结果）
 */
void inccl_allreduce_sendrecv(struct inccl_communicator *comm, int32_t* src_data, uint32_t len, int32_t* dst_data) {
    int receive_num = 0;  // 已接收的消息数量
    int send_num = 0;     // 已发送的消息数量
    int message_num = len / PAYLOAD_COUNT; // 总消息数（每个消息包含PAYLOAD_COUNT个元素）
    // 隐含约束：len必须是PAYLOAD_COUNT的整数倍
    if (len % PAYLOAD_COUNT != 0) {
        printf("Error: len must be a multiple of PAYLOAD_COUNT (%ld)\n", PAYLOAD_COUNT);
        return;
    }


    struct ibv_recv_wr rr;           // 接收工作请求
    struct ibv_sge receive_sge;      // 接收的Scatter-Gather元素
    struct ibv_recv_wr *receive_bad_wr;  // 用于返回失败的接收WR
           
    // ===== 步骤1：预先提交所有接收请求 =====
    // SEND/RECV语义要求接收方必须先post receive，发送方才能发送
    for(int i = 0; i < message_num; i++) {
        // 配置接收缓冲区的SGE
        memset(&receive_sge, 0, sizeof(receive_sge));
        receive_sge.addr = (uintptr_t)(comm->receive_payload + i * PAYLOAD_COUNT * sizeof(int32_t));  // 接收缓冲区地址
        receive_sge.length = PAYLOAD_COUNT * sizeof(int32_t);  // 接收数据长度(字节数)
        receive_sge.lkey = comm->mr_receive_payload->lkey;  // 本地MR的key

        // 配置接收工作请求
        memset(&rr, 0, sizeof(rr));
        rr.next = NULL;  // 单个WR
        rr.wr_id = i;    // 工作请求ID
        rr.sg_list = &receive_sge;  // SG列表
        rr.num_sge = 1;  // SG元素数量

        // 提交接收请求到接收队列
        int ret = ibv_post_recv(comm->qp, &rr, &receive_bad_wr);
        printf("i: %d, post recv ret %d\n", i, ret);
        fflush(stdout);
    }

    // ===== 步骤2：发送初始窗口内的消息 =====
    // 使用滑动窗口机制：先发送window_size范围内的消息
    for(int i = 0; i < comm->window_size/(MESSAGE_SIZE); i++) { // MESSAGE_SIZE = 4KB，代表一个payload的大小（字节数）
        post_send(comm, src_data, i, IBV_WR_SEND);  // 使用SEND操作
        send_num++;  // 增加已发送计数
    }

    // ===== 步骤3：轮询完成队列，处理完成事件 =====
    // 分配工作完成（Work Completion）数组
    struct ibv_wc *wc = (struct ibv_wc *)malloc(sizeof(struct ibv_wc)*message_num); 
    // PAYLOAD_COUNT: 一个payload的元素大小（int32_t数量）
    // message_num: 总消息有多少个payload

    // 循环直到接收到所有消息
    while(receive_num != message_num) {
        // 轮询完成队列，最多获取message_num个完成事件
        int result = ibv_poll_cq(comm->cq, message_num, wc);
        if(result > 0) {  // 有完成事件
            // printf("\n");
            // 遍历所有完成事件
            for(int i = 0; i < result; i++){
                struct ibv_wc *tmp = wc + i;  // 获取当前完成事件
                // printf("tmp->status %d\n", tmp->status);
                // printf("tmp->opcode %d\n", tmp->opcode);

                // 处理接收完成事件
                if(tmp->status==IBV_WC_SUCCESS && tmp->opcode==IBV_WC_RECV) {
                    printf("receive success\n");
                    fflush(stdout);
                    uint64_t idx = tmp->wr_id;  // 获取工作请求ID
                    // 获取接收缓冲区指针
                    int *pack = (int *)(comm->receive_payload + idx * PAYLOAD_COUNT * sizeof(int32_t));

                    // 将数据从接收缓冲区复制到目标缓冲区，并转换为主机字节序
                    for(int j = 0; j <PAYLOAD_COUNT; ++j){
                        uint32_t net_val = pack[j];
                        uint32_t host_val = ntohl(net_val);
                        (dst_data + idx * PAYLOAD_COUNT)[j] = host_val;

                        // 打印前3个元素的调试信息
                        if (j < 3) {
                            printf("recv[%lu][%d]: net=0x%08x, host=%u, stored=%d\n",
                                   idx, j, net_val, host_val, (dst_data + idx * PAYLOAD_COUNT)[j]);
                        }
                    }

                    //memcpy(dst_data + receive_num * PAYLOAD_COUNT, pack, PAYLOAD_COUNT * sizeof(int32_t));
                    receive_num++;  // 增加已接收计数

                    // 滑动窗口：接收一个就发送下一个（如果还有未发送的）
                    if(send_num < message_num) {
                        post_send(comm, src_data, send_num, IBV_WR_SEND);
                        send_num++;
                    }
                } else if(tmp->status==IBV_WC_SUCCESS) {
                    // 处理发送完成事件
                    printf("send success\n");
                    fflush(stdout);
                    // if(send_num < message_num) {
                    //     post_send(comm, src_data, send_num);
                    //     send_num++;
                    // }
                } else {
                    // 处理错误情况
                    printf("what???? wc status: %d, opcode: %d\n", tmp->status, tmp->opcode);
                    fflush(stdout);
                    return;
                }

            }
        }else if(result < 0) {
            // 处理轮询错误
            printf("ibv_poll_cq error: %d\n", result);
            fflush(stdout);
            return;
        }
    }
}

/* AllReduce操作：使用RDMA WRITE语义（单边RDMA）
 * comm: communicator对象
 * src_data: 源数据数组
 * len: 数据元素个数（int32_t的数量）
 * dst_data: 目标数据数组（存储聚合后的结果）
 *
 * 与sendrecv的区别：RDMA WRITE不需要接收方预先post receive
 */
void inccl_allreduce_write(struct inccl_communicator *comm, int32_t* src_data, uint32_t len, int32_t* dst_data) {
    printf("allreduce write...\n");
    int receive_num = 0;  // 已接收的消息数量
    int send_num = 0;     // 已发送的消息数量
    int message_num = len / PAYLOAD_COUNT; // 总消息数

    // ===== 步骤1：发送初始窗口内的消息 =====
    // RDMA WRITE不需要预先post receive，直接发送即可
    for(int i = 0; i < comm->window_size/(MESSAGE_SIZE) && i < message_num; i++) {
        printf("post send idx %d\n", i);
        fflush(stdout);
        post_send(comm, src_data, i, IBV_WR_RDMA_WRITE);  // 使用RDMA_WRITE操作
        send_num++;
    }
    printf("initial send done, send_num %d\n", send_num);
    fflush(stdout); 

    // ===== 步骤2：轮询完成队列，处理完成事件 =====
    struct ibv_wc *wc = (struct ibv_wc *)malloc(sizeof(struct ibv_wc)*message_num);
    while(receive_num != message_num) {
        
        // 轮询完成队列
        int result = ibv_poll_cq(comm->cq, message_num, wc);

        if(result > 0) {
            // 遍历所有完成事件
            for(int i = 0; i < result; i++){
                struct ibv_wc *tmp = wc + i;
                printf("tmp->status %d\n", tmp->status);
                printf("tmp->opcode %d\n", tmp->opcode);
                fflush(stdout);

                // 处理RDMA WRITE完成事件
                if(tmp->status==IBV_WC_SUCCESS && tmp->opcode==IBV_WC_RDMA_WRITE) {
                    printf("receive success\n");
                    fflush(stdout);
                    uint64_t idx = tmp->wr_id;  // 获取工作请求ID
                    // 获取接收缓冲区指针（RDMA WRITE直接写入接收缓冲区）
                    int *pack = (int *)(comm->receive_payload + idx * PAYLOAD_COUNT * sizeof(int32_t));

                    // 将数据从接收缓冲区复制到目标缓冲区，并转换为主机字节序
                    for(int j = 0; j <PAYLOAD_COUNT; ++j){
                        (dst_data + idx * PAYLOAD_COUNT)[j] = ntohl(pack[j]);
                    }

                    //memcpy(dst_data + receive_num * PAYLOAD_COUNT, pack, PAYLOAD_COUNT * sizeof(int32_t));
                    receive_num++;  // 增加已接收计数

                    // 滑动窗口：完成一个就发送下一个（如果还有未发送的）
                    if(send_num < message_num) {
                        post_send(comm, src_data, send_num, IBV_WR_RDMA_WRITE);
                        send_num++;
                    }
                } else if(tmp->status==IBV_WC_SUCCESS) {
                    // 处理其他成功的完成事件
                    printf("send success\n");
                    fflush(stdout);
                    // if(send_num < message_num) {
                    //     post_send(comm, src_data, send_num);
                    //     send_num++;
                    // }
                } else {
                    // 处理错误情况
                    printf("what???? wc status: %d, opcode: %d\n", tmp->status, tmp->opcode);
                    fflush(stdout);
                }

            }
        }else if (result == 0){
            // 没有完成事件，继续轮询
        } else {
            // 处理轮询错误
            printf("ibv_poll_cq error: %d\n", result);
            fflush(stdout);
            return;
        }
        
        
    }
}

/**
 * @brief Reduce操作 - SEND/RECV模式
 * 
 * 所有节点发送数据到交换机进行聚合，但只有root_rank接收聚合结果
 * 
 * @param comm 通信器
 * @param src_data 源数据缓冲区
 * @param len 数据长度（int32_t元素个数）
 * @param dst_data 目标数据缓冲区（仅root_rank使用）
 * @param root_rank 接收聚合结果的根节点rank
 */
void inccl_reduce_sendrecv(struct inccl_communicator *comm, int32_t* src_data, uint32_t len, int32_t* dst_data, int root_rank) {
    int receive_num = 0;  // 已接收的消息数量
    int send_num = 0;     // 已发送的消息数量
    int message_num = len / PAYLOAD_COUNT; // 总消息数
    
    // 检查len是否是PAYLOAD_COUNT的整数倍
    if (len % PAYLOAD_COUNT != 0) {
        printf("Error: len must be a multiple of PAYLOAD_COUNT (%ld)\n", PAYLOAD_COUNT);
        return;
    }

    int my_rank = comm->group->rank;
    bool is_root = (my_rank == root_rank);

    struct ibv_recv_wr rr;
    struct ibv_sge receive_sge;
    struct ibv_recv_wr *receive_bad_wr;

    // ===== 步骤1：仅root节点预先提交接收请求 =====
    if (is_root) {
        for(int i = 0; i < message_num; i++) {
            memset(&receive_sge, 0, sizeof(receive_sge));
            receive_sge.addr = (uintptr_t)(comm->receive_payload + i * PAYLOAD_COUNT * sizeof(int32_t));
            receive_sge.length = PAYLOAD_COUNT * sizeof(int32_t);
            receive_sge.lkey = comm->mr_receive_payload->lkey;

            memset(&rr, 0, sizeof(rr));
            rr.next = NULL;
            rr.wr_id = i;
            rr.sg_list = &receive_sge;
            rr.num_sge = 1;

            int ret = ibv_post_recv(comm->qp, &rr, &receive_bad_wr);
            printf("Reduce root rank %d: post recv ret %d\n", my_rank, ret);
            fflush(stdout);
        }
    }

    // ===== 步骤2：所有节点发送数据（包含操作类型和root_rank元数据） =====
    // 在发送缓冲区的前两个uint32_t存储元数据
    int32_t *send_buffer = (int32_t*)comm->send_payload;
    
    // 准备发送数据：前两个元素是元数据，后面是实际数据
    for(int i = 0; i < message_num; i++) {
        int32_t *msg_buffer = send_buffer + i * PAYLOAD_COUNT;
        
        // 第一个元素：操作类型（OPERATION_REDUCE = 1）
        msg_buffer[0] = htonl(1);  // OPERATION_REDUCE
        // 第二个元素：root_rank
        msg_buffer[1] = htonl(root_rank);
        
        // 复制实际数据（从第3个元素开始）
        for(int j = 2; j < PAYLOAD_COUNT; j++) {
            int src_idx = i * PAYLOAD_COUNT + (j - 2);
            if(src_idx < len) {
                msg_buffer[j] = htonl(src_data[src_idx]);
            } else {
                msg_buffer[j] = 0;  // 填充0
            }
        }
    }

    // 发送初始窗口内的消息
    for(int i = 0; i < comm->window_size/(MESSAGE_SIZE); i++) {
        post_send(comm, send_buffer, i, IBV_WR_SEND);
        send_num++;
    }

    // ===== 步骤3：轮询完成队列 =====
    struct ibv_wc *wc = (struct ibv_wc *)malloc(sizeof(struct ibv_wc)*message_num);

    if (is_root) {
        // Root节点需要接收结果
        while(receive_num != message_num) {
            int result = ibv_poll_cq(comm->cq, message_num, wc);
            if(result > 0) {
                for(int i = 0; i < result; i++){
                    struct ibv_wc *tmp = wc + i;

                    if(tmp->status==IBV_WC_SUCCESS && tmp->opcode==IBV_WC_RECV) {
                        printf("Reduce root receive success\n");
                        fflush(stdout);
                        uint64_t idx = tmp->wr_id;
                        int *pack = (int *)(comm->receive_payload + idx * PAYLOAD_COUNT * sizeof(int32_t));

                        // 复制数据到目标缓冲区（跳过前两个元数据元素）
                        for(int j = 2; j < PAYLOAD_COUNT; ++j){
                            int dst_idx = idx * PAYLOAD_COUNT + (j - 2);
                            if(dst_idx < len) {
                                uint32_t net_val = pack[j];
                                uint32_t host_val = ntohl(net_val);
                                dst_data[dst_idx] = host_val;
                            }
                        }

                        receive_num++;

                        // 滑动窗口
                        if(send_num < message_num) {
                            post_send(comm, send_buffer, send_num, IBV_WR_SEND);
                            send_num++;
                        }
                    } else if(tmp->status==IBV_WC_SUCCESS) {
                        printf("Reduce root send success\n");
                        fflush(stdout);
                    } else {
                        printf("Reduce root error: wc status: %d, opcode: %d\n", tmp->status, tmp->opcode);
                        fflush(stdout);
                        free(wc);
                        return;
                    }
                }
            } else if(result < 0) {
                printf("Reduce root ibv_poll_cq error: %d\n", result);
                fflush(stdout);
                free(wc);
                return;
            }
        }
    } else {
        // 非root节点只需要等待发送完成
        int send_complete = 0;
        while(send_complete < send_num) {
            int result = ibv_poll_cq(comm->cq, message_num, wc);
            if(result > 0) {
                for(int i = 0; i < result; i++){
                    struct ibv_wc *tmp = wc + i;

                    if(tmp->status==IBV_WC_SUCCESS && tmp->opcode==IBV_WC_SEND) {
                        printf("Reduce non-root rank %d send success\n", my_rank);
                        fflush(stdout);
                        send_complete++;
                        
                        // 滑动窗口
                        if(send_num < message_num) {
                            post_send(comm, send_buffer, send_num, IBV_WR_SEND);
                            send_num++;
                        }
                    } else if(tmp->status!=IBV_WC_SUCCESS) {
                        printf("Reduce non-root rank %d error: wc status: %d, opcode: %d\n", 
                               my_rank, tmp->status, tmp->opcode);
                        fflush(stdout);
                        free(wc);
                        return;
                    }
                }
            } else if(result < 0) {
                printf("Reduce non-root rank %d ibv_poll_cq error: %d\n", my_rank, result);
                fflush(stdout);
                free(wc);
                return;
            }
        }
    }

    free(wc);
    printf("Reduce operation completed for rank %d\n", my_rank);
    fflush(stdout);
}

/**
 * @brief Reduce操作 - RDMA WRITE模式
 * 
 * 所有节点发送数据到交换机进行聚合，但只有root_rank接收聚合结果
 * 使用RDMA WRITE操作，性能更高
 * 
 * @param comm 通信器
 * @param src_data 源数据缓冲区
 * @param len 数据长度（int32_t元素个数）
 * @param dst_data 目标数据缓冲区（仅root_rank使用）
 * @param root_rank 接收聚合结果的根节点rank
 */
void inccl_reduce_write(struct inccl_communicator *comm, int32_t* src_data, uint32_t len, int32_t* dst_data, int root_rank) {
    int receive_num = 0;
    int send_num = 0;
    int message_num = len / PAYLOAD_COUNT;
    
    if (len % PAYLOAD_COUNT != 0) {
        printf("Error: len must be a multiple of PAYLOAD_COUNT (%ld)\n", PAYLOAD_COUNT);
        return;
    }

    int my_rank = comm->group->rank;
    bool is_root = (my_rank == root_rank);

    struct ibv_recv_wr rr;
    struct ibv_sge receive_sge;
    struct ibv_recv_wr *receive_bad_wr;

    // Root节点预先提交接收请求
    if (is_root) {
        for(int i = 0; i < message_num; i++) {
            memset(&receive_sge, 0, sizeof(receive_sge));
            receive_sge.addr = (uintptr_t)(comm->receive_payload + i * PAYLOAD_COUNT * sizeof(int32_t));
            receive_sge.length = PAYLOAD_COUNT * sizeof(int32_t);
            receive_sge.lkey = comm->mr_receive_payload->lkey;

            memset(&rr, 0, sizeof(rr));
            rr.next = NULL;
            rr.wr_id = i;
            rr.sg_list = &receive_sge;
            rr.num_sge = 1;

            int ret = ibv_post_recv(comm->qp, &rr, &receive_bad_wr);
            printf("Reduce WRITE root rank %d: post recv ret %d\n", my_rank, ret);
            fflush(stdout);
        }
    }

    // 准备发送数据（包含元数据）
    int32_t *send_buffer = (int32_t*)comm->send_payload;
    
    for(int i = 0; i < message_num; i++) {
        int32_t *msg_buffer = send_buffer + i * PAYLOAD_COUNT;
        
        // 元数据：操作类型和root_rank
        msg_buffer[0] = htonl(1);  // OPERATION_REDUCE
        msg_buffer[1] = htonl(root_rank);
        
        // 实际数据
        for(int j = 2; j < PAYLOAD_COUNT; j++) {
            int src_idx = i * PAYLOAD_COUNT + (j - 2);
            if(src_idx < len) {
                msg_buffer[j] = htonl(src_data[src_idx]);
            } else {
                msg_buffer[j] = 0;
            }
        }
    }

    // 使用RDMA WRITE发送
    for(int i = 0; i < comm->window_size/(MESSAGE_SIZE); i++) {
        post_send(comm, send_buffer, i, IBV_WR_RDMA_WRITE);
        send_num++;
    }

    struct ibv_wc *wc = (struct ibv_wc *)malloc(sizeof(struct ibv_wc)*message_num);

    if (is_root) {
        // Root节点接收结果
        while(receive_num != message_num) {
            int result = ibv_poll_cq(comm->cq, message_num, wc);
            if(result > 0) {
                for(int i = 0; i < result; i++){
                    struct ibv_wc *tmp = wc + i;

                    if(tmp->status==IBV_WC_SUCCESS && tmp->opcode==IBV_WC_RECV) {
                        printf("Reduce WRITE root receive success\n");
                        fflush(stdout);
                        uint64_t idx = tmp->wr_id;
                        int *pack = (int *)(comm->receive_payload + idx * PAYLOAD_COUNT * sizeof(int32_t));

                        for(int j = 2; j < PAYLOAD_COUNT; ++j){
                            int dst_idx = idx * PAYLOAD_COUNT + (j - 2);
                            if(dst_idx < len) {
                                uint32_t net_val = pack[j];
                                uint32_t host_val = ntohl(net_val);
                                dst_data[dst_idx] = host_val;
                            }
                        }

                        receive_num++;

                        if(send_num < message_num) {
                            post_send(comm, send_buffer, send_num, IBV_WR_RDMA_WRITE);
                            send_num++;
                        }
                    } else if(tmp->status==IBV_WC_SUCCESS) {
                        printf("Reduce WRITE root send success\n");
                        fflush(stdout);
                    } else {
                        printf("Reduce WRITE root error: wc status: %d, opcode: %d\n", tmp->status, tmp->opcode);
                        fflush(stdout);
                        free(wc);
                        return;
                    }
                }
            } else if(result < 0) {
                printf("Reduce WRITE root ibv_poll_cq error: %d\n", result);
                fflush(stdout);
                free(wc);
                return;
            }
        }
    } else {
        // 非root节点等待发送完成
        int send_complete = 0;
        while(send_complete < send_num) {
            int result = ibv_poll_cq(comm->cq, message_num, wc);
            if(result > 0) {
                for(int i = 0; i < result; i++){
                    struct ibv_wc *tmp = wc + i;

                    if(tmp->status==IBV_WC_SUCCESS && (tmp->opcode==IBV_WC_RDMA_WRITE)) {
                        printf("Reduce WRITE non-root rank %d send success\n", my_rank);
                        fflush(stdout);
                        send_complete++;
                        
                        if(send_num < message_num) {
                            post_send(comm, send_buffer, send_num, IBV_WR_RDMA_WRITE);
                            send_num++;
                        }
                    } else if(tmp->status!=IBV_WC_SUCCESS) {
                        printf("Reduce WRITE non-root rank %d error: wc status: %d, opcode: %d\n", 
                               my_rank, tmp->status, tmp->opcode);
                        fflush(stdout);
                        free(wc);
                        return;
                    }
                }
            } else if(result < 0) {
                printf("Reduce WRITE non-root rank %d ibv_poll_cq error: %d\n", my_rank, result);
                fflush(stdout);
                free(wc);
                return;
            }
        }
    }

    free(wc);
    printf("Reduce WRITE operation completed for rank %d\n", my_rank);
    fflush(stdout);
}
