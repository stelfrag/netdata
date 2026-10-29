// SPDX-License-Identifier: GPL-3.0-or-later

// The kernel's process events (the proc connector) for the flight recorder: every process exit with its wait status
// and exact time, including processes that live less than one collection iteration.
//
// Kernels before 6.6 let only CAP_NET_ADMIN subscribe. apps.plugin subscribes once at startup, before it starts any
// thread, and then drops the capability; receiving on the subscribed socket needs nothing. The kernel ignores
// subscriptions from user or pid namespaces other than the initial ones, so apps.plugin does not subscribe there.
//
// A thread reads the events and keeps each new process's identity, so a process that exits before the next iteration
// can still be named; at an exit it queues the exit with that identity. The recorder drains the queue every iteration.
// A child forked by its parent's main thread is a copy of the parent: it takes the parent's name and owner, and its
// starttime is computed from the fork event. An exec reads /proc/PID/stat, since the name changes, and so does a fork
// whose parent is not known yet (the parent's, once), that came from another thread, or of a kernel thread. Rename
// and owner-change events update the identity. The reads may take 1% of a core; beyond that a process is kept
// without name and owner, and counted.

#include "apps_recorder_procs.h"

#if defined(OS_LINUX)

#include <linux/netlink.h>
#include <linux/connector.h>
#include <linux/cn_proc.h>

#define APPS_REC_EXITS_QUEUED       4096    // exits waiting for the next iteration; more are lost
#define APPS_REC_IDENTITIES         4096    // processes whose identity the thread keeps, by pid; collisions overwrite
#define APPS_REC_LOOKUP_NS_PER_S    (10 * NSEC_PER_MSEC) // CPU per second for reading identities from /proc (1% of
                                                         // a core); beyond it processes go unnamed

// capget()/capset() are called directly with the kernel's layout (uapi linux/capability.h): libcap may be missing,
// and its header conflicts with the kernel's
#define REC_CAP_VERSION_3           0x20080522
#define REC_CAP_NET_ADMIN           12

typedef struct {
    uint32_t version;
    int pid;
} REC_CAP_HEADER;

typedef struct {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
} REC_CAP_DATA;

typedef struct {
    int32_t pid;                // 0: a free slot
    int32_t ppid;
    uint64_t starttime;
    bool starttime_approx;      // computed from the fork event: /proc may say a little less
    uint32_t uid;               // APPS_REC_UID_UNKNOWN with an empty comm: not named, over the cap
    char comm[16];
} REC_IDENTITY_SLOT;

static struct {
    int fd;
    bool subscribed;
    bool running;
    bool stop;
    ND_THREAD *thread;

    REC_IDENTITY_SLOT *identities;  // the thread's only
    time_t lookups_second;          // the thread's only: the current second and the CPU its reads took
    uint64_t lookups_ns;
    time_t offset_second;           // the thread's only: when boot_offset_ns was measured
    uint64_t boot_offset_ns;        // CLOCK_BOOTTIME - CLOCK_MONOTONIC (the time spent suspended)

    SPINLOCK spinlock;
    APPS_REC_PROC_EXIT *queue;      // ring
    size_t head, used;
    size_t lost;                    // exits that found the ring full, and kernel overruns (each at least one)
    size_t unnamed;                 // new processes not named: identity reads over the cap
} procs = { .fd = -1 };

void apps_rec_drop_net_admin(void) {
    REC_CAP_HEADER h = { .version = REC_CAP_VERSION_3, .pid = 0 };
    REC_CAP_DATA d[2];
    if(syscall(SYS_capget, &h, d) != 0)
        return;

    const size_t i = REC_CAP_NET_ADMIN / 32;
    const uint32_t bit = 1U << (REC_CAP_NET_ADMIN % 32);
    if(!((d[i].effective | d[i].permitted | d[i].inheritable) & bit))
        return;

    d[i].effective &= ~bit;
    d[i].permitted &= ~bit;
    d[i].inheritable &= ~bit;
    if(syscall(SYS_capset, &h, d) != 0)
        nd_log(NDLS_COLLECTORS, NDLP_WARNING, "apps.plugin recorder: cannot drop CAP_NET_ADMIN after subscribing");
}

// the kernel sends process events only to listeners in the initial user and pid namespaces
static bool initial_namespaces(void) {
    char buf[4096];

    // the identity map: 0 0 4294967295
    if(read_txt_file("/proc/self/uid_map", buf, sizeof(buf)) == 0) {
        unsigned long inside, outside, count;
        if(sscanf(buf, "%lu %lu %lu", &inside, &outside, &count) != 3 || inside != 0 || outside != 0 ||
            count != 4294967295UL)
            return false;
    }

    // NSpid lists one pid per nested pid namespace
    if(read_txt_file("/proc/self/status", buf, sizeof(buf)) == 0) {
        const char *s = strstr(buf, "\nNSpid:");
        if(s) {
            s += 7;
            while(*s == ' ' || *s == '\t') s++;
            while(*s >= '0' && *s <= '9') s++;
            while(*s == ' ' || *s == '\t') s++;
            if(*s >= '0' && *s <= '9')
                return false;
        }
    }

    return true;
}

// a PROC_CN_MCAST_LISTEN or PROC_CN_MCAST_IGNORE request
struct __attribute__((aligned(NLMSG_ALIGNTO))) rec_cn_mcast {
    struct nlmsghdr nl;
    struct __attribute__((packed)) {
        struct cn_msg cn;
        enum proc_cn_mcast_op op;
    } body;
};

static struct rec_cn_mcast cn_mcast(enum proc_cn_mcast_op op) {
    return (struct rec_cn_mcast){
        .nl = { .nlmsg_len = sizeof(struct rec_cn_mcast), .nlmsg_type = NLMSG_DONE },
        .body = {
            .cn = { .id = { .idx = CN_IDX_PROC, .val = CN_VAL_PROC }, .len = sizeof(enum proc_cn_mcast_op) },
            .op = op,
        },
    };
}

void apps_rec_procs_subscribe(void) {
    if(procs.subscribed)
        return;

    if(!initial_namespaces()) {
        errno_clear();
        nd_log(NDLS_COLLECTORS, NDLP_NOTICE,
               "apps.plugin recorder: running in a user or pid namespace, where the kernel sends no process events; "
               "exit statuses are not recorded and processes shorter than an iteration are not seen");
        return;
    }

    int fd = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_CONNECTOR);
    if(fd != -1) {
        struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = CN_IDX_PROC };
        struct rec_cn_mcast msg = cn_mcast(PROC_CN_MCAST_LISTEN);

        if(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || send(fd, &msg, sizeof(msg), 0) != (ssize_t)sizeof(msg)) {
            nd_log(NDLS_COLLECTORS, NDLP_NOTICE,
                   "apps.plugin recorder: cannot subscribe to the kernel's process events (%s); "
                   "exit statuses are not recorded and processes shorter than an iteration are not seen. "
                   "Kernels before 6.6 need CAP_NET_ADMIN.", strerror(errno));
            close(fd);
            fd = -1;
        }
    }

    if(fd == -1)
        return;

    // events arrive in bursts; the larger buffer needs CAP_NET_ADMIN (still held), the plain one is capped by
    // net.core.rmem_max. The thread checks for stop once a second: netlink sockets cannot be shut down to wake it.
    int rcvbuf = 4 * 1024 * 1024;
    if(setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)) != 0)
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    if(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        close(fd);
        return;
    }

    procs.fd = fd;
    procs.subscribed = true;
}

// the identity of a process from /proc/PID/stat and the owner of /proc/PID; false when it is gone
static bool identity_read(int32_t pid, REC_IDENTITY_SLOT *s) {
    char path[FILENAME_MAX + 1], buf[1024];
    snprintfz(path, sizeof(path), "%s/proc/%d/stat", netdata_configured_host_prefix, pid);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if(fd == -1)
        return false;

    struct stat st;
    ssize_t len = (fstat(fd, &st) == 0) ? read(fd, buf, sizeof(buf) - 1) : -1;
    close(fd);
    if(len <= 0)
        return false;
    buf[len] = '\0';

    // pid (comm) state ppid ... starttime is field 22; comm may hold spaces and parentheses
    char *open_paren = strchr(buf, '('), *close_paren = strrchr(buf, ')');
    if(!open_paren || !close_paren || close_paren < open_paren)
        return false;

    REC_IDENTITY_SLOT id = { .pid = pid, .uid = (uint32_t)st.st_uid };

    // /proc/PID of a process that is not dumpable (setuid programs) belongs to root: its effective uid is in status
    if(st.st_uid == 0) {
        char status[4096];
        snprintfz(path, sizeof(path), "%s/proc/%d/status", netdata_configured_host_prefix, pid);
        const char *u = (read_txt_file(path, status, sizeof(status)) == 0) ? strstr(status, "\nUid:") : NULL;
        unsigned long real, effective;
        if(u && sscanf(u + 5, "%lu %lu", &real, &effective) == 2)
            id.uid = (uint32_t)effective;
    }
    size_t comm_len = MIN((size_t)(close_paren - open_paren - 1), sizeof(id.comm) - 1);
    memcpy(id.comm, open_paren + 1, comm_len);
    id.comm[comm_len] = '\0';

    char *p = close_paren + 1;
    for(int field = 3; field <= 22 && *p; field++) {
        while(*p == ' ') p++;
        if(field == 4) id.ppid = (int32_t)str2i(p);
        else if(field == 22) id.starttime = str2uint64_t(p, NULL);
        while(*p && *p != ' ') p++;
    }
    if(!id.starttime)
        return false;

    *s = id;
    return true;
}

static inline REC_IDENTITY_SLOT *identity_slot(int32_t pid) {
    return &procs.identities[(uint32_t)pid % APPS_REC_IDENTITIES];
}

// whether one more identity may be read from /proc in this second: the reads' CPU is within the budget
static bool lookup_allowed(void) {
    time_t second = now_monotonic_sec();
    if(second != procs.lookups_second) {
        procs.lookups_second = second;
        procs.lookups_ns = 0;
    }
    return procs.lookups_ns < APPS_REC_LOOKUP_NS_PER_S;
}

static inline uint64_t thread_cpu_ns(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0 ? (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec : 0;
}

// identity_read(), its CPU charged to the budget
static bool identity_lookup(int32_t pid, REC_IDENTITY_SLOT *s) {
    uint64_t started = thread_cpu_ns();
    bool ok = identity_read(pid, s);
    uint64_t ended = thread_cpu_ns();
    procs.lookups_ns += (ended > started) ? ended - started : 0;
    return ok;
}

static void unnamed_count(void) {
    spinlock_lock(&procs.spinlock);
    procs.unnamed++;
    spinlock_unlock(&procs.spinlock);
}

static inline uint64_t timespec_ns(const struct timespec *ts) {
    return (uint64_t)ts->tv_sec * NSEC_PER_SEC + (uint64_t)ts->tv_nsec;
}

// /proc/PID/stat field 22 of a process forked at timestamp_ns (CLOCK_MONOTONIC): clock ticks of CLOCK_BOOTTIME at its
// creation, which comes before the event, so /proc may say a little less (apps_rec_proc_exit_same()); 0 when unknown
static uint64_t fork_starttime(uint64_t timestamp_ns) {
    // the offset changes only by a suspend: measured once a second, the smallest of a few samples, since a
    // preemption between the two clock reads only makes it larger
    time_t second = now_monotonic_sec();
    if(second != procs.offset_second || !procs.boot_offset_ns) {
        uint64_t best = UINT64_MAX;
        for(int i = 0; i < 3; i++) {
            struct timespec mono, boot;
            if(clock_gettime(CLOCK_MONOTONIC, &mono) != 0 || clock_gettime(CLOCK_BOOTTIME, &boot) != 0)
                return 0;
            uint64_t m = timespec_ns(&mono), b = timespec_ns(&boot);
            best = MIN(best, b > m ? b - m : 0);
        }
        procs.boot_offset_ns = best;
        procs.offset_second = second;
    }
    return (timestamp_ns + procs.boot_offset_ns) / (NSEC_PER_SEC / system_hz);
}

// the kernel stamps events with CLOCK_MONOTONIC (libnetdata's monotonic clock is MONOTONIC_RAW)
static usec_t monotonic_ns_to_realtime_ut(uint64_t ns) {
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return now_realtime_usec();

    usec_t now_mono_ut = (usec_t)ts.tv_sec * USEC_PER_SEC + (usec_t)ts.tv_nsec / NSEC_PER_USEC;
    return now_realtime_usec() - now_mono_ut + ns / NSEC_PER_USEC;
}

static void exit_queue(const APPS_REC_PROC_EXIT *e) {
    spinlock_lock(&procs.spinlock);
    if(procs.used < APPS_REC_EXITS_QUEUED) {
        procs.queue[(procs.head + procs.used) % APPS_REC_EXITS_QUEUED] = *e;
        procs.used++;
    }
    else
        procs.lost++;
    spinlock_unlock(&procs.spinlock);
}

static void proc_event(const struct proc_event *ev) {
    switch(ev->what) {
        case PROC_EVENT_FORK:
            // a new process, not a thread
            if(ev->event_data.fork.child_pid == ev->event_data.fork.child_tgid) {
                int32_t pid = ev->event_data.fork.child_pid;
                int32_t ppid = ev->event_data.fork.parent_tgid;
                REC_IDENTITY_SLOT *s = identity_slot(pid);
                REC_IDENTITY_SLOT id = {
                    .pid = pid, .ppid = ppid, .starttime = fork_starttime(ev->timestamp_ns), .starttime_approx = true,
                    .uid = APPS_REC_UID_UNKNOWN,
                };

                // a copy of the parent, when the parent's main thread forked it; not kernel threads (kthreadd's
                // children, renamed without an event)
                bool copied = false;
                if(id.starttime && ev->event_data.fork.parent_pid == ppid && ppid != 2) {
                    REC_IDENTITY_SLOT *parent = identity_slot(ppid), found;
                    if(parent->pid != ppid && lookup_allowed() && identity_lookup(ppid, &found)) {
                        // kept for its next children, unless its slot holds another process
                        if(!parent->pid)
                            *parent = found;
                        else
                            parent = &found;
                    }
                    if(parent->pid == ppid && parent->comm[0]) {
                        id.uid = parent->uid;
                        memcpy(id.comm, parent->comm, sizeof(id.comm));
                        copied = true;
                    }
                }

                if(!copied) {
                    if(lookup_allowed()) {
                        // a process already gone is not kept
                        if(!identity_lookup(pid, &id)) {
                            s->pid = 0;
                            break;
                        }
                    }
                    else if(id.starttime)
                        unnamed_count();
                    else {
                        s->pid = 0;
                        break;
                    }
                }

                *s = id;
            }
            break;

        case PROC_EVENT_EXEC:
            // the name changes at exec
            if(ev->event_data.exec.process_pid == ev->event_data.exec.process_tgid) {
                int32_t pid = ev->event_data.exec.process_pid;
                REC_IDENTITY_SLOT *s = identity_slot(pid);
                REC_IDENTITY_SLOT id;
                if(lookup_allowed()) {
                    if(identity_lookup(pid, &id))
                        *s = id;
                }
                else if(s->pid == pid && s->comm[0]) {
                    // over the cap: the program and maybe the owner changed, and are not known
                    s->comm[0] = '\0';
                    s->uid = APPS_REC_UID_UNKNOWN;
                    unnamed_count();
                }
            }
            break;

        case PROC_EVENT_UID:
            // the owner changed (setuid() or a setuid exec); identity_read() keeps the effective uid too
            if(ev->event_data.id.process_pid == ev->event_data.id.process_tgid) {
                int32_t pid = ev->event_data.id.process_pid;
                REC_IDENTITY_SLOT *s = identity_slot(pid);
                if(s->pid == pid && s->comm[0])
                    s->uid = ev->event_data.id.e.euid;
            }
            break;

        case PROC_EVENT_COMM:
            // a process renamed itself (prctl); the event carries the name
            if(ev->event_data.comm.process_pid == ev->event_data.comm.process_tgid && ev->event_data.comm.comm[0]) {
                int32_t pid = ev->event_data.comm.process_pid;
                REC_IDENTITY_SLOT *s = identity_slot(pid);
                if(s->pid == pid && s->comm[0]) {
                    memcpy(s->comm, ev->event_data.comm.comm, MIN(sizeof(s->comm), sizeof(ev->event_data.comm.comm)));
                    s->comm[sizeof(s->comm) - 1] = '\0';
                }
            }
            break;

        case PROC_EVENT_EXIT:
            // the process ends when its main thread (pid == tgid) exits
            if(ev->event_data.exit.process_pid == ev->event_data.exit.process_tgid) {
                int32_t pid = ev->event_data.exit.process_pid;
                APPS_REC_PROC_EXIT e = {
                    .pid = pid,
                    .exit_ut = monotonic_ns_to_realtime_ut(ev->timestamp_ns),
                    .status = (int32_t)ev->event_data.exit.exit_code,
                };

                REC_IDENTITY_SLOT *s = identity_slot(pid);
                if(s->pid == pid) {
                    e.identity = true;
                    e.ppid = s->ppid;
                    e.starttime = s->starttime;
                    e.starttime_approx = s->starttime_approx;
                    e.uid = s->uid;
                    memcpy(e.comm, s->comm, sizeof(e.comm));
                    s->pid = 0;
                }
                exit_queue(&e);
            }
            break;

        default:
            break;
    }
}

static void procs_thread(void *arg __maybe_unused) {
    static char buf[16384] __attribute__((aligned(NLMSG_ALIGNTO)));

    while(!__atomic_load_n(&procs.stop, __ATOMIC_RELAXED)) {
        ssize_t r = recv(procs.fd, buf, sizeof(buf), 0);
        if(r < 0) {
            if(errno == ENOBUFS) {
                spinlock_lock(&procs.spinlock);
                procs.lost++;
                spinlock_unlock(&procs.spinlock);

                // exits and forks were lost: an identity may now belong to a pid used again
                memset(procs.identities, 0, APPS_REC_IDENTITIES * sizeof(*procs.identities));
            }
            else if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                sleep_usec(USEC_PER_SEC);
            continue;
        }

        int len = (int)r;
        for(const struct nlmsghdr *h = (const struct nlmsghdr *)buf; NLMSG_OK(h, len); h = NLMSG_NEXT(h, len)) {
            if(h->nlmsg_type == NLMSG_ERROR || h->nlmsg_type == NLMSG_NOOP ||
                h->nlmsg_len < NLMSG_LENGTH(sizeof(struct cn_msg)))
                continue;

            const struct cn_msg *cn = NLMSG_DATA(h);
            if(cn->id.idx != CN_IDX_PROC || cn->id.val != CN_VAL_PROC ||
                NLMSG_LENGTH(sizeof(struct cn_msg) + cn->len) > h->nlmsg_len)
                continue;

            // kernels and headers may differ in the size of the event union: copy what both have
            struct proc_event ev = { 0 };
            memcpy(&ev, cn->data, MIN((size_t)cn->len, sizeof(ev)));
            proc_event(&ev);
        }
    }
}

void apps_rec_procs_start(void) {
    if(!procs.subscribed || procs.running)
        return;

    procs.identities = callocz(APPS_REC_IDENTITIES, sizeof(*procs.identities));
    procs.queue = mallocz(APPS_REC_EXITS_QUEUED * sizeof(*procs.queue));
    spinlock_init(&procs.spinlock);
    procs.stop = false;
    procs.thread = nd_thread_create("APPS_PROCS", NETDATA_THREAD_OPTION_DONT_LOG, procs_thread, NULL);
    if(!procs.thread) {
        freez(procs.identities);
        freez(procs.queue);
        procs.identities = NULL;
        procs.queue = NULL;
        return;
    }
    procs.running = true;
}

void apps_rec_procs_stop(void) {
    if(procs.running) {
        // the thread sees stop within its receive timeout
        __atomic_store_n(&procs.stop, true, __ATOMIC_RELAXED);
        nd_thread_join(procs.thread);
        freez(procs.identities);
        freez(procs.queue);
    }
    if(procs.fd != -1) {
        // the kernel counts listeners and keeps building events while it has any
        struct rec_cn_mcast msg = cn_mcast(PROC_CN_MCAST_IGNORE);
        (void)send(procs.fd, &msg, sizeof(msg), 0);
        close(procs.fd);
    }

    memset(&procs, 0, sizeof(procs));
    procs.fd = -1;
}

bool apps_rec_procs_drain(APPS_REC_PROC_EXIT **exits, size_t *size, size_t *used, size_t *lost, size_t *unnamed) {
    *lost = 0;
    *unnamed = 0;
    if(!procs.running)
        return false;

    // room for a full queue, allocated outside the lock
    if(*used + APPS_REC_EXITS_QUEUED > *size) {
        *size = *used + APPS_REC_EXITS_QUEUED;
        *exits = reallocz(*exits, *size * sizeof(**exits));
    }

    spinlock_lock(&procs.spinlock);
    for(size_t i = 0; i < procs.used; i++)
        (*exits)[(*used)++] = procs.queue[(procs.head + i) % APPS_REC_EXITS_QUEUED];
    procs.head = (procs.head + procs.used) % APPS_REC_EXITS_QUEUED;
    procs.used = 0;
    *lost = procs.lost;
    procs.lost = 0;
    *unnamed = procs.unnamed;
    procs.unnamed = 0;
    spinlock_unlock(&procs.spinlock);
    return true;
}

#endif // OS_LINUX
