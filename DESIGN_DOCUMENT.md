# INC (In-Network Computing) 系统设计文档

## 目录

1. [项目概述](#1-项目概述)
2. [系统架构](#2-系统架构)
3. [核心组件设计](#3-核心组件设计)
4. [数据结构设计](#4-数据结构设计)
5. [网络协议设计](#5-网络协议设计)
6. [通信流程设计](#6-通信流程设计)
7. [路由与规则管理](#7-路由与规则管理)
8. [并发模型设计](#8-并发模型设计)
9. [配置管理](#9-配置管理)
10. [部署架构](#10-部署架构)
11. [API参考](#11-api参考)
12. [性能优化](#12-性能优化)

---

## 1. 项目概述

### 1.1 项目背景

INC (In-Network Computing) 是一个基于RDMA的分布式集合通信框架，利用可编程交换机实现低延迟的集合通信操作。该系统专为高性能计算和分布式机器学习场景设计，通过网络内计算（In-Network Computing）技术显著降低AllReduce等集合操作的延迟。

### 1.2 设计目标

| 目标 | 描述 |
|------|------|
| **低延迟** | 利用RDMA零拷贝和内核旁路技术，最小化通信延迟 |
| **高吞吐** | 通过流水线和滑动窗口机制，最大化网络带宽利用率 |
| **可扩展** | 支持树形拓扑结构，可扩展到大规模集群 |
| **网络内聚合** | 在交换机层面完成数据聚合，减少网络流量 |

### 1.3 核心特性

- **RRoCE v2 协议**: 基于UDP的RDMA over Converged Ethernet
- **树形聚合架构**: 分层聚合减少通信复杂度
- **滑动窗口流控**: 高效的流量控制机制
- **软件可编程交换机**: 灵活的网络内计算能力

### 1.4 技术栈

```
┌─────────────────────────────────────────────────────────┐
│                    应用层 (Application)                  │
│                   INCCL API (api.h/api.c)               │
├─────────────────────────────────────────────────────────┤
│                    传输层 (Transport)                    │
│              RDMA Verbs / libibverbs                    │
├─────────────────────────────────────────────────────────┤
│                    网络层 (Network)                      │
│           RRoCE v2 (UDP/IP + BTH + ICRC)               │
├─────────────────────────────────────────────────────────┤
│                    数据链路层 (Data Link)                │
│                    Ethernet + libpcap                   │
└─────────────────────────────────────────────────────────┘
```

---

## 2. 系统架构

### 2.1 整体架构图

```
                    ┌─────────────────┐
                    │   Controller    │
                    │  (控制平面)      │
                    └────────┬────────┘
                             │ 拓扑配置
              ┌──────────────┼──────────────┐
              │              │              │
              ▼              ▼              ▼
        ┌──────────┐   ┌──────────┐   ┌──────────┐
        │  Switch  │   │  Switch  │   │  Switch  │
        │  (Root)  │   │(Intermed)│   │(Intermed)│
        └────┬─────┘   └────┬─────┘   └────┬─────┘
             │              │              │
    ┌────────┴────────┐    ...           ...
    │                 │
    ▼                 ▼
┌────────┐       ┌────────┐
│ Host 0 │       │ Host 1 │  ...  ┌────────┐
│(Rank 0)│       │(Rank 1)│       │Host N-1│
└────────┘       └────────┘       └────────┘
```

### 2.2 组件职责

| 组件 | 职责 | 实现文件 |
|------|------|----------|
| **Controller** | 集群管理、拓扑计算、配置分发 | `controller.cpp` |
| **Switch** | 数据包转发、网络内聚合、广播 | `switch.c` |
| **Host** | 运行用户应用、发起集合操作 | `host.c`, `api.c` |

### 2.3 数据流向

```
AllReduce 数据流:

Phase 1: 聚合 (Aggregation)
Host 0 ──┐
         ├──► Switch (Leaf) ──► Switch (Root)
Host 1 ──┘                           │
                                     │ 聚合完成
                                     ▼
Phase 2: 广播 (Broadcast)
Host 0 ◄──┐
          ├──◄ Switch (Leaf) ◄── Switch (Root)
Host 1 ◄──┘
```

---

## 3. 核心组件设计

### 3.1 Controller (控制器)

#### 3.1.1 功能概述

Controller是系统的控制平面，负责：
- 管理通信组（Group）的创建和生命周期
- 收集所有节点的连接信息
- 计算最优路由拓扑
- 生成并分发交换机配置

#### 3.1.2 类设计

```cpp
// controller.h

struct controller_group {
    int group_id;                    // 组唯一标识
    int world_size;                  // 组内节点数量
    std::vector<std::string> ip_list; // 所有节点IP列表
};

struct controller_communicator {
    controller_group *group;
    int id;                          // 通信器ID
    uint32_t *qp_list;              // 所有节点的QP号列表
    std::vector<SwitchConfig> switches; // 交换机配置列表

    // 核心方法
    void calculate_route(void *topology_info);  // 计算路由
    void generate_yaml();                        // 生成配置文件
};
```

#### 3.1.3 状态机

```
Controller 状态机:

    ┌─────────────────┐
    │     IDLE        │
    └────────┬────────┘
             │ 收到 'G' (Group请求)
             ▼
    ┌─────────────────┐
    │ CREATING_GROUP  │ ◄─── 收集所有节点IP
    └────────┬────────┘
             │ 组创建完成
             ▼
    ┌─────────────────┐
    │  GROUP_READY    │
    └────────┬────────┘
             │ 收到 'C' (Communicator请求)
             ▼
    ┌─────────────────┐
    │CREATING_COMM    │ ◄─── 收集QP号，计算路由
    └────────┬────────┘
             │ 配置分发完成
             ▼
    ┌─────────────────┐
    │   COMM_READY    │
    └─────────────────┘
```

### 3.2 Switch (交换机)

#### 3.2.1 功能概述

Switch是数据平面的核心，负责：
- 接收和解析RRoCE数据包
- 执行网络内数据聚合
- 管理PSN（Packet Sequence Number）
- 处理ACK/NAK和重传

#### 3.2.2 核心数据结构

```c
// switch.c

// 聚合缓冲区
typedef struct {
    int len;                    // 数据长度
    int packet_type;           // 包类型
    int state;                 // 状态 (0=空, 1=有数据)
    int psn;                   // 包序列号
    uint32_t buffer[1036];     // 数据缓冲区
} agg_buffer_t;

// 全局状态数组 (循环缓冲区，大小N=300)
int arrival_state[FAN_IN][N];  // 上游包到达状态
int agg_degree[N];             // 聚合度计数器
agg_buffer_t agg_buffer[N];    // 聚合缓冲区
agg_buffer_t bcast_buffer[N];  // 广播缓冲区
int agg_epsn[FAN_IN];          // 各上游期望PSN
int down_epsn;                 // 下游期望PSN
```

#### 3.2.3 处理流程

```
数据包处理流程:

收到数据包
    │
    ▼
┌─────────────────┐
│  解析包头       │
│ ETH/IP/UDP/BTH  │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  查找路由规则   │
│  lookup_rule()  │
└────────┬────────┘
         │
    ┌────┴────┐
    │         │
    ▼         ▼
上游数据    下游数据
    │         │
    ▼         ▼
┌────────┐ ┌────────┐
│ 聚合   │ │ 广播   │
│处理    │ │处理    │
└────────┘ └────────┘
```

### 3.3 Host (主机)

#### 3.3.1 功能概述

Host运行用户应用程序，通过INCCL API执行集合通信操作：
- 初始化RDMA资源
- 创建通信组和通信器
- 执行AllReduce等集合操作

#### 3.3.2 RDMA资源管理

```c
// api.h

struct inccl_communicator {
    struct inccl_group *group;

    // 缓冲区配置
    uint32_t payload_buf_size;     // 负载缓冲区大小
    uint32_t window_size;          // 滑动窗口大小
    char *send_payload;            // 发送缓冲区
    char *receive_payload;         // 接收缓冲区

    // RDMA资源
    struct ibv_pd *pd;             // Protection Domain
    struct ibv_cq *cq;             // Completion Queue
    struct ibv_qp *qp;             // Queue Pair
    struct ibv_mr *mr_send_payload;    // 发送内存注册
    struct ibv_mr *mr_receive_payload; // 接收内存注册
};
```

---

## 4. 数据结构设计

### 4.1 网络包头结构

#### 4.1.1 以太网头 (14字节)

```c
typedef struct {
    uint8_t dst_mac[6];      // 目标MAC地址
    uint8_t src_mac[6];      // 源MAC地址
    uint16_t ether_type;     // 以太网类型 (0x0800 = IPv4)
} eth_header_t;
```

#### 4.1.2 IPv4头 (20字节)

```c
typedef struct {
    uint8_t version_ihl;     // 版本(4) + 头长度(5) = 0x45
    uint8_t tos;             // 服务类型
    uint16_t total_length;   // 总长度
    uint16_t id;             // 标识
    uint16_t flags_frag_off; // 标志 + 片偏移
    uint8_t ttl;             // 生存时间 (0x40 = 64)
    uint8_t protocol;        // 协议 (0x11 = UDP)
    uint16_t checksum;       // 头校验和
    uint32_t src_ip;         // 源IP地址
    uint32_t dst_ip;         // 目标IP地址
} ipv4_header_t;
```

#### 4.1.3 UDP头 (8字节)

```c
typedef struct {
    uint16_t src_port;       // 源端口
    uint16_t dst_port;       // 目标端口 (RRoCE = 4791)
    uint16_t length;         // UDP长度
    uint16_t checksum;       // UDP校验和 (可为0)
} udp_header_t;
```

#### 4.1.4 BTH头 - Base Transport Header (12字节)

```c
typedef struct {
    uint8_t opcode;          // 操作码
    uint8_t se_m_pad;        // SE + M + Pad
    uint16_t pkey;           // Partition Key
    uint32_t qpn;            // 目标QP号 (高8位保留)
    uint32_t apsn;           // ACK请求位 + PSN
} bth_header_t;
```

#### 4.1.5 AETH头 - ACK Extended Transport Header (4字节)

```c
typedef struct {
    uint32_t syn_msn;        // Syndrome(8bit) + MSN(24bit)
} aeth_t;
```

### 4.2 完整数据包格式

```
┌──────────────────────────────────────────────────────────┐
│                    Ethernet Header (14B)                 │
├──────────────────────────────────────────────────────────┤
│                    IPv4 Header (20B)                     │
├──────────────────────────────────────────────────────────┤
│                    UDP Header (8B)                       │
├──────────────────────────────────────────────────────────┤
│                    BTH Header (12B)                      │
├──────────────────────────────────────────────────────────┤
│                    AETH Header (4B) [可选]               │
├──────────────────────────────────────────────────────────┤
│                    RETH Header (16B) [可选]              │
├──────────────────────────────────────────────────────────┤
│                    Payload Data (变长)                   │
├──────────────────────────────────────────────────────────┤
│                    ICRC (4B)                             │
└──────────────────────────────────────────────────────────┘
```

### 4.3 连接配置结构

```c
typedef struct {
    char device[16];         // 网络设备名
    uint8_t my_mac[6];       // 本地MAC
    uint8_t peer_mac[6];     // 对端MAC
    uint32_t my_ip;          // 本地IP
    uint32_t peer_ip;        // 对端IP
    uint16_t my_port;        // 本地端口
    uint16_t peer_port;      // 对端端口
    uint32_t my_qp;          // 本地QP号
    uint32_t peer_qp;        // 对端QP号
    uint32_t psn;            // 包序列号
    uint32_t msn;            // 消息序列号
    int ok;                  // 连接状态
    pcap_t *handle;          // pcap句柄
} connection_t;
```

### 4.4 通信组结构

```c
struct inccl_group {
    int group_id;            // 组ID
    int rank;                // 本节点rank
    int world_size;          // 组大小
    const char *controller_ip;  // 控制器IP (rank 0使用)
    int controller_fd;          // 控制器连接 (rank 0使用)
    int *group_fd_list;         // 组内TCP连接列表

    // RDMA设备信息
    union ibv_gid local_gid;
    int local_ib_port;
    int local_gid_idx;
    struct ibv_device_attr local_device_attr;
    struct ibv_port_attr local_port_attr;
    struct ibv_context *local_ib_ctx;
};
```

---

## 5. 网络协议设计

### 5.1 RRoCE v2 协议栈

```
┌─────────────────────────────────────┐
│         IB Transport Layer          │
│    (BTH + AETH/RETH + Payload)      │
├─────────────────────────────────────┤
│              UDP                    │
│         (Port 4791)                 │
├─────────────────────────────────────┤
│              IPv4                   │
├─────────────────────────────────────┤
│            Ethernet                 │
└─────────────────────────────────────┘
```

### 5.2 包类型定义

```c
#define PACKET_TYPE_DATA        0    // 常规数据包
#define PACKET_TYPE_ACK         1    // 确认包
#define PACKET_TYPE_NAK         2    // 否定确认包
#define PACKET_TYPE_DATA_SINGLE 3    // 单数据包
#define PACKET_TYPE_RETH        4    // RDMA扩展传输头
```

### 5.3 BTH Opcode 编码

| Opcode | 含义 |
|--------|------|
| 0x00 | RC SEND First |
| 0x01 | RC SEND Middle |
| 0x02 | RC SEND Last |
| 0x04 | RC SEND Only |
| 0x0A | RC RDMA WRITE Only |
| 0x11 | RC ACK |

### 5.4 AETH Syndrome 编码

```c
// syn_msn 字段高3位
0x00 (000xxxxx)  // ACK - 正常确认
0x60 (011xxxxx)  // NAK - PSN序列错误
0x1f             // MSN掩码
```

### 5.5 PSN管理机制

```
PSN (Packet Sequence Number) 管理:

┌─────────────────────────────────────────────────┐
│  apsn 字段 (32位)                               │
│  ┌─────┬──────────────────────────────────┐    │
│  │ A   │           PSN (24位)              │    │
│  │(1位)│                                   │    │
│  └─────┴──────────────────────────────────┘    │
│  A = ACK请求位 (1=请求ACK)                      │
└─────────────────────────────────────────────────┘

循环缓冲区索引: Idx(psn) = psn % N  (N=300)
```

---

## 6. 通信流程设计

### 6.1 系统初始化流程

```
┌─────────────────────────────────────────────────────────────┐
│                    系统初始化时序图                          │
└─────────────────────────────────────────────────────────────┘

Controller          Switch              Host (Rank 0)      Host (Rank i)
    │                  │                     │                  │
    │◄─────────────────┼─────────────────────┤ 连接Controller   │
    │                  │                     │                  │
    │                  │                     │◄─────────────────┤ 连接Rank 0
    │                  │                     │                  │
    │◄─────────────────┼─────────────────────┤ 发送'G'请求      │
    │                  │                     │                  │
    │ 创建Group        │                     │                  │
    │─────────────────►│                     │                  │
    │                  │                     │                  │
    │──────────────────┼────────────────────►│ 返回group_id     │
    │                  │                     │                  │
    │◄─────────────────┼─────────────────────┤ 发送'C'请求+QP号 │
    │                  │                     │                  │
    │ 计算路由         │                     │                  │
    │ 生成YAML         │                     │                  │
    │─────────────────►│ 分发配置            │                  │
    │                  │                     │                  │
    │──────────────────┼────────────────────►│ 返回YAML         │
    │                  │                     │                  │
    │                  │                     │ 解析配置          │
    │                  │                     │ 修改QP状态        │
    │                  │                     │──────────────────►│ 同步配置
    │                  │                     │                  │
    ▼                  ▼                     ▼                  ▼
```

### 6.2 Group创建流程

```c
// inccl_group_create() 流程

1. 检测RDMA设备
   ibv_get_device_list() → ibv_open_device() → ibv_query_device()

2. 查询GID和端口属性
   ibv_query_gid() → ibv_query_port()

3. 建立组内连接
   if (rank == 0) {
       // 监听其他节点连接
       listen(MASTER_PORT)
       for (i = 1; i < world_size; i++) {
           accept() → group_fd_list[i]
       }
       // 连接Controller
       connect(controller_ip, CONTROLLER_GROUP_PORT)
       send('G')  // Group请求
   } else {
       // 连接Rank 0
       connect(rank0_ip, MASTER_PORT)
   }

4. 交换组信息
   // Rank 0收集所有IP，发送给Controller
   // Controller返回group_id
```

### 6.3 Communicator创建流程

```c
// inccl_communicator_create() 流程

1. 分配RDMA资源
   ibv_alloc_pd()           // Protection Domain
   ibv_create_cq()          // Completion Queue
   ibv_create_qp()          // Queue Pair

2. 注册内存区域
   ibv_reg_mr(send_payload)
   ibv_reg_mr(receive_payload)

3. 收集QP信息
   // Rank 0收集所有节点QP号
   // 发送给Controller

4. Controller处理
   calculate_route()        // 计算路由拓扑
   generate_yaml()          // 生成配置文件

5. 配置QP状态转换
   RESET → INIT → RTR → RTS
```

### 6.4 QP状态转换详解

```
┌─────────┐
│  RESET  │  初始状态
└────┬────┘
     │ ibv_modify_qp(IBV_QPS_INIT)
     │ - port_num, pkey_index
     │ - qp_access_flags
     ▼
┌─────────┐
│  INIT   │  可以接收WR
└────┬────┘
     │ ibv_modify_qp(IBV_QPS_RTR)
     │ - path_mtu, dest_qp_num
     │ - rq_psn, ah_attr (GID, hop_limit)
     ▼
┌─────────┐
│   RTR   │  Ready to Receive
└────┬────┘
     │ ibv_modify_qp(IBV_QPS_RTS)
     │ - timeout, retry_cnt
     │ - rnr_retry, sq_psn
     ▼
┌─────────┐
│   RTS   │  Ready to Send (完全可用)
└─────────┘
```

### 6.5 AllReduce执行流程

```
AllReduce 数据流 (RDMA WRITE模式):

Host                    Switch (Leaf)           Switch (Root)
  │                          │                       │
  │ post_send(RDMA_WRITE)    │                       │
  │─────────────────────────►│                       │
  │                          │ aggregate()           │
  │                          │──────────────────────►│
  │                          │                       │ aggregate()
  │                          │◄──────────────────────│ (完成聚合)
  │                          │ cache() + broadcast() │
  │◄─────────────────────────│                       │
  │ poll_cq() 获取结果       │                       │
  │                          │                       │
```

### 6.6 滑动窗口流控

```c
// 滑动窗口机制
#define WINDOW_SIZE 8192      // 窗口大小 (字节)
#define MESSAGE_SIZE 4096     // 单消息大小

窗口内最大未确认消息数 = WINDOW_SIZE / MESSAGE_SIZE = 2

发送流程:
┌─────────────────────────────────────────────┐
│  Window: [PSN_base, PSN_base + window_size) │
│                                             │
│  PSN_base    PSN_next         PSN_limit     │
│     │           │                │          │
│     ▼           ▼                ▼          │
│  ┌──┬──┬──┬──┬──┬──┬──┬──┐                 │
│  │✓ │✓ │✓ │? │? │  │  │  │                 │
│  └──┴──┴──┴──┴──┴──┴──┴──┘                 │
│  已确认  未确认  可发送                      │
└─────────────────────────────────────────────┘
```

---

## 7. 路由与规则管理

### 7.1 路由规则结构

```c
// rule.h

typedef struct {
    uint32_t src_ip;                    // 源IP
    uint32_t dst_ip;                    // 目标IP
    int id;                             // 连接ID
    int direction;                      // 方向 (DIR_UP/DIR_DOWN)
    connection_t *ack_conn;             // ACK返回连接
    connection_t *out_conns[MAX_PORT_NUM]; // 输出连接列表
    int out_conns_cnt;                  // 输出连接数
} rule_t;

typedef struct {
    rule_t rules[MAX_RULES];
    int count;
} rule_table_t;
```

### 7.2 方向定义

```c
#define DIR_UP   0    // 上游方向 (Host → Root)
#define DIR_DOWN 1    // 下游方向 (Root → Host)
```

### 7.3 规则查找算法

```c
rule_t* lookup_rule(rule_table_t* table, uint32_t src_ip, uint32_t dst_ip) {
    for(int i = 0; i < table->count; i++) {
        if(table->rules[i].src_ip == src_ip &&
           table->rules[i].dst_ip == dst_ip)
            return &(table->rules[i]);
    }
    return NULL;
}
```

### 7.4 规则配置示例

```
Root Switch 规则配置:
┌─────────────────────────────────────────────────────┐
│ 上游数据 (来自子节点):                               │
│   src_ip: Host_i_IP                                 │
│   dst_ip: Switch_IP                                 │
│   direction: DIR_UP                                 │
│   out_conns: [所有子节点连接] (用于广播结果)         │
├─────────────────────────────────────────────────────┤
│ 下游数据 (广播结果):                                 │
│   src_ip: Switch_IP                                 │
│   dst_ip: Host_i_IP                                 │
│   direction: DIR_DOWN                               │
│   out_conns: [对应子节点连接]                        │
└─────────────────────────────────────────────────────┘
```

### 7.5 聚合状态机

```
上游数据包处理状态机:

收到PSN=X的数据包
        │
        ▼
┌───────────────────┐
│ X < agg_epsn[id]? │──Yes──► 发送ACK(agg_epsn[id]-1) [滞后包]
└────────┬──────────┘
         │No
         ▼
┌───────────────────┐
│ X > agg_epsn[id]? │──Yes──► 发送NAK(agg_epsn[id]) [乱序包]
└────────┬──────────┘
         │No (X == agg_epsn[id])
         ▼
┌───────────────────┐
│ 更新agg_epsn[id]++│
│ arrival_state=1   │
│ 发送ACK           │
│ aggregate(X,data) │
└────────┬──────────┘
         │
         ▼
┌───────────────────┐
│agg_degree[X]==    │
│   FAN_IN?         │
└────────┬──────────┘
    Yes  │  No
    ┌────┴────┐
    ▼         ▼
┌────────┐  等待更多
│ Root?  │  数据到达
└───┬────┘
Yes │  No
    ▼    ▼
cache() forwarding()
broadcast() 到父节点
```

---

## 8. 并发模型设计

### 8.1 线程池架构

```c
// 使用C-Thread-Pool库
threadpool thpool = thpool_init(8);  // 8个工作线程

// 添加任务
thpool_add_work(thpool, send_packet_thread, &args[i]);

// 等待完成
thpool_wait(thpool);
```

### 8.2 Switch线程模型

```
Switch 进程线程结构:

┌─────────────────────────────────────────────────────┐
│                    Main Thread                       │
│              (初始化、配置解析)                       │
└─────────────────────┬───────────────────────────────┘
                      │
        ┌─────────────┼─────────────┐
        │             │             │
        ▼             ▼             ▼
┌───────────┐  ┌───────────┐  ┌───────────┐
│ Receiving │  │ Receiving │  │  Polling  │
│ Thread 0  │  │ Thread 1  │  │  Thread   │
│ (pcap)    │  │ (pcap)    │  │ (超时检测) │
└───────────┘  └───────────┘  └───────────┘
        │             │
        └──────┬──────┘
               ▼
        ┌───────────┐
        │  Thread   │
        │   Pool    │
        │ (发送任务) │
        └───────────┘
```

### 8.3 互斥锁保护

```c
pthread_mutex_t agg_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t r_mutex = PTHREAD_MUTEX_INITIALIZER;

// 聚合缓冲区访问保护
pthread_mutex_lock(&agg_mutex);
add_payload(agg_buffer[Idx(psn)].buffer, data, len);
agg_degree[Idx(psn)]++;
pthread_mutex_unlock(&agg_mutex);
```

### 8.4 后台接收线程

```c
void *background_receiving(void *arg) {
    int id = (int)(intptr_t)arg;
    connection_t* conn = &conns[id];

    // 设置pcap过滤器
    char filter[256];
    snprintf(filter, sizeof(filter),
             "udp port 4791 and src host %s",
             ip_to_str(conn->peer_ip));

    pcap_compile(handle, &fp, filter, 0, PCAP_NETMASK_UNKNOWN);
    pcap_setfilter(handle, &fp);

    // 循环捕获数据包
    pcap_loop(handle, -1, packet_handler, (uint8_t*)&id);
}
```

### 8.5 超时轮询线程

```c
void *polling_thread(void *arg) {
    while(1) {
        for(int i = 0; i < FAN_IN; i++) {
            uint64_t now_ts = get_now_ts();
            if(ts_buffer[i].ts != 0 &&
               now_ts - ts_buffer[i].ts > 100) {
                // 超时: 触发重传
                retransmit(rule, psn);
            }
        }
        usleep(10000);  // 10ms轮询间隔
    }
}
```

---

## 9. 配置管理

### 9.1 系统参数配置

```c
// parameter.h - 端口配置
#define MASTER_PORT             52223   // Host通信端口
#define CONTROLLER_GROUP_PORT   52200   // Controller组端口
#define CONTROLLER_SWITCH_PORT  52311   // Controller-Switch端口

// api.h - 通信参数
#define WINDOW_SIZE    8192    // 滑动窗口大小(字节)
#define PAYLOAD_LEN    1024    // MTU大小
#define MESSAGE_SIZE   4096    // 消息大小(4*PAYLOAD_LEN)

// switch.c - 交换机参数
#define N        300    // 循环缓冲区大小
#define FAN_IN   2      // 每个交换机的子节点数
```

### 9.2 YAML拓扑配置格式

```yaml
# topology.yaml 示例
switches:
  - id: 0
    root: true
    connections:
      - up: false
        host_id: 0
        my_ip: "192.168.1.1"
        my_mac: "00:11:22:33:44:55"
        my_port: 4791
        my_qp: 100
        peer_ip: "192.168.1.10"
        peer_mac: "00:11:22:33:44:66"
        peer_port: 4791
        peer_qp: 200
      - up: false
        host_id: 1
        my_ip: "192.168.1.1"
        my_mac: "00:11:22:33:44:55"
        my_port: 4791
        my_qp: 101
        peer_ip: "192.168.1.11"
        peer_mac: "00:11:22:33:44:77"
        peer_port: 4791
        peer_qp: 201
```

---

## 10. 部署架构

### 10.1 部署拓扑

```
┌─────────────────────────────────────────────────────────┐
│                    物理部署拓扑                          │
└─────────────────────────────────────────────────────────┘

                 ┌──────────────┐
                 │  Controller  │
                 │   (VM/物理机) │
                 └──────┬───────┘
                        │ TCP
        ┌───────────────┼───────────────┐
        │               │               │
        ▼               ▼               ▼
   ┌─────────┐    ┌─────────┐    ┌─────────┐
   │ Switch  │    │ Switch  │    │ Switch  │
   │   VM    │    │   VM    │    │   VM    │
   └────┬────┘    └────┬────┘    └────┬────┘
        │              │              │
   ┌────┴────┐    ┌────┴────┐    ┌────┴────┐
   │         │    │         │    │         │
   ▼         ▼    ▼         ▼    ▼         ▼
┌──────┐ ┌──────┐ ...
│Host 0│ │Host 1│
│ VM   │ │ VM   │
└──────┘ └──────┘
```

### 10.2 启动顺序

```bash
# 1. 启动控制器
./build/controller

# 2. 启动交换机 (参数: 控制器IP)
./build/switch <controller_ip>

# 3. 启动主机 (参数: world_size, 控制器IP, rank)
./build/host <world_size> <controller_ip> <rank>
```

### 10.3 Multipass虚拟机部署

```bash
# 创建虚拟机
./multipass_load.sh

# 配置网络地址
./multipass_address.sh

# 启动所有进程
./task.sh
```

### 10.4 系统依赖

| 依赖库 | 用途 |
|--------|------|
| libibverbs | RDMA Verbs API |
| libpcap | 网络包捕获 |
| libyaml-cpp | YAML配置解析 |
| pthread | 多线程支持 |

---

## 11. API参考

### 11.1 Group API

```c
// 创建通信组
struct inccl_group* inccl_group_create(
    int world_size,           // 组大小
    const char* controller_ip, // 控制器IP (rank 0)
    int rank                   // 本节点rank
);

// 销毁通信组
void inccl_group_destroy(struct inccl_group* group);
```

### 11.2 Communicator API

```c
// 创建通信器
struct inccl_communicator* inccl_communicator_create(
    struct inccl_group* group  // 所属通信组
);

// 销毁通信器
void inccl_communicator_destroy(struct inccl_communicator* comm);
```

### 11.3 AllReduce API

```c
// RDMA WRITE模式的AllReduce
void inccl_allreduce_write(
    struct inccl_communicator *comm,  // 通信器
    int32_t* src_data,                // 源数据
    uint32_t len,                     // 数据长度
    int32_t* dst_data                 // 目标数据
);

// Send/Recv模式的AllReduce
void inccl_allreduce_sendrecv(
    struct inccl_communicator *comm,
    int32_t* src_data,
    uint32_t len,
    int32_t* dst_data
);
```

### 11.4 使用示例

```c
int main(int argc, char *argv[]) {
    int world_size = atoi(argv[1]);
    const char *controller_ip = argv[2];
    int rank = atoi(argv[3]);

    // 1. 创建通信组
    struct inccl_group *group = inccl_group_create(
        world_size, controller_ip, rank);

    // 2. 创建通信器
    struct inccl_communicator *comm = inccl_communicator_create(group);

    // 3. 准备数据
    int32_t src_data[1024];
    int32_t dst_data[1024];
    for (int i = 0; i < 1024; i++) {
        src_data[i] = rank + 1;
    }

    // 4. 执行AllReduce
    inccl_allreduce_write(comm, src_data, 1024, dst_data);

    // 5. 清理资源
    inccl_communicator_destroy(comm);
    inccl_group_destroy(group);

    return 0;
}
```

---

## 12. 性能优化

### 12.1 RDMA优化技术

| 技术 | 描述 |
|------|------|
| **零拷贝** | 数据直接从用户空间传输，无需内核拷贝 |
| **内核旁路** | 绕过操作系统内核，减少上下文切换 |
| **RDMA WRITE** | 单边操作，无需远端CPU参与 |
| **内存注册** | 预注册内存区域，避免重复注册开销 |

### 12.2 网络内聚合优势

```
传统AllReduce vs 网络内聚合:

传统Ring AllReduce:
Host0 ──► Host1 ──► Host2 ──► Host3 ──► Host0
     ◄──      ◄──      ◄──      ◄──
通信量: O(N * data_size)

网络内聚合 (树形):
Host0 ──┐
        ├──► Switch ──► 结果
Host1 ──┘
通信量: O(log(N) * data_size)
```

### 12.3 CRC32优化

```c
// 8字节并行CRC32计算
#define POLY 0xEDB88320
uint32_t crc32_table[8][256];  // 预计算查找表

// 每次处理8字节
while (length >= 8) {
    crc = crc32_table[0][b7] ^ crc32_table[1][b6] ^
          crc32_table[2][b5] ^ crc32_table[3][b4] ^
          crc32_table[4][b3] ^ crc32_table[5][b2] ^
          crc32_table[6][b1] ^ crc32_table[7][b0];
    buf += 8;
    length -= 8;
}
```

### 12.4 流水线优化

```
滑动窗口流水线:

时间 ──────────────────────────────────►

PSN 0: [发送]──[传输]──[聚合]──[ACK]
PSN 1:    [发送]──[传输]──[聚合]──[ACK]
PSN 2:       [发送]──[传输]──[聚合]──[ACK]
...

通过重叠发送和确认，最大化带宽利用率
```

### 12.5 聚合函数优化

```c
// 使用restrict关键字避免别名分析开销
void add_payload(uint32_t *restrict dst,
                 const uint32_t *restrict src,
                 int len) {
    for (int i = 0; i < len; i++) {
        dst[i] += src[i];
    }
}
```

---

## 附录

### A. 文件清单

| 文件 | 描述 |
|------|------|
| `include/api.h` | INCCL API接口定义 |
| `include/controller.h` | Controller类定义 |
| `include/parameter.h` | 系统参数配置 |
| `include/util.h` | 数据结构和工具函数 |
| `include/rule.h` | 路由规则定义 |
| `include/topo_parser.h` | YAML解析接口 |
| `include/log.h` | 日志系统 |
| `src/api.c` | INCCL API实现 |
| `src/switch.c` | 交换机核心逻辑 |
| `src/controller.cpp` | Controller实现 |
| `src/host.c` | 主机测试程序 |
| `src/util.c` | 工具函数实现 |
| `src/rule.c` | 路由规则实现 |
| `src/topo_parser.cpp` | YAML解析实现 |

### B. 构建说明

```bash
cd repository
mkdir build && cd build
cmake ..
make
```

### C. 版本历史

| 版本 | 日期 | 描述 |
|------|------|------|
| 1.0 | 2026-01 | 初始版本 |

---

*文档生成日期: 2026-01-09*
