// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_ALERT_EVIDENCE_H
#define NETDATA_ALERT_EVIDENCE_H

#include "libnetdata/libnetdata.h"

// Open network connections exist only live, so the PROCESS_HISTORY thread captures them (the network-connections
// Function) whenever it saves process history around an alert, an anomaly or a manual request. The snapshots are
// kept compressed under NETDATA_CACHE_DIR/alert-evidence/, one file per capture, oldest deleted over the cap. The
// alert-evidence Function reads them back.

#define ALERT_EVIDENCE_CONNECTIONS_CAP_BYTES    (16ULL * 1024 * 1024)

// a snapshot answers for a moment when it was captured between these offsets from it
#define ALERT_EVIDENCE_CONNECTIONS_BEFORE_S     10
#define ALERT_EVIDENCE_CONNECTIONS_AFTER_S      180

typedef enum {
    ALERT_EVIDENCE_CONNECTIONS_OK = 0,
    ALERT_EVIDENCE_CONNECTIONS_DISABLED,        // capturing is off, or there is no disk-backed database
    ALERT_EVIDENCE_CONNECTIONS_NOT_CAPTURED,    // no snapshot was taken near that moment
    ALERT_EVIDENCE_CONNECTIONS_EXPIRED,         // the moment is older than the oldest snapshot kept
} ALERT_EVIDENCE_CONNECTIONS_STATUS;

const char *alert_evidence_connections_status_str(ALERT_EVIDENCE_CONNECTIONS_STATUS status);

// called by the PROCESS_HISTORY thread when it starts; reads [health] capture connections on alerts
void alert_evidence_init(void);

// asks network-connections for a snapshot and stores the answer when it arrives; a no-op when disabled
void alert_evidence_capture_connections(void);

// loads the snapshot for a moment into dst (the network-connections answer as JSON)
ALERT_EVIDENCE_CONNECTIONS_STATUS alert_evidence_connections_load(time_t at_s, BUFFER *dst, time_t *captured_s);

int alert_evidence_unittest(void);

#endif //NETDATA_ALERT_EVIDENCE_H
