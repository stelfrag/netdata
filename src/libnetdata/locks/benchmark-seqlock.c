// SPDX-License-Identifier: GPL-3.0-or-later

#include "benchmark-seqlock.h"

#define MAX_THREADS 64
#define TEST_DURATION_SEC 1
#define STOP_SIGNAL UINT64_MAX
#define MAX_CONFIGS 12

// Shared data protected by the seqlock — two fields that must be read consistently.
// The writer sets them to complementary values (a + b == CONSISTENCY_MAGIC),
// so a torn read is detected when a + b != CONSISTENCY_MAGIC.
#define CONSISTENCY_MAGIC 0xDEADBEEFULL

typedef struct {
    uint64_t a;
    uint64_t b;
} shared_data_t;

// --------------------------------------------------------------------------
// control structures

typedef enum {
    THREAD_READER,
    THREAD_WRITER
} thread_type_t;

typedef struct {
    uint64_t operations;
    uint64_t violations;        // torn reads detected
    usec_t test_time;
    volatile int ready;
} thread_stats_t;

typedef struct {
    netdata_cond_t cond;
    netdata_mutex_t cond_mutex;
    uint64_t run_flag;
} thread_control_t;

typedef enum {
    LOCK_SEQLOCK,
    LOCK_SPINLOCK,
    NUM_LOCK_TYPES
} lock_type_t;

typedef struct {
    shared_data_t data;
    SEQLOCK seqlock;
    SPINLOCK spinlock;
    thread_stats_t stats[MAX_THREADS];
    thread_control_t thread_controls[MAX_THREADS];
} seqlock_control_t;

typedef struct {
    int thread_id;
    thread_type_t type;
    lock_type_t lock_type;
    seqlock_control_t *control;
    ND_THREAD *thread;
} thread_context_t;

// --------------------------------------------------------------------------
// summary

typedef struct {
    double reader_ops_per_sec[NUM_LOCK_TYPES][MAX_CONFIGS];
    double writer_ops_per_sec[NUM_LOCK_TYPES][MAX_CONFIGS];
    uint64_t total_violations[NUM_LOCK_TYPES][MAX_CONFIGS];
    int readers[MAX_CONFIGS];
    int writers[MAX_CONFIGS];
    int config_count;
} summary_stats_t;

// --------------------------------------------------------------------------
// thread work

static void wait_for_start(netdata_cond_t *cond, netdata_mutex_t *mutex, uint64_t *flag) {
    netdata_mutex_lock(mutex);
    while (*flag == 0)
        netdata_cond_wait(cond, mutex);
    netdata_mutex_unlock(mutex);
}

static void benchmark_thread(void *arg) {
    thread_context_t *ctx = (thread_context_t *)arg;
    seqlock_control_t *control = ctx->control;
    thread_control_t *tc = &control->thread_controls[ctx->thread_id];

    while(1) {
        wait_for_start(&tc->cond, &tc->cond_mutex, &tc->run_flag);

        if(tc->run_flag == STOP_SIGNAL)
            break;

        usec_t start = now_monotonic_high_precision_usec();
        uint64_t operations = 0;
        uint64_t violations = 0;

        if(ctx->type == THREAD_WRITER) {
            // Writer: update a and b such that a + b == CONSISTENCY_MAGIC
            uint64_t seq = 0;
            while(tc->run_flag) {
                seq++;
                if(ctx->lock_type == LOCK_SEQLOCK) {
                    seqlock_write_begin(&control->seqlock);
                    control->data.a = seq;
                    control->data.b = CONSISTENCY_MAGIC - seq;
                    seqlock_write_end(&control->seqlock);
                }
                else {
                    spinlock_lock(&control->spinlock);
                    control->data.a = seq;
                    control->data.b = CONSISTENCY_MAGIC - seq;
                    spinlock_unlock(&control->spinlock);
                }
                operations++;
            }
        }
        else {
            // Reader: read a and b, check consistency
            while(tc->run_flag) {
                uint64_t a, b;
                if(ctx->lock_type == LOCK_SEQLOCK) {
                    uint64_t gen;
                    do {
                        gen = seqlock_read_begin(&control->seqlock);
                        a = control->data.a;
                        b = control->data.b;
                    } while(seqlock_read_retry(&control->seqlock, gen));
                }
                else {
                    spinlock_lock(&control->spinlock);
                    a = control->data.a;
                    b = control->data.b;
                    spinlock_unlock(&control->spinlock);
                }

                if(unlikely(a + b != CONSISTENCY_MAGIC))
                    violations++;

                operations++;
            }
        }

        usec_t test_time = now_monotonic_high_precision_usec() - start;
        __atomic_store_n(&control->stats[ctx->thread_id].test_time, test_time, __ATOMIC_RELEASE);
        __atomic_store_n(&control->stats[ctx->thread_id].operations, operations, __ATOMIC_RELEASE);
        __atomic_store_n(&control->stats[ctx->thread_id].violations, violations, __ATOMIC_RELEASE);
        __atomic_store_n(&control->stats[ctx->thread_id].ready, 1, __ATOMIC_RELEASE);
    }
}

// --------------------------------------------------------------------------
// test runner

static void print_thread_stats(const char *test_name, int readers, int writers,
                                thread_context_t *contexts, seqlock_control_t *control,
                                summary_stats_t *summary, int config_idx, int lock_type) {
    fprintf(stderr, "\n%-20s (readers: %d, writers: %d)\n", test_name, readers, writers);
    fprintf(stderr, "%4s %8s %12s %12s %12s %12s\n",
            "THR", "TYPE", "OPS", "OPS/SEC", "VIOLATIONS", "TIME (ms)");

    double reader_ops_per_sec = 0;
    double writer_ops_per_sec = 0;
    uint64_t total_violations = 0;

    for(int i = 0; i < readers + writers; i++) {
        uint64_t ops = __atomic_load_n(&control->stats[i].operations, __ATOMIC_RELAXED);
        uint64_t viol = __atomic_load_n(&control->stats[i].violations, __ATOMIC_RELAXED);
        usec_t time = __atomic_load_n(&control->stats[i].test_time, __ATOMIC_RELAXED);
        double ops_per_sec = (double)ops * USEC_PER_SEC / time;

        fprintf(stderr, "%4d %8s %12"PRIu64" %12.0f %12"PRIu64" %12.2f\n",
                i,
                contexts[i].type == THREAD_READER ? "READER" : "WRITER",
                ops, ops_per_sec, viol, (double)time / 1000.0);

        total_violations += viol;

        if(contexts[i].type == THREAD_READER)
            reader_ops_per_sec += ops_per_sec;
        else
            writer_ops_per_sec += ops_per_sec;
    }

    if(total_violations > 0) {
        fprintf(stderr, "\nFATAL ERROR: Detected %"PRIu64" consistency violations (torn reads)!\n",
                total_violations);
        fflush(stderr);
        _exit(1);
    }

    summary->reader_ops_per_sec[lock_type][config_idx] = reader_ops_per_sec;
    summary->writer_ops_per_sec[lock_type][config_idx] = writer_ops_per_sec;
    summary->total_violations[lock_type][config_idx] = total_violations;
    summary->readers[config_idx] = readers;
    summary->writers[config_idx] = writers;
}

static void run_test(const char *name, int readers, int writers,
                     thread_context_t *contexts, seqlock_control_t *control,
                     summary_stats_t *summary, int config_idx, int lock_type) {
    int total_threads = readers + writers;

    fprintf(stderr, "\nRunning test: %s with %d readers and %d writers...\n",
            name, readers, writers);

    // Reset
    memset(&control->stats, 0, sizeof(control->stats));
    control->data.a = 0;
    control->data.b = CONSISTENCY_MAGIC;

    // Signal threads to start
    for(int i = 0; i < total_threads; i++) {
        netdata_mutex_lock(&control->thread_controls[i].cond_mutex);
        control->thread_controls[i].run_flag = 1;
        netdata_cond_signal(&control->thread_controls[i].cond);
        netdata_mutex_unlock(&control->thread_controls[i].cond_mutex);
    }

    sleep_usec(TEST_DURATION_SEC * USEC_PER_SEC);

    // Signal stop
    for(int i = 0; i < total_threads; i++)
        __atomic_store_n(&control->thread_controls[i].run_flag, 0, __ATOMIC_RELEASE);

    // Wait for results
    for(int i = 0; i < total_threads; i++) {
        while(!__atomic_load_n(&control->stats[i].ready, __ATOMIC_ACQUIRE))
            sleep_usec(10);
    }

    print_thread_stats(name, readers, writers, contexts, control, summary, config_idx, lock_type);
}

static void print_summary(const summary_stats_t *summary) {
    const char *lock_names[] = {"seqlock", "spinlock"};

    fprintf(stderr, "\n=== Seqlock vs Spinlock Performance Summary (Million ops/sec) ===\n\n");
    fprintf(stderr, "%-12s %-8s %-8s %16s %16s\n",
            "Lock Type", "Readers", "Writers", "Reader Ops/s", "Writer Ops/s");
    fprintf(stderr, "----------------------------------------------------------------------\n");

    for(int config = 0; config < summary->config_count; config++) {
        for(int lock_type = 0; lock_type < NUM_LOCK_TYPES; lock_type++) {
            double reader_ops = summary->reader_ops_per_sec[lock_type][config];
            double writer_ops = summary->writer_ops_per_sec[lock_type][config];

            fprintf(stderr, "%-12s %-8d %-8d %16.2f %16.2f\n",
                    lock_names[lock_type],
                    summary->readers[config],
                    summary->writers[config],
                    reader_ops / 1000000.0,
                    writer_ops / 1000000.0);
        }
        if(config < summary->config_count - 1)
            fprintf(stderr, "----------------------------------------------------------------------\n");
    }
    fprintf(stderr, "\n");
}

// --------------------------------------------------------------------------
// main entry point

int seqlock_stress_test(void) {
    summary_stats_t summary = {0};

    seqlock_control_t seqlock_control = { 0 };
    seqlock_control.seqlock = (SEQLOCK)SEQLOCK_INITIALIZER;
    seqlock_control.data.a = 0;
    seqlock_control.data.b = CONSISTENCY_MAGIC;

    seqlock_control_t spinlock_control = { 0 };
    spinlock_control.spinlock = (SPINLOCK)SPINLOCK_INITIALIZER;
    spinlock_control.data.a = 0;
    spinlock_control.data.b = CONSISTENCY_MAGIC;

    // Initialize per-thread controls
    for(int i = 0; i < MAX_THREADS; i++) {
        netdata_cond_init(&seqlock_control.thread_controls[i].cond);
        netdata_mutex_init(&seqlock_control.thread_controls[i].cond_mutex);
        seqlock_control.thread_controls[i].run_flag = 0;

        netdata_cond_init(&spinlock_control.thread_controls[i].cond);
        netdata_mutex_init(&spinlock_control.thread_controls[i].cond_mutex);
        spinlock_control.thread_controls[i].run_flag = 0;
    }

    // Create thread contexts
    thread_context_t seqlock_contexts[MAX_THREADS];
    thread_context_t spinlock_contexts[MAX_THREADS];

    fprintf(stderr, "\nStarting seqlock benchmark...\n");
    fprintf(stderr, "Creating threads...\n");

    for(int i = 0; i < MAX_THREADS; i++) {
        char thr_name[32];

        seqlock_contexts[i] = (thread_context_t){
            .thread_id = i,
            .type = THREAD_READER,
            .lock_type = LOCK_SEQLOCK,
            .control = &seqlock_control,
        };
        snprintf(thr_name, sizeof(thr_name), "seqlock%d", i);
        seqlock_contexts[i].thread =
            nd_thread_create(thr_name, NETDATA_THREAD_OPTION_DONT_LOG, benchmark_thread, &seqlock_contexts[i]);

        spinlock_contexts[i] = (thread_context_t){
            .thread_id = i,
            .type = THREAD_READER,
            .lock_type = LOCK_SPINLOCK,
            .control = &spinlock_control,
        };
        snprintf(thr_name, sizeof(thr_name), "spinlk%d", i);
        spinlock_contexts[i].thread =
            nd_thread_create(thr_name, NETDATA_THREAD_OPTION_DONT_LOG, benchmark_thread, &spinlock_contexts[i]);
    }

    // Test configurations: [readers, writers]
    int configs[][2] = {
        {1, 0},   // Single reader (no contention baseline)
        {0, 1},   // Single writer (write throughput baseline)
        {1, 1},   // 1 reader + 1 writer
        {2, 1},   // 2 readers + 1 writer (typical seqlock sweet spot)
        {4, 1},   // 4 readers + 1 writer
        {8, 1},   // 8 readers + 1 writer (high read contention)
        {16, 1},  // 16 readers + 1 writer
        {2, 2},   // 2 readers + 2 writers (multi-writer)
        {4, 4},   // 4 readers + 4 writers (heavy contention)
    };

    const int num_configs = sizeof(configs) / sizeof(configs[0]);
    summary.config_count = num_configs;

    // Warm up
    sleep_usec(100000);

    for(int i = 0; i < num_configs; i++) {
        int readers = configs[i][0];
        int writers = configs[i][1];
        int total = readers + writers;

        // Assign reader/writer roles
        int thread_idx = 0;
        for(int r = 0; r < readers; r++) {
            seqlock_contexts[thread_idx].type = THREAD_READER;
            spinlock_contexts[thread_idx].type = THREAD_READER;
            thread_idx++;
        }
        for(int w = 0; w < writers; w++) {
            seqlock_contexts[thread_idx].type = THREAD_WRITER;
            spinlock_contexts[thread_idx].type = THREAD_WRITER;
            thread_idx++;
        }

        // Reset roles for unused threads
        for(int j = total; j < MAX_THREADS; j++) {
            seqlock_contexts[j].type = THREAD_READER;
            spinlock_contexts[j].type = THREAD_READER;
        }

        char test_name[64];
        snprintf(test_name, sizeof(test_name), "seqlock %dR/%dW", readers, writers);
        run_test(test_name, readers, writers, seqlock_contexts, &seqlock_control, &summary, i, LOCK_SEQLOCK);

        snprintf(test_name, sizeof(test_name), "spinlock %dR/%dW", readers, writers);
        run_test(test_name, readers, writers, spinlock_contexts, &spinlock_control, &summary, i, LOCK_SPINLOCK);
    }

    print_summary(&summary);

    // Stop all threads
    fprintf(stderr, "Stopping threads...\n");
    for(int i = 0; i < MAX_THREADS; i++) {
        netdata_mutex_lock(&seqlock_control.thread_controls[i].cond_mutex);
        seqlock_control.thread_controls[i].run_flag = STOP_SIGNAL;
        netdata_cond_signal(&seqlock_control.thread_controls[i].cond);
        netdata_mutex_unlock(&seqlock_control.thread_controls[i].cond_mutex);

        netdata_mutex_lock(&spinlock_control.thread_controls[i].cond_mutex);
        spinlock_control.thread_controls[i].run_flag = STOP_SIGNAL;
        netdata_cond_signal(&spinlock_control.thread_controls[i].cond);
        netdata_mutex_unlock(&spinlock_control.thread_controls[i].cond_mutex);
    }

    fprintf(stderr, "Waiting for threads to exit...\n");
    for(int i = 0; i < MAX_THREADS; i++) {
        nd_thread_join(seqlock_contexts[i].thread);
        nd_thread_join(spinlock_contexts[i].thread);
    }

    // Cleanup
    for(int i = 0; i < MAX_THREADS; i++) {
        netdata_cond_destroy(&seqlock_control.thread_controls[i].cond);
        netdata_mutex_destroy(&seqlock_control.thread_controls[i].cond_mutex);
        netdata_cond_destroy(&spinlock_control.thread_controls[i].cond);
        netdata_mutex_destroy(&spinlock_control.thread_controls[i].cond_mutex);
    }

    fprintf(stderr, "All tests passed.\n");
    return 0;
}
