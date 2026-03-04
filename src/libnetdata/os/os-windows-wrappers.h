// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_OS_WINDOWS_WRAPPERS_H
#define NETDATA_OS_WINDOWS_WRAPPERS_H

#include "../libnetdata.h"

#if defined(OS_WINDOWS)
int os_fileno(FILE *stream);
int os_stream_isatty(FILE *stream);

long netdata_registry_get_dword_from_open_key(unsigned int *out, void *lKey, char *name);
bool netdata_registry_get_dword(unsigned int *out, void *hKey, char *subKey, char *name);

long netdata_registry_get_string_from_open_key(char *out, unsigned int length, void *lKey, char *name);
bool netdata_registry_get_string(char *out, unsigned int length, void *hKey, char *subKey, char *name);

bool EnableWindowsPrivilege(const char *privilegeName);

// Converts a possibly POSIX-style path to a Windows UTF-16 path.
// Caller owns *out_path_w and must free it with freez().
bool nd_windows_path_to_win32_utf16z(const char *in_path, wchar_t **out_path_w);

// Converts a possibly POSIX-style path to a Windows UTF-8 path.
// Caller owns *out_path_utf8 and must free it with freez().
bool nd_windows_path_to_win32_utf8z(const char *in_path, char **out_path_utf8);

// Converts a Windows process ID to the pid_t representation expected by callers.
pid_t nd_windows_process_id_to_pid_t(DWORD process_id);

// Cross-backend wrappers for process/fd primitives used by Windows code paths.
int os_pipe(int pipefd[2]);
int os_set_fd_blocking(int fd);
int os_kill_pid(pid_t pid, int sig);
int os_open_write_trunc_create(const char *path, int mode);
int os_poll_fds(struct pollfd *fds, nfds_t nfds, int timeout_ms);
ssize_t os_read(int fd, void *buf, size_t count);
ssize_t os_write(int fd, const void *buf, size_t count);
int os_close(int fd);

// Compose PATH for agent/plugin execution on Windows.
// Caller owns *out_path and must free it with freez().
bool nd_windows_compose_agent_path(const char *current_path, char **out_path);

// Returns the active Windows compatibility backend label.
const char *nd_windows_backend_name(void);

#else

static inline int os_fileno(FILE *stream) {
    return stream ? fileno(stream) : -1;
}

static inline int os_stream_isatty(FILE *stream) {
    int fd = os_fileno(stream);
    return (fd == -1) ? 0 : isatty(fd);
}

static inline int os_open_write_trunc_create(const char *path, int mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
}

static inline int os_poll_fds(struct pollfd *fds, nfds_t nfds, int timeout_ms) {
    return poll(fds, nfds, timeout_ms);
}

#endif // OS_WINDOWS
#endif //NETDATA_OS_WINDOWS_WRAPPERS_H
