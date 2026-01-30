#include "switch_context.h"
#include <stdio.h>

/**
 * @brief 测试 switch_context 的初始化和清理（静态分配版本）
 */
int main(int argc, char *argv[]) {
    printf("Testing switch_context module (static allocation)...\n\n");

    // 静态分配上下文
    static switch_context_t ctx;

    // 初始化上下文
    if (switch_context_init(&ctx, 0, 4) < 0) {
        fprintf(stderr, "Failed to initialize switch context\n");
        return 1;
    }

    printf("\nContext initialized successfully!\n");
    printf("  Switch ID: %d\n", ctx.switch_id);
    printf("  Fan-in: %d\n", ctx.fan_in);
    printf("  Is root: %d\n", ctx.is_root);
    printf("  PSN array length: %d\n", SWITCH_ARRAY_LENGTH);

    // 测试 PSN 状态访问
    printf("\nTesting PSN state access...\n");
    psn_state_t *state = &ctx.psn_states[0];
    printf("  PSN[0] degree: %d\n", state->degree);
    printf("  PSN[0] r_degree: %d\n", state->r_degree);

    // 测试设置和访问
    printf("\nTesting PSN state modification...\n");
    pthread_mutex_lock(&state->mutex);
    state->degree = 5;
    state->agg_buffer.psn = 100;
    pthread_mutex_unlock(&state->mutex);
    printf("  PSN[0] degree after modification: %d\n", state->degree);
    printf("  PSN[0] agg_buffer.psn: %d\n", state->agg_buffer.psn);

    // 重置 PSN 状态
    printf("\nResetting PSN states...\n");
    switch_context_reset_psn_states(&ctx);
    printf("  PSN[0] degree after reset: %d\n", state->degree);
    printf("  PSN[0] agg_buffer.psn after reset: %d\n", state->agg_buffer.psn);

    // 清理上下文
    printf("\nCleaning up context...\n");
    switch_context_cleanup(&ctx);

    // 测试可视化功能
    printf("\n=== Testing Visualization Functions ===\n");

    // 重新初始化用于测试
    switch_context_init(&ctx, 1, 4);

    // 设置一些测试数据
    ctx.fan_in = 2;
    ctx.is_root = 0;
    ctx.num_receivers = 3;
    ctx.operation_type = OPERATION_TYPE_REDUCE;
    ctx.root_rank = 0;
    ctx.state = SWITCH_STATE_RUNNING;

    // 添加一些测试规则
    rule_t test_rule;
    test_rule.src_ip = 0xC0A80001;  // 192.168.0.1
    test_rule.dst_ip = 0xC0A80002;  // 192.168.0.2
    test_rule.direction = DIR_UP;
    test_rule.out_conns_cnt = 1;
    add_rule(&ctx.routing_table, &test_rule);

    // 打印上下文
    printf("\n--- Printf Output ---\n");
    switch_context_print(&ctx);

    // 导出为 YAML
    printf("\n--- YAML Export ---\n");
    switch_context_export_yaml(&ctx, "/tmp/switch_context_test.yaml");

    // 清理
    switch_context_cleanup(&ctx);

    printf("\nAll tests passed!\n");
    return 0;
}
