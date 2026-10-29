// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_APPS_RECORDER_PROCS_H
#define NETDATA_APPS_RECORDER_PROCS_H

#include "apps_plugin.h"

// the owner of a process that was not named: identity reads were over their CPU budget
#define APPS_REC_UID_UNKNOWN        UINT32_MAX

// a process exit the kernel reported
typedef struct {
    int32_t pid;
    usec_t exit_ut;             // realtime
    int32_t status;             // the wait status

    // kept since the kernel reported its fork or exec; identity is false when it was not seen. A process not named
    // (reads over their CPU budget) has an empty comm and APPS_REC_UID_UNKNOWN.
    bool identity;
    int32_t ppid;
    uint64_t starttime;
    bool starttime_approx;      // computed from the fork event: /proc may say a little less
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
// not received. *lost counts exits whose event was lost, *unnamed processes whose name and owner were not read (CPU
// budget).
bool apps_rec_procs_drain(APPS_REC_PROC_EXIT **exits, size_t *size, size_t *used, size_t *lost, size_t *unnamed);

// whether an exit's process is the one polling knows by this starttime. A starttime computed from the fork event is
// late by the time between the process's creation and the event (usually under a tick, more when the forking task was
// preempted): up to a second is accepted, since a pid is not used again that fast.
static inline bool apps_rec_proc_exit_same(const APPS_REC_PROC_EXIT *e, uint64_t starttime) {
    return e->starttime == starttime ||
           (e->starttime_approx && e->starttime > starttime && e->starttime - starttime <= (uint64_t)system_hz);
}

#endif //NETDATA_APPS_RECORDER_PROCS_H
