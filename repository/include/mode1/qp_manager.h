#ifndef MODE1_QP_MANAGER_H
#define MODE1_QP_MANAGER_H

#include <stdint.h>
#include <infiniband/verbs.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

// 最大连接数
#define MAX_HOSTS 64
#define MAX_SWITCHES 8

// 接收缓冲区配置
// 每个连接需要 RECV_BUF_PER_CONN 个槽位（滑动窗口）
// 支持最多 4 个连接（2 hosts per LEAF, 或 2 children per ROOT）
#define RECV_BUF_PER_CONN 16384
#define RECV_BUF_MAX_CONNS 4
#define RECV_BUF_COUNT (RECV_BUF_PER_CONN * RECV_BUF_MAX_CONNS)  // 65536 slots
#define RECV_BUF_SIZE  (64 * 1024)  // 64KB per message

// QP 连接信息（用于 TCP 交换）
typedef struct {
    uint32_t qpn;           // Queue Pair Number
    uint32_t psn;           // Packet Sequence Number
    union ibv_gid gid;      // GID address
    uint16_t lid;           // Local ID (for IB)
} qp_info_t;

// 单个 QP 连接上下文
typedef struct {
    int id;                     // 连接 ID
    struct ibv_qp *qp;          // Queue Pair
    qp_info_t local_info;       // 本地 QP 信息
    qp_info_t remote_info;      // 远端 QP 信息
    int tcp_fd;                 // TCP 辅助通道
    int is_connected;           // 连接状态
} qp_conn_t;

// 上行连接上下文（用于 Leaf -> Spine）
typedef struct {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *send_cq;
    struct ibv_cq *recv_cq;
    struct ibv_mr *mr;
    struct ibv_mr *send_mr;
    void *recv_buf;
    void *send_buf;
    uint32_t buf_size;
    union ibv_gid local_gid;
    int gid_idx;
    int ib_port;
    qp_conn_t *conn;
} uplink_ctx_t;

// 下行连接上下文（用于 Spine -> Leaf，支持多设备）
typedef struct {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *send_cq;
    struct ibv_cq *recv_cq;
    struct ibv_mr *mr;
    struct ibv_mr *send_mr;
    void *recv_buf;
    void *send_buf;
    size_t buf_size;  // 使用 size_t 避免溢出
    union ibv_gid local_gid;
    int gid_idx;
    int ib_port;
    qp_conn_t *conns[MAX_SWITCHES];  // 该设备上的子连接
    int conn_count;
} downlink_ctx_t;

// Switch RoCE 上下文
typedef struct {
    // RDMA 设备资源
    struct ibv_context *ctx;    // 设备句柄
    struct ibv_pd *pd;          // Protection Domain
    struct ibv_cq *send_cq;     // 发送完成队列
    struct ibv_cq *recv_cq;     // 接收完成队列
    struct ibv_mr *mr;          // Memory Region

    // 接收缓冲区
    void *recv_buf;
    size_t recv_buf_size;  // 使用 size_t 避免溢出

    // 发送缓冲区
    void *send_buf;
    size_t send_buf_size;  // 使用 size_t 避免溢出
    struct ibv_mr *send_mr;

    // 下行连接：到 Host
    qp_conn_t *host_conns[MAX_HOSTS];
    int host_count;

    // 上行连接：到父 Switch
    qp_conn_t *parent_conn;
    uplink_ctx_t *uplink;  // 上行连接独立上下文（使用不同 RDMA 设备）

    // 下行连接：到子 Switch（对于非叶子节点）
    qp_conn_t *child_conns[MAX_SWITCHES];
    int child_count;

    // 多下行设备支持（Spine 用于连接不同网段的 Leaf）
    downlink_ctx_t *downlinks[MAX_SWITCHES];
    int downlink_count;

    // GID 信息
    union ibv_gid local_gid;
    int gid_idx;
    int ib_port;

    // 同步
    pthread_mutex_t lock;
} switch_roce_ctx_t;

// ============ 初始化与销毁 ============

/**
 * @brief 初始化 Switch RoCE 上下文
 * @param dev_name RDMA 设备名称（如 "rxe_eth1"），NULL 使用默认设备
 * @param gid_idx GID 索引
 * @return 成功返回上下文指针，失败返回 NULL
 */
switch_roce_ctx_t *qp_manager_init(const char *dev_name, int gid_idx);

/**
 * @brief 销毁 Switch RoCE 上下文
 */
void qp_manager_destroy(switch_roce_ctx_t *ctx);

/**
 * @brief 初始化上行连接上下文（使用独立 RDMA 设备）
 */
uplink_ctx_t *uplink_ctx_init(const char *dev_name, int gid_idx);

/**
 * @brief 销毁上行连接上下文
 */
void uplink_ctx_destroy(uplink_ctx_t *uplink);

/**
 * @brief 在上行连接上下文中创建 QP
 */
qp_conn_t *uplink_conn_create(uplink_ctx_t *uplink);

/**
 * @brief 上行连接 QP 交换
 */
int uplink_conn_exchange(uplink_ctx_t *uplink, int tcp_fd);

/**
 * @brief 上行连接发送
 */
int uplink_post_send(uplink_ctx_t *uplink, uint64_t wr_id,
                     void *buf, uint32_t len, uint32_t imm_data);

/**
 * @brief 上行连接接收
 */
int uplink_post_recv(uplink_ctx_t *uplink, uint64_t wr_id,
                     void *buf, uint32_t len);

/**
 * @brief 轮询上行连接接收 CQ
 */
int uplink_poll_recv_cq(uplink_ctx_t *uplink, struct ibv_wc *wc, int max_wc);

// ============ 下行连接（Spine -> Leaf）============

/**
 * @brief 初始化下行连接上下文（使用独立 RDMA 设备）
 */
downlink_ctx_t *downlink_ctx_init(const char *dev_name, int gid_idx);

/**
 * @brief 销毁下行连接上下文
 */
void downlink_ctx_destroy(downlink_ctx_t *downlink);

/**
 * @brief 在下行连接上下文中创建 QP
 */
qp_conn_t *downlink_conn_create(downlink_ctx_t *downlink, int conn_id);

/**
 * @brief 下行连接 QP 交换（服务端）
 */
int downlink_conn_exchange(downlink_ctx_t *downlink, qp_conn_t *conn, int tcp_fd);

/**
 * @brief 下行连接发送
 */
int downlink_post_send(downlink_ctx_t *downlink, qp_conn_t *conn,
                       uint64_t wr_id, void *buf, uint32_t len, uint32_t imm_data);

/**
 * @brief 下行连接接收
 */
int downlink_post_recv(downlink_ctx_t *downlink, qp_conn_t *conn,
                       uint64_t wr_id, void *buf, uint32_t len);

/**
 * @brief 轮询下行连接接收 CQ
 */
int downlink_poll_recv_cq(downlink_ctx_t *downlink, struct ibv_wc *wc, int max_wc);

// ============ 连接管理 ============

/**
 * @brief 创建新的 QP 连接
 * @param ctx Switch 上下文
 * @param conn_id 连接 ID
 * @return 成功返回连接指针，失败返回 NULL
 */
qp_conn_t *qp_conn_create(switch_roce_ctx_t *ctx, int conn_id);

/**
 * @brief 销毁 QP 连接
 */
void qp_conn_destroy(qp_conn_t *conn);

/**
 * @brief 通过 TCP 交换 QP 信息并建立连接
 * @param ctx Switch 上下文
 * @param conn QP 连接
 * @param tcp_fd TCP socket
 * @param is_server 是否作为服务端（先发送后接收）
 * @return 成功返回 0
 */
int qp_conn_exchange(switch_roce_ctx_t *ctx, qp_conn_t *conn,
                     int tcp_fd, int is_server);

/**
 * @brief 将 QP 状态转换到 RTR (Ready to Receive)
 */
int qp_modify_to_rtr(switch_roce_ctx_t *ctx, qp_conn_t *conn);

/**
 * @brief 将 QP 状态转换到 RTS (Ready to Send)
 */
int qp_modify_to_rts(qp_conn_t *conn);

// ============ 消息收发 ============

/**
 * @brief 投递接收请求
 * @param ctx Switch 上下文
 * @param conn QP 连接
 * @param wr_id Work Request ID
 * @param buf 接收缓冲区
 * @param len 缓冲区长度
 * @return 成功返回 0
 */
int qp_post_recv(switch_roce_ctx_t *ctx, qp_conn_t *conn,
                 uint64_t wr_id, void *buf, uint32_t len);

/**
 * @brief 发送消息（带 Immediate Data）
 * @param conn QP 连接
 * @param wr_id Work Request ID
 * @param buf 发送数据
 * @param len 数据长度
 * @param imm_data Immediate Data
 * @return 成功返回 0
 */
int qp_post_send(switch_roce_ctx_t *ctx, qp_conn_t *conn,
                 uint64_t wr_id, void *buf, uint32_t len, uint32_t imm_data);

/**
 * @brief 轮询接收完成队列
 * @param ctx Switch 上下文
 * @param wc 完成条目数组
 * @param max_wc 最大条目数
 * @return 返回完成的条目数，错误返回 -1
 */
int qp_poll_recv_cq(switch_roce_ctx_t *ctx, struct ibv_wc *wc, int max_wc);

/**
 * @brief 轮询发送完成队列
 */
int qp_poll_send_cq(switch_roce_ctx_t *ctx, struct ibv_wc *wc, int max_wc);

// ============ 辅助函数 ============

/**
 * @brief 获取接收缓冲区地址
 * @param ctx Switch 上下文
 * @param wr_id Work Request ID
 * @return 缓冲区地址
 */
void *qp_get_recv_buf(switch_roce_ctx_t *ctx, uint64_t wr_id);

/**
 * @brief 打印 QP 连接信息
 */
void qp_conn_print(qp_conn_t *conn);

#ifdef __cplusplus
}
#endif

#endif // MODE1_QP_MANAGER_H
