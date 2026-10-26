// SPDX-License-Identifier: GPL-3.0-or-later

#include "host-changes.h"

// RAM totals move by a few MiB across kernels (reserved memory), so smaller differences are not changes
#define HOST_CHANGES_RAM_TOLERANCE_PERCENT 1

static bool ram_changed(uint64_t old, uint64_t now) {
    uint64_t diff = old > now ? old - now : now - old;
    return diff * 100 > old * HOST_CHANGES_RAM_TOLERANCE_PERCENT;
}

static bool strings_changed(const char *old, const char *now) {
    // an unknown side is not a change
    return old && *old && now && *now && strcmp(old, now) != 0;
}

size_t host_changes_compare(const HOST_CHANGES_SNAPSHOT *old, const HOST_CHANGES_SNAPSHOT *now, host_change_cb_t cb, void *data) {
    size_t changes = 0;
    char o[64], n[64];

    if(!UUIDiszero(old->boot_id) && !UUIDiszero(now->boot_id) && !UUIDeq(old->boot_id, now->boot_id)) {
        uuid_unparse_lower(old->boot_id.uuid, o);
        uuid_unparse_lower(now->boot_id.uuid, n);
        cb("reboot", "boot id", o, n, data);
        changes++;
    }

    if(strings_changed(old->kernel, now->kernel)) {
        cb("kernel", "kernel version", old->kernel, now->kernel, data);
        changes++;
    }

    if(strings_changed(old->os_name, now->os_name) || strings_changed(old->os_version, now->os_version)) {
        snprintfz(o, sizeof(o), "%s %s", old->os_name ? old->os_name : "", old->os_version ? old->os_version : "");
        snprintfz(n, sizeof(n), "%s %s", now->os_name ? now->os_name : "", now->os_version ? now->os_version : "");
        cb("os", "operating system", o, n, data);
        changes++;
    }

    if(strings_changed(old->netdata_version, now->netdata_version)) {
        cb("netdata_version", "Netdata version", old->netdata_version, now->netdata_version, data);
        changes++;
    }

    if(old->cpus && now->cpus && old->cpus != now->cpus) {
        snprintfz(o, sizeof(o), "%zu", old->cpus);
        snprintfz(n, sizeof(n), "%zu", now->cpus);
        cb("cpus", "CPU count", o, n, data);
        changes++;
    }

    if(old->ram_bytes && now->ram_bytes && ram_changed(old->ram_bytes, now->ram_bytes)) {
        snprintfz(o, sizeof(o), "%" PRIu64, old->ram_bytes);
        snprintfz(n, sizeof(n), "%" PRIu64, now->ram_bytes);
        cb("ram", "RAM bytes", o, n, data);
        changes++;
    }

    return changes;
}

void host_change_log(const char *what, const char *title, const char *old_value, const char *new_value, void *data __maybe_unused) {
    old_value = old_value ? old_value : "";
    new_value = new_value ? new_value : "";

    ND_LOG_STACK lgs[] = {
        ND_LOG_FIELD_UUID(NDF_MESSAGE_ID, &host_change_msgid),
        ND_LOG_FIELD_TXT(NDF_HOST_CHANGE, what),
        ND_LOG_FIELD_TXT(NDF_HOST_CHANGE_OLD, old_value),
        ND_LOG_FIELD_TXT(NDF_HOST_CHANGE_NEW, new_value),
        ND_LOG_FIELD_END(),
    };
    ND_LOG_STACK_PUSH(lgs);

    nd_log(NDLS_DAEMON, NDLP_NOTICE, "Host change: %s changed from '%s' to '%s'", title, old_value, new_value);
}

// --------------------------------------------------------------------------------------------------------------------
// runtime system-info changes

static const struct {
    const char *label;
    const char *what;
    const char *title;
} system_info_changes[] = {
    { "_system_cores",          "cpus",             "CPU count" },
    { "_system_ram_total",      "ram",              "RAM bytes" },
    { "_system_disk_space",     "disk_space",       "disk space bytes" },
    { "_net_default_iface",     "default_iface",    "default network interface" },
    { "_net_default_iface_ip",  "default_iface_ip", "default network interface IP" },
};

size_t host_changes_system_info(const RRDHOST_SYSTEM_INFO_CHANGES *changes, host_change_cb_t cb, void *data) {
    size_t reported = 0;
    for(size_t i = 0; i < changes->used; i++) {
        const char *old = changes->items[i].old_value, *now = changes->items[i].new_value;
        for(size_t k = 0; k < _countof(system_info_changes); k++) {
            if(strcmp(changes->items[i].label, system_info_changes[k].label) != 0)
                continue;

            // a value that appears or disappears is a change (e.g. the default route went away)
            if(!strcmp(system_info_changes[k].what, "ram") && old && now &&
               !ram_changed(str2ull(old, NULL), str2ull(now, NULL)))
                break;

            cb(system_info_changes[k].what, system_info_changes[k].title, old, now, data);
            reported++;
            break;
        }
    }
    return reported;
}

void host_changes_log_system_info(const RRDHOST_SYSTEM_INFO_CHANGES *changes) {
    host_changes_system_info(changes, host_change_log, NULL);
}

// --------------------------------------------------------------------------------------------------------------------
// unit test

struct host_changes_test {
    char seen[512];
};

static void host_changes_test_cb(const char *what, const char *title __maybe_unused, const char *old_value, const char *new_value, void *data) {
    struct host_changes_test *t = data;
    size_t len = strlen(t->seen);
    snprintfz(t->seen + len, sizeof(t->seen) - len, "%s:%s>%s;", what, old_value ? old_value : "-", new_value ? new_value : "-");
}

static int host_changes_test_case(const char *name, const HOST_CHANGES_SNAPSHOT *old, const HOST_CHANGES_SNAPSHOT *now,
                                  size_t expected_changes, const char *expected) {
    struct host_changes_test t = { 0 };
    size_t changes = host_changes_compare(old, now, host_changes_test_cb, &t);
    int err = changes != expected_changes || strcmp(t.seen, expected) != 0;
    fprintf(stderr, "  host changes (%s): %s, expected %zu '%s', got %zu '%s'\n",
            name, err ? "FAILED" : "OK", expected_changes, expected, changes, t.seen);
    return err;
}

int host_changes_unittest(void) {
    int errors = 0;

    ND_UUID boot1, boot2;
    uuid_parse("00000000-0000-0001-0000-000000000001", boot1.uuid);
    uuid_parse("00000000-0000-0001-0000-000000000002", boot2.uuid);

    HOST_CHANGES_SNAPSHOT base = {
        .boot_id = boot1, .kernel = "6.1.0-1", .os_name = "Debian GNU/Linux", .os_version = "12",
        .netdata_version = "v2.12.0", .cpus = 8, .ram_bytes = 16ULL * 1024 * 1024 * 1024,
    };

    HOST_CHANGES_SNAPSHOT same = base;
    errors += host_changes_test_case("unchanged", &base, &same, 0, "");

    HOST_CHANGES_SNAPSHOT unknown = { 0 };
    errors += host_changes_test_case("unknown now", &base, &unknown, 0, "");
    errors += host_changes_test_case("unknown before", &unknown, &base, 0, "");

    HOST_CHANGES_SNAPSHOT ram_wobble = base;
    ram_wobble.ram_bytes = base.ram_bytes - 64ULL * 1024 * 1024; // 0.4%
    errors += host_changes_test_case("ram within tolerance", &base, &ram_wobble, 0, "");

    HOST_CHANGES_SNAPSHOT all = {
        .boot_id = boot2, .kernel = "6.1.0-2", .os_name = "Debian GNU/Linux", .os_version = "13",
        .netdata_version = "v2.12.1", .cpus = 16, .ram_bytes = 32ULL * 1024 * 1024 * 1024,
    };
    errors += host_changes_test_case("everything", &base, &all, 6,
        "reboot:00000000-0000-0001-0000-000000000001>00000000-0000-0001-0000-000000000002;"
        "kernel:6.1.0-1>6.1.0-2;"
        "os:Debian GNU/Linux 12>Debian GNU/Linux 13;"
        "netdata_version:v2.12.0>v2.12.1;"
        "cpus:8>16;"
        "ram:17179869184>34359738368;");

    // runtime system-info changes: names, the RAM tolerance, appearing and disappearing values, unreported fields
    RRDHOST_SYSTEM_INFO_CHANGES rc = {
        .used = 5,
        .items = {
            { .label = "_system_ram_total", .old_value = "17179869184", .new_value = "17179869185" },
            { .label = "_system_ram_total", .old_value = "17179869184", .new_value = "34359738368" },
            { .label = "_net_default_iface", .old_value = "eth0", .new_value = NULL },
            { .label = "_net_default_iface_ip", .old_value = NULL, .new_value = "192.0.2.10" },
            { .label = "_net_default_iface_detection", .old_value = "route", .new_value = "none" },
        },
    };
    struct host_changes_test t = { 0 };
    size_t reported = host_changes_system_info(&rc, host_changes_test_cb, &t);
    const char *expected = "ram:17179869184>34359738368;default_iface:eth0>-;default_iface_ip:->192.0.2.10;";
    int err = reported != 3 || strcmp(t.seen, expected) != 0;
    fprintf(stderr, "  host changes (runtime system info): %s, expected 3 '%s', got %zu '%s'\n",
            err ? "FAILED" : "OK", expected, reported, t.seen);
    errors += err;

    return errors;
}
