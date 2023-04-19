/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Emulation of a CXL Switch Fabric Management
 * interface over MCTP over I2C.
 *
 * Copyright (c) 2022 Huawei Technologies.
 */

/*
 * TODO
 * - multiple packet message reception.
 * - EID programing
 * - Bridges
 * - MTU discovery.
 * - Factor out MCTP control from device type specific parts
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"
#include "block/aio.h"
#include "qemu/main-loop.h"
#include "hw/misc/i2c_mctp_cxl_fmapi.h"
#include "hw/cxl/cxl_fmapi.h"

enum states {
    MCTP_I2C_PROCESS_REQUEST,
    MCTP_I2C_START_SEND,
    MCTP_I2C_ACK,
};

#define MCTP_MESSAGE_TYPE_CONTROL 0x00

enum mctp_command_code {
    MCTP_SET_ENDPOINT_ID = 0x1,
    MCTP_GET_ENDPOINT_ID = 0x2,
    MCTP_GET_ENDPOINT_UUID = 0x3,
    MCTP_GET_VERSION = 0x4,
    MCTP_GET_MESSAGE_TYPE_SUPPORT = 0x5,
    MCTP_GET_VDM_SUPPORT = 0x6,
    MCTP_RESOLVE_EPID = 0x7,
    MCTP_ALLOCATE_EP_IDS = 0x8,
    MCTP_ROUTING_INFORMATION_UPDATE = 0x9,
    MCTP_GET_ROUTING_TABLE_ENTRIES = 0xa,
    MCTP_EP_DISCOVERY_PREP = 0xb,
    MCTP_EP_DISCOVERY = 0xc,
    MCTP_DISCOVERY_NOTIFY = 0xd,
    MCTP_GET_NETWORK_ID = 0xe,
    MCTP_QUERY_HOP = 0xf,
    MCTP_RESOLVE_UUID = 0x10,
    MCTP_QUERY_RATE_LIMIT = 0x11,
    MCTP_REQUEST_TX_RATE_LIMIT = 0x12,
    MCTP_UPDATE_RATE_LIMIT = 0x13,
    MCTP_QUERY_SUPPORTED_INTERFACES = 0x14,
};

#define MCTP_SET_EP_ID_OP_SET 0x0
#define MCTP_SET_EP_ID_OP_FORCE 0x1
#define MCTP_SET_EP_ID_OP_RESET 0x2
#define MCTP_SET_EP_ID_OP_DISCOVERED 0x03

enum mctp_control_comp_code {
    MCTP_COMP_SUCCESS = 0x00,
    MCTP_COMP_ERROR = 0x01,
    MCTP_COMP_ERROR_INVALID_DATA = 0x02,
    MCTP_COMP_ERROR_INVALID_LENGTH = 0x03,
    MCTP_COMP_ERROR_NOT_READY = 0x04,
    MCTP_COMP_ERROR_UNSUPPORTED_CMD = 0x05,
    /* 0x80 to 0xff command specific */
};

#define NUM_PORTS 10

/* TODO split out specific stuff from MCTP generic */
struct I2C_MCTP_CXL_SWITCHState {
    I2CSlave i2c;
    I2CBus *bus;
    int len;
    bool eid_set;
    uint8_t my_eid;
    uint8_t byte_count;
    uint8_t source_slave_addr;
    uint8_t dest_eid;
    uint8_t src_eid;
    uint8_t tag;
    uint8_t message_type;
    union {
        struct {
            uint8_t rq_bit;
            uint8_t d_bit;
            uint8_t instance_id;
            enum mctp_command_code command_code;
            //completion only in response.
            //for now
        } control;
        struct {
            uint8_t tag; // another one?
            uint8_t command;
            uint8_t command_set;
            uint32_t payloadlength;
            uint16_t vendor_specific;
            union {
                struct {
                    int num_ports_req;
                    uint8_t ports_req[NUM_PORTS];
                } get_physical_port_state;
            };
            //rest is payload.
        } cxl_fmapi;
        //other message types.
    };
    enum states state;
    QEMUBH *bh;
    uint8_t send_buf[128];
};

OBJECT_DECLARE_SIMPLE_TYPE(I2C_MCTP_CXL_SWITCHState, I2C_MCTP_CXL_SWITCH)

static void mctp_command_set_eid_parse(I2C_MCTP_CXL_SWITCHState *s, uint8_t data)
{
    switch (s->len) {
    case 10:
        printf("Set end point ID message content %x\n", data & 0x3);
                    break;
    case 11:
        printf("Set Endpoint ID to %x\n", data);
        s->eid_set = true;
        s->my_eid = data;
        break;
    default:
        /* Will happen due to PEC */
    }
}

static void mctp_command_get_version_parse(I2C_MCTP_CXL_SWITCHState *s, uint8_t data)
{
    switch (s->len) {
    case 10:
        if (data != 0xFF)
            printf("Unsupported version request\n");
        break;
    default:
        /* Will happen due to PEC */
        break;
    }
}

static void mctp_command_resolve_epid_parse(I2C_MCTP_CXL_SWITCHState *s,
                                            uint8_t data)
{
    /* Very odd if this gets sent to an EP */
    switch (s->len) {
    case 10:
        printf("ID for which a resolve is requested %x\n", data);
        break;
    default:
        /* Will happen due to PEC */
        break;
    }
}

static void cxl_fmapi_cmd_set_physical_switch_parse(I2C_MCTP_CXL_SWITCHState *s,
                                                    uint8_t data)
{
    switch (s->cxl_fmapi.command) {
    case CXL_FMAPI_PHYSICAL_SWITCH_IDENTIFY_SWITCH:
        break;
    case CXL_FMAPI_GET_PHYSICAL_PORT_STATE:
        switch (s->len) {
        case 20:
            s->cxl_fmapi.get_physical_port_state.num_ports_req = data;
            break;
        default:
            if (s->len >= 21 && s->len < 21 + s->cxl_fmapi.get_physical_port_state.num_ports_req) {
                s->cxl_fmapi.get_physical_port_state.ports_req[s->len - 21] = data;
                break;
            }
            /* Eat anything else - subject to length checks elsewhere */
            break;
        }
        break;
    case CXL_FMAPI_PHYSICAL_PORT_CONTROL:
        /* Emulation so no need to implement it ;) */
        switch (s->len) {
        case 20:
            printf("ppb id : %d\n", data);
            break;
        case 21:
            printf("op code : %d\n", data);
            break;
        }
        break;
    case CXL_FMAPI_SEND_PPB_CXLIO_CONFIG_REQ:
        /* Implement this at some point */
        break;
    default:
        printf("command not handled %d\n", s->cxl_fmapi.command);
        break;
    }
}

static int i2c_mctp_cxl_switch_tx(I2CSlave *i2c, uint8_t data)
{
    /* Will need to decode */
    I2C_MCTP_CXL_SWITCHState *s = I2C_MCTP_CXL_SWITCH(i2c);

    switch (s->len) {
    case 0:
        if (data != 0xf) {
            printf("not an MCTP message\n");
        }
        break;
    case 1:
        s->byte_count = data;
        break;
    case 2:
        s->source_slave_addr = data >> 1;
        break;
    case 3:
        if ((data & 0xF) != 1)
            printf("not MCTP 1.0\n");
        break;
    case 4:
        s->dest_eid = data;
        break;
    case 5:
        s->src_eid = data;
        break;
    case 6:
        s->tag = data & 0x7;
        break;
    case 7:
        s->message_type = data;
        break;
    }
    if (s->len > 7 && s->message_type == 0x00) {
        switch (s->len) {
        case 8:
            /* TODO: Add validity check */
            s->control.rq_bit = (data & 0x80) ? 1 : 0;
            s->control.d_bit = (data & 0x40) ? 1 : 0;
            s->control.instance_id = data & 0x1f;
            break;
        case 9:
            s->control.command_code = data;
            printf("Control command code %x\n", s->control.command_code);
            break;
        }
        if (s->len > 9) {
            switch (s->control.command_code) {
            case MCTP_SET_ENDPOINT_ID:
                mctp_command_set_eid_parse(s, data);
                break;
            case MCTP_GET_VERSION:
                mctp_command_get_version_parse(s, data);
                break;
            case MCTP_RESOLVE_EPID:
                mctp_command_resolve_epid_parse(s, data);
                break;
            case MCTP_GET_ENDPOINT_ID:
            case MCTP_GET_ENDPOINT_UUID:
            case MCTP_GET_MESSAGE_TYPE_SUPPORT:
            case MCTP_GET_VDM_SUPPORT:
                /* Will happen due to PEC */
                break;
            default:
                printf("not yet handled\n");
                break;
            }
        }
    } if (s->len > 7 && s->message_type == 0x7) {
        //  printf(" byte[%d] of FM_API %x\n", s->len, data);
        switch (s->len) {
        case 8:
            if (data) {
                printf("Not a request?\n");
            }
            break;
        case 9:
            s->cxl_fmapi.tag = data;
            break;
        case 10:
            //reserved
            break;
        case 11:
            s->cxl_fmapi.command = data;
            break;
        case 12:
            s->cxl_fmapi.command_set = data;
            break;
        }
        if (s->len > 19) {
            switch (s->cxl_fmapi.command_set) {
            case CXL_FMAPI_CMD_SET_PHYSICAL_SWITCH:
                cxl_fmapi_cmd_set_physical_switch_parse(s, data);
                break;
            default:
                printf("Not yet handled\n");
            }
        }

    }

    s->len++;

    return 0;
}

static uint8_t i2c_mctp_cxl_switch_rx(I2CSlave *i2c)
{
    return 0;
}

static int i2c_mctp_cxl_switch_event(I2CSlave *i2c, enum i2c_event event)
{
    I2C_MCTP_CXL_SWITCHState *s = I2C_MCTP_CXL_SWITCH(i2c);

    switch (event) {
    case I2C_START_RECV:
        s->len = 0;
        return 0;
    case I2C_START_SEND:
        s->len = 0;
        return 0;
    case I2C_FINISH:
        s->len = 0;
        s->state = MCTP_I2C_PROCESS_REQUEST;
        i2c_bus_master(s->bus, s->bh);
        return 0;
    case I2C_NACK:
    default:
        /* Handle this? */
        return 0;
    }
}

#define POLY    (0x1070U << 3)
static uint8_t crc8(uint16_t data)
{
	int i;

	for (i = 0; i < 8; i++) {
		if (data & 0x8000)
			data = data ^ POLY;
		data = data << 1;
	}
	return (uint8_t)(data >> 8);
}

static uint8_t i2c_smbus_pec(uint8_t crc, uint8_t *p, size_t count)
{
    int i;
                
    for (i = 0; i < count; i++) {
        crc = crc8((crc ^ p[i]) << 8);
    }

    return crc;
}

struct mctp_i2c_head { /* DSP0237 1.2.0 */
    uint8_t slave_addr;
    //Destination slave address not here due to slightly different handling.
    uint8_t command_code;
    uint8_t pl_size;
    uint8_t saddr;
    uint8_t hdr_ver;
    uint8_t dest_eid;
    uint8_t source_eid;
    uint8_t flags;
    uint8_t message_type;
} QEMU_PACKED;

struct mctp_command_head {
    uint8_t instance_id;
    uint8_t command_code;
} QEMU_PACKED;

struct mctp_i2c_cmd_combined_head {
    struct mctp_i2c_head i2c_head;
    struct mctp_command_head command_head;
} QEMU_PACKED;

/* Generic reply setup for control message responses */
static uint8_t mctp_control_set_reply(I2C_MCTP_CXL_SWITCHState *s, size_t pl_size)
{
    I2CSlave *subordinate = I2C_SLAVE(s);
    struct mctp_i2c_cmd_combined_head head = {
        .i2c_head = {
            .slave_addr = s->source_slave_addr << 1,
            .command_code = 0x0f,
            /* Size only includes data after this field */
            .pl_size = sizeof(struct mctp_i2c_cmd_combined_head ) -
            	offsetof(struct mctp_i2c_cmd_combined_head, i2c_head.saddr) +
            	pl_size,
            .saddr = (subordinate->address << 1) | 1,
            .hdr_ver = 0x1,
            .dest_eid = s->src_eid,
            .source_eid = s->my_eid,
            .flags = s->tag | 0x80 | 0x40,
            .message_type = 0x0,
        },
        .command_head = {
            .instance_id = s->control.instance_id,
            .command_code = s->control.command_code,
        },
    };
    
    return ((uint8_t *)&head)[s->len];
}

static void mctp_cmd_send_reply(I2C_MCTP_CXL_SWITCHState *s, uint8_t *buf, uint8_t buf_size)
{
    const int com_head_size = sizeof(struct mctp_i2c_cmd_combined_head);
    uint8_t val;

    if (s->len < com_head_size) {
        val = mctp_control_set_reply(s, buf_size);
    } else if (s->len < com_head_size + buf_size) {
        val = ((uint8_t *)buf)[s->len - com_head_size];
    } else if (s->len == com_head_size + buf_size) {
        val = i2c_smbus_pec(0, s->send_buf, s->len);
    } else if (s->len == com_head_size + buf_size + 1) {
        i2c_end_transfer(s->bus);
        i2c_bus_release(s->bus);
        return;
    } else {
        val = 0;
        printf("bug\n");
    }
    i2c_send_async(s->bus, val);
    s->send_buf[s->len] = val;
    s->len++;
}

static void mctp_eid_set_reply(I2C_MCTP_CXL_SWITCHState *s)
{
    uint8_t buf[] = {
        //completion code
        [0] = 0,
        [1] = 0, //accpeted, no pool.
        [2] = s->my_eid,
        [3] = 0,
    };

    mctp_cmd_send_reply(s, buf, sizeof(buf));
}

static void mctp_eid_get_reply(I2C_MCTP_CXL_SWITCHState *s)
{
    uint8_t buf[] = {
        //completion code
        [0] = 0,
        [1] = s->my_eid,
        [2] = 0, //Simple end point, dynamic EID.
        [3] = 0, //medium specific.
    };

    mctp_cmd_send_reply(s, buf, sizeof(buf));
}

static void mctp_uuid_get_reply(I2C_MCTP_CXL_SWITCHState *s)
{
    uint8_t buf[] = 
        { //completion code
            0,
            // version 4 code from an online generator (Who cares!)
            0xdf, 0x2b, 0xbe, 0xba, 0x73, 0xc6, 0x4e, 0x33, 0x82, 0x5c, 0x98, 0x00, 0x15, 0x8a, 0xc9, 0x2e };

    mctp_cmd_send_reply(s, buf, sizeof(buf));
}

static void mctp_version_get_reply(I2C_MCTP_CXL_SWITCHState *s)
{
    uint8_t buf[] = 
        { //completion code
            [0] = 0,
            [1] = 1, //one entry
            [2] = 0, //alpha
            [3] = 0, //update
            [4] = 3,
            [5] = 1,
        };

    mctp_cmd_send_reply(s, buf, sizeof(buf));
}

static void mctp_message_type_support_reply(I2C_MCTP_CXL_SWITCHState *s)
{
    uint8_t buf[] = 
        { //completion code
            [0] = 0,
            [1] = 2, //entry count
            [2] = 0, //mctp control for now.
            [3] = 0x7, //CXL FM-API from DSP0234
        };

    mctp_cmd_send_reply(s, buf, sizeof(buf));
}

static void mctp_vdm_support_reply(I2C_MCTP_CXL_SWITCHState *s)
{
    uint8_t buf[] = 
        { //completion code
            [0] = 0,
            [1] = 0xff, //one entry so first one is end of list.
            [2] = 0,
            [3] = 0x19, // Huawei
            [4] = 0xe5,
            [5] = 0x0, //Test purposes only.
            [6] = 0x0, 
        };

    mctp_cmd_send_reply(s, buf, sizeof(buf));
}

struct cxl_cci_message_head {
    uint8_t message_category; /* 0..3 only */
    uint8_t tag;
    uint8_t rsv1;
    uint8_t command;
    uint8_t command_set;
    uint8_t pl_length[3]; /* 20 bit little endian, BO bit at bit 23 */
    uint16_t return_code;
    uint16_t vendor_specific;
} QEMU_PACKED;

struct mctp_i2c_cxl_combined_header {
    struct mctp_i2c_head i2c_head;
    struct cxl_cci_message_head cci_head;
} QEMU_PACKED;

static uint8_t mctp_fmapi_set_reply(I2C_MCTP_CXL_SWITCHState *s, uint8_t pl_size)
{
    I2CSlave *subordinate = I2C_SLAVE(s);
    struct mctp_i2c_cxl_combined_header packet_head = {
        .i2c_head = {
            .slave_addr = s->source_slave_addr << 1,
            .command_code = 0x0f,
            /* Size only includes data after this field */
            .pl_size = sizeof(packet_head) -
            	offsetof(struct mctp_i2c_cxl_combined_header, i2c_head.saddr) +
            	pl_size,
            .saddr = (subordinate->address << 1) | 1,
            .hdr_ver = 0x1,
            .dest_eid = s->src_eid,
            .source_eid = s->my_eid,
            .flags = s->tag | 0x80 | 0x40,
            .message_type = 0x7,
        },
        .cci_head = {
            .message_category = 0x1, /* response */
            .tag = s->cxl_fmapi.tag,
            .command = s->cxl_fmapi.command,
            .command_set = s->cxl_fmapi.command_set,
            .pl_length[0] = pl_size & 0xff,
            .pl_length[1] = (pl_size >> 8) & 0xff,
            .pl_length[2] = (pl_size >> 16) & 0xf,
            .return_code = 0,
            .vendor_specific = 0xbeef,
        },
    };

    return ((uint8_t *)&packet_head)[s->len];
}

static void cxl_fmapi_reply(I2C_MCTP_CXL_SWITCHState *s, uint8_t *pl, size_t pl_size)
{
    const int com_head_size = sizeof(struct mctp_i2c_cxl_combined_header);
    uint8_t val;

    if (s->len < com_head_size) {
        val = mctp_fmapi_set_reply(s, pl_size);
    } else if (s->len < com_head_size + pl_size) {
        val = ((uint8_t *)pl)[s->len - com_head_size];
    } else if (s->len == com_head_size + pl_size) {
        val = i2c_smbus_pec(0, s->send_buf, s->len);
    } else if (s->len == com_head_size + pl_size + 1) {
        i2c_end_transfer(s->bus);
        i2c_bus_release(s->bus);
        return;
    } else {
        printf("bug\n");
        return;
    }
    i2c_send_async(s->bus, val);
    s->send_buf[s->len] = val;
    s->len++;
}

static void cxl_physical_port_state_reply(I2C_MCTP_CXL_SWITCHState *s)
{
    struct cxl_fmapi_get_phys_port_state_resp_pl *pl;
    size_t pl_size = sizeof(*pl) +
        sizeof(pl->ports[0]) * s->cxl_fmapi.get_physical_port_state.num_ports_req;
    int i;

    /* Todo this is characteristic of switch - no need to keep reallocating */
    pl = g_malloc0(pl_size);
    if (!pl)
        printf("failed to allocate\n");

    pl->num_ports = s->cxl_fmapi.get_physical_port_state.num_ports_req;
    for (i = 0; i < pl->num_ports; i++) {
        struct cxl_fmapi_port_state_info_block *port;
        port = &pl->ports[i];
        port->port_id = s->cxl_fmapi.get_physical_port_state.ports_req[i];
        if (port->port_id < 2) { /* 2 upstream ports */
            port->config_state = 4;
            port->connected_device_type = 0;
        } else { /* remainder downstream ports */
            port->config_state = 3; 
            port->connected_device_type = 4; /* CXL type 3 */
            port->supported_ld_count = 3;
        }
        port->connected_device_cxl_version = 2;
        port->port_cxl_version_bitmask = 0x2;
        port->max_link_width = 0x10; /* x16 */
        port->negotiated_link_width = 0x10;
        port->supported_link_speeds_vector = 0x1c; /* 8, 16, 32 GT/s */
        port->max_link_speed = 5;
        port->current_link_speed = 5; /* 32 */
        port->ltssm_state = 0x7; /* L2 */
        port->first_lane_num = 0;
        port->link_state = 0;
    }

    cxl_fmapi_reply(s, (uint8_t *)pl, pl_size);
    g_free(pl);
}

static void cxl_physical_switch_identify_reply(I2C_MCTP_CXL_SWITCHState *s)
{
    const struct cxl_fmapi_ident_switch_dev_resp_pl pl = {
        .ingres_port_id = 0,
        .num_physical_ports = 10,
        .num_vcs = 2,
        .active_port_bitmask[0] = 0xff,
        .active_port_bitmask[1] = 0x3,
        .active_vcs_bitmask[0] = 0x3,
        .num_total_vppb = 128,
        .num_active_vppb = 8,
    };
    
    cxl_fmapi_reply(s, (uint8_t *)&pl, sizeof(pl));
}

static void cxl_physical_switch_reply(I2C_MCTP_CXL_SWITCHState *s)
{
    switch (s->cxl_fmapi.command) {
    case CXL_FMAPI_PHYSICAL_SWITCH_IDENTIFY_SWITCH:
        cxl_physical_switch_identify_reply(s);
        break;
    case CXL_FMAPI_GET_PHYSICAL_PORT_STATE:
        cxl_physical_port_state_reply(s);
        break;
    default:
        printf("NOT IMP YET\n");
        break;
    }
}

static void mctp_bh(void *opaque)
{
    I2C_MCTP_CXL_SWITCHState *s = opaque;

    switch (s->state) {
    case MCTP_I2C_PROCESS_REQUEST:
        s->state = MCTP_I2C_START_SEND;
//        return;
        //fallthrough
    case MCTP_I2C_START_SEND:
        i2c_start_send_async(s->bus, s->source_slave_addr);
        s->send_buf[s->len] = s->source_slave_addr << 1;
        s->len++;
        s->state = MCTP_I2C_ACK;
        return;

    case MCTP_I2C_ACK:
        if (s->message_type == MCTP_MESSAGE_TYPE_CONTROL) {
            switch (s->control.command_code) {
            case MCTP_SET_ENDPOINT_ID:
                mctp_eid_set_reply(s);
                break;
            case MCTP_GET_ENDPOINT_ID:
                mctp_eid_get_reply(s);
                break;
            case MCTP_GET_ENDPOINT_UUID:
                mctp_uuid_get_reply(s);
                break;
            case MCTP_GET_VERSION:
                //untested so far.
                mctp_version_get_reply(s);
                break;
            case MCTP_GET_MESSAGE_TYPE_SUPPORT:
                mctp_message_type_support_reply(s);
                break;
            case MCTP_GET_VDM_SUPPORT:
                mctp_vdm_support_reply(s);
                break;
            default:
                printf("Unknown message\n");
                break;
            }
        } else if (s->message_type == 0x7) {

            switch (s->cxl_fmapi.command_set) {
            case CXL_FMAPI_CMD_SET_PHYSICAL_SWITCH:
                cxl_physical_switch_reply(s);
                break;
            default:
                printf("not sure how to reply yet\n");
                break;
            }
           
        }
        return;
    }  
}

static void i2c_mctp_cxl_switch_realize(DeviceState *dev, Error **errp)
{
    I2C_MCTP_CXL_SWITCHState *s = I2C_MCTP_CXL_SWITCH(dev);
    BusState *bus = qdev_get_parent_bus(dev);
    
    s->bh = qemu_bh_new(mctp_bh, s);
    s->bus = I2C_BUS(bus);
}

static void i2c_mctp_cxl_switch_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->realize = i2c_mctp_cxl_switch_realize;
    k->event = i2c_mctp_cxl_switch_event;
    k->recv = i2c_mctp_cxl_switch_rx;
    k->send = i2c_mctp_cxl_switch_tx;
}

static const TypeInfo i2c_mctp_cxl_switch_info = {
    .name = TYPE_I2C_MCTP_CXL_SWITCH,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(I2C_MCTP_CXL_SWITCHState),
    .class_init = i2c_mctp_cxl_switch_class_init,
};

static void i2c_mctp_cxl_switch_register_types(void)
{
    type_register_static(&i2c_mctp_cxl_switch_info);
}

type_init(i2c_mctp_cxl_switch_register_types)
