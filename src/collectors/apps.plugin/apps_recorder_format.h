// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_APPS_RECORDER_FORMAT_H
#define NETDATA_APPS_RECORDER_FORMAT_H

#include "libnetdata/libnetdata.h"

// The apps.plugin flight recorder keeps the process table as frames, one per collection iteration.
// Frames are grouped in chunks. A chunk carries its own string table, so every chunk decodes alone.
// Chunks are host-local caches: integers are stored in native byte order and the version guards the layout.

#define APPS_REC_MAGIC              0x5250444eU // "NDPR"
#define APPS_REC_VERSION            5
#define APPS_REC_FRAMES_PER_CHUNK   60
#define APPS_REC_STRING_NONE        UINT16_MAX
#define APPS_REC_STRINGS_MAX        (UINT16_MAX - 1)
#define APPS_REC_STRING_LEN_MAX     4095
#define APPS_REC_RAW_MAX            (64U * 1024 * 1024)    // the largest decompressed chunk a reader accepts

#define APPS_REC_CHUNK_FLAG_ZSTD    0x0001

// why a row was selected
#define APPS_REC_WHY_CPU            0x01
#define APPS_REC_WHY_MEM            0x02
#define APPS_REC_WHY_IO             0x04

// APPS_REC_FRAME_HDR.flags
#define APPS_REC_FRAME_FILES        0x0001  // the open files of the I/O rows were looked at
#define APPS_REC_FRAME_SOCKETS      0x0002  // the rows carry their socket counts
#define APPS_REC_FRAME_CONNS        0x0004  // TCP connections were recorded (the close listener was running)
#define APPS_REC_FRAME_PROC_EVENTS  0x0008  // the kernel's process events were received: exits carry their status

// APPS_REC_EXIT.flags
#define APPS_REC_EXIT_STATUS        0x01    // status and exit_ut are known

// APPS_REC_FILE.flags
#define APPS_REC_FILE_DELETED       0x01    // no name left on the filesystem (st_nlink == 0)

// APPS_REC_CONN.event
#define APPS_REC_CONN_OPEN          1       // new since the previous socket dump
#define APPS_REC_CONN_CLOSE         2       // the kernel destroyed it

// APPS_REC_CONN.how: how the process was found
#define APPS_REC_CONN_BY_NONE       0       // not found: the process is unknown
#define APPS_REC_CONN_BY_DUMP       1       // its socket was in a dump, joined to a process by inode
#define APPS_REC_CONN_BY_LISTENER   2       // its local port is a listening port: the listener's process

// APPS_REC_CONN.direction
#define APPS_REC_CONN_INBOUND       0x01    // to a local listening port
#define APPS_REC_CONN_OUTBOUND      0x02
#define APPS_REC_CONN_LOCAL         0x04    // both ends on this host

typedef struct __attribute__((packed)) {
    int32_t pid;
    int32_t ppid;
    uint64_t starttime;         // /proc/PID/stat field 22, clock ticks since boot
    uint32_t uid;
    uint16_t comm;              // string ids
    uint16_t exe;
    uint16_t group;             // apps_groups.conf target
    uint16_t cmdline;           // APPS_REC_STRING_NONE unless command lines are recorded
    uint32_t cpu;               // 1/10000 of a core
    uint32_t rss_kib;
    uint32_t swap_kib;
    uint32_t lread_kibs;        // logical I/O, KiB/s
    uint32_t lwrite_kibs;
    uint32_t pread_kibs;        // physical I/O, KiB/s
    uint32_t pwrite_kibs;
    uint16_t threads;
    uint16_t sockets;           // open socket fds, when the frame has APPS_REC_FRAME_SOCKETS
    uint8_t state;              // the /proc/PID/stat state letter, 0 when unknown
    uint8_t why;                // APPS_REC_WHY_*
} APPS_REC_ROW;

typedef struct __attribute__((packed)) {
    int32_t pid;
    int32_t ppid;
    uint64_t starttime;
    uint32_t uid;
    uint16_t comm;
    uint16_t exe;
    uint16_t group;
    uint16_t cmdline;
} APPS_REC_START;

typedef struct __attribute__((packed)) {
    int32_t pid;
    uint64_t starttime;
    uint64_t exit_ut;           // realtime of the exit, when APPS_REC_EXIT_STATUS
    int32_t status;             // the wait status (exit code << 8, or the signal), when APPS_REC_EXIT_STATUS
    uint8_t flags;              // APPS_REC_EXIT_*
} APPS_REC_EXIT;

// a regular file a row holds open for writing
typedef struct __attribute__((packed)) {
    uint16_t row;               // index of the row in its frame
    uint16_t path;              // string ids
    uint16_t mount;             // the mount point of its filesystem, APPS_REC_STRING_NONE when unknown
    uint8_t flags;              // APPS_REC_FILE_*
    uint64_t size;              // bytes
} APPS_REC_FILE;

// a TCP connection that opened or closed
typedef struct __attribute__((packed)) {
    int32_t pid;                // 0 when the process is unknown
    uint64_t starttime;
    uint16_t comm;              // string id, APPS_REC_STRING_NONE when the process is unknown
    uint8_t event;              // APPS_REC_CONN_OPEN, APPS_REC_CONN_CLOSE
    uint8_t how;                // APPS_REC_CONN_BY_*
    uint8_t family;             // AF_INET, AF_INET6
    uint8_t state;              // the TCP state (linux/tcp.h numbering) when seen
    uint8_t direction;          // APPS_REC_CONN_* direction bits
    uint8_t reserved;
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t local_ip[16];       // IPv4 in the first 4 bytes, network byte order
    uint8_t remote_ip[16];
    uint64_t bytes_sent;        // acknowledged by the peer (tcpi_bytes_acked); at a close, the final count
    uint64_t bytes_received;    // tcpi_bytes_received
} APPS_REC_CONN;

typedef struct __attribute__((packed)) {
    uint64_t time_ut;           // realtime of the sample
    uint64_t boot_ut;           // realtime of the boot: starttime / system HZ is the time since then
    uint32_t pids;              // running processes
    uint32_t host_cpu;          // 1/10000 of a core, from the system totals (0 when unknown)
    uint32_t procs_cpu;         // 1/10000 of a core, all processes
    uint32_t rows_cpu;          // 1/10000 of a core, the rows of this frame
    uint64_t procs_rss_kib;     // all processes
    uint16_t strings;           // new strings that precede this frame
    uint16_t starts;
    uint16_t exits;
    uint16_t rows;
    uint16_t files;
    uint16_t conns;
    uint16_t conns_lost;        // connection events since the previous frame not recorded (over the cap, kernel overrun)
    uint16_t netns_other;       // processes with sockets in network namespaces the socket dumps do not see
    uint16_t proc_lost;         // times the kernel dropped process events since the previous frame (each at least one)
    uint16_t proc_unnamed;      // processes since the previous frame whose name and owner were not read (CPU budget)
    uint16_t flags;             // APPS_REC_FRAME_*
} APPS_REC_FRAME_HDR;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;             // APPS_REC_CHUNK_FLAG_*
    uint64_t first_ut;
    uint64_t last_ut;
    uint32_t frames;
    uint32_t raw_len;           // payload length when decompressed
    uint32_t stored_len;        // payload length as stored after this header
    uint32_t crc;               // crc32 of this header (with crc = 0) and the stored payload
    uint8_t boot_id[16];
} APPS_REC_CHUNK_HDR;

// Payload layout, per frame:
//   APPS_REC_FRAME_HDR
//   strings x { uint16_t len (including the terminating NUL); char bytes[len] }
//   starts  x APPS_REC_START
//   exits   x APPS_REC_EXIT
//   rows    x APPS_REC_ROW
//   files   x APPS_REC_FILE
//   conns   x APPS_REC_CONN
// String ids are assigned in order of appearance within the chunk, starting at 0.

// --------------------------------------------------------------------------------------------------------------------
// building chunks

typedef struct apps_rec_builder {
    uint8_t boot_id[16];        // stamped on every chunk sealed
    BUFFER *payload;
    BUFFER *pending_strings;    // strings added since the last frame
    uint16_t pending_count;

    STRING **interned;          // open addressing, keyed by STRING pointer; holds a reference to each key
    uint16_t *interned_ids;
    size_t interned_size;       // power of 2
    size_t strings;             // ids assigned in this chunk

    uint32_t frames;
    uint64_t first_ut;
    uint64_t last_ut;
} APPS_REC_BUILDER;

void apps_rec_builder_init(APPS_REC_BUILDER *b, const uint8_t boot_id[16]);
void apps_rec_builder_destroy(APPS_REC_BUILDER *b);

// the id of a string in the chunk being built; APPS_REC_STRING_NONE for NULL
uint16_t apps_rec_builder_string(APPS_REC_BUILDER *b, STRING *s);

// true when the chunk cannot take another frame and must be sealed first
bool apps_rec_builder_full(const APPS_REC_BUILDER *b, size_t new_strings_needed);

void apps_rec_builder_add_frame(
    APPS_REC_BUILDER *b, const APPS_REC_FRAME_HDR *totals,
    const APPS_REC_START *starts, uint16_t nstarts,
    const APPS_REC_EXIT *exits, uint16_t nexits,
    const APPS_REC_ROW *rows, uint16_t nrows,
    const APPS_REC_FILE *files, uint16_t nfiles,
    const APPS_REC_CONN *conns, uint16_t nconns);

// seals the chunk being built and resets the builder; NULL when it has no frames.
// The returned block is a header followed by the stored payload; free it with freez().
APPS_REC_CHUNK_HDR *apps_rec_builder_seal(APPS_REC_BUILDER *b, size_t *size);

// --------------------------------------------------------------------------------------------------------------------
// reading chunks

typedef struct {
    const uint8_t *boot_id;     // 16 bytes: pid + starttime identify a process only within one boot
    const APPS_REC_FRAME_HDR *hdr;
    const APPS_REC_START *starts;
    const APPS_REC_EXIT *exits;
    const APPS_REC_ROW *rows;
    const APPS_REC_FILE *files;
    const APPS_REC_CONN *conns;
} APPS_REC_FRAME_VIEW;

typedef struct {
    const char **str;
    size_t used;
    size_t size;
} APPS_REC_STRINGS;

static inline const char *apps_rec_string(const APPS_REC_STRINGS *s, uint16_t id) {
    return (id < s->used) ? s->str[id] : NULL;
}

// return false to stop the walk
typedef bool (*apps_rec_frame_cb_t)(const APPS_REC_FRAME_VIEW *f, const APPS_REC_STRINGS *strings, void *data);

// checks the header, the lengths and the checksum of a stored chunk
bool apps_rec_chunk_valid(const APPS_REC_CHUNK_HDR *hdr, size_t size);

// walks the frames of a sealed chunk; false when the chunk is invalid or corrupt
bool apps_rec_chunk_foreach_frame(const APPS_REC_CHUNK_HDR *hdr, size_t size, apps_rec_frame_cb_t cb, void *data);

// walks the frames of the chunk being built
bool apps_rec_builder_foreach_frame(const APPS_REC_BUILDER *b, apps_rec_frame_cb_t cb, void *data);

#endif //NETDATA_APPS_RECORDER_FORMAT_H
