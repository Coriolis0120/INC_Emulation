/**
 * @file soft_switch.c
 * @brief 软件交换机 - 用于 MPI 测试的简单 L3 转发
 *
 * 功能：
 * - 监听两个网卡，根据目的 IP 进行路由转发
 * - 支持 Spine 和 Leaf 两种角色
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <pcap.h>
#include <arpa/inet.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <sys/ioctl.h>
#include <net/if.h>

// 以太网头部大小
#define ETH_HLEN 14

// 最大路由条目数
#define MAX_ROUTES 16

// CRC32 查找表 (用于模拟 ICRC 计算)
static uint32_t crc32_table[256];
static int crc32_table_init = 0;

static void init_crc32_table(void) {
    if (crc32_table_init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
        crc32_table[i] = crc;
    }
    crc32_table_init = 1;
}

static uint32_t compute_crc32(const uint8_t *data, int len) {
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

// 路由条目
typedef struct {
    uint32_t network;      // 网络地址
    uint32_t netmask;      // 子网掩码
    int out_port;          // 输出端口 (0 或 1)
    uint8_t next_hop_mac[6]; // 下一跳 MAC 地址
} route_entry_t;

// 端口信息
typedef struct {
    char dev_name[32];
    pcap_t *handle;
    uint8_t mac[6];
    uint32_t ip;
} port_info_t;

// 交换机上下文
typedef struct {
    port_info_t ports[2];
    route_entry_t routes[MAX_ROUTES];
    int route_count;
    int running;
} switch_ctx_t;

static switch_ctx_t g_ctx;

// 统计信息
static uint64_t pkt_count[2] = {0, 0};
static uint64_t fwd_count[2] = {0, 0};

/**
 * @brief 获取网卡 MAC 地址
 */
static int get_mac_addr(const char *dev, uint8_t *mac) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    struct ifreq ifr;
    strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);

    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
        close(fd);
        return -1;
    }

    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    close(fd);
    return 0;
}

/**
 * @brief 获取网卡 IP 地址
 */
static uint32_t get_ip_addr(const char *dev) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;

    struct ifreq ifr;
    strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);

    if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {
        close(fd);
        return 0;
    }

    uint32_t ip = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
    close(fd);
    return ip;
}

/**
 * @brief 添加路由条目
 */
static void add_route(const char *network_str, const char *netmask_str,
                      int out_port, const char *next_hop_mac_str) {
    if (g_ctx.route_count >= MAX_ROUTES) {
        fprintf(stderr, "Route table full!\n");
        return;
    }

    route_entry_t *r = &g_ctx.routes[g_ctx.route_count];
    r->network = inet_addr(network_str);
    r->netmask = inet_addr(netmask_str);
    r->out_port = out_port;

    // 解析 MAC 地址
    sscanf(next_hop_mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
           &r->next_hop_mac[0], &r->next_hop_mac[1], &r->next_hop_mac[2],
           &r->next_hop_mac[3], &r->next_hop_mac[4], &r->next_hop_mac[5]);

    g_ctx.route_count++;

    char net_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &r->network, net_str, sizeof(net_str));
    printf("  Route added: %s/%s -> port %d, MAC %s\n",
           net_str, netmask_str, out_port, next_hop_mac_str);
}

/**
 * @brief 查找路由
 */
static route_entry_t *lookup_route(uint32_t dst_ip) {
    for (int i = 0; i < g_ctx.route_count; i++) {
        route_entry_t *r = &g_ctx.routes[i];
        if ((dst_ip & r->netmask) == (r->network & r->netmask)) {
            return r;
        }
    }
    return NULL;
}

/**
 * @brief 转发数据包
 */
static void forward_packet(int in_port, const uint8_t *packet, int len) {
    if (len < ETH_HLEN + sizeof(struct iphdr)) {
        return;
    }

    // 跳过超过 MTU 的包（可能是 TSO 大包）
    if (len > 1500) {
        return;
    }

    struct ether_header *eth = (struct ether_header *)packet;

    // 只处理 IP 包
    if (ntohs(eth->ether_type) != ETHERTYPE_IP) {
        return;
    }

    struct iphdr *ip = (struct iphdr *)(packet + ETH_HLEN);
    uint32_t dst_ip = ip->daddr;
    uint32_t src_ip = ip->saddr;

    // 查找路由
    route_entry_t *route = lookup_route(dst_ip);
    if (!route) {
        // 调试：打印无路由的包
        char src_str[INET_ADDRSTRLEN], dst_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src_ip, src_str, sizeof(src_str));
        inet_ntop(AF_INET, &dst_ip, dst_str, sizeof(dst_str));
        printf("[DEBUG] No route for %s -> %s (port %d)\n", src_str, dst_str, in_port);
        fflush(stdout);
        return;  // 无路由，丢弃
    }

    int out_port = route->out_port;

    // 不转发回同一端口
    if (out_port == in_port) {
        return;
    }

    // 修改以太网头部
    uint8_t new_packet[65536];
    memcpy(new_packet, packet, len);

    struct ether_header *new_eth = (struct ether_header *)new_packet;
    memcpy(new_eth->ether_dhost, route->next_hop_mac, 6);
    memcpy(new_eth->ether_shost, g_ctx.ports[out_port].mac, 6);

    // 模拟 ICRC 计算 (从 UDP payload 开始，即 ETH+IP+UDP 头之后)
    // ETH(14) + IP(20) + UDP(8) = 42 bytes
    if (len > 42) {
        volatile uint32_t icrc = compute_crc32(new_packet + 42, len - 42);
        (void)icrc;  // 防止编译器优化掉
    }

    // 发送
    if (pcap_sendpacket(g_ctx.ports[out_port].handle, new_packet, len) != 0) {
        fprintf(stderr, "Send failed on port %d: %s\n",
                out_port, pcap_geterr(g_ctx.ports[out_port].handle));
    } else {
        fwd_count[out_port]++;
    }
}

/**
 * @brief 数据包处理回调
 */
static void packet_handler(uint8_t *user, const struct pcap_pkthdr *h,
                           const uint8_t *packet) {
    int port = (int)(intptr_t)user;
    pkt_count[port]++;
    forward_packet(port, packet, h->caplen);
}

/**
 * @brief 接收线程
 */
static void *receiver_thread(void *arg) {
    int port = (int)(intptr_t)arg;
    printf("Receiver thread started for port %d (%s)\n",
           port, g_ctx.ports[port].dev_name);
    fflush(stdout);

    while (g_ctx.running) {
        int ret = pcap_dispatch(g_ctx.ports[port].handle, 100, packet_handler, (uint8_t *)(intptr_t)port);
        if (ret < 0) {
            fprintf(stderr, "pcap_dispatch error on port %d: %s\n",
                    port, pcap_geterr(g_ctx.ports[port].handle));
            break;
        }
    }

    return NULL;
}

/**
 * @brief 统计线程
 */
static void *stats_thread(void *arg) {
    (void)arg;
    while (g_ctx.running) {
        sleep(5);
        printf("[Stats] Port0: recv=%lu fwd=%lu | Port1: recv=%lu fwd=%lu\n",
               pkt_count[0], fwd_count[0], pkt_count[1], fwd_count[1]);
        fflush(stdout);
    }
    return NULL;
}

/**
 * @brief 初始化端口
 */
static int init_port(int port_id, const char *dev_name) {
    port_info_t *p = &g_ctx.ports[port_id];
    strncpy(p->dev_name, dev_name, sizeof(p->dev_name) - 1);

    // 获取 MAC 地址
    if (get_mac_addr(dev_name, p->mac) < 0) {
        fprintf(stderr, "Failed to get MAC for %s\n", dev_name);
        return -1;
    }

    // 获取 IP 地址
    p->ip = get_ip_addr(dev_name);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &p->ip, ip_str, sizeof(ip_str));
    printf("Port %d: %s, MAC=%02x:%02x:%02x:%02x:%02x:%02x, IP=%s\n",
           port_id, dev_name,
           p->mac[0], p->mac[1], p->mac[2], p->mac[3], p->mac[4], p->mac[5],
           ip_str);

    // 打开 pcap
    char errbuf[PCAP_ERRBUF_SIZE];
    p->handle = pcap_open_live(dev_name, 65536, 1, 1, errbuf);
    if (!p->handle) {
        fprintf(stderr, "pcap_open_live failed for %s: %s\n", dev_name, errbuf);
        return -1;
    }

    return 0;
}

static void print_usage(const char *prog) {
    printf("Usage: %s <role>\n", prog);
    printf("  role: spine | leaf1 | leaf2\n");
}

int main(int argc, char *argv[]) {
    // 禁用输出缓冲
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    // 初始化 CRC32 表
    init_crc32_table();

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *role = argv[1];
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.running = 1;

    printf("=== Soft Switch ===\n");
    printf("Role: %s\n\n", role);

    if (strcmp(role, "spine") == 0) {
        // Spine: eth1 (172.16.0.21) <-> eth2 (10.10.0.14)
        if (init_port(0, "eth1") < 0 || init_port(1, "eth2") < 0) {
            return 1;
        }
        // 路由：到 10.1.1.0/24 和 10.2.1.0/24
        // 从 eth1 收到的包，目的是 10.2.1.0/24，转发到 eth2 (vm2: 10.10.0.7)
        add_route("10.2.1.0", "255.255.255.0", 1, "fa:16:3e:7b:25:36");
        // 从 eth2 收到的包，目的是 10.1.1.0/24，转发到 eth1 (vm1: 172.16.0.5)
        add_route("10.1.1.0", "255.255.255.0", 0, "fa:16:3e:87:09:4c");

    } else if (strcmp(role, "leaf1") == 0) {
        // Leaf1 (vm1): eth1 (172.16.0.5) <-> eth2 (10.1.1.25)
        if (init_port(0, "eth1") < 0 || init_port(1, "eth2") < 0) {
            return 1;
        }
        // 从 eth2 收到的包，目的是 10.2.1.0/24，转发到 eth1 (spine: 172.16.0.21)
        add_route("10.2.1.0", "255.255.255.0", 0, "fa:16:3e:9a:26:99");
        // 从 eth1 收到的包，目的是 10.1.1.21 (pku1)，转发到 eth2
        add_route("10.1.1.21", "255.255.255.255", 1, "fa:16:3e:5f:54:f6");
        // 从 eth1 收到的包，目的是 10.1.1.5 (pku2)，转发到 eth2
        add_route("10.1.1.5", "255.255.255.255", 1, "fa:16:3e:c9:8e:7d");

    } else if (strcmp(role, "leaf2") == 0) {
        // Leaf2 (vm2): eth1 (10.10.0.7) <-> eth2 (10.2.1.18)
        if (init_port(0, "eth1") < 0 || init_port(1, "eth2") < 0) {
            return 1;
        }
        // 从 eth2 收到的包，目的是 10.1.1.0/24，转发到 eth1 (spine: 10.10.0.14)
        add_route("10.1.1.0", "255.255.255.0", 0, "fa:16:3e:3d:ab:78");
        // 从 eth1 收到的包，目的是 10.2.1.7 (pku3)，转发到 eth2
        add_route("10.2.1.7", "255.255.255.255", 1, "fa:16:3e:bf:0a:a0");
        // 从 eth1 收到的包，目的是 10.2.1.12 (pku4)，转发到 eth2
        add_route("10.2.1.12", "255.255.255.255", 1, "fa:16:3e:07:9a:2c");

    } else {
        print_usage(argv[0]);
        return 1;
    }

    printf("\nStarting packet forwarding...\n");

    // 启动接收线程
    pthread_t recv_threads[2];

    for (int i = 0; i < 2; i++) {
        pthread_create(&recv_threads[i], NULL, receiver_thread, (void *)(intptr_t)i);
    }

    // 启动统计线程
    pthread_t stats_tid;
    pthread_create(&stats_tid, NULL, stats_thread, NULL);

    printf("Press Ctrl+C to stop.\n\n");

    // 等待线程
    for (int i = 0; i < 2; i++) {
        pthread_join(recv_threads[i], NULL);
    }

    return 0;
}
