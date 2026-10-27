// SPDX-License-Identifier: GPL-3.0-or-later

// The kernel's process events (the proc connector) for the flight recorder: every process exit with its wait status
// and exact time, including processes that live less than one collection iteration.
//
// Kernels before 6.6 let only CAP_NET_ADMIN subscribe. apps.plugin subscribes once at startup, before it starts any
// thread, and then drops the capability; receiving on the subscribed socket needs nothing.
//
// A thread reads the events. At a fork or exec it reads the process's identity from /proc/PID/stat while the process
// still exists, so a process that exits before the next iteration can still be named; at an exit it queues the
// exit with that identity. The recorder drains the queue every iteration.

#include "apps_recorder_procs.h"

#if defined(OS_LINUX)

#include <linux/netlink.h>
#include <linux/connector.h>
#include <linux/cn_proc.h>

#define APPS_REC_EXITS_QUEUED       4096    // exits waiting for the next iteration; more are lost
#define APPS_REC_IDENTITIES         4096    // processes whose identity the thread keeps, by pid; collisions overwrite

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
    uint32_t uid;
    char comm[16];
} REC_IDENTITY_SLOT;

static struct {
    int fd;
    bool subscribed;
    bool running;
    bool stop;
    ND_THREAD *thread;

    REC_IDENTITY_SLOT *identities;  // the thread's only

    SPINLOCK spinlock;
    APPS_REC_PROC_EXIT *queue;      // ring
    size_t head, used;
    size_t lost;                    // exits that found the ring full, and kernel overruns (each at least one)
} procs = { .fd = -1 };

static void drop_net_admin(void) {
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

void apps_rec_procs_subscribe(void) {
    if(procs.subscribed)
        return;

    int fd = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_CONNECTOR);
    if(fd != -1) {
        struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = CN_IDX_PROC };

        struct __attribute__((aligned(NLMSG_ALIGNTO))) {
            struct nlmsghdr nl;
            struct __attribute__((packed)) {
                struct cn_msg cn;
                enum proc_cn_mcast_op op;
            } body;
        } msg = {
            .nl = { .nlmsg_len = sizeof(msg), .nlmsg_type = NLMSG_DONE },
            .body = {
                .cn = { .id = { .idx = CN_IDX_PROC, .val = CN_VAL_PROC }, .len = sizeof(enum proc_cn_mcast_op) },
                .op = PROC_CN_MCAST_LISTEN,
            },
        };

        if(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || send(fd, &msg, sizeof(msg), 0) != (ssize_t)sizeof(msg)) {
            nd_log(NDLS_COLLECTORS, NDLP_NOTICE,
                   "apps.plugin recorder: cannot subscribe to the kernel's process events (%s); "
                   "exit statuses are not recorded and processes shorter than an iteration are not seen. "
                   "Kernels before 6.6 need CAP_NET_ADMIN.", strerror(errno));
            close(fd);
            fd = -1;
        }
    }

    // whether it worked or not, apps.plugin does not need the capability again
    drop_net_admin();

    if(fd == -1)
        return;

    // exits arrive in bursts; the larger buffer needs CAP_NET_ADMIN, which is gone: the plain one is capped by
    // net.core.rmem_max. The thread checks for stop once a second.
    int rcvbuf = 4 * 1024 * 1024;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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
                REC_IDENTITY_SLOT *s = &procs.identities[(uint32_t)pid % APPS_REC_IDENTITIES];
                if(!identity_read(pid, s))
                    s->pid = 0;
            }
            break;

        case PROC_EVENT_EXEC:
            // the name changes at exec
            if(ev->event_data.exec.process_pid == ev->event_data.exec.process_tgid) {
                int32_t pid = ev->event_data.exec.process_pid;
                REC_IDENTITY_SLOT *s = &procs.identities[(uint32_t)pid % APPS_REC_IDENTITIES];
                REC_IDENTITY_SLOT id;
                if(identity_read(pid, &id))
                    *s = id;
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

                REC_IDENTITY_SLOT *s = &procs.identities[(uint32_t)pid % APPS_REC_IDENTITIES];
                if(s->pid == pid) {
                    e.identity = true;
                    e.ppid = s->ppid;
                    e.starttime = s->starttime;
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
        __atomic_store_n(&procs.stop, true, __ATOMIC_RELAXED);
        shutdown(procs.fd, SHUT_RDWR);
        nd_thread_join(procs.thread);
        freez(procs.identities);
        freez(procs.queue);
    }
    if(procs.fd != -1)
        close(procs.fd);

    memset(&procs, 0, sizeof(procs));
    procs.fd = -1;
}

bool apps_rec_procs_drain(APPS_REC_PROC_EXIT **exits, size_t *size, size_t *used, size_t *lost) {
    *lost = 0;
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
    spinlock_unlock(&procs.spinlock);
    return true;
}

#endif // OS_LINUX
