# 交换机模态-I：连接终结模式设计

## 1. 概述

### 1.1 设计目标

连接终结模式（Connection Termination Mode）利用交换机内置的 RoCE 硬件能力，作为 RDMA 连接的终结点。Host 直接与 Switch 建立 QP 连接，Switch 的 RoCE 引擎自动完成包接收、ACK、重传、消息重组，应用层只需处理完整消息的聚合和转发。

### 1.2 适用场景

- **目标厂商**：同时具备交换机和 RoCE 能力的厂商（NVIDIA、AMD、Broadcom）
- **硬件要求**：交换机内置 RoCE NIC（如 ConnectX 系列）
- **典型部署**：数据中心 AI 训练集群、HPC 环境

### 1.3 核心特点

| 特性 | 描述 |
|------|------|
| 连接终结 | Host 与 Switch 建立 QP，Switch 是连接的对端 |
| 硬件 RoCE | 复用内置 RoCE 引擎，自动处理 ACK/重传/消息重组 |
| 消息级处理 | 应用层收到完整消息，无需处理分片 |
| 分级 PS | 形成树形参数服务器架构，逐级聚合 |

---

## 2. 系统架构

### 2.1 整体架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                    IncSwitch (连接终结模式)                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────────┐         ┌─────────────────┐               │
│  │  硬件 RoCE NIC  │         │  硬件 RoCE NIC  │               │
│  │  (下行端口)     │         │  (上行端口)     │               │
│  │  QP1, QP2...    │         │  QP_parent      │               │
│  └────────┬────────┘         └────────┬────────┘               │
│           │ ibv_poll_cq()             │ ibv_post_send()        │
│           │ 完整消息                   │ 完整消息               │
│           ▼                           ▲                        │
│  ┌────────────────────────────────────────────────┐            │
│  │              应用层计算引擎 (IncEngine)          │            │
│  │         聚合 / 广播 / 转发 (复用现有逻辑)        │            │
│  └────────────────────────────────────────────────┘            │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
          │                                           │
          ▼                                           ▼
    ┌──────────┐                               ┌──────────┐
    │  Host 1  │                               │  Host N  │
    │  (QP→SW) │                               │  (QP→SW) │
    └──────────┘                               └──────────┘
```

### 2.2 与现有模式对比

| 特性 | 现有模式 (透传) | 模态-I (连接终结) |
|------|----------------|------------------|
| RDMA 连接 | Host ↔ Host (Switch 透传) | Host ↔ Switch |
| 包处理 | libpcap 逐包捕获 | 硬件 RoCE 自动处理 |
| 消息重组 | 无（逐包聚合） | 硬件自动完成 |
| ACK/重传 | 依赖 Host | Switch 硬件处理 |
| 应用层输入 | RDMA 包 | 完整消息 |
| 聚合逻辑 | 逐包聚合 | 消息级聚合 |

---

## 3. RoCE 连接管理（硬件复用）

### 3.1 设计原则

**不自己实现 RoCE 协议栈**，直接使用 Switch 内置的 RoCE NIC：
- 使用 `libibverbs` API 管理 QP
- 硬件自动处理：ACK、NAK、重传、消息重组
- 应用层通过 `ibv_poll_cq()` 获取完整消息

### 3.2 QP 连接结构

```c
typedef struct {
    // 下行连接：到 Host
    struct ibv_qp *host_qps[MAX_HOSTS];
    int host_count;

    // 上行连接：到父 Switch
    struct ibv_qp *parent_qp;

    // RDMA 资源
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_mr *mr;

    // 接收缓冲
    void *recv_buf;
    uint32_t recv_buf_size;
} switch_roce_ctx_t;
```

### 3.3 连接建立流程

```
Host                          Switch
  │                              │
  │◀──── 交换 QP 信息 (TCP) ────▶│
  │                              │
  │     ibv_modify_qp(INIT)      │
  │     ibv_modify_qp(RTR)       │
  │     ibv_modify_qp(RTS)       │
  │                              │
  │◀════ RDMA QP 连接建立 ═══════▶│
  │                              │
```

### 3.4 消息收发接口

```c
// 接收完整消息（硬件已重组）
int recv_message(switch_roce_ctx_t *ctx,
                 int *src_host_id,
                 void **data,
                 uint32_t *len);

// 发送完整消息（硬件自动分片）
int send_message(switch_roce_ctx_t *ctx,
                 int dst_host_id,
                 void *data,
                 uint32_t len);
```

---

## 4. 消息处理（硬件自动完成）

### 4.1 硬件自动处理的功能

| 功能 | 说明 |
|------|------|
| 包接收 | RoCE NIC 自动接收 |
| ACK 生成 | 硬件自动回复 |
| 重传 | 硬件检测丢包并重传 |
| 消息重组 | FIRST/MIDDLE/LAST 自动重组 |
| 流控 | 硬件信用流控 |

### 4.2 应用层只需关注

```c
// 轮询 CQ 获取完整消息
struct ibv_wc wc;
int n = ibv_poll_cq(ctx->cq, 1, &wc);
if (n > 0 && wc.status == IBV_WC_SUCCESS) {
    // wc.wr_id 标识消息
    // 从 recv_buf 获取完整消息数据
    void *msg_data = get_recv_buf(wc.wr_id);
    uint32_t msg_len = wc.byte_len;

    // 提交到计算引擎
    inc_engine_submit(msg_data, msg_len, wc.imm_data);
}
```

---

## 5. 计算引擎 (IncEngine)

### 5.1 设计理念

IncEngine 作为交换机内的计算核心，类似于分级参数服务器（Parameter Server）。

### 5.2 聚合槽位结构

```c
typedef struct {
    uint32_t slot_id;

    // 聚合状态
    uint32_t expected_count;   // 期望参与聚合的消息数
    uint32_t arrived_count;    // 已到达消息数
    uint64_t arrival_bitmap;   // 到达位图

    // 聚合缓冲
    void *agg_buffer;
    uint32_t data_len;

    // 操作信息
    uint8_t primitive;
    uint8_t op_type;
    uint8_t data_type;
    int root_rank;

    // 状态
    enum {
        SLOT_IDLE,
        SLOT_AGGREGATING,
        SLOT_COMPLETE,
        SLOT_BROADCASTING
    } state;

    pthread_mutex_t mutex;
} agg_slot_t;
```

### 5.3 聚合操作接口

```c
// 支持的归约操作
typedef enum {
    OP_SUM,
    OP_MAX,
    OP_MIN,
    OP_PROD
} reduce_op_t;

// 支持的数据类型
typedef enum {
    DTYPE_INT32,
    DTYPE_FLOAT32,
    DTYPE_FLOAT16,
    DTYPE_BFLOAT16
} data_type_t;
```

### 5.4 聚合处理流程

```c
void inc_engine_submit(agg_slot_t *slot,
                       int conn_id,
                       message_desc_t *msg);
```

**处理逻辑：**

```
消息到达
    │
    ▼
查找/创建聚合槽位 (by msg_id)
    │
    ▼
slot->state == IDLE?
    │
    ├─ Yes ──▶ 初始化槽位，复制数据
    │          slot->state = AGGREGATING
    │
    └─ No ──▶ 执行归约操作
              slot->arrived_count++
                   │
                   ▼
              arrived == expected?
                   │
                   ├─ Yes ──▶ slot->state = COMPLETE
                   │          触发下游处理
                   │
                   └─ No ──▶ 等待更多消息
```

### 5.5 归约操作实现

```c
// 向量化归约 (SIMD 优化)
static inline void reduce_sum_int32(int32_t *dst,
                                    const int32_t *src,
                                    uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        dst[i] += src[i];
    }
}

static inline void reduce_sum_float32(float *dst,
                                      const float *src,
                                      uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        dst[i] += src[i];
    }
}
```

---

## 6. 分级参数服务器架构

### 6.1 概念模型

连接终结模式下，每个交换机都是一个局部参数服务器：

```
                    ┌─────────────────┐
                    │   Root Switch   │
                    │   (Global PS)   │
                    │  聚合全局梯度    │
                    └────────┬────────┘
                             │
              ┌──────────────┼──────────────┐
              │              │              │
              ▼              ▼              ▼
       ┌──────────┐   ┌──────────┐   ┌──────────┐
       │ Leaf PS  │   │ Leaf PS  │   │ Leaf PS  │
       │ Switch 1 │   │ Switch 2 │   │ Switch 3 │
       └────┬─────┘   └────┬─────┘   └────┬─────┘
            │              │              │
       ┌────┴────┐    ┌────┴────┐    ┌────┴────┐
       │         │    │         │    │         │
       ▼         ▼    ▼         ▼    ▼         ▼
    Worker    Worker Worker  Worker Worker  Worker
```

### 6.2 层级角色定义

```c
typedef enum {
    SWITCH_ROLE_LEAF,      // 叶子交换机：连接 Worker
    SWITCH_ROLE_SPINE,     // 脊交换机：连接叶子交换机
    SWITCH_ROLE_ROOT       // 根交换机：全局聚合点
} switch_role_t;
```

### 6.3 AllReduce 数据流

```
阶段1: 上行聚合 (Reduce)
Worker → Leaf PS → Spine PS → Root PS

阶段2: 下行广播 (Broadcast)
Root PS → Spine PS → Leaf PS → Worker
```

**详细流程：**

```
Worker1 ──┐                              ┌── Worker1
Worker2 ──┼─▶ Leaf1 ──┐      ┌── Leaf1 ─┼── Worker2
          │           │      │           │
Worker3 ──┼─▶ Leaf2 ──┼─▶Root┼── Leaf2 ─┼── Worker3
Worker4 ──┘           │      │           └── Worker4
          │           │      │           │
Worker5 ──┼─▶ Leaf3 ──┘      └── Leaf3 ─┼── Worker5
Worker6 ──┘                              └── Worker6

     [上行聚合]              [下行广播]
```

### 6.4 交换机间连接管理

```c
typedef struct {
    // 上行连接 (到父交换机)
    switch_qp_t *parent_qp;
    uint32_t parent_switch_id;

    // 下行连接 (到子交换机或 Worker)
    switch_qp_t *child_qps[MAX_CHILDREN];
    uint32_t child_count;

    // 角色
    switch_role_t role;
} hierarchical_ctx_t;
```

---

## 7. 消息发送（硬件自动分片）

### 7.1 设计原则

发送时只需调用 `ibv_post_send()`，硬件自动处理分片：

```c
// 发送完整消息，硬件自动分片
struct ibv_send_wr wr = {
    .wr_id = msg_id,
    .sg_list = &sge,
    .num_sge = 1,
    .opcode = IBV_WR_SEND_WITH_IMM,
    .send_flags = IBV_SEND_SIGNALED,
    .imm_data = htonl(imm_data)
};
ibv_post_send(qp, &wr, &bad_wr);
```

---

## 8. 数据流完整示例

### 8.1 AllReduce 场景 (4 Workers, 2 Leaf, 1 Root)

```
T1: Workers 通过 RDMA QP 发送梯度到各自的 Leaf Switch
    (硬件自动处理分片、ACK、重传)

T2: Leaf Switch 的 RoCE NIC 接收并重组为完整消息
    应用层通过 ibv_poll_cq() 获取

T3: Leaf IncEngine 聚合本地 Workers 的梯度

T4: Leaf 通过 ibv_post_send() 发送聚合结果到 Root
    (硬件自动分片)

T5: Root RoCE NIC 接收并重组

T6: Root IncEngine 聚合所有 Leaf 的结果

T7: Root 广播全局结果到所有 Leaf

T8: Leaf 广播到各自的 Workers

T9: Workers 收到全局聚合结果
```

### 8.2 硬件 RoCE vs 软件处理对比

| 功能 | 软件处理 (libpcap) | 硬件 RoCE |
|------|-------------------|-----------|
| 包接收 | pcap_dispatch() | NIC 硬件 |
| ACK 生成 | 手动构造发送 | 硬件自动 |
| 重传 | 软件定时器 | 硬件自动 |
| 消息重组 | 手动状态机 | 硬件自动 |
| 分片发送 | 手动分片 | 硬件自动 |
| 应用层接口 | 原始包 | 完整消息 |

---

## 9. 实现规划

### 9.1 文件结构（简化版 - 硬件 RoCE 复用）

```
repository/
├── include/
│   ├── mode1/
│   │   ├── qp_manager.h         # QP 连接管理（使用 libibverbs）
│   │   ├── inc_engine.h         # 计算引擎（聚合/广播）
│   │   └── hierarchical_ps.h    # 分级 PS 架构
│   └── switch_mode.h            # 模式选择
├── src/
│   └── mode1/
│       ├── qp_manager.c         # QP 创建、状态转换、销毁
│       ├── inc_engine.c         # 消息级聚合逻辑
│       └── hierarchical_ps.c    # 层级通信管理
└── termination_switch/
    └── src/
        └── switch_main_mode1.c  # 模态-I 主程序
```

**说明：** 由于复用硬件 RoCE，无需实现：
- ~~message_reassembly.h/c~~ - 硬件自动完成消息重组
- ~~message_segment.h/c~~ - 硬件自动完成消息分片
- ~~roce_endpoint.h/c~~ - 直接使用 libibverbs API

### 9.2 开发阶段（硬件 RoCE 复用）

**阶段 1：QP 连接管理**
- 使用 libibverbs 创建 QP、PD、CQ、MR
- 实现 QP 状态转换 (RESET → INIT → RTR → RTS)
- TCP 辅助通道交换 QP 信息 (QPN, GID, PSN)
- 连接池管理（多 Host 连接）

**阶段 2：计算引擎 (IncEngine)**
- 聚合槽位管理（复用现有设计）
- 消息级归约操作（SUM, MAX, MIN）
- SIMD 优化（可选）
- 与 ibv_poll_cq() 集成

**阶段 3：分级 PS 集成**
- 层级拓扑配置（Leaf/Spine/Root）
- 交换机间 QP 连接
- 上行聚合 + 下行广播逻辑
- 端到端测试

### 9.3 与现有代码的关系

| 现有模块 | 模态-I 复用 | 说明 |
|----------|------------|------|
| api.c | 参考 | QP 管理代码可复用 |
| switch_context.h | 扩展 | 新增消息级聚合状态 |
| rule.c | 部分复用 | 聚合/广播逻辑复用 |
| controller.cpp | 扩展 | 新增模态配置 |
| util.c | 不需要 | 无需逐包解析 |

---

## 10. 与现有模式的切换

### 10.1 模式选择

```c
typedef enum {
    SWITCH_MODE_PASSTHROUGH,  // 现有模式：逐包处理
    SWITCH_MODE_TERMINATION   // 模态-I：连接终结
} switch_mode_t;
```

### 10.2 配置方式

通过 Controller YAML 配置：

```yaml
switch:
  id: 0
  mode: termination  # passthrough | termination
  is_root: true
```

---

## 11. 总结

### 11.1 模态-I 核心优势

| 优势 | 说明 |
|------|------|
| 语义完整 | 消息级处理，支持复杂操作 |
| 可靠传输 | 交换机本地重传，降低端到端延迟 |
| 灵活扩展 | 易于添加新的计算原语 |
| 分级聚合 | 类 PS 架构，减少网络流量 |

### 11.2 适用场景对比

| 场景 | 推荐模式 |
|------|----------|
| 小消息、低延迟 | 现有模式 (Passthrough) |
| 大消息、高吞吐 | 模态-I (Termination) |
| 复杂计算原语 | 模态-I (Termination) |
| 简单拓扑 | 现有模式 |
| 大规模分级拓扑 | 模态-I (Termination) |

### 11.3 后续工作

1. 实现 QP 连接管理模块（复用 libibverbs）
2. 实现消息级计算引擎
3. 分级 PS 架构集成测试
4. 性能基准测试与优化
5. 与现有模式的无缝切换

