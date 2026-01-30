#ifndef RULE_H
#define RULE_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pcap.h>
#include "util.h"
#include "parameter.h"

// ============================================
// 方向枚举
// ============================================
typedef enum {
    DIR_UP = 0,      // 上行：Host -> Leaf -> Spine (聚合方向)
    DIR_DOWN = 1,    // 下行：Spine -> Leaf -> Host (广播方向)
} rule_direction_t;

// primitive_type_t 定义在 util.h 中:
// PRIMITIVE_TYPE_NULL = 0
// PRIMITIVE_TYPE_ALLREDUCE = 1
// PRIMITIVE_TYPE_REDUCE = 2
// PRIMITIVE_TYPE_BROADCAST = 3

// ============================================
// 单条规则
// 通过三元组 (src_ip, dst_ip, primitive_type) 唯一确定
// ============================================
typedef struct {
    // 匹配字段 (三元组)
    uint32_t src_ip;              // 匹配：源 IP
    uint32_t dst_ip;              // 匹配：目的 IP
    primitive_type_t primitive;   // 匹配：原语类型
    int primitive_param;          // 原语参数: root_rank (Reduce) 或 sender_rank (Broadcast), AllReduce 时为 -1

    // 规则属性
    int conn_id;                  // 入连接 ID (用于标识来源)
    int direction;                // DIR_UP 或 DIR_DOWN
    int root;                     // 当前交换机是否是这条规则的根 (1=是, 0=否)

    // 转发信息
    connection_t *ack_conn;       // ACK 发给谁 (入连接)
    connection_t *out_conns[MAX_PORT_NUM];  // 转发给谁
    int out_conns_cnt;            // out_conns 数量
} rule_t;

// ============================================
// 规则表
// ============================================
typedef struct {
    rule_t rules[MAX_RULES];
    int count;
} rule_table_t;

// ============================================
// 函数声明
// ============================================

// 规则匹配 (三元组匹配)
int match_rule(rule_t* rule, uint32_t src_ip, uint32_t dst_ip,
               primitive_type_t primitive, int primitive_param);

// 规则查找 (三元组查找)
rule_t* lookup_rule(rule_table_t* table, uint32_t src_ip, uint32_t dst_ip,
                    primitive_type_t primitive, int primitive_param);

// 规则表操作
int add_rule(rule_table_t* table, const rule_t* rule);

#endif // RULE_H
