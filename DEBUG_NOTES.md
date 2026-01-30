# INC 系统调试笔记 - 2026-01-30

## 问题概述

64MB AllReduce 测试失败，所有节点只收到约 7003/16384 消息后超时。

## 测试结果

| 数据量 | 结果 | 耗时 |
|--------|------|------|
| 1MB | PASS | ~2.2s |
| 4MB | PASS | ~8.7s (较慢) |
| 64MB | FAIL | 超时 |

## 核心发现：ACK 延迟问题

### 现象

Switch1 收到来自 pku1 和 pku2 的 ACK 时间差异巨大：

**4MB 测试：**
- pku1 ACK PSN=0 到达：第 3132 行
- pku2 ACK PSN=0 到达：第 70111 行
- 延迟差异：约 67000 行

**64MB 测试：**
- pku1 ACK PSN=0 到达：第 101796 行
- pku2 ACK PSN=0 到达：第 3274 行
- 延迟差异：约 98000 行（方向相反）

### 关键发现

1. **ACK 延迟不固定**：不同测试中延迟的节点不同，说明问题与特定节点无关。

2. **重复 PSN 发送**：Switch 发送给 Host 的包 PSN 顺序混乱
   ```
   PSN=0, PSN=1, PSN=0, PSN=2, PSN=1, PSN=3, PSN=2, ...
   ```
   这不是重传导致的，而是正常广播过程中的重复发送。

3. **大量 duplicate_request**：
   - pku1: 12836 个
   - pku2: 10180 个

4. **内核错误**（之前测试残留）：
   ```
   rdma_rxe: no qp matches qpn 0x11
   ```
   但在清除内核日志后的测试中没有出现此错误。

## 可能的根本原因

### 假设 1：广播 PSN 重复发送

在 `switch_main.c` 的 `cache_and_broadcast` 函数中，可能存在以下问题：
- 同一个 bcast_PSN 被多次发送给同一个连接
- 导致 Soft-RoCE 收到乱序/重复的包，延迟生成 ACK

**证据**：
```
第 328 行: [ASYNC_SEND] PSN=0, 10.1.1.25->10.1.1.5 (ACK 包)
第 877 行: [ASYNC_SEND] PSN=0, 10.1.1.25->10.1.1.5 (数据包)
```

### 假设 2：ACK 包和数据包 PSN 冲突

Switch 发送的 ACK 包和数据包可能使用了相同的 PSN 空间，导致接收端混淆。

**需要验证**：
- ACK 包的 PSN 是否应该与数据包的 PSN 分开？
- Soft-RoCE 如何处理同一 PSN 的不同类型包？

### 假设 3：Soft-RoCE ACK 生成时机

Soft-RoCE 可能在以下情况下延迟生成 ACK：
- 收到乱序的包
- 接收队列处理延迟
- ACK coalescing（但 `ack_deferred: 0` 表示没有延迟）

## 待调查事项

1. **检查 `cache_and_broadcast` 函数**：
   - 为什么同一个 bcast_PSN 会被发送多次？
   - 广播循环是否有并发问题？

2. **检查 ACK 包的 PSN 设置**：
   - ACK 包是否应该使用独立的 PSN？
   - `forwarding` 函数中 ACK 的 PSN 来源是什么？

3. **检查 Soft-RoCE 的 ACK 生成条件**：
   - 什么情况下会延迟生成 ACK？
   - 乱序包如何影响 ACK 生成？

## 相关代码位置

| 文件 | 函数 | 行号 | 说明 |
|------|------|------|------|
| switch_main.c | cache_and_broadcast | 374 | 广播数据到所有子节点 |
| switch_main.c | handle_upstream_ack | 670 | 处理上行 ACK |
| switch_main.c | retransmit_thread | 1160 | 重传线程 |
| util.c | build_eth_packet | 331 | 构建以太网包 |

## 当前参数配置

```c
// switch_main.c
#define RETRANSMIT_TIMEOUT_US 500000   // 500ms
#define RETRANSMIT_CHECK_INTERVAL_US 100000  // 100ms
#define MAX_RETRANSMIT_BATCH 8
#define RETRANSMIT_PACKET_DELAY_US 10000  // 10ms

// parameter.h
#define SWITCH_ARRAY_LENGTH 131072
```

## 根本原因确认 (2026-01-30)

**ACK 包和广播数据包 PSN 冲突**：

1. Switch1 收到 pku2 的控制消息 (PSN=0)，发送 ACK (PSN=0) 给 pku2
2. Switch1 广播聚合数据 (bcast_PSN=0) 给 pku2
3. pku2 收到两个 PSN=0 的包（一个 ACK，一个 DATA）
4. Soft-RoCE 认为是重复包，延迟生成 ACK

**证据**：
```
第 328 行: [ASYNC_SEND] PSN=0 -> pku2 (ACK 包)
第 877 行: [ASYNC_SEND] PSN=0 -> pku2 (广播数据包)
```

## 修复方案

修改 `cache_and_broadcast` 函数，为每个连接使用独立的 `send_psn[conn_id]`：

```c
// 修改前：所有连接使用相同的 bcast_psn
send_packet_async(conn, PACKET_TYPE_DATA, ..., bcast_psn, ...);

// 修改后：每个连接使用独立的 send_psn
uint32_t send_psn = ctx->send_psn[conn_id]++;
send_packet_async(conn, PACKET_TYPE_DATA, ..., send_psn, ...);
```

## 下一步行动

1. 修改 `cache_and_broadcast` 函数使用独立的 `send_psn[conn_id]`
2. 同步修改重传逻辑以匹配新的 PSN 方案
3. 测试验证修复效果

## 日志文件位置

- `/root/NetLab/new/INC_Emulation/logs/switch1.log` - 关键日志
- `/root/NetLab/new/INC_Emulation/logs/pku1.log`
- `/root/NetLab/new/INC_Emulation/logs/pku2.log`
- `/root/NetLab/new/INC_Emulation/logs/controller.log`
