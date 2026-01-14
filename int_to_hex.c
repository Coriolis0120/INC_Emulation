#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("用法: %s <整数>\n", argv[0]);
        printf("示例: %s 83890348\n", argv[0]);
        return 1;
    }

    // 读取输入的整数
    long int input = strtol(argv[1], NULL, 10);

    // 检查是否超出uint32_t范围
    if (input < 0 || input > UINT32_MAX) {
        printf("错误: 输入值超出范围 (0 - %u)\n", UINT32_MAX);
        return 1;
    }

    uint32_t value = (uint32_t)input;

    // 输出各种格式
    printf("输入值: %ld\n", input);
    printf("十六进制:\n");
    printf("  小写: 0x%08x\n", value);
    printf("  大写: 0x%08X\n", value);
    printf("  无前缀: %08x\n", value);
    printf("  紧凑格式: %x\n", value);

    // 输出字节分解
    printf("\n字节分解 (大端法):\n");
    printf("  字节0 (最高位): 0x%02x\n", (value >> 24) & 0xFF);
    printf("  字节1: 0x%02x\n", (value >> 16) & 0xFF);
    printf("  字节2: 0x%02x\n", (value >> 8) & 0xFF);
    printf("  字节3 (最低位): 0x%02x\n", value & 0xFF);

    // 输出二进制表示
    printf("\n二进制:\n");
    printf("  ");
    for (int i = 31; i >= 0; i--) {
        printf("%d", (value >> i) & 1);
        if (i % 4 == 0 && i > 0) printf(" ");
    }
    printf("\n");

    return 0;
}