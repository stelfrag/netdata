// SPDX-License-Identifier: GPL-3.0-or-later

#include "../apps_recorder_format.h"

static const uint8_t boot_id[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

#define FRAMES 3

typedef struct {
    APPS_REC_FRAME_HDR hdr;
    APPS_REC_START starts[2];
    APPS_REC_EXIT exits[2];
    APPS_REC_ROW rows[3];
    const char *start_comm[2];
    const char *row_comm[3];
    const char *row_exe[3];
} EXPECTED_FRAME;

typedef struct {
    const EXPECTED_FRAME *expected;
    size_t frames;
    size_t stop_after;
    bool failed;
} WALK;

static bool fail(WALK *w, const char *msg, size_t frame) {
    fprintf(stderr, "frame %zu: %s\n", frame, msg);
    w->failed = true;
    return false;
}

static bool same_str(const char *a, const char *b) {
    if(!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

static bool check_frame(const APPS_REC_FRAME_VIEW *f, const APPS_REC_STRINGS *strings, void *data) {
    WALK *w = data;
    size_t i = w->frames;
    const EXPECTED_FRAME *e = &w->expected[i];

    if(f->hdr->time_ut != e->hdr.time_ut || f->hdr->pids != e->hdr.pids || f->hdr->host_cpu != e->hdr.host_cpu ||
        f->hdr->procs_cpu != e->hdr.procs_cpu || f->hdr->rows_cpu != e->hdr.rows_cpu ||
        f->hdr->procs_rss_kib != e->hdr.procs_rss_kib)
        return fail(w, "totals differ", i);

    if(f->hdr->starts != e->hdr.starts || f->hdr->exits != e->hdr.exits || f->hdr->rows != e->hdr.rows)
        return fail(w, "counts differ", i);

    for(uint16_t k = 0; k < f->hdr->starts; k++) {
        APPS_REC_START got = f->starts[k];
        if(got.pid != e->starts[k].pid || got.ppid != e->starts[k].ppid || got.starttime != e->starts[k].starttime ||
            got.uid != e->starts[k].uid)
            return fail(w, "start differs", i);
        if(!same_str(apps_rec_string(strings, got.comm), e->start_comm[k]))
            return fail(w, "start comm differs", i);
    }

    for(uint16_t k = 0; k < f->hdr->exits; k++) {
        APPS_REC_EXIT got = f->exits[k];
        if(got.pid != e->exits[k].pid || got.starttime != e->exits[k].starttime)
            return fail(w, "exit differs", i);
    }

    for(uint16_t k = 0; k < f->hdr->rows; k++) {
        APPS_REC_ROW got = f->rows[k];
        const APPS_REC_ROW *x = &e->rows[k];
        if(got.pid != x->pid || got.ppid != x->ppid || got.starttime != x->starttime || got.uid != x->uid ||
            got.cpu != x->cpu || got.rss_kib != x->rss_kib || got.swap_kib != x->swap_kib ||
            got.lread_kibs != x->lread_kibs || got.lwrite_kibs != x->lwrite_kibs || got.pread_kibs != x->pread_kibs ||
            got.pwrite_kibs != x->pwrite_kibs || got.threads != x->threads || got.state != x->state ||
            got.why != x->why || got.cmdline != APPS_REC_STRING_NONE)
            return fail(w, "row differs", i);
        if(!same_str(apps_rec_string(strings, got.comm), e->row_comm[k]) ||
            !same_str(apps_rec_string(strings, got.exe), e->row_exe[k]))
            return fail(w, "row strings differ", i);
        if(apps_rec_string(strings, got.group))
            return fail(w, "row group should be unset", i);
    }

    w->frames++;
    return w->frames != w->stop_after;
}

static void make_frames(APPS_REC_BUILDER *b, EXPECTED_FRAME *e) {
    STRING *bash = string_strdupz("bash");
    STRING *cc1 = string_strdupz("cc1");
    STRING *bin_bash = string_strdupz("/usr/bin/bash");

    for(size_t i = 0; i < FRAMES; i++) {
        EXPECTED_FRAME *f = &e[i];
        memset(f, 0, sizeof(*f));
        f->hdr = (APPS_REC_FRAME_HDR){
            .time_ut = 1000000ULL * (i + 1), .pids = 300 + i, .host_cpu = 12000, .procs_cpu = 11000,
            .rows_cpu = 10500, .procs_rss_kib = 1ULL << 33,
        };

        // frame 1 has no starts, frame 2 exits only
        f->hdr.starts = (i == 0) ? 2 : 0;
        f->hdr.exits = (i == 2) ? 2 : 0;
        f->hdr.rows = 3;

        for(uint16_t k = 0; k < f->hdr.starts; k++) {
            STRING *comm = k ? cc1 : bash;
            f->starts[k] = (APPS_REC_START){
                .pid = 100 + k, .ppid = 1, .starttime = 5000 + k, .uid = 1000,
                .comm = apps_rec_builder_string(b, comm), .exe = APPS_REC_STRING_NONE,
                .group = APPS_REC_STRING_NONE, .cmdline = APPS_REC_STRING_NONE,
            };
            f->start_comm[k] = k ? "cc1" : "bash";
        }

        for(uint16_t k = 0; k < f->hdr.exits; k++)
            f->exits[k] = (APPS_REC_EXIT){ .pid = 200 + k, .starttime = 7000 + k };

        for(uint16_t k = 0; k < f->hdr.rows; k++) {
            STRING *comm = (k == 1) ? cc1 : bash;
            STRING *exe = (k == 2) ? NULL : bin_bash;
            f->rows[k] = (APPS_REC_ROW){
                .pid = 100 + k, .ppid = 1, .starttime = 5000 + k, .uid = 1000 + k,
                .comm = apps_rec_builder_string(b, comm), .exe = apps_rec_builder_string(b, exe),
                .group = APPS_REC_STRING_NONE, .cmdline = APPS_REC_STRING_NONE,
                .cpu = 10000 * (k + 1), .rss_kib = 4096 * (k + 1), .swap_kib = k, .lread_kibs = 10, .lwrite_kibs = 20,
                .pread_kibs = 30, .pwrite_kibs = 40, .threads = 7, .state = 'R', .why = APPS_REC_WHY_CPU,
            };
            f->row_comm[k] = (k == 1) ? "cc1" : "bash";
            f->row_exe[k] = exe ? "/usr/bin/bash" : NULL;
        }

        // expected strings are literals: the STRING copies are released when the chunk is sealed
        apps_rec_builder_add_frame(b, &f->hdr, f->starts, f->hdr.starts, f->exits, f->hdr.exits,
                                   f->rows, f->hdr.rows);
    }

    // the builder holds its own references
    string_freez(bash);
    string_freez(cc1);
    string_freez(bin_bash);
}

static bool test_round_trip(void) {
    APPS_REC_BUILDER b;
    apps_rec_builder_init(&b);
    EXPECTED_FRAME e[FRAMES];
    make_frames(&b, e);

    if(b.strings != 3) {
        fprintf(stderr, "round trip: expected 3 interned strings, got %zu\n", b.strings);
        return false;
    }

    WALK w = { .expected = e };
    if(!apps_rec_builder_foreach_frame(&b, check_frame, &w) || w.failed || w.frames != FRAMES) {
        fprintf(stderr, "round trip: builder walk failed\n");
        return false;
    }

    size_t size;
    APPS_REC_CHUNK_HDR *hdr = apps_rec_builder_seal(&b, boot_id, &size);
    if(!hdr || hdr->frames != FRAMES || hdr->first_ut != e[0].hdr.time_ut || hdr->last_ut != e[FRAMES - 1].hdr.time_ut ||
        memcmp(hdr->boot_id, boot_id, sizeof(boot_id)) != 0) {
        fprintf(stderr, "round trip: bad sealed header\n");
        return false;
    }

    if(b.frames || b.strings || buffer_strlen(b.payload)) {
        fprintf(stderr, "round trip: builder not reset after seal\n");
        return false;
    }

    w = (WALK){ .expected = e };
    if(!apps_rec_chunk_foreach_frame(hdr, size, check_frame, &w) || w.failed || w.frames != FRAMES) {
        fprintf(stderr, "round trip: sealed walk failed\n");
        return false;
    }

    // a callback that stops early is not an error
    w = (WALK){ .expected = e, .stop_after = 1 };
    if(!apps_rec_chunk_foreach_frame(hdr, size, check_frame, &w) || w.failed || w.frames != 1) {
        fprintf(stderr, "round trip: early stop failed\n");
        return false;
    }

    // an empty builder seals to nothing
    if(apps_rec_builder_seal(&b, boot_id, &size) || size) {
        fprintf(stderr, "round trip: empty builder sealed a chunk\n");
        return false;
    }

    freez(hdr);
    apps_rec_builder_destroy(&b);
    return true;
}

static bool count_frames(const APPS_REC_FRAME_VIEW *f, const APPS_REC_STRINGS *strings, void *data) {
    (void)f; (void)strings;
    (*(size_t *)data)++;
    return true;
}

static bool test_corruption(void) {
    APPS_REC_BUILDER b;
    apps_rec_builder_init(&b);
    EXPECTED_FRAME e[FRAMES];
    make_frames(&b, e);
    size_t size;
    APPS_REC_CHUNK_HDR *hdr = apps_rec_builder_seal(&b, boot_id, &size);
    bool ok = true;

    if(!apps_rec_chunk_valid(hdr, size)) {
        fprintf(stderr, "corruption: intact chunk is invalid\n");
        ok = false;
    }

    if(apps_rec_chunk_valid(hdr, size - 1)) {
        fprintf(stderr, "corruption: truncated chunk is valid\n");
        ok = false;
    }

    if(apps_rec_chunk_valid(hdr, sizeof(*hdr) - 1)) {
        fprintf(stderr, "corruption: short header is valid\n");
        ok = false;
    }

    uint8_t *payload = (uint8_t *)hdr + sizeof(*hdr);
    payload[size - sizeof(*hdr) - 1] ^= 0xff;
    size_t frames = 0;
    if(apps_rec_chunk_valid(hdr, size) || apps_rec_chunk_foreach_frame(hdr, size, count_frames, &frames) || frames) {
        fprintf(stderr, "corruption: flipped payload byte not detected\n");
        ok = false;
    }
    payload[size - sizeof(*hdr) - 1] ^= 0xff;

    hdr->version++;
    if(apps_rec_chunk_valid(hdr, size)) {
        fprintf(stderr, "corruption: unknown version accepted\n");
        ok = false;
    }
    hdr->version--;

    hdr->magic ^= 1;
    if(apps_rec_chunk_valid(hdr, size)) {
        fprintf(stderr, "corruption: bad magic accepted\n");
        ok = false;
    }

    freez(hdr);
    apps_rec_builder_destroy(&b);
    return ok;
}

// a chunk with a valid checksum but counts that point past the payload
static APPS_REC_CHUNK_HDR *raw_chunk(const void *payload, size_t len, uint32_t frames, size_t *size) {
    APPS_REC_CHUNK_HDR *hdr = callocz(1, sizeof(*hdr) + len);
    memcpy((uint8_t *)hdr + sizeof(*hdr), payload, len);
    hdr->magic = APPS_REC_MAGIC;
    hdr->version = APPS_REC_VERSION;
    hdr->first_ut = hdr->last_ut = 1;
    hdr->frames = frames;
    hdr->raw_len = hdr->stored_len = (uint32_t)len;
    hdr->crc = (uint32_t)crc32(crc32(0L, Z_NULL, 0), payload, (uInt)len);
    *size = sizeof(*hdr) + len;
    return hdr;
}

static bool expect_rejected(const char *name, const void *payload, size_t len, uint32_t frames) {
    size_t size, seen = 0;
    APPS_REC_CHUNK_HDR *hdr = raw_chunk(payload, len, frames, &size);
    bool valid = apps_rec_chunk_valid(hdr, size);
    bool walked = apps_rec_chunk_foreach_frame(hdr, size, count_frames, &seen);
    freez(hdr);

    if(!valid) {
        fprintf(stderr, "%s: checksum-valid chunk rejected by header check\n", name);
        return false;
    }
    if(walked) {
        fprintf(stderr, "%s: malformed payload accepted (%zu frames)\n", name, seen);
        return false;
    }
    return true;
}

static bool test_bounds(void) {
    bool ok = true;

    struct __attribute__((packed)) {
        APPS_REC_FRAME_HDR h;
        APPS_REC_ROW row;
    } rows_past_end = { .h = { .time_ut = 1, .rows = 5 } };
    ok &= expect_rejected("rows past end", &rows_past_end, sizeof(rows_past_end), 1);

    struct __attribute__((packed)) {
        APPS_REC_FRAME_HDR h;
        uint16_t len;
        char s[4];
    } string_past_end = { .h = { .time_ut = 1, .strings = 1 }, .len = 100, .s = "abc" };
    ok &= expect_rejected("string past end", &string_past_end, sizeof(string_past_end), 1);

    struct __attribute__((packed)) {
        APPS_REC_FRAME_HDR h;
        uint16_t len;
        char s[3];
    } string_no_nul = { .h = { .time_ut = 1, .strings = 1 }, .len = 3, .s = { 'a', 'b', 'c' } };
    ok &= expect_rejected("string without NUL", &string_no_nul, sizeof(string_no_nul), 1);

    APPS_REC_FRAME_HDR one = { .time_ut = 1 };
    ok &= expect_rejected("frame count mismatch", &one, sizeof(one), 2);

    uint8_t partial[sizeof(APPS_REC_FRAME_HDR) + 3] = { 0 };
    ok &= expect_rejected("trailing partial header", partial, sizeof(partial), 1);

    return ok;
}

static bool test_limits(void) {
    APPS_REC_BUILDER b;
    apps_rec_builder_init(&b);
    bool ok = true;

    // ids stay stable while the intern table grows
    STRING *s[1000];
    uint16_t ids[1000];
    for(size_t i = 0; i < 1000; i++) {
        char name[32];
        snprintfz(name, sizeof(name), "proc-%zu", i);
        s[i] = string_strdupz(name);
        ids[i] = apps_rec_builder_string(&b, s[i]);
    }
    for(size_t i = 0; i < 1000; i++) {
        if(apps_rec_builder_string(&b, s[i]) != ids[i] || ids[i] != i) {
            fprintf(stderr, "limits: id of string %zu changed\n", i);
            ok = false;
            break;
        }
    }

    if(apps_rec_builder_full(&b, 0) || !apps_rec_builder_full(&b, APPS_REC_STRINGS_MAX)) {
        fprintf(stderr, "limits: string capacity check is wrong\n");
        ok = false;
    }

    APPS_REC_FRAME_HDR h = { 0 };
    for(size_t i = 0; i < APPS_REC_FRAMES_PER_CHUNK; i++) {
        if(apps_rec_builder_full(&b, 0)) {
            fprintf(stderr, "limits: full after %zu frames\n", i);
            ok = false;
            break;
        }
        h.time_ut = i + 1;
        apps_rec_builder_add_frame(&b, &h, NULL, 0, NULL, 0, NULL, 0);
    }
    if(!apps_rec_builder_full(&b, 0)) {
        fprintf(stderr, "limits: not full after %d frames\n", APPS_REC_FRAMES_PER_CHUNK);
        ok = false;
    }

    // all 1000 strings went out with the first frame
    size_t size;
    APPS_REC_CHUNK_HDR *hdr = apps_rec_builder_seal(&b, boot_id, &size);
    size_t frames = 0;
    if(!apps_rec_chunk_foreach_frame(hdr, size, count_frames, &frames) || frames != APPS_REC_FRAMES_PER_CHUNK) {
        fprintf(stderr, "limits: full chunk does not decode (%zu frames)\n", frames);
        ok = false;
    }

    freez(hdr);
    for(size_t i = 0; i < 1000; i++)
        string_freez(s[i]);
    apps_rec_builder_destroy(&b);
    return ok;
}

int main(void) {
    bool ok = true;
    ok &= test_round_trip();
    ok &= test_corruption();
    ok &= test_bounds();
    ok &= test_limits();

    fprintf(stderr, "%s\n", ok ? "apps-recorder-format-test: OK" : "apps-recorder-format-test: FAILED");
    return ok ? 0 : 1;
}
