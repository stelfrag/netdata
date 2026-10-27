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
    bool carried;               // kept from the previous iteration: polling still saw the process
} APPS_REC_PROC_EXIT;

// subscribes to the kernel's process events, then drops CAP_NET_ADMIN (which kernels before 6.6 require to
// subscribe). Capabilities are per thread: call it before apps.plugin starts any thread.
void apps_rec_procs_subscribe(void);

// starts reading the events, when subscribed
void apps_rec_procs_start(void);
void apps_rec_procs_stop(void);

// moves the exits received since the previous call into *exits (grown as needed); returns false when the events are
// not received. *lost counts exits whose event was lost.
bool apps_rec_procs_drain(APPS_REC_PROC_EXIT **exits, size_t *size, size_t *used, size_t *lost);

#endif //NETDATA_APPS_RECORDER_PROCS_H
