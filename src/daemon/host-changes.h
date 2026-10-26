// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_HOST_CHANGES_H
#define NETDATA_HOST_CHANGES_H

#include "libnetdata/libnetdata.h"
#include "database/rrdhost-system-info.h"

// Host changes are logged as journal events (MESSAGE_ID host_change_msgid), one per change, with the name of what
// changed and its old and new values (ND_HOST_CHANGE, ND_HOST_CHANGE_OLD, ND_HOST_CHANGE_NEW).

// what the previous Netdata session saw, compared at startup with this session
typedef struct {
    ND_UUID boot_id;
    const char *kernel;
    const char *os_name;
    const char *os_version;
    const char *netdata_version;
    size_t cpus;
    uint64_t ram_bytes;
} HOST_CHANGES_SNAPSHOT;

typedef void (*host_change_cb_t)(const char *what, const char *title, const char *old_value, const char *new_value, void *data);

// calls cb once per change between the two snapshots; returns the number of changes
size_t host_changes_compare(const HOST_CHANGES_SNAPSHOT *old, const HOST_CHANGES_SNAPSHOT *now, host_change_cb_t cb, void *data);

// logs one host change event; NULL values are logged as empty
void host_change_log(const char *what, const char *title, const char *old_value, const char *new_value, void *data);

// calls cb once per reported runtime system-info change; returns the number reported
size_t host_changes_system_info(const RRDHOST_SYSTEM_INFO_CHANGES *changes, host_change_cb_t cb, void *data);

// logs the runtime system-info changes of the last refresh of localhost
void host_changes_log_system_info(const RRDHOST_SYSTEM_INFO_CHANGES *changes);

int host_changes_unittest(void);

#endif //NETDATA_HOST_CHANGES_H
