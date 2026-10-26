// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_APPS_RECORDER_H
#define NETDATA_APPS_RECORDER_H

#include "apps_plugin.h"
#include "apps_recorder_format.h"

#if defined(OS_LINUX)

#define APPS_RECORDER_SAVE_FUNCTION         "__processes-save"
#define APPS_RECORDER_SAVE_BEFORE_DEFAULT   600
#define APPS_RECORDER_SAVE_AFTER_DEFAULT    120
#define APPS_RECORDER_SAVE_AFTER_MAX        600
#define APPS_RECORDER_DISK_MB_DEFAULT       32

extern int apps_recorder_disk_mb;

// where a walked frame came from
typedef enum {
    APPS_REC_SOURCE_RAM  = (1 << 0),
    APPS_REC_SOURCE_DISK = (1 << 1),
} APPS_REC_SOURCE;

// walks, oldest first, every recorded frame with after_ut <= time <= before_ut, from disk and RAM, each frame once.
// Returns the sources that contributed frames. Runs under apps_and_stdout_mutex.
APPS_REC_SOURCE apps_recorder_foreach_frame(usec_t after_ut, usec_t before_ut, apps_rec_frame_cb_t cb, void *data);

// parses a time parameter: a positive value is a unix epoch, zero or negative is relative to now_s
bool apps_recorder_parse_time(const char *value, time_t now_s, time_t *out);

// the time span the recorder can answer for (0 when it has nothing)
void apps_recorder_span(usec_t *first_ut, usec_t *last_ut);

void apps_recorder_function_save(const char *transaction, char *function,
                                 usec_t *stop_monotonic_ut, bool *cancelled,
                                 BUFFER *payload, HTTP_ACCESS access,
                                 const char *source, void *data);

// answers `processes` calls that carry at: or after:/before: (apps_recorder_query.c)
void apps_recorder_function_processes(const char *transaction, char **words, size_t num_words, bool show_cmdline);

// true when a `processes` call asks for the past
bool apps_recorder_processes_wants_history(char **words, size_t num_words);

#endif

#endif //NETDATA_APPS_RECORDER_H
