#ifndef SWITCH_CONTEXT_H
#define SWITCH_CONTEXT_H

#include <pcap.h>
#include <pthread.h>
#include "util.h"
#include "rule.h"
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
 * @brief 交换机上下文结构体
 *
 * 封装交换机的所有全局状态，包括PSN管理、连接信息、路由表和线程资源，
 * 用于在多线程环境中统一管理和传递交换机状态
 */
typedef struct {

    // === 连接管理 ===
    connection_t conns[MAX_CONNECTIONS_NUM];
    int fan_in;
    int is_spine;
    int epoll_fd;
    
    // === PSN 管理 ===
    int data_epsn[MAX_CONNECTIONS_NUM];    // 期望的PSN
    int acked_psn[MAX_CONNECTIONS_NUM];    // acked聚合

    
    // === 路由和映射 ===
    rule_table_t routing_table;     // 替代全局 table
    
    // === 当前处理原语 ===
    primitive_type_t operation_type; // 替代 current_operation_type
    int primitive_param;                   
    uint32_t bitmap_mask; // 在control来的时候计算（本应该也在控制平面设置表，然后控制包来的时候查表）
    uint32_t root_conn;
    // === 聚合资源 ===
    uint32_t arrival_state[SWITCH_ARRAY_LENGTH]; // Max 32 ports, bitmap, need MASK and contain the result from parent, and can use mask to play the role of degree
    int aggregator[SWITCH_ARRAY_LENGTH][PAYLOAD_LEN / sizeof(int)];

    // === 控制器通信 ===
    int switch_id;                   // 交换机ID
    int controller_fd;               // 与 controller 的 TCP 连接
    switch_state_t state;            // 交换机当前状态

} switch_context_t;


typedef struct metadata{
    int ingress_conn;
    packet_type_t type;
    bool root_conn;
    inc_header_t header;
    uint32_t psn;
} packet_metadata_t;

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


#endif // SWITCH_CONTEXT_H