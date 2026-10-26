// SPDX-License-Identifier: GPL-3.0-or-later

#include "apps_recorder_format.h"

#ifdef ENABLE_ZSTD
#include <zstd.h>
#define APPS_REC_ZSTD_LEVEL 1
#endif

#define APPS_REC_INTERN_INITIAL 256

// --------------------------------------------------------------------------------------------------------------------
// building chunks

void apps_rec_builder_init(APPS_REC_BUILDER *b, const uint8_t boot_id[16]) {
    memset(b, 0, sizeof(*b));
    memcpy(b->boot_id, boot_id, sizeof(b->boot_id));
    b->payload = buffer_create(64 * 1024, NULL);
    b->pending_strings = buffer_create(4 * 1024, NULL);
    b->interned_size = APPS_REC_INTERN_INITIAL;
    b->interned = callocz(b->interned_size, sizeof(*b->interned));
    b->interned_ids = callocz(b->interned_size, sizeof(*b->interned_ids));
}

static void builder_reset(APPS_REC_BUILDER *b) {
    for(size_t i = 0; i < b->interned_size; i++) {
        string_freez(b->interned[i]);
        b->interned[i] = NULL;
    }

    buffer_flush(b->payload);
    buffer_flush(b->pending_strings);
    b->pending_count = 0;
    b->strings = 0;
    b->frames = 0;
    b->first_ut = 0;
    b->last_ut = 0;
}

void apps_rec_builder_destroy(APPS_REC_BUILDER *b) {
    if(!b->payload)
        return;

    builder_reset(b);
    buffer_free(b->payload);
    buffer_free(b->pending_strings);
    freez(b->interned);
    freez(b->interned_ids);
    memset(b, 0, sizeof(*b));
}

static inline size_t intern_slot(STRING *s, size_t size) {
    uint64_t h = (uint64_t)(uintptr_t)s;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (size_t)h & (size - 1);
}

static void intern_grow(APPS_REC_BUILDER *b) {
    size_t old_size = b->interned_size;
    STRING **old = b->interned;
    uint16_t *old_ids = b->interned_ids;

    b->interned_size = old_size * 2;
    b->interned = callocz(b->interned_size, sizeof(*b->interned));
    b->interned_ids = callocz(b->interned_size, sizeof(*b->interned_ids));

    for(size_t i = 0; i < old_size; i++) {
        if(!old[i]) continue;
        size_t h = intern_slot(old[i], b->interned_size);
        while(b->interned[h]) h = (h + 1) & (b->interned_size - 1);
        b->interned[h] = old[i];
        b->interned_ids[h] = old_ids[i];
    }

    freez(old);
    freez(old_ids);
}

uint16_t apps_rec_builder_string(APPS_REC_BUILDER *b, STRING *s) {
    if(!s)
        return APPS_REC_STRING_NONE;

    size_t h = intern_slot(s, b->interned_size);
    while(b->interned[h]) {
        if(b->interned[h] == s)
            return b->interned_ids[h];
        h = (h + 1) & (b->interned_size - 1);
    }

    if(b->strings >= APPS_REC_STRINGS_MAX)
        return APPS_REC_STRING_NONE;

    // a reference keeps the pointer from being reused for another string while the chunk is open
    uint16_t id = (uint16_t)b->strings++;
    b->interned[h] = string_dup(s);
    b->interned_ids[h] = id;

    const char *str = string2str(s);
    size_t len = MIN(string_strlen(s), (size_t)APPS_REC_STRING_LEN_MAX);
    uint16_t stored = (uint16_t)(len + 1);
    char nul = '\0';
    buffer_memcat(b->pending_strings, &stored, sizeof(stored));
    buffer_memcat(b->pending_strings, str, len);
    buffer_memcat(b->pending_strings, &nul, 1);
    b->pending_count++;

    if(b->strings * 2 > b->interned_size)
        intern_grow(b);

    return id;
}

bool apps_rec_builder_full(const APPS_REC_BUILDER *b, size_t new_strings_needed) {
    return b->frames >= APPS_REC_FRAMES_PER_CHUNK ||
           b->strings + new_strings_needed > APPS_REC_STRINGS_MAX ||
           buffer_strlen(b->payload) >= APPS_REC_RAW_MAX / 2;
}

void apps_rec_builder_add_frame(
    APPS_REC_BUILDER *b, const APPS_REC_FRAME_HDR *totals,
    const APPS_REC_START *starts, uint16_t nstarts,
    const APPS_REC_EXIT *exits, uint16_t nexits,
    const APPS_REC_ROW *rows, uint16_t nrows) {

    APPS_REC_FRAME_HDR h = *totals;
    h.strings = b->pending_count;
    h.starts = nstarts;
    h.exits = nexits;
    h.rows = nrows;

    buffer_memcat(b->payload, &h, sizeof(h));
    buffer_memcat(b->payload, buffer_tostring(b->pending_strings), buffer_strlen(b->pending_strings));
    if(nstarts) buffer_memcat(b->payload, starts, nstarts * sizeof(*starts));
    if(nexits) buffer_memcat(b->payload, exits, nexits * sizeof(*exits));
    if(nrows) buffer_memcat(b->payload, rows, nrows * sizeof(*rows));

    buffer_flush(b->pending_strings);
    b->pending_count = 0;

    if(!b->frames)
        b->first_ut = h.time_ut;
    b->last_ut = h.time_ut;
    b->frames++;
}

static uint32_t chunk_crc(const APPS_REC_CHUNK_HDR *hdr) {
    APPS_REC_CHUNK_HDR h = *hdr;
    h.crc = 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)&h, (uInt)sizeof(h));
    crc = crc32(crc, (const Bytef *)hdr + sizeof(*hdr), (uInt)hdr->stored_len);
    return (uint32_t)crc;
}

APPS_REC_CHUNK_HDR *apps_rec_builder_seal(APPS_REC_BUILDER *b, size_t *size) {
    *size = 0;
    if(!b->frames)
        return NULL;

    size_t raw_len = buffer_strlen(b->payload);
    if(raw_len > APPS_REC_RAW_MAX) {
        // apps_rec_builder_full() seals long before this; a chunk no reader would accept is not kept
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "apps.plugin recorder: dropping a %zu bytes chunk", raw_len);
        builder_reset(b);
        return NULL;
    }

    const void *raw = buffer_tostring(b->payload);
    APPS_REC_CHUNK_HDR *hdr = NULL;
    size_t stored_len = raw_len;
    uint16_t flags = 0;

#ifdef ENABLE_ZSTD
    size_t bound = ZSTD_compressBound(raw_len);
    hdr = mallocz(sizeof(*hdr) + bound);
    size_t z = ZSTD_compress((uint8_t *)hdr + sizeof(*hdr), bound, raw, raw_len, APPS_REC_ZSTD_LEVEL);
    if(!ZSTD_isError(z) && z < raw_len) {
        stored_len = z;
        flags |= APPS_REC_CHUNK_FLAG_ZSTD;
        hdr = reallocz(hdr, sizeof(*hdr) + stored_len);
    }
    else {
        hdr = reallocz(hdr, sizeof(*hdr) + raw_len);
        memcpy((uint8_t *)hdr + sizeof(*hdr), raw, raw_len);
    }
#else
    hdr = mallocz(sizeof(*hdr) + raw_len);
    memcpy((uint8_t *)hdr + sizeof(*hdr), raw, raw_len);
#endif

    hdr->magic = APPS_REC_MAGIC;
    hdr->version = APPS_REC_VERSION;
    hdr->flags = flags;
    hdr->first_ut = b->first_ut;
    hdr->last_ut = b->last_ut;
    hdr->frames = b->frames;
    hdr->raw_len = (uint32_t)raw_len;
    hdr->stored_len = (uint32_t)stored_len;
    memcpy(hdr->boot_id, b->boot_id, sizeof(hdr->boot_id));
    hdr->crc = chunk_crc(hdr);

    *size = sizeof(*hdr) + stored_len;
    builder_reset(b);
    return hdr;
}

// --------------------------------------------------------------------------------------------------------------------
// reading chunks

static bool strings_add(APPS_REC_STRINGS *s, const char *str) {
    if(s->used >= APPS_REC_STRINGS_MAX)
        return false;

    if(s->used == s->size) {
        s->size = s->size ? s->size * 2 : 256;
        s->str = reallocz(s->str, s->size * sizeof(*s->str));
    }
    s->str[s->used++] = str;
    return true;
}

// walks a decompressed payload; every count and length is checked against the payload bounds
static bool payload_foreach_frame(const uint8_t *p, size_t len, size_t expected_frames, const uint8_t *boot_id,
                                  apps_rec_frame_cb_t cb, void *data) {
    const uint8_t *end = p + len;
    APPS_REC_STRINGS strings = { 0 };
    size_t frames = 0;
    bool ok = true;

    while(p < end) {
        if((size_t)(end - p) < sizeof(APPS_REC_FRAME_HDR)) { ok = false; break; }
        const APPS_REC_FRAME_HDR *hdr = (const APPS_REC_FRAME_HDR *)p;
        p += sizeof(*hdr);

        for(uint16_t i = 0; i < hdr->strings; i++) {
            uint16_t slen;
            if((size_t)(end - p) < sizeof(slen)) { ok = false; break; }
            memcpy(&slen, p, sizeof(slen));
            p += sizeof(slen);
            if(!slen || slen > APPS_REC_STRING_LEN_MAX + 1 || (size_t)(end - p) < slen || p[slen - 1] != '\0' ||
                !strings_add(&strings, (const char *)p)) { ok = false; break; }
            p += slen;
        }
        if(!ok) break;

        size_t body = (size_t)hdr->starts * sizeof(APPS_REC_START) +
                      (size_t)hdr->exits * sizeof(APPS_REC_EXIT) +
                      (size_t)hdr->rows * sizeof(APPS_REC_ROW);
        if((size_t)(end - p) < body) { ok = false; break; }

        APPS_REC_FRAME_VIEW f = { .boot_id = boot_id, .hdr = hdr };
        f.starts = (const APPS_REC_START *)p;
        p += (size_t)hdr->starts * sizeof(APPS_REC_START);
        f.exits = (const APPS_REC_EXIT *)p;
        p += (size_t)hdr->exits * sizeof(APPS_REC_EXIT);
        f.rows = (const APPS_REC_ROW *)p;
        p += (size_t)hdr->rows * sizeof(APPS_REC_ROW);
        frames++;

        if(!cb(&f, &strings, data))
            break;
    }

    if(ok && p == end && expected_frames && frames != expected_frames)
        ok = false;

    freez(strings.str);
    return ok;
}

bool apps_rec_chunk_valid(const APPS_REC_CHUNK_HDR *hdr, size_t size) {
    if(!hdr || size < sizeof(*hdr))
        return false;

    if(hdr->magic != APPS_REC_MAGIC || hdr->version != APPS_REC_VERSION)
        return false;

    if(hdr->stored_len != size - sizeof(*hdr) || !hdr->frames || hdr->frames > APPS_REC_FRAMES_PER_CHUNK ||
        hdr->first_ut > hdr->last_ut || hdr->raw_len > APPS_REC_RAW_MAX)
        return false;

    if(hdr->flags & APPS_REC_CHUNK_FLAG_ZSTD) {
#ifdef ENABLE_ZSTD
        unsigned long long content = ZSTD_getFrameContentSize((const uint8_t *)hdr + sizeof(*hdr), hdr->stored_len);
        if(content != hdr->raw_len)
            return false;
#else
        return false;
#endif
    }
    else if(hdr->raw_len != hdr->stored_len)
        return false;

    return chunk_crc(hdr) == hdr->crc;
}

bool apps_rec_chunk_foreach_frame(const APPS_REC_CHUNK_HDR *hdr, size_t size, apps_rec_frame_cb_t cb, void *data) {
    if(!apps_rec_chunk_valid(hdr, size))
        return false;

    const uint8_t *stored = (const uint8_t *)hdr + sizeof(*hdr);

    if(!(hdr->flags & APPS_REC_CHUNK_FLAG_ZSTD))
        return payload_foreach_frame(stored, hdr->stored_len, hdr->frames, hdr->boot_id, cb, data);

#ifdef ENABLE_ZSTD
    uint8_t *raw = mallocz(hdr->raw_len ? hdr->raw_len : 1);
    size_t got = ZSTD_decompress(raw, hdr->raw_len, stored, hdr->stored_len);
    bool ok = !ZSTD_isError(got) && got == hdr->raw_len &&
              payload_foreach_frame(raw, got, hdr->frames, hdr->boot_id, cb, data);
    freez(raw);
    return ok;
#else
    return false;
#endif
}

bool apps_rec_builder_foreach_frame(const APPS_REC_BUILDER *b, apps_rec_frame_cb_t cb, void *data) {
    if(!b->frames)
        return true;

    return payload_foreach_frame(
        (const uint8_t *)buffer_tostring(b->payload), buffer_strlen(b->payload), b->frames, b->boot_id, cb, data);
}
