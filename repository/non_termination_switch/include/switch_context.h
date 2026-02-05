#ifndef SWITCH_CONTEXT_H
#define SWITCH_CONTEXT_H

#include <stdbool.h>
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
    int psn_synced[MAX_CONNECTIONS_NUM];   // 是否已同步第一个PSN

    
    // === 路由和映射 ===
    rule_table_t routing_table;     // 替代全局 table
    
    // === 当前处理原语 ===
    primitive_type_t operation_type; // 替代 current_operation_type
    int primitive_param;
    uint32_t bitmap_mask; // 在control来的时候计算（本应该也在控制平面设置表，然后控制包来的时候查表）
    uint32_t root_conn;

    // === Reduce 专用 ===
    int reduce_root_conn;  // Reduce 操作中 root 节点所在的连接 ID (-1 表示不在本交换机下)
    // === 聚合资源 ===
    uint32_t *arrival_state;  // 动态分配，Max 32 ports, bitmap
    int **aggregator;         // 动态分配，二维数组
    int *degree;              // 动态分配，重传计数

    // === 控制消息状态 ===
    uint32_t ctrl_arrival_bitmap;    // 已收到控制消息的连接位图
    uint32_t ctrl_expected_bitmap;   // 期望收到控制消息的连接位图（主机连接）
    int ctrl_confirmed;              // 是否已发送控制确认

    // === 控制器通信 ===
    int switch_id;                   // 交换机ID
    int controller_fd;               // 与 controller 的 TCP 连接
    switch_state_t state;            // 交换机当前状态

    // === 同步机制 ===
    pthread_mutex_t config_mutex;    // 配置互斥锁
    pthread_cond_t config_cond;      // 配置条件变量
    int config_ready;                // 配置是否就绪

} switch_context_t;


typedef struct metadata{
    int ingress_conn;
    packet_type_t type;
    bool root_conn;
    inc_header_t header;
    uint32_t psn;
    uint32_t pkt_len;  // 实际包大小
} packet_metadata_t;

// ==================== 函数声明 ====================

/**
 * @brief 初始化交换机上下文（静态分配版本）
 *
 * @param ctx 预分配的上下文指针（通常是全局变量或栈变量）
 * @param switch_id 交换机ID
 * @return 0表示成功，-1表示失败
 */
int switch_context_init(switch_context_t *ctx, int switch_id);

/**
 * @brief 清理交换机上下文（不释放ctx本身的内存）
 *
 * @param ctx 交换机上下文指针
 */
void switch_context_cleanup(switch_context_t *ctx);


#endif // SWITCH_CONTEXT_H