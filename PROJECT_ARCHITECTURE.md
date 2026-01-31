# INC_Emulation 项目架构文档

## 一、项目概述

INC_Emulation 是一个 **In-Network Computing (网内计算)** 仿真系统，在软件模拟的网络交换机中实现集合通信原语（AllReduce、Reduce、Broadcast），用于加速分布式机器学习和高性能计算。

### 核心特点

- 使用 **Soft-RoCE** 模拟 RDMA 通信
- 使用 **pcap** 在用户态抓包和发包，模拟交换机行为
- 支持 **多层树形拓扑**（1-2-4 架构）
- 实现 **滑动窗口** 机制支持大数据量传输（512MB+）

---

## 二、系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                      Host 层 (计算节点)                      │
│                                                             │
│   ┌─────────────────────────────────────────────────────┐   │
│   │              INCCL API (api.c)                      │   │
│   │  inccl_allreduce_sendrecv()                         │   │
│   │  inccl_reduce_sendrecv()                            │   │
│   │  inccl_broadcast_sendrecv()                         │   │
│   └─────────────────────────────────────────────────────┘   │
│                            │                                 │
│                            │ libibverbs (RDMA Verbs API)     │
│                            ▼                                 │
│   ┌─────────────────────────────────────────────────────┐   │
│   │              Soft-RoCE (rdma_rxe)                   │   │
│   │         软件模拟的 RDMA over Ethernet               │   │
│   └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
                            │
                            │ RRoCEv2 (UDP/IP)
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                     Switch 层 (交换机)                       │
│                                                             │
│   ┌─────────────────────────────────────────────────────┐   │
│   │              pcap 抓包/发包                          │   │
│   │         用户态网络包处理（性能瓶颈所在）              │   │
│   └─────────────────────────────────────────────────────┘   │
│                            │                                 │
│   ┌─────────────────────────────────────────────────────┐   │
│   │              数据处理逻辑                            │   │
│   │  - 数据聚合 (SUM)                                   │   │
│   │  - 数据广播                                         │   │
│   │  - PSN 管理 / ACK 流控                              │   │
│   │  - 超时重传                                         │   │
│   └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
                            │
                            │ TCP
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                   Controller 层 (控制器)                     │
│                                                             │
│   - 拓扑管理：定义交换机连接关系                            │
│   - 路由规则生成：为每个操作生成转发规则                     │
│   - YAML 配置分发：将配置下发到各交换机                      │
└─────────────────────────────────────────────────────────────┘
```

---

## 三、多层拓扑结构 (1-2-4)

```
                      Controller (192.168.0.3)
                             │
                             │ 管理网络 192.168.0.0/16
                             │
                   ┌─────────┴─────────┐
                   │    Root Switch    │
                   │   (Switch 0)      │
                   │   192.168.0.19    │
                   └────┬─────────┬────┘
                        │         │
           172.16.0.0/16│         │10.10.0.0/16
                        │         │
                ┌───────┴───┐ ┌───┴───────┐
                │  Switch 1 │ │  Switch 2 │
                │   (vm1)   │ │   (vm2)   │
                │172.16.0.5 │ │ 10.10.0.7 │
                └─────┬─────┘ └─────┬─────┘
                      │             │
         10.1.1.0/24  │             │ 10.2.1.0/24
              ┌───────┴───────┐     │
        ┌─────┴─────┐   ┌─────┴─────┐   ┌─────┴─────┐   ┌─────┴─────┐
        │   pku1    │   │   pku2    │   │   pku3    │   │   pku4    │
        │ (rank 0)  │   │ (rank 1)  │   │ (rank 2)  │   │ (rank 3)  │
        │10.1.1.21  │   │ 10.1.1.5  │   │ 10.2.1.7  │   │10.2.1.12  │
        └───────────┘   └───────────┘   └───────────┘   └───────────┘
```

---

## 四、核心数据结构

### 4.1 Host 端数据结构

```c
// 通信组 (api.h)
struct inccl_group {
    int rank;                           // 当前节点的 rank
    int world_size;                     // 总节点数
    union ibv_gid local_gid;            // 本地 GID (包含 IP 地址)
    struct ibv_context *local_ib_ctx;   // RDMA 设备上下文
    int *group_fd_list;                 // rank 0 存储与其他 rank 的 TCP 连接
    int controller_fd;                  // rank 0 与 Controller 的连接
};

// 通信器 (api.h)
struct inccl_communicator {
    struct inccl_group *group;
    uint32_t payload_buf_size;          // 缓冲区大小
    uint32_t window_size;               // 滑动窗口大小
    char *send_payload;                 // 发送缓冲区
    char *receive_payload;              // 接收缓冲区
    struct ibv_pd *pd;                  // Protection Domain
    struct ibv_cq *cq;                  // Completion Queue
    struct ibv_qp *qp;                  // Queue Pair
    struct ibv_mr *mr_send_payload;     // 发送缓冲区的 Memory Region
    struct ibv_mr *mr_receive_payload;  // 接收缓冲区的 Memory Region
};
```

### 4.2 Switch 端数据结构

```c
// PSN 状态 (switch_context.h)
typedef struct {
    int degree;                         // 已到达的数据包数量
    int r_degree;                       // 已收到的 ACK 数量
    int arrival[MAX_CONNECTIONS];       // 各连接的到达标志
    int r_arrival[MAX_CONNECTIONS];     // 各连接的 ACK 标志

    struct {
        int state;                      // 缓冲区状态 (0=空, 1=有数据)
        int len;                        // 数据长度
        int psn;                        // PSN
        int packet_type;                // RDMA opcode
        uint32_t buffer[1024];          // 数据缓冲区
    } agg_buffer;                       // 聚合缓冲区

    struct { ... } bcast_buffer;        // 广播缓冲区
    pthread_mutex_t mutex;              // 互斥锁
} psn_state_t;

// 交换机上下文 (switch_context.h)
typedef struct {
    int switch_id;                      // 交换机 ID
    int is_root;                        // 是否是根交换机
    int fan_in;                         // 连接数量

    connection_t conns[MAX_CONNECTIONS_NUM];  // 连接数组
    psn_state_t psn_states[SWITCH_ARRAY_LENGTH];  // PSN 状态数组

    // PSN 管理
    uint32_t agg_epsn[MAX_CONNECTIONS_NUM];   // 期望的上行 PSN
    uint32_t down_epsn;                       // 期望的下行 PSN
    int latest_ack[MAX_CONNECTIONS_NUM];      // 最新确认的 ACK
    uint32_t send_psn[MAX_CONNECTIONS_NUM];   // 发送 PSN 计数器

    // 路由表
    routing_table_t routing_table;
    int rank_to_conn[MAX_RANKS];              // rank -> 连接 ID 映射

    // 元数据
    primitive_type_t operation_type;          // 当前操作类型
    int root_rank;                            // root rank (Reduce/Broadcast)
} switch_context_t;
```

---

## 五、协议定义

### 5.1 RDMA 包格式

```
┌──────────────────────────────────────────────────────────────┐
│                    Ethernet Header (14 bytes)                │
├──────────────────────────────────────────────────────────────┤
│                    IPv4 Header (20 bytes)                    │
├──────────────────────────────────────────────────────────────┤
│                    UDP Header (8 bytes)                      │
│                    dst_port = 4791 (Host) / 4792 (Switch)    │
├──────────────────────────────────────────────────────────────┤
│                    BTH Header (12 bytes)                     │
│  ┌─────────┬─────────┬─────────┬─────────────────────────┐   │
│  │ opcode  │ se_m_pad│  pkey   │    QPN (24-bit)         │   │
│  │ (1 byte)│ (1 byte)│(2 bytes)│    (4 bytes)            │   │
│  ├─────────┴─────────┴─────────┴─────────────────────────┤   │
│  │                    PSN (24-bit)                        │   │
│  │                    (4 bytes)                           │   │
│  └───────────────────────────────────────────────────────┘   │
├──────────────────────────────────────────────────────────────┤
│              Immediate Data (4 bytes, 可选)                  │
│  ┌─────────────────┬────────┬────────┬────────┬──────────┐   │
│  │  dest_rank (16) │prim (2)│ op (2) │dtype(4)│reserved(8)│  │
│  └─────────────────┴────────┴────────┴────────┴──────────┘   │
├──────────────────────────────────────────────────────────────┤
│                    Payload (1024 bytes)                      │
│                    256 个 int32_t 元素                        │
├──────────────────────────────────────────────────────────────┤
│                    ICRC (4 bytes)                            │
└──────────────────────────────────────────────────────────────┘
```

### 5.2 RDMA Opcode

| Opcode | 值   | 说明                    |
|--------|------|-------------------------|
| SEND_FIRST | 0x00 | 多包消息的第一个包 |
| SEND_MIDDLE | 0x01 | 多包消息的中间包 |
| SEND_LAST | 0x02 | 多包消息的最后一个包 |
| SEND_LAST_WITH_IMM | 0x03 | 最后一个包 + Immediate Data |
| SEND_ONLY | 0x04 | 单包消息 |
| SEND_ONLY_WITH_IMM | 0x05 | 单包消息 + Immediate Data |
| ACK | 0x11 | 确认包 |

### 5.3 Immediate Data 格式

```
┌─────────────────┬──────────┬──────────┬──────────┬──────────┐
│  dest_rank (16) │ prim (2) │  op (2)  │ dtype(4) │reserved(8)│
└─────────────────┴──────────┴──────────┴──────────┴──────────┘
     位 31-16        位 15-14   位 13-12   位 11-8     位 7-0
```

| 字段 | 位数 | 说明 |
|------|------|------|
| dest_rank | 16 | 目标 rank (0xFFFF = 广播) |
| primitive | 2 | 0=AllReduce, 1=Reduce, 2=Broadcast |
| operator | 2 | 0=Barrier, 1=SUM, 2=MAX, 3=MIN |
| datatype | 4 | 0=INT32, 1=FLOAT32 |

---

## 六、通信流程

### 6.1 Group 创建流程

```
┌─────────┐                    ┌─────────┐                    ┌────────────┐
│ Rank 0  │                    │ Rank 1  │                    │ Controller │
└────┬────┘                    └────┬────┘                    └─────┬──────┘
     │                              │                               │
     │ 1. 打开 RDMA 设备            │ 1. 打开 RDMA 设备              │
     │ 2. 查询 GID                  │ 2. 查询 GID                    │
     │                              │                               │
     │ 3. 监听 TCP:52223            │                               │
     │◄─────────────────────────────│ 4. 连接到 Rank 0              │
     │                              │                               │
     │ 5. 接收 rank info            │ 发送 {rank, IP}               │
     │    (rank, IP)                │                               │
     │                              │                               │
     │ 6. 连接到 Controller ────────────────────────────────────────►│
     │                              │                               │
     │ 7. 发送 "G" + world_size ────────────────────────────────────►│
     │    + IP 列表                 │                               │
     │                              │                               │
     │◄──────────────────────────────────────────────────────────────│
     │ 8. 接收 group_id             │                               │
     │                              │                               │
```

### 6.2 Communicator 创建流程

```
┌─────────┐                    ┌─────────┐                    ┌────────────┐
│ Rank 0  │                    │ Rank 1  │                    │ Controller │
└────┬────┘                    └────┬────┘                    └─────┬──────┘
     │                              │                               │
     │ 1. 创建 PD/CQ/QP/MR          │ 1. 创建 PD/CQ/QP/MR            │
     │                              │                               │
     │◄─────────────────────────────│ 2. 发送 QP 号                  │
     │ 3. 收集所有 QP 号            │                               │
     │                              │                               │
     │ 4. 发送 "C" + QP 列表 ───────────────────────────────────────►│
     │                              │                               │
     │◄──────────────────────────────────────────────────────────────│
     │ 5. 接收 topology.yaml        │                               │
     │                              │                               │
     │ 6. 分发 topology.yaml ──────►│                               │
     │                              │                               │
     │ 7. 解析 YAML, 获取 Switch IP/QP                              │
     │                              │                               │
     │ 8. QP 状态转换:              │ 8. QP 状态转换:                │
     │    RESET → INIT → RTR → RTS │    RESET → INIT → RTR → RTS   │
     │                              │                               │
```

### 6.3 AllReduce 数据流

```
┌─────────┐     ┌─────────┐     ┌─────────┐     ┌─────────┐
│ Rank 0  │     │ Rank 1  │     │ Rank 2  │     │ Rank 3  │
└────┬────┘     └────┬────┘     └────┬────┘     └────┬────┘
     │               │               │               │
     │ 1. 发送控制消息 (imm_data: AllReduce)          │
     │──────────────►│──────────────►│──────────────►│
     │               │               │               │
     │ 2. 发送数据   │ 2. 发送数据   │ 2. 发送数据   │ 2. 发送数据
     │      │        │      │        │      │        │      │
     ▼      ▼        ▼      ▼        ▼      ▼        ▼      ▼
┌─────────────────────────────────────────────────────────────────┐
│                         Switch 层                                │
│                                                                 │
│  ┌─────────────┐                           ┌─────────────┐      │
│  │  Switch 1   │                           │  Switch 2   │      │
│  │  (叶子)     │                           │  (叶子)     │      │
│  │             │                           │             │      │
│  │ 聚合 rank0+1│                           │ 聚合 rank2+3│      │
│  └──────┬──────┘                           └──────┬──────┘      │
│         │                                         │             │
│         │              ┌─────────────┐            │             │
│         └─────────────►│  Switch 0   │◄───────────┘             │
│                        │  (根)       │                          │
│                        │             │                          │
│                        │ 聚合 SW1+SW2│                          │
│                        │             │                          │
│                        │ 广播结果    │                          │
│         ┌──────────────┤             ├──────────────┐           │
│         │              └─────────────┘              │           │
│         ▼                                           ▼           │
│  ┌─────────────┐                           ┌─────────────┐      │
│  │  Switch 1   │                           │  Switch 2   │      │
│  │  广播到     │                           │  广播到     │      │
│  │  rank 0,1   │                           │  rank 2,3   │      │
│  └─────────────┘                           └─────────────┘      │
└─────────────────────────────────────────────────────────────────┘
     │               │               │               │
     ▼               ▼               ▼               ▼
┌─────────┐     ┌─────────┐     ┌─────────┐     ┌─────────┐
│ Rank 0  │     │ Rank 1  │     │ Rank 2  │     │ Rank 3  │
│ 接收结果│     │ 接收结果│     │ 接收结果│     │ 接收结果│
└─────────┘     └─────────┘     └─────────┘     └─────────┘
```

---

## 七、滑动窗口机制

### 7.1 问题背景

大数据量传输（512MB+）时，缓冲区无法一次性容纳所有消息，需要循环复用缓冲区槽位。

### 7.2 解决方案

```c
// 全局消息索引 → 循环缓冲区槽位
int slot = message_index % window_size;
```

### 7.3 工作原理

```
消息索引 (wr_id):  0  1  2  3  4  5  6  7  8  9  10 ...
                   │  │  │  │  │  │  │  │  │  │  │
缓冲区槽位 (slot): 0  1  2  3  0  1  2  3  0  1  2  ...
                   └──────────┘  └──────────┘
                    第一轮        第二轮（复用槽位）
```

### 7.4 关键参数

| 参数 | 值 | 说明 |
|------|-----|------|
| SLIDING_WINDOW_SIZE | 4096 | 滑动窗口消息数 |
| MESSAGE_SIZE | 4096 | 每消息字节数 |
| SWITCH_ARRAY_LENGTH | 131072 | Switch PSN 数组大小 |
| QP max_send_wr | 16384 | QP 发送队列限制 |
| QP max_recv_wr | 16384 | QP 接收队列限制 |
| CQ size | 32767 | 完成队列大小限制 |

---

## 八、Switch 处理流程

### 8.1 数据包接收

```
pcap_dispatch()
      │
      ▼
packet_handler()
      │
      ├── 解析 IP/UDP/BTH 头
      │
      ├── 根据源 IP 查找 conn_id
      │
      ├── 检查 Immediate Data
      │   └── 解析操作类型 (AllReduce/Reduce/Broadcast)
      │
      └── process_packet()
```

### 8.2 数据包处理

```
process_packet()
      │
      ├── 控制消息 (opcode=0x05, payload≤4)
      │   ├── 更新操作类型到上下文
      │   ├── 发送 ACK
      │   └── return
      │
      ├── 数据消息 (上行, direction=UP)
      │   ├── PSN 检查
      │   ├── 发送 ACK
      │   └── aggregate()
      │       ├── 第一个到达: 复制到 agg_buffer
      │       ├── 后续到达: 累加到 agg_buffer
      │       └── 聚合完成:
      │           ├── 根交换机: cache_and_broadcast()
      │           └── 叶子交换机: forwarding() 到父交换机
      │
      └── 数据消息 (下行, direction=DOWN)
          ├── PSN 检查
          ├── 发送 ACK
          └── cache_and_broadcast()
              ├── 缓存到 bcast_buffer
              └── 广播到所有子连接
```

### 8.3 聚合函数 (aggregate)

```c
static int aggregate(ctx, rule, conn_id, psn, data, len, packet_type) {
    psn_state_t *state = &ctx->psn_states[Idx(psn)];

    pthread_mutex_lock(&state->mutex);

    // PSN 槽位冲突检测：当 PSN 超过 SWITCH_ARRAY_LENGTH 时，槽位会被复用
    // 如果槽位已被占用且 PSN 不匹配，丢弃 PSN 较大的包（超前一个周期的包）
    if (state->degree > 0 && state->agg_buffer.psn != psn) {
        if (psn > state->agg_buffer.psn) {
            // 当前包 PSN 较大（超前一个周期），丢弃当前包，不回复 ACK
            pthread_mutex_unlock(&state->mutex);
            return 0;  // 返回 0 表示处理失败
        }
    }

    if (state->degree == 0) {
        // 第一个到达：直接复制
        memcpy(state->agg_buffer.buffer, data, len * sizeof(uint32_t));
        state->agg_buffer.psn = psn;  // 记录 PSN
    } else {
        // 后续到达：累加
        for (int i = 0; i < len; i++) {
            uint32_t dst = ntohl(state->agg_buffer.buffer[i]);
            uint32_t src = ntohl(data[i]);
            state->agg_buffer.buffer[i] = htonl(dst + src);
        }
    }

    state->degree++;
    pthread_mutex_unlock(&state->mutex);

    // 检查是否聚合完成
    if (state->degree == expected_degree) {
        if (ctx->is_root) {
            // 根交换机：广播结果
            cache_and_broadcast(ctx, rule, psn, state->agg_buffer.buffer, len, packet_type);
        } else {
            // 叶子交换机：向上转发
            forwarding(ctx, rule, psn, PACKET_TYPE_DATA, state->agg_buffer.buffer, len, packet_type);
        }
    }
    return 1;  // 成功处理
}
```

### 8.4 PSN 槽位冲突处理

当数据量很大时，PSN 会超过 `SWITCH_ARRAY_LENGTH`，导致槽位被复用：

```
PSN:     0    1    2   ...  131071  131072  131073  ...
         │    │    │          │       │       │
Slot:    0    1    2   ...  131071    0       1     ...
                              │       │
                              └───────┴── 槽位冲突！
```

**冲突场景**：Switch2 发送速度快于 Switch1，导致 Switch2 的 PSN 超前一个周期。

**解决方案**：
- 在 `aggregate()` 中检测槽位冲突
- 如果当前包 PSN 大于槽位中已有的 PSN，丢弃当前包
- 不发送 ACK，等待重传机制处理

### 8.5 重传机制

Switch 实现了超时重传机制，用于处理丢包和槽位冲突：

```c
#define RETRANSMIT_TIMEOUT_US      1000000   // 1秒超时
#define RETRANSMIT_CHECK_INTERVAL_US 50000   // 50ms 检查间隔
```

**重传线程工作流程**：
1. 每 50ms 检查一次各连接的 ACK 状态
2. 如果某连接超过 1 秒未收到新 ACK，触发重传
3. 从 `latest_ack + 1` 开始，重传未确认的数据包
4. 每次最多重传 `MAX_RETRANSMIT_BATCH` 个包

---

## 九、关键文件说明

| 文件 | 位置 | 功能 |
|------|------|------|
| api.h/api.c | repository/include/, repository/src/ | INCCL API 实现 |
| util.h/util.c | repository/include/, repository/src/ | 网络包处理工具 |
| parameter.h | repository/include/ | 系统参数定义 |
| switch_main.c | repository/termination_switch/src/ | 交换机主程序 |
| switch_context.c | repository/termination_switch/src/ | 交换机上下文管理 |
| controller.cpp | repository/src/ | 控制器实现 |
| controller.h | repository/include/ | 控制器头文件 |
| host_simple_test.c | repository/src/ | AllReduce 测试程序 |
| host_reduce.c | repository/src/ | Reduce 测试程序 |
| host_broadcast.c | repository/src/ | Broadcast 测试程序 |

---

## 十、性能特性

### 10.1 测试结果 (1-2-4 拓扑，4 节点)

**AllReduce 性能：**

| 数据量 | 耗时 | 吞吐量 |
|--------|------|--------|
| 32MB | ~5.9s | 45.48 Mbps |
| 128MB | ~3.4s | ~300 Mbps |
| 256MB | ~6.4s | ~320 Mbps |
| 512MB | ~13.8s | ~300 Mbps |
| 1GB | ~89s | ~90 Mbps |

**Reduce 性能：**

| 数据量 | 耗时 | 吞吐量 |
|--------|------|--------|
| 32MB | ~5.3s | 50.81 Mbps |
| 512MB | ~130s | ~31 Mbps |

**性能对比说明：**
- Reduce 理论上应比 AllReduce 快（只需向 root 发送结果）
- 实际测试中 Reduce 略快于 AllReduce
- 大数据量时性能下降，主要受 PSN 槽位冲突和重传影响

### 10.2 性能瓶颈

**主要瓶颈：pcap 用户态抓包**

1. 每个包从内核拷贝到用户态，开销大
2. 逐包处理，无批量优化
3. 没有零拷贝机制

**次要因素：**

- Soft-RoCE 软件模拟 RDMA
- 多层交换机处理延迟
- 小包聚合开销

### 10.3 可能的优化方向

| 方案 | 说明 | 预期提升 |
|------|------|----------|
| DPDK | 绕过内核，直接操作网卡 | 10x+ |
| XDP/eBPF | 在内核中处理包 | 5x+ |
| raw socket + mmap | 比 pcap 轻量 | 2x |

---

## 十一、使用方法

### 11.1 编译

```bash
cd repository
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

### 11.2 运行测试

```bash
# 1. 启动 Controller
./controller

# 2. 启动 Switch (在各交换机节点)
./switch_v2 <controller_ip>

# 3. 启动 Host 测试程序
./host_simple_test <world_size> <master_addr> <rank> <data_count>

# 示例：2 节点 AllReduce 测试，每节点 256MB 数据
# 节点 0:
./host_simple_test 2 192.168.0.6 0 67108864
# 节点 1:
./host_simple_test 2 192.168.0.6 1 67108864
```

### 11.3 使用测试脚本

```bash
./manual_test.sh allreduce 67108864   # 256MB AllReduce 测试
./manual_test.sh reduce 67108864      # 256MB Reduce 测试
./manual_test.sh broadcast 67108864   # 256MB Broadcast 测试
```
