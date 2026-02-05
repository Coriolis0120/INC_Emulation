#ifndef MODE1_INC_ENGINE_H
#define MODE1_INC_ENGINE_H

#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

// 聚合槽位数量 - 滑动窗口大小（slot_id % MAX_AGG_SLOTS 复用）
// 512MB / 4KB = 131072 消息，使用滑动窗口复用
#define MAX_AGG_SLOTS 65536

// 归约操作类型
typedef enum {
    OP_SUM = 0,
    OP_MAX = 1,
    OP_MIN = 2,
    OP_PROD = 3
} reduce_op_t;

// 数据类型
typedef enum {
    DTYPE_INT32 = 0,
    DTYPE_FLOAT32 = 1,
    DTYPE_FLOAT16 = 2,
    DTYPE_BFLOAT16 = 3
} data_type_t;

// 集合通信原语类型
typedef enum {
    PRIM_NULL = 0,
    PRIM_ALLREDUCE = 1,
    PRIM_REDUCE = 2,
    PRIM_BROADCAST = 3,
    PRIM_BARRIER = 4,
    PRIM_REDUCESCATTER = 5,
    PRIM_ALLGATHER = 6
} primitive_t;

// 槽位状态
typedef enum {
    SLOT_IDLE = 0,
    SLOT_AGGREGATING,
    SLOT_COMPLETE,
    SLOT_BROADCASTING
} slot_state_t;

// ============ 引擎 API ============

// 初始化计算引擎
int inc_engine_init(int slot_count);

// 销毁计算引擎
void inc_engine_destroy(void);

// 提交消息到聚合槽位
// 返回: 1=聚合完成, 0=等待更多消息, -1=错误
int inc_engine_submit(uint32_t slot_id, int conn_id,
                      void *data, uint32_t len,
                      primitive_t prim, reduce_op_t op,
                      data_type_t dtype, int expected);

// 获取聚合结果
void *inc_engine_get_result(uint32_t slot_id, uint32_t *len);

// 重置槽位
void inc_engine_reset_slot(uint32_t slot_id);

#ifdef __cplusplus
}
#endif

#endif // MODE1_INC_ENGINE_H
