// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_FUNCTION_ALERT_EVIDENCE_H
#define NETDATA_FUNCTION_ALERT_EVIDENCE_H

#include "database/rrd.h"

#define FUNCTION_ALERT_EVIDENCE "alert-evidence"
#define FUNCTION_ALERT_EVIDENCE_HELP \
    "The evidence this agent holds for one alert transition: processes, connections, host changes and query pointers. " \
    "Use: alert-evidence transition:<transition id>; without a transition, the recent WARNING and CRITICAL " \
    "transitions of this agent"

// the windows around the moment T of the transition
#define ALERT_EVIDENCE_PROCESSES_BEFORE_S       600
#define ALERT_EVIDENCE_PROCESSES_AFTER_S        120
#define ALERT_EVIDENCE_HOST_CHANGES_BEFORE_S    86400
#define ALERT_EVIDENCE_HOST_CHANGES_STARTUP_S   120     // after a first evaluation: changes found at startup log late
#define ALERT_EVIDENCE_WEIGHTS_WINDOW_S         600
#define ALERT_EVIDENCE_WEIGHTS_BASELINE_S       2400

int function_alert_evidence(BUFFER *wb, const char *function, BUFFER *payload, const char *source);

#endif //NETDATA_FUNCTION_ALERT_EVIDENCE_H
