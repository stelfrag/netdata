// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_APPS_RECORDER_PROCS_H
#define NETDATA_APPS_RECORDER_PROCS_H

#include "apps_plugin.h"

// a process exit the kernel reported
typedef struct {
    int32_t pid;
    usec_t exit_ut;             // realtime
    int32_t status;             // the wait status

    // read from /proc when the kernel reported its fork or exec; identity is false when it was not seen
    bool identity;
    int32_t ppid;
    uint64_t starttime;
    uint32_t uid;
    char comm[16];

    // the recorder's
    bool matched;               // given to an exit found by polling
    uint16_t carried;           // iterations it waited: polling still saw the process (a zombie not reaped yet)
} APPS_REC_PROC_EXIT;

// subscribes to the kernel's process events (kernels before 6.6 require CAP_NET_ADMIN for it). Not inside a user or
// pid namespace other than the initial ones, where the kernel accepts the subscription and sends nothing.
void apps_rec_procs_subscribe(void);

// drops CAP_NET_ADMIN for good, once the sockets that need it are open. Capabilities are per thread: call it before
// apps.plugin starts any thread.
void apps_rec_drop_net_admin(void);

// starts reading the events, when subscribed
void apps_rec_procs_start(void);
void apps_rec_procs_stop(void);

// moves the exits received since the previous call into *exits (grown as needed); returns false when the events are
// not received. *lost counts exits whose event was lost.
bool apps_rec_procs_drain(APPS_REC_PROC_EXIT **exits, size_t *size, size_t *used, size_t *lost);

#endif //NETDATA_APPS_RECORDER_PROCS_H
