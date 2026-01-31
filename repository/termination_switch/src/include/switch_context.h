#ifndef SWITCH_CONTEXT_H
#define SWITCH_CONTEXT_H

#include <pcap.h>
#include <pthread.h>
#include "util.h"
#include "rule.h"
#include "thpool.h"
#include "parameter.h"

// ==================== 无锁队列定义 ====================
#define PKT_QUEUE_SIZE 16384  // 必须是 2 的幂（增大以避免丢包）
#define SEND_QUEUE_SIZE 16384 // 发送队列大小，必须是 2 的幂

/**
 * @brief 队列中的数据包结构
 */
typedef struct {
    int conn_id;                    // 连接 ID
    uint32_t psn;                   // PSN
    uint8_t opcode;                 // RDMA opcode
    int payload_len;                // payload 长度（字节）
    uint32_t data[1024];            // 数据缓冲区
    uint32_t src_ip;                // 源 IP
    uint32_t dst_ip;                // 目的 IP
    int has_imm;                    // 是否有 Immediate Data
    uint32_t imm_data;              // Immediate Data
    int is_nak;                     // 是否是 NAK（用于 ACK 包）
} queued_packet_t;

/**
 * @brief 无锁环形队列（SPSC: 单生产者单消费者）
 * 使用 volatile 和内存屏障实现线程安全
 */
typedef struct {
    queued_packet_t packets[PKT_QUEUE_SIZE];
    volatile unsigned int head;     // 生产者写入位置
    volatile unsigned int tail;     // 消费者读取位置
    volatile unsigned int dropped;  // 丢弃计数
} packet_queue_t;

/**
 * @brief 发送队列中的数据包结构
 *
 * 用于异步发送，包含完整的以太网帧数据
 */
typedef struct {
    int conn_id;                    // 目标连接 ID
    int frame_len;                  // 帧长度（字节）
    uint8_t frame[1600];            // 完整的以太网帧数据（MTU + headers）
} send_packet_t;

/**
 * @brief 发送队列（MPSC: 多生产者单消费者）
 *
 * 多个 worker 线程可能同时入队，单个 sender 线程出队发送
 */
typedef struct {
    send_packet_t packets[SEND_QUEUE_SIZE];
    volatile unsigned int head;     // 生产者写入位置
    volatile unsigned int tail;     // 消费者读取位置
    pthread_mutex_t enqueue_mutex;  // 入队锁（多生产者需要）
    volatile unsigned int dropped;  // 丢弃计数
} send_queue_t;

/**
 * @brief 交换机运行状态枚举
 */
typedef enum {
    SWITCH_STATE_INIT,       // 初始化状态（未连接 controller）
    SWITCH_STATE_STALLED,    // 停止状态（接收到 STALL 指令）
    SWITCH_STATE_RUNNING,    // 运行状态（接收到 START 指令）
} switch_state_t;

/**
 * @brief 聚合缓冲区结构体
 *
 * 存储单个数据包的聚合状态和数据内容
 */
typedef struct {
    int len;                    // 数据长度（以uint32_t为单位）
    int packet_type;            // 数据包类型（用于区分不同的RDMA操作类型）
    int state;                  // 缓冲区状态（0-空闲，1-已使用）
    int psn;                    // 对应的包序号（接收PSN）
    int send_psn;               // 发送给父交换机的PSN（用于重传）
    primitive_type_t operation_type;         // 集合通信操作类型（0-AllReduce, 1-Reduce）
    int root_rank;              // Reduce操作的根节点rank（仅用于Reduce）
    uint32_t buffer[1024];      // 数据缓冲区（最大支持约4096字节数据）
} agg_buffer_t;


/**
 * @brief PSN状态结构体
 *
 * 封装单个PSN的所有聚合和广播状态，支持上行聚合和下行广播的独立管理
 */
typedef struct {
    int degree;                     // 替代 agg_degree[psn]
    int arrival[MAX_CONNECTIONS_NUM];        // 替代 arrival_state[i][psn]
    agg_buffer_t agg_buffer;        // 替代 agg_buffer[psn]

    // === 下行广播相关 ===
    int r_degree;                   // 替代 r_degree[psn]
    int r_arrival[MAX_CONNECTIONS_NUM];          // 替代 r_arrival_state[i][psn]
    int bcast_send_psn[MAX_CONNECTIONS_NUM];     // 每个连接的实际发送PSN（用于重传）
    agg_buffer_t bcast_buffer;      // 替代 bcast_buffer[psn]

    // === 线程安全 ===
    pthread_mutex_t mutex;          // 每个PSN独立锁！
} psn_state_t;

/**
 * @brief 交换机上下文结构体
 *
 * 封装交换机的所有全局状态，包括PSN管理、连接信息、路由表和线程资源，
 * 用于在多线程环境中统一管理和传递交换机状态
 */
typedef struct {
    // === PSN 循环缓冲区（静态分配）===
    psn_state_t psn_states[SWITCH_ARRAY_LENGTH];

    // === 连接管理 ===
    connection_t conns[MAX_CONNECTIONS_NUM];
    int fan_in;                     // 动态扇入 (不再硬编码FAN_IN)
    int host_fan_in;                // 主机连接数（用于叶子交换机聚合判断）
    int is_root;                    // 替代全局 root

    // === PSN 管理 ===
    int agg_epsn[MAX_CONNECTIONS_NUM];           // 期望的上行PSN
    int down_epsn;                  // 期望的下行PSN
    int latest_ack[MAX_CONNECTIONS_NUM];         // 最后确认的上行PSN
    int down_ack;                   // 最后确认的下行PSN
    int send_psn[MAX_CONNECTIONS_NUM];           // 每个连接的发送PSN（用于下行广播）
    int acked_psn[MAX_CONNECTIONS_NUM];          // 每个连接已确认的PSN（用于重传检测）
    // 从 (conn_id, send_psn) 映射到 bcast_psn 的数组
    int send_to_bcast[MAX_CONNECTIONS_NUM][SWITCH_ARRAY_LENGTH];
    
    // === 路由和映射 ===
    rule_table_t routing_table;     // 替代全局 table
    int rank_to_conn[MAX_RANKS];    // 替代全局 rank_to_conn
    
    // === 元数据 (跨PSN共享) ===
    primitive_type_t operation_type; // 替代 current_operation_type
    int root_rank;                   // 替代 current_root_rank
    int ctrl_psn;                    // 当前操作的控制消息 PSN（用于计算相对 PSN）
    int ctrl_forwarded;              // 控制包是否已转发给父交换机（0=未转发，1=已转发）
    pthread_mutex_t meta_mutex;      // 保护元数据访问
    
    // === 资源 ===
    threadpool thpool;               // 替代全局 thpool
    int switch_id;                   // 交换机ID

    // === 控制器通信 ===
    int controller_fd;               // 与 controller 的 TCP 连接
    switch_state_t state;            // 交换机当前状态
    pthread_mutex_t state_mutex;     // 保护状态访问

    // === 接收线程管理 ===
    pthread_t receiver_threads[MAX_CONNECTIONS_NUM];  // 接收线程数组
    int num_receivers;               // 实际创建的接收线程数量

    // === 设备级别的 handle 管理 ===
    // 同一个物理设备上的多个连接共享 send/recv handle
    char device_names[MAX_CONNECTIONS_NUM][32];  // 设备名称列表
    pcap_t *device_send_handles[MAX_CONNECTIONS_NUM];  // 每个设备的 send_handle
    pcap_t *device_recv_handles[MAX_CONNECTIONS_NUM];  // 每个设备的 recv_handle
    pthread_mutex_t device_send_mutexes[MAX_CONNECTIONS_NUM];  // 每个设备的发送锁
    int device_conn_list[MAX_CONNECTIONS_NUM][MAX_CONNECTIONS_NUM];  // 每个设备的连接列表
    int device_conn_count[MAX_CONNECTIONS_NUM];  // 每个设备的连接数量
    int num_devices;  // 实际使用的设备数量

    // === 无锁数据包队列（用于非阻塞 callback）===
    packet_queue_t pkt_queues[MAX_CONNECTIONS_NUM];  // 每个连接一个队列
    pthread_t worker_threads[MAX_CONNECTIONS_NUM];   // worker 线程
    int num_workers;                                 // worker 线程数量
    volatile int running;                            // 运行标志

    // === 发送队列（分离发送和接收）===
    send_queue_t send_queues[MAX_CONNECTIONS_NUM];   // 每个设备一个发送队列
    pthread_t sender_threads[MAX_CONNECTIONS_NUM];   // 发送线程数组
    int num_senders;                                 // 发送线程数量

    // === UDP socket 占位（防止内核发送 ICMP port unreachable）===
    int udp_sockets[MAX_CONNECTIONS_NUM];            // 每个连接一个 UDP socket
} switch_context_t;


// ==================== 函数声明 ====================

/**
 * @brief 初始化交换机上下文（静态分配版本）
 *
 * @param ctx 预分配的上下文指针（通常是全局变量或栈变量）
 * @param switch_id 交换机ID
 * @param thread_pool_size 线程池大小
 * @return 0表示成功，-1表示失败
 */
int switch_context_init(switch_context_t *ctx, int switch_id, int thread_pool_size);

/**
 * @brief 清理交换机上下文（不释放ctx本身的内存）
 *
 * @param ctx 交换机上下文指针
 */
void switch_context_cleanup(switch_context_t *ctx);

/**
 * @brief 重置交换机上下文的PSN状态
 *
 * 用于拓扑重配置或新的通信组开始时
 *
 * @param ctx 交换机上下文指针
 */
void switch_context_reset_psn_states(switch_context_t *ctx);

/**
 * @brief 打印交换机上下文状态（用于调试）
 *
 * @param ctx 交换机上下文指针
 */
void switch_context_print(switch_context_t *ctx);

/**
 * @brief 获取连接到父交换机的连接 ID
 *
 * @param ctx 交换机上下文指针
 * @return 父交换机连接 ID，如果没有则返回 -1
 */
int get_parent_switch_conn(switch_context_t *ctx);

#endif // SWITCH_CONTEXT_H