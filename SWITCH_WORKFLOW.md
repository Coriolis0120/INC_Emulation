# INC交换机完整工作流程

## 目录
- [系统架构概述](#系统架构概述)
- [数据结构](#数据结构)
- [初始化流程](#初始化流程)
- [上行聚合流程](#上行聚合流程)
- [下行广播流程](#下行广播流程)
- [流控与重传机制](#流控与重传机制)
- [集合通信操作](#集合通信操作)
- [线程模型](#线程模型)

---

## 系统架构概述

INC交换机是基于RRoCE v2协议的可编程交换机,支持在网络层进行数据聚合操作。交换机在树形拓扑中充当中间节点,实现分布式集合通信。

### 核心功能
1. **数据包捕获**: 使用libpcap捕获RRoCE v2数据包
2. **数据聚合**: 对多个上行数据流进行求和聚合
3. **数据广播**: 将聚合结果广播到下游节点
4. **流量控制**: 基于PSN的滑动窗口机制
5. **可靠传输**: ACK/NAK机制保证数据可靠性

### 拓扑角色
- **根交换机** (root=1): 树的根节点,负责最终聚合和广播
- **中间交换机** (root=0): 树的中间节点,负责部分聚合和转发

### 关键参数
```c
#define FAN_IN 2        // 扇入系数:每个交换机的子节点数量
#define N 300           // 循环缓冲区大小
#define PAYLOAD_LEN 1024  // MTU大小(字节)
```

---

## 数据结构

### 1. 连接信息 (connection_t)
```c
typedef struct {
    char device[32];        // 网络接口名称 (eth0, eth1等)
    uint8_t my_mac[6];      // 本端MAC地址
    uint8_t peer_mac[6];    // 对端MAC地址
    uint32_t my_ip;         // 本端IP地址
    uint32_t peer_ip;       // 对端IP地址
    uint16_t my_port;       // 本端端口(4791)
    uint16_t peer_port;     // 对端端口
    uint32_t my_qp;         // 本端QP号
    uint32_t peer_qp;       // 对端QP号
    pcap_t *handle;         // pcap句柄
    uint32_t psn;           // 当前发送PSN
    uint32_t msn;           // 当前发送MSN
    int ok;                 // 连接状态
} connection_t;
```

**说明**: 交换机有 `FAN_IN + 1` 个连接:
- `conns[0..FAN_IN-1]`: 下行连接(到子节点或主机)
- `conns[FAN_IN]`: 上行连接(到父节点或根)

### 2. 聚合缓冲区 (agg_buffer_t)
```c
typedef struct {
    int len;                    // 数据长度(uint32_t个数)
    int packet_type;            // RDMA操作类型
    int state;                  // 状态: 0-空闲, 1-已使用
    int psn;                    // 对应的PSN
    int operation_type;         // 操作类型: 0-AllReduce, 1-Reduce
    int root_rank;              // Reduce的根节点rank
    uint32_t buffer[1036];      // 数据缓冲区
} agg_buffer_t;
```

**用途**: 存储正在聚合的数据,每个PSN对应一个缓冲区槽位(通过 `Idx(psn) = psn % N` 映射)

### 3. 路由规则 (rule_t)
```c
typedef struct {
    uint32_t src_ip;            // 源IP匹配
    uint32_t dst_ip;            // 目的IP匹配
    int id;                     // 子节点编号(0~FAN_IN-1)
    int direction;              // 方向: DIR_UP或DIR_DOWN
    connection_t *ack_conn;     // ACK回复连接
    connection_t *out_conns[MAX_PORT_NUM];  // 转发目标连接
    int out_conns_cnt;          // 转发目标数量
} rule_t;
```

**说明**: 路由表由controller生成,根据IP五元组查找转发规则

### 4. 全局状态数组
```c
// 上行数据到达状态
int arrival_state[FAN_IN][N];          // [子节点ID][PSN%N]

// 下行ACK到达状态
int r_arrival_state[FAN_IN][N];        // [子节点ID][PSN%N]

// 聚合缓冲区和广播缓冲区
agg_buffer_t agg_buffer[N];             // 聚合缓冲区
agg_buffer_t bcast_buffer[N];           // 广播缓冲区

// 聚合度计数
int agg_degree[N];                      // 每个PSN已收到的上行数据份数

// 期望PSN
int agg_epsn[FAN_IN];                   // 每个上行连接的期望PSN
int down_epsn;                          // 下行期望PSN

// 最新确认PSN
int latest_ack[FAN_IN];                 // 每个下行连接最后确认的PSN

// 元数据全局变量
primitive_type_t current_operation_type;  // 当前操作类型
int current_root_rank;                    // 当前根节点rank

// Rank映射
int rank_to_conn[FAN_IN];               // rank到连接ID的映射
```

---

## 初始化流程

### 主流程 (main函数)
```
1. 初始化日志系统
   └─> log_init()

2. 连接controller并获取配置
   └─> init_all(controller_ip)
       ├─> controller_thread()           # 在单独线程中运行
       │   ├─> 连接到controller (TCP, port 52311)
       │   ├─> 接收switch_id
       │   ├─> 接收topology.yaml文件
       │   └─> 解析YAML获取连接配置
       │
       ├─> 初始化所有连接的pcap句柄
       │   └─> 为每个connection_t设置pcap过滤器
       │
       ├─> 初始化全局数据结构
       │   ├─> 清零arrival_state
       │   ├─> 清零r_arrival_state
       │   ├─> 清零agg_buffer和bcast_buffer
       │   └─> 初始化互斥锁(agg_mutex, r_mutex)
       │
       ├─> 构建路由表
       │   └─> build_rules()
       │       ├─> 遍历所有连接
       │       ├─> 为每个连接添加上行和下行规则
       │       └─> 打印路由表
       │
       └─> 创建线程池
           └─> thpool_init(FAN_IN)       # 用于多播发送

3. 创建接收线程
   └─> 为每个连接创建一个线程
       ├─> receivers[0..FAN_IN-1]: 下行连接接收线程
       └─> receivers[FAN_IN]: 上行连接接收线程(根节点无此线程)

4. (可选)创建轮询线程
   └─> polling_thread()                  # 处理超时重传

5. 等待所有线程结束
   └─> pthread_join()
```

### 控制器通信详解 (controller_thread)
```
1. 创建TCP socket
   └─> socket(AF_INET, SOCK_STREAM, 0)

2. 连接到controller
   └─> connect(sockfd, controller_addr, ...)
       ├─> IP: controller_ip
       └─> Port: CONTROLLER_SWITCH_PORT (52311)

3. 接收switch_id (4字节)
   └─> recv(sockfd, &switch_id, 4, ...)
       └─> 示例: switch_id = 0

4. 接收topology.yaml文件
   └─> receive_file(sockfd, "/home/ubuntu/topology.yaml")
       ├─> 先接收文件长度(4字节)
       └─> 再接收文件内容

5. 解析YAML配置
   └─> parse_config("/home/ubuntu/topology.yaml", ...)
       ├─> 输出: root标志
       ├─> 输出: conns[]数组
       └─> 输出: rank_to_conn[]映射

6. 关闭连接
   └─> close(sockfd)
```

### YAML配置示例
```yaml
switches:
  - id: 0
    root: true
    connections:
      - up: false              # false=主机, true=交换机
        host_id: 0
        my_ip: "192.168.1.1"
        my_mac: "00:11:22:33:44:55"
        my_name: "eth0"
        my_port: 4791
        my_qp: 100
        peer_ip: "192.168.1.10"
        peer_mac: "52:54:00:9e:ed:87"
        peer_port: 4791
        peer_qp: 200
```

### 路由表构建 (build_rules)
```
对于每个连接 i (0 到 FAN_IN):

  1. 创建上行规则 (DIR_UP)
     ├─> src_ip = conns[i].peer_ip
     ├─> dst_ip = conns[FAN_IN].peer_ip  (根节点无此规则)
     ├─> direction = DIR_UP
     ├─> ack_conn = &conns[i]            # ACK发回给发送者
     └─> out_conns[0] = &conns[FAN_IN]   # 转发到上行连接

  2. 创建下行规则 (DIR_DOWN)
     ├─> src_ip = conns[FAN_IN].peer_ip  (根节点规则特殊)
     ├─> dst_ip = conns[i].peer_ip
     ├─> direction = DIR_DOWN
     ├─> ack_conn = &conns[FAN_IN]       # ACK发给上级
     └─> out_conns = &conns[0..FAN_IN-1] # 广播到所有下行连接
```

---

## 上行聚合流程

上行数据流: **主机 → 叶交换机 → 中间交换机 → 根交换机**

### 1. 接收线程 (background_receiving)
```
循环执行:
  └─> pcap_dispatch(handle, -1, packet_handler, conn_id)
      └─> 捕获数据包,调用packet_handler处理
```

### 2. 数据包处理 (packet_handler)

#### 2.1 解析数据包头部
```
1. 解析以太网头部 (14字节)
   └─> eth_header_t: src_mac, dst_mac, ethertype

2. 解析IPv4头部 (20字节)
   └─> ipv4_header_t: src_ip, dst_ip, protocol

3. 解析UDP头部 (8字节)
   └─> udp_header_t: src_port, dst_port, length

4. 解析BTH头部 (12字节)
   └─> bth_header_t: opcode, psn, dst_qp
       └─> PSN = ntohl(bth->apsn) & 0x00FFFFFF
```

#### 2.2 路由查找
```
rule = lookup_rule(&table, ip->src_ip, ip->dst_ip)
└─> 遍历路由表,匹配src_ip和dst_ip
    └─> 返回对应的rule_t指针
```

#### 2.3 上行数据处理 (rule->direction == DIR_UP)
```
if (bth->opcode == 0x04) {  // RC SEND ONLY

    提取数据:
    ├─> data = BTH头部之后的payload
    └─> data_len = (udp->length - headers) / 4

    if (psn < agg_epsn[id]) {
        // 情况1: 滞后包 (已经处理过的PSN)
        └─> forwarding(rule, agg_epsn[id]-1, PACKET_TYPE_ACK, ...)
            └─> 发送旧的ACK
    }
    else if (psn > agg_epsn[id]) {
        // 情况2: 超前包 (乱序到达)
        └─> forwarding(rule, agg_epsn[id], PACKET_TYPE_NAK, ...)
            └─> 发送NAK,请求重传
    }
    else {  // psn == agg_epsn[id]
        // 情况3: 期望包

        agg_epsn[id]++  // 更新期望PSN

        if (arrival_state[id][Idx(psn)] == 1) {
            // 子情况A: 重传包(已处理过)
            └─> forwarding(rule, psn, PACKET_TYPE_ACK, ...)
        }
        else {
            // 子情况B: 首次到达

            1. 标记到达状态
               └─> arrival_state[id][Idx(psn)] = 1

            2. 发送ACK
               └─> forwarding(rule, psn, PACKET_TYPE_ACK, ...)

            3. 提取元数据 (仅PSN=0)
               if (psn == 0) {
                   operation_type = ntohl(data[0])
                   root_rank = ntohl(data[1])
                   current_operation_type = operation_type
                   current_root_rank = root_rank
               }
               else {
                   operation_type = current_operation_type
                   root_rank = current_root_rank
               }

            4. 调用聚合函数
               └─> aggregate(rule, psn, data, data_len,
                            bth->opcode, operation_type, root_rank)
        }
    }
}
```

### 3. 聚合函数 (aggregate)
```
功能: 累加所有上行数据,当FAN_IN个数据都到达时触发下一步操作

加锁: pthread_mutex_lock(&agg_mutex)

if (agg_degree[Idx(psn)] == 0) {
    // 第一份数据: 初始化缓冲区

    if (psn == 0) {
        // PSN=0: 保留元数据,聚合实际数据
        agg_buffer[Idx(psn)].buffer[0] = data[0]  // operation_type
        agg_buffer[Idx(psn)].buffer[1] = data[1]  // root_rank
        for (i = 2; i < len; i++) {
            agg_buffer[Idx(psn)].buffer[i] = data[i]
        }
    }
    else {
        // PSN>0: 全部聚合
        memcpy(agg_buffer[Idx(psn)].buffer, data, len*4)

        // 从PSN=0获取元数据
        agg_buffer[Idx(psn)].operation_type = agg_buffer[Idx(0)].operation_type
        agg_buffer[Idx(psn)].root_rank = agg_buffer[Idx(0)].root_rank
    }
}
else {
    // 后续数据: 累加到缓冲区

    if (psn == 0) {
        // PSN=0: 跳过元数据,只聚合实际数据
        for (i = 2; i < len; i++) {
            dst_val = ntohl(agg_buffer[Idx(psn)].buffer[i])
            src_val = ntohl(data[i])
            agg_buffer[Idx(psn)].buffer[i] = htonl(dst_val + src_val)
        }
    }
    else {
        // PSN>0: 全部聚合
        for (i = 0; i < len; i++) {
            dst_val = ntohl(agg_buffer[Idx(psn)].buffer[i])
            src_val = ntohl(data[i])
            agg_buffer[Idx(psn)].buffer[i] = htonl(dst_val + src_val)
        }
    }
}

// 更新元数据
agg_buffer[Idx(psn)].len = len
agg_buffer[Idx(psn)].packet_type = packet_type
agg_buffer[Idx(psn)].state = 1
agg_buffer[Idx(psn)].psn = psn

// 增加聚合度
agg_degree[Idx(psn)]++

解锁: pthread_mutex_unlock(&agg_mutex)

// 检查是否所有上行数据都已到达
if (agg_degree[Idx(psn)] == FAN_IN) {

    if (root == 1) {
        // ========== 根交换机: 开始下行广播 ==========

        if (operation_type == OPERATION_ALLREDUCE) {
            // AllReduce: 广播到所有子节点

            1. 缓存聚合结果
               └─> cache(rule, psn, agg_buffer[Idx(psn)].buffer, len, packet_type, false)

            2. 触发广播
               └─> aeth.syn_msn = 0  (ACK)
               └─> down_ack_handler(&aeth, NULL, psn)
        }
        else if (operation_type == OPERATION_REDUCE) {
            // Reduce: 单播到root_rank

            1. 查找目标连接ID
               └─> target_conn_id = rank_to_conn[root_rank]

            2. 构造单播规则
               └─> unicast_rule.out_conns[0] = &conns[target_conn_id]
               └─> unicast_rule.out_conns_cnt = 1

            3. 发送数据
               └─> forwarding(&unicast_rule, psn, PACKET_TYPE_DATA, ...)

            4. 清理聚合缓冲区
               └─> agg_degree[Idx(psn)] = 0
               └─> agg_buffer[Idx(psn)].state = 0
        }
    }
    else {
        // ========== 中间交换机: 向上转发 ==========

        1. 查找上行规则
           └─> up_rule = lookup_rule(..., DIR_UP)

        2. 转发聚合结果
           └─> forwarding(up_rule, psn, PACKET_TYPE_DATA,
                         agg_buffer[Idx(psn)].buffer, len, packet_type)

        3. 清理聚合缓冲区
           └─> agg_degree[Idx(psn)] = 0
           └─> agg_buffer[Idx(psn)].state = 0
    }
}
```

### 4. ACK发送 (forwarding - ACK模式)
```
if (type == PACKET_TYPE_ACK || type == PACKET_TYPE_NAK) {

    1. 获取ACK目标连接
       └─> conn = rule->ack_conn

    2. 构造ACK包
       └─> build_eth_packet(
               packet, type, NULL, 0,
               conn->my_mac, conn->peer_mac,
               conn->my_ip, conn->peer_ip,
               conn->my_port, conn->peer_port,
               conn->peer_qp, psn, psn+1, 0x11, NULL
           )
           └─> 包含: ETH + IP + UDP + BTH + AETH头部
           └─> AETH.syn_msn = 0 (ACK) 或 1 (NAK)

    3. 发送ACK包
       └─> pcap_sendpacket(handle, packet, size)
}
```

---

## 下行广播流程

下行数据流: **根交换机 → 中间交换机 → 叶交换机 → 主机**

### 1. 根交换机启动广播 (cache + down_ack_handler)

#### 1.1 缓存聚合结果 (cache)
```
功能: 将聚合结果存入广播缓冲区,准备广播

assert(psn == down_epsn)
down_epsn++

1. 复制数据到广播缓冲区
   └─> memcpy(bcast_buffer[Idx(psn)].buffer, data, len*4)

2. 设置元数据
   ├─> bcast_buffer[Idx(psn)].len = len
   ├─> bcast_buffer[Idx(psn)].packet_type = packet_type
   ├─> bcast_buffer[Idx(psn)].state = 1
   └─> bcast_buffer[Idx(psn)].psn = psn

3. 查找下行规则
   └─> down_rule = lookup_rule(..., DIR_DOWN)

4. 发送广播数据
   └─> forwarding(down_rule, psn, PACKET_TYPE_DATA,
                 bcast_buffer[Idx(psn)].buffer, len, packet_type)
```

#### 1.2 广播数据发送 (forwarding - DATA模式)
```
if (type == PACKET_TYPE_DATA) {

    if (rule->out_conns_cnt == 1) {
        // 单播模式 (Reduce使用)

        1. 获取目标连接
           └─> conn = rule->out_conns[0]

        2. 构造数据包
           └─> build_eth_packet(
                   packet, PACKET_TYPE_DATA, data, len*4,
                   conn->my_mac, conn->peer_mac,
                   conn->my_ip, conn->peer_ip,
                   conn->my_port, conn->peer_port,
                   conn->peer_qp, psn, psn+1, packet_type, NULL
               )

        3. 发送数据包
           └─> pcap_sendpacket(handle, packet, size)
    }
    else {
        // 多播模式 (AllReduce使用)
        └─> send_packets_multithread(rule, type, data, len, psn, packet_type)
    }
}
```

#### 1.3 多播发送 (send_packets_multithread)
```
功能: 使用线程池并行发送数据到多个目标

for (i = 0; i < rule->out_conns_cnt; i++) {

    1. 准备发送任务
       └─> task.rule = rule
       └─> task.conn = rule->out_conns[i]
       └─> task.data = data
       └─> task.len = len
       └─> task.psn = psn
       └─> task.packet_type = packet_type

    2. 提交到线程池
       └─> thpool_add_work(thpool, send_packet_thread, &task)
}
```

#### 1.4 线程池发送 (send_packet_thread)
```
1. 构造数据包
   └─> build_eth_packet(...)

2. 发送数据包
   └─> pcap_sendpacket(conn->handle, packet, size)
```

### 2. 中间交换机接收广播 (packet_handler - DIR_DOWN)
```
if (rule->direction == DIR_DOWN) {

    if (psn < down_epsn) {
        // 情况1: 滞后包
        └─> forwarding(rule, down_epsn-1, PACKET_TYPE_ACK, ...)
    }
    else if (psn > down_epsn) {
        // 情况2: 超前包
        └─> forwarding(rule, down_epsn, PACKET_TYPE_NAK, ...)
    }
    else {  // psn == down_epsn
        // 情况3: 期望包

        if (bcast_buffer[Idx(psn)].state == 1) {
            // 子情况A: 重传包
            └─> forwarding(rule, psn, PACKET_TYPE_ACK, ...)
        }
        else {
            // 子情况B: 首次到达

            1. 发送ACK给上级
               └─> forwarding(rule, psn, PACKET_TYPE_ACK, ...)

            2. 缓存并继续广播
               └─> cache(rule, psn, data, data_len, bth->opcode, true)
                   ├─> 存入bcast_buffer
                   ├─> down_epsn++
                   └─> forwarding(down_rule, psn, PACKET_TYPE_DATA, ...)
                       └─> 广播到下游节点
        }
    }
}
```

### 3. 下行ACK处理 (down_ack_handler)
```
功能: 处理子节点发来的ACK,实现可靠广播

if (aeth->syn_msn == 0) {  // ACK

    if (rule == NULL) {
        // 根交换机自己触发广播
        └─> 跳过ACK检查
    }
    else {
        // 收到子节点的ACK

        1. 更新ACK状态
           └─> r_arrival_state[rule->id][Idx(psn)] = 1

        2. 更新最新确认PSN
           └─> latest_ack[rule->id] = psn
    }

    3. 检查是否所有子节点都已确认
       all_acked = true
       for (i = 0; i < FAN_IN; i++) {
           if (r_arrival_state[i][Idx(psn)] == 0) {
               all_acked = false
           }
       }

    4. 如果全部确认,清理缓冲区
       if (all_acked) {
           ├─> bcast_buffer[Idx(psn)].state = 0
           ├─> 清零r_arrival_state[][Idx(psn)]
           └─> 清零agg_degree[Idx(psn)]
       }
}
else if (aeth->syn_msn == 1) {  // NAK
    // 重传数据
    └─> retransmit(rule, psn)
}
```

---

## 流控与重传机制

### 1. 滑动窗口机制

#### PSN循环缓冲区
```
PSN范围: 0 ~ 2^24-1 (24位)
缓冲区大小: N = 300

索引映射: Idx(psn) = psn % N

示例:
  PSN 0, 300, 600 → Idx = 0
  PSN 1, 301, 601 → Idx = 1
```

#### 期望PSN (Expected PSN)
```
上行连接: agg_epsn[0..FAN_IN-1]
  └─> 每个上行连接独立维护
  └─> 收到期望PSN后递增: agg_epsn[id]++

下行连接: down_epsn
  └─> 全局共享
  └─> 收到下行数据后递增: down_epsn++
```

### 2. ACK/NAK机制

#### ACK (Acknowledgement)
```
触发条件: psn == epsn 且首次到达
ACK类型: AETH.syn_msn = 0

格式:
  ETH (14B) + IP (20B) + UDP (8B) + BTH (12B) + AETH (4B) + ICRC (4B)

AETH字段:
  └─> syn_msn = 0 (ACK)
  └─> msn = psn + 1
```

#### NAK (Negative Acknowledgement)
```
触发条件: psn > epsn (乱序)
NAK类型: AETH.syn_msn = 1

功能: 请求重传丢失的数据包
```

### 3. 重传机制

#### 被动重传 (NAK触发)
```
接收方发送NAK → 发送方收到NAK → retransmit()

retransmit(rule, psn):
  └─> forwarding(rule, psn, PACKET_TYPE_DATA_SINGLE,
                bcast_buffer[Idx(psn)].buffer, ...)
```

#### 主动重传 (超时检测)
```
轮询线程: polling_thread()

每10ms检查一次:
  for (i = 0; i < FAN_IN; i++) {
      if (now_ts - ts_buffer[i].ts > 100μs) {
          // 超时: 从最后确认PSN开始重传
          psn = latest_ack[i] + 1
          while (bcast_buffer[Idx(psn)].state == 1) {
              retransmit(rule, psn)
              psn++
          }
      }
  }
```

### 4. 状态维护

#### arrival_state (上行)
```
用途: 记录哪些上行数据包已经到达
大小: [FAN_IN][N]

生命周期:
  设置: arrival_state[id][Idx(psn)] = 1  (首次到达时)
  清除: 聚合完成后不清除(用于检测重传)
```

#### r_arrival_state (下行)
```
用途: 记录哪些子节点已经确认广播数据
大小: [FAN_IN][N]

生命周期:
  设置: r_arrival_state[id][Idx(psn)] = 1  (收到ACK时)
  清除: 所有子节点都确认后清零
```

#### agg_degree
```
用途: 记录每个PSN已收到的上行数据份数
大小: [N]

生命周期:
  递增: agg_degree[Idx(psn)]++  (每次聚合时)
  清零: 聚合完成后

触发条件: agg_degree[Idx(psn)] == FAN_IN
```

---

## 集合通信操作

### 1. AllReduce操作

#### 元数据格式 (PSN=0)
```
data[0] = OPERATION_ALLREDUCE (0)
data[1] = root_rank (不使用)
data[2..PAYLOAD_COUNT-1] = 实际数据
```

#### 流程图
```
主机0              主机1
  │                 │
  ├─ 发送数据 ──────┤
  │                 │
  ▼                 ▼
┌─────────────────────┐
│    叶交换机 (S0)     │
│   aggregate()       │
└──────────┬──────────┘
           │ 聚合结果
           ▼
┌─────────────────────┐
│    根交换机 (Root)   │
│   aggregate()       │
│   cache()           │
│   down_ack_handler()│
└──────────┬──────────┘
           │ 广播结果
           ▼
┌─────────────────────┐
│    叶交换机 (S0)     │
│   cache()           │
└──────────┬──────────┘
           │ 广播到主机
  ┌────────┴────────┐
  ▼                 ▼
主机0              主机1
(收到结果)        (收到结果)
```

#### 关键代码路径
```
1. 上行聚合:
   packet_handler() → aggregate() → (聚合完成)

2. 根节点广播:
   → cache() → forwarding(PACKET_TYPE_DATA, 多播)
   → down_ack_handler()

3. 下行转发:
   packet_handler(DIR_DOWN) → cache() → forwarding()
```

### 2. Reduce操作

#### 元数据格式 (PSN=0)
```
data[0] = OPERATION_REDUCE (1)
data[1] = root_rank (目标rank)
data[2..PAYLOAD_COUNT-1] = 实际数据
```

#### 流程图
```
主机0              主机1
  │                 │
  ├─ 发送数据 ──────┤
  │                 │
  ▼                 ▼
┌─────────────────────┐
│    叶交换机 (S0)     │
│   aggregate()       │
└──────────┬──────────┘
           │ 聚合结果
           ▼
┌─────────────────────┐
│    根交换机 (Root)   │
│   aggregate()       │
│   查找target_conn_id │
└──────────┬──────────┘
           │ 单播到root_rank
           ▼
        主机0
      (收到结果)

        主机1
      (不接收数据)
```

#### 关键代码路径
```
1. 上行聚合:
   packet_handler() → aggregate() → (聚合完成)

2. 根节点单播:
   → 查找rank_to_conn[root_rank]
   → 构造unicast_rule
   → forwarding(PACKET_TYPE_DATA, 单播)
   → 清理缓冲区(不进入广播流程)
```

### 3. rank_to_conn映射
```
用途: 将rank编号映射到连接ID

来源: 从topology.yaml解析
  connections:
    - host_id: 0  → rank_to_conn[0] = 0
    - host_id: 1  → rank_to_conn[1] = 1

使用:
  target_conn_id = rank_to_conn[root_rank]
  target_conn = &conns[target_conn_id]
```

---

## 线程模型

### 线程概览
```
主线程 (main)
  │
  ├─> 控制器线程 (controller_thread)
  │   └─> 获取配置后退出
  │
  ├─> 接收线程组 (background_receiving × FAN_IN+1)
  │   ├─> receivers[0]: 处理conns[0]的数据包
  │   ├─> receivers[1]: 处理conns[1]的数据包
  │   ├─> ...
  │   └─> receivers[FAN_IN]: 处理conns[FAN_IN]的数据包(根节点无)
  │
  ├─> 轮询线程 (polling_thread, 可选)
  │   └─> 每10ms检查超时并重传
  │
  └─> 线程池 (thpool × FAN_IN)
      └─> 处理多播发送任务
```

### 1. 控制器线程 (controller_thread)
```
生命周期: 启动 → 获取配置 → 退出

主函数等待:
  pthread_join(tid_controller, NULL)
  └─> 确保配置加载完成后才继续
```

### 2. 接收线程 (background_receiving)
```
参数: conn_id (0 ~ FAN_IN)

主循环:
  while (1) {
      pcap_dispatch(conns[conn_id].handle, -1, packet_handler, conn_id)
      └─> 阻塞等待数据包
      └─> 捕获到数据包后调用packet_handler
  }

pcap配置:
  ├─> set_promisc(1): 混杂模式
  ├─> set_timeout(1ms): 1ms超时
  └─> set_immediate_mode(1): 立即模式
```

### 3. 数据包处理回调 (packet_handler)
```
调用栈:
  pcap_dispatch() → packet_handler() → aggregate()/cache()

线程安全:
  ├─> 使用互斥锁保护共享数据
  │   ├─> agg_mutex: 保护agg_buffer和agg_degree
  │   └─> r_mutex: 保护r_arrival_state
  │
  └─> 每个接收线程处理独立的连接
      └─> arrival_state[id]不需要锁(id是线程独占的)
```

### 4. 线程池 (thpool)
```
初始化:
  thpool = thpool_init(FAN_IN)
  └─> 创建FAN_IN个工作线程

工作模式:
  1. 主线程提交任务
     └─> thpool_add_work(thpool, send_packet_thread, &task)

  2. 工作线程执行任务
     └─> send_packet_thread(task)
         └─> build_eth_packet() + pcap_sendpacket()

优势:
  └─> 多播并行发送,减少延迟
```

### 5. 轮询线程 (polling_thread, 默认禁用)
```
主循环:
  while (1) {
      for (i = 0; i < FAN_IN; i++) {
          检查ts_buffer[i].ts
          if (超时) {
              重传未确认的数据包
          }
      }
      usleep(10000)  // 10ms
  }

启用方式:
  取消注释 main() 中的 pthread_create(&polling, ...)
```

### 线程同步

#### 互斥锁
```c
pthread_mutex_t agg_mutex;     // 保护聚合缓冲区
pthread_mutex_t r_mutex;       // 保护下行ACK状态

使用示例:
  pthread_mutex_lock(&agg_mutex);
  agg_degree[Idx(psn)]++;
  pthread_mutex_unlock(&agg_mutex);
```

#### 无锁设计
```
arrival_state[id][]:
  └─> 每个接收线程只写入自己的id行,无需加锁

agg_epsn[id]:
  └─> 每个接收线程只访问自己的id索引,无需加锁
```

---

## 性能优化要点

### 1. 零拷贝优化
- pcap直接捕获到内核缓冲区
- 使用指针操作避免内存复制
- 网络字节序转换最小化

### 2. 并行处理
- 每个连接独立的接收线程
- 线程池并行发送多播数据
- 减少锁竞争(细粒度锁设计)

### 3. 循环缓冲区
- PSN % N映射,支持滑动窗口
- 固定内存占用(N=300)
- 支持乱序接收

### 4. 快速路径优化
- 期望PSN的快速检查
- ACK立即发送(无等待)
- 聚合完成后立即触发下一步

---

## 调试与日志

### 日志系统
```c
log_init(NULL)                    // 初始化日志
log_write(id, "message", ...)     // 写入日志
LOG_FUNC_ENTRY(id)                // 函数入口日志
LOG_FUNC_EXIT(id)                 // 函数出口日志
```

### 关键调试点
```
1. 路由表打印
   └─> build_rules()结束后打印所有规则

2. PSN跟踪
   └─> packet_handler()打印每个收到的PSN

3. 聚合状态
   └─> aggregate()打印agg_degree和operation_type

4. 广播状态
   └─> down_ack_handler()打印ACK到达情况
```

---

## 常见问题排查

### Q1: 聚合卡住不完成
```
检查点:
  ├─> agg_degree[Idx(psn)]是否达到FAN_IN?
  ├─> arrival_state[i][Idx(psn)]是否都为1?
  └─> 是否有子节点未发送数据?

调试:
  在aggregate()中打印agg_degree值
```

### Q2: 广播未到达主机
```
检查点:
  ├─> 路由表down规则是否正确?
  ├─> out_conns_cnt是否正确?
  └─> ACK是否都收到?

调试:
  在forwarding()中打印发送目标
```

### Q3: PSN乱序导致NAK风暴
```
原因:
  └─> 网络延迟或丢包导致PSN超前

解决:
  ├─> 增大窗口大小(N)
  ├─> 启用轮询线程重传
  └─> 优化网络配置
```

### Q4: 内存占用过高
```
检查点:
  └─> 缓冲区是否及时清理?
      ├─> agg_buffer[].state是否正确重置?
      └─> bcast_buffer[].state是否正确重置?

调试:
  定期打印缓冲区使用情况
```

---

## 总结

INC交换机通过以下机制实现高效的在网络聚合:

1. **多线程并发**: 每个连接独立接收,线程池并行发送
2. **滑动窗口流控**: 基于PSN的循环缓冲区,支持乱序和重传
3. **可靠传输**: ACK/NAK机制保证数据完整性
4. **灵活路由**: 基于IP五元组的规则匹配
5. **操作灵活性**: 支持AllReduce和Reduce两种集合通信模式

整个系统设计充分利用了RDMA的低延迟特性和交换机的可编程能力,为分布式机器学习和高性能计算提供了高效的通信原语。
