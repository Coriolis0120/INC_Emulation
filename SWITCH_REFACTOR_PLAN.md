# Switch.c 重构计划 (C语言版)

## 重构目标

1. **提高可维护性**: 将2000+行的单文件拆分为模块化组件
2. **改进可扩展性**: 支持动态拓扑重配置和多种集合通信原语
3. **增强线程安全**: 细化锁粒度，减少竞争
4. **保持兼容性**: 渐进式重构，每步都可编译运行

## 当前问题总结

### 1. 架构问题
- ❌ 全局变量过多 (20+ 个全局数组/变量)
- ❌ 硬编码配置 (FAN_IN, N, rank_to_conn)
- ❌ 路由表缺少 operation_type 维度
- ❌ 单一通信组，无法并发

### 2. 代码质量问题
- ❌ `packet_handler()` 函数过长 (200+ 行)
- ❌ 职责混乱 (解析、路由、聚合、流控混在一起)
- ❌ 重复代码 (ACK/NAK 处理逻辑重复)
- ❌ 注释与代码不一致

### 3. 并发问题
- ❌ 粗粒度锁 (只有 agg_mutex, r_mutex)
- ❌ current_operation_type 等全局变量缺少保护
- ❌ 重传逻辑分散，难以调试

## 重构策略: 分层 + 模块化

```
┌────────────────────────────────────────────────┐
│  switch_main.c - 主入口和初始化                │
└────────────────────────────────────────────────┘
                     ↓
┌────────────────────────────────────────────────┐
│  topology_manager.c - 拓扑配置和动态重载       │
│  - parse_topology()                            │
│  - reload_topology()                           │
│  - build_routing_table()                       │
└────────────────────────────────────────────────┘
                     ↓
┌────────────────────────────────────────────────┐
│  packet_dispatcher.c - 数据包分发               │
│  - dispatch_packet()                           │
│  - parse_packet()                              │
└────────────────────────────────────────────────┘
                     ↓
┌─────────────────────┬──────────────────────────┐
│  uplink_handler.c   │  downlink_handler.c      │
│  - 处理上行数据      │  - 处理下行广播          │
│  - 聚合逻辑         │  - 缓存和转发            │
└─────────────────────┴──────────────────────────┘
                     ↓
┌────────────────────────────────────────────────┐
│  flow_control.c - 流控和重传                   │
│  - handle_ack()                                │
│  - handle_nak()                                │
│  - retransmit()                                │
└────────────────────────────────────────────────┘
                     ↓
┌────────────────────────────────────────────────┐
│  connection_manager.c - 连接和发送             │
│  - send_packet()                               │
│  - multicast_packet()                          │
└────────────────────────────────────────────────┘
```

## 重构步骤 (渐进式)

### Phase 1: 封装全局状态 (1-2天)

**目标**: 将散乱的全局变量封装到结构体中

#### 1.1 创建核心数据结构

```c
// switch_context.h
typedef struct {
    // PSN 状态 (每个 PSN 独立)
    int degree;                  // 聚合度
    int arrival[FAN_IN];         // 上行到达状态
    int r_arrival[FAN_IN];       // 下行ACK到达状态
    agg_buffer_t buffer;         // 聚合缓冲区
    agg_buffer_t bcast_buffer;   // 广播缓冲区
    pthread_mutex_t mutex;       // 细粒度锁
} psn_state_t;

typedef struct {
    // PSN 管理
    psn_state_t psn_states[N];   // 循环缓冲区
    int epsn[FAN_IN];            // 上行期望PSN
    int down_epsn;               // 下行期望PSN
    int latest_ack[FAN_IN];      // 最后确认PSN
    int down_ack;                // 下行最后确认PSN

    // 连接管理
    connection_t conns[FAN_IN + 1];
    int fan_in;                  // 动态扇入
    int is_root;                 // 是否根节点

    // 路由表
    rule_table_t routing_table;
    int rank_to_conn[MAX_RANKS]; // rank到连接映射

    // 元数据 (从PSN=0提取)
    primitive_type_t operation_type;
    int root_rank;
    pthread_mutex_t meta_mutex;  // 保护元数据

    // 线程池
    threadpool thpool;

} switch_context_t;
```

**改动文件**:
- 新建 `repository/include/switch_context.h`
- 修改 `repository/src/switch.c`: 将全局变量迁移到 `switch_context_t`

**验证**: 编译通过，功能测试通过

---

### Phase 2: 拆分 packet_handler (2-3天)

**目标**: 将200+行的 `packet_handler` 拆分为可维护的小函数

#### 2.1 创建数据包处理模块

```c
// packet_processor.h
typedef enum {
    PKT_UPLINK_DATA,
    PKT_DOWNLINK_DATA,
    PKT_UPLINK_ACK,
    PKT_DOWNLINK_ACK,
    PKT_UNKNOWN
} packet_category_t;

typedef struct {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t psn;
    uint8_t opcode;
    uint32_t *payload;
    int payload_len;
    rule_t *rule;
} parsed_packet_t;

// 解析数据包
packet_category_t parse_and_classify_packet(
    const uint8_t *raw_packet,
    parsed_packet_t *out,
    switch_context_t *ctx
);

// 分发到不同的处理器
void dispatch_packet(
    packet_category_t category,
    parsed_packet_t *pkt,
    switch_context_t *ctx
);
```

#### 2.2 实现独立的处理器

```c
// uplink_processor.c
void process_uplink_data(
    parsed_packet_t *pkt,
    switch_context_t *ctx
);

void process_uplink_ack(
    parsed_packet_t *pkt,
    switch_context_t *ctx
);

// downlink_processor.c
void process_downlink_data(
    parsed_packet_t *pkt,
    switch_context_t *ctx
);

void process_downlink_ack(
    parsed_packet_t *pkt,
    switch_context_t *ctx
);
```

#### 2.3 重构后的 packet_handler

```c
void packet_handler(uint8_t *user_data,
                   const struct pcap_pkthdr *pkthdr,
                   const uint8_t *packet) {
    switch_context_t *ctx = (switch_context_t *)user_data;

    // 1. 解析和分类
    parsed_packet_t pkt;
    packet_category_t category = parse_and_classify_packet(packet, &pkt, ctx);

    // 2. 分发处理
    dispatch_packet(category, &pkt, ctx);
}
```

**改动文件**:
- 新建 `repository/src/packet_processor.c`
- 新建 `repository/src/uplink_processor.c`
- 新建 `repository/src/downlink_processor.c`
- 新建 `repository/include/packet_processor.h`
- 修改 `repository/src/switch.c`: 重构 `packet_handler`

**验证**: 单元测试每个处理器，集成测试完整流程

---

### Phase 3: 细化锁粒度 (1天)

**目标**: 从全局锁改为每个PSN独立锁，减少竞争

#### 3.1 修改聚合逻辑

**之前**:
```c
pthread_mutex_lock(&agg_mutex);  // 全局锁
// 修改 agg_buffer[Idx(psn)]
agg_degree[Idx(psn)]++;
pthread_mutex_unlock(&agg_mutex);
```

**之后**:
```c
psn_state_t *state = &ctx->psn_states[Idx(psn)];
pthread_mutex_lock(&state->mutex);  // PSN独立锁
// 修改 state->buffer
state->degree++;
pthread_mutex_unlock(&state->mutex);
```

**改动文件**:
- 修改 `repository/src/uplink_processor.c`: aggregate函数
- 修改 `repository/src/downlink_processor.c`: cache函数

**验证**: 性能测试，确保锁竞争减少

---

### Phase 4: 支持动态拓扑重配置 (2-3天)

**目标**: 实现拓扑热重载，无需重启交换机

#### 4.1 创建拓扑管理模块

```c
// topology_manager.h
typedef struct {
    int switch_id;
    int is_root;
    int fan_in;
    connection_config_t connections[MAX_CONNECTIONS];
    routing_rule_t rules[MAX_RULES];
} topology_config_t;

// 解析YAML拓扑
int parse_topology_file(
    const char *yaml_path,
    int switch_id,
    topology_config_t *out
);

// 应用新拓扑 (原子操作)
int apply_topology(
    switch_context_t *ctx,
    topology_config_t *new_config
);

// 等待所有PSN完成 (优雅切换)
void wait_for_idle(switch_context_t *ctx);
```

#### 4.2 实现热重载流程

```c
// 控制器线程收到新拓扑时
void reload_topology_handler(switch_context_t *ctx, const char *yaml_path) {
    topology_config_t new_config;

    // 1. 解析新拓扑
    if (parse_topology_file(yaml_path, ctx->switch_id, &new_config) < 0) {
        return;
    }

    // 2. 等待当前所有任务完成
    wait_for_idle(ctx);

    // 3. 原子替换拓扑
    apply_topology(ctx, &new_config);

    // 4. 发送ACK给控制器
    send_reload_ack(controller_fd);
}
```

**改动文件**:
- 新建 `repository/src/topology_manager.c`
- 新建 `repository/include/topology_manager.h`
- 修改 `repository/src/switch.c`: 在 controller_thread 中监听重载请求

**验证**:
- 测试在运行中切换 AllReduce/Reduce 模式
- 测试改变 FAN_IN 配置

---

### Phase 5: 改进路由表 (1-2天)

**目标**: 路由键包含 operation_type，支持同一时间不同操作

#### 5.1 扩展路由键

**之前**:
```c
typedef struct {
    uint32_t src_ip;
    uint32_t dst_ip;
} routing_key_t;
```

**之后**:
```c
typedef struct {
    uint32_t src_ip;
    uint32_t dst_ip;
    primitive_type_t op_type;  // 新增
} routing_key_t;

// 哈希函数
uint32_t routing_key_hash(routing_key_t *key) {
    return key->src_ip ^ key->dst_ip ^ (key->op_type << 24);
}
```

#### 5.2 修改路由查找

```c
rule_t* lookup_rule_ex(
    rule_table_t *table,
    uint32_t src_ip,
    uint32_t dst_ip,
    primitive_type_t op_type
) {
    for (int i = 0; i < table->count; i++) {
        if (table->rules[i].src_ip == src_ip &&
            table->rules[i].dst_ip == dst_ip &&
            table->rules[i].op_type == op_type) {
            return &table->rules[i];
        }
    }
    return NULL;
}
```

**改动文件**:
- 修改 `repository/include/rule.h`: 添加 op_type 字段
- 修改 `repository/src/rule.c`: 更新查找逻辑
- 修改 `repository/src/packet_processor.c`: 使用新的查找接口

---

### Phase 6: 添加日志和监控 (1天)

**目标**: 统一日志格式，添加性能指标

#### 6.1 统一日志宏

```c
// logger.h
#define LOG_DEBUG(ctx, fmt, ...) \
    log_write_ex(ctx->switch_id, LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_INFO(ctx, fmt, ...) \
    log_write_ex(ctx->switch_id, LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_ERROR(ctx, fmt, ...) \
    log_write_ex(ctx->switch_id, LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
```

#### 6.2 添加性能指标

```c
typedef struct {
    uint64_t packets_received;
    uint64_t packets_sent;
    uint64_t aggregations_completed;
    uint64_t retransmissions;
    uint64_t lock_contentions;
} switch_metrics_t;

// 定期打印
void print_metrics(switch_metrics_t *metrics);
```

---

## 文件结构 (重构后)

```
repository/
├── include/
│   ├── switch_context.h       # 核心上下文结构
│   ├── packet_processor.h     # 数据包处理接口
│   ├── uplink_processor.h     # 上行处理接口
│   ├── downlink_processor.h   # 下行处理接口
│   ├── topology_manager.h     # 拓扑管理接口
│   ├── flow_control.h         # 流控接口
│   └── logger.h               # 日志接口
│
├── src/
│   ├── switch_main.c          # 主入口 (main函数)
│   ├── switch_context.c       # 上下文初始化/销毁
│   ├── packet_processor.c     # 数据包解析和分发
│   ├── uplink_processor.c     # 上行数据/ACK处理
│   ├── downlink_processor.c   # 下行数据/ACK处理
│   ├── topology_manager.c     # 拓扑解析和重载
│   ├── flow_control.c         # ACK/NAK/重传逻辑
│   ├── connection_manager.c   # pcap连接管理
│   └── logger.c               # 日志实现
```

## 工作量评估

| 阶段 | 工作量 | 风险 | 优先级 |
|------|--------|------|--------|
| Phase 1: 封装状态 | 1-2天 | 低 | 高 |
| Phase 2: 拆分handler | 2-3天 | 中 | 高 |
| Phase 3: 细化锁 | 1天 | 低 | 中 |
| Phase 4: 动态拓扑 | 2-3天 | 高 | 高 |
| Phase 5: 改进路由 | 1-2天 | 中 | 中 |
| Phase 6: 日志监控 | 1天 | 低 | 低 |
| **总计** | **8-12天** | - | - |

## 第一步行动: Phase 1 实现

我建议先做 Phase 1，这是最安全且影响最大的改动。

**具体任务**:
1. 创建 `switch_context.h` 和 `switch_context.c`
2. 定义 `switch_context_t` 结构体
3. 修改 `switch.c`，将全局变量改为 `ctx->xxx`
4. 更新函数签名，传递 `switch_context_t *ctx` 参数
5. 编译、测试、验证

**你想先看 Phase 1 的详细代码实现吗？我可以直接写出来供你审阅。**
