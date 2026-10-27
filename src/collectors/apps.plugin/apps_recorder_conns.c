// SPDX-License-Identifier: GPL-3.0-or-later

// The TCP connections of the flight recorder, in the network namespace of apps.plugin.
//
// Closes: a thread joins the sock_diag TCP destroy groups. The kernel sends every TCP socket it destroys with its final
// tcp_info, so the bytes are exact, but without an inode or a uid: the process is found through the last socket dumps
// (by address and port) or, for the server side of a connection, through its listening port.
//
// Opens, and the maps that find the processes of closes: a sock_diag dump of TCP, joined by inode to the sockets in
// apps.plugin's fd table. A dump walks the kernel's whole TCP hash, so it runs only when the socket fds of the
// processes change, and at most every APPS_REC_CONNS_DUMP_EVERY_S. A connection that opens and closes between two
// dumps is seen only at its close, and its client side has no process.

#include "apps_recorder_conns.h"

#if defined(OS_LINUX)

#include "libnetdata/local-sockets/local-sockets.h"

#define APPS_REC_CONNS_DUMP_EVERY_S     10
#define APPS_REC_CLOSES_QUEUED          4096    // closes waiting for the next frame; more are lost
#define APPS_REC_LOCAL_IPS_MAX          256

#define REC_TCPI_BYTES_ACKED            120     // offsets in the kernel's struct tcp_info (uapi linux/tcp.h): read by
#define REC_TCPI_BYTES_RECEIVED         128     // offset, some libc headers do not have these fields

#define REC_OWNER_NONE                  UINT32_MAX

typedef struct __attribute__((packed)) {
    uint8_t family;
    uint8_t zero;               // keys are compared as bytes
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t local_ip[16];
    uint8_t remote_ip[16];
} REC_TUPLE;

typedef struct {
    REC_TUPLE t;
    uint8_t state;
    uint64_t bytes_sent, bytes_received;
} REC_CLOSE;

typedef struct {
    int32_t pid;
    uint64_t starttime;
    STRING *comm;
} REC_OWNER;

typedef struct {
    uint16_t port;
    uint32_t owner;
} REC_LISTEN;

typedef struct {
    uint8_t family;
    uint8_t ip[16];
} REC_IP;

// what one dump learned
typedef struct {
    REC_OWNER *owners;
    size_t owners_used, owners_size;

    REC_TUPLE *tuples;          // open addressing; family 0 is a free slot
    uint32_t *tuple_owner;
    size_t tuples_size;         // power of 2
    size_t tuples_used;

    REC_LISTEN *listen;         // sorted by port
    size_t listen_used, listen_size;

    REC_IP ips[APPS_REC_LOCAL_IPS_MAX];
    size_t ips_used;

    uint64_t *inodes;           // the connected sockets, sorted
    size_t inodes_used, inodes_size;
} REC_DUMP;

// the sockets of the processes, by inode, while a dump is joined
typedef struct {
    uint64_t *inode;            // 0 is a free slot
    uint32_t *owner;
    size_t size;                // power of 2
} REC_INODES;

static struct {
    bool running;

    struct {
        int fd;
        ND_THREAD *thread;
        bool stop;

        SPINLOCK spinlock;
        REC_CLOSE *queue;       // ring
        size_t head, used;
        size_t lost;            // closes that found the ring full
        size_t overruns;        // the kernel dropped notifications (each one loses at least one)
    } listener;

    REC_CLOSE *drained;

    REC_DUMP dumps[2];
    REC_DUMP *cur, *prev;
    bool dumped;
    time_t dumped_s;
    uint64_t signature;         // of the socket fds at the last dump
    uint64_t own_netns;
    uint16_t netns_other;
    bool dump_failed;           // the last dump was incomplete: the next one is a baseline
    bool dump_error_logged;

    REC_INODES inodes;
} conns = { 0 };

static void ensure(void **ptr, size_t *size, size_t needed, size_t item) {
    if(needed <= *size) return;
    size_t n = *size ? *size : 64;
    while(n < needed) n *= 2;
    *ptr = reallocz(*ptr, n * item);
    *size = n;
}

static inline uint64_t mix64(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

static inline uint64_t tcp_info_u64(const LOCAL_SOCKET *n, size_t offset) {
    uint64_t v = 0;
    if(offset + sizeof(v) <= sizeof(n->info))
        memcpy(&v, n->info.raw + offset, sizeof(v));
    return v;
}

static void tuple_of(const LOCAL_SOCKET *n, REC_TUPLE *t) {
    memset(t, 0, sizeof(*t));
    t->family = (uint8_t)n->local.family;
    t->local_port = n->local.port;
    t->remote_port = n->remote.port;
    if(n->local.family == AF_INET) {
        memcpy(t->local_ip, &n->local.ip.ipv4, sizeof(n->local.ip.ipv4));
        memcpy(t->remote_ip, &n->remote.ip.ipv4, sizeof(n->remote.ip.ipv4));
    }
    else {
        memcpy(t->local_ip, &n->local.ip.ipv6, sizeof(n->local.ip.ipv6));
        memcpy(t->remote_ip, &n->remote.ip.ipv6, sizeof(n->remote.ip.ipv6));
    }
}

static inline uint64_t tuple_hash(const REC_TUPLE *t) {
    return XXH3_64bits(t, sizeof(*t));
}

// --------------------------------------------------------------------------------------------------------------------
// the close listener

static void listener_thread(void *arg __maybe_unused) {
    static char buf[65536] __attribute__((aligned(NLMSG_ALIGNTO)));

    while(!__atomic_load_n(&conns.listener.stop, __ATOMIC_RELAXED)) {
        ssize_t r = recv(conns.listener.fd, buf, sizeof(buf), 0);
        if(r < 0) {
            if(errno == ENOBUFS) {
                spinlock_lock(&conns.listener.spinlock);
                conns.listener.overruns++;
                spinlock_unlock(&conns.listener.spinlock);
            }
            else if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                sleep_usec(USEC_PER_SEC);
            continue;
        }

        int len = (int)r;
        for(const struct nlmsghdr *h = (const struct nlmsghdr *)buf; NLMSG_OK(h, len); h = NLMSG_NEXT(h, len)) {
            if(h->nlmsg_type != SOCK_DIAG_BY_FAMILY || h->nlmsg_len < NLMSG_LENGTH(sizeof(struct inet_diag_msg)))
                continue;

            LOCAL_SOCKET n;
            local_sockets_from_inet_diag_msg(h, IPPROTO_TCP, &n);

            // every TCP socket is reported when freed: listening and never connected ones are not connections
            if(!n.remote.port)
                continue;

            REC_CLOSE c = {
                .state = (uint8_t)n.state,
                .bytes_sent = tcp_info_u64(&n, REC_TCPI_BYTES_ACKED),
                .bytes_received = tcp_info_u64(&n, REC_TCPI_BYTES_RECEIVED),
            };
            tuple_of(&n, &c.t);

            spinlock_lock(&conns.listener.spinlock);
            if(conns.listener.used < APPS_REC_CLOSES_QUEUED) {
                conns.listener.queue[(conns.listener.head + conns.listener.used) % APPS_REC_CLOSES_QUEUED] = c;
                conns.listener.used++;
            }
            else
                conns.listener.lost++;
            spinlock_unlock(&conns.listener.spinlock);
        }
    }
}

static uint64_t netns_of(const char *path) {
    char link[128];
    ssize_t len = readlink(path, link, sizeof(link) - 1);
    if(len <= 0)
        return 0;
    link[len] = '\0';

    // net:[4026531840]
    const char *s = strchr(link, '[');
    return s ? str2uint64_t(s + 1, NULL) : 0;
}

bool apps_rec_conns_open(void) {
    if(conns.listener.fd > 0)
        return true;

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if(fd == -1) {
        nd_log(NDLS_COLLECTORS, NDLP_NOTICE,
               "apps.plugin recorder: cannot open a sock_diag socket; TCP connections are not recorded");
        return false;
    }

    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    int groups[] = { SKNLGRP_INET_TCP_DESTROY, SKNLGRP_INET6_TCP_DESTROY };
    bool ok = bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0;
    for(size_t i = 0; ok && i < _countof(groups); i++)
        ok = setsockopt(fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &groups[i], sizeof(groups[i])) == 0;

    if(!ok) {
        nd_log(NDLS_COLLECTORS, NDLP_NOTICE,
               "apps.plugin recorder: the kernel does not send TCP closes to apps.plugin; "
               "TCP connections are not recorded");
        close(fd);
        return false;
    }

    // closes arrive in bursts; the larger buffer needs CAP_NET_ADMIN (held only until apps.plugin drops it after
    // opening its sockets), the plain one is capped by net.core.rmem_max
    int rcvbuf = 4 * 1024 * 1024;
    if(setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)) != 0)
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    // the thread checks for stop once a second: netlink sockets cannot be shut down to wake it
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    if(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        close(fd);
        return false;
    }

    conns.listener.fd = fd;
    return true;
}

bool apps_rec_conns_start(void) {
    if(conns.running || conns.listener.fd <= 0)
        return conns.running;

    int fd = conns.listener.fd;
    conns.listener.queue = mallocz(APPS_REC_CLOSES_QUEUED * sizeof(*conns.listener.queue));
    conns.drained = mallocz(APPS_REC_CLOSES_QUEUED * sizeof(*conns.drained));
    spinlock_init(&conns.listener.spinlock);
    conns.listener.stop = false;
    conns.listener.thread = nd_thread_create("APPS_CONNS", NETDATA_THREAD_OPTION_DONT_LOG, listener_thread, NULL);
    if(!conns.listener.thread) {
        close(fd);
        conns.listener.fd = 0;
        freez(conns.listener.queue);
        freez(conns.drained);
        conns.listener.queue = conns.drained = NULL;
        return false;
    }

    conns.cur = &conns.dumps[0];
    conns.prev = &conns.dumps[1];
    conns.own_netns = netns_of("/proc/self/ns/net");
    conns.running = true;
    return true;
}

static void dump_reset(REC_DUMP *d) {
    for(size_t i = 0; i < d->owners_used; i++)
        string_freez(d->owners[i].comm);
    d->owners_used = 0;
    if(d->tuples_used) {
        memset(d->tuples, 0, d->tuples_size * sizeof(*d->tuples));
        d->tuples_used = 0;
    }
    d->listen_used = 0;
    d->ips_used = 0;
    d->inodes_used = 0;
}

static void dump_free(REC_DUMP *d) {
    dump_reset(d);
    freez(d->owners);
    freez(d->tuples);
    freez(d->tuple_owner);
    freez(d->listen);
    freez(d->inodes);
    memset(d, 0, sizeof(*d));
}

void apps_rec_conns_stop(void) {
    if(!conns.running) {
        if(conns.listener.fd > 0)
            close(conns.listener.fd);
        conns.listener.fd = 0;
        return;
    }

    // the thread sees stop within its receive timeout
    __atomic_store_n(&conns.listener.stop, true, __ATOMIC_RELAXED);
    nd_thread_join(conns.listener.thread);
    close(conns.listener.fd);
    freez(conns.listener.queue);
    freez(conns.drained);

    dump_free(&conns.dumps[0]);
    dump_free(&conns.dumps[1]);
    freez(conns.inodes.inode);
    freez(conns.inodes.owner);
    memset(&conns, 0, sizeof(conns));
}

// --------------------------------------------------------------------------------------------------------------------
// dumps

static void inodes_add(REC_INODES *m, uint64_t inode, uint32_t owner) {
    size_t i = mix64(inode) & (m->size - 1);
    while(m->inode[i] && m->inode[i] != inode)
        i = (i + 1) & (m->size - 1);
    m->inode[i] = inode;
    m->owner[i] = owner;
}

static uint32_t inodes_find(const REC_INODES *m, uint64_t inode) {
    if(!m->size || !inode)
        return REC_OWNER_NONE;
    size_t i = mix64(inode) & (m->size - 1);
    while(m->inode[i]) {
        if(m->inode[i] == inode)
            return m->owner[i];
        i = (i + 1) & (m->size - 1);
    }
    return REC_OWNER_NONE;
}

static void tuples_grow(REC_DUMP *d);

static void tuples_add(REC_DUMP *d, const REC_TUPLE *t, uint32_t owner) {
    if((d->tuples_used + 1) * 2 > d->tuples_size)
        tuples_grow(d);

    size_t i = tuple_hash(t) & (d->tuples_size - 1);
    while(d->tuples[i].family) {
        if(!memcmp(&d->tuples[i], t, sizeof(*t)))
            return;
        i = (i + 1) & (d->tuples_size - 1);
    }
    d->tuples[i] = *t;
    d->tuple_owner[i] = owner;
    d->tuples_used++;
}

static void tuples_grow(REC_DUMP *d) {
    REC_TUPLE *old = d->tuples;
    uint32_t *old_owner = d->tuple_owner;
    size_t old_size = d->tuples_size;

    d->tuples_size = old_size ? old_size * 2 : 1024;
    d->tuples = callocz(d->tuples_size, sizeof(*d->tuples));
    d->tuple_owner = mallocz(d->tuples_size * sizeof(*d->tuple_owner));
    d->tuples_used = 0;
    for(size_t i = 0; i < old_size; i++)
        if(old[i].family)
            tuples_add(d, &old[i], old_owner[i]);

    freez(old);
    freez(old_owner);
}

static bool tuples_find(const REC_DUMP *d, const REC_TUPLE *t, uint32_t *owner) {
    if(!d->tuples_used)
        return false;
    size_t i = tuple_hash(t) & (d->tuples_size - 1);
    while(d->tuples[i].family) {
        if(!memcmp(&d->tuples[i], t, sizeof(*t))) {
            *owner = d->tuple_owner[i];
            return true;
        }
        i = (i + 1) & (d->tuples_size - 1);
    }
    return false;
}

static int cmp_listen(const void *a, const void *b) {
    const REC_LISTEN *x = a, *y = b;
    return (x->port > y->port) - (x->port < y->port);
}

static const REC_LISTEN *listen_find(const REC_DUMP *d, uint16_t port) {
    REC_LISTEN key = { .port = port };
    return d->listen_used ? bsearch(&key, d->listen, d->listen_used, sizeof(*d->listen), cmp_listen) : NULL;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static bool ip_is_local(const REC_DUMP *d, uint8_t family, const uint8_t ip[16]) {
    if(family == AF_INET && ip[0] == 127)
        return true;

    static const uint8_t loopback6[16] = { [15] = 1 };
    static const uint8_t mapped_prefix[12] = { [10] = 0xff, [11] = 0xff };
    if(family == AF_INET6 && (!memcmp(ip, loopback6, 16) || (!memcmp(ip, mapped_prefix, 12) && ip[12] == 127)))
        return true;

    for(size_t i = 0; i < d->ips_used; i++)
        if(d->ips[i].family == family && !memcmp(d->ips[i].ip, ip, 16))
            return true;
    return false;
}

static uint8_t direction_of(const LOCAL_SOCKET *n) {
    if(n->direction & SOCKET_DIRECTION_INBOUND) return APPS_REC_CONN_INBOUND;
    if(n->direction & SOCKET_DIRECTION_OUTBOUND) return APPS_REC_CONN_OUTBOUND;
    if(n->direction & SOCKET_DIRECTION_LOCAL_INBOUND) return APPS_REC_CONN_INBOUND | APPS_REC_CONN_LOCAL;
    if(n->direction & SOCKET_DIRECTION_LOCAL_OUTBOUND) return APPS_REC_CONN_OUTBOUND | APPS_REC_CONN_LOCAL;
    return 0;
}

typedef struct {
    APPS_REC_BUILDER *b;
    APPS_REC_CONN *out;
    uint16_t *n;
    uint16_t max;
    size_t lost;
    bool baseline;              // the first dump: what is open already did not open now
} REC_DUMP_CB;

static void conn_event(REC_DUMP_CB *x, uint8_t event, const REC_OWNER *o, uint8_t how, const REC_TUPLE *t,
                       uint8_t state, uint8_t direction, uint64_t sent, uint64_t received) {
    if(*x->n >= x->max) {
        x->lost++;
        return;
    }

    APPS_REC_CONN *c = &x->out[(*x->n)++];
    *c = (APPS_REC_CONN){
        .pid = o ? o->pid : 0,
        .starttime = o ? o->starttime : 0,
        .comm = apps_rec_builder_string(x->b, o ? o->comm : NULL),
        .event = event,
        .how = o ? how : APPS_REC_CONN_BY_NONE,
        .family = t->family,
        .state = state,
        .direction = direction,
        .local_port = t->local_port,
        .remote_port = t->remote_port,
        .bytes_sent = sent,
        .bytes_received = received,
    };
    memcpy(c->local_ip, t->local_ip, sizeof(c->local_ip));
    memcpy(c->remote_ip, t->remote_ip, sizeof(c->remote_ip));
}

static void dump_cb(LS_STATE *ls __maybe_unused, const LOCAL_SOCKET *n, void *data) {
    REC_DUMP_CB *x = data;
    REC_DUMP *d = conns.cur;
    uint32_t owner = inodes_find(&conns.inodes, n->inode);

    if(n->state == TCP_LISTEN) {
        ensure((void **)&d->listen, &d->listen_size, d->listen_used + 1, sizeof(*d->listen));
        d->listen[d->listen_used++] = (REC_LISTEN){ .port = n->local.port, .owner = owner };
        return;
    }

    REC_TUPLE t;
    tuple_of(n, &t);

    static const uint8_t zero[16] = { 0 };
    if(memcmp(t.local_ip, zero, sizeof(zero)) != 0 && !ip_is_local(d, t.family, t.local_ip) &&
        d->ips_used < APPS_REC_LOCAL_IPS_MAX) {
        d->ips[d->ips_used].family = t.family;
        memcpy(d->ips[d->ips_used].ip, t.local_ip, 16);
        d->ips_used++;
    }

    // bound but not connected
    if(n->state == TCP_CLOSE || !n->remote.port)
        return;

    tuples_add(d, &t, owner);
    ensure((void **)&d->inodes, &d->inodes_size, d->inodes_used + 1, sizeof(*d->inodes));
    d->inodes[d->inodes_used++] = n->inode;

    if(x->baseline || (conns.prev->inodes_used &&
        bsearch(&n->inode, conns.prev->inodes, conns.prev->inodes_used, sizeof(uint64_t), cmp_u64)))
        return;

    conn_event(x, APPS_REC_CONN_OPEN, owner != REC_OWNER_NONE ? &d->owners[owner] : NULL, APPS_REC_CONN_BY_DUMP, &t,
               (uint8_t)n->state, direction_of(n),
               tcp_info_u64(n, REC_TCPI_BYTES_ACKED), tcp_info_u64(n, REC_TCPI_BYTES_RECEIVED));
}

// the sockets of the processes, by inode, into conns.inodes and the owners of the new dump; counts the processes with
// sockets in other network namespaces
static void join_processes(REC_DUMP *d, size_t sockets) {
    size_t size = 1024;
    while(size < sockets * 2) size *= 2;
    if(size != conns.inodes.size) {
        freez(conns.inodes.inode);
        freez(conns.inodes.owner);
        conns.inodes.inode = mallocz(size * sizeof(*conns.inodes.inode));
        conns.inodes.owner = mallocz(size * sizeof(*conns.inodes.owner));
        conns.inodes.size = size;
    }
    memset(conns.inodes.inode, 0, size * sizeof(*conns.inodes.inode));

    size_t netns_other = 0;
    char path[FILENAME_MAX + 1];
    for(struct pid_stat *p = root_of_pids(); p; p = p->next) {
        if(!p->updated || !p->openfds.sockets)
            continue;

        // the dump shows only this namespace
        snprintfz(path, sizeof(path), "%s/proc/%d/ns/net", netdata_configured_host_prefix, p->pid);
        uint64_t ns = netns_of(path);
        if(ns && conns.own_netns && ns != conns.own_netns) {
            netns_other++;
            continue;
        }

        uint32_t owner = REC_OWNER_NONE;
        for(size_t fdid = 0; fdid < p->fds_size; fdid++) {
            uint64_t inode = pid_fd_socket_inode(p, fdid);
            if(!inode)
                continue;

            if(owner == REC_OWNER_NONE) {
                ensure((void **)&d->owners, &d->owners_size, d->owners_used + 1, sizeof(*d->owners));
                owner = (uint32_t)d->owners_used++;
                d->owners[owner] = (REC_OWNER){ .pid = p->pid, .starttime = p->starttime, .comm = string_dup(p->comm) };
            }
            inodes_add(&conns.inodes, inode, owner);
        }
    }

    conns.netns_other = (uint16_t)MIN(netns_other, UINT16_MAX);
}

static void dump(REC_DUMP_CB *x, size_t sockets) {
    const uint16_t first = *x->n;
    REC_DUMP *t = conns.prev;
    conns.prev = conns.cur;
    conns.cur = t;
    dump_reset(conns.cur);

    join_processes(conns.cur, sockets);

    LS_STATE ls = {
        .config = {
            .listening = true,
            .inbound = true,
            .outbound = true,
            .local = true,
            .tcp4 = true,
            .tcp6 = true,
            .tcp_info = true,
            .max_errors = 0,        // reported once below
            .cb = dump_cb,
            .data = x,
        },
    };
    local_sockets_process(&ls);

    conns.dump_failed = ls.stats.errors_encountered > 0;
    if(conns.dump_failed && !conns.dump_error_logged) {
        conns.dump_error_logged = true;
        nd_log(NDLS_COLLECTORS, NDLP_NOTICE,
               "apps.plugin recorder: the TCP socket dump failed; new connections may be missing");
    }

    qsort(conns.cur->listen, conns.cur->listen_used, sizeof(*conns.cur->listen), cmp_listen);
    qsort(conns.cur->inodes, conns.cur->inodes_used, sizeof(*conns.cur->inodes), cmp_u64);

    // inbound opens no socket was found for: the process listening on their port (complete only now)
    for(uint16_t i = first; i < *x->n; i++) {
        APPS_REC_CONN *c = &x->out[i];
        if(c->how != APPS_REC_CONN_BY_NONE || !(c->direction & APPS_REC_CONN_INBOUND))
            continue;

        const REC_LISTEN *l = listen_find(conns.cur, c->local_port);
        if(!l || l->owner == REC_OWNER_NONE)
            continue;

        const REC_OWNER *o = &conns.cur->owners[l->owner];
        c->pid = o->pid;
        c->starttime = o->starttime;
        c->comm = apps_rec_builder_string(x->b, o->comm);
        c->how = APPS_REC_CONN_BY_LISTENER;
    }
}

// --------------------------------------------------------------------------------------------------------------------

static void close_event(REC_DUMP_CB *x, const REC_CLOSE *c) {
    const REC_OWNER *o = NULL;
    uint8_t how = APPS_REC_CONN_BY_NONE;
    uint32_t owner;

    // a connection the newest dump saw
    if(tuples_find(conns.cur, &c->t, &owner) && owner != REC_OWNER_NONE) {
        o = &conns.cur->owners[owner];
        how = APPS_REC_CONN_BY_DUMP;
    }

    // the server side of a connection: its local port is a listening port
    const REC_LISTEN *l = listen_find(conns.cur, c->t.local_port);
    const REC_DUMP *ld = conns.cur;
    if(!l) {
        l = listen_find(conns.prev, c->t.local_port);
        ld = conns.prev;
    }
    if(!o && l && l->owner != REC_OWNER_NONE) {
        o = &ld->owners[l->owner];
        how = APPS_REC_CONN_BY_LISTENER;
    }

    // the older dump last: its owner of a reused address and port may be long gone
    if(!o && tuples_find(conns.prev, &c->t, &owner) && owner != REC_OWNER_NONE) {
        o = &conns.prev->owners[owner];
        how = APPS_REC_CONN_BY_DUMP;
    }

    uint8_t direction = l ? APPS_REC_CONN_INBOUND : APPS_REC_CONN_OUTBOUND;
    if(ip_is_local(conns.cur, c->t.family, c->t.remote_ip))
        direction |= APPS_REC_CONN_LOCAL;

    conn_event(x, APPS_REC_CONN_CLOSE, o, how, &c->t, c->state, direction, c->bytes_sent, c->bytes_received);
}

uint16_t apps_rec_conns_collect(APPS_REC_BUILDER *b, APPS_REC_CONN *out, uint16_t max, APPS_REC_CONNS_FRAME *frame) {
    *frame = (APPS_REC_CONNS_FRAME){ 0 };
    if(!conns.running)
        return 0;

    // this frame's closes; the ring is emptied even when they cannot be attributed
    spinlock_lock(&conns.listener.spinlock);
    size_t closes = conns.listener.used;
    for(size_t i = 0; i < closes; i++)
        conns.drained[i] = conns.listener.queue[(conns.listener.head + i) % APPS_REC_CLOSES_QUEUED];
    conns.listener.head = (conns.listener.head + closes) % APPS_REC_CLOSES_QUEUED;
    conns.listener.used = 0;
    size_t lost = conns.listener.lost + conns.listener.overruns;
    conns.listener.lost = conns.listener.overruns = 0;
    spinlock_unlock(&conns.listener.spinlock);

    // without the fd table (file charts off) the processes cannot be found
    if(!enable_file_charts)
        return 0;

    uint16_t n = 0;
    REC_DUMP_CB x = { .b = b, .out = out, .n = &n, .max = max, .lost = lost };

    // the closes happened after the dumps already made: attribute them before this frame's dump replaces the oldest
    for(size_t i = 0; i < closes; i++)
        close_event(&x, &conns.drained[i]);

    // the socket fds of the processes: when they change, a dump finds the new connections
    uint64_t signature = 0;
    size_t sockets = 0;
    for(struct pid_stat *p = root_of_pids(); p; p = p->next) {
        if(!p->updated || !p->openfds.sockets)
            continue;
        for(size_t fdid = 0; fdid < p->fds_size; fdid++) {
            uint64_t inode = pid_fd_socket_inode(p, fdid);
            if(!inode)
                continue;
            signature ^= mix64(inode ^ ((uint64_t)(uint32_t)p->pid << 32));
            sockets++;
        }
    }
    signature ^= mix64(sockets);

    time_t now_s = now_monotonic_sec();
    if(!conns.dumped || (signature != conns.signature && now_s - conns.dumped_s >= APPS_REC_CONNS_DUMP_EVERY_S)) {
        x.baseline = !conns.dumped || conns.dump_failed;
        dump(&x, sockets);
        conns.dumped = true;
        conns.dumped_s = now_s;
        conns.signature = signature;
    }

    frame->recorded = true;
    frame->lost = (uint16_t)MIN(x.lost, UINT16_MAX);
    frame->netns_other = conns.netns_other;
    return n;
}

// --------------------------------------------------------------------------------------------------------------------

static void endpoint_txt(uint8_t family, const uint8_t ip[16], uint16_t port, char *dst, size_t size) {
    char addr[INET6_ADDRSTRLEN];
    if(!inet_ntop(family == AF_INET6 ? AF_INET6 : AF_INET, ip, addr, sizeof(addr)))
        strncpyz(addr, "?", sizeof(addr) - 1);

    if(family == AF_INET6)
        snprintfz(dst, size, "[%s]:%u", addr, (unsigned)port);
    else
        snprintfz(dst, size, "%s:%u", addr, (unsigned)port);
}

void apps_rec_conn_to_json(BUFFER *wb, const APPS_REC_CONN *c) {
    static const char *found_by[] = {
        [APPS_REC_CONN_BY_NONE] = "none",
        [APPS_REC_CONN_BY_DUMP] = "socket",
        [APPS_REC_CONN_BY_LISTENER] = "listening_port",
    };
    char local[INET6_ADDRSTRLEN + 10], remote[INET6_ADDRSTRLEN + 10];
    endpoint_txt(c->family, c->local_ip, c->local_port, local, sizeof(local));
    endpoint_txt(c->family, c->remote_ip, c->remote_port, remote, sizeof(remote));

    buffer_json_member_add_string(wb, "event", c->event == APPS_REC_CONN_OPEN ? "open" : "close");
    buffer_json_member_add_string(wb, "found_by", c->how < _countof(found_by) ? found_by[c->how] : "none");
    buffer_json_member_add_string(wb, "protocol", "tcp");
    buffer_json_member_add_string(wb, "family", c->family == AF_INET6 ? "ipv6" : "ipv4");
    buffer_json_member_add_string(wb, "local", local);
    buffer_json_member_add_string(wb, "remote", remote);
    buffer_json_member_add_string(wb, "state", local_sockets_tcp_state_name(c->state));
    buffer_json_member_add_string(wb, "direction",
        (c->direction & APPS_REC_CONN_INBOUND) ? "inbound" : (c->direction & APPS_REC_CONN_OUTBOUND) ? "outbound" : "unknown");
    buffer_json_member_add_boolean(wb, "local_peer", c->direction & APPS_REC_CONN_LOCAL);
    buffer_json_member_add_uint64(wb, "bytes_sent", c->bytes_sent);
    buffer_json_member_add_uint64(wb, "bytes_received", c->bytes_received);
}

#endif // OS_LINUX
