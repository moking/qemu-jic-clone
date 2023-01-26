
#include "qemu/osdep.h"
#include "qapi/qapi-commands-cxl.h"

void qmp_cxl_inject_gen_media_event(const char *path, uint8_t log,
                                    uint8_t flags, uint64_t physaddr,
                                    uint8_t descriptor, uint8_t type,
                                    uint8_t transaction_type,
                                    bool has_channel, uint8_t channel,
                                    bool has_rank, uint8_t rank,
                                    bool has_device, uint32_t device,
                                    const char *component_id,
                                    Error **errp) {}

void qmp_cxl_inject_poison(const char *path, uint64_t start, uint64_t length,
                           Error **errp) {}

void qmp_cxl_inject_uncorrectable_errors(const char *path,
                                         CXLUncorErrorRecordList *errors,
                                         Error **errp) {}

void qmp_cxl_inject_correctable_error(const char *path, CxlCorErrorType type,
                                      Error **errp) {}
