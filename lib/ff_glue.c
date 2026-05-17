/*
 * Copyright (c) 2010 Kip Macy. All rights reserved.
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
 * Derived in part from libplebnet's pn_glue.c.
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/event.h>
#include <sys/jail.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/refcount.h>
#include <sys/resourcevar.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/priv.h>
#include <sys/time.h>
#include <sys/ucred.h>
#include <sys/uio.h>
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/buf.h>
#include <sys/file.h>
#include <sys/vmem.h>
#include <sys/mbuf.h>
#include <sys/smp.h>
#include <sys/sched.h>
#include <sys/vmmeter.h>
#include <sys/unpcb.h>
#include <sys/eventfd.h>
#include <sys/linker.h>
#include <sys/sleepqueue.h>
#ifdef FF_FILESYSTEM
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/rangelock.h>
#include <sys/devicestat.h>
#include <sys/sbuf.h>
#include <sys/rwlock.h>
#include <sys/disk.h>
#endif
#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_object.h>
#ifdef FF_FILESYSTEM
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#endif
#include <vm/vm_map.h>
#include <vm/vm_extern.h>
#include <vm/vm_domainset.h>
#include <vm/vm_page.h>
#include <vm/vm_pagequeue.h>

#include <netinet/in_systm.h>

#include <ck_epoch.h>
#include <ck_stack.h>

#include "ff_host_interface.h"

int kstack_pages = KSTACK_PAGES;
SYSCTL_INT(_kern, OID_AUTO, kstack_pages, CTLFLAG_RD, &kstack_pages, 0,
    "Kernel stack size in pages");
#ifndef FF_FILESYSTEM
int __read_mostly vm_ndomains = 1;
SYSCTL_INT(_vm, OID_AUTO, ndomains, CTLFLAG_RD,
    &vm_ndomains, 0, "Number of physical memory domains available.");
#endif
#ifndef MAXMEMDOM
#define MAXMEMDOM 1
#endif

struct domainset __read_mostly domainset_fixed[MAXMEMDOM];
struct domainset __read_mostly domainset_prefer[MAXMEMDOM];
struct domainset __read_mostly domainset_roundrobin;

#ifndef FF_FILESYSTEM
struct vm_domain vm_dom[MAXMEMDOM];

domainset_t __exclusive_cache_line vm_min_domains;
#endif

int bootverbose;

SYSCTL_ROOT_NODE(0, sysctl, CTLFLAG_RW, 0, "Sysctl internal magic");

SYSCTL_ROOT_NODE(CTL_VFS, vfs, CTLFLAG_RW, 0, "File system");

SYSCTL_ROOT_NODE(CTL_KERN, kern, CTLFLAG_RW, 0, "High kernel, proc, limits &c");

SYSCTL_ROOT_NODE(CTL_NET, net, CTLFLAG_RW, 0, "Network, (see socket.h)");

SYSCTL_ROOT_NODE(CTL_MACHDEP, machdep, CTLFLAG_RW, 0, "machine dependent");

SYSCTL_ROOT_NODE(CTL_VM, vm, CTLFLAG_RW, 0, "Virtual memory");

SYSCTL_ROOT_NODE(CTL_DEBUG, debug, CTLFLAG_RW, 0, "Debugging");

#ifdef FF_FILESYSTEM

SYSCTL_NODE(_debug, OID_AUTO,  sizeof,  CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "Sizeof various things");

SYSCTL_NODE(_security, OID_AUTO, bsd, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "BSD security policy");

SYSCTL_ROOT_NODE(CTL_HW, hw, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "hardware");

#endif

SYSCTL_ROOT_NODE(OID_AUTO, security, CTLFLAG_RW, 0, "Security");

SYSCTL_NODE(_kern, OID_AUTO, features, CTLFLAG_RD, 0, "Kernel Features");

SYSCTL_NODE(_kern, KERN_PROC, proc, CTLFLAG_RD,  0, "Process table");

MALLOC_DEFINE(M_DEVBUF, "devbuf", "device driver memory");
MALLOC_DEFINE(M_TEMP, "temp", "misc temporary data buffers");
static MALLOC_DEFINE(M_CRED, "cred", "credentials");
static MALLOC_DEFINE(M_PLIMIT, "plimit", "plimit structures");

MALLOC_DEFINE(M_IP6OPT, "ip6opt", "IPv6 options");

static void configure_final(void *dummy);

SYSINIT(configure3, SI_SUB_CONFIGURE, SI_ORDER_ANY, configure_final, NULL);

volatile int ticks;
int cpu_disable_deep_sleep;

static int sysctl_kern_smp_active(SYSCTL_HANDLER_ARGS);

/* This is used in modules that need to work in both SMP and UP. */
cpuset_t all_cpus;

int mp_ncpus = 1;
/* export this for libkvm consumers. */
int mp_maxcpus = MAXCPU;

volatile int smp_started;
u_int mp_maxid;

static SYSCTL_NODE(_kern, OID_AUTO, smp, CTLFLAG_RD|CTLFLAG_CAPRD, NULL,
    "Kernel SMP");

SYSCTL_INT(_kern_smp, OID_AUTO, maxid, CTLFLAG_RD|CTLFLAG_CAPRD, &mp_maxid, 0,
    "Max CPU ID.");

SYSCTL_INT(_kern_smp, OID_AUTO, maxcpus, CTLFLAG_RD|CTLFLAG_CAPRD, &mp_maxcpus,
    0, "Max number of CPUs that the system was compiled for.");

SYSCTL_PROC(_kern_smp, OID_AUTO, active, CTLFLAG_RD | CTLTYPE_INT, NULL, 0,
    sysctl_kern_smp_active, "I", "Indicates system is running in SMP mode");

int smp_disabled = 0;    /* has smp been disabled? */
SYSCTL_INT(_kern_smp, OID_AUTO, disabled, CTLFLAG_RDTUN|CTLFLAG_CAPRD,
    &smp_disabled, 0, "SMP has been disabled from the loader");

int smp_cpus = 1;    /* how many cpu's running */
SYSCTL_INT(_kern_smp, OID_AUTO, cpus, CTLFLAG_RD|CTLFLAG_CAPRD, &smp_cpus, 0,
    "Number of CPUs online");

int smp_topology = 0;    /* Which topology we're using. */
SYSCTL_INT(_kern_smp, OID_AUTO, topology, CTLFLAG_RDTUN, &smp_topology, 0,
    "Topology override setting; 0 is default provided by hardware.");

#ifndef FF_FILESYSTEM
u_int vn_lock_pair_pause_max = 1; // ff_global_cfg.freebsd.hz / 100;
SYSCTL_UINT(_debug, OID_AUTO, vn_lock_pair_pause_max, CTLFLAG_RW,
    &vn_lock_pair_pause_max, 0,
    "Max ticks for vn_lock_pair deadlock avoidance sleep");

long first_page = 0;

struct vmmeter vm_cnt;
#endif
vm_map_t kernel_map = 0;
vm_map_t kmem_map = 0;

vmem_t *kernel_arena = NULL;
vmem_t *kmem_arena = NULL;

#ifdef FF_FILESYSTEM
vmem_t *buffer_arena = NULL;
vmem_t *transient_arena = NULL;
#endif

#ifndef FF_FILESYSTEM
struct vm_object kernel_object_store;
#endif

struct vm_object kmem_object_store;

#ifndef FF_FILESYSTEM
struct filterops fs_filtops;
#endif

struct filterops sig_filtops;

#ifdef FF_FILESYSTEM
struct vop_vector fifo_specops;
int swap_pager_avail;
int allproc_gen;
u_long vm_kmem_size;
bool __read_frequently panicked;
int __read_mostly dumping;
int
sysctl_handle_domainset(SYSCTL_HANDLER_ARGS)
{
    return 0;
}

void
cngets(char *cp, size_t size, int visible);

void
cngets(char *cp, size_t size, int visible)
{

}

void
inittodr(time_t base)
{

}

// kern_lock.c
int
lockstatus(const struct lock *lk)
{
    return 0;
}

int
lockmgr_lock_flags(struct lock *lk, u_int flags, struct lock_object *ilk,
    const char *file, int line)
{
    return 0;
}

int
lockmgr_unlock(struct lock *lk)
{
    return 0;
}

void
_lockmgr_disown(struct lock *lk, const char *file, int line)
{

}

int
lockmgr_slock(struct lock *lk, u_int flags, const char *file, int line)
{
    return 0;
}

int
lockmgr_xlock(struct lock *lk, u_int flags, const char *file, int line)
{
    return 0;
}

int	wdog_kern_pat(u_int utim);
int	wdog_kern_pat(u_int utim)
{
    return 0;
}

int
vm_mmap_cdev(struct thread *td, vm_size_t objsize, vm_prot_t prot,
    vm_prot_t *maxprotp, int *flagsp, struct cdev *cdev, struct cdevsw *dsw,
    vm_ooffset_t *foff, vm_object_t *objp)
{
    return ENOTSUP;
}

int
vm_mmap_object(vm_map_t map, vm_offset_t *addr, vm_size_t size, vm_prot_t prot,
    vm_prot_t maxprot, int flags, vm_object_t object, vm_ooffset_t foff,
    boolean_t writecounted, struct thread *td)
{
    return ENOTSUP;
}


void
umtx_shm_object_terminated(vm_object_t object)
{

}






void
devctl_safe_quote_sb(struct sbuf *sb, const char *src)
{
    while (*src != '\0') {
        if (*src == '"' || *src == '\\')
            sbuf_putc(sb, '\\');
        sbuf_putc(sb, *src++);
    }
}

int
kmem_back_domain(int domain, vm_object_t object, vm_offset_t addr,
    vm_size_t size, int flags)
{
    return 0;
}

void
devstat_end_transaction_bio(struct devstat *ds, const struct bio *bp)
{

}

int
__lockmgr_args(struct lock *lk, u_int flags, struct lock_object *ilk,
    const char *wmesg, int pri, int timo, const char *file, int line)
{
    return 0;
}

void kern_psignal(struct proc *p, int sig)
{

}

void
kproc_start(const void *udata)
{

}

void
kproc_shutdown(void *arg, int howto)
{

}

void
kthread_shutdown(void *arg, int howto)
{

}

void
kthread_suspend_check(void)
{

}

void
kproc_suspend_check(struct proc *p)
{

}

extern vmem_t *kernel_arena;

vm_offset_t
kva_alloc(vm_size_t size)
{
    vm_offset_t addr;

    size = round_page(size);
    if (vmem_alloc(kernel_arena, size, M_BESTFIT | M_NOWAIT, &addr))
        return (0);

    return (addr);
}

void
kva_free(vm_offset_t addr, vm_size_t size)
{

    size = round_page(size);
    vmem_free(kernel_arena, addr, size);
}

int prison_allow(struct ucred *cred, unsigned flag)
{
    return ((cred->cr_prison->pr_allow & flag) != 0);
}

void lockinit(struct lock *lk, int pri, const char *wmesg, int timo, int flags)
{

}

void lockallowshare(struct lock *lk)
{

}

void lockdestroy(struct lock *lk)
{

}

void
lockmgr_printinfo(const struct lock *lk)
{

}

void
maybe_yield(void)
{

}

void mtx_wait_unlocked(struct mtx *m)
{

}

void
prison_add_vfs(struct vfsconf *vfsp)
{

}

int
prison_check(struct ucred *cred1, struct ucred *cred2)
{
    return 0;
}

int
priv_check_cred_vfs_lookup_nomac(struct ucred *cred)
{
    return 0;
}

uint32_t prng32_bounded(uint32_t bound);
uint32_t prng32_bounded(uint32_t bound)
{
    return 0;
}

#include <sys/rangelock.h>

void
rangelock_init(struct rangelock *lock)
{

    TAILQ_INIT(&lock->rl_waiters);
    lock->rl_currdep = NULL;
}

void
rangelock_destroy(struct rangelock *lock)
{
    KASSERT(TAILQ_EMPTY(&lock->rl_waiters), ("Dangling waiters"));
}

int
should_yield(void)
{
    return 1;
}

void
sigallowstop_impl(int prev)
{

}

int
sig_intr(void)
{
    return 1;
}


int
priv_check_cred_vfs_generation(struct ucred *cred)
{
    return 0;
}

void
crfree(struct ucred *cr)
{

}

struct ucred *
crdup(struct ucred *cr)
{
    //return (cr);
    return NULL;
}

int
vfs_export(struct mount *mp, struct export_args *argp)
{
    return 0;
}

int
vfs_setpublicfs(struct mount *mp, struct netexport *nep,
                struct export_args *argp)
{
    return 0;
}

void
vfs_unp_reclaim(struct vnode *vp);

void
vfs_unp_reclaim(struct vnode *vp)
{

}

//kern_synch.c

void
_blockcount_wakeup(blockcount_t *bc, u_int old);
void
_blockcount_wakeup(blockcount_t *bc, u_int old)
{

}

int
_blockcount_sleep(blockcount_t *bc, struct lock_object *lock, const char *wmesg,
    int prio);
int
_blockcount_sleep(blockcount_t *bc, struct lock_object *lock, const char *wmesg,
    int prio)
{
    return 0;
}

int
physio(struct cdev *dev, struct uio *uio, int ioflag);

int
physio(struct cdev *dev, struct uio *uio, int ioflag)
{
    return ENOTSUP;
}

struct pagerops defaultpagerops;
struct pagerops swappagerops;
struct pagerops devicepagerops;
struct pagerops physpagerops;
struct pagerops sgpagerops;
struct pagerops mgtdevicepagerops;


int led_set(char const *name, char const *cmd);

int
led_set(char const *name, char const *cmd)
{
    return ENOTSUP;
}


/*
 *	zfree:
 *
 *	Zero then free a block of memory allocated by malloc.
 *
 *	This routine may not block.
 */
void
zfree(void *addr, struct malloc_type *mtp)
{

}

//kern_shutdown.c
int
dumper_remove(const char *devname, const struct diocskerneldump_arg *kda)
{
    return ENOTSUP;
}

int
dumper_insert(const struct dumperinfo *di_template, const char *devname,
    const struct diocskerneldump_arg *kda)
{
    return ENOTSUP;
}

//subr_devstat.c
void
devstat_remove_entry(struct devstat *ds)
{

}

struct devstat *
devstat_new_entry(const void *dev_name,
          int unit_number, uint32_t block_size,
          devstat_support_flags flags,
          devstat_type_flags device_type,
          devstat_priority priority)
{
    return NULL;
}

void
devstat_start_transaction_bio(struct devstat *ds, struct bio *bp)
{

}

void
devstat_start_transaction_bio_t0(struct devstat *ds, struct bio *bp)
{

}

void
devstat_end_transaction_bio_bt(struct devstat *ds, const struct bio *bp,
    const struct bintime *now)
{

}

struct msgbuf *msgbufp;


int
vm_fault_disable_pagefaults(void)
{
    return 0;
}

void
vm_fault_enable_pagefaults(int save)
{

}

int
vm_fault_quick_hold_pages(vm_map_t map, vm_offset_t addr, vm_size_t len,
    vm_prot_t prot, vm_page_t *ma, int max_count)
{
    return 0;
}

int
sigdeferstop_impl(int mode)
{
    return -1;
}

void
rangelock_unlock(struct rangelock *lock, void *cookie, struct mtx *ilk)
{

}

void *
rangelock_tryrlock(struct rangelock *lock, off_t start, off_t end,
    struct mtx *ilk)
{
    return NULL;
}

void *
rangelock_wlock(struct rangelock *lock, off_t start, off_t end, struct mtx *ilk)
{
    return NULL;
}

void *
rangelock_rlock(struct rangelock *lock, off_t start, off_t end, struct mtx *ilk)
{
    return NULL;
}

int
prison_canseemount(struct ucred *cred, struct mount *mp)
{
    return 0;
}

void
prison_enforce_statfs(struct ucred *cred, struct mount *mp, struct statfs *sp)
{

}
#endif
int cold = 1;
#ifndef FF_FILESYSTEM
int unmapped_buf_allowed = 1;
#endif

int cpu_deepest_sleep = 0;    /* Deepest Cx state available. */
int cpu_disable_c2_sleep = 0; /* Timer dies in C2. */
int cpu_disable_c3_sleep = 0; /* Timer dies in C3. */

u_char __read_frequently kdb_active = 0;

static void timevalfix(struct timeval *);

/* Extra care is taken with this sysctl because the data type is volatile */
static int
sysctl_kern_smp_active(SYSCTL_HANDLER_ARGS)
{
    int error, active;

    active = smp_started;
    error = SYSCTL_OUT(req, &active, sizeof(active));
    return (error);
}

void
procinit()
{
    sx_init(&allproc_lock, "allproc");
    LIST_INIT(&allproc);
}


/*
 * Find a prison that is a descendant of mypr.  Returns a locked prison or NULL.
 */
struct prison *
prison_find_child(struct prison *mypr, int prid)
{
    return (NULL);
}

void
prison_free(struct prison *pr)
{

}

void
prison_hold_locked(struct prison *pr)
{

}

int
prison_if(struct ucred *cred, const struct sockaddr *sa)
{
    return (0);
}

int
prison_check_af(struct ucred *cred, int af)
{
    return (0);
}

int
prison_check_ip4(const struct ucred *cred, const struct in_addr *ia)
{
    return (0);
}

int
prison_equal_ip4(struct prison *pr1, struct prison *pr2)
{
    return (1);
}

#ifdef INET6
int
prison_check_ip6(const struct ucred *cred, const struct in6_addr *ia)
{
    return (0);
}

int
prison_equal_ip6(struct prison *pr1, struct prison *pr2)
{
    return (1);
}
#endif

/*
 * See if a prison has the specific flag set.
 */
int
prison_flag(struct ucred *cred, unsigned flag)
{
    /* This is an atomic read, so no locking is necessary. */
    return (flag & PR_HOST);
}

int
prison_get_ip4(struct ucred *cred, struct in_addr *ia)
{
    return (0);
}

int
prison_local_ip4(struct ucred *cred, struct in_addr *ia)
{
    return (0);
}

int
prison_remote_ip4(struct ucred *cred, struct in_addr *ia)
{
    return (0);
}

#ifdef INET6
int
prison_get_ip6(struct ucred *cred, struct in6_addr *ia)
{
    return (0);
}

int
prison_local_ip6(struct ucred *cred, struct in6_addr *ia, int other)
{
    return (0);
}

int
prison_remote_ip6(struct ucred *cred, struct in6_addr *ia)
{
    return (0);
}
#endif

int 
prison_saddrsel_ip4(struct ucred *cred, struct in_addr *ia)
{
    /* not jailed */
    return (1);
}

#ifdef INET6
int 
prison_saddrsel_ip6(struct ucred *cred, struct in6_addr *ia)
{
    /* not jailed */
    return (1);
}
#endif

#if 0
int
jailed(struct ucred *cred)
{
    return (0);
}
#endif

/*
 * Return 1 if the passed credential is in a jail and that jail does not
 * have its own virtual network stack, otherwise 0.
 */
int
jailed_without_vnet(struct ucred *cred)
{
    return (0);
}

int
priv_check(struct thread *td, int priv)
{
    return (0);
}

int
priv_check_cred(struct ucred *cred, int priv)
{
    return (0);
}


int
vslock(void *addr, size_t len)
{
    return (0);
}

void
vsunlock(void *addr, size_t len)
{

}


/*
 * Check that a proposed value to load into the .it_value or
 * .it_interval part of an interval timer is acceptable, and
 * fix it to have at least minimal value (i.e. if it is less
 * than the resolution of the clock, round it up.)
 */
int
itimerfix(struct timeval *tv)
{

    if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000)
        return (EINVAL);
    if (tv->tv_sec == 0 && tv->tv_usec != 0 && tv->tv_usec < tick)
        tv->tv_usec = tick;
    return (0);
}

/*
 * Decrement an interval timer by a specified number
 * of microseconds, which must be less than a second,
 * i.e. < 1000000.  If the timer expires, then reload
 * it.  In this case, carry over (usec - old value) to
 * reduce the value reloaded into the timer so that
 * the timer does not drift.  This routine assumes
 * that it is called in a context where the timers
 * on which it is operating cannot change in value.
 */
int
itimerdecr(struct itimerval *itp, int usec)
{
    if (itp->it_value.tv_usec < usec) {
        if (itp->it_value.tv_sec == 0) {
            /* expired, and already in next interval */
            usec -= itp->it_value.tv_usec;
            goto expire;
        }
        itp->it_value.tv_usec += 1000000;
        itp->it_value.tv_sec--;
    }
    itp->it_value.tv_usec -= usec;
    usec = 0;
    if (timevalisset(&itp->it_value))
        return (1);
    /* expired, exactly at end of interval */
expire:
    if (timevalisset(&itp->it_interval)) {
        itp->it_value = itp->it_interval;
        itp->it_value.tv_usec -= usec;
        if (itp->it_value.tv_usec < 0) {
            itp->it_value.tv_usec += 1000000;
            itp->it_value.tv_sec--;
        }
    } else
        itp->it_value.tv_usec = 0;        /* sec is already 0 */
    return (0);
}

/*
 * Add and subtract routines for timevals.
 * N.B.: subtract routine doesn't deal with
 * results which are before the beginning,
 * it just gets very confused in this case.
 * Caveat emptor.
 */
void
timevaladd(struct timeval *t1, const struct timeval *t2)
{
    t1->tv_sec += t2->tv_sec;
    t1->tv_usec += t2->tv_usec;
    timevalfix(t1);
}

void
timevalsub(struct timeval *t1, const struct timeval *t2)
{
    t1->tv_sec -= t2->tv_sec;
    t1->tv_usec -= t2->tv_usec;
    timevalfix(t1);
}

static void
timevalfix(struct timeval *t1)
{
    if (t1->tv_usec < 0) {
        t1->tv_sec--;
        t1->tv_usec += 1000000;
    }
    if (t1->tv_usec >= 1000000) {
        t1->tv_sec++;
        t1->tv_usec -= 1000000;
    }
}

/*
 * ratecheck(): simple time-based rate-limit checking.
 */
int
ratecheck(struct timeval *lasttime, const struct timeval *mininterval)
{
    struct timeval tv, delta;
    int rv = 0;

    getmicrouptime(&tv);        /* NB: 10ms precision */
    delta = tv;
    timevalsub(&delta, lasttime);

    /*
     * check for 0,0 is so that the message will be seen at least once,
     * even if interval is huge.
     */
    if (timevalcmp(&delta, mininterval, >=) ||
        (lasttime->tv_sec == 0 && lasttime->tv_usec == 0)) {
        *lasttime = tv;
        rv = 1;
    }

    return (rv);
}

/*
 * ppsratecheck(): packets (or events) per second limitation.
 *
 * Return 0 if the limit is to be enforced (e.g. the caller
 * should drop a packet because of the rate limitation).
 *
 * maxpps of 0 always causes zero to be returned.  maxpps of -1
 * always causes 1 to be returned; this effectively defeats rate
 * limiting.
 *
 * Note that we maintain the struct timeval for compatibility
 * with other bsd systems.  We reuse the storage and just monitor
 * clock ticks for minimal overhead.  
 */
int
ppsratecheck(struct timeval *lasttime, int *curpps, int maxpps)
{
    int now;

    /*
     * Reset the last time and counter if this is the first call
     * or more than a second has passed since the last update of
     * lasttime.
     */
    now = ticks;
    if (lasttime->tv_sec == 0 || (u_int)(now - lasttime->tv_sec) >= hz) {
        lasttime->tv_sec = now;
        *curpps = 1;
        return (maxpps != 0);
    } else {
        (*curpps)++;        /* NB: ignore potential overflow */
        return (maxpps < 0 || *curpps < maxpps);
    }
}

/*
 * Compute number of ticks in the specified amount of time.
 */
int
tvtohz(tv)
    struct timeval *tv;
{
    register unsigned long ticks;
    register long sec, usec;

    /*
     * If the number of usecs in the whole seconds part of the time
     * difference fits in a long, then the total number of usecs will
     * fit in an unsigned long.  Compute the total and convert it to
     * ticks, rounding up and adding 1 to allow for the current tick
     * to expire.  Rounding also depends on unsigned long arithmetic
     * to avoid overflow.
     *
     * Otherwise, if the number of ticks in the whole seconds part of
     * the time difference fits in a long, then convert the parts to
     * ticks separately and add, using similar rounding methods and
     * overflow avoidance.  This method would work in the previous
     * case but it is slightly slower and assumes that hz is integral.
     *
     * Otherwise, round the time difference down to the maximum
     * representable value.
     *
     * If ints have 32 bits, then the maximum value for any timeout in
     * 10ms ticks is 248 days.
     */
    sec = tv->tv_sec;
    usec = tv->tv_usec;
    if (usec < 0) {
        sec--;
        usec += 1000000;
    }
    if (sec < 0) {
#ifdef DIAGNOSTIC
        if (usec > 0) {
            sec++;
            usec -= 1000000;
        }
        printf("tvotohz: negative time difference %ld sec %ld usec\n",
               sec, usec);
#endif
        ticks = 1;
    } else if (sec <= LONG_MAX / 1000000)
        ticks = (sec * 1000000 + (unsigned long)usec + (tick - 1))
            / tick + 1;
    else if (sec <= LONG_MAX / hz)
        ticks = sec * hz
            + ((unsigned long)usec + (tick - 1)) / tick + 1;
    else
        ticks = LONG_MAX;
    if (ticks > INT_MAX)
        ticks = INT_MAX;
    return ((int)ticks);
}

int
copyin(const void *uaddr, void *kaddr, size_t len)
{
    memcpy(kaddr, uaddr, len);
    return (0);
}

int
copyout(const void *kaddr, void *uaddr, size_t len)
{
    memcpy(uaddr, kaddr, len);
    return (0);
}

#if 0
int
copystr(const void *kfaddr, void *kdaddr, size_t len, size_t *done)
{
    size_t bytes;

    bytes = strlcpy(kdaddr, kfaddr, len);
    if (done != NULL)
        *done = bytes;

    return (0);
}
#endif

int
copyinstr(const void *uaddr, void *kaddr, size_t len, size_t *done)
{    
    size_t bytes;

    bytes = strlcpy(kaddr, uaddr, len);
    if (done != NULL)
        *done = bytes;

    return (0);
}

int
copyiniov(const struct iovec *iovp, u_int iovcnt, struct iovec **iov, int error)
{
    u_int iovlen;

    *iov = NULL;
    if (iovcnt > UIO_MAXIOV)
        return (error);
    iovlen = iovcnt * sizeof (struct iovec);
    *iov = malloc(iovlen, M_IOV, M_WAITOK);
    error = copyin(iovp, *iov, iovlen);
    if (error) {
        free(*iov, M_IOV);
        *iov = NULL;
    }
    return (error);
}

int
subyte(volatile void *base, int byte)
{
    *(volatile char *)base = (uint8_t)byte;
    return (0);
}

static inline int
chglimit(struct uidinfo *uip, long *limit, int diff, rlim_t max, const char *name)
{
    /* Don't allow them to exceed max, but allow subtraction. */
    if (diff > 0 && max != 0) {
        if (atomic_fetchadd_long(limit, (long)diff) + diff > max) {
            atomic_subtract_long(limit, (long)diff);
            return (0);
        }
    } else {
        atomic_add_long(limit, (long)diff);
        if (*limit < 0)
            printf("negative %s for uid = %d\n", name, uip->ui_uid);
    }
    return (1);
}

/*
 * Change the count associated with number of processes
 * a given user is using.  When 'max' is 0, don't enforce a limit
 */
int
chgproccnt(struct uidinfo *uip, int diff, rlim_t max)
{
    return (chglimit(uip, &uip->ui_proccnt, diff, max, "proccnt"));
}

/*
 * Change the total socket buffer size a user has used.
 */
int
chgsbsize(struct uidinfo *uip, u_int *hiwat, u_int to, rlim_t max)
{
    int diff, rv;

    diff = to - *hiwat;
    if (diff > 0 && max == 0) {
        rv = 0;
    } else {
        rv = chglimit(uip, &uip->ui_sbsize, diff, max, "sbsize");
        if (rv != 0)
            *hiwat = to;
    }
    return (rv);
}

/*
 * Change the count associated with number of pseudo-terminals
 * a given user is using.  When 'max' is 0, don't enforce a limit
 */
int
chgptscnt(struct uidinfo *uip, int diff, rlim_t max)
{
    return (chglimit(uip, &uip->ui_ptscnt, diff, max, "ptscnt"));
}

int
chgkqcnt(struct uidinfo *uip, int diff, rlim_t max)
{
    return (chglimit(uip, &uip->ui_kqcnt, diff, max, "kqcnt"));
}

int
chgumtxcnt(struct uidinfo *uip, int diff, rlim_t max)
{
    return (chglimit(uip, &uip->ui_umtxcnt, diff, max, "umtxcnt"));
}

/*
 * Allocate a new resource limits structure and initialize its
 * reference count and mutex pointer.
 */
struct plimit *
lim_alloc()
{
    struct plimit *limp;

    limp = malloc(sizeof(struct plimit), M_PLIMIT, M_WAITOK);
    refcount_init(&limp->pl_refcnt, 1);
    return (limp);
}

void
lim_free(struct plimit *limp)
{

	if (refcount_release(&limp->pl_refcnt))
		free((void *)limp, M_PLIMIT);
}

struct plimit *
lim_hold(struct plimit *limp)
{
    refcount_acquire(&limp->pl_refcnt);
    return (limp);
}

/*
 * Return the current (soft) limit for a particular system resource.
 * The which parameter which specifies the index into the rlimit array
 */
rlim_t
lim_cur(struct thread *td, int which)
{
    struct rlimit rl;

    lim_rlimit(td, which, &rl);
    return (rl.rlim_cur);
}

rlim_t
lim_cur_proc(struct proc *p, int which)
{
    struct rlimit rl;

    lim_rlimit_proc(p, which, &rl);
    return (rl.rlim_cur);
}

/*
 * Return a copy of the entire rlimit structure for the system limit
 * specified by 'which' in the rlimit structure pointed to by 'rlp'.
 */
void
lim_rlimit(struct thread *td, int which, struct rlimit *rlp)
{
    struct proc *p = td->td_proc;

    MPASS(td == curthread);
    KASSERT(which >= 0 && which < RLIM_NLIMITS,
        ("request for invalid resource limit"));
    *rlp = p->p_limit->pl_rlimit[which];
    if (p->p_sysent->sv_fixlimit != NULL)
        p->p_sysent->sv_fixlimit(rlp, which);
}

void
lim_rlimit_proc(struct proc *p, int which, struct rlimit *rlp)
{
    PROC_LOCK_ASSERT(p, MA_OWNED);
    KASSERT(which >= 0 && which < RLIM_NLIMITS,
        ("request for invalid resource limit"));
    *rlp = p->p_limit->pl_rlimit[which];
    if (p->p_sysent->sv_fixlimit != NULL)
        p->p_sysent->sv_fixlimit(rlp, which);
}

int
useracc(void *addr, int len, int rw)
{
    return (1);
}

struct pgrp *
pgfind(pid_t pgid)
{
    return (NULL); 
}

#if 0
struct proc *
zpfind(pid_t pid)
{
    return (NULL);
}
#endif

int
p_cansee(struct thread *td, struct proc *p)
{
    return (0);
}

struct proc *
pfind(pid_t pid)
{
    return (NULL);
}

int
pget(pid_t pid, int flags, struct proc **pp)
{
    return (ESRCH);
}

struct uidinfo uid0;

struct uidinfo *
uifind(uid_t uid)
{
    return (&uid0);
}

/*
 * Allocate a zeroed cred structure.
 */
struct ucred *
crget(void)
{
    register struct ucred *cr;

    cr = malloc(sizeof(*cr), M_CRED, M_WAITOK | M_ZERO);
    refcount_init(&cr->cr_ref, 1);

    return (cr);
}

/*
 * Claim another reference to a ucred structure.
 */
struct ucred *
crhold(struct ucred *cr)
{
    refcount_acquire(&cr->cr_ref);
    return (cr);
}

#ifndef FF_FILESYSTEM
/*
 * Free a cred structure.  Throws away space when ref count gets to 0.
 */
void
crfree(struct ucred *cr)
{
    KASSERT(cr->cr_ref > 0, ("bad ucred refcount: %d", cr->cr_ref));
    KASSERT(cr->cr_ref != 0xdeadc0de, ("dangling reference to ucred"));
    if (refcount_release(&cr->cr_ref)) {

        free(cr, M_CRED);
    }
}
#endif

/*
 * Fill in a struct xucred based on a struct ucred.
 */

void
cru2x(struct ucred *cr, struct xucred *xcr)
{
#if 0
    int ngroups;

    bzero(xcr, sizeof(*xcr));
    xcr->cr_version = XUCRED_VERSION;
    xcr->cr_uid = cr->cr_uid;

    ngroups = MIN(cr->cr_ngroups, XU_NGROUPS);
    xcr->cr_ngroups = ngroups;
    bcopy(cr->cr_groups, xcr->cr_groups,
        ngroups * sizeof(*cr->cr_groups));
#endif
}


int
cr_cansee(struct ucred *u1, struct ucred *u2)
{
    return (0);
}

int
cr_canseesocket(struct ucred *cred, struct socket *so)
{
    return (0);
}

int
cr_canseeinpcb(struct ucred *cred, struct inpcb *inp)
{
    return (0);
}

int
securelevel_gt(struct ucred *cr, int level)
{
    return (0);
}

int
securelevel_ge(struct ucred *cr, int level)
{
        return (0);
}

/**
 * @brief Send a 'notification' to userland, using standard ways
 */
void
devctl_notify(const char *system, const char *subsystem, const char *type,
    const char *data)
{

}

void
cpu_pcpu_init(struct pcpu *pcpu, int cpuid, size_t size)
{

}

static void
configure_final(void *dummy)
{
    cold = 0;
}

/*
 * Send a SIGIO or SIGURG signal to a process or process group using stored
 * credentials rather than those of the current process.
 */
void
pgsigio(sigiop, sig, checkctty)
    struct sigio **sigiop;
    int sig, checkctty;
{
    panic("SIGIO not supported yet\n");
#ifdef notyet
    ksiginfo_t ksi;
    struct sigio *sigio;

    ksiginfo_init(&ksi);
    ksi.ksi_signo = sig;
    ksi.ksi_code = SI_KERNEL;

    SIGIO_LOCK();
    sigio = *sigiop;
    if (sigio == NULL) {
        SIGIO_UNLOCK();
        return;
    }
    if (sigio->sio_pgid > 0) {
        PROC_LOCK(sigio->sio_proc);
        if (CANSIGIO(sigio->sio_ucred, sigio->sio_proc->p_ucred))
            psignal(sigio->sio_proc, sig);
        PROC_UNLOCK(sigio->sio_proc);
    } else if (sigio->sio_pgid < 0) {
        struct proc *p;

        PGRP_LOCK(sigio->sio_pgrp);
        LIST_FOREACH(p, &sigio->sio_pgrp->pg_members, p_pglist) {
            PROC_LOCK(p);
            if (CANSIGIO(sigio->sio_ucred, p->p_ucred) &&
                (checkctty == 0 || (p->p_flag & P_CONTROLT)))
                psignal(p, sig);
            PROC_UNLOCK(p);
        }
        PGRP_UNLOCK(sigio->sio_pgrp);
    }
    SIGIO_UNLOCK();
#endif
}

void
kproc_exit(int ecode)
{
    panic("kproc_exit unsupported");
}

vm_offset_t
kmem_malloc(vm_size_t bytes, int flags)
{
    void *alloc = ff_mmap(NULL, bytes, ff_PROT_READ|ff_PROT_WRITE, ff_MAP_ANON|ff_MAP_PRIVATE, -1, 0);
    if ((flags & M_ZERO) && alloc != NULL)
        bzero(alloc, bytes);
    return ((vm_offset_t)alloc);
}

void
kmem_free(vm_offset_t addr, vm_size_t size)
{
    ff_munmap((void *)addr, size);
}

vm_offset_t
kmem_alloc_contig(vm_size_t size, int flags, vm_paddr_t low,
    vm_paddr_t high, u_long alignment, vm_paddr_t boundary, vm_memattr_t memattr)
{
    return (kmem_malloc(size, flags));
}

void
malloc_init(void *data)
{
    /* Nothing to do here */ 
}


void
malloc_uninit(void *data)
{
    /* Nothing to do here */ 
}

void *
malloc(unsigned long size, struct malloc_type *type, int flags)
{
    void *alloc;

    do {
        alloc = ff_malloc(size);
        if (alloc || !(flags & M_WAITOK))
            break;

        pause("malloc", hz/100);
    } while (alloc == NULL);

    if ((flags & M_ZERO) && alloc != NULL)
        bzero(alloc, size);
    return (alloc);
}

void
free(void *addr, struct malloc_type *type)
{
    ff_free(addr);
}

void *
realloc(void *addr, unsigned long size, struct malloc_type *type,
    int flags)
{
    return (ff_realloc(addr, size));
}

void *
reallocf(void *addr, unsigned long size, struct malloc_type *type,
     int flags)
{
    void *mem;

    if ((mem = ff_realloc(addr, size)) == NULL)
        ff_free(addr);

    return (mem);
}

void
DELAY(int delay)
{
    struct timespec rqt;

    if (delay < 1000)
        return;
    
    rqt.tv_nsec = 1000*((unsigned long)delay);
    rqt.tv_sec = 0;
    /*
     * FIXME: We shouldn't sleep in dpdk apps.
     */
    //nanosleep(&rqt, NULL);
}
#ifndef FF_FILESYSTEM
void 
bwillwrite(void) 
{

}


off_t
foffset_lock(struct file *fp, int flags)
{
    struct mtx *mtxp;
    off_t res;

    KASSERT((flags & FOF_OFFSET) == 0, ("FOF_OFFSET passed"));

#if OFF_MAX <= LONG_MAX
    /*
     * Caller only wants the current f_offset value.  Assume that
     * the long and shorter integer types reads are atomic.
     */
    if ((flags & FOF_NOLOCK) != 0)
        return (fp->f_offset);
#endif

    /*
     * According to McKusick the vn lock was protecting f_offset here.
     * It is now protected by the FOFFSET_LOCKED flag.
     */
    mtxp = mtx_pool_find(mtxpool_sleep, fp);
    mtx_lock(mtxp);
    /*
    if ((flags & FOF_NOLOCK) == 0) {
        while (fp->f_vnread_flags & FOFFSET_LOCKED) {
            fp->f_vnread_flags |= FOFFSET_LOCK_WAITING;
            msleep(&fp->f_vnread_flags, mtxp, PUSER -1,
                "vofflock", 0);
        }
        fp->f_vnread_flags |= FOFFSET_LOCKED;
    }
    */
    res = fp->f_offset;
    mtx_unlock(mtxp);
    return (res);
}
#endif
#if 0
void
sf_ext_free(void *arg1, void *arg2)
{
    panic("sf_ext_free not implemented.\n");
}

void
sf_ext_free_nocache(void *arg1, void *arg2)
{
    panic("sf_ext_free_nocache not implemented.\n");
}
#endif

void
sched_bind(struct thread *td, int cpu)
{

}

void
sched_unbind(struct thread* td)
{

}

void
getcredhostid(struct ucred *cred, unsigned long *hostid)
{
    *hostid = 0;
}

/*
 * Check if gid is a member of the group set.
 */
int
groupmember(gid_t gid, struct ucred *cred)
{
    int l;
    int h;
    int m;

    if (cred->cr_groups[0] == gid)
        return(1);

    /*
     * If gid was not our primary group, perform a binary search
     * of the supplemental groups.  This is possible because we
     * sort the groups in crsetgroups().
     */
    l = 1;
    h = cred->cr_ngroups;
    while (l < h) {
        m = l + ((h - l) / 2);
        if (cred->cr_groups[m] < gid)
            l = m + 1; 
        else
            h = m; 
    }
    if ((l < cred->cr_ngroups) && (cred->cr_groups[l] == gid))
        return (1);

    return (0);
}

#ifndef FF_FILESYSTEM
int
vm_wait_doms(const domainset_t *wdoms, int mflags)
{
    return 0;
}

void
vm_domainset_iter_policy_ref_init(struct vm_domainset_iter *di,
    struct domainset_ref *dr, int *domain, int *flags)
{
    *domain = 0;
}

int
vm_domainset_iter_policy(struct vm_domainset_iter *di, int *domain)
{
    //return (EJUSTRETURN);
    return 0;
}
#endif

vm_offset_t
kmem_malloc_domainset(struct domainset *ds, vm_size_t size, int flags)
{
    return (kmem_malloc(size, flags));
}

void *
mallocarray(size_t nmemb, size_t size, struct malloc_type *type, int flags)
{
    return (malloc(size * nmemb, type, flags));
}

void
getcredhostuuid(struct ucred *cred, char *buf, size_t size)
{
    mtx_lock(&cred->cr_prison->pr_mtx);
    strlcpy(buf, cred->cr_prison->pr_hostuuid, size);
    mtx_unlock(&cred->cr_prison->pr_mtx);
}

void
getjailname(struct ucred *cred, char *name, size_t len)
{
    mtx_lock(&cred->cr_prison->pr_mtx);
    strlcpy(name, cred->cr_prison->pr_name, len);
    mtx_unlock(&cred->cr_prison->pr_mtx);
}

void *
malloc_domainset(size_t size, struct malloc_type *mtp, struct domainset *ds,
    int flags)
{
    return (malloc(size, mtp, flags));
}

void *
malloc_exec(size_t size, struct malloc_type *mtp, int flags)
{

    return (malloc(size, mtp, flags));
}

int
bus_get_domain(device_t dev, int *domain)
{
    return (-1);
}

void
cru2xt(struct thread *td, struct xucred *xcr)
{
    cru2x(td->td_ucred, xcr);
    xcr->cr_pid = td->td_proc->p_pid;
}

/*
 * Set socket peer credentials at connection time.
 *
 * The client's PCB credentials are copied from its process structure.  The
 * server's PCB credentials are copied from the socket on which it called
 * listen(2).  uipc_listen cached that process's credentials at the time.
 */
void
unp_copy_peercred(struct thread *td, struct unpcb *client_unp,
    struct unpcb *server_unp, struct unpcb *listen_unp)
{
    cru2xt(td, &client_unp->unp_peercred);
    client_unp->unp_flags |= UNP_HAVEPC;

    memcpy(&server_unp->unp_peercred, &listen_unp->unp_peercred,
        sizeof(server_unp->unp_peercred));
    server_unp->unp_flags |= UNP_HAVEPC;
    client_unp->unp_flags |= (listen_unp->unp_flags & UNP_WANTCRED_MASK);
}

int
eventfd_create_file(struct thread *td, struct file *fp, uint32_t initval,
    int flags)
{
    return (0);
}

void
sched_prio(struct thread *td, u_char prio)
{

}

/*
 * The machine independent parts of context switching.
 *
 * The thread lock is required on entry and is no longer held on return.
 */
void
mi_switch(int flags)
{

}

int
sched_is_bound(struct thread *td)
{
    return (1);
}

/*
 * This function must not be called with-in read section.
 */
void
ck_epoch_synchronize_wait(struct ck_epoch *global,
    ck_epoch_wait_cb_t *cb, void *ct)
{

}

bool
ck_epoch_poll_deferred(struct ck_epoch_record *record, ck_stack_t *deferred)
{
    return (true);
}

void
_ck_epoch_addref(struct ck_epoch_record *record,
    struct ck_epoch_section *section)
{

}

bool
_ck_epoch_delref(struct ck_epoch_record *record,
    struct ck_epoch_section *section)
{
    return true;
}

void
ck_epoch_register(struct ck_epoch *global, struct ck_epoch_record *record,
    void *ct)
{

}

void
ck_epoch_init(struct ck_epoch *global)
{

}

#if 0
void
wakeup_any(const void *ident)
{

}
#endif

/*
 * kmem_bootstrap_free:
 *
 * Free pages backing preloaded data (e.g., kernel modules) to the
 * system.  Currently only supported on platforms that create a
 * vm_phys segment for preloaded data.
 */
void
kmem_bootstrap_free(vm_offset_t start, vm_size_t size)
{

}

#if 0
int
elf_cpu_parse_dynamic(caddr_t loadbase __unused, Elf_Dyn *dynamic __unused)
{
    return (0);
}
#endif

#ifndef FF_FILESYSTEM
int
pmap_change_prot(vm_offset_t va, vm_size_t size, vm_prot_t prot)
{
    return 0;
}
#endif

void *
memset_early(void *buf, int c, size_t len)
{
    return (memset(buf, c, len));
}

int
elf_reloc_late(linker_file_t lf, Elf_Addr relocbase, const void *data,
    int type, elf_lookup_fn lookup)
{
    return (0);
}

bool
elf_is_ifunc_reloc(Elf_Size r_info) 
{
    return (true);
}

void
sleepq_chains_remove_matching(bool (*matches)(struct thread *))
{

}

#ifndef FF_FILESYSTEM
u_int
vm_free_count(void)
{
    return vm_dom[0].vmd_free_count;
}
#endif

struct proc *
pfind_any(pid_t pid)
{
    return (curproc);
}

