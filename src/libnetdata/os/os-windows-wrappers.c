// SPDX-License-Identifier: GPL-3.0-or-later

#include "../libnetdata.h"

#if defined(OS_WINDOWS)
#if defined(OS_WINDOWS_MSYS2) && !defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
#include <sys/cygwin.h>
#endif

long netdata_registry_get_dword_from_open_key(unsigned int *out, void *lKey, char *name)
{
    DWORD length = 260;
    return RegQueryValueEx(lKey, name, NULL, NULL, (LPBYTE) out, &length);
}

bool netdata_registry_get_dword(unsigned int *out, void *hKey, char *subKey, char *name)
{
    HKEY lKey;
    bool status = true;
    long ret = RegOpenKeyEx(hKey,
                            subKey,
                            0,
                            KEY_READ,
                            &lKey);
    if (ret != ERROR_SUCCESS)
        return false;

    ret = netdata_registry_get_dword_from_open_key(out, lKey, name);
    if (ret != ERROR_SUCCESS)
        status = false;

    RegCloseKey(lKey);

    return status;
}

long netdata_registry_get_string_from_open_key(char *out, unsigned int length, void *lKey, char *name)
{
    return RegQueryValueEx(lKey, name, NULL, NULL, (LPBYTE) out, &length);
}

bool netdata_registry_get_string(char *out, unsigned int length, void *hKey, char *subKey, char *name)
{
    HKEY lKey;
    bool status = true;
    long ret = RegOpenKeyEx(hKey,
                            subKey,
                            0,
                            KEY_READ,
                            &lKey);
    if (ret != ERROR_SUCCESS)
        return false;

    ret = netdata_registry_get_string_from_open_key(out, length, lKey, name);
    if (ret != ERROR_SUCCESS)
        status = false;

    RegCloseKey(lKey);

    return status;
}

bool EnableWindowsPrivilege(const char *privilegeName) {
    HANDLE hToken;
    LUID luid;
    TOKEN_PRIVILEGES tkp;

    // Open the process token with appropriate access rights
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;

    // Lookup the LUID for the specified privilege
    if (!LookupPrivilegeValue(NULL, privilegeName, &luid)) {
        CloseHandle(hToken);  // Close the token handle before returning
        return false;
    }

    // Set up the TOKEN_PRIVILEGES structure
    tkp.PrivilegeCount = 1;
    tkp.Privileges[0].Luid = luid;
    tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    // Adjust the token's privileges
    if (!AdjustTokenPrivileges(hToken, FALSE, &tkp, sizeof(tkp), NULL, NULL)) {
        CloseHandle(hToken);  // Close the token handle before returning
        return false;
    }

    // Check if AdjustTokenPrivileges succeeded
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
        CloseHandle(hToken);  // Close the token handle before returning
        return false;
    }

    // Close the handle to the token after success
    CloseHandle(hToken);

    return true;
}

static bool windows_utf8_to_utf16_alloc(const char *in, wchar_t **out_w) {
    if(!in || !*in || !out_w)
        return false;

    int needed = MultiByteToWideChar(CP_UTF8, 0, in, -1, NULL, 0);
    if(needed <= 0)
        return false;

    wchar_t *w = mallocz((size_t)needed * sizeof(wchar_t));
    int written = MultiByteToWideChar(CP_UTF8, 0, in, -1, w, needed);
    if(written <= 0) {
        freez(w);
        return false;
    }

    *out_w = w;
    return true;
}

static bool windows_wide_to_utf8_alloc(const wchar_t *in_w, char **out_utf8) {
    if(!in_w || !*in_w || !out_utf8)
        return false;

    int needed = WideCharToMultiByte(CP_UTF8, 0, in_w, -1, NULL, 0, NULL, NULL);
    if(needed <= 0)
        return false;

    char *utf8 = mallocz((size_t)needed);
    int written = WideCharToMultiByte(CP_UTF8, 0, in_w, -1, utf8, needed, NULL, NULL);
    if(written <= 0) {
        freez(utf8);
        return false;
    }

    *out_utf8 = utf8;
    return true;
}

bool nd_windows_path_to_win32_utf16z(const char *in_path, wchar_t **out_path_w) {
    if(!out_path_w)
        return false;

    *out_path_w = NULL;
    if(!in_path || !*in_path)
        return false;

#if defined(OS_WINDOWS_MSYS2) && !defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
    ssize_t wpath_size = cygwin_conv_path(CCP_POSIX_TO_WIN_W, in_path, NULL, 0);
    if(wpath_size > 0) {
        wchar_t *wpath = mallocz((size_t)wpath_size);
        if(cygwin_conv_path(CCP_POSIX_TO_WIN_W, in_path, wpath, (size_t)wpath_size) == 0) {
            *out_path_w = wpath;
            return true;
        }
        freez(wpath);
    }
#endif

    return windows_utf8_to_utf16_alloc(in_path, out_path_w);
}

bool nd_windows_path_to_win32_utf8z(const char *in_path, char **out_path_utf8) {
    if(!out_path_utf8)
        return false;

    *out_path_utf8 = NULL;
    if(!in_path || !*in_path)
        return false;

#if defined(OS_WINDOWS_MSYS2) && !defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
    ssize_t path_size = cygwin_conv_path(CCP_POSIX_TO_WIN_A, in_path, NULL, 0);
    if(path_size > 0) {
        char *path = mallocz((size_t)path_size);
        if(cygwin_conv_path(CCP_POSIX_TO_WIN_A, in_path, path, (size_t)path_size) == 0) {
            *out_path_utf8 = path;
            return true;
        }
        freez(path);
    }
#endif

    wchar_t *wpath = NULL;
    if(!windows_utf8_to_utf16_alloc(in_path, &wpath))
        return false;

    bool ok = windows_wide_to_utf8_alloc(wpath, out_path_utf8);
    freez(wpath);
    return ok;
}

pid_t nd_windows_process_id_to_pid_t(DWORD process_id) {
#if defined(OS_WINDOWS_MSYS2) && !defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
    return cygwin_winpid_to_pid((pid_t)process_id);
#else
    return (pid_t)process_id;
#endif
}

int os_fileno(FILE *stream) {
    if(!stream) {
        errno = EINVAL;
        return -1;
    }

#if defined(OS_WINDOWS_MSYS2) && !defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
    return fileno(stream);
#else
    return _fileno(stream);
#endif
}

int os_stream_isatty(FILE *stream) {
    int fd = os_fileno(stream);
    if(fd == -1)
        return 0;

#if defined(OS_WINDOWS_MSYS2) && !defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
    return isatty(fd);
#else
    return _isatty(fd);
#endif
}

int os_pipe(int pipefd[2]) {
    if(!pipefd) {
        errno = EINVAL;
        return -1;
    }

#if defined(OS_WINDOWS_MSYS2) && !defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
    return pipe(pipefd);
#else
    return _pipe(pipefd, 4096, _O_BINARY);
#endif
}

int os_set_fd_blocking(int fd) {
#if defined(OS_WINDOWS_MSYS2) && !defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags == -1)
        return -1;

    flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
#else
    (void)fd;
    // CRT-created anonymous pipes are blocking by default on native Windows.
    return 0;
#endif
}

int os_kill_pid(pid_t pid, int sig) {
#if defined(OS_WINDOWS_MSYS2) && !defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
    return kill(pid, sig);
#else
    if(sig != SIGTERM) {
        errno = EINVAL;
        return -1;
    }

    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if(!h)
        return -1;

    BOOL ok = TerminateProcess(h, STATUS_CONTROL_C_EXIT);
    CloseHandle(h);
    return ok ? 0 : -1;
#endif
}

int os_open_write_trunc_create(const char *path, int mode) {
    if(!path) {
        errno = EINVAL;
        return -1;
    }

#if defined(OS_WINDOWS_MSYS2) && !defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
    return open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
#else
    return _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY | _O_NOINHERIT, mode);
#endif
}

int os_poll_fds(struct pollfd *fds, nfds_t nfds, int timeout_ms) {
#if defined(OS_WINDOWS_MSYS2) && !defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
    return poll(fds, nfds, timeout_ms);
#else
    if(!fds && nfds) {
        errno = EINVAL;
        return -1;
    }

    if(nfds == 0) {
        if(timeout_ms > 0)
            sleep_usec((usec_t)timeout_ms * USEC_PER_MS);
        return 0;
    }

    WSAPOLLFD *wfds = callocz(nfds, sizeof(*wfds));
    for(nfds_t i = 0; i < nfds; i++) {
        wfds[i].fd = (SOCKET)fds[i].fd;
        wfds[i].events = fds[i].events;
        wfds[i].revents = 0;
    }

    int ret = WSAPoll(wfds, (ULONG)nfds, timeout_ms);
    if(ret >= 0) {
        for(nfds_t i = 0; i < nfds; i++)
            fds[i].revents = wfds[i].revents;
    }
    else {
        int wsa_err = WSAGetLastError();
        switch(wsa_err) {
            case WSAEINTR:
                errno = EINTR;
                break;
            case WSAEWOULDBLOCK:
                errno = EAGAIN;
                break;
            default:
                errno = EINVAL;
                break;
        }
    }

    freez(wfds);
    return ret;
#endif
}

ssize_t os_read(int fd, void *buf, size_t count) {
#if defined(OS_WINDOWS_MSYS2) && !defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
    return read(fd, buf, count);
#else
    return _read(fd, buf, (unsigned int)count);
#endif
}

ssize_t os_write(int fd, const void *buf, size_t count) {
#if defined(OS_WINDOWS_MSYS2) && !defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
    return write(fd, buf, count);
#else
    return _write(fd, buf, (unsigned int)count);
#endif
}

int os_close(int fd) {
#if defined(OS_WINDOWS_MSYS2) && !defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
    return close(fd);
#else
    return _close(fd);
#endif
}

bool nd_windows_compose_agent_path(const char *current_path, char **out_path) {
    if(!out_path)
        return false;

    *out_path = NULL;

#if defined(OS_WINDOWS_MSYS2) && !defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
    const char *p = current_path;
    if(!p || !*p)
        p = "/bin:/usr/bin";

    const char *suffix = "/sbin:/usr/sbin:/usr/local/bin:/usr/local/sbin";
    size_t len = strlen(p) + 1 + strlen(suffix) + 1;
    char *path = mallocz(len);
    snprintfz(path, len, "%s:%s", p, suffix);
    *out_path = path;
    return true;
#else
    if(current_path && *current_path) {
        *out_path = strdupz(current_path);
        return true;
    }

    *out_path = strdupz("C:\\Windows\\System32;C:\\Windows;C:\\Windows\\System32\\Wbem");
    return true;
#endif
}

const char *nd_windows_backend_name(void) {
#if defined(OS_WINDOWS_MSYS2) && !defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
    return "msys2";
#elif defined(OS_WINDOWS_MSYS2) && defined(NETDATA_WINDOWS_FORCE_NATIVE_BACKEND)
    return "native-forced";
#else
    return "native";
#endif
}

#endif
