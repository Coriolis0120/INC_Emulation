#include "rule.h"

/**
 * @brief 规则匹配 (四元组匹配)
 *
 * @param rule 规则指针
 * @param src_ip 源 IP
 * @param dst_ip 目的 IP
 * @param primitive 原语类型
 * @param primitive_param 原语参数 (root_rank/sender_rank, AllReduce 时为 -1)
 * @return 1 匹配成功, 0 匹配失败
 */
int match_rule(rule_t* rule, uint32_t src_ip, uint32_t dst_ip,
               primitive_type_t primitive, int primitive_param) {
    if (rule->src_ip != src_ip || rule->dst_ip != dst_ip) {
        return 0;
    }
    if (rule->primitive != primitive) {
        return 0;
    }
    // AllReduce, Barrier, ReduceScatter 不需要匹配 primitive_param
    if (primitive == PRIMITIVE_TYPE_ALLREDUCE ||
        primitive == PRIMITIVE_TYPE_BARRIER ||
        primitive == PRIMITIVE_TYPE_REDUCESCATTER) {
        return 1;
    }
    // Reduce/Broadcast 需要匹配 primitive_param
    return rule->primitive_param == primitive_param;
}

/**
 * @brief 规则查找 (四元组查找)
 *
 * @param table 规则表
 * @param src_ip 源 IP
 * @param dst_ip 目的 IP
 * @param primitive 原语类型
 * @param primitive_param 原语参数 (root_rank/sender_rank, AllReduce 时为 -1)
 * @return 匹配的规则指针, 未找到返回 NULL
 */
rule_t* lookup_rule(rule_table_t* table, uint32_t src_ip, uint32_t dst_ip,
                    primitive_type_t primitive, int primitive_param) {
    for (int i = 0; i < table->count; i++) {
        if (match_rule(&(table->rules[i]), src_ip, dst_ip, primitive, primitive_param) == 1) {
            return &(table->rules[i]);
        }
    }
    return NULL;
}

/**
 * @brief 添加规则到规则表
 *
 * @param table 规则表
 * @param rule 要添加的规则
 * @return 0 成功, -1 规则表已满
 */
int add_rule(rule_table_t* table, const rule_t* rule) {
    if (table->count >= MAX_RULES) {
        return -1;
    }

    table->rules[table->count] = *rule;
    table->count++;

    return 0;
}

