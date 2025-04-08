#ifndef CXL_MCTP_MESSAGE_H_H
#define CXL_MCTP_MESSAGE_H_H
#include<stdint.h>

#define MCTP_CXL_MAILBOX_BYTES 512
#define MCTP_MESSAGE_BUF_NAME "mctp-message-buf"

struct CXLMCTPCommandBuf {
    /* uint8_t cci_name[64]; */
    uint8_t command_set;
    uint8_t command;
    size_t len_in;
    size_t len_out;
    uint8_t payload[MCTP_CXL_MAILBOX_BYTES];
    uint8_t payload_out[MCTP_CXL_MAILBOX_BYTES];
    bool bg_started;
    int ret_val;
};

typedef struct CXLMCTPCommandBuf CXLMCTPCommandBuf;

struct CXLCCINamePtrMap {
    char cci_name[64];
    void *cci_pointer; /* This should be filled by the target VM */
};

struct CXLCCINamePtrMaps {
    int num_mappings;
    struct CXLCCINamePtrMap maps[32];
};

struct CXLMCTPSharedBuf {
    /* set to 1 when sent to target VM and wait for 0 as it completes */
    int status;
    CXLMCTPCommandBuf command_buf;
};

typedef struct CXLMCTPSharedBuf CXLMCTPSharedBuf;
extern struct CXLCCINamePtrMaps *cci_map_buf;
int setup_mctp_qmp_connection(const char *qmp_str);
void read_qmp_response(int sockfd);
void qmp_cxl_mctp_process_cci_message(const int sockfd, const char *cci_name);
#endif
