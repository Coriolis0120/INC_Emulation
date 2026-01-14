/**
 * @file switch.c
 * @brief INC (In-Network Computing) 交换机实现 - 支持RDMA的聚合交换机
 *
 * 该文件实现了INC系统中的核心交换机功能，支持：
 * 1. 基于RRoCE v2协议的数据包接收和发送
 * 2. 多线程数据包处理（接收线程池）
 * 3. 基于PSN（Packet Sequence Number）的滑动窗口流控
 * 4. 树形拓扑结构中的数据聚合和广播
 * 5. 与控制器通信获取配置信息
 *
 * 主要数据结构：
 * - arrival_state: 上行数据到达状态数组
 * - agg_buffer: 聚合缓冲区
 * - bcast_buffer: 广播缓冲区
 * - r_arrival_state: 下行ACK到达状态数组
 *
 * 线程模型：
 * - 控制器线程：连接控制器获取配置
 * - FAN_IN+1个接收线程：每个连接一个接收线程
 * - 轮询线程：处理超时重传（可选）
 * - 线程池：处理多播发送
 *
 * 作业流程：
 * 1. 上行阶段：主机发送数据包到交换机
 *    - 交换机接收并缓存数据
 *    - 等待所有FAN_IN个上行数据到达
 *    - 执行聚合操作（对数据进行求和）
 *    - 根交换机：直接进入广播阶段
 *    - 中间交换机：向上级交换机转发聚合结果
 *
 * 2. 下行阶段：根交换机广播结果到所有主机
 *    - 根交换机将聚合结果广播给所有子节点
 *    - 中间交换机接收上级的广播数据
 *    - 继续向下级交换机或主机转发
 *    - 主机接收最终的全局聚合结果
 *
 * 流控机制：
 * - 基于PSN的滑动窗口，支持乱序接收和重传
 * - ACK/NAK机制保证可靠传输
 * - 超时检测和重传（可选轮询线程）
 *
 * 键字解释：
 * - PSN: Packet Sequence Number，数据包序号
 * - BTH: Base Transport Header，RDMA基础传输头
 * - AETH: ACK Extended Transport Header，确认头
 * - FAN_IN: 扇入系数，每个交换机的输入连接数
 * - RoCEv2: RDMA over Converged Ethernet v2
 */

#include <pcap.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <assert.h>
#include "api.h"
#include "util.h"
#include "log.h"
#include "rule.h"
#include "thpool.h"
#include "parameter.h"
#include "topo_parser.h"

// =========================== 配置信息(yaml or 控制器, 暂时硬编码) =======================
#define N 300  // 交换机上各个数组的长度（循环缓冲区大小）
#define FAN_IN 2  // 交换机子节点个数（扇入系数）
#define Idx(psn) ((psn) % N)  // 根据PSN计算循环缓冲区索引

// 集合通信操作类型定义
#define OPERATION_ALLREDUCE 0  // AllReduce操作：所有节点接收结果
#define OPERATION_REDUCE 1     // Reduce操作：仅根节点接收结果
// =====================================================================================

/**
 * @brief 聚合/广播缓冲区元数据结构
 *
 * 用于存储聚合过程中的中间结果或广播数据
 */
typedef struct {
    int len;                    // 数据长度（以uint32_t为单位）
    int packet_type;            // 数据包类型（用于区分不同的RDMA操作类型）
    int state;                  // 缓冲区状态（0-空闲，1-已使用）
    int psn;                    // 对应的包序号
    int operation_type;         // 集合通信操作类型（0-AllReduce, 1-Reduce）
    int root_rank;              // Reduce操作的根节点rank（仅用于Reduce）
    uint32_t buffer[1036];      // 数据缓冲区（最大支持约4144字节数据）
} agg_buffer_t;

/**
 * @brief 时间戳结构
 *
 * 用于记录数据包的发送时间，支持超时重传机制
 */
typedef struct {
    int64_t ts;                 // 时间戳（微秒级）
    rule_t* rule;               // 对应的路由规则
    int psn;                    // 包序号
} ts_t;


// ============================ 静态分配全局变量 ==============================
/**
 * @brief 全局变量定义
 *
 * 这些变量支持交换机的核心功能：数据聚合、广播、流控和重传
 */

// 通信组（未使用）
struct inccl_communicator* comms[FAN_IN];

// 入流控制：记录上行数据包是否已经到达
// arrival_state[i][j] = 1 表示从第i个上行连接收到的PSN为j的数据包已到达
int arrival_state[FAN_IN][N];

// 时间戳缓冲区：用于超时检测和重传
// ts_buffer[i] 记录第i个连接的最后确认时间
// ts_buffer[FAN_IN] 特殊，用于下行数据
 ts_t ts_buffer[FAN_IN + 1];

// 下行ACK状态：记录广播后是否收到了子节点的ACK
int r_arrival_state[FAN_IN][N];

// 聚合缓冲区：存储正在聚合的数据
// 每个PSN对应一个缓冲区，用于累加所有上行数据
agg_buffer_t agg_buffer[N];

// 聚合度计数器：记录每个PSN已经收集到的上行数据份数
// 当agg_degree[i] == FAN_IN时，表示聚合完成
int agg_degree[N];

// 保护聚合操作的互斥锁
pthread_mutex_t agg_mutex = PTHREAD_MUTEX_INITIALIZER;

// 广播缓冲区：存储已经聚合完成的全局结果，用于向下广播
agg_buffer_t bcast_buffer[N];

// 广播度计数器：记录每个PSN的广播结果已经被多少个子节点确认
// 当r_degree[i] == FAN_IN时，表示广播完成
int r_degree[N];

// 保护广播操作的互斥锁
pthread_mutex_t r_mutex = PTHREAD_MUTEX_INITIALIZER;

// PSN管理：记录每个连接期望的下一个数据包序号
int agg_epsn[FAN_IN];      // 上行连接期望的PSN
int down_epsn;             // 下行连接期望的PSN
int latest_ack[FAN_IN];    // 上行连接最后确认的PSN
int down_ack;              // 下行连接最后确认的PSN

// 连接信息：包含所有上行和下行连接的配置
// conns[0..FAN_IN-1]: 上行连接（来自子节点或上级交换机）
// conns[FAN_IN]: 下行连接（去往父节点或下级交换机）
connection_t conns[FAN_IN + 1];

int root; // 标识是否为根交换机（1-是，0-否）

// Reduce操作支持：rank到连接ID的映射
// rank_to_conn[rank] = connection_id，用于Reduce操作时找到目标rank的连接
int rank_to_conn[FAN_IN];  // 最多支持FAN_IN个子节点

// 路由表：根据源IP和目标IP查找对应的转发规则
rule_table_t table;

// 线程池：用于并行处理多播发送任务
threadpool thpool;

int parse_finish = 0;  // 标诈配置解析是否完成

// =====================================================================================

// =============================== controller ===============================

/**
 * @brief 控制器通信线程
 *
 * 该线程负责与控制器建立TCP连接，获取以下信息：
 * 1. 交换机ID
 * 2. 网络拓扑配置文件（YAML格式）
 *
 * @param arg 控制器IP地址字符串
 * @return NULL
 */
static void *controller_thread(void *arg){
    const char *controller_ip = (char *)arg;
    int sockfd;
    struct sockaddr_in controller_addr;
    FILE* file;
    char buffer[4096];
    ssize_t bytes_received;
    int switch_id;

    // 创建TCP客户端套接字
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        return NULL;
    }

    // 配置控制器地址信息
    memset(&controller_addr, 0, sizeof(controller_addr));
    controller_addr.sin_family = AF_INET;
    controller_addr.sin_port = htons(CONTROLLER_SWITCH_PORT);  // 默认端口52311
    if (inet_pton(AF_INET, controller_ip, &controller_addr.sin_addr) <= 0) {
        perror("Invalid IP address");
        close(sockfd);
        return NULL;
    }

    // 尝试连接控制器
    if (connect(sockfd, (struct sockaddr*)&controller_addr, sizeof(controller_addr)) < 0) {
        perror("Connection failed");
        close(sockfd);
        return NULL;
    }

    printf("Connected to %s:%d\n", controller_ip, CONTROLLER_SWITCH_PORT);

    // 接收交换机ID（四字节整型）
    size_t total_received = 0;
    while (total_received < sizeof(switch_id)) {
        ssize_t ret = recv(sockfd, (char*)&switch_id + total_received, sizeof(switch_id) - total_received, 0);
        if (ret <= 0) {
            perror("Failed to receive switch_id");
            fclose(file);
            close(sockfd);
            return NULL;
        }
        total_received += ret;
    }
    printf("recv switch id %d\n", (switch_id));

    // 接收网络拓扑配置文件（YAML格式）
    // 文件保存在/home/ubuntu/topology.yaml
    receive_file(sockfd, "/home/ubuntu/topology.yaml");

    close(sockfd);

    // 解析YAML文件，获取交换机配置信息
    // 输出：root标识和conns连接数组
    parse_config("/home/ubuntu/topology.yaml", 10, &root, switch_id, conns);

    return NULL;
}

// =============================== helper ===============================

/**
 * @brief 初始化交换机全局状态
 *
 * 该函数完成以下初始化工作：
 * 1. 启动控制器线程，获取网络配置
 * 2. 初始化所有网络连接（pcap详情）
 * 3. 初始化全局数据结构（缓冲区、计数器等）
 * 4. 构建路由表
 * 5. 创建线程池
 *
 * @param controller_ip 控制器IP地址
 */
void init_all(const char *controller_ip) {

    // 创建控制器通信线程，获取网络拓扑配置
    pthread_t tid_controller;
    if(pthread_create(&tid_controller, NULL, controller_thread, (void *)controller_ip)){
        perror("Thread creation failed");
        return;
    }
    pthread_join(tid_controller, NULL);  // 等待控制器线程完成
    // 此处修改连接配置
    // root = 1;
    // uint8_t peer_macs[FAN_IN + 1][6] = {
    //     {0x52, 0x54, 0x00, 0x52, 0x5d, 0xae},
    //     {0x52, 0x54, 0x00, 0x5e, 0x4c, 0xb0},
    //     {}
    // };
    // char *peer_ips[FAN_IN + 1] = {
    //     "10.50.183.49",
    //     "10.50.183.102",
    //     ""
    // };
    for(int i = 0; i < FAN_IN + 1; i++) {
        if(root == 1 && i == FAN_IN)
            continue;
        
        
        
        // uint8_t my_mac[6] = {0x52, 0x54, 0x00, 0x46, 0x57, 0x82};
        // // uint8_t peer_mac[6] = {0x52, 0x54, 0x00, 0xdf, 0x0c, 0x28};
        // memcpy(conns[i].my_mac, my_mac, 6);
        // memcpy(conns[i].peer_mac, peer_macs[i], 6);

        // conns[i].my_ip = get_ip("10.50.183.69");
        // // conns[i].peer_ip = get_ip("10.50.183.146");
        // conns[i].peer_ip = get_ip(peer_ips[i]);

        // conns[i].my_port = 23333 + i;
        // conns[i].peer_port = 4791;

        // conns[i].my_qp = 28 + i;
        // conns[i].peer_qp = 0x11; // 待填
        
        conns[i].psn = 0;
        conns[i].msn = 0;
        conns[i].ok = 0;

        print_connection(i, &conns[i]);

        char errbuf[PCAP_ERRBUF_SIZE];
        conns[i].handle = pcap_create(conns[i].device, errbuf);
        //conns[i].handle = pcap_create("ens3", errbuf);
        pcap_set_snaplen(conns[i].handle, BUFSIZ);
        pcap_set_promisc(conns[i].handle, 1);
        pcap_set_timeout(conns[i].handle, 1);  // 1ms timeout
        pcap_set_immediate_mode(conns[i].handle, 1);
        if (pcap_activate(conns[i].handle) != 0) {
            fprintf(stderr, "pcap_activate failed: %s\n", pcap_geterr(conns[i].handle));
            return;
        }
        if (conns[i].handle == NULL) {
            fprintf(stderr, "Could not open device: %s, err: %s\n", conns[i].device, errbuf);
            return;
        }
    }
    
    memset(arrival_state, 0, sizeof(arrival_state));
    memset(ts_buffer, 0, sizeof(ts_buffer));
    memset(r_arrival_state, 0, sizeof(r_arrival_state));
    memset(agg_buffer, 0, sizeof(agg_buffer));
    memset(agg_degree, 0, sizeof(agg_degree));
    memset(bcast_buffer, 0, sizeof(bcast_buffer));
    // memset(bcast_arrival_state, 0, sizeof(bcast_arrival_state));
    memset(r_degree, 0, sizeof(r_degree));
    // memset(agg_psn, 0, sizeof(agg_psn));
    // for(int i = 0; i < N; i++) {
    //     agg_psn[i] = i;
    // }
    for(int i = 0; i < FAN_IN; i++) {
        agg_epsn[i] = 0;
        latest_ack[i] = -1;
        rank_to_conn[i] = i;  // 默认映射：rank i 对应连接 i
    }
    down_epsn = 0;
    down_ack = -1;

    memset(&table, 0, sizeof(table));
    for(int i = 0; i < FAN_IN + 1; i++) {
        if(root == 1 && i == FAN_IN)
            continue;

        rule_t rule;
        rule.src_ip = conns[i].peer_ip;
        rule.dst_ip = conns[i].my_ip;
        rule.id = i;
        if(i != FAN_IN)
            rule.direction = DIR_UP;
        else
            rule.direction = DIR_DOWN;
        rule.ack_conn = &conns[i];
        rule.out_conns_cnt = 0;
        if(root == 1 || (root == 0 && i == FAN_IN)) {
            // 1. 对于根交换机，所有上行入流的出流均是广播子节点
            // 2. 对于中间交换机的下行入流的出流均是广播子节点
            for(int j = 0; j < FAN_IN; j++) { // 广播
                rule.out_conns[j] = &conns[j];
                rule.out_conns_cnt++;
            }
        } else {
            // 中间交换机的上行入流
            rule.out_conns[0] = &conns[FAN_IN];
            rule.out_conns_cnt = 1;
        }
        
        add_rule(&table, &rule);
    }

    init_crc32_table();

    thpool = thpool_init(8);
}

/**
 * @brief 线程池发送任务参数结构
 *
 * 用于向线程池传递发送任务的参数
 */
typedef struct {
    connection_t* conn;     // 目标连接
    int type;               // 数据包类型（ACK/NAK/DATA）
    void* data;             // 数据缓冲区
    int len;                // 数据长度（以uint32_t为单位）
    uint32_t psn;           // 包序号
    int packet_type;        // RDMA操作类型（opcode）
} thread_arg_t;

/**
 * @brief 线程池发送任务函数
 *
 * 在线程池中执行的发送任务，构建并发送以太网数据包
 *
 * @param arg thread_arg_t类型参数
 */
void send_packet_thread(void* arg) {
    thread_arg_t* t_arg = (thread_arg_t*)arg;
    connection_t* conn = t_arg->conn;

    uint8_t packet[5555];
    int size = build_eth_packet(
        packet, t_arg->type, (char*)t_arg->data, t_arg->len * sizeof(uint32_t),
        conn->my_mac, conn->peer_mac,
        conn->my_ip, conn->peer_ip,
        conn->my_port, conn->peer_port,
        conn->peer_qp, t_arg->psn, t_arg->psn + 1, t_arg->packet_type, NULL
    );

    if (pcap_sendpacket(conn->handle, (uint8_t *)packet, size) == -1) {
        fprintf(stderr, "Error sending packet: %s\n", pcap_geterr(conn->handle));
    }

    return;
}


// 多线程发送封装函数
void send_packets_multithread(rule_t* rule, int type, void* data, int len, uint32_t psn, int packet_type) {
    int cnt = rule->out_conns_cnt;
    pthread_t threads[16];
    thread_arg_t args[16];  // 使用栈分配

    for (int i = 0; i < cnt; i++) {
        args[i].conn = rule->out_conns[i];
        args[i].type = type;
        args[i].data = data;
        args[i].len = len;
        args[i].psn = psn;
        args[i].packet_type = packet_type;

        // if (pthread_create(&threads[i], NULL, send_packet_thread, &args[i]) != 0) {
        //     perror("Failed to create thread");
        // }
        thpool_add_work(thpool, send_packet_thread, &args[i]);
    }

    thpool_wait(thpool);

    // for (int i = 0; i < cnt; i++) {
    //     pthread_join(threads[i], NULL);
    // }
}

/**
 * @brief 数据包转发函数
 *
 * 根据路由规则，将数据包转发到指定的连接
 * 支持以下类型：
 * - ACK/NAK：单播发送
 * - DATA：支持单播和多播（使用线程池）
 * - DATA_SINGLE：单播（重传使用）
 *
 * @param rule 路由规则
 * @param psn 包序号
 * @param type 数据包类型（ACK/NAK/DATA）
 * @param data 数据缓冲区
 * @param len 数据长度
 * @param packet_type RDMA操作类型
 */
void forwarding(rule_t* rule, uint32_t psn, uint32_t type, uint32_t *data, int len, int packet_type) {
    // TODO: 待优化, 尤其 rule 的设计
    int id = rule->id;
    LOG_FUNC_ENTRY(id);
    log_write(id, "conn_id: %d, forwarding... psn: %d, type: %d, len: %d\n", id, psn,type, len);

    if(type == PACKET_TYPE_ACK || type == PACKET_TYPE_NAK) {
        // ACK/NAK包：单播发送
        connection_t* conn = rule->ack_conn;

        uint8_t packet[2048];
        int size = build_eth_packet(
            packet, type, (char*)data, len * sizeof(uint32_t),
            conn->my_mac, conn->peer_mac,
            conn->my_ip, conn->peer_ip,
            conn->my_port, conn->peer_port,
            conn->peer_qp, psn, psn + 1, packet_type, NULL
        );

        pcap_t *handle = rule->ack_conn->handle;
        if (pcap_sendpacket(handle, (uint8_t *)packet, size) == -1) {
            fprintf(stderr, "Error sending packet: %s\n", pcap_geterr(handle));
        }

    } else if(type == PACKET_TYPE_DATA) {
        // 数据包：支持多播
        if(rule->out_conns_cnt == 1) {
            // 单个目标，直接发送
            connection_t* conn = rule->out_conns[0];

            uint8_t packet[5555];
            int size = build_eth_packet(
                packet, type, (char*)data, len * sizeof(uint32_t),
                conn->my_mac, conn->peer_mac,
                conn->my_ip, conn->peer_ip,
                conn->my_port, conn->peer_port,
                conn->peer_qp, psn, psn + 1, packet_type, NULL
            );

            pcap_t *handle = rule->out_conns[0]->handle;
            if (pcap_sendpacket(handle, (uint8_t *)packet, size) == -1) {
                fprintf(stderr, "Error sending packet: %s\n", pcap_geterr(handle));
            }
        } else {
            // 多个目标，使用线程池并行发送
            send_packets_multithread(rule, type, data, len, psn, packet_type);
        }

    } else if (type == PACKET_TYPE_DATA_SINGLE) {
        // 单播数据（重传使用）
        connection_t* conn = rule->ack_conn;

        uint8_t packet[5555];
        int size = build_eth_packet(
            packet, PACKET_TYPE_DATA, (char*)data, len * sizeof(uint32_t),
            conn->my_mac, conn->peer_mac,
            conn->my_ip, conn->peer_ip,
            conn->my_port, conn->peer_port,
            conn->peer_qp, psn, psn + 1, packet_type, NULL
        );

        if (pcap_sendpacket(conn->handle, (uint8_t *)packet, size) == -1) {
            fprintf(stderr, "Error sending packet: %s\n", pcap_geterr(conn->handle));
        }

    } else {
        assert(false);
    }
    LOG_FUNC_EXIT(id);
}

int retransmit(rule_t* rule, uint32_t psn) {
    int id = rule->id;
    rule_t* _rule = rule;
    uint32_t _psn = psn;
    LOG_FUNC_ENTRY(id);
    forwarding(_rule, _psn, PACKET_TYPE_DATA_SINGLE, bcast_buffer[Idx(_psn)].buffer, bcast_buffer[Idx(_psn)].len, bcast_buffer[Idx(_psn)].packet_type);
    LOG_FUNC_EXIT(id);
}

int cache(rule_t* rule, uint32_t psn, uint32_t *data, int len, int packet_type, bool revert) {
    // 缓存数据, 此时是收到父节点的全局聚合结果(用于广播的数据)
    // for(int i = 0; i < len; i++) {
    //     bcast_buffer[Idx(psn)].buffer[i] = revert ? ntohl(data[i]) : data[i];;
    // }
    assert(psn == down_epsn);
    down_epsn++;
    memcpy(bcast_buffer[Idx(psn)].buffer, data, len * sizeof(uint32_t));

    bcast_buffer[Idx(psn)].len = len;
    bcast_buffer[Idx(psn)].packet_type = packet_type;
    bcast_buffer[Idx(psn)].state = 1;
    bcast_buffer[Idx(psn)].psn = psn;
    
    // 广播
    printf("broadcast... psn: %d\n", psn);
    forwarding(rule, psn, PACKET_TYPE_DATA, bcast_buffer[Idx(psn)].buffer, len, packet_type);
}

void down_ack_handler(aeth_t* aeth, rule_t* rule, int psn) {
    int id;
    if(rule)
        id = rule->id;
    else
        id = FAN_IN; // 父节点

    if((ntohl(aeth->syn_msn) >> 29) == 0) {
        // 收到ACK
        log_write(id, "downstream ack...\n");
        int tmp = psn;
        if(tmp > down_ack) {
            // 重置计时器
            ts_buffer[id].ts = get_now_ts();
            
            while(1) {
                if(agg_buffer[Idx(tmp)].state == 1 && agg_buffer[Idx(tmp)].psn == tmp) {
                    pthread_mutex_lock(&agg_mutex);
                    agg_degree[Idx(tmp)] = 0;
                    memset(&agg_buffer[Idx(tmp)], 0, sizeof(agg_buffer_t));
                    for(int i = 0; i < FAN_IN; i++) {
                        arrival_state[i][Idx(tmp)] = 0;
                    }
                    pthread_mutex_unlock(&agg_mutex);
                } else {
                    break;
                }

                tmp--;
            }
            
            down_ack = psn;
        }
    } else {
        // NAK
        log_write(id, "downstream nak...\n");
        int tmp = psn - 1;
        if(tmp > down_ack) {
            // 重置计时器
            ts_buffer[id].ts = get_now_ts();
            
            while(1) {
                if(agg_buffer[Idx(tmp)].state == 1 && agg_buffer[Idx(tmp)].psn == tmp) {
                    pthread_mutex_lock(&agg_mutex);
                    agg_degree[Idx(tmp)] = 0;
                    memset(&agg_buffer[Idx(tmp)], 0, sizeof(agg_buffer_t));
                    for(int i = 0; i < FAN_IN; i++) {
                        arrival_state[i][Idx(tmp)] = 0;
                    }
                    pthread_mutex_unlock(&agg_mutex);
                } else {
                    break;
                }

                tmp--;
            }
            
            down_ack = psn - 1;
        }

        while(1) {
            if(agg_degree[Idx(psn)] == FAN_IN && agg_buffer[Idx(psn)].psn > down_ack) {
                retransmit(rule, psn);
            } else {
                break;
            }
            psn++;
        }
    }
}

void add_payload(uint32_t *restrict dst, const uint32_t *restrict src, int len) {
    for (int i = 0; i < len; i++) {
        // 将网络字节序转换为主机字节序，进行加法，再转回网络字节序
        uint32_t dst_host = ntohl(dst[i]);
        uint32_t src_host = ntohl(src[i]);
        uint32_t result = dst_host + src_host;
        dst[i] = htonl(result);

        // 打印前几个元素的调试信息
        if (i < 3) {
            printf("add_payload[%d]: dst_net=0x%08x dst_host=%u, src_net=0x%08x src_host=%u, sum=%u, result_net=0x%08x\n",
                   i, ntohl(dst[i]), dst_host, src[i], src_host, result, dst[i]);
        }
    }
}


/**
 * @brief 数据聚合函数
 *
 * 将上行数据包的内容聚合到对应PSN的缓冲区中
 * 当所有FAN_IN个上行数据都到达后，触发下一步操作：
 * - 根交换机 + AllReduce：直接开始广播到所有子节点
 * - 根交换机 + Reduce：仅发送到指定的根rank节点
 * - 中间交换机：向上级交换机转发聚合结果
 *
 * @param rule 路由规则（标识数据来源）
 * @param psn 包序号
 * @param data 数据缓冲区
 * @param len 数据长度
 * @param packet_type RDMA操作类型
 * @param operation_type 集合通信操作类型（OPERATION_ALLREDUCE或OPERATION_REDUCE）
 * @param root_rank Reduce操作的根节点rank（仅用于Reduce操作）
 * @return 成功标识
 */
int aggregate(rule_t* rule, uint32_t psn, uint32_t *data, int len, int packet_type, int operation_type, int root_rank) {
    int id = rule->id;
    LOG_FUNC_ENTRY(id);

    // 保护聚合操作（多线程环境）
    pthread_mutex_lock(&agg_mutex);

    // 将当前数据聚合到缓冲区（网络字节序转换为主机字节序进行加法）
    add_payload(agg_buffer[Idx(psn)].buffer, data, len);

    // 更新缓冲区元数据
    agg_buffer[Idx(psn)].len = len;
    agg_buffer[Idx(psn)].packet_type = packet_type;
    agg_buffer[Idx(psn)].state = 1;  // 标记为已使用
    agg_buffer[Idx(psn)].psn = psn;
    agg_buffer[Idx(psn)].operation_type = operation_type;  // 记录操作类型
    agg_buffer[Idx(psn)].root_rank = root_rank;  // 记录根节点rank

    // 增加聚合度计数器
    agg_degree[Idx(psn)]++;
    pthread_mutex_unlock(&agg_mutex);

    // 检查是否所有上行数据都已经到达
    if(agg_degree[Idx(psn)] == FAN_IN) {
        // 聚合完成，触发下一步操作
        if(root == 1) {
            // 根交换机：根据操作类型决定下一步
            if (bcast_buffer[Idx(psn)].state == 1) {
                assert(0);  // 不应该发生，缓冲区已被占用
            } else {
                if(operation_type == OPERATION_ALLREDUCE) {
                    // AllReduce操作：广播到所有子节点
                    log_write(id, "AllReduce: broadcasting to all nodes, psn: %d\n", psn);

                    // 模拟收到下行ACK和广播包的情况
                    aeth_t aeth;
                    aeth.syn_msn = 0; // ACK

                    // 缓存聚合结果并开始广播
                    cache(rule, psn, agg_buffer[Idx(psn)].buffer, len, packet_type, false);
                    down_ack_handler(&aeth, NULL, psn);
                } else if(operation_type == OPERATION_REDUCE) {
                    // Reduce操作：仅发送到指定的根rank节点
                    log_write(id, "Reduce: sending to root rank %d, psn: %d\n", root_rank, psn);

                    // 查找根rank对应的连接ID
                    int target_conn_id = -1;
                    for(int i = 0; i < FAN_IN; i++) {
                        if(rank_to_conn[i] == root_rank) {
                            target_conn_id = i;
                            break;
                        }
                    }

                    if(target_conn_id >= 0) {
                        // 找到目标连接，发送聚合结果
                        connection_t* target_conn = &conns[target_conn_id];

                        uint8_t packet[5555];
                        int size = build_eth_packet(
                            packet, PACKET_TYPE_DATA, (char*)agg_buffer[Idx(psn)].buffer, len * sizeof(uint32_t),
                            target_conn->my_mac, target_conn->peer_mac,
                            target_conn->my_ip, target_conn->peer_ip,
                            target_conn->my_port, target_conn->peer_port,
                            target_conn->peer_qp, psn, psn + 1, packet_type, NULL
                        );

                        if (pcap_sendpacket(target_conn->handle, (uint8_t *)packet, size) == -1) {
                            fprintf(stderr, "Error sending Reduce result: %s\n", pcap_geterr(target_conn->handle));
                        }

                        log_write(id, "Reduce result sent to rank %d via conn %d\n", root_rank, target_conn_id);
                    } else {
                        log_write(id, "ERROR: Cannot find connection for root rank %d\n", root_rank);
                    }

                    // Reduce操作完成后，清理聚合缓冲区（不需要等待ACK）
                    // 注意：这里简化处理，实际可能需要等待ACK确认
                    pthread_mutex_lock(&agg_mutex);
                    agg_degree[Idx(psn)] = 0;
                    memset(&agg_buffer[Idx(psn)], 0, sizeof(agg_buffer_t));
                    for(int i = 0; i < FAN_IN; i++) {
                        arrival_state[i][Idx(psn)] = 0;
                    }
                    pthread_mutex_unlock(&agg_mutex);
                }
            }
        } else {
            // 中间交换机：向上级交换机转发聚合结果（AllReduce和Reduce处理相同）
            forwarding(rule, psn, PACKET_TYPE_DATA, agg_buffer[Idx(psn)].buffer, len, agg_buffer[Idx(psn)].packet_type);
        }
        log_write(id, "agg over...\n");

    }
    LOG_FUNC_EXIT(id);
}

int re_idx[FAN_IN];
rule_t* re_rule[FAN_IN];
uint32_t re_psn[FAN_IN];
void re_thread(void* arg) {
    int id = *(int*)arg;
    log_write(id, "re thread\n");
    rule_t* rule = re_rule[id];
    uint32_t psn = re_psn[id];
    while(1) {
        if(bcast_buffer[Idx(psn)].state == 1 && bcast_buffer[Idx(psn)].psn > latest_ack[id]) {
            retransmit(rule, psn);
        } else {
            break;
        }
        psn++;
    }
    log_write(id, "re thread over\n");
}

// =====================================================================================




// ================================ 核心线程 接收模块 ===================

/**
 * @brief 数据包处理回调函数（核心功能）
 *
 * 该函数是交换机的核心，处理所有接收到的数据包：
 * 1. 解析数据包结构（Ethernet + IP + UDP + BTH）
 * 2. 根据源/目标IP查找路由规则
 * 3. 根据数据包类型和方向进行不同处理：
 *    - 上行数据：进行聚合操作
 *    - 下行数据：进行广播操作
 *    - ACK/NAK：更新状态和触发重传
 *
 * @param user_data 线程ID（字符串形式）
 * @param pkthdr pcap数据包头信息
 * @param packet 数据包内容
 */
void packet_handler(uint8_t *user_data, const struct pcap_pkthdr *pkthdr, const uint8_t *packet) {
    int id = atoi((char*)user_data);  // 获取线程ID
    LOG_FUNC_ENTRY(id);

    // 解析数据包各层头部
    eth_header_t* eth = (eth_header_t*)packet;
    ipv4_header_t* ip = (ipv4_header_t*)(packet + sizeof(eth_header_t));
    udp_header_t* udp = (udp_header_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t));
    bth_header_t* bth = (bth_header_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(udp_header_t));

    // 根据源IP和目标IP查找路由规则
    rule_t* rule = lookup_rule(&table, ip->src_ip, ip->dst_ip);
    assert(rule != NULL);

    // 提取包序号（PSN），用于流控和重传
    int psn = ntohl(bth->apsn) & 0x00FFFFFF;  // 取BTH头部中的24位PSN
    log_write(id, "psn: %u\n", psn);

    // 处理数据包（RDMA SEND操作）
    if(bth->opcode == 0x04 || bth->opcode == 0x00 || bth->opcode == 0x01 || bth->opcode == 0x02) { // rc send only
        uint32_t* data = (uint32_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(udp_header_t) + sizeof(bth_header_t));
        int data_len = (ntohs(udp->length) - sizeof(bth_header_t) - sizeof(udp_header_t) - 4) / sizeof(uint32_t); // icrc = 4
        log_write(id, "udp len: %d, data_len: %d\n", udp->length, data_len);

        if(rule->direction == DIR_DOWN) {
            log_write(id, "downstream data...\n");
            // 下行数据, 广播
            if (psn < down_epsn) {
                // 滞后
                log_write(id, "lag\n");
                forwarding(rule, down_epsn - 1, PACKET_TYPE_ACK, NULL, 0, 0);
            } else if (psn > down_epsn) {
                log_write(id, "ahead\n");
                forwarding(rule, down_epsn, PACKET_TYPE_NAK, NULL, 0, 0);
            } else {
                // 持平
                log_write(id, "equal\n");
                if (bcast_buffer[Idx(psn)].state == 1) {
                    assert(0); // 这个其实是说明已经快了一轮
                    log_write(id, "retransmit\n");
                    forwarding(rule, psn, PACKET_TYPE_ACK, NULL, 0, 0);
                } else {
                    // 首传
                    log_write(id, "first\n");
                    
                    // 发送ACK
                    forwarding(rule, psn, PACKET_TYPE_ACK, NULL, 0, 0);
                    // 缓存模块
                    cache(rule, psn, data, data_len, bth->opcode, true); // epsn++
                }
            }
        } else {
            // 上行数据, 聚合
            log_write(id, "upstream data...\n");
            if (psn < agg_epsn[id]) {
                // 滞后
                // 发送ACK
                log_write(id, "lag\n");
                forwarding(rule, agg_epsn[id] - 1, PACKET_TYPE_ACK, NULL, 0, 0);
            } else if (psn > agg_epsn[id]) {
                // 超前/乱序
                log_write(id, "ahead\n");
                forwarding(rule, agg_epsn[id], PACKET_TYPE_NAK, NULL, 0, 0);
            } else {
                // 持平
                log_write(id, "equal\n");
                agg_epsn[id]++;
                if (arrival_state[id][Idx(psn)] == 1) {
                    assert(0); // 这个其实是说明已经快了一轮, 当发送方窗口较小时,不可能发生 => 这里可以加个流控机制
                    log_write(id, "retransmit\n");
                    // 发送ACK
                    forwarding(rule, psn, PACKET_TYPE_ACK, NULL, 0, 0);
                } else {
                    // 首传
                    log_write(id, "first\n");
                    arrival_state[id][Idx(psn)] = 1;
                    // r_arrival_state[id][Idx(psn)] = 0;

                    // 发送ACK
                    forwarding(rule, psn, PACKET_TYPE_ACK, NULL, 0, 0);

                    // 从数据包中提取操作类型和根rank信息
                    // 假设数据包的前两个uint32_t存储元数据：
                    // data[0] = operation_type (OPERATION_ALLREDUCE 或 OPERATION_REDUCE)
                    // data[1] = root_rank (仅用于Reduce操作)
                    int operation_type = OPERATION_ALLREDUCE;  // 默认为AllReduce
                    int root_rank = 0;  // 默认根节点为rank 0

                    if(data_len >= 2) {
                        // 提取操作类型（网络字节序转主机字节序）
                        operation_type = ntohl(data[0]);
                        root_rank = ntohl(data[1]);

                        // 跳过元数据，只聚合实际数据
                        // 注意：这里简化处理，实际应该在协议层面明确定义
                        log_write(id, "Operation type: %d, Root rank: %d\n", operation_type, root_rank);
                    }

                    // 聚合模块
                    aggregate(rule, psn, data, data_len, bth->opcode, operation_type, root_rank);
                }
            }
        }
    } else if(bth->opcode == 0x11) { // rc ack
        aeth_t* aeth = (aeth_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(udp_header_t) + sizeof(bth_header_t));

        if(rule->direction == DIR_DOWN) {
            // 下行ACK
            down_ack_handler(aeth, rule, psn);
        } else {
            // 上行ACK
            if((ntohl(aeth->syn_msn) >> 29) == 0) {
                // 收到ACK
                log_write(id, "upstream ack...\n");
                int tmp = psn;
                if(psn > latest_ack[id]) {
                    if(r_arrival_state[id][Idx(psn)] == 1 || bcast_buffer[Idx(psn)].psn != psn)
                        return;

                    // 重置计时器
                    ts_buffer[id].ts = get_now_ts();
                    r_arrival_state[id][Idx(psn)] = 1;
                    pthread_mutex_lock(&r_mutex);
                    while(1) {
                        r_degree[Idx(psn)] += 1;
                        if (r_degree[Idx(psn)] == FAN_IN) {
                            // 说明该psn广播完成
                            log_write(id, "psn: %d, broadcast over...\n", psn);
                            r_degree[Idx(psn)] = 0;
                            memset(&bcast_buffer[Idx(psn)], 0, sizeof(agg_buffer_t));
                            for(int i = 0; i < FAN_IN; i++) {
                                r_arrival_state[i][Idx(psn)] = 0;
                            }
                        }

                        psn--;
                        if(psn == latest_ack[id])
                            break;
                    }
                    pthread_mutex_unlock(&r_mutex);
                    latest_ack[id] = tmp;
                }
            } else {
                // NAK
                // pthread_mutex_unlock(&agg_mutex);
                log_write(id, "upstream nak...\n");
                int tmp = psn - 1;
                if(tmp > latest_ack[id]) {
                    pthread_mutex_lock(&r_mutex);
                    while(1) {
                        if(r_arrival_state[id][Idx(tmp)] == 1 || bcast_buffer[Idx(tmp)].psn != tmp)
                            continue;
                        r_arrival_state[id][Idx(tmp)] = 1;
                        r_degree[Idx(tmp)] += 1;
                        if (r_degree[Idx(tmp)] == FAN_IN) {
                            // 说明该psn广播完成
                            log_write(id, "psn: %d, broadcast over...\n", tmp);
                            r_degree[Idx(tmp)] = 0;
                            memset(&bcast_buffer[Idx(tmp)], 0, sizeof(agg_buffer_t));
                            for(int i = 0; i < FAN_IN; i++) {
                                r_arrival_state[i][Idx(tmp)] = 0;
                            }
                        }

                        tmp--;
                        if(tmp == latest_ack[id])
                            break;
                    }
                    pthread_mutex_unlock(&r_mutex);
                    latest_ack[id] = psn - 1;
                }

                // while(1) {
                //     if(bcast_buffer[Idx(psn)].state == 1 && bcast_buffer[Idx(psn)].psn > latest_ack[id]) {
                //         retransmit(rule, psn);
                //     } else {
                //         break;
                //     }
                //     psn++;
                // }
                re_rule[id] = rule;
                re_psn[id] = psn;
                re_idx[id] = id;
                thpool_add_work(thpool, re_thread, &re_idx[id]);
            }
        }
    }else{
        log_write(id, "unknown packet...\n");
    }
    LOG_FUNC_EXIT(id);
}


/**
 * @brief 后台接收线程
 *
 * 每个网络连接一个独立的接收线程，负责：
 * 1. 配置pcap过滤器（只接收特定源的RoCEv2流量）
 * 2. 开始抓包并调用packet_handler处理
 *
 * @param arg 连接ID（整型）
 * @return NULL
 */
void *background_receiving(void *arg) {
    int id = (int)(intptr_t)arg;
    printf("thread %d start...\n", id);
    connection_t* conn = &conns[id];

    pcap_t *handle = conn->handle;

    // 设置过滤器（仅捐获 RoCEv2 流量）
    struct bpf_program fp;
    char filter_exp[100];
    char ip_str[INET_ADDRSTRLEN]; // IPv4 缓冲区大小（16字节）
    if (!inet_ntop(AF_INET, &(conn->peer_ip), ip_str, sizeof(ip_str))) {
        printf("to p err\n");
        return NULL;
    }
    snprintf(filter_exp, sizeof(filter_exp), "udp port 4791 and src host %s", ip_str);
    log_write(id, "filter: %s\n", filter_exp);
    if (pcap_compile(handle, &fp, filter_exp, 0, PCAP_NETMASK_UNKNOWN) == -1) {
        fprintf(stderr, "Could not parse filter: %s\n", pcap_geterr(handle));
        return NULL;
    }
    if (pcap_setfilter(handle, &fp) == -1) {
        fprintf(stderr, "Could not set filter: %s\n", pcap_geterr(handle));
        return NULL;
    }

    // 开始抓包
    printf("================================================\n");
    char str[8];
    sprintf(str, "%d", id);
    pcap_loop(handle, -1, packet_handler, str);

    pcap_close(handle);
    printf("++++++++++++++++++++++++++++++++++++++++++++++++\n");

    return NULL;
}

/**
 * @brief 轮询线程（未使用）
 *
 * 该线程用于检测超时并触发重传：
 * 1. 每隔10ms检查一次
 * 2. 如果某个连接的数据包超过100微秒未收到ACK
 * 3. 从最后确认的PSN开始，重传所有未确认的数据包
 *
 * @param arg 未使用
 * @return NULL（无限循环）
 */
void *polling_thread(void *arg) {
    while(1) {
        for(int i = 0; i < FAN_IN; i++) {
            uint64_t now_ts = get_now_ts();
            // 检查是否超时（100微秒）
            if(ts_buffer[i].ts != 0 && now_ts - ts_buffer[i].ts > 100) {
                int psn = latest_ack[i] + 1;
                rule_t* rule = &table.rules[i];

                printf("timeout, conn id: %d, psn: %d, pack type: %d\n", i, psn, bcast_buffer[Idx(psn)].packet_type);

                // 重传所有未确认的数据包
                while(1) {
                    if(bcast_buffer[Idx(psn)].state == 1 && bcast_buffer[Idx(psn)].psn > latest_ack[i]) {
                        retransmit(rule, psn);
                    } else {
                        break;
                    }
                    psn++;
                }

                printf("timeout over\n");
            }
        }

        usleep(10000); // 10ms轮询一次
    }

}

// =====================================================================================



/**
 * @brief 交换机主函数
 *
 * 交换机启动流程：
 * 1. 连接控制器获取网络配置
 * 2. 创建接收线程（每个网络接口一个）
 * 3. 启动轮询线程（可选）
 * 4. 等待所有线程结束
 *
 * 注意：根交换机没有下行连接（i == FAN_IN）
 *
 * @param argc 参数个数
 * @param argv 参数列表（可以传入控制器IP）
 * @return 0-成功，其他-失败
 */
int main(int argc, char *argv[]) {
    // 控制器IP地址（可以通过命令行参数传入）
    // char *controller_ip;
    // if(argc != 2) {
    //     return -1;
    // } else {
    //     controller_ip = argv[1];
    // }
    char * controller_ip = "192.168.0.3";  // 默认控制器IP

    // 初始化日志系统
    if (log_init(NULL) != 0) {
        fprintf(stderr, "Failed to open log file\n");
        return 1;
    }

    // 初始化交换机（获取配置、创建连接等）
    init_all(controller_ip);
    printf("init finish\n");

    // 线程又
    pthread_t receivers[FAN_IN + 1];  // 接收线程
    pthread_t polling;                  // 轮询线程（未使用）

    // 创建接收线程（每个网络接口一个）
    for(int i = 0; i < FAN_IN + 1; i++) {
        if(root && i == FAN_IN)
            continue;  // 根交换机没有下行连接
        pthread_create(&receivers[i], NULL, background_receiving, (void *)(intptr_t)i);
    }

    // 可选：启动轮询线程处理超时重传
    // pthread_create(&polling, NULL, polling_thread, NULL);

    // 等待所有接收线程结束
    for(int i = 0; i < FAN_IN + 1; i++) {
        if(root && i == FAN_IN)
            continue;
        pthread_join(receivers[i], NULL);
    }

    // 等待轮询线程结束（如果启用）
    // pthread_join(polling, NULL);

    // 关闭日志系统
    log_close();

    return 0;
}