//
// Created by 纪文铖 on 2026/4/9.
//

#include <stdint.h>
#include "ff_spdk_if.h"

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk_internal/nvme_util.h"
#include "spdk/vmd.h"
#include "spdk/nvme_zns.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"

struct nvme_consumer {
    ff_nvme_cons_ns_fn_t	ns_fn;
    ff_nvme_cons_ctrlr_fn_t	ctrlr_fn;
    ff_nvme_cons_async_fn_t	async_fn;
    ff_nvme_cons_fail_fn_t	fail_fn;
};

struct ff_sequence {
    struct spdk_nvme_qpair	*qpair;
    ff_done_fn_t ff_done;
    void *arg;
    int error;
    int	is_completed;
};

struct nvme_consumer nvme_consumer;
static struct spdk_nvme_transport_id ff_trid = {};


uint32_t
ff_nvme_ns_get_flags(void *ns)
{
    return spdk_nvme_ns_get_flags((struct spdk_nvme_ns *)ns);
}

uint32_t
ff_nvme_ns_get_sector_size(void *ns)
{
    return spdk_nvme_ns_get_sector_size((struct spdk_nvme_ns *)ns);
}

uint64_t
ff_nvme_ns_get_num_sectors(void *ns)
{
    return spdk_nvme_ns_get_num_sectors((struct spdk_nvme_ns *)ns);
}

uint64_t
ff_nvme_ns_get_size(void *ns)
{
    return spdk_nvme_ns_get_size((struct spdk_nvme_ns *)ns);
}

uint32_t
ff_nvme_ns_get_max_io_xfer_size(void *ns)
{
    return spdk_nvme_ctrlr_get_max_xfer_size(spdk_nvme_ns_get_ctrlr((struct spdk_nvme_ns *)ns));
}

const char *
ff_nvme_ns_get_serial_number(void *ns)
{
    const struct spdk_nvme_ctrlr_data *cdata;
    cdata = spdk_nvme_ctrlr_get_data(spdk_nvme_ns_get_ctrlr((struct spdk_nvme_ns *)ns));

    return ((const char *)cdata->sn);
}

const char *
ff_nvme_ns_get_model_number(void *ns)
{
    const struct spdk_nvme_ctrlr_data *cdata;
    cdata = spdk_nvme_ctrlr_get_data(spdk_nvme_ns_get_ctrlr((struct spdk_nvme_ns *)ns));

    return ((const char *)cdata->mn);
}

const void *
ff_nvme_ns_get_data(void *ns)
{
    return spdk_nvme_ns_get_data((struct spdk_nvme_ns *)ns);
}

uint32_t
ff_nvme_ns_get_stripesize(void *ns)
{
    struct spdk_nvme_ns_data *data = (struct spdk_nvme_ns_data *)spdk_nvme_ns_get_data((struct spdk_nvme_ns *)ns);

    if (data->nsfeat.optperf && data->npwg ) {
        return ((data->npwg + 1) * spdk_nvme_ns_get_sector_size((struct spdk_nvme_ns *)ns));
    }

    return spdk_nvme_ns_get_optimal_io_boundary((struct spdk_nvme_ns *)ns) * spdk_nvme_ns_get_sector_size((struct spdk_nvme_ns *)ns);
}

uint16_t
ff_pci_get_vendor(void *dev)
{
    return spdk_pci_device_get_vendor_id(dev);
}

uint16_t
ff_pci_get_device(void *dev)
{
    return spdk_pci_device_get_device_id(dev);
}

uint16_t
ff_pci_get_subvendor(void *dev)
{
    return spdk_pci_device_get_subvendor_id(dev);
}

uint16_t
ff_pci_get_subdevice(void *dev)
{
    return spdk_pci_device_get_subdevice_id(dev);
}

static void
nvme_notify(struct nvme_consumer *cons,
        struct spdk_nvme_ns *ns)
{
    void *ctrlr_cookie = NULL;

    if (cons->ctrlr_fn != NULL)
        ctrlr_cookie = (*cons->ctrlr_fn)(spdk_nvme_ns_get_ctrlr((struct spdk_nvme_ns *)ns));
    // 遍历namespace
    //ns->data = struct spdk_nvme_ns ->nsdata;
    if (cons->ns_fn != NULL)
        (*cons->ns_fn)(ns, ctrlr_cookie);

    return;
}

static void
ff_nvme_ns_bio_done(void *arg, const struct spdk_nvme_cpl *completion)
{
    struct ff_sequence *sequence = arg;

    sequence->is_completed = 1;

    if (spdk_nvme_cpl_is_error(completion)) {
        spdk_nvme_qpair_print_completion(sequence->qpair, (struct spdk_nvme_cpl *)completion);
        fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
        fprintf(stderr, "Read I/O failed, aborting run\n");
        sequence->error = 1;
        //exit(1);
    }
    sequence->ff_done(sequence->arg);
}

#define BIO_READ	0x01	/* Read I/O data */
#define BIO_WRITE	0x02	/* Write I/O data */

int
ff_nvme_ns_bio_process(void *ns,uint16_t cmd,void *data,uint64_t lba,uint64_t lba_count,ff_done_fn_t cb_fn,void *arg)
{
    //struct nvme_dsm_range	*dsm_range;
    int	err;
    struct spdk_nvme_ctrlr	*ctrlr = spdk_nvme_ns_get_ctrlr((struct spdk_nvme_ns *)ns);
    struct spdk_nvme_qpair	*qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);

    struct ff_sequence sequence;

    sequence.qpair = qpair;
    sequence.ff_done = cb_fn;
    sequence.arg = arg;
    sequence.error = 0;
    sequence.is_completed = 0;

    switch (cmd) {
        case BIO_READ:
            err = spdk_nvme_ns_cmd_read(ns, qpair, data,lba,lba_count,
                                        ff_nvme_ns_bio_done, &sequence, 0);
            break;
        case BIO_WRITE:
            err = spdk_nvme_ns_cmd_write(ns, qpair, data,lba,lba_count,
                                        ff_nvme_ns_bio_done, &sequence, 0);
            break;
        /*
        case BIO_FLUSH:
            err = nvme_ns_cmd_flush(ns, nvme_ns_bio_done, bp);
            break;
        case BIO_DELETE:
            dsm_range =
                malloc(sizeof(struct nvme_dsm_range), M_NVME,
                M_ZERO | M_WAITOK);
            if (!dsm_range) {
                err = ENOMEM;
                break;
            }
            dsm_range->length = htole32(lba);
            dsm_range->starting_lba = htole64(lba_count);
            bp->bio_driver2 = dsm_range;
            err = nvme_ns_cmd_deallocate(ns, dsm_range, 1,
                nvme_ns_bio_done, bp);
            if (err != 0)
                free(dsm_range, M_NVME);
            */
            break;
        default:
            err = ENOTSUP;
            break;
    }

    while (!sequence.is_completed) {
        spdk_nvme_qpair_process_completions(qpair, 0);
    }

    spdk_nvme_ctrlr_free_io_qpair(qpair);

    return (err);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
     struct spdk_nvme_ctrlr_opts *opts)
{
    printf("Attaching to %s\n", trid->traddr);

    return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
      struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
    int nsid;
    struct spdk_nvme_ns *ns;

    /*
     * Each controller has one or more namespaces.  An NVMe namespace is basically
     *  equivalent to a SCSI LUN.  The controller's IDENTIFY data tells us how
     *  many namespaces exist on the controller.  For Intel(R) P3X00 controllers,
     *  it will just be one namespace.
     *
     * Note that in NVMe, namespace IDs start at 1, not 0.
     */
    for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
         nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
        ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
        if (ns == NULL) {
            continue;
        }
        nvme_notify(cb_ctx, ns);
    }
}

struct nvme_consumer *
ff_nvme_register_consumer(ff_nvme_cons_ns_fn_t ns_fn, ff_nvme_cons_ctrlr_fn_t ctrlr_fn,
               ff_nvme_cons_async_fn_t async_fn,
               ff_nvme_cons_fail_fn_t fail_fn)
{
    nvme_consumer.ns_fn = ns_fn;
    nvme_consumer.ctrlr_fn = ctrlr_fn;
    nvme_consumer.async_fn = async_fn;
    nvme_consumer.fail_fn = fail_fn;

    spdk_nvme_trid_populate_transport(&ff_trid, SPDK_NVME_TRANSPORT_PCIE);
    snprintf(ff_trid.subnqn, sizeof(ff_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

    int rc = spdk_nvme_probe(&ff_trid, &nvme_consumer, probe_cb, attach_cb, NULL);
    if (rc != 0) {
        fprintf(stderr, "spdk_nvme_probe() failed\n");
        rc = 1;
        return NULL;
    }

    return (&nvme_consumer);
}

void
ff_nvme_unregister_consumer(struct nvme_consumer *consumer)
{

}

void *
ff_vmem_alloc(uint64_t size)
{
    return spdk_zmalloc(size, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
}

void
ff_vmem_free(void *addr)
{
    spdk_free(addr);
}
