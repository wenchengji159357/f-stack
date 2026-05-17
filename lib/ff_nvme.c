//
// Created by 纪文铖 on 2026/4/9.
//

#include <dev/nvme/nvme.h>
#include <dev/nvme/nvme_private.h>
#include "ff_spdk_if.h"

#ifndef NVME_USE_NVD
#define NVME_USE_NVD 1
#endif

int nvme_use_nvd = NVME_USE_NVD;


#if 0
static void
nvme_ns_bio_done(void *arg, const struct nvme_completion *status)
{
    struct bio	*bp = arg;
    nvme_cb_fn_t	bp_cb_fn;

    bp_cb_fn = bp->bio_driver1;

    if (bp->bio_driver2)
        free(bp->bio_driver2, M_NVME);

    if (nvme_completion_is_error(status)) {
        bp->bio_flags |= BIO_ERROR;
        if (bp->bio_error == 0)
            bp->bio_error = EIO;
    }

    if ((bp->bio_flags & BIO_ERROR) == 0)
        bp->bio_resid = 0;
    else
        bp->bio_resid = bp->bio_bcount;

    bp_cb_fn(bp, status);
}
#endif


int
nvme_ns_ioctl_process(struct nvme_namespace *ns, u_long cmd, caddr_t arg,
    int flag, struct thread *td)
{
    return 0;
}

void
nvme_strvis(uint8_t *dst, const uint8_t *src, int dstlen, int srclen)
{
    uint8_t *cur_pos;

    /* Trim leading/trailing spaces, nulls. */
    while (srclen > 0 && src[0] == ' ')
        src++, srclen--;
    while (srclen > 0
        && (src[srclen - 1] == ' ' || src[srclen - 1] == '\0'))
        srclen--;

    while (srclen > 0 && dstlen > 1) {
        cur_pos = dst;

        /* Show '?' for non-printable characters. */
        if (*src < 0x20 || *src >= 0x7F)
            *cur_pos++ = '?';
        else
            *cur_pos++ = *src;
        src++;
        srclen--;
        dstlen -= cur_pos - dst;
        dst = cur_pos;
    }
    *dst = '\0';
}
#if 0
uint32_t
nvme_get_num_segments(uint64_t addr, uint64_t size, uint32_t align)
{
    uint32_t	num_segs, offset, remainder;

    if (align == 0)
        return (1);

    KASSERT((align & (align - 1)) == 0, ("alignment not power of 2\n"));

    num_segs = size / align;
    remainder = size & (align - 1);
    offset = addr & (align - 1);
    if (remainder > 0 || offset > 0)
        num_segs += 1 + (remainder + offset - 1) / align;
    return (num_segs);
}
#endif
int
nvme_ns_dump(struct nvme_namespace *ns, void *virt, off_t offset, size_t len)
{
    return 0;
}