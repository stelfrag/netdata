// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_PROCESS_HISTORY_H
#define NETDATA_PROCESS_HISTORY_H

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Asks apps.plugin to save its recorded process history around a moment (the restricted Function
// __processes-save). The requests are queued and sent by the PROCESS_HISTORY thread, so callers never wait on
// the plugin and may hold their own locks. A no-op when process history saving is disabled.

// a local alert raised to WARNING or CRITICAL, an anomaly episode, or a manual save
void process_history_request(time_t at_s, const char *reason);

// called by ML after every localhost detection pass; a rate above the threshold for a minute requests one save
void process_history_anomaly_rate(double rate);

// the static thread: enabled with a disk-backed database and outside the iot profile
bool process_history_enabled(void);
void process_history_main(void *ptr);

#ifdef __cplusplus
}
#endif

#endif //NETDATA_PROCESS_HISTORY_H
