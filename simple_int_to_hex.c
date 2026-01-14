#include <stdio.h>
#include <stdint.h>

int main() {
    unsigned int value;

    printf("请输入一个整数: ");
    if (scanf("%u", &value) == 1) {
        printf("十六进制: 0x%08X\n", value);
        printf("无\u524d\u7f00\u5341\u516d\u8fdb\u5236: %X\n", value);
        printf("二\u8fdb\u5236: ");
        for (int i = 31; i >= 0; i--) {
            printf("%d", (value >> i) & 1);
            if (i % 4 == 0 && i > 0) printf(" ");
        }
        printf("\n");
    } else {
        printf("输\u5165\u65e0\u6548\uff01\n");
    }

    return 0;
}