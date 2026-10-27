// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_APPS_RECORDER_CONNS_H
#define NETDATA_APPS_RECORDER_CONNS_H

#include "apps_plugin.h"
#include "apps_recorder_format.h"

#define APPS_REC_CONNS_MAX          256     // connection events per frame; the rest are counted as lost

typedef struct {
    bool recorded;              // the close listener runs and the fd table is available
    uint16_t lost;              // events since the previous frame not returned
    uint16_t netns_other;       // processes with sockets in other network namespaces, at the last dump
} APPS_REC_CONNS_FRAME;

// opens the socket that receives TCP closes; false (logged once) when the kernel refuses. Call it before apps.plugin
// drops CAP_NET_ADMIN, which allows a larger receive buffer.
bool apps_rec_conns_open(void);

// starts reading the closes, when the socket is open
bool apps_rec_conns_start(void);
void apps_rec_conns_stop(void);

// the connection events since the previous call, interning process names in the chunk being built;
// call once per frame, after apps.plugin has read the fds of the processes
uint16_t apps_rec_conns_collect(APPS_REC_BUILDER *b, APPS_REC_CONN *out, uint16_t max, APPS_REC_CONNS_FRAME *frame);

// the members describing a connection event: event, found_by, protocol, family, local, remote, state, direction,
// bytes; the caller opens the object and adds the time and the process
void apps_rec_conn_to_json(BUFFER *wb, const APPS_REC_CONN *c);

#endif //NETDATA_APPS_RECORDER_CONNS_H
