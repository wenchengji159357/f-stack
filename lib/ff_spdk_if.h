//
// Created by 纪文铖 on 2026/4/9.
//

#ifndef _FSTACK_SPDK_IF_H
#define _FSTACK_SPDK_IF_H

typedef void (*ff_done_fn_t)(void *);
typedef void *(*ff_nvme_cons_ns_fn_t)(void *, void *);
typedef void *(*ff_nvme_cons_ctrlr_fn_t)(void *);
typedef void (*ff_nvme_cons_async_fn_t)(void *, void *,uint32_t, void *, uint32_t);
typedef void (*ff_nvme_cons_fail_fn_t)(void *);

uint32_t ff_nvme_ns_get_flags(void *ns);

uint32_t ff_nvme_ns_get_sector_size(void *ns);

uint64_t ff_nvme_ns_get_num_sectors(void *ns);

uint64_t ff_nvme_ns_get_size(void *ns);

uint32_t ff_nvme_ns_get_max_io_xfer_size(void *ns);

const char *ff_nvme_ns_get_serial_number(void *ns);

const char *ff_nvme_ns_get_model_number(void *ns);

const void *ff_nvme_ns_get_data(void *ns);

uint32_t ff_nvme_ns_get_stripesize(void *ns);

uint16_t ff_pci_get_vendor(void *dev);

uint16_t ff_pci_get_device(void *dev);

uint16_t ff_pci_get_subvendor(void *dev);

uint16_t ff_pci_get_subdevice(void *dev);

void ff_nvme_notify_new_consumer(void *cons);

int
ff_nvme_ns_bio_process(void *ns,uint16_t cmd,void *data,uint64_t lba,
                       uint64_t lba_count,ff_done_fn_t cb_fn,void *arg);

struct nvme_consumer *
ff_nvme_register_consumer(ff_nvme_cons_ns_fn_t ns_fn, ff_nvme_cons_ctrlr_fn_t ctrlr_fn,
                          ff_nvme_cons_async_fn_t async_fn,ff_nvme_cons_fail_fn_t fail_fn);

void ff_nvme_unregister_consumer(struct nvme_consumer *consumer);

void *ff_vmem_alloc(uint64_t size);

void ff_vmem_free(void *addr);

int ff_spdk_init(void);

#endif //F_STACK_FF_SPDK_IF_H