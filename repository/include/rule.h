#ifndef RULE_H
#define RULE_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pcap.h>
#include "util.h"
#include "parameter.h"


enum Direction {
    DIR_UP,
    DIR_DOWN,
};


typedef struct {
    uint32_t src_ip;
    uint32_t dst_ip;

    int id; // 子节点交换机编号，范围0~FAN_IN-1，上行连接时该值无意义

    int direction;

    // pcap_t *ack_handle; // 回复ACK的端口 相当于入端口
    connection_t *ack_conn; // ACK 发给谁
    // pcap_t *out_handles[MAX_PORT_NUM]; // 相当于交换机出端口
    connection_t *out_conns[MAX_PORT_NUM]; // 转发给谁？
    int out_conns_cnt; //记录out_conns 的大小
} rule_t;

typedef struct {
    rule_t rules[MAX_RULES];
    int count;
} rule_table_t;



// Rules table;

int match_rule(rule_t* rule, uint32_t src_ip, uint32_t dst_ip);

rule_t* lookup_rule(rule_table_t* table, uint32_t src_ip, uint32_t dst_ip);

int add_rule(rule_table_t* table, const rule_t* rule);

#endif // RULE_H
