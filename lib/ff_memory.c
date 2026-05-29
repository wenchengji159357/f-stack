/*
 * Copyright (C) 2017-2021 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_pci.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <rte_launch.h>
#include <rte_ethdev.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_malloc.h>
#include <rte_cycles.h>
#include <rte_timer.h>
#include <rte_thash.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_vfio.h>

#include "ff_dpdk_if.h"
#include "ff_dpdk_pcap.h"
#include "ff_dpdk_kni.h"
#include "ff_config.h"
#include "ff_veth.h"
#include "ff_host_interface.h"
#include "ff_msg.h"
#include "ff_api.h"
#include "ff_memory.h"

#define    PAGE_SIZE            4096
#define    PAGE_SHIFT            12
#define    PAGE_MASK            (PAGE_SIZE - 1)
#define    trunc_page(x)        ((x) & ~PAGE_MASK)
#define    round_page(x)        (((x) + PAGE_MASK) & ~PAGE_MASK)

extern struct rte_mempool *pktmbuf_pool[NB_SOCKETS];
extern struct lcore_conf lcore_conf;

void *ff_mem_get_page();
int ff_mem_free_addr(void *p);

typedef struct _list_manager_s
{
    uint64_t    *ele;
    int        size;
    //int        FreeNum;
    int     top;
}StackList_t;

static StackList_t ff_mpage_ctl = {0};
static uint64_t ff_page_start = (uint64_t)NULL, ff_page_end = (uint64_t)NULL;
static phys_addr_t *ff_mpage_phy = NULL;

static inline void *stklist_pop(StackList_t *p);
static inline int stklist_push(StackList_t * p, uint64_t val);

static int stklist_init(StackList_t*p, int size)
{

    int i = 0;

    if (p==NULL || size<=0){
        return -1;
    }
    p->size = size;
    p->top = 0;
    if ( posix_memalign((void**)&p->ele, sizeof(uint64_t), sizeof(uint64_t)*size) != 0)
        return -2;

    return 0;
}

static inline void *stklist_pop(StackList_t *p)
{
    int head = 0;

    if (p==NULL)
        return NULL;

    if (p->top > 0 ){
        return (void*)p->ele[--p->top];
    }
    else
        return NULL;
}

//id: the id of element to be freed.
//return code: -1: faile;  >=0:OK.
static inline int stklist_push(StackList_t *p,  const uint64_t val){
    int tail = 0;

    if (p==NULL)
        return -1;
    if (p->top < p->size){
        p->ele[p->top++] = val;
        return 0;
    }
    else
        return -1;
}

static inline int stklist_size(StackList_t * p)
{
    return p->size;
}

// set (void*) to rte_mbuf's priv_data.
static inline int ff_mbuf_set_uint64(struct rte_mbuf* p, uint64_t data)
{
    if (rte_pktmbuf_priv_size(p->pool) >= sizeof(uint64_t))
        *((uint64_t*)(p+1)) = data;
    return 0;
}


int ff_mmap_init()
{
    int err = 0;
    int i = 0;
    uint64_t    virt_addr = (uint64_t)NULL;
    phys_addr_t    phys_addr = 0;
    uint64_t    bsd_memsz = (ff_global_cfg.freebsd.mem_size << 20);
    unsigned int bsd_pagesz = 0;

    ff_page_start = (uint64_t)mmap( NULL, bsd_memsz, PROT_READ | PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE, -1, 0);
    if (ff_page_start == (uint64_t)-1){
        rte_panic("ff_mmap_init get ff_page_start failed, err=%d.\n", errno);
        return -1;
    }

    if ( mlock((void*)ff_page_start, bsd_memsz)<0 )    {
        rte_panic("mlock failed, err=%d.\n", errno);
        return -1;
    }
    ff_page_end = ff_page_start + bsd_memsz;
    bsd_pagesz = (bsd_memsz>>12);
    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_USER1, "ff_mmap_init mmap %d pages, %d MB.\n", bsd_pagesz, ff_global_cfg.freebsd.mem_size);
    printf("ff_mmap_init mem[0x%lx:0x%lx]\n", ff_page_start, ff_page_end);

    if (posix_memalign((void**)&ff_mpage_phy, sizeof(phys_addr_t), bsd_pagesz*sizeof(phys_addr_t))!=0){
        rte_panic("posix_memalign get ff_mpage_phy failed, err=%d.\n", errno);
        return -1;
    }

    stklist_init(&ff_mpage_ctl, bsd_pagesz);

    for (i=0; (unsigned)i<bsd_pagesz; i++ ){
        virt_addr = ff_page_start + PAGE_SIZE*i;
        memset((void*)virt_addr, 0, PAGE_SIZE);

        stklist_push( &ff_mpage_ctl, virt_addr);
        ff_mpage_phy[i] = rte_mem_virt2iova((const void*)virt_addr);
        if ( ff_mpage_phy[i] == RTE_BAD_IOVA ){
            rte_panic("rte_mem_virt2phy return invalid address.");
            return -1;
        }

        if (rte_vfio_is_enabled("vfio_pci"))
            if (rte_vfio_container_dma_map(RTE_VFIO_DEFAULT_CONTAINER_FD, virt_addr,ff_mpage_phy[i],PAGE_SIZE)<0)
                return -1;

    }

    return 0;
}

// 1: vma in fstack page table;  0: vma not in fstack pages, in DPDK pool.
static inline int ff_chk_vma(const uint64_t virtaddr)
{
    return  !!( virtaddr > ff_page_start && virtaddr < ff_page_end );
}

/*
 * Get physical address of any mapped virtual address in the current process.
 */
static inline uint64_t ff_mem_virt2phy(const void* virtaddr)
{
    uint64_t    addr = 0;
    uint32_t    pages = 0;

    pages = (((uint64_t)virtaddr - (uint64_t)ff_page_start)>>PAGE_SHIFT);
    if (pages >= (uint32_t)stklist_size(&ff_mpage_ctl)) {
        return rte_mem_virt2iova((const void*)virtaddr);
    }

    addr = ff_mpage_phy[pages] + ((const uint64_t)virtaddr & PAGE_MASK);
    return addr;
}

void *ff_mem_get_page()
{
    return (void*)stklist_pop(&ff_mpage_ctl);
}

int ff_mem_free_addr(void *p)
{
    stklist_push(&ff_mpage_ctl, (const uint64_t)p);
    return 0;
}

static inline void ff_offload_set(struct ff_dpdk_if_context *ctx, void *m, struct rte_mbuf *head)
{
    void                    *data = NULL;
    struct ff_tx_offload     offload = {0};

    ff_mbuf_tx_offload(m, &offload);
    data = rte_pktmbuf_mtod(head, void*);

    if (offload.ip_csum) {
        /* ipv6 not supported yet */
        struct rte_ipv4_hdr *iph;
        int iph_len;
        iph = (struct rte_ipv4_hdr *)(data + RTE_ETHER_HDR_LEN);
        iph_len = (iph->version_ihl & 0x0f) << 2;

        head->ol_flags |= RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_IPV4;
        head->l2_len = RTE_ETHER_HDR_LEN;
        head->l3_len = iph_len;
    }

    if (ctx->hw_features.tx_csum_l4) {
        struct rte_ipv4_hdr *iph;
        int iph_len;
        iph = (struct rte_ipv4_hdr *)(data + RTE_ETHER_HDR_LEN);
        iph_len = (iph->version_ihl & 0x0f) << 2;

        if (offload.tcp_csum) {
            head->ol_flags |= RTE_MBUF_F_TX_TCP_CKSUM;
            head->l2_len = RTE_ETHER_HDR_LEN;
            head->l3_len = iph_len;
        }

       /*
         *  TCP segmentation offload.
         *
         *  - set the PKT_TX_TCP_SEG flag in mbuf->ol_flags (this flag
         *    implies PKT_TX_TCP_CKSUM)
         *  - set the flag PKT_TX_IPV4 or PKT_TX_IPV6
         *  - if it's IPv4, set the PKT_TX_IP_CKSUM flag and
         *    write the IP checksum to 0 in the packet
         *  - fill the mbuf offload information: l2_len,
         *    l3_len, l4_len, tso_segsz
         *  - calculate the pseudo header checksum without taking ip_len
         *    in account, and set it in the TCP header. Refer to
         *    rte_ipv4_phdr_cksum() and rte_ipv6_phdr_cksum() that can be
         *    used as helpers.
         */
        if (offload.tso_seg_size) {
            struct rte_tcp_hdr *tcph;
            int tcph_len;
            tcph = (struct rte_tcp_hdr *)((char *)iph + iph_len);
            tcph_len = (tcph->data_off & 0xf0) >> 2;
            tcph->cksum = rte_ipv4_phdr_cksum(iph, RTE_MBUF_F_TX_TCP_SEG);

            head->ol_flags |= RTE_MBUF_F_TX_TCP_SEG;
            head->l4_len = tcph_len;
            head->tso_segsz = offload.tso_seg_size;
        }

        if (offload.udp_csum) {
            head->ol_flags |= RTE_MBUF_F_TX_UDP_CKSUM;
            head->l2_len = RTE_ETHER_HDR_LEN;
            head->l3_len = iph_len;
        }
    }
}

static void extbuf_free_cb(void *addr, void *fcb_opaque)
{
    ff_mbuf_extbuf_free(fcb_opaque);
}

//  create rte_mbuf refer to data in bsd mbuf.
int ff_bsd_to_rte(void **m, struct rte_mbuf *cur)
{
    void *data = NULL;
    int len = 0;
    rte_iova_t buf_iova = 0;

    struct rte_mbuf_ext_shared_info *shinfo = (struct rte_mbuf_ext_shared_info *)(cur+1);
    shinfo->free_cb = extbuf_free_cb;
    shinfo->fcb_opaque = *m;

    ff_next_mbuf(m, &data, &len); // p_bsdbuf move to next mbuf.

    buf_iova = ff_mem_virt2phy(data);

    rte_mbuf_ext_refcnt_set(shinfo, 1);
    rte_pktmbuf_attach_extbuf(cur,data,buf_iova,len,shinfo);
    return len;
}


