

#ifndef INFINIBAND_OPCODE_H
#define INFINIBAND_OPCODE_H



typedef enum {
	/* operations -- just used to define real constants */
	RC_SEND_FIRST                        = 0x00,
	RC_SEND_MIDDLE                       = 0x01,
	RC_SEND_LAST                         = 0x02,
	RC_SEND_LAST_WITH_IMMEDIATE          = 0x03,
	RC_SEND_ONLY                         = 0x04,
	RC_SEND_ONLY_WITH_IMMEDIATE          = 0x05,
	RC_RDMA_WRITE_FIRST                  = 0x06,
	RC_RDMA_WRITE_MIDDLE                 = 0x07,
	RC_RDMA_WRITE_LAST                   = 0x08,
	RC_RDMA_WRITE_LAST_WITH_IMMEDIATE    = 0x09,
	RC_RDMA_WRITE_ONLY                   = 0x0a,
	RC_RDMA_WRITE_ONLY_WITH_IMMEDIATE    = 0x0b,
	RC_RDMA_READ_REQUEST                 = 0x0c,
	RC_RDMA_READ_RESPONSE_FIRST          = 0x0d,
	RC_RDMA_READ_RESPONSE_MIDDLE         = 0x0e,
	RC_RDMA_READ_RESPONSE_LAST           = 0x0f,
	RC_RDMA_READ_RESPONSE_ONLY           = 0x10,
	RC_ACKNOWLEDGE                       = 0x11,
	RC_ATOMIC_ACKNOWLEDGE                = 0x12,
	RC_COMPARE_SWAP                      = 0x13,
	RC_FETCH_ADD                         = 0x14
} opcode_t;

#endif /* INFINIBAND_OPCODE_H */
