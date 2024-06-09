// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_WINDOWS_PLUGIN_H
#define NETDATA_WINDOWS_PLUGIN_H

#include "daemon/common.h"

#define PLUGIN_WINDOWS_NAME "windows.plugin"

void *win_plugin_main(void *ptr);

extern char windows_shared_buffer[8192];

int do_GetSystemUptime(int update_every, usec_t dt);
int do_GetSystemRAM(int update_every, usec_t dt);
int do_GetSystemCPU(int update_every, usec_t dt);
int do_PerflibStorage(int update_every, usec_t dt);
int do_PerflibNetwork(int update_every, usec_t dt);
int do_PerflibProcesses(int update_every, usec_t dt);
int do_PerflibProcessor(int update_every, usec_t dt);
int do_PerflibProcessorPerformance(int update_every, usec_t dt);
int do_PerflibSynchronizationPerformance(int update_every, usec_t dt);
int do_PerflibBatteryStatus(int update_every, usec_t dt);
int do_PerflibLdapClientPerformance(int update_every, usec_t dt);
int do_Perflib_nameres(int update_every, usec_t dt __maybe_unused);
int do_PerflibMemory(int update_every, usec_t dt);

#include "perflib.h"

#endif //NETDATA_WINDOWS_PLUGIN_H
