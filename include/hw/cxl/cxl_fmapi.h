/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * CXL Fabric Manager API definitions
 *
 * Copyright (c) 2022 Huawei Technologies.
 */

#ifndef CXL_FMAPI_H
#define CXL_FMAPI_H

#include "qemu/osdep.h"

/*
 * Errata for the Compute Express Link Specification Revision 2.0 - May 2021
 * Errata F24 applies
 */

#define CXL_FMAPI_INF_STAT_SET 0x00
#define CXL_FMAPI_INF_STAT_IDENITFY 0x01
#define CXL_FMAPI_INF_STAT_BO_STAT 0x02
#define CXL_FMAPI_INF_STAT_GET_RESP_MESSAGE_LIMIT 0x03
#define CXL_FMAPI_INF_STAT_SET_RESP_MESSAGE_LMIT 0x04
/*
 * Errata F24 introduces Table X - Mandatory over MCTP
 */
struct cxl_fmapi_inf_stat_ident_resp_pl {
    uint16_t pci_vendor_id;
    uint16_t pci_device_id;
    uint16_t pci_subsystem_vendor_id;
    uint16_t pci_subsystem_id;
    uint8_t serial_number[8];
    uint8_t max_message_size;
} QEMU_PACKED;

/*
 * Errata F24 introduces Table Y - Mandatory over MCTP
 */
struct cxl_fmapi_inf_stat_bo_stat_resp_pl {
    uint8_t background_operation_status;
    uint8_t rsv1;
    uint16_t command_op_code;
    uint16_t return_code;
    uint16_t vendor_specific;
} QEMU_PACKED;

/*
 * Errata F24 introduces Table Z - Mandatory over MCTP
 */
struct cxl_fmapi_inf_stat_get_resp_message_limit_resp_pl {
    uint8_t message_limit;
} QEMU_PACKED;

/*
 * Errata F24 introduces Table A - Mandatory over MCTP
 */
struct cxl_fmapi_inf_stat_set_resp_message_limit_req_pl {
    uint8_t message_limit;
} QEMU_PACKED;

/*
 * Errata F24 introduces Table B - Mandatory over MCTP
 */
struct cxl_fmapi_inf_stat_set_resp_message_limit_resp_pl {
    uint8_t message_limit;
} QEMU_PACKED;

#define CXL_FMAPI_CMD_SET_PHYSICAL_SWITCH 0x51
#define   CXL_FMAPI_PHYSICAL_SWITCH_IDENTIFY_SWITCH 0x00
#define   CXL_FMAPI_GET_PHYSICAL_PORT_STATE 0x01
#define   CXL_FMAPI_PHYSICAL_PORT_CONTROL 0x02
#define   CXL_FMAPI_SEND_PPB_CXLIO_CONFIG_REQ 0x03
#define CXL_FMAPI_CMD_SET_VIRTUAL_SWITCH 0x52
#define   CXL_FMAPI_GET_VIRTUAL_SWITCH_INFO 0x00
#define   CXL_FMAPI_BIND_VPPB 0x01
#define   CXL_FMAPI_UNBIND_VPPD 0x02
#define   CXL_FMAPI_GENERATE_AER_EVENT 0x03
#define CXL_FMAPI_CMD_SET_MLD_PORT 0x53
#define   CXL_FMAPI_MLD_TUNNEL_MANAGEMENT_COMMAND 0x00
#define   CXL_FMAPI_MLD_SEND_PPB_CXLIO_CONFIG_REQ 0x01
#define   CXL_FMAPI_MLD_SEND_PPB_CXLIO_MEMORY_REQ 0x02
#define CXL_FMAPI_CMD_SET_MLD_COMPONENT 0x54 /* MLD only */
#define   CXL_FMAPI_GET_LD_INFO 0x00
#define   CXL_FMAPI_GET_LD_ALLOCATIONS 0x01
#define   CXL_FMAPI_SET_LD_ALLOCATIONS 0x02
#define   CXL_FMAPI_GET_QOS_CONTROL 0x03
#define   CXL_FMAPI_SET_QOS_CONTROL 0x04
#define   CXL_FMAPI_GET_QOS_STATUS 0x05
#define   CXL_FMAPI_GET_QOS_ALLOCATED_BW 0x06
#define   CXL_FMAPI_SET_QOS_ALLOCATED_BW 0x07
#define   CXL_FMAPI_GET_QOS_BW_LIMIT 0x08
#define   CXL_FMAPI_SET_QOS_BW_LIMIT 0x09

/*
 * CXL 2.0 Table 89 - Errata F24
 */
struct cxl_fmapi_ident_switch_dev_resp_pl {
    uint8_t ingres_port_id;
    uint8_t rsv1;
    uint8_t num_physical_ports;
    uint8_t num_vcs;
    uint8_t active_port_bitmask[32];
    uint8_t active_vcs_bitmask[32];
    uint16_t num_total_vppb;
    uint16_t num_active_vppb;
} QEMU_PACKED;

/*
 * CXL 2.0
 * Table 90 - Get Physical Port State Request Payload
 * Table 91 - Get Physical Port State Response Payload
 * Table 92 - Get Phsyical Port State Port Information Block Format
 */
struct cxl_fmapi_get_phys_port_state_req_pl {
    uint8_t num_ports; /* CHECK. may get too large for MCTP message size */
    uint8_t ports[];
} QEMU_PACKED;

struct cxl_fmapi_port_state_info_block {
    uint8_t port_id;
    uint8_t config_state;
    uint8_t connected_device_cxl_version;
    uint8_t rsv1;
    uint8_t connected_device_type;
    uint8_t port_cxl_version_bitmask;
    uint8_t max_link_width;
    uint8_t negotiated_link_width;
    uint8_t supported_link_speeds_vector;
    uint8_t max_link_speed;
    uint8_t current_link_speed;
    uint8_t ltssm_state;
    uint8_t first_lane_num;
    uint16_t link_state;
    uint8_t supported_ld_count;
} QEMU_PACKED;

struct cxl_fmapi_get_phys_port_state_resp_pl {
    uint8_t num_ports;
    uint8_t rsv1[3];
    struct cxl_fmapi_port_state_info_block ports[];
} QEMU_PACKED;

struct cxl_fmapi_physical_port_state_ctrl_req_pl {
    uint8_t ppb_id;
    uint8_t port_opcode;
} QEMU_PACKED;

struct cxl_fmapi_physical_port_send_config_req_pl {
    uint8_t ppb_id;
    uint8_t otherdata[3];
    uint32_t write_data;
} QEMU_PACKED;

struct cxl_fmapi_physical_port_send_config_rsp_pl {
    uint32_t read_data;
} QEMU_PACKED;

#endif /* CXL_FMAPI_H */
