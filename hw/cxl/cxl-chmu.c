/*
 * CXL Hotness Monitoring Unit
 *
 * Copyright(C) 2024 Huawei
 *
 * This work is licensed under the terms of the GNU GPL, version 2. See the
 * COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/guest-random.h"
#include "hw/cxl/cxl.h"
#include "hw/cxl/cxl_chmu.h"

#include "hw/pci/msi.h"
#include "hw/pci/msix.h"

#define CHMU_HOTLIST_LENGTH 1024

enum chmu_consumer_request {
    QUERY_TAIL,
    QUERY_HEAD,
    SET_HEAD,
    SET_HOTLIST_SIZE,
    QUERY_HOTLIST_ENTRY,
    SIGNAL_EPOCH_END,
    SET_ENABLED,
    SET_NUMBER_GRANUALS,
    SET_HPA_BASE,
    SET_HPA_SIZE,
};

static int chmu_send(CHMUState *chmu, uint64_t instance,
                     enum chmu_consumer_request command,
                     uint64_t param, uint64_t *response)
{
    uint64_t request[3] = { instance, command, param };
    uint64_t temp;
    uint64_t *reply = response ?: &temp;
    int rc;

    send(chmu->socket, request, sizeof(request), 0);
    rc = recv(chmu->socket, reply, sizeof(*reply), 0);
    if (rc < sizeof(reply)) {
        return -1;
    }
    return 0;
}

static uint64_t chmu_read(void *opaque, hwaddr offset, unsigned size)
{
    CHMUState *chmu = opaque;
    CHMUInstance *chmui;
    uint64_t val = 0;
    hwaddr chmu_stride = A_CXL_CHMU1_CAP0 - A_CXL_CHMU0_CAP0;
    int instance = 0;
    int rc;

    if (offset >= A_CXL_CHMU0_CAP0) {
        instance = (offset - A_CXL_CHMU0_CAP0) / chmu_stride;
        /*
         * Offset allows register defs for CHMU instance 0 to be used
         * for all instances. Includes common cap.
         */
        offset -= chmu_stride * instance;
    }

    if (instance >= CXL_CHMU_INSTANCES_PER_BLOCK) {
        return 0;
    }

    chmui = &chmu->inst[instance];
    switch (offset) {
    case A_CXL_CHMU_COMMON_CAP0:
        val = FIELD_DP64(val, CXL_CHMU_COMMON_CAP0, VERSION, 1);
        val = FIELD_DP64(val, CXL_CHMU_COMMON_CAP0, NUM_INSTANCES,
                         CXL_CHMU_INSTANCES_PER_BLOCK);
        break;
    case A_CXL_CHMU_COMMON_CAP1:
        val = FIELD_DP64(val, CXL_CHMU_COMMON_CAP1, INSTANCE_LENGTH,
                         A_CXL_CHMU1_CAP0 - A_CXL_CHMU0_CAP0);
        break;
    case A_CXL_CHMU0_CAP0:
        val = FIELD_DP64(val, CXL_CHMU0_CAP0, MSI_N, chmui->msi_n);
        val = FIELD_DP64(val, CXL_CHMU0_CAP0, OVERFLOW_INT, 1);
        val = FIELD_DP64(val, CXL_CHMU0_CAP0, LEVEL_INT, 1);
        val = FIELD_DP64(val, CXL_CHMU0_CAP0, EPOCH_TYPE,
                         CXL_CHMU0_CAP0_EPOCH_TYPE_GLOBAL);
        val = FIELD_DP64(val, CXL_CHMU0_CAP0, TRACKED_M2S_REQ_NONTEE_R, 1);
        val = FIELD_DP64(val, CXL_CHMU0_CAP0, TRACKED_M2S_REQ_NONTEE_W, 1);
        val = FIELD_DP64(val, CXL_CHMU0_CAP0, TRACKED_M2S_REQ_NONTEE_RW, 1);
        /* No emulation of TEE modes yet so don't pretend to support them */
        val = FIELD_DP64(val, CXL_CHMU0_CAP0, MAX_EPOCH_LENGTH_SCALE,
                         CXL_CHMU_EPOCH_LENGTH_SCALE_1SEC);
        val = FIELD_DP64(val, CXL_CHMU0_CAP0, MAX_EPOCH_LENGTH_VAL, 100);
        val = FIELD_DP64(val, CXL_CHMU0_CAP0, MIN_EPOCH_LENGTH_SCALE,
                         CXL_CHMU_EPOCH_LENGTH_SCALE_100MSEC);
        val = FIELD_DP64(val, CXL_CHMU0_CAP0, MIN_EPOCH_LENGTH_VAL, 1);
        val = FIELD_DP64(val, CXL_CHMU0_CAP0, HOTLIST_SIZE,
                         CXL_HOTLIST_ENTRIES);
        break;
    case A_CXL_CHMU0_CAP1:
        /* 4KiB and 8KiB only */
        val = FIELD_DP64(val, CXL_CHMU0_CAP1, UNIT_SIZES, BIT(4) | BIT(5));
        /* Only support downsamp by 32 */
        val = FIELD_DP64(val, CXL_CHMU0_CAP1, DOWN_SAMPLING_FACTORS, BIT(5));
        val = FIELD_DP64(val, CXL_CHMU0_CAP1, FLAGS_EPOCH_BASED, 1);
        val = FIELD_DP64(val, CXL_CHMU0_CAP1, FLAGS_ALWAYS_ON, 0);
        val = FIELD_DP64(val, CXL_CHMU0_CAP1, FLAGS_RANDOMIZED_DOWN_SAMPLING,
                         1);
        val = FIELD_DP64(val, CXL_CHMU0_CAP1, FLAGS_OVERLAPPING_ADDRESS_RANGES,
                         1);
        val = FIELD_DP64(val, CXL_CHMU0_CAP1, FLAGS_INSERT_AFTER_CLEAR, 0);
        break;
    case A_CXL_CHMU0_CAP2:
        val = FIELD_DP64(val, CXL_CHMU0_CAP2, BITMAP_REG_OFFSET,
                         A_CXL_CHMU0_RANGE_CONFIG_BITMAP0 - A_CXL_CHMU0_CAP0);
        break;
    case A_CXL_CHMU0_CAP3:
        val = FIELD_DP64(val, CXL_CHMU0_CAP3, HOTLIST_REG_OFFSET,
                         A_CXL_CHMU0_HOTLIST0 - A_CXL_CHMU0_CAP0);
        break;
    case A_CXL_CHMU0_STATUS:
        val = FIELD_DP64(val, CXL_CHMU0_STATUS, STATUS_ENABLED,
                         chmui->enabled ? 1 : 0);
        val = FIELD_DP64(val, CXL_CHMU0_STATUS, OPERATION_IN_PROG,
                         chmui->op_in_prog);
        val = FIELD_DP64(val, CXL_CHMU0_STATUS, COUNTER_WIDTH, 16);
        val = FIELD_DP64(val, CXL_CHMU0_STATUS, OVERFLOW_INT,
                         chmui->overflow_set ? 1 : 0);
        val = FIELD_DP64(val, CXL_CHMU0_STATUS, LEVEL_INT,
                         chmui->fill_thresh_set ? 1 : 0);
        break;
    case A_CXL_CHMU0_TAIL:
        if (chmu->socket) {
            rc = chmu_send(chmu, instance, QUERY_TAIL, 0, &val);
            if (rc < 0) {
                printf("Failed to read tail\n");
                return 0;
            }
        } else {
            val = chmui->tail;
        }
        break;
    case A_CXL_CHMU0_HEAD:
        if (chmu->socket) {
            rc = chmu_send(chmu, instance, QUERY_HEAD, 0, &val);
            if (rc < 0) {
                printf("Failed to read head\n");
                return 0;
            }
        } else {
            val = chmui->head;
        }
        break;
    case A_CXL_CHMU0_HOTLIST0...(8 * (A_CXL_CHMU0_HOTLIST0 +
                                      CHMU_HOTLIST_LENGTH)):
        if (chmu->socket) {
            rc = chmu_send(chmu, instance, QUERY_HOTLIST_ENTRY,
                           (offset - A_CXL_CHMU0_HOTLIST0) / 8, &val);
            if (rc < 0) {
                printf("Failed to read a hotlist entry\n");
                return 0;
            }
        } else {
            val = chmui->hotlist[(offset - A_CXL_CHMU0_HOTLIST0) / 8];
        }
        break;
    }
    return val;
}

static void chmu_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    CHMUState *chmu = opaque;
    CHMUInstance *chmui;
    hwaddr chmu_stride = A_CXL_CHMU1_CAP0 - A_CXL_CHMU0_CAP0;
    int instance = 0;
    int i, rc;

    if (offset >= A_CXL_CHMU0_CAP0) {
        instance = (offset - A_CXL_CHMU0_CAP0) / chmu_stride;
        /* offset as if in chmu0 so includes the common caps */
        offset -= chmu_stride * instance;
    }
    if (instance >= CXL_CHMU_INSTANCES_PER_BLOCK) {
        return;
    }

    chmui = &chmu->inst[instance];

    switch (offset) {
    case A_CXL_CHMU0_STATUS:
        /* The interrupt fields are RW12C */
        if (FIELD_EX64(value, CXL_CHMU0_STATUS, OVERFLOW_INT)) {
            chmui->overflow_set = false;
        }
        if (FIELD_EX64(value, CXL_CHMU0_STATUS, LEVEL_INT)) {
            chmui->fill_thresh_set = false;
        }
        break;
    case A_CXL_CHMU0_RANGE_CONFIG_BITMAP0...(A_CXL_CHMU0_HOTLIST0 - 8):
        /* TODO - wire this up */
        printf("Bitmap write %lx %lx\n",
               offset - A_CXL_CHMU0_RANGE_CONFIG_BITMAP0, value);
        break;
    case A_CXL_CHMU0_CONF0:
        if (FIELD_EX64(value, CXL_CHMU0_CONF0, CONTROL_ENABLE)) {
            chmui->enabled = true;
            timer_mod(chmui->timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + chmui->epoch_ms);
        } else {
            timer_del(chmui->timer);
            chmui->enabled = false;
        }
        if (chmu->socket) {
            bool enabled = FIELD_EX64(value, CXL_CHMU0_CONF0, CONTROL_ENABLE);

            if (enabled) {
                rc = chmu_send(chmu, instance, SET_HPA_BASE, chmu->base, NULL);
                if (rc < 0) {
                    printf("Failed to set base\n");
                }
                rc = chmu_send(chmu, instance, SET_HPA_SIZE, chmu->size, NULL);
                if (rc < 0) {
                    printf("Failed to set size\n");
                }
            }
            rc = chmu_send(chmu, instance, SET_ENABLED, enabled ? 1 : 0, NULL);
            if (rc < 0) {
                printf("Failed to set enabled\n");
            }
        }

        if (FIELD_EX64(value, CXL_CHMU0_CONF0, CONTROL_RESET)) {
            /* TODO reset counters once implemented */
            chmui->head = 0;
            chmui->tail = 0;
            for (i = 0; i < CXL_HOTLIST_ENTRIES; i++) {
                chmui->hotlist[i] = 0;
            }
        }
        chmui->what =
            FIELD_EX64(value, CXL_CHMU0_CONF0, M2S_REQ_TO_TRACK);
        chmui->int_on_overflow =
            FIELD_EX64(value, CXL_CHMU0_CONF0, FLAGS_INT_ON_OVERFLOW);
        chmui->int_on_fill_thresh =
            FIELD_EX64(value, CXL_CHMU0_CONF0, FLAGS_INT_ON_FILL_THRESH);
        chmui->hotness_thresh =
            FIELD_EX64(value, CXL_CHMU0_CONF0, HOTNESS_THRESHOLD);
        break;
    case A_CXL_CHMU0_CONF1: {
        uint8_t scale;
        uint32_t mult;

        chmui->unit_size = FIELD_EX64(value, CXL_CHMU0_CONF1, UNIT_SIZE);
        chmui->ds_factor =
            FIELD_EX64(value, CXL_CHMU0_CONF1, DOWN_SAMPLING_FACTOR);

        /* TODO: Sanity check value in supported range */
        scale = FIELD_EX64(value, CXL_CHMU0_CONF1, EPOCH_LENGTH_SCALE);
        mult = FIELD_EX64(value, CXL_CHMU0_CONF1, EPOCH_LENGTH_VAL);
        switch (scale) {
            /* TODO: Implement maths, not lookup */
        case 1: /* 100usec */
            chmui->epoch_ms = mult / 10;
            break;
        case 2:
            chmui->epoch_ms = mult;
            break;
        case 3:
            chmui->epoch_ms = mult * 10;
            break;
        case 4:
            chmui->epoch_ms = mult * 100;
            break;
        case 5:
            chmui->epoch_ms = mult * 1000;
            break;
        default:
            /* Unknown value so ignore */
            break;
        }
        break;
    }
    case A_CXL_CHMU0_CONF2:
        chmui->fillthresh = FIELD_EX64(value, CXL_CHMU0_CONF2,
                                      NOTIFICATION_THRESHOLD);
        break;
    case A_CXL_CHMU0_HEAD:
        chmui->head = value;
        if (chmu->socket) {
            rc = chmu_send(chmu, instance, SET_HEAD, value, NULL);
            if (rc < 0) {
                printf("Failed to set head\n");
            }
        }
        break;
    case A_CXL_CHMU0_TAIL: /* Not sure why this is writeable! */
        chmui->tail = value;
        break;
    }
}

static const MemoryRegionOps chmu_ops = {
    .read = chmu_read,
    .write = chmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void chmu_timer_update(void *opaque)
{
    CHMUInstance *chmui = opaque;
    PCIDevice *pdev = PCI_DEVICE(chmui->private);
    int i;
#define entries_to_add  167
    bool interrupt_needed = false;
    bool remote = chmui->parent->socket;

    timer_del(chmui->timer);

    /* This tick is the epoch. How to handle? */
    if (remote) {
        int rc;
        uint64_t reply;
        /* hack instance always 0! */
        rc = chmu_send(chmui->parent, 0, SIGNAL_EPOCH_END, 0, &reply);
        if (rc < 0) {
            printf("Epoch signalling failed\n");
        }

        rc = chmu_send(chmui->parent, 0, QUERY_TAIL, 0, &reply);
        if (rc < 0) {
            printf("failed to read the tail\n");
        }
        chmui->tail = reply;
        printf("after epoch tail is %x\n", chmui->tail);
    } else { /* Fake some data if we don't have a real source */
        uint8_t rand[entries_to_add];

        qemu_guest_getrandom_nofail(rand, sizeof(rand));
        for (i = 0; i < entries_to_add; i++) {
            if ((chmui->tail + 1) % CXL_HOTLIST_ENTRIES == chmui->head) {
                /* Overflow occured, drop out */
                break;
            }
            chmui->hotlist[chmui->tail % CXL_HOTLIST_ENTRIES] =
                (chmui->tail << 16) | (chmui->hotness_thresh + rand[i]);
            chmui->tail++;
            chmui->tail %= CXL_HOTLIST_ENTRIES;
        }
    }

    /* All interrupt code is kept in here whatever the data source */
    if (chmui->int_on_fill_thresh && !chmui->fill_thresh_set) {
        if (((chmui->tail > chmui->head) &&
             (chmui->tail - chmui->head > chmui->fillthresh)) |
            ((chmui->tail < chmui->head) &&
             (CXL_HOTLIST_ENTRIES - chmui->head + chmui->tail >
              chmui->fillthresh))) {
            chmui->fill_thresh_set = true;
            interrupt_needed = true;
        }
    }
    if (chmui->int_on_overflow && !chmui->overflow_set) {
        if ((chmui->tail + 1) % CXL_HOTLIST_ENTRIES == chmui->head) {
            chmui->overflow_set = true;
            interrupt_needed = true;
        }
    }

    if (interrupt_needed) {
        if (msix_enabled(pdev)) {
            msix_notify(pdev, chmui->msi_n);
        } else if (msi_enabled(pdev)) {
            msi_notify(pdev, chmui->msi_n);
        }
    }

    timer_mod(chmui->timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + chmui->epoch_ms);
}

int cxl_chmu_register_block_init(Object *obj,
                                 CXLDeviceState *cxl_dstate,
                                 int id, uint8_t msi_n,
                                 Error **errp)
{
    CHMUState *chmu = &cxl_dstate->chmu[id];
    MemoryRegion *registers = &cxl_dstate->chmu_registers[id];
    g_autofree gchar *name = g_strdup_printf("chmu%d-registers", id);
    struct sockaddr_in server_addr;
    int i;

    memory_region_init_io(registers, obj, &chmu_ops, chmu, name,
                          pow2ceil(CXL_CHMU_SIZE));
    memory_region_add_subregion(&cxl_dstate->device_registers,
                                CXL_CHMU_OFFSET(id), registers);

    for (i = 0; i < CXL_CHMU_INSTANCES_PER_BLOCK; i++) {
        CHMUInstance *chmui = &chmu->inst[i];

        chmui->parent = chmu;/* hack */
        chmui->private = obj;
        chmui->msi_n = msi_n + i;
        chmui->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, chmu_timer_update,
                                    chmui);
    }

    if (chmu->port) {
        uint64_t helloval = 41;
        chmu->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (chmu->socket < 0) {
            error_setg(errp, "Failed to create a socket");
            return -1;
        }

        memset((char *)&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        server_addr.sin_port = htons(chmu->port);
        if (connect(chmu->socket, (struct sockaddr *)&server_addr,
                    sizeof(server_addr)) < 0) {
            close(chmu->socket);
            error_setg(errp, "Socket connect failed");
            return -1;
        }

        send(chmu->socket, &helloval, sizeof(helloval), 0);
        for (i = 0; i < CXL_CHMU_INSTANCES_PER_BLOCK; i++) {
            int rc;
            rc = chmu_send(chmu, i, SET_HOTLIST_SIZE,
                           CHMU_HOTLIST_LENGTH, NULL);
            if (rc) {
                error_setg(errp, "Failed to set hotlist size");
                return rc;
            }

            rc = chmu_send(chmu, i, SET_NUMBER_GRANUALS,
                           cxl_dstate->static_mem_size / 4096, NULL);
            if (rc) {
                error_setg(errp, "Failed to set number of granuals");
                return rc;
            }
        }
    }
    return 0;
}
