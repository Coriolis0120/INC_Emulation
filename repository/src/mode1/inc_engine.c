/**
 * @file inc_engine.c
 * @brief 计算引擎 - 消息级聚合逻辑
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "mode1/inc_engine.h"

// 聚合槽位结构
typedef struct {
    uint32_t slot_id;           // 槽位索引
    uint32_t original_slot_id;  // 原始 slot_id（用于滑动窗口区分）
    slot_state_t state;

    // 聚合状态
    uint32_t expected_count;
    uint32_t arrived_count;
    uint64_t arrival_bitmap;

    // 聚合缓冲
    void *agg_buffer;
    uint32_t buf_size;   // 已分配的缓冲区大小
    uint32_t data_len;

    // 操作信息
    primitive_t primitive;
    reduce_op_t op_type;
    data_type_t data_type;
    int root_rank;

    pthread_mutex_t mutex;
} agg_slot_t;

// 计算引擎上下文
typedef struct {
    agg_slot_t *slots;
    int slot_count;
    pthread_mutex_t global_lock;
} inc_engine_ctx_t;

// 全局引擎实例
static inc_engine_ctx_t *g_engine = NULL;

// ============ 归约操作 ============

static void reduce_sum_int32(int32_t *dst, const int32_t *src, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        dst[i] += src[i];
    }
}

static void reduce_sum_float32(float *dst, const float *src, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        dst[i] += src[i];
    }
}

static void reduce_max_int32(int32_t *dst, const int32_t *src, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (src[i] > dst[i]) dst[i] = src[i];
    }
}

static void reduce_min_int32(int32_t *dst, const int32_t *src, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (src[i] < dst[i]) dst[i] = src[i];
    }
}

// 通用归约函数
static void do_reduce(void *dst, const void *src, uint32_t len,
                      reduce_op_t op, data_type_t dtype) {
    uint32_t count;

    switch (dtype) {
    case DTYPE_INT32:
        count = len / sizeof(int32_t);
        if (op == OP_SUM) reduce_sum_int32(dst, src, count);
        else if (op == OP_MAX) reduce_max_int32(dst, src, count);
        else if (op == OP_MIN) reduce_min_int32(dst, src, count);
        break;
    case DTYPE_FLOAT32:
        count = len / sizeof(float);
        if (op == OP_SUM) reduce_sum_float32(dst, src, count);
        break;
    default:
        break;
    }
}

// ============ 引擎初始化 ============

int inc_engine_init(int slot_count) {
    if (g_engine) return 0;

    g_engine = calloc(1, sizeof(inc_engine_ctx_t));
    if (!g_engine) return -1;

    g_engine->slot_count = slot_count;
    g_engine->slots = calloc(slot_count, sizeof(agg_slot_t));
    if (!g_engine->slots) {
        free(g_engine);
        g_engine = NULL;
        return -1;
    }

    pthread_mutex_init(&g_engine->global_lock, NULL);

    for (int i = 0; i < slot_count; i++) {
        g_engine->slots[i].slot_id = i;
        g_engine->slots[i].state = SLOT_IDLE;
        pthread_mutex_init(&g_engine->slots[i].mutex, NULL);
    }

    printf("[IncEngine] Initialized with %d slots\n", slot_count);
    return 0;
}

void inc_engine_destroy(void) {
    if (!g_engine) return;

    for (int i = 0; i < g_engine->slot_count; i++) {
        if (g_engine->slots[i].agg_buffer) {
            free(g_engine->slots[i].agg_buffer);
        }
        pthread_mutex_destroy(&g_engine->slots[i].mutex);
    }

    free(g_engine->slots);
    pthread_mutex_destroy(&g_engine->global_lock);
    free(g_engine);
    g_engine = NULL;
}

// ============ 消息提交 ============

// 获取槽位 - 使用滑动窗口机制
static agg_slot_t *get_slot(uint32_t slot_id) {
    if (!g_engine) return NULL;
    // 使用模运算实现滑动窗口
    uint32_t idx = slot_id % g_engine->slot_count;
    return &g_engine->slots[idx];
}

// 提交消息到聚合槽位
int inc_engine_submit(uint32_t slot_id, int conn_id,
                      void *data, uint32_t len,
                      primitive_t prim, reduce_op_t op,
                      data_type_t dtype, int expected) {
    agg_slot_t *slot = get_slot(slot_id);
    if (!slot) {
        return -1;
    }

    pthread_mutex_lock(&slot->mutex);

    // 滑动窗口：检查是否是新的 slot_id（复用旧槽位）
    if (slot->state != SLOT_IDLE && slot->original_slot_id != slot_id) {
        // 旧 slot 被复用，强制重置
        slot->state = SLOT_IDLE;
        slot->arrived_count = 0;
        slot->arrival_bitmap = 0;
        if (slot->agg_buffer && slot->buf_size > 0) {
            memset(slot->agg_buffer, 0, slot->buf_size);
        }
        slot->data_len = 0;
    }

    if (slot->state == SLOT_IDLE) {
        // 第一个消息，初始化槽位
        slot->original_slot_id = slot_id;
        slot->state = SLOT_AGGREGATING;
        slot->primitive = prim;
        slot->op_type = op;
        slot->data_type = dtype;
        slot->expected_count = expected;
        slot->arrived_count = 1;
        slot->arrival_bitmap = (1ULL << conn_id);
        slot->data_len = len;

        // 分配或重新分配聚合缓冲区
        if (!slot->agg_buffer || slot->buf_size < len) {
            free(slot->agg_buffer);
            slot->agg_buffer = malloc(len);
            slot->buf_size = len;
            if (!slot->agg_buffer) {
                pthread_mutex_unlock(&slot->mutex);
                return -1;
            }
        }
        memcpy(slot->agg_buffer, data, len);
    } else {
        // 后续消息，执行归约
        uint32_t reduce_len = (len < slot->data_len) ? len : slot->data_len;
        do_reduce(slot->agg_buffer, data, reduce_len, op, dtype);
        slot->arrived_count++;
        slot->arrival_bitmap |= (1ULL << conn_id);
    }

    int complete = (slot->arrived_count >= slot->expected_count);
    if (complete) {
        slot->state = SLOT_COMPLETE;
    }

    pthread_mutex_unlock(&slot->mutex);
    return complete ? 1 : 0;
}

// 获取聚合结果
void *inc_engine_get_result(uint32_t slot_id, uint32_t *len) {
    agg_slot_t *slot = get_slot(slot_id);
    if (!slot || slot->state != SLOT_COMPLETE || slot->original_slot_id != slot_id) {
        return NULL;
    }
    if (len) *len = slot->data_len;
    return slot->agg_buffer;
}

// 重置槽位
void inc_engine_reset_slot(uint32_t slot_id) {
    agg_slot_t *slot = get_slot(slot_id);
    if (!slot) return;

    pthread_mutex_lock(&slot->mutex);
    slot->state = SLOT_IDLE;
    slot->arrived_count = 0;
    slot->arrival_bitmap = 0;
    // 清零缓冲区，防止旧数据影响新聚合
    if (slot->agg_buffer && slot->buf_size > 0) {
        memset(slot->agg_buffer, 0, slot->buf_size);
    }
    slot->data_len = 0;
    pthread_mutex_unlock(&slot->mutex);
}
