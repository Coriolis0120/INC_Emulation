#ifndef UTIL_H
#define UTIL_H


#include <stdint.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <pcap.h>
#include <pthread.h>

typedef enum {
    PRIMITIVE_TYPE_NULL = 0,
    PRIMITIVE_TYPE_ALLREDUCE = 1,
    PRIMITIVE_TYPE_REDUCE = 2,
    PRIMITIVE_TYPE_BROADCAST = 3,
    PRIMITIVE_TYPE_BARRIER = 4
} primitive_type_t;

// ==================== 控制包协议定义 ====================
// 使用 RDMA Send with Immediate 的 32-bit immediate data 传递元数据
// 格式: [Destination Rank: 16 bits][Primitive: 2 bits][Operator: 2 bits][Data Type: 4 bits][Reserved: 8 bits]

// Primitive 类型 (2 bits)
#define CTL_PRIMITIVE_ALLREDUCE  0x00  // 00
#define CTL_PRIMITIVE_REDUCE     0x01  // 01
#define CTL_PRIMITIVE_BROADCAST  0x02  // 10

// Operator 类型 (2 bits)
#define CTL_OPERATOR_BARRIER     0x00  // 00
#define CTL_OPERATOR_SUM         0x01  // 01
#define CTL_OPERATOR_MAX         0x02  // 10
#define CTL_OPERATOR_MIN         0x03  // 11

// Data Type (4 bits)
#define CTL_DATATYPE_INT32       0x00  // 0000
#define CTL_DATATYPE_FLOAT32     0x01  // 0001

// 特殊 Destination Rank 值
#define CTL_DEST_RANK_ALL        0xFFFF  // AllReduce/Broadcast 使用

// Immediate Data 构建宏
// 格式: [dest_rank:16][primitive:2][operator:2][datatype:4][reserved:8]
#define BUILD_IMM_DATA(dest_rank, primitive, operator, datatype) \
    ((((uint32_t)(dest_rank) & 0xFFFF) << 16) | \
     (((uint32_t)(primitive) & 0x03) << 14) | \
     (((uint32_t)(operator) & 0x03) << 12) | \
     (((uint32_t)(datatype) & 0x0F) << 8))

// Immediate Data 解析宏
#define GET_IMM_DEST_RANK(imm)   (((imm) >> 16) & 0xFFFF)
#define GET_IMM_PRIMITIVE(imm)   (((imm) >> 14) & 0x03)
#define GET_IMM_OPERATOR(imm)    (((imm) >> 12) & 0x03)
#define GET_IMM_DATATYPE(imm)    (((imm) >> 8) & 0x0F)


#define PAYLOAD_LEN 4096 // mtu bytes

typedef struct {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ether_type;
} eth_header_t;

// IPv4 头 20 bytes
typedef struct {
    uint8_t  version_ihl;    // 版本 (4 bits) + IHL (4 bits)
    uint8_t  tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_frag_off; // 标志 (3 bits) + 分片偏移 (13 bits)
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} ipv4_header_t;

// UDP 头 8 bytes
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length; // 总长
    uint16_t checksum;
} udp_header_t;

// RRoCE (RoCEv2) BTH 头 12bytes
typedef struct {
    uint8_t  opcode;
    uint8_t  se_m_pad;
    uint16_t pkey;
    uint32_t qpn;
    uint32_t apsn;
} bth_header_t;

// AETH 4 bytes
typedef struct {
    uint32_t syn_msn; // syndrome + msn
} aeth_header_t;

typedef struct {
	uint64_t va;
	uint32_t rkey;
	uint32_t len;
} reth_header_t;

// inc协议定义的控制头
typedef struct 
{
    uint16_t destination_rank; // 若为Allreduce和Broadcast，则为0xffff，如果为reduce，则为目的rank号。
    uint8_t primitive_operator_dataType; // 4bits primitive + 2bits Operator + 2bits unused
    // primitive: 00-Allreduce，01-Reduce，10-Broadcast，11-Undifined
    uint8_t Data_type; // 4bits Data_type + 4 bits unused.  0000-int32, 0001-float32 
    
} ctl_header_t;



typedef struct {
    eth_header_t *eth;
    ipv4_header_t *ip;
    udp_header_t *udp;
    bth_header_t *bth;
    ctl_header_t *inc;
    aeth_header_t *aeth;
    int *payload;
} inc_header_t;


typedef struct {
    char device[16]; // 网卡设备名

    uint8_t  my_mac[6];
    uint8_t  peer_mac[6];
    uint32_t my_ip;
    uint32_t peer_ip;
    uint16_t my_port;
    uint16_t peer_port;

    uint32_t my_qp;
    uint32_t peer_qp;
    uint32_t psn; // 用于记录该connection下一个发送的包的psn
    uint32_t msn; // 用于记录max sequence number

    int is_switch;  // 对端是否是交换机

    pcap_t *handle;       // 接收用 pcap handle

} connection_t;

typedef enum {
    PACKET_TYPE_DATA,
    PACKET_TYPE_ACK,
    PACKET_TYPE_CONTROLL
} packet_type_t;





uint32_t get_ip(const char *ip_str);

void print_mac(int id, const char *prefix, const uint8_t mac[6]);
void print_ip(int id, const char *prefix, uint32_t ip);
void print_eth_header(int id, const eth_header_t *eth);
void print_ipv4_header(int id, const ipv4_header_t *ip);
void print_udp_header(int id, const udp_header_t *udp);
void print_bth_header(int id, const bth_header_t *bth);
void print_connection(int id, const connection_t *conn);

uint16_t ipv4_checksum(const ipv4_header_t *ip);
int is_ipv4_checksum_valid(const ipv4_header_t *ip);
uint32_t crc32(const void *data, size_t length);
uint32_t compute_icrc(int id, const char* packet);
int is_icrc_valid(int id, const char* packet);
void print_all(int id, const char* packet);

uint32_t build_eth_packet
(
    char *dst_packet, packet_type_t type, char *data, int data_len, 
    char *src_mac, char *dst_mac,
    uint32_t src_ip, uint32_t dst_ip,
    uint16_t src_port, uint16_t dst_port,
    uint32_t qp, uint32_t psn, 
    uint32_t msn, int _opcode, const uint8_t *reth
);

uint64_t get_now_ts();

void init_crc32_table();
uint32_t crc32(const void *data, size_t length);

void send_file_with_length(int fd, const char *file_path) ;
void receive_file(int sockfd, const char *save_path) ;

// 将大端法uint32_t以IP格式转换为字符串（分解为四个字节）
char* uint32_to_ip_string_big_endian(uint32_t value);

#endif // UTIL_H

