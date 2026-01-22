/**
 * @file controller_comm.h
 * @brief 交换机与控制器通信模块
 *
 * 实现交换机主动连接控制器，并持续监听控制器发送的指令
 */

#ifndef CONTROLLER_COMM_H
#define CONTROLLER_COMM_H

#include "switch_context.h"

/**
 * @brief 控制器指令类型
 */
typedef enum {
    CMD_STALL = 1,           // 停止所有连接的接收与发送
    CMD_RESET = 2,           // 清空PSN、RULE规则、拓扑，回到初始化状态
    CMD_RESET_PSN = 3,       // 仅清空PSN，其他不变
    CMD_SET_CONNECTIONS = 4, // 设置拓扑规则（后续接收YAML），并生成所有路由规则
    CMD_START = 5,           // 开始正常接收数据
} controller_cmd_t;

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
 * @brief 控制器通信线程（后台监听）
 *
 * 该线程持续监听控制器发送的指令并处理
 *
 * @param arg 交换机上下文指针 (switch_context_t*)
 * @return NULL
 */
void *controller_thread(void *arg);

/**
 * @brief 处理 STALL 指令
 *
 * @param ctx 交换机上下文
 * @return 0表示成功，-1表示失败
 */
int handle_cmd_stall(switch_context_t *ctx);

/**
 * @brief 处理 RESET 指令
 *
 * @param ctx 交换机上下文
 * @return 0表示成功，-1表示失败
 */
int handle_cmd_reset(switch_context_t *ctx);

/**
 * @brief 处理 RESET_PSN 指令
 *
 * @param ctx 交换机上下文
 * @return 0表示成功，-1表示失败
 */
int handle_cmd_reset_psn(switch_context_t *ctx);

/**
 * @brief 处理 SET_CONNECTIONS 指令
 *
 * 解析YAML拓扑配置，建立连接，生成所有路由规则，并启动接收线程
 *
 * @param ctx 交换机上下文
 * @param sockfd 与控制器的socket连接（用于接收YAML数据）
 * @return 0表示成功，-1表示失败
 */
int handle_cmd_set_connections(switch_context_t *ctx, int sockfd);

/**
 * @brief 处理 START 指令
 *
 * @param ctx 交换机上下文
 * @return 0表示成功，-1表示失败
 */
int handle_cmd_start(switch_context_t *ctx);

#endif // CONTROLLER_COMM_H
