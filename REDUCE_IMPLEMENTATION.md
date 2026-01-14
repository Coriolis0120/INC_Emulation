# Reduce原语实现文档

## 概述

本文档描述了在INC_Emulation项目中实现的Reduce集合通信原语。Reduce操作与AllReduce的主要区别在于：
- **AllReduce**: 所有节点发送数据 → 交换机聚合 → **广播结果到所有节点**
- **Reduce**: 所有节点发送数据 → 交换机聚合 → **仅发送结果到指定的根节点**

## 实现架构

### 1. 交换机层面的修改 (switch.c)

#### 1.1 新增数据结构

在 `agg_buffer_t` 结构中添加了两个新字段：

```c
typedef struct {
    int len;                    // 数据长度
    int packet_type;            // RDMA操作类型
    int state;                  // 缓冲区状态
    int psn;                    // 包序号
    int operation_type;         // 集合通信操作类型（新增）
    int root_rank;              // Reduce操作的根节点rank（新增）
    uint32_t buffer[1036];      // 数据缓冲区
} agg_buffer_t;
```

#### 1.2 操作类型定义

```c
#define OPERATION_ALLREDUCE 0  // AllReduce操作
#define OPERATION_REDUCE 1     // Reduce操作
```

#### 1.3 Rank到连接的映射

新增全局变量用于Reduce操作时查找目标rank的连接：

```c
int rank_to_conn[FAN_IN];  // rank到连接ID的映射
```

在 `init_all()` 函数中初始化：

```c
for(int i = 0; i < FAN_IN; i++) {
    rank_to_conn[i] = i;  // 默认映射：rank i 对应连接 i
}
```

#### 1.4 聚合函数修改

`aggregate()` 函数签名更新，新增两个参数：

```c
int aggregate(rule_t* rule, uint32_t psn, uint32_t *data, int len,
              int packet_type, int operation_type, int root_rank);
```

**核心逻辑变化**：

在根交换机完成聚合后，根据 `operation_type` 决定下一步操作：

```c
if(operation_type == OPERATION_ALLREDUCE) {
    // AllReduce：广播到所有子节点
    cache(rule, psn, agg_buffer[Idx(psn)].buffer, len, packet_type, false);
    down_ack_handler(&aeth, NULL, psn);
} else if(operation_type == OPERATION_REDUCE) {
    // Reduce：仅发送到指定的根rank节点
    int target_conn_id = -1;
    for(int i = 0; i < FAN_IN; i++) {
        if(rank_to_conn[i] == root_rank) {
            target_conn_id = i;
            break;
        }
    }

    if(target_conn_id >= 0) {
        // 单播发送到目标rank
        connection_t* target_conn = &conns[target_conn_id];
        // ... 构建并发送数据包 ...
    }

    // 清理聚合缓冲区
    pthread_mutex_lock(&agg_mutex);
    agg_degree[Idx(psn)] = 0;
    memset(&agg_buffer[Idx(psn)], 0, sizeof(agg_buffer_t));
    // ...
    pthread_mutex_unlock(&agg_mutex);
}
```

#### 1.5 数据包处理函数修改

在 `packet_handler()` 函数中，从上行数据包提取操作类型和根rank信息：

```c
// 从数据包中提取元数据
// data[0] = operation_type (OPERATION_ALLREDUCE 或 OPERATION_REDUCE)
// data[1] = root_rank (仅用于Reduce操作)
int operation_type = OPERATION_ALLREDUCE;  // 默认为AllReduce
int root_rank = 0;  // 默认根节点为rank 0

if(data_len >= 2) {
    operation_type = ntohl(data[0]);
    root_rank = ntohl(data[1]);
    log_write(id, "Operation type: %d, Root rank: %d\n", operation_type, root_rank);
}

// 调用聚合函数
aggregate(rule, psn, data, data_len, bth->opcode, operation_type, root_rank);
```

### 2. API层面的修改

#### 2.1 API头文件 (api.h)

新增两个Reduce函数声明：

```c
// Reduce operations: only root rank receives the aggregated result
void inccl_reduce_sendrecv(struct inccl_communicator *comm, int32_t* src_data, 
                           uint32_t len, int32_t* dst_data, int root_rank);

void inccl_reduce_write(struct inccl_communicator *comm, int32_t* src_data, 
                        uint32_t len, int32_t* dst_data, int root_rank);
```

#### 2.2 API实现 (api.c)

实现了两个Reduce函数：

**`inccl_reduce_sendrecv()`** - 使用SEND/RECV语义

关键特性：
- 所有节点都发送数据到交换机
- 只有root_rank节点预先提交接收请求
- 非root节点只等待发送完成，不接收数据
- 数据包前两个uint32_t存储元数据（operation_type和root_rank）

**`inccl_reduce_write()`** - 使用RDMA WRITE语义

关键特性：
- 使用RDMA WRITE操作，性能更高
- 其他逻辑与sendrecv版本相同


## 数据流程

### Reduce操作的完整流程

```
1. 初始化阶段：
   - 所有节点调用 inccl_reduce_sendrecv() 或 inccl_reduce_write()
   - Root节点预先提交接收请求
   - 非root节点不提交接收请求

2. 发送阶段：
   - 所有节点准备发送缓冲区：
     * data[0] = OPERATION_REDUCE (1)
     * data[1] = root_rank
     * data[2..N] = 实际数据
   - 所有节点通过RDMA发送数据到交换机

3. 交换机聚合阶段：
   - 交换机接收所有上行数据
   - 提取operation_type和root_rank
   - 执行聚合操作（求和）
   - 根交换机检测到operation_type == OPERATION_REDUCE
   - 查找root_rank对应的连接ID
   - 仅向root_rank节点发送聚合结果（单播）

4. 接收阶段：
   - Root节点接收聚合结果并存储到dst_data
   - 非root节点不接收任何数据
   - 所有节点等待发送完成

5. 完成：
   - Root节点的dst_data包含全局聚合结果
   - 非root节点的dst_data未被修改
```


## 使用示例

### 基本用法

```c
#include "api.h"

int main() {
    int world_size = 4;
    int rank = 0;  // 当前节点的rank
    int root_rank = 0;  // 指定rank 0为根节点
    const char *controller_ip = "192.168.0.3";
    
    // 1. 创建通信组
    struct inccl_group *group = inccl_group_create(world_size, rank, controller_ip);
    
    // 2. 创建通信器
    struct inccl_communicator *comm = inccl_communicator_create(group, 4096);
    
    // 3. 准备数据
    int32_t src_data[1024];
    int32_t dst_data[1024];
    
    for(int i = 0; i < 1024; i++) {
        src_data[i] = rank * 1000 + i;  // 每个rank有不同的数据
    }
    
    // 4. 执行Reduce操作（SEND/RECV模式）
    inccl_reduce_sendrecv(comm, src_data, 1024, dst_data, root_rank);
    
    // 或者使用RDMA WRITE模式（性能更高）
    // inccl_reduce_write(comm, src_data, 1024, dst_data, root_rank);
    
    // 5. 检查结果
    if(rank == root_rank) {
        printf("Root rank %d received aggregated result:\n", rank);
        for(int i = 0; i < 10; i++) {
            printf("dst_data[%d] = %d\n", i, dst_data[i]);
        }
    } else {
        printf("Non-root rank %d completed sending\n", rank);
    }
    
    // 6. 清理
    inccl_communicator_destroy(comm);
    inccl_group_destroy(group);
    
    return 0;
}
```


## 关键设计决策

### 1. 元数据传输方式

选择在数据包的前两个uint32_t存储元数据（operation_type和root_rank）：
- **优点**：实现简单，不需要修改底层协议
- **缺点**：占用2个元素的payload空间
- **替代方案**：可以在协议层面添加专门的头部字段

### 2. Root节点查找机制

使用 `rank_to_conn[]` 数组映射rank到连接ID：
- 默认映射：`rank_to_conn[i] = i`
- 可以根据实际拓扑结构调整映射关系
- 支持灵活的rank分配策略

### 3. 缓冲区管理

Reduce操作完成后立即清理聚合缓冲区：
- AllReduce需要保留缓冲区用于广播和ACK处理
- Reduce只需单播一次，可以立即释放资源
- 简化了流控机制


## 性能考虑

### 1. 网络流量优化

- **AllReduce**: N个节点发送 → 聚合 → N个节点接收（2N次传输）
- **Reduce**: N个节点发送 → 聚合 → 1个节点接收（N+1次传输）
- **节省**: 减少了(N-1)次下行传输

### 2. 内存使用

- Root节点需要接收缓冲区
- 非root节点只需要发送缓冲区
- 相比AllReduce节省了非root节点的接收缓冲区

### 3. 延迟分析

- 上行聚合阶段：与AllReduce相同
- 下行传输阶段：Reduce只需单播，延迟更低


## 测试建议

### 1. 功能测试

```c
// 测试用例1：基本Reduce操作
void test_basic_reduce() {
    // 所有节点发送相同的数据
    // 验证root节点收到的是sum(所有节点的数据)
}

// 测试用例2：不同root_rank
void test_different_root() {
    // 测试root_rank = 0, 1, 2, ...
    // 验证每个rank都能正确接收结果
}

// 测试用例3：大数据量
void test_large_data() {
    // 测试超过窗口大小的数据
    // 验证滑动窗口机制正常工作
}
```

### 2. 性能测试

- 测量Reduce vs AllReduce的延迟差异
- 测量不同数据量下的吞吐量
- 测量不同节点数量下的扩展性


## 注意事项和限制

### 1. 当前实现的限制

- **元数据开销**：每个数据包的前两个uint32_t用于元数据，实际payload减少8字节
- **Rank映射**：当前使用简单的线性映射（rank i → conn i），复杂拓扑可能需要调整
- **错误处理**：简化了ACK/NAK机制，生产环境可能需要更完善的流控

### 2. 与AllReduce的兼容性

- 两种操作可以在同一系统中共存
- 通过operation_type字段区分
- 共享相同的底层基础设施（QP、CQ、MR等）

### 3. 未来改进方向

- **协议优化**：在BTH头部添加专用字段，避免占用payload空间
- **动态映射**：支持从控制器获取rank到连接的映射关系
- **多根节点**：支持同时向多个节点发送结果（介于Reduce和AllReduce之间）
- **流控增强**：为Reduce操作添加完整的ACK/NAK和重传机制


## 文件修改清单

### 修改的文件

1. **repository/src/switch.c**
   - 添加 `OPERATION_ALLREDUCE` 和 `OPERATION_REDUCE` 常量定义
   - 修改 `agg_buffer_t` 结构，添加 `operation_type` 和 `root_rank` 字段
   - 添加 `rank_to_conn[]` 全局变量
   - 修改 `aggregate()` 函数签名和实现
   - 修改 `packet_handler()` 函数，提取元数据并传递给aggregate
   - 修改 `init_all()` 函数，初始化rank_to_conn映射

2. **repository/include/api.h**
   - 添加 `inccl_reduce_sendrecv()` 函数声明
   - 添加 `inccl_reduce_write()` 函数声明

3. **repository/src/api.c**
   - 实现 `inccl_reduce_sendrecv()` 函数
   - 实现 `inccl_reduce_write()` 函数

### 新增的文件

- **REDUCE_IMPLEMENTATION.md** - 本文档


## 总结

本次实现成功为INC_Emulation项目添加了Reduce集合通信原语，主要特点：

✅ **完整的链接终结模式支持**：在switch.c中实现了完整的Reduce逻辑
✅ **双模式API**：提供SEND/RECV和RDMA WRITE两种实现
✅ **高效的单播机制**：根交换机仅向root节点发送结果，减少网络流量
✅ **与AllReduce兼容**：两种操作可以共存，通过元数据区分
✅ **灵活的根节点选择**：支持任意rank作为根节点

## 参考资料

- 原始AllReduce实现：[repository/src/switch.c](repository/src/switch.c)
- INCCL API文档：[CLAUDE.md](CLAUDE.md)
- RRoCE v2协议：[repository/include/util.h](repository/include/util.h)

---

**文档版本**: 1.0  
**创建日期**: 2026-01-14  
**作者**: Claude Code

