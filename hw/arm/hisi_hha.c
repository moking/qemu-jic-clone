#include "qemu/osdep.h"
#include "hw/register.h"
#include "hw/sysbus.h"
#include "qemu/module.h"
#include "hw/qdev-properties.h"

REG32(HISI_HHA_CTRL, 0x5004)
    FIELD(HISI_HHA_CTRL, EN, 0, 1)
    FIELD(HISI_HHA_CTRL, RANGE, 1, 1)
    FIELD(HISI_HHA_CTRL, TYPE, 2, 2)
REG32(HISI_HHA_START_L, 0x5008)
REG32(HISI_HHA_START_H, 0x500C)
REG32(HISI_HHA_LEN_L, 0x5010)
REG32(HISI_HHA_LEN_H, 0x5014)
    
typedef struct HisiHha {
    SysBusDevice parent_obj;

    MemoryRegion mr;
    bool inflight;
    int checkcount;
    uint64_t base, size;
} HisiHha;
#define TYPE_HISI_HHA "hisi_hha"
OBJECT_DECLARE_SIMPLE_TYPE(HisiHha, HISI_HHA);


static void hisi_hha_write(void *opaque, hwaddr offset,
                           uint64_t value, unsigned int size)
{
    HisiHha *h = opaque;

    if (size != 4) {
        printf("Unexpected write of size %x\n", size);
        return;
    }

    switch (offset / 4) {
    case R_HISI_HHA_CTRL:
        h->inflight = FIELD_EX32(value, HISI_HHA_CTRL, EN);
        if (h->inflight) {
            /* hack relying on kernel driver polling */
            h->checkcount = 4;
            printf("Flushing %lx %lx\n", h->base, h->size);
        }
        break;
    case R_HISI_HHA_START_L:
        h->base = deposit64(h->base, 0, 32, value); 
        break;
    case R_HISI_HHA_START_H:
        h->base = deposit64(h->base, 32, 32, value); 
        break;
    case R_HISI_HHA_LEN_L:
        h->size = deposit64(h->size, 0, 32, value);
        break;
    case R_HISI_HHA_LEN_H:
        h->size = deposit64(h->size, 32, 32, value);
        break;
    default:
        printf("Unknown register? %lx\n", offset);
    }
}

static uint64_t hisi_hha_read(void *opaque, hwaddr offset, unsigned int size)
{
    HisiHha *h = opaque;
    uint64_t value = 0;

    switch (offset / 4) {
    case R_HISI_HHA_CTRL:
        if (--h->checkcount == 0) {
            h->inflight = false;
            printf("Finished flush\n");
        }
        value = FIELD_DP64(value, HISI_HHA_CTRL, EN, h->inflight ? 1 : 0);
        break;
    case R_HISI_HHA_START_L:
        value = extract64(h->base, 0, 32);
        break;
    case R_HISI_HHA_START_H:
        value = extract64(h->base, 32, 32);
        break;
    case R_HISI_HHA_LEN_L:
        value = extract64(h->size, 0, 32);
        break;
    case R_HISI_HHA_LEN_H:
        value = extract64(h->size, 32, 32);
        break;
    default:
        printf("Unknown register? %lx\n", offset);
    }
    return value;
}

static const MemoryRegionOps hisi_hha_ops = {
    .read = hisi_hha_read,
    .write = hisi_hha_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void hisi_hha_realize(DeviceState *dev, Error **errp)
{
    HisiHha *h = HISI_HHA(dev);
    memory_region_init_io(&h->mr, OBJECT(dev), &hisi_hha_ops,
                        h, "hisi_hha", 0x10000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &h->mr);
}

static void hisi_hha_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Hisilicon Hydra Home Agent";
    dc->realize = hisi_hha_realize;
}

static const TypeInfo hisi_hha_info = {
    .name = TYPE_HISI_HHA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(HisiHha),
    .class_init = hisi_hha_class_init,
};

static void hisi_hha_register_types(void)
{
    type_register_static(&hisi_hha_info);
}
type_init(hisi_hha_register_types)
