/**
 * @file controller_comm.h
 * @brief 交换机与控制器通信模块（简化版）
 *
 * 简化协议：连接后直接接收 YAML 配置
 * 格式：4字节长度(网络字节序) + YAML内容
 */

#ifndef CONTROLLER_COMM_H
#define CONTROLLER_COMM_H

#include "switch_context.h"

/**
 * @brief 连接到控制器
 *
 * @param ctx 交换机上下文
 * @param controller_ip 控制器IP地址
 * @param controller_port 控制器端口（默认使用 CONTROLLER_SWITCH_PORT）
 * @return 0表示成功，-1表示失败
 */
int controller_connect(switch_context_t *ctx, const char *controller_ip, int controller_port);

/**
 * @brief 控制器通信线程
 *
 * 持续接收 YAML 配置，每次收到新配置就更新上下文
 *
 * @param arg 交换机上下文指针 (switch_context_t*)
 * @return NULL
 */
void *controller_thread(void *arg);


int controller_init(switch_context_t *ctx, const char *controller_ip);


#endif // CONTROLLER_COMM_H
