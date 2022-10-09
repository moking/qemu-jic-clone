#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "hw/mem/memory-device.h"
#include "hw/mem/pc-dimm.h"
#include "hw/pci/pci.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/pmem.h"
#include "qemu/range.h"
#include "qemu/rcu.h"
#include "sysemu/hostmem.h"
#include "sysemu/numa.h"
#include "hw/cxl/cxl.h"
#include "hw/pci/msix.h"
#include "hw/pci/spdm.h"

/*
 * Null value of all Fs suggested by IEEE RA guidelines for use of
 * EU, OUI and CID
 */
#define UI64_NULL ~(0ULL)
#define DWORD_BYTE 4

static int ct3_build_cdat_table(CDATSubHeader ***cdat_table,
                                void *priv)
{
    g_autofree CDATDsmas *dsmas_nonvolatile = NULL;
    g_autofree CDATDslbis *dslbis_nonvolatile = NULL;
    g_autofree CDATDsemts *dsemts_nonvolatile = NULL;
    CXLType3Dev *ct3d = priv;
    int len = 0;
    int i = 0;
    int next_dsmad_handle = 0;
    int nonvolatile_dsmad = -1;
    int dslbis_nonvolatile_num = 4;
    MemoryRegion *mr;

    /* Non volatile aspects */
    if (ct3d->hostmem) {
        dsmas_nonvolatile = g_malloc(sizeof(*dsmas_nonvolatile));
        if (!dsmas_nonvolatile) {
            return -ENOMEM;
        }
        nonvolatile_dsmad = next_dsmad_handle++;
        mr = host_memory_backend_get_memory(ct3d->hostmem);
        if (!mr) {
            return -EINVAL;
        }
        *dsmas_nonvolatile = (CDATDsmas) {
            .header = {
                .type = CDAT_TYPE_DSMAS,
                .length = sizeof(*dsmas_nonvolatile),
            },
            .DSMADhandle = nonvolatile_dsmad,
            .flags = CDAT_DSMAS_FLAG_NV,
            .DPA_base = 0,
            .DPA_length = int128_get64(mr->size),
        };
        len++;

        /* For now, no memory side cache, plausiblish numbers */
        dslbis_nonvolatile = g_malloc(sizeof(*dslbis_nonvolatile) * dslbis_nonvolatile_num);
        if (!dslbis_nonvolatile)
            return -ENOMEM;

        dslbis_nonvolatile[0] = (CDATDslbis) {
            .header = {
                .type = CDAT_TYPE_DSLBIS,
                .length = sizeof(*dslbis_nonvolatile),
            },
            .handle = nonvolatile_dsmad,
            .flags = HMAT_LB_MEM_MEMORY,
            .data_type = HMAT_LB_DATA_READ_LATENCY,
            .entry_base_unit = 10000, /* 10ns base */
            .entry[0] = 15, /* 150ns */
        };
        len++;

        dslbis_nonvolatile[1] = (CDATDslbis) {
            .header = {
                .type = CDAT_TYPE_DSLBIS,
                .length = sizeof(*dslbis_nonvolatile),
            },
            .handle = nonvolatile_dsmad,
            .flags = HMAT_LB_MEM_MEMORY,
            .data_type = HMAT_LB_DATA_WRITE_LATENCY,
            .entry_base_unit = 10000,
            .entry[0] = 25, /* 250ns */
        };
        len++;
       
        dslbis_nonvolatile[2] = (CDATDslbis) {
            .header = {
                .type = CDAT_TYPE_DSLBIS,
                .length = sizeof(*dslbis_nonvolatile),
            },
            .handle = nonvolatile_dsmad,
            .flags = HMAT_LB_MEM_MEMORY,
            .data_type = HMAT_LB_DATA_READ_BANDWIDTH,
            .entry_base_unit = 1000, /* GB/s */
            .entry[0] = 16,
        };
        len++;

        dslbis_nonvolatile[3] = (CDATDslbis) {
            .header = {
                .type = CDAT_TYPE_DSLBIS,
                .length = sizeof(*dslbis_nonvolatile),
            },
            .handle = nonvolatile_dsmad,
            .flags = HMAT_LB_MEM_MEMORY,
            .data_type = HMAT_LB_DATA_WRITE_BANDWIDTH,
            .entry_base_unit = 1000, /* GB/s */
            .entry[0] = 16,
        };
        len++;

        mr = host_memory_backend_get_memory(ct3d->hostmem);
        if (!mr) {
            return -EINVAL;
        }
        dsemts_nonvolatile = g_malloc(sizeof(*dsemts_nonvolatile));
        *dsemts_nonvolatile = (CDATDsemts) {
            .header = {
                .type = CDAT_TYPE_DSEMTS,
                .length = sizeof(*dsemts_nonvolatile),
            },
            .DSMAS_handle = nonvolatile_dsmad,
            .EFI_memory_type_attr = 2, /* Reserved - the non volatile from DSMAS matters */
            .DPA_offset = 0,
            .DPA_length = int128_get64(mr->size),
        };
        len++;
    }

    *cdat_table = g_malloc0(len * sizeof(*cdat_table));
    /* Header always at start of structure */
    if (dsmas_nonvolatile) {
        (*cdat_table)[i++] = g_steal_pointer(&dsmas_nonvolatile);
    }
    if (dslbis_nonvolatile) {
        CDATDslbis *dslbis = g_steal_pointer(&dslbis_nonvolatile);        
        int j;

        for (j = 0; j < dslbis_nonvolatile_num; j++) {
            (*cdat_table)[i++] = (CDATSubHeader *)&dslbis[j];
        }
    }
    if (dsemts_nonvolatile) {
        (*cdat_table)[i++] = g_steal_pointer(&dsemts_nonvolatile);
    }
    
    return len;
}

static void ct3_free_cdat_table(CDATSubHeader **cdat_table, int num, void *priv)
{
    int i;

    for (i = 0; i < num; i++) {
        g_free(cdat_table[i]);
    }
    g_free(cdat_table);
}

static bool cxl_doe_cdat_rsp(DOECap *doe_cap)
{
    CDATObject *cdat = &CXL_TYPE3(doe_cap->pdev)->cxl_cstate.cdat;
    uint16_t ent;
    void *base;
    uint32_t len;
    CDATReq *req = pcie_doe_get_write_mbox_ptr(doe_cap);
    CDATRsp rsp;

    assert(cdat->entry_len);

    /* Discard if request length mismatched */
    if (pcie_doe_get_obj_len(req) <
        DIV_ROUND_UP(sizeof(CDATReq), DWORD_BYTE)) {
        return false;
    }

    ent = req->entry_handle;
    base = cdat->entry[ent].base;
    len = cdat->entry[ent].length;

    rsp = (CDATRsp) {
        .header = {
            .vendor_id = CXL_VENDOR_ID,
            .data_obj_type = CXL_DOE_TABLE_ACCESS,
            .reserved = 0x0,
            .length = DIV_ROUND_UP((sizeof(rsp) + len), DWORD_BYTE),
        },
        .rsp_code = CXL_DOE_TAB_RSP,
        .table_type = CXL_DOE_TAB_TYPE_CDAT,
        .entry_handle = (ent < cdat->entry_len - 1) ?
                        ent + 1 : CXL_DOE_TAB_ENT_MAX,
    };

    memcpy(doe_cap->read_mbox, &rsp, sizeof(rsp));
    memcpy(doe_cap->read_mbox + DIV_ROUND_UP(sizeof(rsp), DWORD_BYTE),
           base, len);

    doe_cap->read_mbox_len += rsp.header.length;

    return true;
}

static bool cxl_doe_compliance_rsp(DOECap *doe_cap)
{
    CXLCompRsp *rsp = &CXL_TYPE3(doe_cap->pdev)->cxl_cstate.compliance.response;
    CXLCompReqHeader *req = pcie_doe_get_write_mbox_ptr(doe_cap);
    uint32_t req_len = 0, rsp_len = 0;
    CXLCompType type = req->req_code;

    switch (type) {
    case CXL_COMP_MODE_CAP:
        req_len = sizeof(CXLCompCapReq);
        rsp_len = sizeof(CXLCompCapRsp);
        rsp->cap_rsp.status = 0x0;
        rsp->cap_rsp.available_cap_bitmask = 0;
        rsp->cap_rsp.enabled_cap_bitmask = 0;
        break;
    case CXL_COMP_MODE_STATUS:
        req_len = sizeof(CXLCompStatusReq);
        rsp_len = sizeof(CXLCompStatusRsp);
        rsp->status_rsp.cap_bitfield = 0;
        rsp->status_rsp.cache_size = 0;
        rsp->status_rsp.cache_size_units = 0;
        break;
    case CXL_COMP_MODE_HALT:
        req_len = sizeof(CXLCompHaltReq);
        rsp_len = sizeof(CXLCompHaltRsp);
        break;
    case CXL_COMP_MODE_MULT_WR_STREAM:
        req_len = sizeof(CXLCompMultiWriteStreamingReq);
        rsp_len = sizeof(CXLCompMultiWriteStreamingRsp);
        break;
    case CXL_COMP_MODE_PRO_CON:
        req_len = sizeof(CXLCompProducerConsumerReq);
        rsp_len = sizeof(CXLCompProducerConsumerRsp);
        break;
    case CXL_COMP_MODE_BOGUS:
        req_len = sizeof(CXLCompBogusWritesReq);
        rsp_len = sizeof(CXLCompBogusWritesRsp);
        break;
    case CXL_COMP_MODE_INJ_POISON:
        req_len = sizeof(CXLCompInjectPoisonReq);
        rsp_len = sizeof(CXLCompInjectPoisonRsp);
        break;
    case CXL_COMP_MODE_INJ_CRC:
        req_len = sizeof(CXLCompInjectCrcReq);
        rsp_len = sizeof(CXLCompInjectCrcRsp);
        break;
    case CXL_COMP_MODE_INJ_FC:
        req_len = sizeof(CXLCompInjectFlowCtrlReq);
        rsp_len = sizeof(CXLCompInjectFlowCtrlRsp);
        break;
    case CXL_COMP_MODE_TOGGLE_CACHE:
        req_len = sizeof(CXLCompToggleCacheFlushReq);
        rsp_len = sizeof(CXLCompToggleCacheFlushRsp);
        break;
    case CXL_COMP_MODE_INJ_MAC:
        req_len = sizeof(CXLCompInjectMacDelayReq);
        rsp_len = sizeof(CXLCompInjectMacDelayRsp);
        break;
    case CXL_COMP_MODE_INS_UNEXP_MAC:
        req_len = sizeof(CXLCompInsertUnexpMacReq);
        rsp_len = sizeof(CXLCompInsertUnexpMacRsp);
        break;
    case CXL_COMP_MODE_INJ_VIRAL:
        req_len = sizeof(CXLCompInjectViralReq);
        rsp_len = sizeof(CXLCompInjectViralRsp);
        break;
    case CXL_COMP_MODE_INJ_ALMP:
        req_len = sizeof(CXLCompInjectAlmpReq);
        rsp_len = sizeof(CXLCompInjectAlmpRsp);
        break;
    case CXL_COMP_MODE_IGN_ALMP:
        req_len = sizeof(CXLCompIgnoreAlmpReq);
        rsp_len = sizeof(CXLCompIgnoreAlmpRsp);
        break;
    case CXL_COMP_MODE_INJ_BIT_ERR:
        req_len = sizeof(CXLCompInjectBitErrInFlitReq);
        rsp_len = sizeof(CXLCompInjectBitErrInFlitRsp);
        break;
    default:
        break;
    }

    /* Discard if request length mismatched */
    if (pcie_doe_get_obj_len(req) < DIV_ROUND_UP(req_len, DWORD_BYTE)) {
        return false;
    }

    /* Common fields for each compliance type */
    rsp->header.doe_header.vendor_id = CXL_VENDOR_ID;
    rsp->header.doe_header.data_obj_type = CXL_DOE_COMPLIANCE;
    rsp->header.doe_header.length = DIV_ROUND_UP(rsp_len, DWORD_BYTE);
    rsp->header.rsp_code = type;
    rsp->header.version = 0x1;
    rsp->header.length = rsp_len;

    memcpy(doe_cap->read_mbox, rsp, rsp_len);

    doe_cap->read_mbox_len += rsp->header.doe_header.length;

    return true;
}

static uint32_t ct3d_config_read(PCIDevice *pci_dev, uint32_t addr, int size)
{
    CXLType3Dev *ct3d = CXL_TYPE3(pci_dev);
    uint32_t val;

    if (pcie_doe_read_config(&ct3d->doe_cdat, addr, size, &val)) {
        return val;
    } else if (pcie_doe_read_config(&ct3d->doe_comp, addr, size, &val)) {
        return val;
    } else if (ct3d->spdm_port &&
               pcie_doe_read_config(&ct3d->doe_spdm, addr, size, &val)) {
        return val;
    }

    return pci_default_read_config(pci_dev, addr, size);
}

static void ct3d_config_write(PCIDevice *pci_dev, uint32_t addr, uint32_t val,
                              int size)
{
    CXLType3Dev *ct3d = CXL_TYPE3(pci_dev);

    if (ct3d->spdm_port) {
        pcie_doe_write_config(&ct3d->doe_spdm, addr, val, size);
    }
    pcie_doe_write_config(&ct3d->doe_cdat, addr, val, size);
    pcie_doe_write_config(&ct3d->doe_comp, addr, val, size);
    pci_default_write_config(pci_dev, addr, val, size);
}

/*
 * Null value of all Fs suggested by IEEE RA guidelines for use of
 * EU, OUI and CID
 */
#define UI64_NULL ~(0ULL)

static void build_dvsecs(CXLType3Dev *ct3d)
{
    CXLComponentState *cxl_cstate = &ct3d->cxl_cstate;
    CXLDVSECRegisterLocator *regloc_dvsec;
    uint8_t *dvsec;
    int i;

    dvsec = (uint8_t *)&(CXLDVSECDevice){
        .cap = 0x1e,
        .ctrl = 0x2,
        .status2 = 0x2,
        .range1_size_hi = ct3d->hostmem->size >> 32,
        .range1_size_lo = (2 << 5) | (2 << 2) | 0x3 |
        (ct3d->hostmem->size & 0xF0000000),
        .range1_base_hi = 0,
        .range1_base_lo = 0,
    };
    cxl_component_create_dvsec(cxl_cstate, CXL2_TYPE3_DEVICE,
                               PCIE_CXL_DEVICE_DVSEC_LENGTH,
                               PCIE_CXL_DEVICE_DVSEC,
                               PCIE_CXL2_DEVICE_DVSEC_REVID, dvsec);

    regloc_dvsec = &(CXLDVSECRegisterLocator){
        .rsvd         = 0,
        .reg_base[0].lo = RBI_COMPONENT_REG | CXL_COMPONENT_REG_BAR_IDX,
        .reg_base[0].hi = 0,
        .reg_base[1].lo = RBI_CXL_DEVICE_REG | CXL_DEVICE_REG_BAR_IDX,
        .reg_base[1].hi = 0,
    };
    for (i = 0; i < CXL_NUM_CPMU_INSTANCES; i++) {
        regloc_dvsec->reg_base[2 + i].lo = CXL_CPMU_OFFSET(i) |
            RBI_CXL_CPMU_REG | CXL_DEVICE_REG_BAR_IDX;
        regloc_dvsec->reg_base[2 + i].hi = 0;
    }
    cxl_component_create_dvsec(cxl_cstate, CXL2_TYPE3_DEVICE,
                               REG_LOC_DVSEC_LENGTH, REG_LOC_DVSEC,
                               REG_LOC_DVSEC_REVID, (uint8_t *)regloc_dvsec);
    dvsec = (uint8_t *)&(CXLDVSECDeviceGPF){
        .phase2_duration = 0x603, /* 3 seconds */
        .phase2_power = 0x33, /* 0x33 miliwatts */
    };
    cxl_component_create_dvsec(cxl_cstate, CXL2_TYPE3_DEVICE,
                               GPF_DEVICE_DVSEC_LENGTH, GPF_DEVICE_DVSEC,
                               GPF_DEVICE_DVSEC_REVID, dvsec);
}

static void hdm_decoder_commit(CXLType3Dev *ct3d, int which)
{
    ComponentRegisters *cregs = &ct3d->cxl_cstate.crb;
    uint32_t *cache_mem = cregs->cache_mem_registers;

    assert(which == 0);

    /* TODO: Sanity checks that the decoder is possible */
    ARRAY_FIELD_DP32(cache_mem, CXL_HDM_DECODER0_CTRL, COMMIT, 0);
    ARRAY_FIELD_DP32(cache_mem, CXL_HDM_DECODER0_CTRL, ERR, 0);

    ARRAY_FIELD_DP32(cache_mem, CXL_HDM_DECODER0_CTRL, COMMITTED, 1);
}

static void ct3d_reg_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    CXLComponentState *cxl_cstate = opaque;
    ComponentRegisters *cregs = &cxl_cstate->crb;
    CXLType3Dev *ct3d = container_of(cxl_cstate, CXLType3Dev, cxl_cstate);
    uint32_t *cache_mem = cregs->cache_mem_registers;
    bool should_commit = false;
    int which_hdm = -1;

    assert(size == 4);
    g_assert(offset < CXL2_COMPONENT_CM_REGION_SIZE);

    switch (offset) {
    case A_CXL_HDM_DECODER0_CTRL:
        should_commit = FIELD_EX32(value, CXL_HDM_DECODER0_CTRL, COMMIT);
        which_hdm = 0;
        break;
    case A_CXL_RAS_UNC_ERR_STATUS:
    case A_CXL_RAS_COR_ERR_STATUS:
    {
        uint32_t rw1c = value;
        uint32_t temp = ldl_le_p((uint8_t *)cache_mem + offset);
        temp &= ~rw1c;
        stl_le_p((uint8_t *)cache_mem + offset, temp);
        return;
    }
    default:
        break;
    }

    stl_le_p((uint8_t *)cache_mem + offset, value);
    if (should_commit) {
        hdm_decoder_commit(ct3d, which_hdm);
    }
}

static bool cxl_setup_memory(CXLType3Dev *ct3d, Error **errp)
{
    DeviceState *ds = DEVICE(ct3d);
    MemoryRegion *mr;
    char *name;

    if (!ct3d->hostmem) {
        error_setg(errp, "memdev property must be set");
        return false;
    }

    mr = host_memory_backend_get_memory(ct3d->hostmem);
    if (!mr) {
        error_setg(errp, "memdev property must be set");
        return false;
    }
    memory_region_set_nonvolatile(mr, true);
    memory_region_set_enabled(mr, true);
    host_memory_backend_set_mapped(ct3d->hostmem, true);

    if (ds->id) {
        name = g_strdup_printf("cxl-type3-dpa-space:%s", ds->id);
    } else {
        name = g_strdup("cxl-type3-dpa-space");
    }
    address_space_init(&ct3d->hostmem_as, mr, name);
    g_free(name);

    ct3d->cxl_dstate.pmem_size = ct3d->hostmem->size;

    if (!ct3d->lsa) {
        error_setg(errp, "lsa property must be set");
        return false;
    }

    return true;
}

static DOEProtocol doe_cdat_prot[] = {
    { CXL_VENDOR_ID, CXL_DOE_TABLE_ACCESS, cxl_doe_cdat_rsp },
    { }
};

static DOEProtocol doe_comp_prot[] = {
    {CXL_VENDOR_ID, CXL_DOE_COMPLIANCE, cxl_doe_compliance_rsp},
    { }
};

static DOEProtocol doe_spdm_prot[] = {
    { PCI_VENDOR_ID_PCI_SIG, PCI_SIG_DOE_CMA, pcie_doe_spdm_rsp },
    { PCI_VENDOR_ID_PCI_SIG, PCI_SIG_DOE_SECURED_CMA, pcie_doe_spdm_rsp },
    { }
};

static void ct3_inject_poison(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    CXLType3Dev *ct3d = CXL_TYPE3(obj);
    CXLPoison *p = g_new0(CXLPoison, 1);
    /* should check if bool is true, but meh */

    p->length = ct3d->poison_length;
    p->start = ct3d->poison_start;

    QLIST_INSERT_HEAD(&ct3d->poison_list, p, node);
}

static void ct3_realize(PCIDevice *pci_dev, Error **errp)
{
    CXLType3Dev *ct3d = CXL_TYPE3(pci_dev);
    CXLComponentState *cxl_cstate = &ct3d->cxl_cstate;
    ComponentRegisters *regs = &cxl_cstate->crb;
    MemoryRegion *mr = &regs->component_registers;
    uint8_t *pci_conf = pci_dev->config;
    unsigned short msix_num = 4;
    int i;

    object_property_add_uint64_ptr(OBJECT(pci_dev), "poison_start",
                                   &ct3d->poison_start,
                                   OBJ_PROP_FLAG_READ | OBJ_PROP_FLAG_WRITE);
    object_property_add_uint64_ptr(OBJECT(pci_dev), "poison_length",
                                   &ct3d->poison_length,
                                   OBJ_PROP_FLAG_READ | OBJ_PROP_FLAG_WRITE);
    object_property_add(OBJECT(pci_dev), "poison_inject", "bool", NULL,
                        ct3_inject_poison, NULL, ct3d);

    if (!cxl_setup_memory(ct3d, errp)) {
        return;
    }

    pci_config_set_prog_interface(pci_conf, 0x10);
    pci_config_set_class(pci_conf, PCI_CLASS_MEMORY_CXL);

    pcie_endpoint_cap_init(pci_dev, 0x80);
    if (ct3d->sn != UI64_NULL) {
        pcie_dev_ser_num_init(pci_dev, 0x100, ct3d->sn);
        cxl_cstate->dvsec_offset = 0x100 + 0x0c;
    } else {
        cxl_cstate->dvsec_offset = 0x100;
    }

    ct3d->cxl_cstate.pdev = pci_dev;
    build_dvsecs(ct3d);

    regs->special_ops = g_new0(MemoryRegionOps, 1);
    regs->special_ops->write = ct3d_reg_write;

    cxl_component_register_block_init(OBJECT(pci_dev), cxl_cstate,
                                      TYPE_CXL_TYPE3);

    pci_register_bar(
        pci_dev, CXL_COMPONENT_REG_BAR_IDX,
        PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64, mr);

    cxl_device_register_block_init(OBJECT(pci_dev), &ct3d->cxl_dstate);
    cxl_cpmu_register_block_init(OBJECT(pci_dev), &ct3d->cxl_dstate, 0, 3);
    cxl_cpmu_register_block_init(OBJECT(pci_dev), &ct3d->cxl_dstate, 1, 3);
    pci_register_bar(pci_dev, CXL_DEVICE_REG_BAR_IDX,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                         PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &ct3d->cxl_dstate.device_registers);

    /* MSI(-X) Initailization */
    msix_init_exclusive_bar(pci_dev, msix_num, 4, NULL);
    for (i = 0; i < msix_num; i++) {
        msix_vector_use(pci_dev, i);
    }

    /* DOE Initailization */
    pcie_doe_init(pci_dev, &ct3d->doe_cdat, 0x190, doe_cdat_prot, true, 0);

    cxl_cstate->cdat.build_cdat_table = ct3_build_cdat_table;
    cxl_cstate->cdat.free_cdat_table = ct3_free_cdat_table;
    cxl_cstate->cdat.private = ct3d;
    cxl_doe_cdat_init(cxl_cstate, errp);
    pcie_doe_init(pci_dev, &ct3d->doe_comp, 0x1b0, doe_comp_prot, true, 1);
    if (ct3d->spdm_port) {
        pcie_doe_init(pci_dev, &ct3d->doe_spdm, 0x1d0, doe_spdm_prot, true, 2);
        ct3d->doe_spdm.socket = spdm_sock_init(ct3d->spdm_port, errp);
    }
}

static void ct3_exit(PCIDevice *pci_dev)
{
    CXLType3Dev *ct3d = CXL_TYPE3(pci_dev);
    CXLComponentState *cxl_cstate = &ct3d->cxl_cstate;
    ComponentRegisters *regs = &cxl_cstate->crb;

    cxl_doe_cdat_release(cxl_cstate);
    spdm_sock_fini(ct3d->doe_spdm.socket);
    g_free(regs->special_ops);
    address_space_destroy(&ct3d->hostmem_as);
}

/* TODO: Support multiple HDM decoders and DPA skip */
static bool cxl_type3_dpa(CXLType3Dev *ct3d, hwaddr host_addr, uint64_t *dpa)
{
    uint32_t *cache_mem = ct3d->cxl_cstate.crb.cache_mem_registers;
    uint64_t decoder_base, decoder_size, hpa_offset;
    uint32_t hdm0_ctrl;
    int ig, iw;

    decoder_base = (((uint64_t)cache_mem[R_CXL_HDM_DECODER0_BASE_HI] << 32) |
                    cache_mem[R_CXL_HDM_DECODER0_BASE_LO]);
    if ((uint64_t)host_addr < decoder_base) {
        return false;
    }

    hpa_offset = (uint64_t)host_addr - decoder_base;

    decoder_size = ((uint64_t)cache_mem[R_CXL_HDM_DECODER0_SIZE_HI] << 32) |
        cache_mem[R_CXL_HDM_DECODER0_SIZE_LO];
    if (hpa_offset >= decoder_size) {
        return false;
    }

    hdm0_ctrl = cache_mem[R_CXL_HDM_DECODER0_CTRL];
    iw = FIELD_EX32(hdm0_ctrl, CXL_HDM_DECODER0_CTRL, IW);
    ig = FIELD_EX32(hdm0_ctrl, CXL_HDM_DECODER0_CTRL, IG);

    *dpa = (MAKE_64BIT_MASK(0, 8 + ig) & hpa_offset) |
        ((MAKE_64BIT_MASK(8 + ig + iw, 64 - 8 - ig - iw) & hpa_offset) >> iw);

    return true;
}

MemTxResult cxl_type3_read(PCIDevice *d, hwaddr host_addr, uint64_t *data,
                           unsigned size, MemTxAttrs attrs)
{
    CXLType3Dev *ct3d = CXL_TYPE3(d);
    uint64_t dpa_offset;
    MemoryRegion *mr;

    /* TODO support volatile region */
    mr = host_memory_backend_get_memory(ct3d->hostmem);
    if (!mr) {
        return MEMTX_ERROR;
    }

    if (!cxl_type3_dpa(ct3d, host_addr, &dpa_offset)) {
        return MEMTX_ERROR;
    }

    if (dpa_offset > int128_get64(mr->size)) {
        return MEMTX_ERROR;
    }

    return address_space_read(&ct3d->hostmem_as, dpa_offset, attrs, data, size);
}

MemTxResult cxl_type3_write(PCIDevice *d, hwaddr host_addr, uint64_t data,
                            unsigned size, MemTxAttrs attrs)
{
    CXLType3Dev *ct3d = CXL_TYPE3(d);
    uint64_t dpa_offset;
    MemoryRegion *mr;

    mr = host_memory_backend_get_memory(ct3d->hostmem);
    if (!mr) {
        return MEMTX_OK;
    }

    if (!cxl_type3_dpa(ct3d, host_addr, &dpa_offset)) {
        return MEMTX_OK;
    }

    if (dpa_offset > int128_get64(mr->size)) {
        return MEMTX_OK;
    }
    return address_space_write(&ct3d->hostmem_as, dpa_offset, attrs,
                               &data, size);
}

static void ct3d_reset(DeviceState *dev)
{
    CXLType3Dev *ct3d = CXL_TYPE3(dev);
    uint32_t *reg_state = ct3d->cxl_cstate.crb.cache_mem_registers;
    uint32_t *write_msk = ct3d->cxl_cstate.crb.cache_mem_regs_write_mask;

    cxl_component_register_init_common(reg_state, write_msk, CXL2_TYPE3_DEVICE);
    cxl_device_register_init_common(&ct3d->cxl_dstate);
}

static Property ct3_props[] = {
    DEFINE_PROP_LINK("memdev", CXLType3Dev, hostmem, TYPE_MEMORY_BACKEND,
                     HostMemoryBackend *),
    DEFINE_PROP_LINK("lsa", CXLType3Dev, lsa, TYPE_MEMORY_BACKEND,
                     HostMemoryBackend *),
    DEFINE_PROP_UINT64("sn", CXLType3Dev, sn, UI64_NULL),
    DEFINE_PROP_STRING("cdat", CXLType3Dev, cxl_cstate.cdat.filename),
    DEFINE_PROP_UINT16("spdm", CXLType3Dev, spdm_port, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static uint64_t get_lsa_size(CXLType3Dev *ct3d)
{
    MemoryRegion *mr;

    mr = host_memory_backend_get_memory(ct3d->lsa);
    return memory_region_size(mr);
}

static void validate_lsa_access(MemoryRegion *mr, uint64_t size,
                                uint64_t offset)
{
    assert(offset + size <= memory_region_size(mr));
    assert(offset + size > offset);
}

static uint64_t get_lsa(CXLType3Dev *ct3d, void *buf, uint64_t size,
                    uint64_t offset)
{
    MemoryRegion *mr;
    void *lsa;

    mr = host_memory_backend_get_memory(ct3d->lsa);
    validate_lsa_access(mr, size, offset);

    lsa = memory_region_get_ram_ptr(mr) + offset;
    memcpy(buf, lsa, size);

    return size;
}

static void set_lsa(CXLType3Dev *ct3d, const void *buf, uint64_t size,
                    uint64_t offset)
{
    MemoryRegion *mr;
    void *lsa;

    mr = host_memory_backend_get_memory(ct3d->lsa);
    validate_lsa_access(mr, size, offset);

    lsa = memory_region_get_ram_ptr(mr) + offset;
    memcpy(lsa, buf, size);
    memory_region_set_dirty(mr, offset, size);

    /*
     * Just like the PMEM, if the guest is not allowed to exit gracefully, label
     * updates will get lost.
     */
}

static CXLPoisonList *get_poison_list(CXLType3Dev *ct3d)
{
    /* This will get more complex  - for now it's a bit pointless */
    return &ct3d->poison_list;
}

static void ct3_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    CXLType3Class *cvc = CXL_TYPE3_CLASS(oc);

    pc->config_write = ct3d_config_write;
    pc->config_read = ct3d_config_read;
    pc->realize = ct3_realize;
    pc->exit = ct3_exit;
    pc->class_id = PCI_CLASS_STORAGE_EXPRESS;
    pc->vendor_id = PCI_VENDOR_ID_INTEL;
    pc->device_id = 0xd93; /* LVF for now */
    pc->revision = 1;

    pc->config_write = ct3d_config_write;
    pc->config_read = ct3d_config_read;

    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    dc->desc = "CXL PMEM Device (Type 3)";
    dc->reset = ct3d_reset;
    device_class_set_props(dc, ct3_props);

    cvc->get_lsa_size = get_lsa_size;
    cvc->get_lsa = get_lsa;
    cvc->set_lsa = set_lsa;
    cvc->get_poison_list = get_poison_list;

}

static const TypeInfo ct3d_info = {
    .name = TYPE_CXL_TYPE3,
    .parent = TYPE_PCI_DEVICE,
    .class_size = sizeof(struct CXLType3Class),
    .class_init = ct3_class_init,
    .instance_size = sizeof(CXLType3Dev),
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CXL_DEVICE },
        { INTERFACE_PCIE_DEVICE },
        {}
    },
};

static void ct3d_registers(void)
{
    type_register_static(&ct3d_info);
}

type_init(ct3d_registers);
