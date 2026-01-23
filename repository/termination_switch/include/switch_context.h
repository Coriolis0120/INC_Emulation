#ifndef SWITCH_CONTEXT_H
#define SWITCH_CONTEXT_H

#include <pcap.h>
#include <pthread.h>
#include "util.h"
#include "rule.h"
#include "thpool.h"
#include "parameter.h"

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
    int psn;                    // 对应的包序号
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
    // === PSN 循环缓冲区 ===
    psn_state_t psn_states[SWITCH_ARRAY_LENGTH];      // 封装了所有PSN相关状态
    
    // === 连接管理 ===
    connection_t conns[MAX_CONNECTIONS_NUM];
    int fan_in;                     // 动态扇入 (不再硬编码FAN_IN)
    int is_root;                    // 替代全局 root
    
    // === PSN 管理 ===
    int agg_epsn[MAX_CONNECTIONS_NUM];           // 期望的上行PSN
    int down_epsn;                  // 期望的下行PSN
    int latest_ack[MAX_CONNECTIONS_NUM];         // 最后确认的上行PSN
    int down_ack;                   // 最后确认的下行PSN
    int send_psn[MAX_CONNECTIONS_NUM];           // 每个连接的发送PSN（用于下行广播）
    
    // === 路由和映射 ===
    rule_table_t routing_table;     // 替代全局 table
    int rank_to_conn[MAX_RANKS];    // 替代全局 rank_to_conn
    
    // === 元数据 (跨PSN共享) ===
    primitive_type_t operation_type; // 替代 current_operation_type
    int root_rank;                   // 替代 current_root_rank
    int ctrl_psn;                    // 当前操作的控制消息 PSN（用于计算相对 PSN）
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

#endif // SWITCH_CONTEXT_H