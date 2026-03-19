// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_SEQLOCK_H
#define NETDATA_SEQLOCK_H

#include "spinlock.h"

// SEQLOCK - a sequence lock for single-writer or multi-writer, multiple-reader scenarios.
//
// Writers acquire the spinlock for mutual exclusion, then increment the generation to odd
// (signalling "write in progress"), update the data, and increment to even (signalling "done").
//
// Readers never block. They read the generation, copy the data, then check if the generation
// changed. If it did (or was odd), they retry. This gives lock-free reads with guaranteed
// consistency for small data (ideally <= 1 cache line / 64 bytes).
//
// Usage:
//
//   Writer:
//     seqlock_write_begin(&sl);
//     ... update shared data ...
//     seqlock_write_end(&sl);
//
//   Reader:
//     uint64_t gen;
//     do {
//         gen = seqlock_read_begin(&sl);
//         ... copy shared data to local variables ...
//     } while(seqlock_read_retry(&sl, gen));

typedef struct seqlock {
    uint64_t generation;
    SPINLOCK spinlock;
} SEQLOCK;

#define SEQLOCK_INITIALIZER { .generation = 0, .spinlock = SPINLOCK_INITIALIZER }

static inline void seqlock_init(SEQLOCK *sl) {
    sl->generation = 0;
    spinlock_init(&sl->spinlock);
}

// --- writer API (mutually exclusive via spinlock) ---

static inline void seqlock_write_begin(SEQLOCK *sl) {
    spinlock_lock(&sl->spinlock);
    __atomic_add_fetch(&sl->generation, 1, __ATOMIC_ACQ_REL); // odd = write in progress
    // ACQ_REL: the ACQUIRE half prevents subsequent payload stores from being
    // reordered before this increment on weakly-ordered architectures (ARM, POWER).
    // Without it, a reader could see updated data while generation is still even.
}

static inline void seqlock_write_end(SEQLOCK *sl) {
    __atomic_add_fetch(&sl->generation, 1, __ATOMIC_RELEASE); // even = write complete
    spinlock_unlock(&sl->spinlock);
}

// --- reader API (lock-free, may retry) ---

static inline uint64_t seqlock_read_begin(SEQLOCK *sl) {
    uint64_t gen;
    while(unlikely((gen = __atomic_load_n(&sl->generation, __ATOMIC_ACQUIRE)) & 1))
        ; // writer active — spin briefly
    return gen;
}

static inline bool seqlock_read_retry(SEQLOCK *sl, uint64_t start_gen) {
    uint64_t end_gen = __atomic_load_n(&sl->generation, __ATOMIC_ACQUIRE);
    return unlikely(start_gen != end_gen);
}

#endif //NETDATA_SEQLOCK_H
