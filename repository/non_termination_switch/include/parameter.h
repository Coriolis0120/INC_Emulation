#define MASTER_PORT 52223 // May 22nd 23:00
#define CONTROLLER_GROUP_PORT 52200
#define CONTROLLER_SWITCH_PORT 52311
#define TOPOLOGY_SIZE 3

// RDMA 端口
#define RDMA_HOST_PORT 4791      // Host-Switch 通信端口
#define RDMA_SWITCH_PORT 4792    // Switch-Switch 通信端口

// SWITCH 相关
#define MAX_CONNECTIONS_NUM 10
#define WINDOW_SIZE (131072) // window size - 支持128MB测试 (131072 PSN)
#define SWITCH_ARRAY_LENGTH (WINDOW_SIZE * 2)  // 支持256MB测试 
#define MAX_PORT_NUM 10
#define MAX_EVENTS (MAX_PORT_NUM*4)


// 拓扑相关
#define MAX_RANKS 128
#define MAX_WORLD_SIZE 128    // 最大 world_size (用于规则表)

// Rule 相关
#define MAX_RULES 100
