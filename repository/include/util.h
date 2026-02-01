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
    PRIMITIVE_TYPE_BARRIER = 4,
    PRIMITIVE_TYPE_REDUCESCATTER = 5
} primitive_type_t;

// ==================== 控制包协议定义 ====================
// 使用 RDMA Send with Immediate 的 32-bit immediate data 传递元数据
// 格式: [Destination Rank: 16 bits][Primitive: 2 bits][Operator: 2 bits][Data Type: 4 bits][Extended Primitive: 4 bits][Reserved: 4 bits]
// 当 Primitive == 0x03 时，使用 Extended Primitive 字段区分 Barrier/ReduceScatter 等

// Primitive 类型 (2 bits) - 基础类型
#define CTL_PRIMITIVE_ALLREDUCE  0x00  // 00
#define CTL_PRIMITIVE_REDUCE     0x01  // 01
#define CTL_PRIMITIVE_BROADCAST  0x02  // 10
#define CTL_PRIMITIVE_EXTENDED   0x03  // 11 - 扩展类型，查看 Extended Primitive 字段

// Extended Primitive 类型 (4 bits) - 当 Primitive == 0x03 时使用
#define CTL_EXT_BARRIER          0x00  // 0000 - Barrier
#define CTL_EXT_REDUCESCATTER    0x01  // 0001 - ReduceScatter

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
// 格式: [dest_rank:16][primitive:2][operator:2][datatype:4][ext_prim:4][reserved:4]
#define BUILD_IMM_DATA(dest_rank, primitive, operator, datatype) \
    ((((uint32_t)(dest_rank) & 0xFFFF) << 16) | \
     (((uint32_t)(primitive) & 0x03) << 14) | \
     (((uint32_t)(operator) & 0x03) << 12) | \
     (((uint32_t)(datatype) & 0x0F) << 8))

// 扩展原语的 Immediate Data 构建宏
#define BUILD_IMM_DATA_EXT(dest_rank, ext_prim, operator, datatype) \
    ((((uint32_t)(dest_rank) & 0xFFFF) << 16) | \
     (((uint32_t)CTL_PRIMITIVE_EXTENDED & 0x03) << 14) | \
     (((uint32_t)(operator) & 0x03) << 12) | \
     (((uint32_t)(datatype) & 0x0F) << 8) | \
     (((uint32_t)(ext_prim) & 0x0F) << 4))

// Immediate Data 解析宏
#define GET_IMM_DEST_RANK(imm)   (((imm) >> 16) & 0xFFFF)
#define GET_IMM_PRIMITIVE(imm)   (((imm) >> 14) & 0x03)
#define GET_IMM_OPERATOR(imm)    (((imm) >> 12) & 0x03)
#define GET_IMM_DATATYPE(imm)    (((imm) >> 8) & 0x0F)
#define GET_IMM_EXT_PRIM(imm)    (((imm) >> 4) & 0x0F)

// RDMA opcode 定义
#define RDMA_OPCODE_SEND_ONLY           0x04
#define RDMA_OPCODE_SEND_ONLY_WITH_IMM  0x05  // Send Only with Immediate
#define RDMA_OPCODE_SEND_FIRST          0x00
#define RDMA_OPCODE_SEND_MIDDLE         0x01
#define RDMA_OPCODE_SEND_LAST           0x02
#define RDMA_OPCODE_SEND_LAST_WITH_IMM  0x03  // Send Last with Immediate
#define RDMA_OPCODE_ACK                 0x11

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
} aeth_t;

typedef struct {
	uint64_t va;
	uint32_t rkey;
	uint32_t len;
} reth_header_t;

// 协议定义的控制头
typedef struct 
{
    uint16_t destination_rank; // 若为Allreduce和Broadcast，则为0xffff，如果为reduce，则为目的rank号。
    uint8_t primitive_operator_dataType; // 4bits primitive + 2bits Operator + 2bits unused
    // primitive: 00-Allreduce，01-Reduce，10-Broadcast，11-Undifined
    uint8_t Data_type; // 4bits Data_type + 4 bits unused.  0000-int32, 0001-float32 
    
} ctl_header_t;



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
    int ok; // 连接状态标志（连接是否可用）
    int is_switch;  // 对端是否是交换机（用于多层拓扑）
    int peer_id;    // 对端 ID（交换机 ID 或 rank）

    pcap_t *handle;       // 接收用 pcap handle
    pcap_t *send_handle;  // 发送用 pcap handle（同一设备的连接共享）
    pthread_mutex_t *send_mutex;  // 发送锁指针（指向设备级别的共享锁）
    int device_id;        // 设备 ID（用于查找发送队列）
} connection_t;

typedef enum {
    PACKET_TYPE_DATA,
    PACKET_TYPE_ACK,
    PACKET_TYPE_NAK,
    PACKET_TYPE_DATA_SINGLE,
    PACKET_TYPE_RETH,
    PACKET_TYPE_CONTROLL
} packet_type_t;

#define PAYLOAD_LEN 4096 // mtu bytes (4KB per packet)
#define ELEMENT_SIZE sizeof(int32_t)

typedef struct {
    uint32_t seq;
    uint32_t type;
    // ...
} my_header_t;

typedef int32_t my_payload_t[PAYLOAD_LEN];

typedef struct  {
    my_header_t header;
    my_payload_t payload;
} my_packet_t; 

void print_packet(const my_packet_t *p);

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

