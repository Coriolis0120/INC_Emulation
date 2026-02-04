#ifndef SWITCH_MODE_H
#define SWITCH_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

// 交换机运行模式
typedef enum {
    SWITCH_MODE_PASSTHROUGH = 0,  // 现有模式：逐包处理
    SWITCH_MODE_TERMINATION = 1   // 模态-I：连接终结
} switch_mode_t;

#ifdef __cplusplus
}
#endif

#endif // SWITCH_MODE_H
